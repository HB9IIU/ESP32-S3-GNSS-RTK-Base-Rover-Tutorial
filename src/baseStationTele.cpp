// =============================================================================
// Headless RTK Base Station -- ESP32-S3-WROOM-1 + u-blox ZED-F9P + 3DR/SiK
// =============================================================================
// Critical path:
//   ZED-F9P UART1 -> SparkFun parser -> CRC-validated RTCM3 -> 3DR UART2
//
// UBX monitoring messages remain enabled during relay. This provides live
// satellite, DOP and position-quality data without forwarding UBX bytes to the
// rover. Wi-Fi, web and OTA are optional and never cause a reboot.
// =============================================================================

#include <Arduino.h>
#include <SparkFun_u-blox_GNSS_v3.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#ifdef RGB_LED_PIN
#include <Adafruit_NeoPixel.h>
#endif

// Generic defaults. PlatformIO build flags can override these for another
// ESP32-S3 carrier without changing the application source.
#ifndef GPS_RX_PIN
#define GPS_RX_PIN       4   // UART1 RX <- ZED-F9P TX1
#endif
#ifndef GPS_TX_PIN
#define GPS_TX_PIN       5   // UART1 TX -> ZED-F9P RX1
#endif
#ifndef TELE_RX_PIN
#define TELE_RX_PIN     17   // UART2 RX <- 3DR radio TX
#endif
#ifndef TELE_TX_PIN
#define TELE_TX_PIN     18   // UART2 TX -> 3DR radio RX
#endif
#define TELE_BAUD        57600
#define GPS_BAUD         38400

#define SVIN_MIN_TIME_S  300
#define SVIN_DEFAULT_ACC_MM 100
#define SVIN_MIN_TARGET_MM  10
#define SVIN_MAX_TARGET_MM  10000
#define GNSS_STABLE_TIME_S 15
#define GNSS_MIN_SV      12
#define GNSS_MAX_PDOP    2.5f
#define GNSS_MAX_HACC_M  5.0f
#define SVIN_PLOT_MIN_TIME_S 15
#define SVIN_RESULT_HOLD_S 15

#define WIFI_SSID        "MESH"
#define WIFI_PASS        "REDACTED-WIFI-PASSWORD"
#define WIFI_HOSTNAME    "rtk-base-tele"
#define WIFI_TIMEOUT_S   20
#define WIFI_RETRY_S     60
#define OTA_PASSWORD     "REDACTED-BASE-OTA-PASSWORD"

#define RTCM_PREAMBLE    0xD3
#define RTCM_MAX_FRAME   1029
#define SVIN_HISTORY_SIZE 600
#define MAX_SATELLITES   64
#define PROFILE_VERSION  2
#define SESSION_HISTORY_SECONDS (12UL * 60UL * 60UL)

HardwareSerial GPS_Serial(1);
HardwareSerial Tele_Serial(2);
SFE_UBLOX_GNSS_SERIAL myGNSS;
WebServer webServer(80);
Preferences prefs;

enum BaseState { ACQUIRING, SURVEY_IN, RELAY };
static BaseState baseState = ACQUIRING;
static bool gnssReady = false;
static bool networkReady = false;
static bool wifiAttempting = false;
static uint32_t wifiAttemptStartedMs = 0;
static uint32_t lastWifiAttemptMs = 0;

#ifdef RGB_LED_PIN
static Adafruit_NeoPixel rgbLed(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);
static uint32_t ledLastColor = 0xFFFFFFFFUL;
static uint32_t wifiFlashStartedMs = 0;
static bool wifiSuccessAnimation = false;
static bool wifiWasConnected = false;
static uint32_t rtcmBlipUntilMs = 0;
static uint32_t lastRtcmBlipMs = 0;

static void setStatusLed(uint8_t red, uint8_t green, uint8_t blue) {
    uint32_t color = rgbLed.Color(red, green, blue);
    if (color == ledLastColor) return;
    ledLastColor = color;
    rgbLed.setPixelColor(0, color);
    rgbLed.show();
}

static void triggerWifiSuccessAnimation() {
    wifiSuccessAnimation = true;
    wifiFlashStartedMs = millis();
}

static void triggerRtcmBlip() {
    uint32_t now = millis();
    if (now - lastRtcmBlipMs < 200) return;
    lastRtcmBlipMs = now;
    rtcmBlipUntilMs = now + 40;
}

static void serviceStatusLed() {
    uint32_t now = millis();

    // Five 100 ms green flashes when Wi-Fi connects.
    if (wifiSuccessAnimation) {
        uint32_t elapsed = now - wifiFlashStartedMs;
        if (elapsed < 1000) {
            bool on = ((elapsed / 100) & 1U) == 0;
            setStatusLed(0, on ? 32 : 0, 0);
            return;
        }
        wifiSuccessAnimation = false;
    }

    // A validated RTCM frame produces a short green blip during relay mode.
    if (baseState == RELAY && (int32_t)(rtcmBlipUntilMs - now) > 0) {
        setStatusLed(0, 32, 0);
        return;
    }

    if (!gnssReady) {
        setStatusLed(((now / 100) & 1U) == 0 ? 32 : 0, 0, 0);
        return;
    }
    if (wifiAttempting) {
        setStatusLed(((now / 250) & 1U) == 0 ? 24 : 0, 0, 0);
        return;
    }
    if (WiFi.status() != WL_CONNECTED && (now % 5000UL) < 80) {
        setStatusLed(20, 0, 20);
        return;
    }

    if (baseState == ACQUIRING) {
        bool on = ((now / 500) & 1U) == 0;
        setStatusLed(on ? 24 : 0, on ? 10 : 0, 0);
    } else if (baseState == SURVEY_IN) {
        setStatusLed(0, 0, (now % 1000UL) < 200 ? 32 : 0);
    } else {
        setStatusLed(0, 0, 0);
    }
}
#else
static inline void triggerWifiSuccessAnimation() {}
static inline void triggerRtcmBlip() {}
static inline void serviceStatusLed() {}
#endif

