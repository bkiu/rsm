// Persistent device settings (stored in NVS). Edited from the serial CLI or the on-screen region picker.
#pragma once
#include <stddef.h>
#include <stdint.h>

static constexpr int MAX_CHANNELS = 8; // index 0 is the primary channel; 1..7 are secondary (private) channels

struct ExtraChannel {
    char name[12];
    uint8_t psk[32];
    uint8_t pskLen;
    bool used;
    bool sharePosition;
};

struct Settings {
    uint32_t magic;
    char longName[25];
    char shortName[5];
    char region[12];      // region code name, e.g. "US" or "EU_868"; "" = not set yet (radio stays off)
    uint8_t preset;       // meshtastic_Config_LoRaConfig_ModemPreset
    uint16_t slot;        // frequency slot (1-based); 0 = derive from channel name like the official firmware
    char channelName[12]; // "" = the preset name, i.e. the default public channel
    uint8_t psk[32];
    uint8_t pskLen;
    int8_t txPower;    // dBm; clamped to the region limit and the SX1262's 22 dBm
    uint8_t hopLimit;  // 1..7
    bool relay;        // rebroadcast other nodes' packets like a normal CLIENT node
    uint8_t fontSize;  // 0 = medium, 1 = large, 2 = extra large
    uint8_t privateKey[32];
    uint8_t publicKey[32];
    // --- added in v2; older saved blobs end here and get defaults for the fields below ---
    uint32_t ext;              // keeps the new fields 4-byte aligned after the old tail padding
    bool gpsEnabled;
    bool sharePosition;        // broadcast our GPS position on the primary channel (off by default, like stock)
    uint8_t positionPrecision; // bits of lat/lon precision shared: 32 = exact, 16 ~ 350 m, 13 ~ 3 km
    uint16_t positionIntervalMin;
    // --- added in v3 ---
    uint32_t ext2;
    ExtraChannel channels[MAX_CHANNELS - 1]; // secondary channels, index 1..7
};

// Canned messages sent with Fn+<key> from the node list (set up over serial with 'quick'). Stored separately from
// Settings, under their own NVS key.
struct QuickMessage {
    char key;        // 'a'-'z' or '0'-'9'; 0 = unused slot
    uint32_t thread; // node number or channelThread(n)
    char text[101];
};
static constexpr int MAX_QUICK = 12;

extern Settings settings;
extern QuickMessage quickMessages[MAX_QUICK];
extern uint32_t myNodeNum;

void settingsLoad();
void settingsSave();
void settingsFactoryReset(); // wipes settings and generates a fresh key pair
void quickSave();
QuickMessage *quickFind(char key);
