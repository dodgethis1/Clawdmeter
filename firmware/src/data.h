#pragma once
#include <Arduino.h>

#define MODEL_BUCKETS_MAX 4

struct ModelBucket {
    char  name[12];          // model display name, truncated
    float pct;               // weekly-scoped utilization (0-100)
    int   reset_mins;        // minutes until this bucket resets
};

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    int local_mins;          // daemon local time, minutes since midnight (-1 = unknown)
    ModelBucket models[MODEL_BUCKETS_MAX];
    int model_count;
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};
