// GPS on the Cap LoRa-1262 (ATGM336H, NMEA over UART: RX GPIO15, TX GPIO13).
#pragma once
#include <stdint.h>

struct GpsState {
    bool receiving;   // NMEA sentences are arriving
    bool hasFix;
    int32_t latitudeI, longitudeI; // 1e-7 degrees
    int32_t altitudeM;
    uint32_t sats;
    uint32_t unixTime; // 0 when unknown
    uint32_t baud;
};

extern GpsState gpsState;

void gpsBegin();
void gpsPoll();
void gpsSetEnabled(bool on);

// Great-circle distance in metres between two 1e-7-degree coordinates.
float gpsDistanceM(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2);
// "850 m" / "12.3 km"
void gpsFormatDistance(float m, char *out, int n);
