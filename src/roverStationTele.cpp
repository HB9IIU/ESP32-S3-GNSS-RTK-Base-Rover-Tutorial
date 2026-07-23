// =============================================================================
// RTK Rover Station (Telemetry)  —  Heltec V4  +  u-blox ZED-F9P
// =============================================================================
// Receives RTCM3 correction bytes from the base station via a 3DR/SiK radio
// module connected to UART2.  Every byte is forwarded directly to the ZED-F9P
// UART so the module can compute an RTK Fixed position (~1 cm accuracy).
//
// The 3DR link is a transparent byte stream so no packet framing is needed.
// A lightweight RTCM3 frame tracker runs in parallel to count message types
// for display and the web API (non-blocking, does not affect forwarding).
//
// Fix quality progression:
//   NO FIX  →  3D FIX  →  RTK FLOAT  →  RTK FIXED
//
// Wiring (ZED-F9P ↔ Heltec V4):
//   ZED-F9P TX1  →  GPIO4  (GPS_RX_PIN)
//   ZED-F9P RX1  →  GPIO5  (GPS_TX_PIN)
//   VCC 3.3 V    →  3.3 V
//   GND          →  GND
//
// Wiring (3DR radio ↔ Heltec V4):
//   3DR TX  →  GPIO33  (TELE_RX_PIN)
//   3DR RX  →  GPIO34  (TELE_TX_PIN)
//   VCC 5 V →  5 V (or 3.3 V depending on your 3DR module)
//   GND     →  GND
// =============================================================================

#include <Arduino.h>
#include <SparkFun_u-blox_GNSS_v3.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#ifdef RGB_LED_PIN
#include <Adafruit_NeoPixel.h>
#endif

// =============================================================================
// PIN DEFINITIONS  —  defaults for Heltec V4; override via build_flags for S3
// =============================================================================
#ifndef GPS_RX_PIN
#define GPS_RX_PIN     4   // UART1 RX ← ZED-F9P TX1
#endif
#ifndef GPS_TX_PIN
#define GPS_TX_PIN     5   // UART1 TX → ZED-F9P RX1
#endif
#ifndef TELE_RX_PIN
#define TELE_RX_PIN   33   // UART2 RX ← 3DR radio TX
#endif
#ifndef TELE_TX_PIN
#define TELE_TX_PIN   34   // UART2 TX → 3DR radio RX
#endif
#ifndef REF_BUTTON_PIN
#define REF_BUTTON_PIN 21  // Active-low push button to GND
#endif

