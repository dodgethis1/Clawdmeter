#pragma once
#include <stdint.h>

// Ring buffer of session-usage samples for the History screen.
// One sample every 5 minutes; 96 samples = 8 hours.
#define HISTORY_CAP 96

void    usage_history_sample(float session_pct);  // time-gated internally
int     usage_history_count(void);
uint8_t usage_history_at(int i);                  // 0 = oldest
uint8_t usage_history_peak(void);
uint8_t usage_history_avg(void);
uint8_t usage_history_resets(void);               // sharp drops = session resets
