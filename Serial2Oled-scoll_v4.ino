/*
  ESP32-C3 + 0.42" SSD1306 (72x40) OLED

  Features
  - Lower ticker with commands:
      f/1..3  font small/medium/large (Nordic-ready Helvetica)
      s/0     stop scroll (non-blocking)
      s/1..9  scroll speed (1=slowest, 9=fastest)
      h/1     show ticker
      h/0     hide ticker (static centered text)
  - Full-screen clock modes:
      t/1     show time (HH:MM:SS)
      t/2     show time + date (YYYY-MM-DD) on the line under time
      t/0     (or any other command/text) hides time until t/1 or t/2
      tz/N    manual timezone offset in HOURS (e.g., tz/2, tz/-1)
      ts/HH:MM:SS  set manual time (used when no valid NTP)
  - Wi-Fi via serial (optional):
      w/SSID        save SSID
      wp/PASS       save password
      wc/           connect now (NTP auto if connected)
      wf/           forget saved Wi-Fi
  - Serial: 115200 baud, newline '\n'
  - Pins:   SDA=5, SCL=6, I2C addr 0x3C
*/

#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <time.h>
#include <Preferences.h>

#define OLED_SDA   5
#define OLED_SCL   6
#define OLED_ADDR  0x3C

U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ---- Layout ----
const uint8_t W = 72, H = 40;
const uint8_t GAP = 16;
const uint8_t BASE_TICKER = 32; // tuned for Helvetica
const uint8_t BASE_STATIC = 21;

// ---- State ----
String msg   = "Hei! æ ø å Æ Ø Å";
String inbuf = "";

bool tickerEnabled = true;           // h/1 on, h/0 off
bool scrolling     = true;           // s/0 stop, s/1..9 scroll
int  scroll_interval_ms = 100;       // s/1=100 ... s/9≈5
int16_t x          = W;              // ticker x
int16_t text_w     = 0;              // width of msg

// Clock modes: 0=off, 1=time, 2=time+date
uint8_t clockMode  = 0;

// ---- Fonts (Nordic-ready) ----
enum FontMode { F_SMALL=1, F_MEDIUM, F_LARGE };
FontMode currentFont = F_SMALL;

// ---- Manual clock (no NTP) ----
bool manualClock = false;
uint32_t manualEpochBase = 0;        // seconds since epoch at set time (0 if unknown date)
uint32_t manualSetMillis = 0;        // millis at set time
bool manualDateKnown = false;        // true if we had a real epoch when ts/ was issued

// ---- Timezone (manual offset) ----
int16_t tzOffsetMinutes = 0;         // minutes relative to UTC; tz/±H updates this

// ---- Wi-Fi / NTP ----
Preferences prefs;
String WIFI_SSID = "";
String WIFI_PASS = "";
bool wifiOK = false;

// For NTP we use raw epoch (UTC) and apply tzOffset ourselves
static const char* NTP_0 = "pool.ntp.org";
static const char* NTP_1 = "time.nist.gov";
static const char* NTP_2 = "time.google.com";

// ---- Timers (non-blocking) ----
uint32_t t_now = 0;
uint32_t t_nextFrame = 0;         // display refresh cadence
uint32_t t_nextScroll = 0;        // scroll step cadence
uint32_t t_nextNtpCheck = 0;      // periodic NTP sanity check
const uint16_t FRAME_INTERVAL_MS = 33;  // ~30 FPS
const uint16_t CLOCK_FRAME_MS    = 200; // ~5 FPS

// ---------- Helpers ----------
void applyFont() {
  switch (currentFont) {
    case F_SMALL:  u8g2.setFont(u8g2_font_helvR08_tf); break;
    case F_MEDIUM: u8g2.setFont(u8g2_font_helvR10_tf); break;
    case F_LARGE:  u8g2.setFont(u8g2_font_helvR12_tf); break;
  }
}

void computeWidth() {
  applyFont();
  text_w = u8g2.getUTF8Width(msg.c_str());
  if (text_w < 1) text_w = 1;
}

// s/0 stop; s/1 slowest (100ms) ... s/9 fastest (≈5ms)
int speedToInterval(int n) {
  if (n <= 0) return 0;
  if (n >= 9) return 5;
  return 110 - n * 10; // 1->100, 2->90 ... 8->20
}

String two(int v){ char b[3]; snprintf(b, sizeof(b), "%02d", v); return String(b); }
String four(int v){ char b[5]; snprintf(b, sizeof(b), "%04d", v); return String(b); }

bool epochValid(time_t e) { return e >= 1700000000; } // ~2023+

