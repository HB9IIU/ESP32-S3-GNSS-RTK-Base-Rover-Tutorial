// =============================================================================
// LC29H(EA) RTK Rover — Generic ESP32 DevKit + Quectel LC29H(EA)
// =============================================================================
// Receives RTCM3 correction bytes from the base station over ESP-NOW
// (broadcast) instead of a 3DR radio. Every byte is forwarded directly to
// the LC29H UART so the module can compute an RTK Fixed/Float position.
//
// NMEA parsing here is manual (GGA + RMC + GSA only, no checksum
// validation) rather than a library, to stay consistent with this
// project's no-extra-dependencies convention. This is a UART link, not a
// noisy radio, so the risk of a corrupted line slipping through is low;
// worst case is one stale-looking dashboard update.
//
// This board has no PSRAM, so track recording uses a modest heap-allocated
// buffer (~1 hour at 1 Hz) rather than the F9P rover's multi-hour PSRAM
// buffer. There's no physical waypoint button wired on this board, so
// waypoint marking is a button in the web dashboard instead of a GPIO pin.
//
// Fix quality (GGA field 6): 0=none 1=GPS 2=DGPS 4=RTK FIXED 5=RTK FLOAT
//
// Wiring (LC29H(EA) breakout <-> generic ESP32 DevKit):
//   LC29H TX  -> GPIO16 (GPS_RX_PIN)
//   LC29H RX  <- GPIO17 (GPS_TX_PIN)
//   5V        -> 5V (module has its own onboard regulator per board silkscreen)
//   GND       -> GND
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <string.h>
#include <stdlib.h>
#include "myconfig.h"   // WIFI_SSID, WIFI_PASS, LC29H_OTA_PASSWORD

#ifndef GPS_RX_PIN
#define GPS_RX_PIN   16   // LC29H TX -> here
#endif
#ifndef GPS_TX_PIN
#define GPS_TX_PIN   17   // LC29H RX <- here
#endif

// The LC29H(EA) is a special case within the LC29H family: it has a single
// UART (not two like AA/BA/CA/DA), and its factory-default baud rate is
// 460800 -- not the 115200 you'd expect from the other variants. Confirmed
// against Quectel's LC29H Series GNSS Module Specification (EA column).
#define GPS_BAUD     460800

#define WIFI_HOSTNAME   "rtk-lc29h-rover"
#define WIFI_TIMEOUT_S  20
#define WIFI_RETRY_S    30
#define OTA_PASSWORD    LC29H_OTA_PASSWORD

#define RTCM_PREAMBLE   0xD3
#define RTCM_MAX_FRAME  1029

// Plain heap (no PSRAM on this board): 3600 samples @ 1 Hz = 1 hour.
#define TRACK_CAPACITY_TARGET 3600
#define MAX_WAYPOINTS 50

HardwareSerial GPS_Serial(1);
WebServer webServer(80);

static bool wifiAttempting = false;
static bool networkReady = false;
static uint32_t wifiAttemptStartedMs = 0;
static uint32_t lastWifiAttemptMs = 0;

// ---------------------------------------------------------------------------
// Parsed GNSS state -- updated from GGA/RMC/GSA sentences
// ---------------------------------------------------------------------------
static uint8_t  gnssFix = 0;
static uint8_t  gnssSV = 0;
static double   gnssLat = 0.0;
static double   gnssLon = 0.0;
static double   gnssAltM = 0.0;
static float    gnssHdop = 0.0f;
static float    gnssPdop = 0.0f;
static float    gnssVdop = 0.0f;
static float    gnssSpeedKn = 0.0f;
static float    gnssHeadingDeg = 0.0f;
static uint32_t lastGgaMs = 0;

static const char *fixLabel() {
    if (gnssFix == 4) return "RTK FIXED";
    if (gnssFix == 5) return "RTK FLOAT";
    if (gnssFix == 2) return "DGPS";
    if (gnssFix == 1) return "GPS";
    return "NO FIX";
}

// ---------------------------------------------------------------------------
// ESP-NOW / RTCM link stats -- a lightweight frame tracker runs alongside
// the raw byte forwarding purely for stats/logging (same pattern as the
// F9P rover's trackRtcmByte). It never affects forwarding itself.
// ---------------------------------------------------------------------------
static const uint16_t RTCM_TYPES[] = {1005, 1074, 1084, 1094, 1124, 1230};
static uint32_t rtcmTypeCnt[] = {0, 0, 0, 0, 0, 0};
static uint32_t rtcmPktsReceived = 0;
static uint32_t rtcmBytesReceived = 0;
static uint32_t lastRtcmMs = 0;

static void trackRtcmByte(uint8_t b) {
    static uint8_t hdr[5];
    static uint16_t idx = 0;
    static int32_t expected = -1;

    if (expected == -1) {
        if (b != RTCM_PREAMBLE) return;
        idx = 0;
        expected = 0;
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
        lastRtcmMs = millis();
        for (uint8_t i = 0; i < 6; i++)
            if (RTCM_TYPES[i] == msgType) { rtcmTypeCnt[i]++; break; }
        idx = 0; expected = -1;
    }
}

// NOTE: this project pins espressif32@6.11.0, which bundles arduino-esp32
// 2.0.17 (ESP-IDF 4.4). That release's esp_now_recv_cb_t takes the sender's
// MAC address directly -- not the esp_now_recv_info_t struct used from
// arduino-esp32 3.x (IDF 5) onward. If you ever bump the platform version,
// this signature will need to change to match.
void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
    GPS_Serial.write(data, len);
    rtcmBytesReceived += len;
    for (int i = 0; i < len; i++) trackRtcmByte(data[i]);
}

// ---------------------------------------------------------------------------
// Civil calendar <-> Unix epoch conversion (Howard Hinnant's days_from_civil /
// civil_from_days algorithm) -- self-contained so ZDA parsing doesn't have to
// rely on <time.h>'s timegm()/mktime() timezone behaviour, which varies
// across libc builds. Valid for any Gregorian date; we only ever feed it
// GNSS-plausible years.
// ---------------------------------------------------------------------------
static int32_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

struct CivilDate { int y, m, d, hh, mi, ss; };

