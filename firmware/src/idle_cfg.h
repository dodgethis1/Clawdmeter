#pragma once

// Auto-sleep / idle screen-off configuration.
// All tunables live here so nothing is hard-coded in main.cpp / idle.cpp.

#define IDLE_TIMEOUT_MS             (30UL * 60UL * 1000UL)  // 30 min
#define IDLE_FADE_OUT_MS            400      // fade-to-black duration
#define IDLE_FADE_IN_MS             180      // wake fade-in (snappier)
#define IDLE_FADE_STEP_MS           20       // tick interval per fade step

#define DISPLAY_DEFAULT_BRIGHTNESS  200      // active-screen brightness

// When false, the device never enters sleep while USB power is present (also
// wakes from sleep when USB is plugged back in). Useful when sitting on a
// desk plugged in — also covers battery-less hardware that's always on USB.
// Set true to sleep regardless of power source.
#define IDLE_SLEEP_WHEN_CHARGING    false

// When true, a touch on the dark panel wakes the device (first touch is
// consumed for wake only, second touch acts normally). When false, touch is
// fully ignored during sleep — useful if cats/sleeves brushing the panel
// overnight would be a problem.
#define IDLE_WAKE_ON_TOUCH          true

// ---- AMOLED burn-in prevention (always-on desk duty) ----
// Pixel shift: the whole UI orbits through 5 positions, +/- this many px.
// Invisible against the true-black background, keeps static elements
// (clock, titles, status line) from parking on one set of pixels.
#define BURNIN_SHIFT_PX             2
#define BURNIN_SHIFT_INTERVAL_MS    (3UL * 60UL * 1000UL)   // hop every 3 min

// Night dim: cap panel brightness during these local-time minutes
// (requires daemon clock; no dim until first data arrives). Wear scales
// steeply with drive current, so this is the biggest lever on panel life.
#define NIGHT_DIM_START_MIN         0            // 12:00am
#define NIGHT_DIM_END_MIN           (7 * 60)     // 7:00am
#define NIGHT_BRIGHTNESS_CAP        80