// Get current epoch seconds (UTC) from either NTP or manual clock
// Returns -1 if unknown
int64_t getEpochNow() {
  time_t now = time(nullptr);
  if (epochValid(now)) return (int64_t)now;
  if (manualClock) {
    uint32_t elapsed = (millis() - manualSetMillis) / 1000;
    if (manualEpochBase == 0) {
      // date unknown: still return seconds-of-day since a fake epoch 0
      return (int64_t)elapsed; // will show -- date later
    } else {
      return (int64_t)manualEpochBase + elapsed;
    }
  }
  return -1;
}

// Convert epoch + tzOffsetMinutes to broken-down UTC-like time using gmtime
// (we apply offset ourselves so it's independent of system TZ)
bool getBrokenDownWithTZ(struct tm& out) {
  int64_t e = getEpochNow();
  if (e < 0) return false;
  e += (int32_t)tzOffsetMinutes * 60;
  time_t adj = (time_t)e;
  gmtime_r(&adj, &out);
  return true;
}

String getClockString() {
  struct tm tinfo;
  if (!getBrokenDownWithTZ(tinfo)) return String("--:--:--");
  return two(tinfo.tm_hour) + ":" + two(tinfo.tm_min) + ":" + two(tinfo.tm_sec);
}

String getDateStringISO() {
  struct tm tinfo;
  if (!getBrokenDownWithTZ(tinfo)) return String("----------");
  return four(tinfo.tm_year + 1900) + "-" + two(tinfo.tm_mon + 1) + "-" + two(tinfo.tm_mday);
}

// ---------- Wi-Fi / NTP ----------
void wifiConnect() {
  if (WIFI_SSID.isEmpty()) { wifiOK=false; return; }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID.c_str(), WIFI_PASS.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(50);
  wifiOK = (WiFi.status() == WL_CONNECTED);
  if (wifiOK) {
    // Use UTC; we'll apply tzOffset manually when drawing
    configTime(0, 0, NTP_0, NTP_1, NTP_2);
  }
}

// ---------- Drawing ----------
void drawTickerFrame() {
  applyFont();
  u8g2.clearBuffer();
  // two copies for seamless loop
  u8g2.drawUTF8(x,                BASE_TICKER, msg.c_str());
  u8g2.drawUTF8(x + text_w + GAP, BASE_TICKER, msg.c_str());
  u8g2.sendBuffer();
}

void drawStaticFrame() {
  applyFont();
  u8g2.clearBuffer();
  int16_t x0 = (W - text_w) / 2; if (x0 < 0) x0 = 0;
  u8g2.drawUTF8(x0, BASE_STATIC, msg.c_str());
  u8g2.sendBuffer();
}

void drawClockFrame(uint8_t mode /*1=time, 2=time+date*/) {
  String t = getClockString();
  u8g2.clearBuffer();

  // Big time centered
  u8g2.setFont(u8g2_font_helvR12_tf);
  int16_t w = u8g2.getUTF8Width(t.c_str());
  int16_t x0 = (W - w) / 2; if (x0 < 0) x0 = 0;
  uint8_t baseline = (mode == 1) ? 26 : 20; // lift time if we also show date
  u8g2.drawUTF8(x0, baseline, t.c_str());

  if (mode == 2) {
    String d = getDateStringISO(); // YYYY-MM-DD
    u8g2.setFont(u8g2_font_helvR08_tf);
    int16_t wd = u8g2.getUTF8Width(d.c_str());
    int16_t xd = (W - wd) / 2; if (xd < 0) xd = 0;
    u8g2.drawUTF8(xd, 38, d.c_str());
  }

  u8g2.sendBuffer();
}

// ---------- Storage ----------
void saveWifi() {
  prefs.begin("net", false);
  prefs.putString("ssid", WIFI_SSID);
  prefs.putString("pass", WIFI_PASS);
  prefs.end();
}
void loadWifi() {
  prefs.begin("net", true);
  WIFI_SSID = prefs.getString("ssid", "");
  WIFI_PASS = prefs.getString("pass", "");
  prefs.end();
}