static CivilDate civilFromEpoch(uint32_t epoch) {
    int32_t days = (int32_t)(epoch / 86400UL);
    uint32_t secOfDay = epoch % 86400UL;
    int32_t z = days + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t y = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    CivilDate c;
    c.y = (int)y; c.m = (int)m; c.d = (int)d;
    c.hh = secOfDay / 3600; c.mi = (secOfDay % 3600) / 60; c.ss = secOfDay % 60;
    return c;
}

// ---------------------------------------------------------------------------
// Real UTC time. The LC29H(EA) doesn't support enabling ZDA output via
// $PAIR062 (its <Type> parameter only accepts 0-5 on this variant -- ZDA is
// type 6), and it doesn't emit ZDA by default either, so we get the date/time
// from RMC instead: RMC always carries both a UTC time field and a DDMMYY
// date field (2-digit year, safely assumed 20xx), so no proprietary command
// is needed to unlock it. ZDA is still parsed too, in case a firmware
// update or a different LC29H variant ever starts sending it. We don't keep
// an RTC; instead we track a millis()-to-epoch offset, refreshed every time
// either sentence arrives, so any historical millis() value (e.g. a track
// point's ms field) can still be converted to a real timestamp afterward.
// ---------------------------------------------------------------------------
static bool   utcValid = false;
static double utcOffsetSec = 0.0;   // epochSeconds = ms/1000 + utcOffsetSec
static uint32_t lastUtcMs = 0;

static void updateUtcFromDateTime(int year, int month, int day, int hh, int mi, int ss) {
    if (day < 1 || day > 31 || month < 1 || month > 12 || year < 2020) return;
    int32_t days = daysFromCivil(year, month, day);
    double epoch = (double)days * 86400.0 + hh * 3600.0 + mi * 60.0 + ss;
    utcOffsetSec = epoch - (double)millis() / 1000.0;
    utcValid = true;
    lastUtcMs = millis();
}

static uint32_t utcEpochNow() {
    if (!utcValid) return 0;
    return (uint32_t)lround((double)millis() / 1000.0 + utcOffsetSec);
}

static uint32_t utcEpochForMs(uint32_t ms) {
    return (uint32_t)lround((double)ms / 1000.0 + utcOffsetSec);
}

static void formatIso8601(uint32_t epoch, char *out, size_t outLen) {
    CivilDate c = civilFromEpoch(epoch);
    snprintf(out, outLen, "%04d-%02d-%02dT%02d:%02d:%02dZ", c.y, c.m, c.d, c.hh, c.mi, c.ss);
}

// ---------------------------------------------------------------------------
// Satellites in view -- from GSV, merged across all constellations into one
// self-cleaning table (entries simply age out of the /api/satellites JSON
// after 5 s of no update, rather than trying to track exactly which GSV
// group cleared which entries).
// ---------------------------------------------------------------------------
#define MAX_SATS_VIEW 64
struct SatInfo {
    char     sys;    // 'P'=GPS 'L'=GLONASS 'A'=Galileo 'B'=BeiDou 'Q'=QZSS 'I'=NavIC
    uint8_t  prn;
    uint8_t  elev;   // degrees, 0-90
    uint16_t az;     // degrees, 0-359
    uint8_t  snr;    // dB-Hz, 0 = not tracking
    uint32_t lastSeenMs;
};
static SatInfo satView[MAX_SATS_VIEW];
static uint8_t satViewCount = 0;

// ---------------------------------------------------------------------------
// Jamming detection -- enabled once at boot via $PAIR391,1 (see setup()); the
// module then streams $PAIRSPF (L1) and optionally $PAIRSPF5 (L5) status
// sentences on its own, once per second, so no repeated polling is needed.
// 0=unknown 1=good (no jamming) 2=warning 3=critical.
// ---------------------------------------------------------------------------
static uint8_t  jamStatusL1 = 0;
static uint8_t  jamStatusL5 = 0;
static bool     jamL5Seen = false;
static uint32_t lastJamMs = 0;

// Sends a one-off proprietary command to the module, computing the standard
// NMEA XOR checksum. Used once at boot for $PAIR391 -- not part of the
// regular RTCM injection path, so there's no interleaving risk.
static void sendNmeaCommand(const char *payload) {
    uint8_t cksum = 0;
    for (const char *p = payload; *p; p++) cksum ^= (uint8_t)*p;
    char buf[64];
    snprintf(buf, sizeof(buf), "$%s*%02X\r\n", payload, cksum);
    GPS_Serial.print(buf);
}

// ---------------------------------------------------------------------------
// Position scatter history + local reference displacement (ENU) -- mirrors
// the F9P rover's posHist/reference feature so the same dashboard JS can be
// reused. LC29H coordinates are already double degrees (no 1e7 integer
// scaling coming off the module), but posHist is still emitted as
// [lat*1e7, lon*1e7] integer pairs to match the F9P rover's JSON shape.
// ---------------------------------------------------------------------------
#define POS_HIST_SIZE 120
struct PosPoint { int32_t lat; int32_t lon; };
static PosPoint posHistory[POS_HIST_SIZE];
static uint8_t  posHistIdx = 0;
static uint8_t  posHistLen = 0;

#define REF_HIST_SIZE 120
struct RefPoint { int32_t east; int32_t north; int32_t up; };
static bool     referenceSet = false;
static double   referenceLat = 0.0;
static double   referenceLon = 0.0;
static double   referenceAlt = 0.0;
static RefPoint referenceHistory[REF_HIST_SIZE];
static uint8_t  referenceHistIdx = 0;
static uint8_t  referenceHistLen = 0;
static uint32_t referenceSession = 0;
static uint32_t referenceErrorUntil = 0;

// RTK Fixed + corrections received within the last 2 s -- same bar the F9P
// rover uses before trusting a position enough to anchor a reference off it.
static bool referenceQualityOK() {
    return gnssFix == 4 && lastRtcmMs && millis() - lastRtcmMs <= 2000;
}

// Offsets in millimetres, East/North/Up, from the reference position.
static void referenceOffsets(int32_t &east, int32_t &north, int32_t &up) {
    double cosLat = cos(referenceLat * PI / 180.0);
    east  = (int32_t)lround((gnssLon - referenceLon) * 111320.0 * cosLat * 1000.0);
    north = (int32_t)lround((gnssLat - referenceLat) * 111320.0 * 1000.0);
    up    = (int32_t)lround((gnssAltM - referenceAlt) * 1000.0);
}

