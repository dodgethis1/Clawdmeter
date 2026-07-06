#pragma once
#include <stdint.h>

// Ring buffer of weekly-usage samples for the Weekly screen.
// One sample per hour; 168 samples = 7 days. Persisted to NVS so the
// curve survives reboots (no RTC on board, so a long power gap simply
// compresses into adjacent samples — acceptable for a trend view).
#define WEEKLY_CAP 168

void    weekly_history_init(void);                // load from NVS
void    weekly_history_sample(float weekly_pct);  // time-gated internally
int     weekly_history_count(void);
uint8_t weekly_history_at(int i);                 // 0 = oldest
uint8_t weekly_history_peak(void);
uint8_t weekly_history_avg(void);