// Survey and GNSS quality
static uint32_t svInTime = 0;
static float svInAcc = 0.0f;
static bool svInValid = false;
static uint8_t baseSV = 0;
static uint8_t baseFix = 0;
static float baseHAcc = 0.0f;
static float basePDOP = 0.0f;
static bool qualityLive = false;
static float svInHistory[SVIN_HISTORY_SIZE];
static uint16_t svInHistIdx = 0;
static uint16_t svInHistLen = 0;
static uint16_t gnssStableSec = 0;
static bool gnssQualityReady = false;
static uint32_t surveyCompletedMs = 0;
static uint32_t surveyTargetMm = SVIN_DEFAULT_ACC_MM;

// Saved high-precision LLA base profile.
static bool hasSavedPos = false;
static int32_t savedLat = 0;       // 1e-7 degrees
static int8_t savedLatHp = 0;      // 1e-9 degrees
static int32_t savedLon = 0;       // 1e-7 degrees
static int8_t savedLonHp = 0;      // 1e-9 degrees
static int32_t savedAltCm = 0;     // centimetres above ellipsoid
static int8_t savedAltHp = 0;      // 0.1 mm
static uint32_t savedSurveySec = 0;
static uint32_t savedSurveyAccMm = 0;

// Signal configuration cache
static uint8_t sigGpsEna, sigGpsL1, sigGpsL2;
static uint8_t sigGloEna, sigGloL1, sigGloL2;
static uint8_t sigGalEna, sigGalE1, sigGalE5;
static uint8_t sigBdsEna, sigBdsB1, sigBdsB2;

// Live satellite snapshot from UBX-NAV-SAT
struct SatelliteInfo {
    uint8_t gnssId;
    uint8_t svId;
    uint8_t cno;
    int8_t elevation;
    int16_t azimuth;
    bool used;
    uint8_t health;
};
static SatelliteInfo satellites[MAX_SATELLITES];
static uint8_t satelliteCount = 0;
static uint32_t lastNavSatMs = 0;

// RTCM health
static const uint16_t RTCM_TYPES[] = {1005, 1074, 1084, 1094, 1124, 1230};
static uint32_t rtcmTypeCount[] = {0, 0, 0, 0, 0, 0};
static uint32_t rtcmTypeLastMs[] = {0, 0, 0, 0, 0, 0};
static uint8_t rtcmBuf[RTCM_MAX_FRAME];
static uint16_t rtcmIdx = 0;
static int32_t rtcmExpected = -1;
static uint32_t rtcmFramesSent = 0;
static uint32_t rtcmBytesSent = 0;
static uint32_t rtcmCrcErrors = 0;
static uint32_t rtcmLengthErrors = 0;
static uint32_t lastRtcmMs = 0;
static float rtcmFramesPerSec = 0.0f;
static float rtcmBytesPerSec = 0.0f;

static String gnssModule = "unknown";
static String gnssFirmware = "unknown";
static String gnssProtocol = "unknown";

// One sample per second in PSRAM. This is intentionally volatile: reports can
// be downloaded, but no runtime history is written to flash or to the F9P.
struct SessionSample {
    uint32_t uptimeSec;
    uint32_t rtcmFrames;
    uint32_t rtcmBytes;
    uint32_t rtcmCrcErrors;
    uint32_t surveySec;
    float hAccM;
    float pDop;
    float surveyAccM;
    float rtcmFps;
    float rtcmBytesPerSec;
    int8_t wifiRssi;
    uint8_t state;
    uint8_t fix;
    uint8_t satellites;
};

static SessionSample *sessionHistory = nullptr;
static uint32_t sessionCapacity = 0;
static uint32_t sessionCount = 0;
static uint32_t sessionWriteIdx = 0;
static uint32_t lastSessionSampleMs = 0;

static void beginSessionHistory() {
    if (!psramFound()) {
        Serial.println(F("[REPORT] PSRAM unavailable; session history disabled"));
        return;
    }
    sessionCapacity = SESSION_HISTORY_SECONDS;
    size_t bytes = (size_t)sessionCapacity * sizeof(SessionSample);
    sessionHistory = static_cast<SessionSample *>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!sessionHistory) {
        sessionCapacity = 0;
        Serial.println(F("[REPORT] PSRAM allocation failed; session history disabled"));
        return;
    }
    Serial.printf("[REPORT] PSRAM history: %u samples, %u bytes (12 hours @ 1 Hz)\n",
                  sessionCapacity, (unsigned)bytes);
}

static void recordSessionSample() {
    if (!sessionHistory) return;
    uint32_t now = millis();
    if (now - lastSessionSampleMs < 1000) return;
    lastSessionSampleMs = now;

    SessionSample &s = sessionHistory[sessionWriteIdx];
    s.uptimeSec = now / 1000;
    s.rtcmFrames = rtcmFramesSent;
    s.rtcmBytes = rtcmBytesSent;
    s.rtcmCrcErrors = rtcmCrcErrors;
    s.surveySec = svInTime;
    s.hAccM = baseHAcc;
    s.pDop = basePDOP;
    s.surveyAccM = svInAcc;
    s.rtcmFps = rtcmFramesPerSec;
    s.rtcmBytesPerSec = rtcmBytesPerSec;
    s.wifiRssi = WiFi.status() == WL_CONNECTED ? (int8_t)constrain(WiFi.RSSI(), -127, 0) : -127;
    s.state = (uint8_t)baseState;
    s.fix = baseFix;
    s.satellites = baseSV;

    sessionWriteIdx = (sessionWriteIdx + 1) % sessionCapacity;
    if (sessionCount < sessionCapacity) sessionCount++;
}

static uint32_t sessionPhysicalIndex(uint32_t chronologicalIndex) {
    uint32_t oldest = sessionCount < sessionCapacity ? 0 : sessionWriteIdx;
    return (oldest + chronologicalIndex) % sessionCapacity;
}

static const char *sessionStateName(uint8_t state) {
    return state == RELAY ? "RELAY" : (state == SURVEY_IN ? "SURVEY_IN" : "ACQUIRING");
}

static void processRTCMByte(uint8_t b);

// SparkFun defines this as weak specifically so applications can consume RTCM
// while the same parser continues to process UBX monitoring messages.
void DevUBLOXGNSS::processRTCM(uint8_t incoming) {
    processRTCMByte(incoming);
}

