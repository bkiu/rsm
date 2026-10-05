// Battery voltage history. The Cardputer ADV has no charge-status signal, only the battery voltage on GPIO10,
// so the only way to tell whether it is charging is to watch the voltage over time.
#pragma once
#include <stdint.h>

void batteryPoll();
int batteryMv();                 // latest reading, 0 if unknown
int batteryTrendMv(int minutes); // change over the last N minutes (0 if not enough history)
int batteryHistoryMinutes();     // how much history we have
