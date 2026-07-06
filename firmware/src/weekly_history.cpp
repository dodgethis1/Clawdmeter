#include <Arduino.h>
#include <Preferences.h>
#include "weekly_history.h"

#define SAMPLE_INTERVAL_MS (60UL * 60UL * 1000UL)   // hourly
#define NVS_NAMESPACE "clawd"
#define NVS_KEY_BUF   "wkbuf"
#define NVS_KEY_META  "wkmeta"

static uint8_t  buf[WEEKLY_CAP];
static int      count = 0;
static int      head = 0;        // next write slot
static uint32_t last_sample_ms = 0;
static bool     have_any = false;

struct Meta { int32_t count; int32_t head; };

void weekly_history_init(void) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) return;   // read-only; absent = fresh
    Meta meta = {0, 0};
    if (prefs.getBytes(NVS_KEY_META, &meta, sizeof(meta)) == sizeof(meta) &&
        meta.count > 0 && meta.count <= WEEKLY_CAP &&
        meta.head >= 0 && meta.head < WEEKLY_CAP) {
        if (prefs.getBytes(NVS_KEY_BUF, buf, WEEKLY_CAP) == WEEKLY_CAP) {
            count = meta.count;
            head  = meta.head;
            Serial.printf("weekly: restored %d samples from NVS\n", count);
        }
    }
    prefs.end();
}

static void persist(void) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false)) return;
    Meta meta = { (int32_t)count, (int32_t)head };
    prefs.putBytes(NVS_KEY_BUF, buf, WEEKLY_CAP);
    prefs.putBytes(NVS_KEY_META, &meta, sizeof(meta));
    prefs.end();
}

void weekly_history_sample(float weekly_pct) {
    uint32_t now = millis();
    if (have_any && now - last_sample_ms < SAMPLE_INTERVAL_MS) return;
    have_any = true;
    last_sample_ms = now;

    float v = weekly_pct;
    if (v < 0.0f) v = 0.0f;
    if (v > 100.0f) v = 100.0f;
    buf[head] = (uint8_t)(v + 0.5f);
    head = (head + 1) % WEEKLY_CAP;
    if (count < WEEKLY_CAP) count++;
    persist();   // hourly write — trivial NVS wear
}

int weekly_history_count(void) { return count; }

uint8_t weekly_history_at(int i) {
    if (count == 0) return 0;
    if (i < 0) i = 0;
    if (i >= count) i = count - 1;
    int start = (head - count + 2 * WEEKLY_CAP) % WEEKLY_CAP;
    return buf[(start + i) % WEEKLY_CAP];
}

uint8_t weekly_history_peak(void) {
    uint8_t peak = 0;
    for (int i = 0; i < count; i++) {
        uint8_t v = weekly_history_at(i);
        if (v > peak) peak = v;
    }
    return peak;
}

uint8_t weekly_history_avg(void) {
    if (count == 0) return 0;
    uint32_t sum = 0;
    for (int i = 0; i < count; i++) sum += weekly_history_at(i);
    return (uint8_t)(sum / count);
}
