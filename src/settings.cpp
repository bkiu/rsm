#include "settings.h"

#include <Arduino.h>
#include <stddef.h>
#include <Preferences.h>
#include <esp_mac.h>

#include "crypto.h"
#include "meshtastic/config.pb.h"

static constexpr uint32_t SETTINGS_MAGIC = 0x4d534831; // "MSH1"

Settings settings;
QuickMessage quickMessages[MAX_QUICK];
uint32_t myNodeNum = 0;

static void computeNodeNum()
{
    // Same derivation as the official firmware: the low four bytes of the factory MAC.
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    myNodeNum = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
}

static void applyNewFieldDefaults()
{
    settings.ext = 0;
    settings.gpsEnabled = true;
    settings.sharePosition = false;
    settings.positionPrecision = 32;
    settings.positionIntervalMin = 15;
}

static void applyDefaults()
{
    memset(&settings, 0, sizeof(settings));
    settings.magic = SETTINGS_MAGIC;
    snprintf(settings.shortName, sizeof(settings.shortName), "%04x", (unsigned)(myNodeNum & 0xffff));
    snprintf(settings.longName, sizeof(settings.longName), "Cardputer %04x", (unsigned)(myNodeNum & 0xffff));
    settings.preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    settings.psk[0] = 1; // default public key (AQ==)
    settings.pskLen = 1;
    settings.txPower = 22;
    settings.hopLimit = 3;
    settings.relay = true;
    settings.fontSize = 1;
    cryptoGenerateKeyPair(settings.privateKey, settings.publicKey);
    applyNewFieldDefaults();
}

void settingsLoad()
{
    computeNodeNum();
    Preferences prefs;
    prefs.begin("mesh", true);
    size_t n = prefs.getBytes("settings", &settings, sizeof(settings));
    if (prefs.getBytesLength("quick") != sizeof(quickMessages) ||
        prefs.getBytes("quick", quickMessages, sizeof(quickMessages)) != sizeof(quickMessages))
        memset(quickMessages, 0, sizeof(quickMessages));
    prefs.end();
    if (n < offsetof(Settings, ext) || n > sizeof(settings) || settings.magic != SETTINGS_MAGIC) {
        applyDefaults();
        settingsSave();
    } else if (n < sizeof(settings)) {
        // Saved by an older build: keep everything (especially the key pair) and default the new fields.
        memset((uint8_t *)&settings + n, 0, sizeof(settings) - n); // v3: no secondary channels
        if (n <= offsetof(Settings, ext))
            applyNewFieldDefaults(); // v2 GPS fields
        settingsSave();
    }
}

void settingsSave()
{
    Preferences prefs;
    prefs.begin("mesh", false);
    prefs.putBytes("settings", &settings, sizeof(settings));
    prefs.end();
}

void settingsFactoryReset()
{
    applyDefaults();
    settingsSave();
    memset(quickMessages, 0, sizeof(quickMessages));
    quickSave();
}

void quickSave()
{
    Preferences prefs;
    prefs.begin("mesh", false);
    prefs.putBytes("quick", quickMessages, sizeof(quickMessages));
    prefs.end();
}

QuickMessage *quickFind(char key)
{
    for (auto &q : quickMessages)
        if (q.key && q.key == key)
            return &q;
    return nullptr;
}

// Saved blobs from older builds are read by size, so these offsets must never move.
static_assert(offsetof(Settings, ext) == 164, "v1 settings layout changed");
static_assert(offsetof(Settings, ext2) == 176, "v2 settings layout changed");