static uint32_t crc24q(const uint8_t *data, size_t len) {
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 16;
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc <<= 1;
            if (crc & 0x1000000UL) crc ^= 0x1864CFBUL;
        }
    }
    return crc & 0xFFFFFFUL;
}

static void resetRtcmParser() {
    rtcmIdx = 0;
    rtcmExpected = -1;
}

static void processRTCMByte(uint8_t b) {
    if (rtcmExpected == -1) {
        if (b != RTCM_PREAMBLE) return;
        rtcmIdx = 0;
        rtcmExpected = 0;
    }

    if (rtcmIdx >= RTCM_MAX_FRAME) {
        rtcmLengthErrors++;
        resetRtcmParser();
        return;
    }
    rtcmBuf[rtcmIdx++] = b;

    if (rtcmExpected == 0 && rtcmIdx == 3) {
        if ((rtcmBuf[1] & 0xFC) != 0) {
            rtcmLengthErrors++;
            resetRtcmParser();
            return;
        }
        uint16_t payloadLen = ((uint16_t)(rtcmBuf[1] & 0x03) << 8) | rtcmBuf[2];
        rtcmExpected = 3 + payloadLen + 3;
        if (rtcmExpected < 6 || rtcmExpected > RTCM_MAX_FRAME) {
            rtcmLengthErrors++;
            resetRtcmParser();
            return;
        }
    }

    if (rtcmExpected > 0 && rtcmIdx == (uint16_t)rtcmExpected) {
        uint32_t expectedCrc = ((uint32_t)rtcmBuf[rtcmIdx - 3] << 16) |
                               ((uint32_t)rtcmBuf[rtcmIdx - 2] << 8) |
                               rtcmBuf[rtcmIdx - 1];
        uint32_t calculatedCrc = crc24q(rtcmBuf, rtcmIdx - 3);
        if (calculatedCrc == expectedCrc) {
            // RTCM generated before the base position is valid must never be
            // sent to a rover. Parse and discard it until fixed mode is active.
            if (baseState == RELAY) {
                Tele_Serial.write(rtcmBuf, rtcmIdx);
                rtcmFramesSent++;
                rtcmBytesSent += rtcmIdx;
                lastRtcmMs = millis();
                triggerRtcmBlip();
                uint16_t type = (rtcmIdx >= 5) ?
                    (((uint16_t)rtcmBuf[3] << 4) | (rtcmBuf[4] >> 4)) : 0;
                for (uint8_t i = 0; i < 6; i++) {
                    if (RTCM_TYPES[i] == type) {
                        rtcmTypeCount[i]++;
                        rtcmTypeLastMs[i] = millis();
                        break;
                    }
                }
            }
        } else {
            rtcmCrcErrors++;
        }
        resetRtcmParser();
    }
}

static void navSatCallback(UBX_NAV_SAT_data_t *data) {
    uint8_t count = min((uint16_t)data->header.numSvs, (uint16_t)MAX_SATELLITES);
    for (uint8_t i = 0; i < count; i++) {
        satellites[i].gnssId = data->blocks[i].gnssId;
        satellites[i].svId = data->blocks[i].svId;
        satellites[i].cno = data->blocks[i].cno;
        satellites[i].elevation = data->blocks[i].elev;
        satellites[i].azimuth = data->blocks[i].azim;
        satellites[i].used = data->blocks[i].flags.bits.svUsed;
        satellites[i].health = data->blocks[i].flags.bits.health;
    }
    satelliteCount = count;
    lastNavSatMs = millis();
}

static void loadBaseProfile() {
    prefs.begin("rtk-base-t", true);
    surveyTargetMm = constrain(prefs.getUInt("targetMm", SVIN_DEFAULT_ACC_MM),
                               (uint32_t)SVIN_MIN_TARGET_MM,
                               (uint32_t)SVIN_MAX_TARGET_MM);
    hasSavedPos = prefs.getBool("valid", false);
    if (hasSavedPos) {
        savedLat = prefs.getInt("lat", 0);
        savedLatHp = (int8_t)prefs.getInt("latHp", 0);
        savedLon = prefs.getInt("lon", 0);
        savedLonHp = (int8_t)prefs.getInt("lonHp", 0);
        savedAltCm = prefs.getInt("altCm", 0);
        savedAltHp = (int8_t)prefs.getInt("altHp", 0);
        savedSurveySec = prefs.getUInt("svSec", 0);
        savedSurveyAccMm = prefs.getUInt("svAccMm", 0);
    }
    prefs.end();
}

static void saveBaseProfile() {
    prefs.begin("rtk-base-t", false);
    prefs.putUInt("version", PROFILE_VERSION);
    prefs.putBool("valid", true);
    prefs.putInt("lat", savedLat);
    prefs.putInt("latHp", savedLatHp);
    prefs.putInt("lon", savedLon);
    prefs.putInt("lonHp", savedLonHp);
    prefs.putInt("altCm", savedAltCm);
    prefs.putInt("altHp", savedAltHp);
    prefs.putUInt("svSec", savedSurveySec);
    prefs.putUInt("svAccMm", savedSurveyAccMm);
    prefs.end();
    hasSavedPos = true;
}

static void prepareForSurvey() {
    // Do not let a previous Survey-In or fixed-time-mode solution leak into a
    // new survey. First return the F9P to normal navigation, then wait for a
    // continuous period of good GNSS geometry before starting Survey-In.
    myGNSS.disableSurveyMode();
    svInHistIdx = 0;
    svInHistLen = 0;
    svInTime = 0;
    svInAcc = 0;
    svInValid = false;
    gnssStableSec = 0;
    gnssQualityReady = false;
    surveyCompletedMs = 0;
    resetRtcmParser();
    rtcmFramesSent = 0;
    rtcmBytesSent = 0;
    rtcmCrcErrors = 0;
    rtcmLengthErrors = 0;
    lastRtcmMs = 0;
    rtcmFramesPerSec = 0;
    rtcmBytesPerSec = 0;
    for (uint8_t i = 0; i < 6; i++) {
        rtcmTypeCount[i] = 0;
        rtcmTypeLastMs[i] = 0;
    }
    baseState = ACQUIRING;
    Serial.printf("[BASE] Stabilizing GNSS for %us before Survey-In\n", GNSS_STABLE_TIME_S);
}

