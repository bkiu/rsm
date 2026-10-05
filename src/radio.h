// SX1262 on the Cap LoRa-1262, driven by RadioLib. Single-threaded: call poll() from loop().
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "regions.h"

static constexpr size_t MAX_FRAME = 255;

struct RxFrame {
    uint8_t data[MAX_FRAME];
    size_t len;
    float rssi;
    float snr;
};

class Radio
{
  public:
    bool begin(); // enables the Cap's RF front end and applies the current settings
    bool reconfigure();
    bool ready() const { return ok; }
    const RadioParams &params() const { return rp; }
    const char *status() const { return statusText; }

    // Queue a frame for transmission no earlier than delayMs from now. (from, id) identifies it for cancel().
    bool enqueue(const uint8_t *buf, size_t len, uint32_t delayMs, uint32_t from, uint32_t id);
    void cancel(uint32_t from, uint32_t id);
    bool isQueued(uint32_t from, uint32_t id) const;

    // Services the radio. Returns true and fills `out` when a frame was received.
    bool poll(RxFrame &out);

    uint32_t airtimeMs(size_t len) const;
    uint32_t slotTimeMs() const;

    uint32_t rxCount = 0, txCount = 0, rxBad = 0;

  private:
    struct TxItem {
        bool used;
        uint32_t due, from, id;
        uint8_t len;
        uint8_t data[MAX_FRAME];
    };
    static constexpr int TXQ = 16; // room for a NodeInfo broadcast on all 8 channels plus traffic
    TxItem txq[TXQ] = {};

    bool ok = false;
    bool transmitting = false;
    uint32_t txStartedAt = 0;
    uint32_t rxActivitySince = 0;
    RadioParams rp = {};
    char statusText[48] = "radio off";

    bool configure();
    void startRx();
    bool activelyReceiving();
};

extern Radio radio;