static bool setReferencePosition() {
    if (!referenceQualityOK()) {
        referenceErrorUntil = millis() + 4000;
        Serial.println(F("[REF] Refused: need RTK FIXED with fresh corrections"));
        return false;
    }
    referenceLat = gnssLat;
    referenceLon = gnssLon;
    referenceAlt = gnssAltM;
    referenceSet = true;
    referenceSession++;
    referenceHistIdx = 0;
    referenceHistLen = 1;
    referenceHistory[0] = {0, 0, 0};
    Serial.printf("[REF] Set %.8f, %.8f, %.3fm\n", referenceLat, referenceLon, referenceAlt);
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

// Runs once per second (piggybacks on recordTrackSample's 1 Hz gate),
// independent of whether track recording is paused -- posHist and the
// reference trail keep going even when the track buffer isn't.
static void sampleGnssHistory() {
    if (gnssFix == 4) {
        posHistory[posHistIdx] = { (int32_t)lround(gnssLat * 1e7), (int32_t)lround(gnssLon * 1e7) };
        posHistIdx = (posHistIdx + 1) % POS_HIST_SIZE;
        if (posHistLen < POS_HIST_SIZE) posHistLen++;
    }
    appendReferenceSample();
}

// ---------------------------------------------------------------------------
// Track recording -- plain heap (no PSRAM on this board), ~1 hour @ 1 Hz
// ---------------------------------------------------------------------------
struct TrackPoint {
    uint32_t ms;
    float lat;
    float lon;
    float altM;
    float spd;   // m/s, derived from RMC speed-over-ground (knots)
    uint8_t fix;
};
struct Waypoint { uint32_t ms; double lat; double lon; double altM; uint32_t trackIdx; };

static TrackPoint *trackBuf = nullptr;
static uint32_t trackCapacity = 0;
static uint32_t trackHead = 0;
static uint32_t trackCount = 0;
static bool trackRecording = true;
static uint32_t trackStartMs = 0;
static uint32_t lastTrackSampleMs = 0;
static Waypoint waypoints[MAX_WAYPOINTS];
static uint8_t waypointCount = 0;

static void beginTrackBuffer() {
    trackCapacity = TRACK_CAPACITY_TARGET;
    trackBuf = (TrackPoint *)malloc((size_t)trackCapacity * sizeof(TrackPoint));
    if (!trackBuf) {
        trackCapacity = 0;
        Serial.println(F("[Track] Heap allocation failed; recording disabled"));
        return;
    }
    Serial.printf("[Track] Heap buffer: %u samples (~%.1f h @ 1 Hz)\n",
                  trackCapacity, trackCapacity / 3600.0f);
}

static void recordTrackSample() {
    uint32_t now = millis();
    if (now - lastTrackSampleMs < 1000) return;
    lastTrackSampleMs = now;

    sampleGnssHistory();

    if (!trackBuf || !trackRecording) return;
    if (trackCount == 0) trackStartMs = now;

    TrackPoint &p = trackBuf[trackHead];
    p.ms = now;
    p.lat = (float)gnssLat;
    p.lon = (float)gnssLon;
    p.altM = (float)gnssAltM;
    p.spd = gnssSpeedKn * 0.514444f;   // knots -> m/s
    p.fix = gnssFix;

    trackHead = (trackHead + 1) % trackCapacity;
    if (trackCount < trackCapacity) trackCount++;
}

static bool addWaypoint() {
    if (waypointCount >= MAX_WAYPOINTS || gnssFix < 1) return false;
    Waypoint &w = waypoints[waypointCount++];
    w.ms = millis();
    w.lat = gnssLat;
    w.lon = gnssLon;
    w.altM = gnssAltM;
    w.trackIdx = (trackHead == 0 ? trackCapacity - 1 : trackHead - 1);
    Serial.printf("[Track] Waypoint %u  %.7f, %.7f\n", waypointCount, gnssLat, gnssLon);
    return true;
}

static float trackDistanceM() {
    if (trackCount < 2 || !trackBuf) return 0.0f;
    double dist = 0;
    uint32_t start = (trackCount == trackCapacity) ? trackHead : 0;
    double prevLat = 0, prevLon = 0;
    for (uint32_t i = 0; i < trackCount; i++) {
        const TrackPoint &p = trackBuf[(start + i) % trackCapacity];
        if (i > 0) {
            double dlat = (p.lat - prevLat) * 111320.0;
            double dlon = (p.lon - prevLon) * 111320.0 * cos(prevLat * PI / 180.0);
            dist += sqrt(dlat * dlat + dlon * dlon);
        }
        prevLat = p.lat; prevLon = p.lon;
    }
    return (float)dist;
}

// =============================================================================
// NMEA PARSING -- manual, GGA/RMC/GSA only
// =============================================================================
static double nmeaCoordToDeg(const char *field, char hemi) {
    if (!field || !field[0]) return 0.0;
    double raw = atof(field);
    int deg = (int)(raw / 100);
    double minutes = raw - deg * 100;
    double dec = deg + minutes / 60.0;
    if (hemi == 'S' || hemi == 'W') dec = -dec;
    return dec;
}

// Splits an NMEA sentence in place on commas (and the trailing '*' before
// the checksum); fields[i] point into `line`, which is mutated ('\0'
// inserted at each separator). Returns the number of fields found.
static int splitNmeaFields(char *line, char **fields, int maxFields) {
    int count = 0;
    char *p = line;
    fields[count++] = p;
    while (*p && count < maxFields) {
        if (*p == ',' || *p == '*') {
            *p = '\0';
            fields[count++] = p + 1;
        }
        p++;
    }
    return count;
}

static void parseGGA(char *line) {
    char *f[20];
    int n = splitNmeaFields(line, f, 20);
    if (n < 10) return;
    // 0:talker 1:time 2:lat 3:N/S 4:lon 5:E/W 6:fixQuality 7:numSV 8:HDOP 9:alt
    gnssLat = nmeaCoordToDeg(f[2], f[3][0]);
    gnssLon = nmeaCoordToDeg(f[4], f[5][0]);
    gnssFix = (uint8_t)atoi(f[6]);
    gnssSV = (uint8_t)atoi(f[7]);
    gnssHdop = atof(f[8]);
    gnssAltM = atof(f[9]);
    lastGgaMs = millis();
}

static void parseRMC(char *line) {
    char *f[13];
    int n = splitNmeaFields(line, f, 13);
    if (n < 9) return;
    // 1:time(hhmmss.ss) 7:speed(knots) 8:course(deg) 9:date(ddmmyy)
    gnssSpeedKn = atof(f[7]);
    gnssHeadingDeg = atof(f[8]);

    // RMC's own date+time fields are our real-UTC source (see the block
    // comment above updateUtcFromDateTime): the LC29H(EA) can't be told to
    // emit ZDA, but RMC carries everything ZDA would have given us anyway.
    if (n >= 10 && strlen(f[1]) >= 6 && strlen(f[9]) >= 6) {
        int hh = (f[1][0] - '0') * 10 + (f[1][1] - '0');
        int mi = (f[1][2] - '0') * 10 + (f[1][3] - '0');
        int ss = (f[1][4] - '0') * 10 + (f[1][5] - '0');
        int day   = (f[9][0] - '0') * 10 + (f[9][1] - '0');
        int month = (f[9][2] - '0') * 10 + (f[9][3] - '0');
        int year  = 2000 + (f[9][4] - '0') * 10 + (f[9][5] - '0');
        updateUtcFromDateTime(year, month, day, hh, mi, ss);
    }
}

static void parseGSA(char *line) {
    char *f[19];
    int n = splitNmeaFields(line, f, 19);
    if (n < 18) return;
    // 15:PDOP 16:HDOP 17:VDOP -- multiple GSA lines per epoch (one per
    // constellation) carry the same combined DOP values; last one wins.
    gnssPdop = atof(f[15]);
    gnssVdop = atof(f[17]);
}

// GSV: up to 4 satellites per sentence, several sentences per constellation
// per epoch (talker ID tells us which). Format (NMEA 4.10, our default):
//   $ttGSV,TotalNumSen,SenNum,TotalNumSat{,SatID,Elev,Az,CN0}*4[,SignalID]*CS
// We merge every satellite we see into satView[] keyed by (sys,prn); entries
// simply age out at read time (see handleApiSatellites) rather than being
// cleared per-sentence, which sidesteps having to track sentence-group state.
static void parseGSV(char *line) {
    char *f[26];
    int n = splitNmeaFields(line, f, 26);
    if (n < 4) return;
    char sys = f[0][2];   // $GPGSV->'P' $GLGSV->'L' $GAGSV->'A' $GBGSV->'B' $GQGSV->'Q' $GIGSV->'I'
    uint32_t now = millis();
    for (int k = 0; k < 4; k++) {
        int idBase = 4 + 4 * k;
        if (idBase + 3 >= n) break;
        if (f[idBase][0] == '\0') continue;
        uint8_t  prn  = (uint8_t)atoi(f[idBase]);
        uint8_t  elev = (uint8_t)atoi(f[idBase + 1]);
        uint16_t az   = (uint16_t)atoi(f[idBase + 2]);
        uint8_t  snr  = (uint8_t)atoi(f[idBase + 3]);

        int idx = -1;
        for (uint8_t i = 0; i < satViewCount; i++) {
            if (satView[i].sys == sys && satView[i].prn == prn) { idx = i; break; }
        }
        if (idx < 0) {
            if (satViewCount >= MAX_SATS_VIEW) continue;
            idx = satViewCount++;
            satView[idx].sys = sys;
            satView[idx].prn = prn;
        }
        satView[idx].elev = elev;
        satView[idx].az = az;
        satView[idx].snr = snr;
        satView[idx].lastSeenMs = now;
    }
}

// ZDA: full UTC date/time. Not emitted by the LC29H(EA) as far as we've
// seen, and can't be force-enabled on this variant (see the comment above
// updateUtcFromDateTime) -- RMC is the real source in practice. Kept here in
// case a firmware update or a different LC29H variant ever sends it.
static void parseZDA(char *line) {
    char *f[8];
    int n = splitNmeaFields(line, f, 8);
    if (n < 5) return;
    if (strlen(f[1]) < 6) return;
    int hh = (f[1][0] - '0') * 10 + (f[1][1] - '0');
    int mi = (f[1][2] - '0') * 10 + (f[1][3] - '0');
    int ss = (f[1][4] - '0') * 10 + (f[1][5] - '0');
    int day = atoi(f[2]);
    int month = atoi(f[3]);
    int year = atoi(f[4]);
    updateUtcFromDateTime(year, month, day, hh, mi, ss);
}

// $PAIRSPF (L1) / $PAIRSPF5 (L5) jamming status, auto-reported at 1 Hz once
// enabled via $PAIR391,1 in setup(). 0=unknown 1=good 2=warning 3=critical.
static void parsePairSpf(char *line, bool isL5) {
    char *f[4];
    int n = splitNmeaFields(line, f, 4);
    if (n < 2) return;
    uint8_t status = (uint8_t)atoi(f[1]);
    if (isL5) { jamStatusL5 = status; jamL5Seen = true; }
    else jamStatusL1 = status;
    lastJamMs = millis();
}

// Buffers one NMEA line at a time from GPS_Serial, dispatches GGA/RMC/GSA/
// GSV/ZDA/PAIRSPF to the parsers above, and separately prints at most one
// GGA line per second to the USB serial monitor so it stays readable on the
// bench.
static void serviceGnssUart() {
    static char lineBuf[110];
    static uint8_t lineLen = 0;
    static char latestGgaPrint[110];
    static bool ggaPrintPending = false;
    static uint32_t lastPrintMs = 0;

    while (GPS_Serial.available()) {
        char c = (char)GPS_Serial.read();
        if (c == '\r') continue;
        if (c == '\n' || lineLen >= sizeof(lineBuf) - 1) {
            lineBuf[lineLen] = '\0';
            if (lineLen > 5 && lineBuf[0] == '$') {
                char sentenceCopy[110];
                strncpy(sentenceCopy, lineBuf, sizeof(sentenceCopy) - 1);
                sentenceCopy[sizeof(sentenceCopy) - 1] = '\0';

                if (strstr(lineBuf, "GGA") != nullptr) {
                    strncpy(latestGgaPrint, lineBuf, sizeof(latestGgaPrint) - 1);
                    latestGgaPrint[sizeof(latestGgaPrint) - 1] = '\0';
                    ggaPrintPending = true;
                    parseGGA(sentenceCopy);
                } else if (strstr(lineBuf, "RMC") != nullptr) {
                    parseRMC(sentenceCopy);
                } else if (strstr(lineBuf, "GSA") != nullptr) {
                    parseGSA(sentenceCopy);
                } else if (strstr(lineBuf, "GSV") != nullptr) {
                    parseGSV(sentenceCopy);
                } else if (strstr(lineBuf, "ZDA") != nullptr) {
                    parseZDA(sentenceCopy);
                } else if (strstr(lineBuf, "PAIRSPF5") != nullptr) {
                    parsePairSpf(sentenceCopy, true);
                } else if (strstr(lineBuf, "PAIRSPF") != nullptr) {
                    parsePairSpf(sentenceCopy, false);
                }
            }
            lineLen = 0;
            continue;
        }
        lineBuf[lineLen++] = c;
    }

    if (ggaPrintPending && millis() - lastPrintMs >= 1000) {
        lastPrintMs = millis();
        ggaPrintPending = false;
        Serial.println(latestGgaPrint);
    }
}

// =============================================================================
// JSON STATUS API -- /api/status
// =============================================================================
static void appendJsonString(String &j, const String &value) {
    j += '"';
    for (size_t i = 0; i < value.length(); i++) {
        char c = value[i];
        if (c == '"' || c == '\\') j += '\\';
        j += c;
    }
    j += '"';
}

static void handleApiStatus() {
    String j;
    j.reserve(3600);
    j = F("{\"fix\":\""); j += fixLabel();
    j += F("\",\"fixType\":"); j += gnssFix;
    j += F(",\"lat\":"); j += String(gnssLat, 8);
    j += F(",\"lon\":"); j += String(gnssLon, 8);
    j += F(",\"alt\":"); j += String(gnssAltM, 2);
    j += F(",\"sv\":"); j += gnssSV;
    j += F(",\"hdop\":"); j += String(gnssHdop, 2);
    j += F(",\"pdop\":"); j += String(gnssPdop, 2);
    j += F(",\"vdop\":"); j += String(gnssVdop, 2);
    j += F(",\"speedKn\":"); j += String(gnssSpeedKn, 2);
    j += F(",\"headingDeg\":"); j += String(gnssHeadingDeg, 1);
    j += F(",\"fixAgeSec\":"); j += lastGgaMs ? (millis() - lastGgaMs) / 1000 : 9999;

    j += F(",\"rtcm\":{\"frames\":"); j += rtcmPktsReceived;
    j += F(",\"bytes\":"); j += rtcmBytesReceived;
    uint32_t rtcmAge = lastRtcmMs ? (millis() - lastRtcmMs) / 1000 : 9999;
    j += F(",\"ageSec\":"); j += rtcmAge;
    j += F(",\"types\":{");
    for (uint8_t i = 0; i < 6; i++) {
        if (i) j += ',';
        j += '"'; j += RTCM_TYPES[i]; j += F("\":"); j += rtcmTypeCnt[i];
    }
    j += F("}}");

    j += F(",\"espNow\":{\"ready\":true}");

    {
        uint32_t utcAge = utcValid ? (millis() - lastUtcMs) / 1000 : 9999;
        j += F(",\"utc\":{\"valid\":"); j += utcValid ? F("true") : F("false");
        j += F(",\"ageSec\":"); j += utcAge;
        if (utcValid) {
            uint32_t epoch = utcEpochNow();
            char iso[26];
            formatIso8601(epoch, iso, sizeof(iso));
            j += F(",\"epoch\":"); j += epoch;
            j += F(",\"iso\":"); appendJsonString(j, String(iso));
        }
        j += F("}");
    }

    {
        uint32_t jamAge = lastJamMs ? (millis() - lastJamMs) / 1000 : 9999;
        j += F(",\"jamming\":{\"l1\":"); j += jamStatusL1;
        j += F(",\"l5\":"); j += jamL5Seen ? String(jamStatusL5) : String(-1);
        j += F(",\"ageSec\":"); j += jamAge;
        j += F("}");
    }

    {
        uint32_t satAge = 5000;
        uint8_t visibleCount = 0;
        uint32_t now = millis();
        for (uint8_t i = 0; i < satViewCount; i++)
            if (now - satView[i].lastSeenMs < satAge) visibleCount++;
        j += F(",\"satsInView\":"); j += visibleCount;
    }

    j += F(",\"track\":{\"enabled\":"); j += trackBuf ? F("true") : F("false");
    j += F(",\"count\":"); j += trackCount;
    j += F(",\"capacity\":"); j += trackCapacity;
    j += F(",\"recording\":"); j += trackRecording ? F("true") : F("false");
    j += F(",\"waypointCount\":"); j += waypointCount;
    j += F("}");

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
        j += F(",\"lat\":"); j += String(referenceLat, 8);
        j += F(",\"lon\":"); j += String(referenceLon, 8);
        j += F(",\"alt\":"); j += String(referenceAlt, 3);
        j += F(",\"eastMm\":"); j += east;
        j += F(",\"northMm\":"); j += north;
        j += F(",\"upMm\":"); j += up;
        j += F(",\"distanceMm\":"); j += String(distance, 1);
        j += F(",\"bearingDeg\":"); j += String(bearing, 1);
    }
    j += F(",\"history\":[");
    {
        uint8_t refStart = (referenceHistLen == REF_HIST_SIZE) ? referenceHistIdx : 0;
        for (uint8_t i = 0; i < referenceHistLen; i++) {
            if (i) j += ',';
            uint8_t idx = (refStart + i) % REF_HIST_SIZE;
            j += '['; j += referenceHistory[idx].east;
            j += ','; j += referenceHistory[idx].north;
            j += ','; j += referenceHistory[idx].up; j += ']';
        }
    }
    j += F("]}");

    j += F(",\"system\":{\"heap\":"); j += ESP.getFreeHeap();
    j += F(",\"minHeap\":"); j += ESP.getMinFreeHeap();
    j += F(",\"uptime\":"); j += millis() / 1000;
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

// =============================================================================
// SATELLITE API -- /api/satellites
// =============================================================================
static void handleApiSatellites() {
    String j;
    j.reserve((size_t)satViewCount * 40 + 16);
    j = '[';
    bool first = true;
    uint32_t now = millis();
    for (uint8_t i = 0; i < satViewCount; i++) {
        SatInfo &s = satView[i];
        if (now - s.lastSeenMs > 5000) continue;   // stale, no longer in view
        if (!first) j += ','; first = false;
        j += F("{\"sys\":\""); j += s.sys;
        j += F("\",\"prn\":"); j += s.prn;
        j += F(",\"elev\":"); j += s.elev;
        j += F(",\"az\":"); j += s.az;
        j += F(",\"snr\":"); j += s.snr;
        j += '}';
    }
    j += ']';
    webServer.sendHeader(F("Cache-Control"), F("no-store"));
    webServer.sendHeader(F("Access-Control-Allow-Origin"), F("*"));
    webServer.send(200, F("application/json"), j);
}

// =============================================================================
// TRACK API HANDLERS
// =============================================================================
static void handleTrackInfo() {
    float dist = trackDistanceM();
    uint32_t durMs = (trackCount > 0) ? millis() - trackStartMs : 0;
    String j;
    j.reserve(200);
    j = F("{\"count\":");        j += trackCount;
    j += F(",\"capacity\":");    j += trackCapacity;
    j += F(",\"recording\":");   j += trackRecording ? F("true") : F("false");
    j += F(",\"durationMs\":");  j += durMs;
    j += F(",\"distanceM\":");   j += String(dist, 1);
    j += F(",\"waypointCount\":"); j += waypointCount;
    j += F(",\"bufferPct\":");   j += (trackCapacity ? (uint32_t)(trackCount * 100UL / trackCapacity) : 0);
    j += '}';
    webServer.send(200, F("application/json"), j);
}

// Streams the response in small chunks via sendContent() instead of building
// one giant String, matching the CSV/GPX export pattern below. At count=1000
// a monolithic String here would be ~70 KB -- on this no-PSRAM board (the
// 3600-sample track buffer alone permanently pins ~84 KB of heap) a single
// allocation that large can fail or fragment under pressure, and a silently
// failed String::concat() leaves the buffer truncated mid-record, which is
// exactly what produces a "valid number, then garbage" JSON parse error
// partway through a large /api/track/points response.
static void handleTrackPoints() {
    if (!trackBuf || trackCount == 0) { webServer.send(200, F("application/json"), F("[]")); return; }
    uint32_t offset     = webServer.hasArg("offset")     ? (uint32_t)webServer.arg("offset").toInt()     : 0;
    uint32_t count      = webServer.hasArg("count")      ? (uint32_t)webServer.arg("count").toInt()      : 500;
    uint32_t decimation = webServer.hasArg("decimation") ? (uint32_t)webServer.arg("decimation").toInt() : 1;
    if (count > 1000) count = 1000;
    if (decimation < 1) decimation = 1;
    uint32_t start = (trackCount == trackCapacity) ? trackHead : 0;

    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.send(200, F("application/json"), "");

    char line[128];
    String chunk;
    chunk.reserve(1800);
    chunk = '[';
    bool first = true;
    uint32_t written = 0;
    for (uint32_t i = offset; i < trackCount && written < count; i += decimation) {
        const TrackPoint &p = trackBuf[(start + i) % trackCapacity];
        snprintf(line, sizeof(line), "%s{\"ms\":%lu,\"lat\":%.7f,\"lon\":%.7f,\"alt\":%.2f,\"spd\":%.2f,\"fix\":%u}",
                 first ? "" : ",", (unsigned long)p.ms, p.lat, p.lon, p.altM, p.spd, p.fix);
        first = false;
        chunk += line;
        written++;
        if (chunk.length() >= 1500) { webServer.sendContent(chunk); chunk = ""; }
    }
    chunk += ']';
    webServer.sendContent(chunk);
    webServer.sendContent("");
}

static void handleTrackWaypoints() {
    String j = "[";
    for (uint8_t i = 0; i < waypointCount; i++) {
        const Waypoint &w = waypoints[i];
        if (i) j += ',';
        j += F("{\"ms\":"); j += w.ms;
        j += F(",\"lat\":"); j += String(w.lat, 7);
        j += F(",\"lon\":"); j += String(w.lon, 7);
        j += F(",\"alt\":"); j += String(w.altM, 2);
        j += F(",\"n\":"); j += i;
        j += '}';
    }
    j += ']';
    webServer.send(200, F("application/json"), j);
}

static void handleTrackExportCsv() {
    if (!trackBuf) { webServer.send(503, F("text/plain"), F("No buffer")); return; }
    webServer.sendHeader(F("Content-Disposition"), F("attachment; filename=\"lc29h-track.csv\""));
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.send(200, F("text/csv"), "");
    webServer.sendContent(utcValid ? F("ms,utc,lat,lon,alt_m,spd_mps,fix\n") : F("ms,lat,lon,alt_m,spd_mps,fix\n"));
    uint32_t start = (trackCount == trackCapacity) ? trackHead : 0;
    char line[128]; char iso[26]; String chunk; chunk.reserve(2048);
    for (uint32_t i = 0; i < trackCount; i++) {
        const TrackPoint &p = trackBuf[(start + i) % trackCapacity];
        if (utcValid) {
            formatIso8601(utcEpochForMs(p.ms), iso, sizeof(iso));
            snprintf(line, sizeof(line), "%lu,%s,%.7f,%.7f,%.2f,%.2f,%u\n",
                     (unsigned long)p.ms, iso, p.lat, p.lon, p.altM, p.spd, p.fix);
        } else {
            snprintf(line, sizeof(line), "%lu,%.7f,%.7f,%.2f,%.2f,%u\n",
                     (unsigned long)p.ms, p.lat, p.lon, p.altM, p.spd, p.fix);
        }
        chunk += line;
        if (chunk.length() >= 1800) { webServer.sendContent(chunk); chunk = ""; }
    }
    if (chunk.length()) webServer.sendContent(chunk);
    webServer.sendContent("");
}

static void handleTrackExportGpx() {
    if (!trackBuf) { webServer.send(503, F("text/plain"), F("No buffer")); return; }
    webServer.sendHeader(F("Content-Disposition"), F("attachment; filename=\"lc29h-track.gpx\""));
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.send(200, F("application/gpx+xml"), "");
    webServer.sendContent(F("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<gpx version=\"1.1\" creator=\"LC29H Rover\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
        "<trk><name>LC29H Track</name><trkseg>\n"));
    uint32_t start = (trackCount == trackCapacity) ? trackHead : 0;
    char line[160]; char iso[26]; String chunk; chunk.reserve(2048);
    for (uint32_t i = 0; i < trackCount; i++) {
        const TrackPoint &p = trackBuf[(start + i) % trackCapacity];
        if (utcValid) {
            formatIso8601(utcEpochForMs(p.ms), iso, sizeof(iso));
            snprintf(line, sizeof(line),
                "<trkpt lat=\"%.7f\" lon=\"%.7f\"><ele>%.2f</ele><time>%s</time></trkpt>\n",
                p.lat, p.lon, p.altM, iso);
        } else {
            snprintf(line, sizeof(line),
                "<trkpt lat=\"%.7f\" lon=\"%.7f\"><ele>%.2f</ele></trkpt>\n",
                p.lat, p.lon, p.altM);
        }
        chunk += line;
        if (chunk.length() >= 1800) { webServer.sendContent(chunk); chunk = ""; }
    }
    for (uint8_t i = 0; i < waypointCount; i++) {
        const Waypoint &w = waypoints[i];
        if (utcValid) {
            formatIso8601(utcEpochForMs(w.ms), iso, sizeof(iso));
            snprintf(line, sizeof(line),
                "<wpt lat=\"%.7f\" lon=\"%.7f\"><ele>%.2f</ele><name>WP%u</name><time>%s</time></wpt>\n",
                w.lat, w.lon, w.altM, i + 1, iso);
        } else {
            snprintf(line, sizeof(line),
                "<wpt lat=\"%.7f\" lon=\"%.7f\"><ele>%.2f</ele><name>WP%u</name></wpt>\n",
                w.lat, w.lon, w.altM, i + 1);
        }
        chunk += line;
    }
    if (chunk.length()) webServer.sendContent(chunk);
    webServer.sendContent(F("</trkseg></trk>\n</gpx>"));
    webServer.sendContent("");
}

// =============================================================================
// WEB PAGE -- inline fallback; served when no /lc29h.html in LittleFS
// =============================================================================
static void handleRoot() {
    if (LittleFS.exists(F("/lc29h.html"))) {
        File f = LittleFS.open(F("/lc29h.html"), "r");
        webServer.sendHeader(F("Cache-Control"), F("no-store"));
        webServer.streamFile(f, F("text/html"));
        f.close();
        return;
    }

    String p;
    p.reserve(4200);
    p = F("<!DOCTYPE html><html><head>"
          "<meta charset='utf-8'>"
          "<title>RTK Rover (LC29H)</title>"
          "<style>"
          "body{font-family:monospace;background:#111;color:#ccc;padding:20px;max-width:480px;margin:0 auto}"
          "h2{color:#4af;margin-bottom:4px}"
          "h3{color:#4af;margin:14px 0 4px;font-size:1em}"
          ".ok{color:#4d4}.warn{color:#fa0}.bad{color:#f44}"
          "td{padding:3px 0;width:50%}"
          "hr{border:0;border-top:1px solid #333;margin:10px 0}"
          ".fix{font-size:1.1em;font-weight:bold}"
          "button{font-family:monospace;background:#222;color:#ccc;border:1px solid #444;"
          "padding:6px 12px;margin:4px 4px 4px 0;cursor:pointer}"
          "button:hover{border-color:#4af}"
          "</style></head><body>"
          "<h2>&#x1F6F0; RTK Rover (LC29H(EA), ESP-NOW)</h2>"

          "<table>"
          "<tr><td>Fix</td>"
              "<td><span class='fix' id='fix'>-</span></td></tr>"
          "<tr><td>Latitude</td>  <td><span id='lat'>-</span></td></tr>"
          "<tr><td>Longitude</td> <td><span id='lon'>-</span></td></tr>"
          "<tr><td>Altitude</td>  <td><span id='alt'>-</span></td></tr>"
          "<tr><td>Satellites</td><td><span id='sv'>-</span></td></tr>"
          "<tr><td>HDOP / PDOP</td><td><span id='dop'>-</span></td></tr>"
          "<tr><td>Speed / heading</td><td><span id='spd'>-</span></td></tr>"
          "</table>"

          "<hr><h3>ESP-NOW Correction Link</h3>"
          "<table>"
          "<tr><td>Frames received</td><td><span id='frames'>-</span></td></tr>"
          "<tr><td>Bytes received</td> <td><span id='bytes'>-</span></td></tr>"
          "<tr><td>Correction age</td> <td><span id='age'>-</span></td></tr>"
          "</table>"

          "<hr><h3>RTCM Message Counts</h3>"
          "<table id='rtcm'>"
          "<tr><td colspan='2' style='color:#555'>loading...</td></tr>"
          "</table>"

          "<hr><h3>Track</h3>"
          "<table>"
          "<tr><td>Buffer</td><td><span id='trackBuf'>-</span></td></tr>"
          "<tr><td>Waypoints</td><td><span id='wpCount'>-</span></td></tr>"
          "</table>"
          "<button onclick=\"post('/api/track/waypoint')\">Mark waypoint</button>"
          "<button onclick=\"post('/api/track/pause')\">Pause/resume</button>"
          "<button onclick=\"post('/api/track/clear')\">Clear</button>"
          "<a href='/api/track/export/gpx'><button>Export GPX</button></a>"
          "<a href='/api/track/export/csv'><button>Export CSV</button></a>"

          "<hr>"
          "<p id='uptime' style='color:#444;font-size:0.85em'>connecting...</p>"

          "<script>"
          "const RN={1005:'ARP',1074:'GPS MSM4',1084:'GLO MSM4',"
                     "1094:'GAL MSM4',1124:'BDS MSM4',1230:'GLO Biases'};"
          "function post(url){fetch(url,{method:'POST'});}"
          "function upd(){"
            "fetch('/api/status').then(r=>r.json()).then(d=>{"
              "const fc=d.fix==='RTK FIXED'?'ok':d.fix==='RTK FLOAT'?'warn':'bad';"
              "const fe=document.getElementById('fix');"
              "fe.className='fix '+fc; fe.textContent=d.fix;"
              "document.getElementById('lat').textContent=d.lat.toFixed(7)+' N';"
              "document.getElementById('lon').textContent=d.lon.toFixed(7)+' E';"
              "document.getElementById('alt').textContent=d.alt.toFixed(2)+' m';"
              "document.getElementById('sv').textContent=d.sv;"
              "document.getElementById('dop').textContent=d.hdop.toFixed(2)+' / '+d.pdop.toFixed(2);"
              "document.getElementById('spd').textContent=(d.speedKn*1.852).toFixed(1)+' km/h @ '+d.headingDeg.toFixed(0)+'°';"
              "document.getElementById('frames').textContent=d.rtcm.frames.toLocaleString();"
              "document.getElementById('bytes').textContent=d.rtcm.bytes.toLocaleString();"
              "const a=d.rtcm.ageSec,ac=a<5?'ok':a<15?'warn':'bad';"
              "document.getElementById('age').innerHTML="
                "'<span class=\"'+ac+'\">'+(a<9999?a+' s':'never')+'</span>';"
              "let rt='';"
              "for(const[k,v]of Object.entries(d.rtcm.types))"
                "rt+='<tr><td>'+k+' <span style=\"color:#555\">'+(RN[k]||'')+'</span></td>"
                     "<td>'+v.toLocaleString()+'</td></tr>';"
              "document.getElementById('rtcm').innerHTML=rt||"
                "'<tr><td colspan=\"2\" style=\"color:#555\">none yet</td></tr>';"
              "document.getElementById('trackBuf').textContent="
                "d.track.count+' / '+d.track.capacity+(d.track.recording?' (recording)':' (paused)');"
              "document.getElementById('wpCount').textContent=d.track.waypointCount;"
              "const u=d.system.uptime;"
              "document.getElementById('uptime').textContent="
                "'uptime '+Math.floor(u/3600)+'h '+Math.floor(u%3600/60)+'m '+u%60+'s  ● live';"
            "}).catch(()=>{"
              "document.getElementById('uptime').textContent='connection lost...';"
            "})}"
          "upd();setInterval(upd,2000);"
          "</script></body></html>");

    webServer.send(200, F("text/html"), p);
}

static void startNetworkServices() {
    if (networkReady) return;
    MDNS.begin(WIFI_HOSTNAME);
    LittleFS.begin(false);

    webServer.on("/", HTTP_GET, handleRoot);
    webServer.serveStatic("/vendor/", LittleFS, "/vendor/");
    webServer.on("/api/status", HTTP_GET, handleApiStatus);
    webServer.on("/api/satellites", HTTP_GET, handleApiSatellites);
    webServer.on("/api/reference", HTTP_POST, []() {
        bool accepted = setReferencePosition();
        webServer.send(accepted ? 200 : 409, F("application/json"),
                       accepted ? F("{\"ok\":true}") : F("{\"ok\":false,\"error\":\"RTK FIXED with fresh corrections required\"}"));
    });
    webServer.on("/api/track/info", HTTP_GET, handleTrackInfo);
    webServer.on("/api/track/points", HTTP_GET, handleTrackPoints);
    webServer.on("/api/track/waypoints", HTTP_GET, handleTrackWaypoints);
    webServer.on("/api/track/export/csv", HTTP_GET, handleTrackExportCsv);
    webServer.on("/api/track/export/gpx", HTTP_GET, handleTrackExportGpx);
    webServer.on("/api/track/waypoint", HTTP_POST, []() {
        bool ok = addWaypoint();
        webServer.send(ok ? 200 : 409, F("application/json"), ok ? F("{\"ok\":true}") : F("{\"ok\":false}"));
    });
    webServer.on("/api/track/pause", HTTP_POST, []() {
        trackRecording = !trackRecording;
        webServer.send(200, F("application/json"), trackRecording ? F("{\"recording\":true}") : F("{\"recording\":false}"));
    });
    webServer.on("/api/track/clear", HTTP_POST, []() {
        trackHead = 0; trackCount = 0; waypointCount = 0; trackStartMs = 0;
        webServer.send(200, F("application/json"), F("{\"ok\":true}"));
    });
    webServer.begin();

    ArduinoOTA.setHostname(WIFI_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() { Serial.println(F("[OTA] Update started")); });
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
        wifiAttempting = false;
        startNetworkServices();
        if (networkReady) {
            webServer.handleClient();
            ArduinoOTA.handle();
        }
        return;
    }
    if (wifiAttempting && millis() - wifiAttemptStartedMs > WIFI_TIMEOUT_S * 1000UL) {
        wifiAttempting = false;
        Serial.println(F("[WiFi] Unavailable; ESP-NOW may not reach the base until reconnected"));
    }
    if (!wifiAttempting && millis() - lastWifiAttemptMs > WIFI_RETRY_S * 1000UL)
        beginWifiAttempt();
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println(F("\n=== LC29H(EA) RTK Rover (ESP-NOW) ==="));

    beginTrackBuffer();

    GPS_Serial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.printf("[GPS] UART ready  GPIO%d RX  GPIO%d TX  %d baud\n",
                  GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);

    // One-time command: enables L1/L5 jamming detection. The module then
    // streams $PAIRSPF/$PAIRSPF5 status sentences on its own at 1 Hz, so we
    // never need to poll for it -- this is the only thing this firmware ever
    // writes to the module besides the incoming RTCM correction bytes, and
    // it's sent before ESP-NOW is initialised so there's no chance of it
    // interleaving with a correction chunk.
    sendNmeaCommand("PAIR391,1");
    Serial.println(F("[GPS] Jamming detection enabled ($PAIR391,1)"));

    beginWifiAttempt();

    if (esp_now_init() != ESP_OK) {
        Serial.println(F("[ESP-NOW] Init failed"));
    } else {
        esp_now_register_recv_cb(onEspNowRecv);
        Serial.println(F("[ESP-NOW] Ready, listening for broadcast corrections"));
    }
}

void loop() {
    serviceGnssUart();
    serviceNetwork();
    recordTrackSample();
    delay(2);
}