static void clearBaseProfileAndSurvey() {
    prefs.begin("rtk-base-t", false);
    prefs.clear();
    prefs.putUInt("targetMm", surveyTargetMm);
    prefs.end();
    hasSavedPos = false;
    savedSurveySec = 0;
    savedSurveyAccMm = 0;
    myGNSS.setUART1Output(COM_TYPE_UBX | COM_TYPE_RTCM3);
    prepareForSurvey();
    Serial.println(F("[BASE] Saved profile cleared; GNSS stabilization started"));
}

static void cacheModuleInfo() {
    if (!myGNSS.getModuleInfo()) return;
    gnssModule = myGNSS.getModuleName();
    gnssFirmware = String(myGNSS.getFirmwareType()) + " " +
                   String(myGNSS.getFirmwareVersionHigh()) + "." +
                   String(myGNSS.getFirmwareVersionLow());
    gnssProtocol = String(myGNSS.getProtocolVersionHigh()) + "." +
                   String(myGNSS.getProtocolVersionLow());
}

static bool configureGNSS() {
    cacheModuleInfo();
    bool ok = true;
    ok &= myGNSS.setUART1Input(COM_TYPE_UBX);
    ok &= myGNSS.setUART1Output(COM_TYPE_UBX | COM_TYPE_RTCM3);
    ok &= myGNSS.setNavigationFrequency(1);

    myGNSS.newCfgValset();
    myGNSS.addCfgValset(UBLOX_CFG_MSGOUT_NMEA_ID_GGA_UART1, (uint8_t)0);
    myGNSS.addCfgValset(UBLOX_CFG_MSGOUT_NMEA_ID_GLL_UART1, (uint8_t)0);
    myGNSS.addCfgValset(UBLOX_CFG_MSGOUT_NMEA_ID_GSA_UART1, (uint8_t)0);
    myGNSS.addCfgValset(UBLOX_CFG_MSGOUT_NMEA_ID_GSV_UART1, (uint8_t)0);
    myGNSS.addCfgValset(UBLOX_CFG_MSGOUT_NMEA_ID_RMC_UART1, (uint8_t)0);
    myGNSS.addCfgValset(UBLOX_CFG_MSGOUT_NMEA_ID_VTG_UART1, (uint8_t)0);
    myGNSS.addCfgValset(UBLOX_CFG_MSGOUT_RTCM_3X_TYPE1005_UART1, (uint8_t)1);
    myGNSS.addCfgValset(UBLOX_CFG_MSGOUT_RTCM_3X_TYPE1074_UART1, (uint8_t)1);
    myGNSS.addCfgValset(UBLOX_CFG_MSGOUT_RTCM_3X_TYPE1084_UART1, (uint8_t)1);
    myGNSS.addCfgValset(UBLOX_CFG_MSGOUT_RTCM_3X_TYPE1094_UART1, (uint8_t)1);
    myGNSS.addCfgValset(UBLOX_CFG_MSGOUT_RTCM_3X_TYPE1124_UART1, (uint8_t)1);
    myGNSS.addCfgValset(UBLOX_CFG_MSGOUT_RTCM_3X_TYPE1230_UART1, (uint8_t)1);
    ok &= myGNSS.sendCfgValset();

    ok &= myGNSS.setAutoPVT(true);
    ok &= myGNSS.setAutoDOP(true);
    ok &= myGNSS.setAutoNAVSATcallbackPtr(navSatCallback);

    sigGpsEna = myGNSS.getVal8(UBLOX_CFG_SIGNAL_GPS_ENA);
    sigGpsL1 = myGNSS.getVal8(UBLOX_CFG_SIGNAL_GPS_L1CA_ENA);
    sigGpsL2 = myGNSS.getVal8(UBLOX_CFG_SIGNAL_GPS_L2C_ENA);
    sigGloEna = myGNSS.getVal8(UBLOX_CFG_SIGNAL_GLO_ENA);
    sigGloL1 = myGNSS.getVal8(UBLOX_CFG_SIGNAL_GLO_L1_ENA);
    sigGloL2 = myGNSS.getVal8(UBLOX_CFG_SIGNAL_GLO_L2_ENA);
    sigGalEna = myGNSS.getVal8(UBLOX_CFG_SIGNAL_GAL_ENA);
    sigGalE1 = myGNSS.getVal8(UBLOX_CFG_SIGNAL_GAL_E1_ENA);
    sigGalE5 = myGNSS.getVal8(UBLOX_CFG_SIGNAL_GAL_E5B_ENA);
    sigBdsEna = myGNSS.getVal8(UBLOX_CFG_SIGNAL_BDS_ENA);
    sigBdsB1 = myGNSS.getVal8(UBLOX_CFG_SIGNAL_BDS_B1_ENA);
    sigBdsB2 = myGNSS.getVal8(UBLOX_CFG_SIGNAL_BDS_B2_ENA);

    loadBaseProfile();
    if (hasSavedPos) {
        Serial.printf("[BASE] Loading saved profile: %.9f, %.9f\n",
                      savedLat / 1e7 + savedLatHp / 1e9,
                      savedLon / 1e7 + savedLonHp / 1e9);
        if (myGNSS.setStaticPosition(savedLat, savedLatHp, savedLon, savedLonHp,
                                     savedAltCm, savedAltHp, true)) {
            baseState = RELAY;
            svInTime = savedSurveySec;
            svInAcc = savedSurveyAccMm / 1000.0f;
            svInValid = true;
        } else {
            Serial.println(F("[BASE] Saved profile rejected; preparing a new Survey-In"));
            hasSavedPos = false;
            prepareForSurvey();
        }
    } else {
        prepareForSurvey();
    }
    return ok;
}

static void appendJsonString(String &j, const String &value) {
    j += '"';
    for (size_t i = 0; i < value.length(); i++) {
        char c = value[i];
        if (c == '"' || c == '\\') j += '\\';
        j += c;
    }
    j += '"';
}

static void beginReportDownload(const __FlashStringHelper *contentType,
                                const __FlashStringHelper *fileName) {
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    String disposition = F("attachment; filename=\"");
    disposition += fileName;
    disposition += '"';
    webServer.sendHeader(F("Content-Disposition"), disposition);
    webServer.sendHeader(F("Cache-Control"), F("no-store"));
    webServer.send(200, contentType, "");
}

