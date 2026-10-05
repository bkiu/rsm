// LoRa regions and modem presets, mirroring the official firmware's tables (src/mesh/RadioInterface.cpp,
// src/mesh/MeshRadio.h) so we land on exactly the same frequency slot as stock Meshtastic nodes.
#pragma once
#include <stdint.h>

struct Region {
    const char *name;
    float freqStart; // MHz
    float freqEnd;   // MHz
    int8_t powerLimit;
};

struct ModemParams {
    float bwKHz;
    uint8_t sf;
    uint8_t cr;
};

struct RadioParams {
    float freqMHz;
    float bwKHz;
    uint8_t sf;
    uint8_t cr;
    int8_t power;
    uint16_t slot; // 1-based
};

const Region *findRegion(const char *name);
const Region *regionAt(int i); // nullptr past the end
int regionCount();

bool presetFromName(const char *name, uint8_t *preset);
const char *presetName(uint8_t preset); // e.g. "LongFast"; also the default channel name
ModemParams presetParams(uint8_t preset);

// Frequency/power the official firmware would pick for the current settings. False if region unset.
bool computeRadioParams(RadioParams *out);

uint32_t djb2(const char *s);
