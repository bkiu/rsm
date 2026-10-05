#include "radio.h"

#include <M5Unified.h>
#include <RadioLib.h>
#include <SPI.h>

#include "settings.h"

// Cardputer ADV + Cap LoRa-1262 wiring (shares the SPI bus with the SD card slot)
static constexpr int PIN_SCK = 40, PIN_MISO = 39, PIN_MOSI = 14;
static constexpr int PIN_CS = 5, PIN_RST = 3, PIN_DIO1 = 4, PIN_BUSY = 6;
static constexpr uint8_t SYNC_WORD = 0x2b;
static constexpr uint16_t PREAMBLE_LEN = 16;

// PI4IOE5V6408 on the Cap: pin 0 enables the RF front end and must stay high.
static constexpr uint8_t IOEX_ADDR = 0x43;
static constexpr uint8_t IOEX_DIR = 0x03, IOEX_OUT = 0x05, IOEX_HIGHZ = 0x07;

Radio radio;

static SPIClass loraSpi(FSPI);
static SX1262 lora(new Module(PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY, loraSpi));
static volatile bool irqPending = false;

static void IRAM_ATTR onDio1()
{
    irqPending = true;
}

static void enableFrontEnd()
{
    auto &i2c = M5.In_I2C;
    const uint32_t f = 400000;
    uint8_t v = i2c.readRegister8(IOEX_ADDR, IOEX_DIR, f);
    i2c.writeRegister8(IOEX_ADDR, IOEX_DIR, v | 0x01, f);
    v = i2c.readRegister8(IOEX_ADDR, IOEX_HIGHZ, f);
    i2c.writeRegister8(IOEX_ADDR, IOEX_HIGHZ, v & ~0x01, f);
    v = i2c.readRegister8(IOEX_ADDR, IOEX_OUT, f);
    i2c.writeRegister8(IOEX_ADDR, IOEX_OUT, v | 0x01, f);
}

bool Radio::begin()
{
    enableFrontEnd();
    loraSpi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    return configure();
}

bool Radio::reconfigure()
{
    transmitting = false;
    return configure();
}

bool Radio::configure()
{
    ok = false;
    if (!computeRadioParams(&rp)) {
        snprintf(statusText, sizeof(statusText), "Region not set");
        lora.standby();
        return false;
    }

    // The Cap has a 1.8 V TCXO on DIO3; fall back to the crystal if that fails, as the firmware does.
    int16_t st = lora.begin(rp.freqMHz, rp.bwKHz, rp.sf, rp.cr, SYNC_WORD, rp.power, PREAMBLE_LEN, 1.8f, false);
    if (st != RADIOLIB_ERR_NONE)
        st = lora.begin(rp.freqMHz, rp.bwKHz, rp.sf, rp.cr, SYNC_WORD, rp.power, PREAMBLE_LEN, 0.0f, false);
    if (st != RADIOLIB_ERR_NONE) {
        snprintf(statusText, sizeof(statusText), "LoRa init failed (%d)", st);
        return false;
    }
    lora.setCurrentLimit(140);
    lora.setDio2AsRfSwitch(true);
    lora.setRxBoostedGainMode(true);
    lora.setCRC(2);
    lora.setDio1Action(onDio1);

    ok = true;
    snprintf(statusText, sizeof(statusText), "%s %.3f MHz", settings.region, rp.freqMHz);
    startRx();
    return true;
}

void Radio::startRx()
{
    irqPending = false;
    rxActivitySince = 0;
    lora.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF, RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED),
                      RADIOLIB_IRQ_RX_DEFAULT_MASK, 0);
}

bool Radio::activelyReceiving()
{
    uint32_t irq = lora.getIrqFlags();
    if (!(irq & (RADIOLIB_SX126X_IRQ_HEADER_VALID | RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED))) {
        rxActivitySince = 0;
        return false;
    }
    uint32_t now = millis();
    if (!rxActivitySince)
        rxActivitySince = now ? now : 1;
    // A preamble detection from noise stays latched; give up on it after the longest possible frame.
    if (now - rxActivitySince > airtimeMs(MAX_FRAME) + 100) {
        lora.clearIrqFlags(RADIOLIB_SX126X_IRQ_HEADER_VALID | RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED);
        rxActivitySince = 0;
        return false;
    }
    return true;
}