static void serviceCriticalPathDuringReport() {
    if (gnssReady) {
        myGNSS.checkUblox();
        myGNSS.checkCallbacks();
    }
    serviceStatusLed();
    delay(0);
}

static void handleSessionCsv() {
    beginReportDownload(F("text/csv"), F("rtk-base-session.csv"));
    webServer.sendContent(F("uptime_s,state,fix,satellites,hacc_m,pdop,survey_s,survey_acc_m,rtcm_frames,rtcm_bytes,rtcm_fps,rtcm_bytes_s,rtcm_crc_errors,wifi_rssi_dbm\r\n"));
    String line;
    line.reserve(192);
    String chunk;
    chunk.reserve(4096);
    uint32_t count = sessionCount;
    for (uint32_t i = 0; i < count; i++) {
        const SessionSample &s = sessionHistory[sessionPhysicalIndex(i)];
        line = s.uptimeSec;
        line += ','; line += sessionStateName(s.state);
        line += ','; line += s.fix;
        line += ','; line += s.satellites;
        line += ','; line += String(s.hAccM, 3);
        line += ','; line += String(s.pDop, 2);
        line += ','; line += s.surveySec;
        line += ','; line += String(s.surveyAccM, 3);
        line += ','; line += s.rtcmFrames;
        line += ','; line += s.rtcmBytes;
        line += ','; line += String(s.rtcmFps, 1);
        line += ','; line += String(s.rtcmBytesPerSec, 1);
        line += ','; line += s.rtcmCrcErrors;
        line += ','; line += s.wifiRssi;
        line += F("\r\n");
        chunk += line;
        if (chunk.length() >= 3072) {
            webServer.sendContent(chunk);
            chunk = "";
            serviceCriticalPathDuringReport();
        }
    }
    if (chunk.length()) webServer.sendContent(chunk);
    webServer.sendContent("");
}

static void handleSessionJson() {
    beginReportDownload(F("application/json"), F("rtk-base-session.json"));
    String line;
    line.reserve(320);
    String chunk;
    chunk.reserve(4096);
    line = F("{\"sampleIntervalSec\":1,\"capacity\":");
    line += sessionCapacity;
    line += F(",\"count\":"); line += sessionCount;
    line += F(",\"samples\":[");
    webServer.sendContent(line);
    uint32_t count = sessionCount;
    for (uint32_t i = 0; i < count; i++) {
        const SessionSample &s = sessionHistory[sessionPhysicalIndex(i)];
        line = i ? F(",{") : F("{");
        line += F("\"t\":"); line += s.uptimeSec;
        line += F(",\"state\":\""); line += sessionStateName(s.state); line += '"';
        line += F(",\"fix\":"); line += s.fix;
        line += F(",\"sv\":"); line += s.satellites;
        line += F(",\"hAcc\":"); line += String(s.hAccM, 3);
        line += F(",\"pdop\":"); line += String(s.pDop, 2);
        line += F(",\"surveySec\":"); line += s.surveySec;
        line += F(",\"surveyAcc\":"); line += String(s.surveyAccM, 3);
        line += F(",\"frames\":"); line += s.rtcmFrames;
        line += F(",\"bytes\":"); line += s.rtcmBytes;
        line += F(",\"fps\":"); line += String(s.rtcmFps, 1);
        line += F(",\"bytesPerSec\":"); line += String(s.rtcmBytesPerSec, 1);
        line += F(",\"crcErrors\":"); line += s.rtcmCrcErrors;
        line += F(",\"rssi\":"); line += s.wifiRssi;
        line += '}';
        chunk += line;
        if (chunk.length() >= 3072) {
            webServer.sendContent(chunk);
            chunk = "";
            serviceCriticalPathDuringReport();
        }
    }
    if (chunk.length()) webServer.sendContent(chunk);
    webServer.sendContent(F("]}"));
    webServer.sendContent("");
}

static void handleSessionSummary() {
    String j;
    j.reserve(768);
    j = F("{\"enabled\":"); j += sessionHistory ? F("true") : F("false");
    j += F(",\"count\":"); j += sessionCount;
    j += F(",\"capacity\":"); j += sessionCapacity;
    j += F(",\"sampleIntervalSec\":1");
    if (sessionCount) {
        const SessionSample &first = sessionHistory[sessionPhysicalIndex(0)];
        const SessionSample &last = sessionHistory[sessionPhysicalIndex(sessionCount - 1)];
        float minPdop = INFINITY, sumPdop = 0.0f;
        float minHAcc = INFINITY, sumHAcc = 0.0f;
        uint32_t pdopSamples = 0, hAccSamples = 0;
        uint8_t maxSatellites = 0;
        for (uint32_t i = 0; i < sessionCount; i++) {
            const SessionSample &s = sessionHistory[sessionPhysicalIndex(i)];
            if (isfinite(s.pDop) && s.pDop > 0.0f && s.pDop < 50.0f) {
                minPdop = min(minPdop, s.pDop); sumPdop += s.pDop; pdopSamples++;
            }
            if (isfinite(s.hAccM) && s.hAccM > 0.0f && s.hAccM < 100000.0f) {
                minHAcc = min(minHAcc, s.hAccM); sumHAcc += s.hAccM; hAccSamples++;
            }
            maxSatellites = max(maxSatellites, s.satellites);
        }
        j += F(",\"startUptimeSec\":"); j += first.uptimeSec;
        j += F(",\"endUptimeSec\":"); j += last.uptimeSec;
        j += F(",\"durationSec\":"); j += last.uptimeSec - first.uptimeSec;
        j += F(",\"maxSatellites\":"); j += maxSatellites;
        j += F(",\"minPdop\":"); j += pdopSamples ? String(minPdop, 2) : F("null");
        j += F(",\"avgPdop\":"); j += pdopSamples ? String(sumPdop / pdopSamples, 2) : F("null");
        j += F(",\"minHAccM\":"); j += hAccSamples ? String(minHAcc, 3) : F("null");
        j += F(",\"avgHAccM\":"); j += hAccSamples ? String(sumHAcc / hAccSamples, 3) : F("null");
        j += F(",\"rtcmFrames\":"); j += last.rtcmFrames - first.rtcmFrames;
        j += F(",\"rtcmBytes\":"); j += last.rtcmBytes - first.rtcmBytes;
        j += F(",\"newCrcErrors\":"); j += last.rtcmCrcErrors - first.rtcmCrcErrors;
    }
    j += '}';
    webServer.sendHeader(F("Cache-Control"), F("no-store"));
    webServer.send(200, F("application/json"), j);
}

