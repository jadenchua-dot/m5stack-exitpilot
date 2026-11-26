// M5Stack Fire - ExitPilot: tells you when to leave the house.
//
// Shows live bus arrivals for your stop (LTA DataMall), works out when you need
// to walk out the door to catch the next bus, and warns you when NEA's 2-hour
// forecast says rain for your area (blue LED bars + beep).
// The ToF sensor wakes the screen when someone walks up, the Light unit dims it
// at night, and the ENV II unit shows indoor temperature and humidity.
// Tap your tag on the RFID unit on the way out / back in and you get a Telegram
// message ("Jad left home at 07:42, heading for bus 169").
//
// Two cores: all internet work (LTA, NEA, Telegram) runs in a background task on
// core 0, so the screen, sensors and RFID reader on core 1 never wait for it.
//
// Buttons:  A = refresh now   B = mute / unmute   C = show all buses / my buses
//           (any button also wakes the screen)
// Serial (115200) prints every fetch for debugging.

#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <VL53L0X.h>
#include <Preferences.h>
#include <time.h>
#include <climits>
#include "MFRC522_I2C.h"
#include "config.h"
#include "secrets.h"

// ---------------------------------------------------------------- hardware
static constexpr int LED_PIN = 15;     // M5Stack Fire side LED bars
static constexpr int LED_COUNT = 10;
Adafruit_NeoPixel leds(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
M5Canvas canvas(&M5.Display);
VL53L0X tof;                           // ToF unit, I2C 0x29 on Port A
static constexpr uint8_t SHT30_ADDR = 0x44;   // ENV II temperature / humidity
static constexpr int LIGHT_PIN = 36;   // Light unit, Port B analog
MFRC522 rfid(0x28);                    // RFID unit, I2C 0x28 on Port A
Preferences prefs;                     // survives reboots: Telegram chat id, home/out state

// ---------------------------------------------------------------- shared data
// Written by the network task, read by the screen. Guarded by dataLock.
SemaphoreHandle_t dataLock;

struct Bus {
  char no[8];
  time_t t1, t2;      // next two arrivals (0 = none)
  char load1[4];      // SEA / SDA / LSD
};
static constexpr int MAX_BUSES = 16;
Bus buses[MAX_BUSES];
int busCount = 0;
volatile bool busOk = false;

char weatherText[40] = "--";
volatile bool raining = false;

// requests from the UI to the network task
volatile bool forceRefresh = true;     // fetch buses + weather now
volatile bool awake = true;            // network task polls slower while asleep
volatile bool rainStarted = false;     // network task -> UI: beep
volatile bool tgFailed = false;        // network task -> UI: show a warning

struct TgMsg { char text[320]; };
QueueHandle_t tgQueue;
String chatId = TELEGRAM_CHAT_ID;      // found automatically if left empty

// ---------------------------------------------------------------- UI-only state
bool muted = false;
bool onlyMine = true;        // only matters if MY_BUSES is set

bool tofOk = false;
int distanceMm = -1;
float background = -1;       // learned empty-scene ToF distance
bool envOk = false;
float indoorC = NAN, indoorRH = NAN;
uint32_t lastEnvRead = 0;
int lightRaw = 0;
bool night = false;
uint32_t lastPresence = 0;

bool isOut = false;          // toggled by each RFID tap
char toast[64] = "";         // short message shown over the screen
uint32_t toastUntil = 0;

// ---------------------------------------------------------------- helpers
time_t parseIso(const char *s) {
  if (!s || !*s) return 0;
  int Y, M, D, h, m, sec;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  struct tm t = {};
  t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D;
  t.tm_hour = h; t.tm_min = m; t.tm_sec = sec;
  return mktime(&t);    // TZ is SGT, same as LTA's +08:00 timestamps
}

bool isMine(const char *no) {
  if (strlen(MY_BUSES) == 0 || !onlyMine) return true;
  char list[64], key[12];
  snprintf(list, sizeof(list), ",%s,", MY_BUSES);
  snprintf(key, sizeof(key), ",%s,", no);
  return strstr(list, key) != nullptr;
}

// whole minutes until t, or -1 if no bus
int minsUntil(time_t t, time_t now) {
  if (t == 0) return -1;
  long s = (long)(t - now);
  return s <= 0 ? 0 : (int)(s / 60);
}

bool timeReady() { return time(nullptr) > 1700000000; }

// soonest bus you can still walk to (bestMins = -1 if none). Call with dataLock held.
void findBest(time_t now, char *bestNo, size_t n, int &bestMins) {
  bestNo[0] = 0;
  bestMins = -1;
  for (int i = 0; i < busCount; i++) {
    if (!isMine(buses[i].no)) continue;
    time_t ts[2] = {buses[i].t1, buses[i].t2};
    for (time_t t : ts) {
      int m = minsUntil(t, now);
      if (m >= WALK_MINUTES && (bestMins < 0 || m < bestMins)) { bestMins = m; strlcpy(bestNo, buses[i].no, n); }
    }
  }
}

// ---------------------------------------------------------------- network (core 0)
void connectWifi() {
  canvas.fillScreen(TFT_BLACK);
  canvas.setFont(&fonts::Font4);
  canvas.setTextColor(TFT_WHITE);
  canvas.setTextDatum(middle_center);
  canvas.drawString("Connecting to Wi-Fi...", 160, 110);
  canvas.setFont(&fonts::Font2);
  canvas.drawString(WIFI_SSID, 160, 140);
  canvas.pushSprite(0, 0);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(250);
  Serial.printf("WiFi: %s  IP %s\n",
                WiFi.status() == WL_CONNECTED ? "connected" : "FAILED",
                WiFi.localIP().toString().c_str());
}

bool fetchBuses() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);   // no chunked encoding, so ArduinoJson can read the stream
  http.setTimeout(10000);
  String url = String("https://datamall2.mytransport.sg/ltaodataservice/v3/BusArrival?BusStopCode=") + BUS_STOP_CODE;
  if (!http.begin(client, url)) return false;
  http.addHeader("AccountKey", LTA_ACCOUNT_KEY);
  http.addHeader("accept", "application/json");
  int code = http.GET();
  Serial.printf("LTA HTTP %d\n", code);
  if (code != 200) { http.end(); return false; }

  JsonDocument filter;
  filter["Services"][0]["ServiceNo"] = true;
  filter["Services"][0]["NextBus"]["EstimatedArrival"] = true;
  filter["Services"][0]["NextBus"]["Load"] = true;
  filter["Services"][0]["NextBus2"]["EstimatedArrival"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("LTA JSON error: %s\n", err.c_str()); return false; }

  static Bus tmp[MAX_BUSES];
  int n = 0;
  for (JsonObject s : doc["Services"].as<JsonArray>()) {
    if (n >= MAX_BUSES) break;
    Bus &b = tmp[n++];
    strlcpy(b.no, s["ServiceNo"] | "?", sizeof(b.no));
    b.t1 = parseIso(s["NextBus"]["EstimatedArrival"] | "");
    b.t2 = parseIso(s["NextBus2"]["EstimatedArrival"] | "");
    strlcpy(b.load1, s["NextBus"]["Load"] | "", sizeof(b.load1));
    Serial.printf("  %-5s next %s | %s  load %s\n", b.no,
                  s["NextBus"]["EstimatedArrival"] | "-", s["NextBus2"]["EstimatedArrival"] | "-", b.load1);
  }
  // sort by next arrival, buses with no arrival last
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++) {
      time_t a = tmp[i].t1 ? tmp[i].t1 : LONG_MAX;
      time_t c = tmp[j].t1 ? tmp[j].t1 : LONG_MAX;
      if (c < a) { Bus t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t; }
    }
  xSemaphoreTake(dataLock, portMAX_DELAY);
  memcpy(buses, tmp, sizeof(Bus) * n);
  busCount = n;
  xSemaphoreGive(dataLock);
  return true;
}

