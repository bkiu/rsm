#include "battery.h"

#include <M5Unified.h>

static constexpr uint32_t SAMPLE_MS = 30000;
static constexpr int SAMPLES = 40; // 20 minutes
static int16_t hist[SAMPLES];
static int count = 0, head = 0;
static uint32_t nextAt = 0;
static int latest = 0;

static int readMv()
{
    // Average a few readings; single ADC samples are noisy.
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int v = M5.Power.getBatteryVoltage();
        if (v > 0) {
            sum += v;
            n++;
        }
    }
    return n ? sum / n : 0;
}

void batteryPoll()
{
    uint32_t now = millis();
    if ((int32_t)(now - nextAt) < 0)
        return;
    nextAt = now + SAMPLE_MS;
    latest = readMv();
    if (!latest)
        return;
    hist[head] = latest;
    head = (head + 1) % SAMPLES;
    if (count < SAMPLES)
        count++;
}

int batteryMv()
{
    return latest;
}

int batteryHistoryMinutes()
{
    return count > 1 ? (count - 1) * SAMPLE_MS / 60000 : 0;
}

int batteryTrendMv(int minutes)
{
    int back = minutes * 60000 / SAMPLE_MS;
    if (back >= count || back <= 0)
        return 0;
    int newest = hist[(head - 1 + SAMPLES) % SAMPLES];
    int older = hist[(head - 1 - back + 2 * SAMPLES) % SAMPLES];
    return newest - older;
}