static void handleApiStatus() {
    String j;
    j.reserve(10000);
    j = F("{\"state\":\"");
    j += baseState == RELAY ? "RELAY" : (baseState == SURVEY_IN ? "SURVEY_IN" : "ACQUIRING");
    j += F("\",\"uptime\":"); j += millis() / 1000;
    j += F(",\"config\":{\"surveyTime\":"); j += SVIN_MIN_TIME_S;
    j += F(",\"surveyAcc\":"); j += String(surveyTargetMm / 1000.0f, 3);
    j += F(",\"surveyTargetMm\":"); j += surveyTargetMm;
    j += F(",\"teleBaud\":"); j += TELE_BAUD;
    j += F("}");

    j += F(",\"svIn\":{\"time\":"); j += svInTime;
    j += F(",\"acc\":"); j += String(svInAcc, 3);
    j += F(",\"valid\":"); j += svInValid ? F("true") : F("false");
    j += F(",\"show\":");
    j += (baseState != RELAY || (surveyCompletedMs && millis() - surveyCompletedMs < SVIN_RESULT_HOLD_S * 1000UL)) ? F("true") : F("false");
    j += F(",\"stableSec\":"); j += gnssStableSec;
    j += F(",\"stableTarget\":"); j += GNSS_STABLE_TIME_S;
    j += F(",\"qualityReady\":"); j += gnssQualityReady ? F("true") : F("false");
    j += F(",\"history\":[");
    uint16_t start = svInHistLen < SVIN_HISTORY_SIZE ? 0 : svInHistIdx;
    for (uint16_t i = 0; i < svInHistLen; i++) {
        if (i) j += ',';
        j += String(svInHistory[(start + i) % SVIN_HISTORY_SIZE], 3);
    }
    j += F("]}");

    j += F(",\"gnss\":{\"ready\":"); j += gnssReady ? F("true") : F("false");
    j += F(",\"sv\":"); j += baseSV;
    j += F(",\"fix\":"); j += baseFix;
    j += F(",\"hAcc\":"); j += String(baseHAcc, 3);
    j += F(",\"pdop\":"); j += String(basePDOP, 2);
    j += F(",\"qualityLive\":"); j += qualityLive ? F("true") : F("false");
    j += F(",\"module\":"); appendJsonString(j, gnssModule);
    j += F(",\"firmware\":"); appendJsonString(j, gnssFirmware);
    j += F(",\"protocol\":"); appendJsonString(j, gnssProtocol);
    j += F("}");

    j += F(",\"signals\":{\"gps\":["); j += sigGpsEna; j += ','; j += sigGpsL1; j += ','; j += sigGpsL2; j += ']';
    j += F(",\"glo\":["); j += sigGloEna; j += ','; j += sigGloL1; j += ','; j += sigGloL2; j += ']';
    j += F(",\"gal\":["); j += sigGalEna; j += ','; j += sigGalE1; j += ','; j += sigGalE5; j += ']';
    j += F(",\"bds\":["); j += sigBdsEna; j += ','; j += sigBdsB1; j += ','; j += sigBdsB2; j += F("]}");

    j += F(",\"satellites\":{\"ageSec\":");
    j += lastNavSatMs ? (millis() - lastNavSatMs) / 1000 : 9999;
    j += F(",\"items\":[");
    for (uint8_t i = 0; i < satelliteCount; i++) {
        if (i) j += ',';
        j += F("{\"g\":"); j += satellites[i].gnssId;
        j += F(",\"id\":"); j += satellites[i].svId;
        j += F(",\"cno\":"); j += satellites[i].cno;
        j += F(",\"el\":"); j += satellites[i].elevation;
        j += F(",\"az\":"); j += satellites[i].azimuth;
        j += F(",\"used\":"); j += satellites[i].used ? F("true") : F("false");
        j += F(",\"health\":"); j += satellites[i].health;
        j += '}';
    }
    j += F("]}");

    j += F(",\"rtcm\":{\"frames\":"); j += rtcmFramesSent;
    j += F(",\"bytes\":"); j += rtcmBytesSent;
    j += F(",\"fps\":"); j += String(rtcmFramesPerSec, 1);
    j += F(",\"bps\":"); j += String(rtcmBytesPerSec, 1);
    j += F(",\"crcErrors\":"); j += rtcmCrcErrors;
    j += F(",\"lengthErrors\":"); j += rtcmLengthErrors;
    j += F(",\"ageSec\":"); j += lastRtcmMs ? (millis() - lastRtcmMs) / 1000 : 9999;
    j += F(",\"txFree\":"); j += Tele_Serial.availableForWrite();
    j += F(",\"types\":{");
    for (uint8_t i = 0; i < 6; i++) {
        if (i) j += ',';
        j += '"'; j += RTCM_TYPES[i]; j += F("\":{\"count\":"); j += rtcmTypeCount[i];
        j += F(",\"ageSec\":"); j += rtcmTypeLastMs[i] ? (millis() - rtcmTypeLastMs[i]) / 1000 : 9999;
        j += '}';
    }
    j += F("}}");

    double lat = savedLat / 1e7 + savedLatHp / 1e9;
    double lon = savedLon / 1e7 + savedLonHp / 1e9;
    double alt = savedAltCm / 100.0 + savedAltHp / 10000.0;
    j += F(",\"pos\":{\"saved\":"); j += hasSavedPos ? F("true") : F("false");
    j += F(",\"lat\":"); j += String(lat, 9);
    j += F(",\"lon\":"); j += String(lon, 9);
    j += F(",\"altEllipsoid\":"); j += String(alt, 4);
    j += F(",\"surveySec\":"); j += savedSurveySec;
    j += F(",\"surveyAccMm\":"); j += savedSurveyAccMm;
    j += F("}");

    j += F(",\"system\":{\"heap\":"); j += ESP.getFreeHeap();
    j += F(",\"minHeap\":"); j += ESP.getMinFreeHeap();
    j += F(",\"psramFree\":"); j += ESP.getFreePsram();
    j += F(",\"reportEnabled\":"); j += sessionHistory ? F("true") : F("false");
    j += F(",\"reportSamples\":"); j += sessionCount;
    j += F(",\"reportCapacity\":"); j += sessionCapacity;
    j += F(",\"wifiConnected\":"); j += WiFi.status() == WL_CONNECTED ? F("true") : F("false");
    j += F(",\"wifiRssi\":"); j += WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    j += F(",\"ip\":"); appendJsonString(j, WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("-"));
    j += F(",\"build\":\"");
    j += F(__DATE__ " " __TIME__);
    j += F("\"}}");

    webServer.sendHeader(F("Cache-Control"), F("no-store"));
    webServer.sendHeader(F("Access-Control-Allow-Origin"), F("*"));
    webServer.send(200, F("application/json"), j);
}