// ---------- Commands ----------
void handleCommand(const String& line) {
  // --- Fullscreen clock modes ---
  if (line.startsWith("t/")) {
    char c = line.length() > 2 ? line.charAt(2) : '0';
    if (c == '1') clockMode = 1;            // time only
    else if (c == '2') clockMode = 2;       // time + date
    else clockMode = 0;                     // any other "t/.." hides clock
    return;
  }

  // --- Timezone: tz/<hours>, e.g. tz/2 or tz/-1 ---
  if (line.startsWith("tz/")) {
    int n = line.substring(3).toInt();      // hours
    if (n < -12) n = -12;
    if (n >  14) n = 14;
    tzOffsetMinutes = n * 60;
    // any non-t/.. command hides clock until explicitly re-enabled
    clockMode = 0;
    return;
  }

  // --- Ticker visibility ---
  if (line.startsWith("h/")) {
    char c = line.charAt(2);
    if (c == '1') tickerEnabled = true;
    if (c == '0') tickerEnabled = false;
    clockMode = 0; // hide clock on other commands
    return;
  }

  // --- Speed ---
  if (line.startsWith("s/")) {
    int n = line.substring(2).toInt();
    if (n == 0) { scrolling = false; scroll_interval_ms = 0; }
    else if (n >= 1 && n <= 9) { scrolling = true; scroll_interval_ms = speedToInterval(n); }
    clockMode = 0;
    return;
  }

  // --- Font ---
  if (line.startsWith("f/")) {
    char c = line.charAt(2);
    if      (c == '1') currentFont = F_SMALL;
    else if (c == '2') currentFont = F_MEDIUM;
    else if (c == '3') currentFont = F_LARGE;
    computeWidth();
    clockMode = 0;
    return;
  }

  // --- Manual time set: ts/HH:MM:SS ---
  if (line.startsWith("ts/") && line.length() >= 11) {
    int HH = line.substring(3,5).toInt();
    int MM = line.substring(6,8).toInt();
    int SS = line.substring(9,11).toInt();
    if (HH>=0 && HH<24 && MM>=0 && MM<60 && SS>=0 && SS<60) {
      time_t now = time(nullptr);
      bool haveEpoch = epochValid(now);
      // Build a base epoch: today's midnight if we know epoch; else "unknown date"
      uint32_t baseMidnight = haveEpoch ? (uint32_t)(now - (now % 86400)) : 0;
      manualEpochBase  = baseMidnight + (uint32_t)(HH*3600 + MM*60 + SS);
      manualSetMillis  = millis();
      manualClock      = true;
      manualDateKnown  = haveEpoch;
    }
    clockMode = 0;
    return;
  }

  // --- Wi-Fi setup ---
  if (line.startsWith("w/"))  { WIFI_SSID = line.substring(2); saveWifi(); clockMode = 0; return; }
  if (line.startsWith("wp/")) { WIFI_PASS = line.substring(3); saveWifi(); clockMode = 0; return; }
  if (line.startsWith("wc/")) { wifiConnect(); clockMode = 0; return; }
  if (line.startsWith("wf/")) {
    prefs.begin("net", false);
    prefs.remove("ssid"); prefs.remove("pass");
    prefs.end();
    WIFI_SSID=""; WIFI_PASS="";
    WiFi.disconnect(true, true);
    wifiOK=false;
    clockMode = 0;
    return;
  }

  // --- Plain text updates ticker; hide clock ---
  msg = line;
  x = W;
  computeWidth();
  clockMode = 0;
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { if (inbuf.length()) { handleCommand(inbuf); inbuf = ""; } }
    else { inbuf += c; if (inbuf.length() > 300) inbuf.remove(0, 100); }
  }
}

void setup() {
  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.setI2CAddress(OLED_ADDR * 2);
  u8g2.begin();
  u8g2.setContrast(255);

  Serial.begin(115200);

  // Start with UTC on the system; we apply tzOffset manually for display.
  configTime(0, 0, NTP_0, NTP_1, NTP_2);

  // Load Wi-Fi creds and try connect (optional)
  loadWifi();
  if (!WIFI_SSID.isEmpty()) wifiConnect();

  computeWidth();

  t_now = millis();
  t_nextFrame = t_now + FRAME_INTERVAL_MS;
  t_nextScroll = t_now + scroll_interval_ms;
  t_nextNtpCheck = t_now + 2000;
}

void loop() {
  t_now = millis();
  handleSerial();

  // periodic "have epoch" check (non-blocking)
  if ((int32_t)(t_now - t_nextNtpCheck) >= 0) {
    // just touching time(nullptr) is enough; no flag needed beyond epochValid()
    t_nextNtpCheck += 2000;
  }

  if (clockMode == 1 || clockMode == 2) {
    if ((int32_t)(t_now - t_nextFrame) >= 0) {
      drawClockFrame(clockMode);
      t_nextFrame += CLOCK_FRAME_MS;
    }
    delay(1); // yield
    return;
  }

  // Scroll step (non-blocking)
  if (tickerEnabled && scrolling && scroll_interval_ms > 0) {
    if ((int32_t)(t_now - t_nextScroll) >= 0) {
      x -= 1;
      if (x < -(text_w + GAP)) x = W;
      t_nextScroll += scroll_interval_ms;
    }
  }

  // Redraw at steady frame rate
  if ((int32_t)(t_now - t_nextFrame) >= 0) {
    if (tickerEnabled) drawTickerFrame();
    else               drawStaticFrame();
    t_nextFrame += FRAME_INTERVAL_MS;
  }

  delay(1); // tiny yield
}
