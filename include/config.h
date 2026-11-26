#pragma once

// ======================= BUS STOP =========================
#define BUS_STOP_CODE       "59521"
#define BUS_STOP_NAME       "Yishun Emerald"
#define WALK_MINUTES        5        // door to bus stop
// Only show these services, comma separated. Leave "" to show every bus at the stop.
#define MY_BUSES            ""

// ======================= WEATHER ==========================
// Must match an NEA 2-hour forecast area name exactly (Canberra is in "Sembawang").
#define WEATHER_AREA        "Sembawang"

// ======================= TIMING ===========================
#define BUS_REFRESH_MS        30000UL   // while awake (LTA asks for light use)
#define BUS_REFRESH_IDLE_MS   120000UL  // while asleep, so the data is fresh when you walk up
#define WEATHER_REFRESH_MS    600000UL  // NEA updates every 30 min; check every 10 min
#define ENV_REFRESH_MS        10000UL   // indoor temperature / humidity

// ======================= WAKE (ToF) =======================
#define WAKE_DISTANCE_MM    1200     // only wake for things closer than this...
#define WAKE_DELTA_MM       250      // ...and at least this much closer than the learned background
#define AWAKE_SECONDS       60       // stay on this long after the last detection
#define ALWAYS_ON           false    // true = never sleep (handy while testing)

// ======================= AUTO DIM (Light unit) ============
// Light unit on Port B, analogRead 0-4095. HIGHER = DARKER on this unit
// (measured: room lights on ~2150, covered / dark 4095).
#define LIGHT_NIGHT_ABOVE   3500     // darker than this = night: dim screen and LEDs
#define LIGHT_DAY_BELOW     3000     // brighter than this = day (gap stops flicker)
#define SCREEN_DAY          180      // backlight 0-255
#define SCREEN_NIGHT        25

// ======================= OUTPUTS ==========================
#define SPEAKER_VOLUME      140    // 0-255
#define LED_BRIGHTNESS      40     // side LED bars, 0-255 (day)
#define LED_BRIGHTNESS_NIGHT 6