// =============================================================================
// RGB LED  (onboard WS2812 on S3 devkit — omitted on Heltec V4)
//
// Boot:      rainbow sweep → green flash
// WiFi OK:   2× blue flash (setup)
// WiFi fail: 3× orange flash (setup)
// OTA:       solid purple
// Loop status (priority order):
//   ZED-F9P missing → red 8 Hz
//   No GNSS fix     → red 1 Hz
//   3D fix          → yellow 1 Hz
//   RTK Float       → yellow 4 Hz
//   RTK Fixed       → green solid
// RTCM blip:        white 50 ms overlay on any status
// =============================================================================
#ifdef RGB_LED_PIN
static Adafruit_NeoPixel rgbLed(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

static void ledSet(uint8_t r, uint8_t g, uint8_t b) {
    rgbLed.setPixelColor(0, rgbLed.Color(r, g, b));
    rgbLed.show();
}

// Trigger a 50 ms blip — external LED on BLIP_LED_PIN if defined, else white RGB flash
static uint32_t ledBlipUntil = 0;
static void ledRtcmBlip() {
    if (millis() >= ledBlipUntil) {
#ifdef BLIP_LED_PIN
        digitalWrite(BLIP_LED_PIN, HIGH);
#endif
        ledBlipUntil = millis() + 50;
    }
}

// Boot event flashes — blocking, called once from setup()
// connected=true → 2× blue,  connected=false → 3× orange
static void ledWifiEvent(bool connected) {
    uint8_t r = connected ?   0 : 255;
    uint8_t g = connected ?   0 :  80;
    uint8_t b = connected ? 255 :   0;
    uint8_t n = connected ?   2 :   3;
    for (uint8_t i = 0; i < n; i++) {
        ledSet(r, g, b); delay(150);
        ledSet(0, 0, 0); delay(150);
    }
}

static void ledBootShow() {
    rgbLed.begin();
    rgbLed.setBrightness(40);

    for (uint8_t i = 0; i < 3; i++) {
        ledSet(255, 255, 255); delay(80);
        ledSet(0, 0, 0);       delay(80);
    }
    delay(100);

    const uint32_t colors[] = {
        rgbLed.Color(255,   0,   0),
        rgbLed.Color(255, 128,   0),
        rgbLed.Color(255, 255,   0),
        rgbLed.Color(  0, 255,   0),
        rgbLed.Color(  0, 255, 128),
        rgbLed.Color(  0, 128, 255),
        rgbLed.Color(  0,   0, 255),
        rgbLed.Color(128,   0, 255),
    };
    for (uint8_t i = 0; i < 8; i++) {
        rgbLed.setPixelColor(0, colors[i]); rgbLed.show(); delay(120);
    }

    ledSet(0, 255, 0); delay(400);
    ledSet(0, 0, 0);
}
#endif

// =============================================================================
// 3DR TELEMETRY PARAMETERS
// =============================================================================
#define TELE_BAUD    57600    // 3DR SiK radio factory default

// =============================================================================
// WiFi
// =============================================================================
#define WIFI_SSID      "MESH"
#define WIFI_PASS      "Nestle2010Nestle"
#define WIFI_HOSTNAME  "rtk-rover-tele"     // http://rtk-rover-tele.local
#define WIFI_TIMEOUT_S  15
#define OTA_PASSWORD   "rtk-rover-tele-ota"

// =============================================================================
// RTCM3 FRAMING  (for stats / logging only — forwarding is byte-by-byte)
// =============================================================================
#define RTCM_PREAMBLE    0xD3
#define RTCM_MAX_FRAME   1029

// =============================================================================
// OBJECTS
// =============================================================================
HardwareSerial GPS_Serial(1);
HardwareSerial Tele_Serial(2);
SFE_UBLOX_GNSS_SERIAL myGNSS;
WebServer webServer(80);

// =============================================================================
// STATE
// =============================================================================
static bool     gnssReady          = false;
static bool     wifiReady          = false;
static uint32_t rtcmPktsReceived   = 0;
static uint32_t rtcmBytesReceived  = 0;
static uint32_t lastTeleMs         = 0;   // millis() of last byte received

// RTCM message type counters  (1005, 1074, 1084, 1094, 1124, 1230)
static const uint16_t RTCM_TYPES[]  = {1005, 1074, 1084, 1094, 1124, 1230};
static uint32_t       rtcmTypeCnt[] = {   0,    0,    0,    0,    0,    0};

// Position history for N/E scatter chart (RTK FIXED points only, 1 Hz)
#define POS_HIST_SIZE 120
struct PosPoint { int32_t lat; int32_t lon; };
static PosPoint posHistory[POS_HIST_SIZE];
static uint8_t  posHistIdx = 0;
static uint8_t  posHistLen = 0;

// Cached GNSS values — updated every second
static int32_t  gnssLat  = 0;
static int32_t  gnssLon  = 0;
static int32_t  gnssAlt  = 0;
static float    gnssHAcc = 0.0f;
static float    gnssVAcc = 0.0f;
static uint8_t  gnssFix  = 0;
static uint8_t  gnssCarr = 0;
static uint8_t  gnssSV   = 0;

// =============================================================================
// TRACK RECORDING  —  PSRAM circular buffer
// =============================================================================
struct TrackPoint {
    uint32_t ms;
    int32_t  lat;      // degrees * 1e7
    int8_t   latHp;    // degrees * 1e9 extension
    int32_t  lon;      // degrees * 1e7
    int8_t   lonHp;    // degrees * 1e9 extension
    int32_t  altMm;
    uint16_t hAccMm;
    uint16_t spdMms;   // ground speed mm/s
    uint16_t hdg10;    // heading degrees * 10
    uint8_t  fix;
    uint8_t  carr;
};  // 22 bytes

#define MAX_WAYPOINTS 50
struct Waypoint { uint32_t ms; int32_t lat; int32_t lon; int32_t altMm; uint32_t trackIdx; };

static TrackPoint* trackBuf      = nullptr;
static uint32_t   trackCapacity  = 0;
static uint32_t   trackHead      = 0;
static uint32_t   trackCount     = 0;
static bool       trackRecording = true;
static uint32_t   trackStartMs   = 0;
static Waypoint   waypoints[MAX_WAYPOINTS];
static uint8_t    waypointCount  = 0;

// Non-blocking LED status display — reads gnssReady/gnssFix/gnssCarr declared above
#ifdef RGB_LED_PIN
static void ledUpdate() {
    uint32_t now = millis();

#ifndef BLIP_LED_PIN
    if (now < ledBlipUntil) { ledSet(255, 255, 255); return; }
#endif

    uint8_t  r = 0, g = 0, b = 0;
    uint32_t halfPeriod = 0;   // 0 = solid

    if (!gnssReady) {
        r = 255;               halfPeriod = 62;   // red  8 Hz
    } else if (gnssFix < 3) {
        r = 255;               halfPeriod = 500;  // red  1 Hz
    } else if (gnssCarr == 2) {
        if (trackBuf && trackRecording) {
            // Breathe green ↔ teal to show active recording
            static uint32_t breathMs = 0; static bool breathPhase = false;
            if (now - breathMs >= 1500) { breathMs = now; breathPhase = !breathPhase; }
            breathPhase ? ledSet(0, 255, 0) : ledSet(0, 160, 120);
            return;
        }
        g = 255;                                  // solid green when paused
    } else if (gnssCarr == 1) {
        r = 255; g = 160;      halfPeriod = 125;  // yellow 4 Hz
    } else {
        r = 255; g = 160;      halfPeriod = 500;  // yellow 1 Hz
    }

    if (halfPeriod == 0) {
        ledSet(r, g, b);
    } else {
        static uint32_t lastToggle = 0;
        static bool     phase = false;
        if (now - lastToggle >= halfPeriod) { lastToggle = now; phase = !phase; }
        phase ? ledSet(r, g, b) : ledSet(0, 0, 0);
    }
}
#endif

// User-defined local reference and ENU displacement history.
#define REF_HIST_SIZE 120
struct RefPoint { int32_t east; int32_t north; int32_t up; };
static bool     referenceSet = false;
static int32_t  referenceLat = 0;
static int32_t  referenceLon = 0;
static int32_t  referenceAlt = 0;
static RefPoint referenceHistory[REF_HIST_SIZE];
static uint8_t  referenceHistIdx = 0;
static uint8_t  referenceHistLen = 0;
static uint32_t referenceSession = 0;
static uint32_t referenceErrorUntil = 0;

// =============================================================================
// HELPERS
// =============================================================================
static const char* fixLabel() {
    if (gnssFix < 2) return "NO FIX";
    if (gnssFix == 2) return "2D";
    if (gnssCarr == 2) return "RTK FIXED";
    if (gnssCarr == 1) return "RTK FLOAT";
    return "3D FIX";
}

static bool referenceQualityOK() {
    return gnssFix >= 3 && gnssCarr == 2 && lastTeleMs &&
           millis() - lastTeleMs <= 2000;
}

static void referenceOffsets(int32_t &east, int32_t &north, int32_t &up) {
    double refLatDeg = referenceLat / 1e7;
    double cosLat = cos(refLatDeg * PI / 180.0);
    east = (int32_t)lround((gnssLon - referenceLon) / 1e7 * 111320.0 * cosLat * 1000.0);
    north = (int32_t)lround((gnssLat - referenceLat) / 1e7 * 111320.0 * 1000.0);
    up = gnssAlt - referenceAlt;
}

static bool setReferencePosition() {
    if (!referenceQualityOK()) {
        referenceErrorUntil = millis() + 4000;
        Serial.println(F("[REF] Refused: need RTK FIXED with fresh corrections"));
        return false;
    }
    referenceLat = gnssLat;
    referenceLon = gnssLon;
    referenceAlt = gnssAlt;
    referenceSet = true;
    referenceSession++;
    referenceHistIdx = 0;
    referenceHistLen = 1;
    referenceHistory[0] = {0, 0, 0};
    Serial.printf("[REF] Set %.8f, %.8f, %.3fm\n",
                  referenceLat / 1e7, referenceLon / 1e7, referenceAlt / 1000.0f);
    return true;
}

static void appendReferenceSample() {
    if (!referenceSet || !referenceQualityOK()) return;
    int32_t east, north, up;
    referenceOffsets(east, north, up);
    referenceHistory[referenceHistIdx] = {east, north, up};
    referenceHistIdx = (referenceHistIdx + 1) % REF_HIST_SIZE;
    if (referenceHistLen < REF_HIST_SIZE) referenceHistLen++;
}

static bool addWaypoint() {
    if (!gnssReady || gnssFix < 3 || gnssCarr < 2 || waypointCount >= MAX_WAYPOINTS) return false;
    Waypoint& w = waypoints[waypointCount++];
    w.ms       = millis();
    w.lat      = gnssLat;
    w.lon      = gnssLon;
    w.altMm    = gnssAlt;
    w.trackIdx = (trackHead == 0 ? trackCapacity - 1 : trackHead - 1);
    Serial.printf("[Track] Waypoint %u  %.7f, %.7f\n", waypointCount, gnssLat/1e7, gnssLon/1e7);
    return true;
}

static float trackDistanceM() {
    if (trackCount < 2 || !trackBuf) return 0.0f;
    double dist = 0;
    uint32_t start = (trackCount == trackCapacity) ? trackHead : 0;
    double prevLat = 0, prevLon = 0;
    for (uint32_t i = 0; i < trackCount; i++) {
        const TrackPoint& p = trackBuf[(start + i) % trackCapacity];
        double lat = p.lat / 1e7 + p.latHp / 1e9;
        double lon = p.lon / 1e7 + p.lonHp / 1e9;
        if (i > 0) {
            double dlat = (lat - prevLat) * 111320.0;
            double dlon = (lon - prevLon) * 111320.0 * cos(prevLat * PI / 180.0);
            dist += sqrt(dlat*dlat + dlon*dlon);
        }
        prevLat = lat; prevLon = lon;
    }
    return (float)dist;
}

// Short press → waypoint mark  |  Long press (≥2 s) → set reference position
static void serviceReferenceButton() {
    static bool     lastRaw   = HIGH;
    static uint32_t pressedMs = 0;
    static bool     longFired = false;
    bool raw = digitalRead(REF_BUTTON_PIN);
    if (raw == LOW && lastRaw == HIGH) { pressedMs = millis(); longFired = false; }
    if (raw == LOW && !longFired && millis() - pressedMs >= 2000) {
        longFired = true;
        setReferencePosition();
    }
    if (raw == HIGH && lastRaw == LOW && !longFired && millis() - pressedMs >= 50) {
#ifdef RGB_LED_PIN
        ledSet(0, 200, 200); delay(80); ledSet(0, 0, 0); delay(60);
        ledSet(0, 200, 200); delay(80); ledSet(0, 0, 0);
#endif
        addWaypoint();
    }
    lastRaw = raw;
}

// =============================================================================
// RTCM3 FRAME TRACKER  —  stats & logging only; forwarding is independent
// =============================================================================
static void trackRtcmByte(uint8_t b) {
    static uint8_t  hdr[5];
    static uint16_t idx      = 0;
    static int32_t  expected = -1;

    if (expected == -1) {
        if (b != RTCM_PREAMBLE) return;
        idx = 0; expected = 0;
    }
    if (idx < 5) hdr[idx] = b;
    idx++;

    if (expected == 0 && idx == 3) {
        uint16_t payloadLen = ((uint16_t)(hdr[1] & 0x03) << 8) | hdr[2];
        expected = 3 + (int32_t)payloadLen + 3;
    }
    if (expected > RTCM_MAX_FRAME) { idx = 0; expected = -1; return; }

    if (expected > 0 && idx == (uint16_t)expected) {
        uint16_t msgType = (expected >= 5) ?
                           (((uint16_t)hdr[3] << 4) | (hdr[4] >> 4)) : 0;
        rtcmPktsReceived++;
        lastTeleMs = millis();
#ifdef RGB_LED_PIN
        ledRtcmBlip();
#endif
        for (uint8_t i = 0; i < 6; i++)
            if (RTCM_TYPES[i] == msgType) { rtcmTypeCnt[i]++; break; }
        if (msgType)
            Serial.printf("[3DR] RX  RTCM %u  %u bytes  (total: %u pkts)\n",
                          msgType, (unsigned)expected, rtcmPktsReceived);
        idx = 0; expected = -1;
    }
}

// =============================================================================
// JSON STATUS API  —  /api/status
// =============================================================================
static void handleApiStatus() {
    String j;
    j.reserve(10000);
    j  = F("{\"fix\":\""); j += fixLabel();
    j += F("\",\"fixType\":"); j += gnssFix;
    j += F(",\"carr\":"); j += gnssCarr;
    j += F(",\"lat\":"); j += String(gnssLat / 1e7, 8);
    j += F(",\"lon\":"); j += String(gnssLon / 1e7, 8);
    j += F(",\"alt\":"); j += String(gnssAlt / 1000.0f, 2);
    j += F(",\"hAcc\":"); j += String(gnssHAcc, 4);
    j += F(",\"vAcc\":"); j += String(gnssVAcc, 4);
    j += F(",\"sv\":"); j += gnssSV;
    j += F(",\"tele\":{\"pkts\":"); j += rtcmPktsReceived;
    j += F(",\"bytes\":"); j += rtcmBytesReceived;
    j += F(",\"baud\":"); j += TELE_BAUD;
    uint32_t ageSec = lastTeleMs ? (millis() - lastTeleMs) / 1000 : 9999;
    j += F(",\"ageSec\":"); j += ageSec;
    j += '}';
    j += F(",\"rtcm\":{");
    for (uint8_t i = 0; i < 6; i++) {
        if (i) j += ',';
        j += '"'; j += RTCM_TYPES[i]; j += '"';
        j += ':'; j += rtcmTypeCnt[i];
    }
    j += '}';
    j += F(",\"posHist\":[");
    {
        uint8_t start = (posHistLen == POS_HIST_SIZE) ? posHistIdx : 0;
        for (uint8_t i = 0; i < posHistLen; i++) {
            if (i) j += ',';
            uint8_t idx = (start + i) % POS_HIST_SIZE;
            j += '['; j += (long)posHistory[idx].lat;
            j += ','; j += (long)posHistory[idx].lon; j += ']';
        }
    }
    j += ']';
    j += F(",\"reference\":{\"set\":"); j += referenceSet ? F("true") : F("false");
    j += F(",\"session\":"); j += referenceSession;
    j += F(",\"error\":");
    j += (referenceErrorUntil && millis() < referenceErrorUntil) ? F("true") : F("false");
    if (referenceSet) {
        int32_t east, north, up;
        referenceOffsets(east, north, up);
        double distance = sqrt((double)east * east + (double)north * north);
        double bearing = atan2((double)east, (double)north) * 180.0 / PI;
        if (bearing < 0) bearing += 360.0;
        j += F(",\"lat\":"); j += String(referenceLat / 1e7, 8);
        j += F(",\"lon\":"); j += String(referenceLon / 1e7, 8);
        j += F(",\"alt\":"); j += String(referenceAlt / 1000.0f, 3);
        j += F(",\"eastMm\":"); j += east;
        j += F(",\"northMm\":"); j += north;
        j += F(",\"upMm\":"); j += up;
        j += F(",\"distanceMm\":"); j += String(distance, 1);
        j += F(",\"bearingDeg\":"); j += String(bearing, 1);
    }
    j += F(",\"history\":[");
    uint8_t refStart = referenceHistLen == REF_HIST_SIZE ? referenceHistIdx : 0;
    for (uint8_t i = 0; i < referenceHistLen; i++) {
        if (i) j += ',';
        uint8_t idx = (refStart + i) % REF_HIST_SIZE;
        j += '['; j += referenceHistory[idx].east;
        j += ','; j += referenceHistory[idx].north;
        j += ','; j += referenceHistory[idx].up; j += ']';
    }
    j += F("]}");
    j += F(",\"uptime\":"); j += millis() / 1000;
    j += '}';
    webServer.sendHeader(F("Access-Control-Allow-Origin"), F("*"));
    webServer.send(200, F("application/json"), j);
}

// =============================================================================
// WEB PAGE  —  inline fallback; served when no /rover.html in LittleFS
// Polls /api/status every 2 s via JS fetch — no full-page reload.
// =============================================================================
static void handleRoot() {
    if (LittleFS.exists(F("/rover.html"))) {
        File f = LittleFS.open(F("/rover.html"), "r");
        webServer.sendHeader(F("Cache-Control"), F("no-store"));
        webServer.streamFile(f, F("text/html"));
        f.close();
        return;
    }

    String p;
    p.reserve(3800);

    p = F("<!DOCTYPE html><html><head>"
          "<meta charset='utf-8'>"
          "<title>RTK Rover (3DR)</title>"
          "<style>"
          "body{font-family:monospace;background:#111;color:#ccc;padding:20px;max-width:480px;margin:0 auto}"
          "h2{color:#4af;margin-bottom:4px}"
          "h3{color:#4af;margin:14px 0 4px;font-size:1em}"
          ".ok{color:#4d4}.warn{color:#fa0}.bad{color:#f44}"
          "td{padding:3px 0;width:50%}"
          "hr{border:0;border-top:1px solid #333;margin:10px 0}"
          ".fix{font-size:1.1em;font-weight:bold}"
          ".dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:5px}"
          "</style></head><body>"
          "<h2>&#x1F6F0; RTK Rover (3DR Tele)</h2>"

          "<table>"
          "<tr><td>Fix</td>"
              "<td><span class='fix' id='fix'>-</span></td></tr>"
          "<tr><td>Latitude</td>  <td><span id='lat'>-</span></td></tr>"
          "<tr><td>Longitude</td> <td><span id='lon'>-</span></td></tr>"
          "<tr><td>Altitude</td>  <td><span id='alt'>-</span></td></tr>"
          "<tr><td>H accuracy</td><td><span id='hacc'>-</span></td></tr>"
          "<tr><td>V accuracy</td><td><span id='vacc'>-</span></td></tr>"
          "<tr><td>Satellites</td><td><span id='sv'>-</span></td></tr>"
          "</table>"

          "<hr><h3>3DR Telemetry Link</h3>"
          "<table>");

    p += "<tr><td>Baud rate</td><td>"  + String(TELE_BAUD)    + F(" bps</td></tr>");
    p += "<tr><td>UART pins</td><td>RX=GPIO" + String(TELE_RX_PIN) +
         "  TX=GPIO" + String(TELE_TX_PIN)   + F("</td></tr>");

    p += F("<tr><td>Bytes received</td> <td><span id='bytes'>-</span></td></tr>"
           "<tr><td>Frames received</td><td><span id='pkts'>-</span></td></tr>"
           "<tr><td>Correction age</td> <td><span id='age'>-</span></td></tr>"
           "</table>"

           "<hr><h3>RTCM Message Counts</h3>"
           "<table id='rtcm'>"
           "<tr><td colspan='2' style='color:#555'>loading...</td></tr>"
           "</table>"

           "<hr>"
           "<p id='uptime' style='color:#444;font-size:0.85em'>connecting...</p>"

           "<script>"
           "const RN={1005:'ARP',1074:'GPS MSM4',1084:'GLO MSM4',"
                      "1094:'GAL MSM4',1124:'BDS MSM4',1230:'GLO Biases'};"
           "function upd(){"
             "fetch('/api/status').then(r=>r.json()).then(d=>{"
               // Fix
               "const fc=d.fix==='RTK FIXED'?'ok':d.fix==='RTK FLOAT'?'warn':'bad';"
               "const fe=document.getElementById('fix');"
               "fe.className='fix '+fc; fe.textContent=d.fix;"
               // Position
               "document.getElementById('lat').textContent=d.lat.toFixed(8)+' N';"
               "document.getElementById('lon').textContent=d.lon.toFixed(8)+' E';"
               "document.getElementById('alt').textContent=d.alt.toFixed(2)+' m';"
               "document.getElementById('hacc').textContent=(d.hAcc*1000).toFixed(1)+' mm';"
               "document.getElementById('vacc').textContent=(d.vAcc*1000).toFixed(1)+' mm';"
               "document.getElementById('sv').textContent=d.sv;"
               // 3DR link
               "document.getElementById('bytes').textContent=d.tele.bytes.toLocaleString();"
               "document.getElementById('pkts').textContent=d.tele.pkts.toLocaleString();"
               "const a=d.tele.ageSec,ac=a<5?'ok':a<15?'warn':'bad';"
               "document.getElementById('age').innerHTML="
                 "'<span class=\"'+ac+'\">'+(a<9999?a+' s':'never')+'</span>';"
               // RTCM types
               "let rt='';"
               "for(const[k,v]of Object.entries(d.rtcm))"
                 "rt+='<tr><td>'+k+' <span style=\"color:#555\">'+(RN[k]||'')+'</span></td>"
                      "<td>'+v.toLocaleString()+'</td></tr>';"
               "document.getElementById('rtcm').innerHTML=rt||"
                 "'<tr><td colspan=\"2\" style=\"color:#555\">none yet</td></tr>';"
               // Uptime
               "const u=d.uptime;"
               "document.getElementById('uptime').textContent="
                 "'uptime '+Math.floor(u/3600)+'h '+Math.floor(u%3600/60)+'m '+u%60+'s  ● live';"
             "}).catch(()=>{"
               "document.getElementById('uptime').textContent='connection lost...';"
             "})}"
           "upd();setInterval(upd,2000);"
           "</script></body></html>");

    webServer.send(200, F("text/html"), p);
}

// =============================================================================
// TRACK API HANDLERS
// =============================================================================
static void handleTrackInfo() {
    float dist = trackDistanceM();
    uint32_t durMs = (trackCount > 0 && trackStartMs > 0) ? millis() - trackStartMs : 0;
    String j;
    j.reserve(200);
    j  = F("{\"count\":");        j += trackCount;
    j += F(",\"capacity\":");     j += trackCapacity;
    j += F(",\"recording\":");    j += trackRecording ? F("true") : F("false");
    j += F(",\"durationMs\":");   j += durMs;
    j += F(",\"distanceM\":");    j += String(dist, 1);
    j += F(",\"waypointCount\":"); j += waypointCount;
    j += F(",\"bufferPct\":");    j += (trackCapacity ? (uint32_t)(trackCount * 100UL / trackCapacity) : 0);
    j += '}';
    webServer.send(200, F("application/json"), j);
}

static void handleTrackPoints() {
    if (!trackBuf || trackCount == 0) { webServer.send(200, F("application/json"), F("[]")); return; }
    uint32_t offset     = webServer.hasArg("offset")      ? (uint32_t)webServer.arg("offset").toInt()     : 0;
    uint32_t count      = webServer.hasArg("count")       ? (uint32_t)webServer.arg("count").toInt()      : 500;
    uint32_t decimation = webServer.hasArg("decimation")  ? (uint32_t)webServer.arg("decimation").toInt() : 1;
    if (count > 1000) count = 1000;
    if (decimation < 1) decimation = 1;
    uint32_t start = (trackCount == trackCapacity) ? trackHead : 0;
    String j;
    j.reserve(count * 85);
    j = '[';
    bool first = true;
    uint32_t written = 0;
    for (uint32_t i = offset; i < trackCount && written < count; i += decimation) {
        const TrackPoint& p = trackBuf[(start + i) % trackCapacity];
        double lat = p.lat / 1e7 + p.latHp / 1e9;
        double lon = p.lon / 1e7 + p.lonHp / 1e9;
        if (!first) j += ','; first = false;
        j += F("{\"ms\":"); j += p.ms;
        j += F(",\"lat\":"); j += String(lat, 8);
        j += F(",\"lon\":"); j += String(lon, 8);
        j += F(",\"alt\":"); j += String(p.altMm / 1000.0f, 3);
        j += F(",\"hAcc\":"); j += p.hAccMm;
        j += F(",\"spd\":"); j += String(p.spdMms / 1000.0f, 3);
        j += F(",\"hdg\":"); j += String(p.hdg10 / 10.0f, 1);
        j += F(",\"carr\":"); j += p.carr;
        j += '}';
        written++;
    }
    j += ']';
    webServer.send(200, F("application/json"), j);
}

static void handleTrackWaypoints() {
    String j = "[";
    for (uint8_t i = 0; i < waypointCount; i++) {
        const Waypoint& w = waypoints[i];
        if (i) j += ',';
        j += F("{\"ms\":"); j += w.ms;
        j += F(",\"lat\":"); j += String(w.lat / 1e7, 8);
        j += F(",\"lon\":"); j += String(w.lon / 1e7, 8);
        j += F(",\"alt\":"); j += String(w.altMm / 1000.0f, 3);
        j += F(",\"n\":"); j += i;
        j += '}';
    }
    j += ']';
    webServer.send(200, F("application/json"), j);
}

static void handleTrackExportGpx() {
    if (!trackBuf) { webServer.send(503, F("text/plain"), F("No buffer")); return; }
    webServer.sendHeader(F("Content-Disposition"), F("attachment; filename=\"track.gpx\""));
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.send(200, F("application/gpx+xml"), "");
    webServer.sendContent(F("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<gpx version=\"1.1\" creator=\"RTK Rover\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
        "<trk><name>RTK Track</name><trkseg>\n"));
    uint32_t start = (trackCount == trackCapacity) ? trackHead : 0;
    char line[128]; String chunk; chunk.reserve(2048);
    for (uint32_t i = 0; i < trackCount; i++) {
        const TrackPoint& p = trackBuf[(start + i) % trackCapacity];
        snprintf(line, sizeof(line),
            "<trkpt lat=\"%.8f\" lon=\"%.8f\"><ele>%.3f</ele></trkpt>\n",
            p.lat / 1e7 + p.latHp / 1e9, p.lon / 1e7 + p.lonHp / 1e9, p.altMm / 1000.0);
        chunk += line;
        if (chunk.length() >= 1800) { webServer.sendContent(chunk); chunk = ""; }
    }
    for (uint8_t i = 0; i < waypointCount; i++) {
        const Waypoint& w = waypoints[i];
        snprintf(line, sizeof(line),
            "<wpt lat=\"%.8f\" lon=\"%.8f\"><ele>%.3f</ele><name>WP%u</name></wpt>\n",
            w.lat / 1e7, w.lon / 1e7, w.altMm / 1000.0, i + 1);
        chunk += line;
    }
    if (chunk.length()) webServer.sendContent(chunk);
    webServer.sendContent(F("</trkseg></trk>\n</gpx>"));
    webServer.sendContent("");
}

static void handleTrackExportCsv() {
    if (!trackBuf) { webServer.send(503, F("text/plain"), F("No buffer")); return; }
    webServer.sendHeader(F("Content-Disposition"), F("attachment; filename=\"track.csv\""));
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.send(200, F("text/csv"), "");
    webServer.sendContent(F("ms,lat,lon,alt_m,hAcc_mm,speed_ms,heading_deg,fix,carr\n"));
    uint32_t start = (trackCount == trackCapacity) ? trackHead : 0;
    char line[110]; String chunk; chunk.reserve(2048);
    for (uint32_t i = 0; i < trackCount; i++) {
        const TrackPoint& p = trackBuf[(start + i) % trackCapacity];
        snprintf(line, sizeof(line), "%lu,%.8f,%.8f,%.3f,%u,%.3f,%.1f,%u,%u\n",
            (unsigned long)p.ms, p.lat / 1e7 + p.latHp / 1e9, p.lon / 1e7 + p.lonHp / 1e9,
            p.altMm / 1000.0, p.hAccMm, p.spdMms / 1000.0, p.hdg10 / 10.0, p.fix, p.carr);
        chunk += line;
        if (chunk.length() >= 1800) { webServer.sendContent(chunk); chunk = ""; }
    }
    if (chunk.length()) webServer.sendContent(chunk);
    webServer.sendContent("");
}

// =============================================================================
// WiFi + WEB SERVER SETUP  —  non-fatal, rover keeps working without WiFi
// =============================================================================
static void wifiSetup() {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(WIFI_HOSTNAME);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print(F("[WiFi] Connecting"));
    for (uint8_t i = 0; i < WIFI_TIMEOUT_S * 2 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500); Serial.print('.');
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("\n[WiFi] Not found — continuing without WiFi"));
#ifdef RGB_LED_PIN
        ledWifiEvent(false);
#endif
        return;
    }
    Serial.printf("\n[WiFi] Connected  IP: %s\n", WiFi.localIP().toString().c_str());
