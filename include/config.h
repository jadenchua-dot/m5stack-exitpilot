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
#define BUS_REFRESH_MS      30000UL     // LTA asks for light use: every 30 s is plenty
#define WEATHER_REFRESH_MS  600000UL    // NEA updates every 30 min; check every 10 min

// ======================= OUTPUTS ==========================
#define SPEAKER_VOLUME      140    // 0-255
#define LED_BRIGHTNESS      40     // side LED bars, 0-255
