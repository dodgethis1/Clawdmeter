#include <Arduino.h>
#include "usage_history.h"

#define SAMPLE_INTERVAL_MS (5UL * 60UL * 1000UL)
#define RESET_DROP_PTS     20   // a drop this large between samples = reset

static uint8_t  buf[HISTORY_CAP];
static int      count = 0;
static int      head = 0;        // next write slot
static uint32_t last_sample_ms = 0;
static bool     have_any = false;

void usage_history_sample(float session_pct) {
    uint32_t now = millis();
    if (have_any && now - last_sample_ms < SAMPLE_INTERVAL_MS) return;
    have_any = true;
    last_sample_ms = now;

    float v = session_pct;
    if (v < 0.0f) v = 0.0f;
    if (v > 100.0f) v = 100.0f;
    buf[head] = (uint8_t)(v + 0.5f);
    head = (head + 1) % HISTORY_CAP;
    if (count < HISTORY_CAP) count++;
}

int usage_history_count(void) { return count; }

uint8_t usage_history_at(int i) {
    if (count == 0) return 0;
    if (i < 0) i = 0;
    if (i >= count) i = count - 1;
    int start = (head - count + 2 * HISTORY_CAP) % HISTORY_CAP;
    return buf[(start + i) % HISTORY_CAP];
}

uint8_t usage_history_peak(void) {
    uint8_t peak = 0;
    for (int i = 0; i < count; i++) {
        uint8_t v = usage_history_at(i);
        if (v > peak) peak = v;
    }
    return peak;
}

uint8_t usage_history_avg(void) {
    if (count == 0) return 0;
    uint32_t sum = 0;
    for (int i = 0; i < count; i++) sum += usage_history_at(i);
    return (uint8_t)(sum / count);
}

uint8_t usage_history_resets(void) {
    uint8_t n = 0;
    for (int i = 1; i < count; i++) {
        int prev = usage_history_at(i - 1);
        int cur  = usage_history_at(i);
        if (prev - cur >= RESET_DROP_PTS) n++;
    }
    return n;
}