static void handleRoot() {
    if (LittleFS.exists(F("/index.html"))) {
        File f = LittleFS.open(F("/index.html"), "r");
        webServer.streamFile(f, F("text/html"));
        f.close();
        return;
    }
    webServer.send(200, F("text/html"),
        F("<!doctype html><meta charset='utf-8'><title>RTK Base</title>"
          "<style>body{font:16px monospace;background:#111;color:#ddd;padding:2rem}a{color:#4af}</style>"
          "<h2>RTK Base Station</h2><p>Dashboard filesystem is not installed.</p>"
          "<p><a href='/api/status'>Open JSON status</a></p>"));
}

static void startNetworkServices() {
    if (networkReady) return;
    MDNS.begin(WIFI_HOSTNAME);
    LittleFS.begin(false);
    webServer.on("/", HTTP_GET, handleRoot);
    webServer.serveStatic("/vendor/", LittleFS, "/vendor/");
    webServer.on("/api/status", HTTP_GET, handleApiStatus);
    webServer.on("/api/report/summary", HTTP_GET, handleSessionSummary);
    webServer.on("/api/report/session.csv", HTTP_GET, handleSessionCsv);
    webServer.on("/api/report/session.json", HTTP_GET, handleSessionJson);
    webServer.on("/api/survey-target", HTTP_POST, []() {
        if (!webServer.hasArg("targetMm")) {
            webServer.send(400, F("text/plain"), F("Missing targetMm"));
            return;
        }
        long requested = webServer.arg("targetMm").toInt();
        if (requested < SVIN_MIN_TARGET_MM || requested > SVIN_MAX_TARGET_MM) {
            webServer.send(400, F("text/plain"), F("Target must be between 10 and 10000 mm"));
            return;
        }
        surveyTargetMm = (uint32_t)requested;
        prefs.begin("rtk-base-t", false);
        prefs.putUInt("targetMm", surveyTargetMm);
        prefs.end();

        // The F9P receives the accuracy threshold when Survey-In starts. If a
        // survey is already active, return to acquisition so the new value is
        // applied cleanly. Never disturb an active fixed-position relay.
        if (baseState == SURVEY_IN) prepareForSurvey();
        Serial.printf("[BASE] Survey-In target set to %u mm%s\n", surveyTargetMm,
                      baseState == RELAY ? " (next re-survey)" : "");
        webServer.sendHeader(F("Location"), F("/"));
        webServer.send(303);
    });
    webServer.on("/api/resurvey", HTTP_POST, []() {
        clearBaseProfileAndSurvey();
        webServer.sendHeader(F("Location"), F("/"));
        webServer.send(303);
    });
    webServer.begin();

    ArduinoOTA.setHostname(WIFI_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() { Serial.println(F("[OTA] Update started; RTCM service paused")); });
    ArduinoOTA.onEnd([]() { Serial.println(F("[OTA] Update complete")); });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] Error %u\n", e); });
    ArduinoOTA.begin();
    networkReady = true;
    Serial.printf("[WEB] http://%s.local  (%s)\n", WIFI_HOSTNAME, WiFi.localIP().toString().c_str());
}

static void beginWifiAttempt() {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(WIFI_HOSTNAME);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    wifiAttempting = true;
    wifiAttemptStartedMs = millis();
    lastWifiAttemptMs = millis();
    Serial.println(F("[WiFi] Connecting in background"));
}

static void serviceNetwork() {
    if (WiFi.status() == WL_CONNECTED) {
#ifdef RGB_LED_PIN
        if (!wifiWasConnected) {
            wifiWasConnected = true;
            triggerWifiSuccessAnimation();
        }
#endif
        wifiAttempting = false;
        startNetworkServices();
        if (networkReady) {
            webServer.handleClient();
            ArduinoOTA.handle();
        }
        return;
    }
#ifdef RGB_LED_PIN
    wifiWasConnected = false;
#endif
    if (wifiAttempting && millis() - wifiAttemptStartedMs > WIFI_TIMEOUT_S * 1000UL) {
        wifiAttempting = false;
        Serial.println(F("[WiFi] Unavailable; RTK continues and Wi-Fi will retry"));
    }
    if (!wifiAttempting && millis() - lastWifiAttemptMs > WIFI_RETRY_S * 1000UL)
        beginWifiAttempt();
}

static void saveCompletedSurveyPosition() {
    if (!myGNSS.getHPPOSLLH()) {
        Serial.println(F("[BASE] HPPOSLLH unavailable; refusing to save imprecise profile"));
        return;
    }
    savedLat = myGNSS.getHighResLatitude();
    savedLatHp = myGNSS.getHighResLatitudeHp();
    savedLon = myGNSS.getHighResLongitude();
    savedLonHp = myGNSS.getHighResLongitudeHp();
    int64_t altTenthMm = (int64_t)myGNSS.getElipsoid() * 10 + myGNSS.getElipsoidHp();
    savedAltCm = (int32_t)(altTenthMm / 100);
    savedAltHp = (int8_t)(altTenthMm - (int64_t)savedAltCm * 100);
    savedSurveySec = svInTime;
    savedSurveyAccMm = (uint32_t)lroundf(svInAcc * 1000.0f);
    saveBaseProfile();
    baseState = RELAY;
    surveyCompletedMs = millis();
    Serial.printf("[BASE] Profile saved: %.9f, %.9f, ellipsoid %.4fm\n",
                  savedLat / 1e7 + savedLatHp / 1e9,
                  savedLon / 1e7 + savedLonHp / 1e9,
                  savedAltCm / 100.0 + savedAltHp / 10000.0);
}