bool Radio::enqueue(const uint8_t *buf, size_t len, uint32_t delayMs, uint32_t from, uint32_t id)
{
    if (!ok || len > MAX_FRAME)
        return false;
    for (auto &t : txq) {
        if (!t.used) {
            t.used = true;
            t.due = millis() + delayMs;
            t.from = from;
            t.id = id;
            t.len = len;
            memcpy(t.data, buf, len);
            return true;
        }
    }
    return false;
}

void Radio::cancel(uint32_t from, uint32_t id)
{
    for (auto &t : txq)
        if (t.used && t.from == from && t.id == id)
            t.used = false;
}

bool Radio::isQueued(uint32_t from, uint32_t id) const
{
    for (auto &t : txq)
        if (t.used && t.from == from && t.id == id)
            return true;
    return false;
}

uint32_t Radio::airtimeMs(size_t len) const
{
    if (!ok)
        return 1000;
    // Semtech AN1200.13 time-on-air, explicit header, CRC on.
    float tSym = (float)(1u << rp.sf) / rp.bwKHz; // ms
    bool lowDr = tSym > 16.0f;
    float num = 8.0f * len - 4.0f * rp.sf + 28 + 16;
    float den = 4.0f * (rp.sf - (lowDr ? 2 : 0));
    float nPayload = 8 + fmaxf(ceilf(num / den) * rp.cr, 0);
    return (uint32_t)((PREAMBLE_LEN + 4.25f + nPayload) * tSym) + 1;
}

uint32_t Radio::slotTimeMs() const
{
    if (!ok)
        return 30;
    float tSym = (float)(1u << rp.sf) / rp.bwKHz;
    return (uint32_t)(2.5f * tSym + 7.6f);
}

bool Radio::poll(RxFrame &out)
{
    if (!ok)
        return false;
    uint32_t now = millis();
    bool got = false;

    if (irqPending) {
        irqPending = false;
        uint32_t irq = lora.getIrqFlags();
        if (transmitting) {
            if (irq & (RADIOLIB_SX126X_IRQ_TX_DONE | RADIOLIB_SX126X_IRQ_TIMEOUT)) {
                lora.finishTransmit();
                transmitting = false;
                txCount++;
                startRx();
            }
        } else if (irq & RADIOLIB_SX126X_IRQ_RX_DONE) {
            size_t len = lora.getPacketLength();
            int16_t st = RADIOLIB_ERR_UNKNOWN;
            if (len > 0 && len <= MAX_FRAME)
                st = lora.readData(out.data, len);
            if (st == RADIOLIB_ERR_NONE) {
                out.len = len;
                out.rssi = lora.getRSSI();
                out.snr = lora.getSNR();
                rxCount++;
                got = true;
            } else {
                rxBad++;
            }
            startRx();
        } else if (irq & (RADIOLIB_SX126X_IRQ_CRC_ERR | RADIOLIB_SX126X_IRQ_HEADER_ERR)) {
            rxBad++;
            startRx();
        }
    }

    // Watchdog for a lost TX_DONE interrupt.
    if (transmitting && now - txStartedAt > 2 * airtimeMs(MAX_FRAME) + 2000) {
        transmitting = false;
        startRx();
    }

    if (!transmitting && !got) {
        TxItem *next = nullptr;
        for (auto &t : txq)
            if (t.used && (int32_t)(now - t.due) >= 0 && (!next || (int32_t)(t.due - next->due) < 0))
                next = &t;
        if (next) {
            if (activelyReceiving()) {
                next->due = now + slotTimeMs() * (1 + random(0, 8)); // back off; someone is talking
            } else {
                lora.standby();
                irqPending = false;
                if (lora.startTransmit(next->data, next->len) == RADIOLIB_ERR_NONE) {
                    transmitting = true;
                    txStartedAt = now;
                } else {
                    startRx();
                }
                next->used = false;
            }
        }
    }
    return got;
}
