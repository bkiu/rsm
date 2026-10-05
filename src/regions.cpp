#include "regions.h"

#include <math.h>
#include <string.h>
#include <strings.h>

#include "meshtastic/config.pb.h"
#include "settings.h"

#define PRESET(x) meshtastic_Config_LoRaConfig_ModemPreset_##x

// Only regions using the standard (no spacing/padding) band profile; ham and EU_866/EU_N_868 need
// presets we don't support.
static const Region REGIONS[] = {
    {"US", 902.0f, 928.0f, 30},       {"EU_868", 869.4f, 869.65f, 27}, {"EU_433", 433.0f, 434.0f, 10},
    {"ANZ", 915.0f, 928.0f, 30},      {"ANZ_433", 433.05f, 434.79f, 14}, {"CN", 470.0f, 510.0f, 19},
    {"JP", 920.5f, 923.5f, 13},       {"KR", 920.0f, 923.0f, 23},      {"TW", 920.0f, 925.0f, 27},
    {"RU", 868.7f, 869.2f, 20},       {"IN", 865.0f, 867.0f, 30},      {"NZ_865", 864.0f, 868.0f, 36},
    {"TH", 920.0f, 925.0f, 27},       {"UA_433", 433.0f, 434.7f, 10},  {"MY_433", 433.0f, 435.0f, 20},
    {"MY_919", 919.0f, 924.0f, 27},   {"SG_923", 917.0f, 925.0f, 20},  {"PH_433", 433.0f, 434.7f, 10},
    {"PH_868", 868.0f, 869.4f, 14},   {"PH_915", 915.0f, 918.0f, 24},  {"KZ_433", 433.075f, 434.775f, 10},
    {"KZ_863", 863.0f, 868.0f, 30},   {"NP_865", 865.0f, 868.0f, 30},  {"BR_902", 902.0f, 907.5f, 30},
};

static const struct {
    uint8_t preset;
    const char *name;
} PRESETS[] = {
    {PRESET(LONG_FAST), "LongFast"},     {PRESET(LONG_SLOW), "LongSlow"},       {PRESET(LONG_MODERATE), "LongMod"},
    {PRESET(LONG_TURBO), "LongTurbo"},   {PRESET(MEDIUM_SLOW), "MediumSlow"},   {PRESET(MEDIUM_FAST), "MediumFast"},
    {PRESET(MEDIUM_TURBO), "MediumTurbo"}, {PRESET(SHORT_SLOW), "ShortSlow"},   {PRESET(SHORT_FAST), "ShortFast"},
    {PRESET(SHORT_TURBO), "ShortTurbo"},
};

const Region *findRegion(const char *name)
{
    for (const Region &r : REGIONS)
        if (strcasecmp(r.name, name) == 0)
            return &r;
    return nullptr;
}

const Region *regionAt(int i)
{
    return (i >= 0 && i < regionCount()) ? &REGIONS[i] : nullptr;
}

int regionCount()
{
    return sizeof(REGIONS) / sizeof(REGIONS[0]);
}

bool presetFromName(const char *name, uint8_t *preset)
{
    for (auto &p : PRESETS)
        if (strcasecmp(p.name, name) == 0) {
            *preset = p.preset;
            return true;
        }
    return false;
}

const char *presetName(uint8_t preset)
{
    for (auto &p : PRESETS)
        if (p.preset == preset)
            return p.name;
    return "LongFast";
}

ModemParams presetParams(uint8_t preset)
{
    switch (preset) {
    case PRESET(SHORT_TURBO): return {500, 7, 5};
    case PRESET(SHORT_FAST): return {250, 7, 5};
    case PRESET(SHORT_SLOW): return {250, 8, 5};
    case PRESET(MEDIUM_FAST): return {250, 9, 5};
    case PRESET(MEDIUM_SLOW): return {250, 10, 5};
    case PRESET(MEDIUM_TURBO): return {500, 9, 5};
    case PRESET(LONG_TURBO): return {500, 11, 8};
    case PRESET(LONG_MODERATE): return {125, 11, 8};
    case PRESET(LONG_SLOW): return {125, 12, 8};
    default: return {250, 11, 5}; // LONG_FAST
    }
}

uint32_t djb2(const char *s)
{
    uint32_t h = 5381;
    int c;
    while ((c = (unsigned char)*s++) != 0)
        h = ((h << 5) + h) + c;
    return h;
}

bool computeRadioParams(RadioParams *out)
{
    const Region *r = findRegion(settings.region);
    if (!r)
        return false;
    ModemParams m = presetParams(settings.preset);
    float slotWidth = m.bwKHz / 1000.0f;
    uint32_t numSlots = (uint32_t)roundf((r->freqEnd - r->freqStart) / slotWidth);
    if (numSlots == 0)
        numSlots = 1;
    const char *chName = settings.channelName[0] ? settings.channelName : presetName(settings.preset);
    uint32_t slot0 = (settings.slot > 0 && settings.slot <= numSlots) ? settings.slot - 1 : djb2(chName) % numSlots;

    out->freqMHz = r->freqStart + m.bwKHz / 2000.0f + slot0 * slotWidth;
    out->bwKHz = m.bwKHz;
    out->sf = m.sf;
    out->cr = m.cr;
    int p = settings.txPower;
    if (p > r->powerLimit)
        p = r->powerLimit;
    if (p > 22)
        p = 22; // SX1262 maximum
    out->power = p;
    out->slot = slot0 + 1;
    return true;
}