bool fetchWeather() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(10000);
  if (!http.begin(client, "https://api-open.data.gov.sg/v2/real-time/api/two-hr-forecast")) return false;
  int code = http.GET();
  Serial.printf("NEA HTTP %d\n", code);
  if (code != 200) { http.end(); return false; }

  JsonDocument filter;
  filter["data"]["items"][0]["forecasts"][0]["area"] = true;
  filter["data"]["items"][0]["forecasts"][0]["forecast"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("NEA JSON error: %s\n", err.c_str()); return false; }

  for (JsonObject f : doc["data"]["items"][0]["forecasts"].as<JsonArray>()) {
    if (strcmp(f["area"] | "", WEATHER_AREA) == 0) {
      const char *text = f["forecast"] | "--";
      String w = text;
      bool rain = w.indexOf("Rain") >= 0 || w.indexOf("Shower") >= 0 || w.indexOf("Thunder") >= 0;
      xSemaphoreTake(dataLock, portMAX_DELAY);
      strlcpy(weatherText, text, sizeof(weatherText));
      if (rain && !raining) rainStarted = true;
      raining = rain;
      xSemaphoreGive(dataLock);
      Serial.printf("Weather %s: %s  rain=%d\n", WEATHER_AREA, text, rain);
      return true;
    }
  }
  Serial.printf("Area '%s' not found in NEA forecast\n", WEATHER_AREA);
  return false;
}