#ifdef RGB_LED_PIN
    ledWifiEvent(true);
#endif
    if (MDNS.begin(WIFI_HOSTNAME))
        Serial.printf("[WiFi] http://%s.local\n", WIFI_HOSTNAME);

    if (LittleFS.begin(false))
        Serial.println(F("[LittleFS] Mounted OK"));
    else
        Serial.println(F("[LittleFS] Mount failed"));

    webServer.on("/", handleRoot);
    webServer.serveStatic("/vendor/", LittleFS, "/vendor/");
    webServer.on("/api/status", handleApiStatus);
    webServer.on("/api/reference", HTTP_POST, []() {
        bool accepted = setReferencePosition();
        webServer.send(accepted ? 200 : 409, F("application/json"),
                       accepted ? F("{\"ok\":true}") : F("{\"ok\":false,\"error\":\"RTK FIXED with fresh corrections required\"}"));
    });
    webServer.on("/api/track/info",           handleTrackInfo);
    webServer.on("/api/track/points",         handleTrackPoints);
    webServer.on("/api/track/waypoints",      handleTrackWaypoints);
    webServer.on("/api/track/export/gpx",     handleTrackExportGpx);
    webServer.on("/api/track/export/csv",     handleTrackExportCsv);
    webServer.on("/api/track/waypoint", HTTP_POST, []() {
        bool ok = addWaypoint();
        webServer.send(ok ? 200 : 409, F("application/json"), ok ? F("{\"ok\":true}") : F("{\"ok\":false}"));
    });
    webServer.on("/api/track/clear", HTTP_POST, []() {
        trackHead = 0; trackCount = 0; waypointCount = 0; trackStartMs = 0;
        webServer.send(200, F("application/json"), F("{\"ok\":true}"));
    });
    webServer.on("/api/track/pause", HTTP_POST, []() {
        trackRecording = !trackRecording;
        webServer.send(200, F("application/json"), trackRecording ? F("{\"recording\":true}") : F("{\"recording\":false}"));
    });
    webServer.begin();
    Serial.println(F("[WebServer] Started"));

    ArduinoOTA.setHostname(WIFI_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
        Serial.println(F("[OTA] Update started"));
#ifdef RGB_LED_PIN
        ledSet(128, 0, 255);
#endif
    });
    ArduinoOTA.onEnd([]() { Serial.println(F("[OTA] Update complete")); });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] Error %u\n", e); });
    ArduinoOTA.begin();
    Serial.println(F("[OTA] Ready"));

    wifiReady = true;
}

