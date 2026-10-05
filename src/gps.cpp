#include "gps.h"

#include <Arduino.h>
#include <TinyGPSPlus.h>
#include <math.h>
#include <time.h>

#include "settings.h"

static constexpr int PIN_RX = 15, PIN_TX = 13;
static const uint32_t BAUDS[] = {115200, 9600}; // Cap ships at 115200; plain ATGM336H modules default to 9600

GpsState gpsState;

static HardwareSerial gpsSerial(1);
static TinyGPSPlus parser;
static int baudIdx = 0;
static uint32_t baudTriedAt = 0, lastSentenceAt = 0, lastChecksums = 0;
static bool running = false;

static void open()
{
    gpsSerial.begin(BAUDS[baudIdx], SERIAL_8N1, PIN_RX, PIN_TX);
    gpsState.baud = BAUDS[baudIdx];
    baudTriedAt = millis();
    running = true;
}

void gpsBegin()
{
    memset(&gpsState, 0, sizeof(gpsState));
    if (settings.gpsEnabled)
        open();
}

void gpsSetEnabled(bool on)
{
    if (on && !running)
        open();
    else if (!on && running) {
        gpsSerial.end();
        running = false;
        memset(&gpsState, 0, sizeof(gpsState));
    }
}

void gpsPoll()
{
    if (!running)
        return;
    while (gpsSerial.available())
        parser.encode(gpsSerial.read());

    uint32_t now = millis();
    if (parser.passedChecksum() != lastChecksums) {
        lastChecksums = parser.passedChecksum();
        lastSentenceAt = now;
    }
    gpsState.receiving = lastSentenceAt && now - lastSentenceAt < 5000;

    // Nothing decodable yet: try the other baud rate.
    if (!lastSentenceAt && now - baudTriedAt > 4000) {
        baudIdx = (baudIdx + 1) % (sizeof(BAUDS) / sizeof(BAUDS[0]));
        gpsSerial.end();
        open();
    }

    gpsState.sats = parser.satellites.isValid() ? parser.satellites.value() : 0;
    bool fresh = parser.location.isValid() && parser.location.age() < 10000;
    gpsState.hasFix = gpsState.receiving && fresh;
    if (gpsState.hasFix) {
        gpsState.latitudeI = (int32_t)lround(parser.location.lat() * 1e7);
        gpsState.longitudeI = (int32_t)lround(parser.location.lng() * 1e7);
        gpsState.altitudeM = parser.altitude.isValid() ? (int32_t)parser.altitude.meters() : 0;
    }
    if (parser.date.isValid() && parser.time.isValid() && parser.date.year() >= 2024) {
        struct tm t = {};
        t.tm_year = parser.date.year() - 1900;
        t.tm_mon = parser.date.month() - 1;
        t.tm_mday = parser.date.day();
        t.tm_hour = parser.time.hour();
        t.tm_min = parser.time.minute();
        t.tm_sec = parser.time.second();
        // mktime uses local time; the ESP32 default TZ is UTC, which is what we want.
        gpsState.unixTime = (uint32_t)mktime(&t) + parser.time.age() / 1000;
    }
}

float gpsDistanceM(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2)
{
    const double R = 6371000.0, k = M_PI / 180.0 / 1e7;
    double p1 = lat1 * k, p2 = lat2 * k, dp = (lat2 - lat1) * k, dl = ((double)lon2 - lon1) * k;
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return (float)(2 * R * atan2(sqrt(a), sqrt(1 - a)));
}

void gpsFormatDistance(float m, char *out, int n)
{
    if (m < 1000)
        snprintf(out, n, "%d m", (int)m);
    else if (m < 100000)
        snprintf(out, n, "%.1f km", m / 1000);
    else
        snprintf(out, n, "%d km", (int)(m / 1000));
}