bool tgEnabled() { return strlen(TELEGRAM_BOT_TOKEN) > 0; }

// First run: read the newest message sent to the bot and remember who sent it.
void tgFindChat() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(10000);
  String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN + "/getUpdates?offset=-1&limit=1";
  if (!http.begin(client, url)) return;
  int code = http.GET();
  if (code != 200) { Serial.printf("Telegram getUpdates HTTP %d\n", code); http.end(); return; }
  JsonDocument filter;
  filter["result"][0]["message"]["chat"]["id"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) return;
  JsonVariant id = doc["result"][0]["message"]["chat"]["id"];
  if (id.isNull()) { Serial.println("Telegram: no messages yet - send your bot a message"); return; }
  chatId = String(id.as<long long>());
  prefs.putString("chat", chatId);
  Serial.printf("Telegram chat found: %s\n", chatId.c_str());
}

bool tgSend(const char *text) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN + "/sendMessage";
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  JsonDocument body;
  body["chat_id"] = chatId;
  body["text"] = text;
  String json;
  serializeJson(body, json);
  int code = http.POST(json);
  http.end();
  Serial.printf("Telegram send HTTP %d\n", code);
  return code == 200;
}

void netTask(void *) {
  uint32_t lastBus = 0, lastWeather = 0, lastChatTry = 0, lastWifiTry = 0;
  bool first = true;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      if (millis() - lastWifiTry > 15000) {      // dropped or never connected: retry
        Serial.println("WiFi not connected, retrying...");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        lastWifiTry = millis();
      }
      busOk = false;
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // Telegram messages from RFID taps go first: they're what you're waiting on
    TgMsg m;
    while (xQueueReceive(tgQueue, &m, 0) == pdTRUE) {
      if (tgEnabled() && chatId.length() && !tgSend(m.text)) tgFailed = true;
    }

    bool force = forceRefresh;
    forceRefresh = false;
    uint32_t busEvery = awake ? BUS_REFRESH_MS : BUS_REFRESH_IDLE_MS;
    if (first || force || millis() - lastBus > busEvery) {
      busOk = fetchBuses();
      lastBus = millis();
    }
    if (first || force || millis() - lastWeather > WEATHER_REFRESH_MS) {
      fetchWeather();
      lastWeather = millis();
    }
    if (tgEnabled() && !chatId.length() && (first || millis() - lastChatTry > 20000)) {
      tgFindChat();
      if (chatId.length()) tgSend("\xE2\x9C\x85 ExitPilot is connected. Tap your tag on the way out and I'll message you here.");
      lastChatTry = millis();
    }
    first = false;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ---------------------------------------------------------------- sensors (core 1)
bool readSht30(float &c, float &rh) {
  Wire.beginTransmission(SHT30_ADDR);
  Wire.write(0x2C); Wire.write(0x06);          // single shot, high repeatability
  if (Wire.endTransmission() != 0) return false;
  delay(16);
  if (Wire.requestFrom(SHT30_ADDR, (uint8_t)6) != 6) return false;
  uint8_t d[6];
  for (int i = 0; i < 6; i++) d[i] = Wire.read();
  c  = ((d[0] << 8) | d[1]) * 175.0f / 65535.0f - 45.0f;
  rh = ((d[3] << 8) | d[4]) * 100.0f / 65535.0f;
  return true;
}

void readSensors() {
  // ToF: only read when a new measurement is ready, so it never blocks
  if (tofOk && (tof.readReg(VL53L0X::RESULT_INTERRUPT_STATUS) & 0x07)) {
    uint16_t mm = tof.readRangeContinuousMillimeters();
    distanceMm = (tof.timeoutOccurred() || mm >= 8000) ? -1 : mm;
    // Learn the empty-scene distance (wall, desk, door) and wake when something comes
    // clearly closer than it, so the same code works wherever the Fire is mounted.
    float d = distanceMm > 0 ? (float)distanceMm : 2000.0f;    // out of range = far
    if (background < 0) background = d;
    bool near = d < background - WAKE_DELTA_MM && d < WAKE_DISTANCE_MM;
    if (near) lastPresence = millis();
    else background = background * 0.98f + d * 0.02f;       // track slow changes only
  }

  // Light: higher = darker. Must hold for 3 s so a passing shadow doesn't flicker it.
  lightRaw = analogRead(LIGHT_PIN);
  static uint32_t lightSince = 0;
  bool wantNight = night ? !(lightRaw < LIGHT_DAY_BELOW) : (lightRaw > LIGHT_NIGHT_ABOVE);
  if (wantNight == night) lightSince = millis();
  else if (millis() - lightSince > 3000) {
    night = wantNight;
    Serial.println(night ? "Night mode" : "Day mode");
  }

  // ENV II every few seconds
  if (millis() - lastEnvRead > ENV_REFRESH_MS || lastEnvRead == 0) {
    float c, rh;
    envOk = readSht30(c, rh);
    if (envOk) { indoorC = c; indoorRH = rh; }
    lastEnvRead = millis();
  }
}

// ---------------------------------------------------------------- outputs
uint16_t loadColour(const char *load) {
  if (strcmp(load, "SEA") == 0) return TFT_GREEN;      // seats available
  if (strcmp(load, "SDA") == 0) return TFT_YELLOW;     // standing available
  if (strcmp(load, "LSD") == 0) return TFT_RED;        // limited standing
  return TFT_DARKGREY;
}

void beep(int times, int freq = 2200) {
  if (muted) return;
  for (int i = 0; i < times; i++) {
    M5.Speaker.tone(freq, 100);
    if (i < times - 1) delay(160);
  }
}

// leaveIn: minutes until you must leave (-1 = no catchable bus)
void updateLeds(int leaveIn) {
  uint32_t c;
  leds.setBrightness(night ? LED_BRIGHTNESS_NIGHT : LED_BRIGHTNESS);
  if (!awake) {
    c = leds.Color(0, 0, 0);
  } else if (raining) {                                  // blue breathing = bring umbrella
    float k = 0.5f + 0.5f * sinf(millis() / 400.0f);
    c = leds.Color(0, 0, (uint8_t)(40 + 215 * k));
  } else if (leaveIn < 0) {
    c = leds.Color(0, 0, 0);
  } else if (leaveIn <= 1) {
    c = leds.Color(255, 40, 0);                          // leave now
  } else if (leaveIn <= 3) {
    c = leds.Color(255, 160, 0);                         // get ready
  } else {
    c = leds.Color(0, 120, 0);                           // relax
  }
  for (int i = 0; i < LED_COUNT; i++) leds.setPixelColor(i, c);
  leds.show();
}

void showToast(const char *msg, uint32_t ms = 4000) {
  strlcpy(toast, msg, sizeof(toast));
  toastUntil = millis() + ms;
}

// ---------------------------------------------------------------- rfid (core 1)
bool tagAllowed(const char *uid) {
  if (strlen(KNOWN_TAGS) == 0) return true;
  char list[128], key[24];
  snprintf(list, sizeof(list), ",%s,", KNOWN_TAGS);
  snprintf(key, sizeof(key), ",%s,", uid);
  return strstr(list, key) != nullptr;
}

void handleTap(const char *uid, bool randomId) {
  lastPresence = millis();                               // a tap also wakes the screen
  Serial.printf("RFID tag %s\n", uid);

  if (randomId) {                                        // some bank cards: new ID every tap
    showToast("Random-ID card, use another tag");
    beep(3, 900);
    return;
  }
  if (!tagAllowed(uid)) {
    char m[64];
    snprintf(m, sizeof(m), "Unknown tag %s", uid);
    showToast(m);
    beep(3, 900);
    return;
  }

  isOut = !isOut;
  beep(1, isOut ? 2600 : 1800);                          // instant feedback: high = out, low = home
  prefs.putBool("out", isOut);

  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  char hhmm[8];
  strftime(hhmm, sizeof(hhmm), "%H:%M", &lt);

  TgMsg m;
  if (isOut) {
    char bestNo[8]; int bestMins;
    char wx[40]; bool rain;
    xSemaphoreTake(dataLock, portMAX_DELAY);
    findBest(now, bestNo, sizeof(bestNo), bestMins);
    strlcpy(wx, weatherText, sizeof(wx));
    rain = raining;
    xSemaphoreGive(dataLock);
    int n = snprintf(m.text, sizeof(m.text), "\xF0\x9F\x9A\xB6 %s left home at %s.", PERSON_NAME, hhmm);
    if (bestMins >= 0)
      n += snprintf(m.text + n, sizeof(m.text) - n, "\nHeading for bus %s, arriving in %d min.", bestNo, bestMins);
    if (rain)
      snprintf(m.text + n, sizeof(m.text) - n, "\n\xE2\x98\x94 Rain forecast for %s (%s).", WEATHER_AREA, wx);
  } else {
    int n = snprintf(m.text, sizeof(m.text), "\xF0\x9F\x8F\xA0 %s is home at %s.", PERSON_NAME, hhmm);
    if (envOk)
      snprintf(m.text + n, sizeof(m.text) - n, "\nIndoor %.1f\xC2\xB0" "C, %.0f%% humidity.", indoorC, indoorRH);
  }
  if (tgEnabled()) xQueueSend(tgQueue, &m, 0);          // the network task sends it

  char t[64];
  snprintf(t, sizeof(t), isOut ? "Bye! Left at %s" : "Welcome home, %s", hhmm);
  showToast(t);
}

// A tap counts when a card ARRIVES. Holding it there does nothing; it has to be
// away from the reader for a moment (and the lockout must be over) to count again.
void pollRfid() {
  static bool present = false;
  static uint32_t lastSeen = 0, lockoutUntil = 0;

  // WUPA also wakes cards we halted last time, so a card left on the reader keeps
  // answering every poll and is never mistaken for a new tap.
  byte atqa[2];
  byte sz = sizeof(atqa);
  byte st = rfid.PICC_WakeupA(atqa, &sz);
  bool seen = (st == MFRC522::STATUS_OK || st == MFRC522::STATUS_COLLISION) && rfid.PICC_ReadCardSerial();

  if (seen) {
    lastSeen = millis();
    if (!present) {
      present = true;
      if (millis() > lockoutUntil) {
        char uid[24] = "";
        for (byte i = 0; i < rfid.uid.size && i < 10; i++) {
          char h[3];
          snprintf(h, sizeof(h), "%02X", rfid.uid.uidByte[i]);
          strlcat(uid, h, sizeof(uid));
        }
        bool randomId = rfid.uid.size == 4 && rfid.uid.uidByte[0] == 0x08;
        handleTap(uid, randomId);
        lockoutUntil = millis() + TAP_LOCKOUT_MS;
      }
    }
    rfid.PICC_HaltA();
  } else if (present && millis() - lastSeen > TAP_GONE_MS) {
    present = false;                                     // card taken away
  }
}

// ---------------------------------------------------------------- screen
void drawScreen(int &leaveInOut) {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  canvas.fillScreen(TFT_BLACK);

  xSemaphoreTake(dataLock, portMAX_DELAY);

  // top bar: stop + clock
  canvas.fillRect(0, 0, 320, 22, 0x2104);
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(BUS_STOP_NAME " (" BUS_STOP_CODE ")", 6, 11);
  canvas.setTextDatum(middle_right);
  char clock[16];
  strftime(clock, sizeof(clock), "%H:%M:%S", &lt);
  canvas.setTextColor(WiFi.status() == WL_CONNECTED && busOk ? TFT_WHITE : TFT_RED);
  canvas.drawString(WiFi.status() == WL_CONNECTED ? clock : "NO WIFI", 314, 11);

  // find the soonest bus you can still walk to
  char bestNo[8];
  int bestMins;
  findBest(now, bestNo, sizeof(bestNo), bestMins);
  int leaveIn = bestMins < 0 ? -1 : bestMins - WALK_MINUTES;
  leaveInOut = leaveIn;

  // hero: when to leave
  canvas.setTextDatum(top_center);
  if (!busOk && busCount == 0) {
    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(TFT_ORANGE);
    canvas.drawString("Waiting for bus data", 160, 34);
  } else if (leaveIn < 0) {
    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(TFT_LIGHTGREY);
    canvas.drawString("No catchable bus", 160, 34);
  } else {
    char hero[32];
    uint16_t col;
    if (leaveIn <= 1) { snprintf(hero, sizeof(hero), "LEAVE NOW"); col = TFT_RED; }
    else { snprintf(hero, sizeof(hero), "Leave in %d min", leaveIn); col = leaveIn <= 3 ? TFT_ORANGE : TFT_GREEN; }
    canvas.setFont(&fonts::FreeSansBold18pt7b);
    canvas.setTextColor(col);
    canvas.drawString(hero, 160, 30);
    char sub[48];
    snprintf(sub, sizeof(sub), "for bus %s, arriving in %d min", bestNo, bestMins);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString(sub, 160, 70);
  }

  // bus list
  int y = 94;
  canvas.drawFastHLine(0, y - 4, 320, 0x4208);
  canvas.setFont(&fonts::Font2);
  int shown = 0;
  for (int i = 0; i < busCount && shown < 7; i++) {
    if (!isMine(buses[i].no)) continue;
    int m1 = minsUntil(buses[i].t1, now);
    int m2 = minsUntil(buses[i].t2, now);
    canvas.setTextDatum(top_left);
    canvas.setTextColor(TFT_CYAN);
    canvas.drawString(buses[i].no, 10, y);
    char s1[12], s2[12];
    if (m1 < 0) strcpy(s1, "-"); else if (m1 == 0) strcpy(s1, "Arr"); else snprintf(s1, sizeof(s1), "%d min", m1);
    if (m2 < 0) strcpy(s2, "-"); else if (m2 == 0) strcpy(s2, "Arr"); else snprintf(s2, sizeof(s2), "%d min", m2);
    canvas.setTextColor(m1 >= 0 && m1 < WALK_MINUTES ? TFT_DARKGREY : TFT_WHITE);   // too soon to catch
    canvas.drawString(s1, 90, y);
    canvas.setTextColor(TFT_LIGHTGREY);
    canvas.drawString(s2, 175, y);
    canvas.fillCircle(270, y + 8, 5, loadColour(buses[i].load1));
    y += 17;
    shown++;
  }

  // weather bar
  canvas.fillRect(0, 216, 320, 24, raining ? 0x001F : 0x2104);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(TFT_WHITE);
  char indoor[24] = "";
  if (envOk) snprintf(indoor, sizeof(indoor), "  |  Home %.1fC %.0f%%", indoorC, indoorRH);
  char wline[80];
  if (raining) snprintf(wline, sizeof(wline), "BRING UMBRELLA - %s%s", weatherText, indoor);
  else snprintf(wline, sizeof(wline), "%s: %s%s%s", WEATHER_AREA, weatherText, indoor, muted ? " [M]" : "");
  canvas.drawString(wline, 160, 228);

  xSemaphoreGive(dataLock);

  // RFID toast on top of everything
  if (millis() < toastUntil) {
    canvas.fillRoundRect(10, 92, 300, 56, 8, isOut ? 0x0320 : 0x0010);
    canvas.drawRoundRect(10, 92, 300, 56, 8, TFT_WHITE);
    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(TFT_WHITE);
    canvas.setTextDatum(middle_center);
    canvas.drawString(toast, 160, 120);
  }

  canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------- main
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Speaker.setVolume(SPEAKER_VOLUME);
  leds.begin();
  leds.setBrightness(LED_BRIGHTNESS);
  leds.clear();
  leds.show();

  canvas.setColorDepth(16);
  canvas.createSprite(320, 240);
  M5.Display.setBrightness(SCREEN_DAY);

  Wire.begin(M5.Ex_I2C.getSDA(), M5.Ex_I2C.getSCL());   // Port A
  tof.setTimeout(200);
  tofOk = tof.init();
  if (tofOk) tof.startContinuous(100);
  Serial.printf("ToF: %s\n", tofOk ? "ok" : "NOT FOUND");
  pinMode(LIGHT_PIN, INPUT);
  rfid.PCD_Init();
  lastPresence = millis();

  prefs.begin("exitpilot", false);
  if (!chatId.length()) chatId = prefs.getString("chat", "");
  isOut = prefs.getBool("out", false);
  Serial.printf("Telegram: %s, chat %s, state %s\n", tgEnabled() ? "on" : "off",
                chatId.length() ? chatId.c_str() : "(not found yet)", isOut ? "out" : "home");

  connectWifi();
  configTzTime("SGT-8", "pool.ntp.org", "time.google.com");
  uint32_t start = millis();
  while (!timeReady() && millis() - start < 10000) delay(200);

  dataLock = xSemaphoreCreateMutex();
  tgQueue = xQueueCreate(4, sizeof(TgMsg));
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  M5.update();
  bool wasAwake = awake;
  if (M5.BtnA.wasPressed() && wasAwake) { forceRefresh = true; beep(1); }
  if (M5.BtnB.wasPressed() && wasAwake) { muted = !muted; if (!muted) beep(1); }
  if (M5.BtnC.wasPressed() && wasAwake) { onlyMine = !onlyMine; }
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed()) lastPresence = millis();

  static uint32_t lastRfid = 0;
  if (millis() - lastRfid > RFID_POLL_MS) { pollRfid(); lastRfid = millis(); }

  static uint32_t lastSense = 0;
  if (millis() - lastSense > 50) { readSensors(); lastSense = millis(); }

  // sleep / wake
  awake = ALWAYS_ON || (millis() - lastPresence < AWAKE_SECONDS * 1000UL);
  if (awake && !wasAwake) {                    // someone just walked up
    Serial.println("Wake");
    forceRefresh = true;                       // fresh arrivals right away
    if (raining) beep(2);                      // umbrella reminder on the way out
  }
  if (!awake && wasAwake) Serial.println("Sleep");
  static int lastBl = -1;
  int bl = awake ? (night ? SCREEN_NIGHT : SCREEN_DAY) : 0;
  if (bl != lastBl) { M5.Display.setBrightness(bl); lastBl = bl; }

  if (rainStarted) { rainStarted = false; if (awake) beep(2); }   // rain just appeared in the forecast
  if (tgFailed) { tgFailed = false; showToast("Telegram send failed"); }

  static uint32_t lastDraw = 0;
  static int leaveIn = -1;
  if (awake && millis() - lastDraw > 250) {
    drawScreen(leaveIn);
    lastDraw = millis();
  }
  updateLeds(leaveIn);

  static uint32_t lastLog = 0;
  if (millis() - lastLog > 2000) {
    Serial.printf("wifi=%d dist=%dmm bg=%.0fmm light=%d night=%d awake=%d out=%d indoor=%.1fC %.0f%%\n",
                  WiFi.status() == WL_CONNECTED, distanceMm, background, lightRaw, night, awake, isOut,
                  indoorC, indoorRH);
    lastLog = millis();
  }
  delay(5);
}