void setup() {
    Serial.begin(115200);
#ifdef RGB_LED_PIN
    rgbLed.begin();
    rgbLed.setBrightness(48);
    setStatusLed(0, 0, 0);
#endif
    delay(500);
    Serial.println(F("\n=== Headless RTK Base Station (3DR) ==="));
    beginSessionHistory();

    Tele_Serial.setRxBufferSize(512);
    Tele_Serial.begin(TELE_BAUD, SERIAL_8N1, TELE_RX_PIN, TELE_TX_PIN);
    GPS_Serial.setRxBufferSize(4096);
    GPS_Serial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    delay(1000);

    if (myGNSS.begin(GPS_Serial)) {
        gnssReady = true;
        bool configured = configureGNSS();
        Serial.printf("[GNSS] Connected; configuration %s\n", configured ? "OK" : "has warnings");
    } else {
        Serial.println(F("[GNSS] Not responding; background retry enabled"));
    }
    beginWifiAttempt();
}

void loop() {
    serviceNetwork();
    serviceStatusLed();

    if (!gnssReady) {
        static uint32_t lastRetryMs = 0;
        if (millis() - lastRetryMs > 3000) {
            lastRetryMs = millis();
            if (myGNSS.begin(GPS_Serial)) {
                gnssReady = true;
                configureGNSS();
                Serial.println(F("[GNSS] Reconnected"));
            }
        }
        recordSessionSample();
        delay(2);
        return;
    }

    // This services UBX monitoring and invokes processRTCM for RTCM bytes.
    myGNSS.checkUblox();
    myGNSS.checkCallbacks();

    static uint32_t lastStatusMs = 0;
    static uint32_t previousFrames = 0;
    static uint32_t previousBytes = 0;
    if (millis() - lastStatusMs >= 1000) {
        uint32_t elapsed = millis() - lastStatusMs;
        lastStatusMs = millis();
        // Counters are cleared when a new survey is requested. Avoid unsigned
        // wrap if that happens between two samples.
        rtcmFramesPerSec = rtcmFramesSent >= previousFrames ?
            (rtcmFramesSent - previousFrames) * 1000.0f / elapsed : 0.0f;
        rtcmBytesPerSec = rtcmBytesSent >= previousBytes ?
            (rtcmBytesSent - previousBytes) * 1000.0f / elapsed : 0.0f;
        previousFrames = rtcmFramesSent;
        previousBytes = rtcmBytesSent;

        baseSV = myGNSS.getSIV();
        baseFix = myGNSS.getFixType();
        baseHAcc = myGNSS.getHorizontalAccEst() / 1000.0f;
        basePDOP = myGNSS.getPDOP() / 100.0f;
        qualityLive = lastNavSatMs && millis() - lastNavSatMs < 3000;

        gnssQualityReady = qualityLive && baseFix == 3 && baseSV >= GNSS_MIN_SV &&
                           basePDOP > 0.0f && basePDOP <= GNSS_MAX_PDOP &&
                           baseHAcc > 0.0f && baseHAcc <= GNSS_MAX_HACC_M;

        if (baseState == ACQUIRING) {
            if (gnssQualityReady) {
                if (gnssStableSec < GNSS_STABLE_TIME_S) gnssStableSec++;
            } else {
                gnssStableSec = 0;
            }
            Serial.printf("[ACQUIRE] stable=%u/%us fix=%u SV=%u hAcc=%.3fm PDOP=%.2f\n",
                          gnssStableSec, GNSS_STABLE_TIME_S, baseFix, baseSV, baseHAcc, basePDOP);
            if (gnssStableSec >= GNSS_STABLE_TIME_S) {
                float surveyTargetM = surveyTargetMm / 1000.0f;
                Serial.printf("[BASE] Starting Survey-In: %us, %u mm\n",
                              SVIN_MIN_TIME_S, surveyTargetMm);
                if (myGNSS.enableSurveyMode(SVIN_MIN_TIME_S, surveyTargetM)) {
                    svInHistIdx = 0;
                    svInHistLen = 0;
                    svInTime = 0;
                    svInAcc = 0;
                    svInValid = false;
                    baseState = SURVEY_IN;
                } else {
                    gnssStableSec = 0;
                    Serial.println(F("[BASE] Failed to start Survey-In; retrying after stabilization"));
                }
            }
        } else if (baseState == SURVEY_IN) {
            svInTime = myGNSS.getSurveyInObservationTimeFull();
            svInAcc = myGNSS.getSurveyInMeanAccuracy();
            svInValid = myGNSS.getSurveyInValid();
            if (svInTime > 0 && isfinite(svInAcc) && svInAcc > 0.0f) {
                svInHistory[svInHistIdx] = svInAcc;
                svInHistIdx = (svInHistIdx + 1) % SVIN_HISTORY_SIZE;
                if (svInHistLen < SVIN_HISTORY_SIZE) svInHistLen++;
            }
            Serial.printf("[SVIN] %us %.3fm valid=%s SV=%u PDOP=%.2f\n",
                          svInTime, svInAcc, svInValid ? "yes" : "no", baseSV, basePDOP);
            // This independent floor keeps a useful plot even if the receiver
            // configuration is shortened in the future. The current F9P
            // requirement remains the safer 300 seconds above.
            if (svInValid && svInTime >= SVIN_PLOT_MIN_TIME_S) saveCompletedSurveyPosition();
        } else {
            uint32_t age = lastRtcmMs ? (millis() - lastRtcmMs) / 1000 : 9999;
            Serial.printf("[RTCM] %.1f fps %.0f B/s total=%u CRCerr=%u age=%us\n",
                          rtcmFramesPerSec, rtcmBytesPerSec, rtcmFramesSent,
                          rtcmCrcErrors, age);
        }
    }
    recordSessionSample();
    serviceStatusLed();
    delay(1);
}