// =============================================================================
// ZED-F9P CONFIGURATION  —  ROVER MODE
// =============================================================================
static void configureGNSS() {
    if (myGNSS.getModuleInfo()) {
        Serial.print(F("[ZED-F9P] Module   : ")); Serial.println(myGNSS.getModuleName());
        Serial.print(F("[ZED-F9P] Firmware : "));
        Serial.print(myGNSS.getFirmwareType()); Serial.print(' ');
        Serial.print(myGNSS.getFirmwareVersionHigh()); Serial.print('.');
        Serial.println(myGNSS.getFirmwareVersionLow());
        Serial.print(F("[ZED-F9P] Protocol : "));
        Serial.print(myGNSS.getProtocolVersionHigh()); Serial.print('.');
        Serial.println(myGNSS.getProtocolVersionLow());
    } else {
        Serial.println(F("[ZED-F9P] getModuleInfo() failed"));
    }

    myGNSS.setUART1Input(COM_TYPE_UBX | COM_TYPE_RTCM3);
    myGNSS.setUART1Output(COM_TYPE_UBX);
    myGNSS.setNavigationFrequency(5);
    myGNSS.setAutoPVT(true);

    Serial.println(F("[ZED-F9P] Rover configured — awaiting RTCM corrections via 3DR"));
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
    Serial.begin(115200);
#ifdef RGB_LED_PIN
    ledBootShow();
#endif
    trackCapacity = (4UL * 1024 * 1024) / sizeof(TrackPoint);
    trackBuf = (TrackPoint*)ps_malloc(trackCapacity * sizeof(TrackPoint));
    if (trackBuf)
        Serial.printf("[Track] PSRAM buffer: %u samples (~%.0f h @ 5Hz)\n",
                      trackCapacity, trackCapacity / 5.0f / 3600.0f);
    else
        Serial.println(F("[Track] PSRAM alloc FAILED — recording disabled"));
    delay(2000);
    Serial.println(F("\n=== RTK Rover Station (3DR Tele) — HB9IIU ==="));

    pinMode(REF_BUTTON_PIN, INPUT_PULLUP);
#ifdef BLIP_LED_PIN
    pinMode(BLIP_LED_PIN, OUTPUT);
    digitalWrite(BLIP_LED_PIN, LOW);
#endif

    // 3DR radio UART2
    Tele_Serial.begin(TELE_BAUD, SERIAL_8N1, TELE_RX_PIN, TELE_TX_PIN);
    Serial.printf("[3DR] UART2 ready  GPIO%d RX  GPIO%d TX  %d baud\n",
                  TELE_RX_PIN, TELE_TX_PIN, TELE_BAUD);

    wifiSetup();

    GPS_Serial.setRxBufferSize(2048);
    GPS_Serial.begin(38400, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    delay(1500);

    Serial.print(F("[ZED-F9P] Connecting via UART (GPIO4 RX, GPIO5 TX) ... "));
    if (!myGNSS.begin(GPS_Serial)) {
        Serial.println(F("FAILED — will retry in loop()"));
        return;
    }
    Serial.println(F("OK"));
    gnssReady = true;
    configureGNSS();
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
    serviceReferenceButton();
    if (!gnssReady) {
        static unsigned long lastRetry = 0;
        if (millis() - lastRetry > 3000) {
            lastRetry = millis();
            Serial.print(F("[ZED-F9P] Retrying... "));
            if (myGNSS.begin(GPS_Serial)) {
                Serial.println(F("OK"));
                gnssReady = true;
                configureGNSS();
            } else {
                Serial.println(F("still not responding"));
            }
        }
        return;
    }

    if (wifiReady) { webServer.handleClient(); ArduinoOTA.handle(); }
#ifdef RGB_LED_PIN
    ledUpdate();
#endif
#ifdef BLIP_LED_PIN
    if (ledBlipUntil && millis() >= ledBlipUntil) { digitalWrite(BLIP_LED_PIN, LOW); ledBlipUntil = 0; }
#endif

    // ---- Forward incoming 3DR bytes to ZED-F9P ----
    while (Tele_Serial.available()) {
        uint8_t b = (uint8_t)Tele_Serial.read();
        GPS_Serial.write(b);
        rtcmBytesReceived++;
        trackRtcmByte(b);
    }

    // ---- Let SparkFun lib process incoming UBX bytes from ZED-F9P ----
    myGNSS.checkUblox();
    myGNSS.checkCallbacks();

    // ---- Poll position data (every 1 s) ----
    static unsigned long lastPoll = 0;
    if (millis() - lastPoll > 1000) {
        lastPoll = millis();
        gnssFix  = myGNSS.getFixType();
        gnssCarr = myGNSS.getCarrierSolutionType();
        gnssLat  = myGNSS.getLatitude();
        gnssLon  = myGNSS.getLongitude();
        gnssAlt  = myGNSS.getAltitude();
        gnssSV   = myGNSS.getSIV();
        gnssHAcc = myGNSS.getHorizontalAccuracy() / 10000.0f;
        gnssVAcc = myGNSS.getVerticalAccuracy()   / 10000.0f;

        if (gnssCarr == 2 && gnssFix >= 3) {
            posHistory[posHistIdx] = { gnssLat, gnssLon };
            posHistIdx = (posHistIdx + 1) % POS_HIST_SIZE;
            if (posHistLen < POS_HIST_SIZE) posHistLen++;
        }
        appendReferenceSample();

        // ---- Track recording at 5 Hz (separate timer) ----
        static uint32_t lastTrackMs = 0;
        if (trackBuf && trackRecording && millis() - lastTrackMs >= 200) {
            lastTrackMs = millis();
            TrackPoint tp;
            tp.ms     = lastTrackMs;
            tp.lat    = gnssLat;
            tp.latHp  = myGNSS.getHighResLatitudeHp();
            tp.lon    = gnssLon;
            tp.lonHp  = myGNSS.getHighResLongitudeHp();
            tp.altMm  = gnssAlt;
            tp.hAccMm = (uint16_t)min((uint32_t)65535, (uint32_t)(gnssHAcc * 1000));
            tp.spdMms = (uint16_t)min((uint32_t)65535, (uint32_t)myGNSS.getGroundSpeed());
            tp.hdg10  = (uint16_t)((myGNSS.getHeading() / 10000UL) % 3600);
            tp.fix    = gnssFix;
            tp.carr   = gnssCarr;
            if (trackCount == 0) trackStartMs = tp.ms;
            trackBuf[trackHead] = tp;
            trackHead = (trackHead + 1) % trackCapacity;
            if (trackCount < trackCapacity) trackCount++;
        }

        uint32_t ageSec = lastTeleMs ? (millis() - lastTeleMs) / 1000 : 9999;
        Serial.printf("[GNSS] %-9s  lat=%.7f  lon=%.7f  alt=%.1fm  H=%.1fmm  V=%.1fmm  SV=%u  3DR=%uB age=%us\n",
                      fixLabel(),
                      gnssLat / 1e7, gnssLon / 1e7, gnssAlt / 1000.0f,
                      gnssHAcc * 1000.0f, gnssVAcc * 1000.0f,
                      gnssSV, rtcmBytesReceived, ageSec);
    }

    delay(5);
}
