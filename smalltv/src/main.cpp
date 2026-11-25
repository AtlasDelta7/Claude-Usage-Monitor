// SmallTV Ultra - Claude usage display
//
// - Wi-Fi via WiFiManager ("SmallTV-Setup" hotspot if no known network)
// - Password-protected OTA updates (smalltv.local)
// - Receives usage from the laptop bridge: POST http://smalltv.local/u
//   with header X-Token: PUSH_TOKEN and form fields
//   s, sr (session %, seconds to reset), w, wr (weekly %, seconds to reset), err,
//   a (1 = the computer is unlocked/in use, 0 = locked), demo (seconds of sleep-screen preview)
// - Counts reset timers down locally between pushes
// - Night mode: between NIGHT_START_H and NIGHT_END_H (local time, from NTP) the
//   screen dims and goes black except for a snoring mascot - unless the computer
//   is still unlocked (the bridge sends a=1)

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <TFT_eSPI.h>
#include <time.h>
#include "secrets.h"

#ifndef PUSH_TOKEN
#error "Add  #define PUSH_TOKEN \"...\"  to src/secrets.h - see README"
#endif

#define BACKLIGHT_PIN 5          // active low
#define BRIGHTNESS 40            // backlight, percent (1-100)
#define STALE_MS (10UL * 60UL * 1000UL)   // no push for 10 min -> warn

// Night mode
#define TZ_INFO "GMT0BST,M3.5.0/1,M10.5.0"  // POSIX TZ string; this is UK time
#define NIGHT_START_H 0                  // sleep screen from 00:00 ...
#define NIGHT_END_H   7                  // ... until 07:00
#define NIGHT_BRIGHTNESS 12              // backlight percent while asleep
#define AWAKE_MS (3UL * 60UL * 1000UL)   // computer counts as in use for 3 min after an a=1 push

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const uint16_t C_BG     = rgb(0, 0, 0);
const uint16_t C_TRACK  = rgb(40, 40, 42);
const uint16_t C_TEXT   = rgb(255, 255, 255);
const uint16_t C_SUB    = rgb(120, 120, 125);
const uint16_t C_BLUE   = rgb(10, 132, 255);
const uint16_t C_AMBER  = rgb(255, 159, 10);
const uint16_t C_RED    = rgb(255, 69, 58);
const uint16_t C_ACCENT = rgb(217, 119, 87);

TFT_eSPI tft;
ESP8266WebServer server(80);

struct Limit {
  int pct = -1;          // -1 = unknown
  long resetSecs = -1;   // -1 = no active window
  uint32_t at = 0;       // millis() when received
};

Limit sess, week;
bool haveData = false;
bool fresh = false;      // new data arrived
int drawnPct[2] = {-3, -3};
int drawnDay[2] = {-1, -1};
uint32_t lastPush = 0;
String err = "";
String drawnReset[2] = {"", ""};
String drawnFooter = "";

// Layout. ROOMY spreads everything over the whole screen; TIGHT squeezes it up
// to leave room for the footer whenever there's an error/warning to show.
const int X0 = 16, X1 = 224, BAR_W = X1 - X0;
struct Layout {
  int headY;       // centre line of the logo + "Claude" header
  int secY[2];     // top of each section
  int barOff, barH;  // progress bar: offset from section top, height
  int mark;        // how far the weekly target marker sticks out above/below the bar
  int resetOff;    // "Resets in" line: offset from section top (24 px tall)
};
const Layout ROOMY = {22, {50, 144}, 32, 14, 3, 50};   // sections 74 px tall, 20 px gap between them
const Layout TIGHT = {24, {48, 126}, 32, 12, 2, 46};
const int FOOTER_Y = 206;
const Layout* L = &TIGHT;   // boot shows "Waiting for laptop" in the footer

// ---------- helpers ----------

void bootScreen(const String& title, const String& l1, const String& l2, uint16_t c) {
  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(c);
  tft.drawString(title, X0, 30);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(C_TEXT);
  tft.drawString(l1, X0, 80);
  tft.setTextColor(C_SUB);
  tft.drawString(l2, X0, 110);
}

long remaining(const Limit& l) {
  if (l.resetSecs < 0) return -1;
  long r = l.resetSecs - (long)((millis() - l.at) / 1000);
  return r < 0 ? 0 : r;
}

String resetText(const Limit& l) {
  if (!haveData) return "Waiting for data";
  long r = remaining(l);
  if (r < 0) return "Not started";
  if (r == 0) return "Resetting...";
  long d = r / 86400, h = (r % 86400) / 3600, m = (r % 3600) / 60;
  if (d > 0) return "Resets in " + String(d) + "d " + String(h) + "h";
  if (h > 0) return "Resets in " + String(h) + "h " + String(m) + "m";
  return "Resets in " + String(m < 1 ? 1 : m) + "m";
}

int shownPct(const Limit& l) {
  // once the window has reset, show 0 until the next push says otherwise
  if (l.resetSecs >= 0 && remaining(l) == 0) return 0;
  return l.pct;
}

// Which day (1-7) of the weekly window we're in; 0 = unknown.
int targetDay(const Limit& l) {
  if (!haveData) return 0;
  long r = remaining(l);
  if (r <= 0 || r > 7L * 86400) return 0;
  long elapsed = 7L * 86400 - r;
  return elapsed / 86400 + 1;
}

uint16_t barColour(int pct) {
  if (pct >= 90) return C_RED;
  if (pct >= 75) return C_AMBER;
  return C_BLUE;
}

void drawLogo(int cx, int cy) {
  // Claude "spark": 12 rays with deliberately irregular angles, lengths and
  // tip widths so it looks hand-drawn rather than like a symmetric asterisk.
  // Each ray is thin at the centre and widens to a rounded tip. Drawn with
  // anti-aliased wedge lines, so it costs no image data at all.
  struct Ray { int16_t ang; uint8_t len, tip; };   // degrees, % of R, tip radius x10
  static const Ray RAYS[] = {
    {-95, 96, 18}, {-62, 70, 14}, {-30, 100, 17}, {3, 74, 15}, {35, 90, 18}, {60, 58, 13},
    {88, 100, 17}, {118, 76, 15}, {146, 94, 16}, {176, 64, 14}, {203, 100, 18}, {236, 72, 14},
  };
  const float R = 11.5f;
  for (const Ray& ray : RAYS) {
    float a = ray.ang * DEG_TO_RAD, c = cosf(a), s = sinf(a);
    float r = R * ray.len / 100.0f;
    tft.drawWedgeLine(cx + 0.5f * c, cy + 0.5f * s, cx + r * c, cy + r * s,
                      0.2f, ray.tip / 10.0f, C_ACCENT, C_BG);
  }
  tft.fillSmoothCircle(cx, cy, 1, C_ACCENT, C_BG);   // clean up where the rays meet
}

// Draw into an off-screen buffer, then copy to the screen in one go (no flicker).
// Falls back to drawing directly if there's not enough RAM.
template <typename F>
void flickerFree(int x, int y, int w, int h, F draw) {
  TFT_eSprite spr(&tft);
  if (spr.createSprite(w, h)) {
    spr.fillSprite(C_BG);
    draw(spr, 0, 0);
    spr.pushSprite(x, y);
    spr.deleteSprite();
  } else {
    tft.fillRect(x, y, w, h, C_BG);
    draw(tft, x, y);
  }
}

void drawResetIcon(TFT_eSPI& g, int cx, int cy, uint16_t c) {
  // clockwise "refresh" arrow: a ring with a gap on the right, arrowhead at
  // the top end pointing down into the gap.
  // TFT_eSPI arc angles: 0 = 6 o'clock, clockwise. Ring runs 4 o'clock -> 1:30.
  g.drawArc(cx, cy, 7, 5, 300, 360, c, C_BG, true);
  g.drawArc(cx, cy, 7, 5, 0, 225, c, C_BG, true);
  // arrowhead at screen angle -45 deg (1:30), along the clockwise tangent
  const float a = -45 * DEG_TO_RAD, ca = cosf(a), sa = sinf(a);
  const float px = cx + 6 * ca, py = cy + 6 * sa;      // point on ring centre-line
  const float tx = -sa, ty = ca;                       // tangent (clockwise)
  g.fillTriangle(px + 3.5f * ca, py + 3.5f * sa,       // outer corner
                 px - 3.5f * ca, py - 3.5f * sa,       // inner corner
                 px + 4.5f * tx, py + 4.5f * ty, c);   // tip
}

void drawResetLine(int i, const Limit& l) {
  String t = resetText(l);
  flickerFree(0, L->secY[i] + L->resetOff, 240, 24, [&](TFT_eSPI& g, int ox, int oy) {
    drawResetIcon(g, ox + X0 + 8, oy + 11, C_SUB);
    g.setFreeFont(&FreeSans9pt7b);
    g.setTextColor(C_SUB);
    g.setTextDatum(ML_DATUM);
    g.drawString(t, ox + X0 + 24, oy + 11);
  });
  drawnReset[i] = t;
}

void drawPctAndBar(int i, const Limit& l) {
  int y = L->secY[i];
  int pct = shownPct(l);
  String p = (haveData && pct >= 0) ? String(pct) + "% used" : "--";
  flickerFree(X1 - 118, y, 118, 28, [&](TFT_eSPI& g, int ox, int oy) {
    g.setTextDatum(BR_DATUM);
    g.setFreeFont(&FreeSansBold12pt7b);
    g.setTextColor(C_TEXT);
    g.drawString(p, ox + 118, oy + 24);
  });

  // progress bar (drawn directly - takes ~1 ms, not visible)
  int by = y + L->barOff, bh = L->barH, mk = L->mark;
  tft.fillRect(X0 - 1, by - mk, BAR_W + 2, bh + 2 * mk, C_BG);   // clears the old target marker too
  tft.fillSmoothRoundRect(X0, by, BAR_W, bh, bh / 2, C_TRACK, C_BG);
  if (haveData && pct > 0) {
    int w = (long)BAR_W * min(pct, 100) / 100;
    if (w < bh) w = bh;
    tft.fillSmoothRoundRect(X0, by, w, bh, bh / 2, barColour(pct), C_TRACK);
  }

  // weekly bar: split into 7 day segments by gaps cut right through the bar,
  // plus a white marker sticking out above and below at today's target (end of
  // the current day, ~14% per day) - stay left of it to last the week.
  int day = (i == 1) ? targetDay(l) : 0;
  if (day) {
    for (int d = 1; d < 7; d++) {
      int x = X0 + (long)BAR_W * d / 7;
      if (d == day) tft.fillRect(x - 1, by - mk, 3, bh + 2 * mk, C_TEXT);
      else          tft.fillRect(x, by, 2, bh, C_BG);
    }
  }
  drawnDay[i] = day;
  drawnPct[i] = haveData ? pct : -2;
}

void drawSection(int i, const char* label, const Limit& l) {
  int y = L->secY[i];
  tft.fillRect(0, y, 240, L->resetOff, C_BG);
  tft.setTextDatum(BL_DATUM);
  tft.setFreeFont(&FreeSans12pt7b);
  tft.setTextColor(C_TEXT);
  tft.drawString(label, X0, y + 24);
  drawPctAndBar(i, l);
  drawResetLine(i, l);
}

String footerText(uint16_t& c) {
  c = C_SUB;
  if (!haveData) return "Waiting for laptop  " + WiFi.localIP().toString();
  uint32_t age = millis() - lastPush;
  if (err == "auth") { c = C_RED;   return "sessionKey expired - update it"; }
  if (err == "cf")   { c = C_AMBER; return "Blocked by claude.ai - retrying"; }
  if (err == "net")  { c = C_AMBER; return "Laptop can't reach claude.ai"; }
  if (err.length())  { c = C_AMBER; return "Bridge error: " + err; }
  if (age > STALE_MS) { c = C_AMBER; return "No update for " + String(age / 60000) + "m - laptop off?"; }
  return "";
}

void drawFooter(bool force = false) {
  uint16_t c;
  String t = footerText(c);
  if (t == drawnFooter && !force) return;
  if (L != &TIGHT) return;   // only the tight layout has room for a footer
  flickerFree(0, FOOTER_Y, 240, 240 - FOOTER_Y, [&](TFT_eSPI& g, int ox, int oy) {
    if (!t.length()) return;
    g.setTextFont(2);
    g.setTextColor(c, C_BG);
    g.setTextDatum(MC_DATUM);
    g.drawString(t, ox + 120, oy + 16);
  });
  drawnFooter = t;
}

// Full redraw - at boot, and when switching between the roomy/tight layouts
void drawAll() {
  tft.fillScreen(C_BG);
  drawLogo(X0 + 10, L->headY);
  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(C_TEXT);
  tft.drawString("Claude", X0 + 30, L->headY - 1);
  drawSection(0, "Session", sess);
  drawSection(1, "Weekly", week);
  drawFooter(true);
}

// Redraw only the parts whose content changed
void refresh() {
  // switch layout when a footer message appears or goes away
  uint16_t fc;
  const Layout* want = footerText(fc).length() ? &TIGHT : &ROOMY;
  if (want != L) {
    L = want;
    drawAll();
    return;
  }
  Limit* ls[2] = {&sess, &week};
  for (int i = 0; i < 2; i++) {
    Limit& l = *ls[i];
    if (haveData && l.resetSecs >= 0 && remaining(l) == 0) l.pct = 0;   // window just reset
    int want = haveData ? shownPct(l) : -2;
    int day = (i == 1) ? targetDay(l) : 0;
    if (want != drawnPct[i] || day != drawnDay[i]) drawPctAndBar(i, l);
    if (resetText(l) != drawnReset[i]) drawResetLine(i, l);
  }
  drawFooter();
}

// ---------- night mode ----------

bool sleeping = false;
bool macAwake = false;     // last push said the computer is unlocked
uint32_t awakeAt = 0;      // millis() of that push
uint32_t demoUntil = 0;    // sleep-screen preview end (millis), 0 = off

void setBrightness(int pct) {
  analogWrite(BACKLIGHT_PIN, 1023 - (1023 * constrain(pct, 1, 100)) / 100);  // inverted
}

bool isNightTime() {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;   // clock not set yet (no NTP answer)
  struct tm t;
  localtime_r(&now, &t);
  if (NIGHT_START_H <= NIGHT_END_H) return t.tm_hour >= NIGHT_START_H && t.tm_hour < NIGHT_END_H;
  return t.tm_hour >= NIGHT_START_H || t.tm_hour < NIGHT_END_H;   // window crosses midnight
}

bool wantSleep() {
  if (demoUntil && (long)(millis() - demoUntil) < 0) return true;
  demoUntil = 0;
  bool inUse = macAwake && millis() - awakeAt < AWAKE_MS;
  return isNightTime() && !inUse;
}

// The mascot: an 18x6 pixel-art critter (body, side arms, 4 legs), drawn at
// MS x scale. Rows are bitmasks, leftmost pixel = bit 17.
const int MS = 6, M_W = 18, M_BODY = 5;
const uint32_t M_ROWS[M_BODY] = {
  0b000111111111111000,   // top of head
  0b000111111111111000,
  0b000111111111111000,   // eye row (eyes drawn on top)
  0b011111111111111110,   // arms
  0b000111111111111000,   // bottom of body
};
const int M_EYE_ROW = 2;
const int M_LEGS[4] = {4, 6, 11, 13};  // leg columns
const int M_EYES[2] = {5, 12};         // eye columns (row 1)
const uint16_t C_MASCOT = rgb(235, 100, 50);   // more saturated than C_ACCENT so it stays orange when dimmed
const uint16_t C_EYE  = rgb(40, 20, 14);
const uint16_t C_ZZZ  = rgb(170, 170, 185);

// sprite areas
const int MASCOT_X = 62, MASCOT_Y = 176, MASCOT_W = 116, MASCOT_H = 46;
const int ZZZ_X = 130, ZZZ_Y = 94, ZZZ_W = 100, ZZZ_H = 82;   // ends just above the mascot's head

void drawMascot(TFT_eSPI& g, int ox, int oy, int lift) {
  int x0 = ox + 4;
  int bodyTop = oy + 4 - lift;                     // body rises when breathing in
  int legTop  = bodyTop + M_BODY * MS;             // legs stretch so their feet stay planted
  for (int r = 0; r < M_BODY; r++)
    for (int c = 0; c < M_W; c++)
      if (M_ROWS[r] & (1UL << (M_W - 1 - c)))
        g.fillRect(x0 + c * MS, bodyTop + r * MS, MS, MS, C_MASCOT);
  for (int c : M_LEGS)
    g.fillRect(x0 + c * MS, legTop, MS, MS + lift, C_MASCOT);
  for (int c : M_EYES)                           // closed eyes: short dark dashes
    g.fillRect(x0 + c * MS, bodyTop + M_EYE_ROW * MS + MS / 2, MS, 2, C_EYE);
}

void drawZ(TFT_eSPI& g, float cx, float cy, float size, uint16_t c) {
  float h = size / 2;
  g.drawWideLine(cx - h, cy - h, cx + h, cy - h, 2, c, C_BG);
  g.drawWideLine(cx + h, cy - h, cx - h, cy + h, 2, c, C_BG);
  g.drawWideLine(cx - h, cy + h, cx + h, cy + h, 2, c, C_BG);
}

void enterSleep() {
  tft.fillScreen(C_BG);
  setBrightness(NIGHT_BRIGHTNESS);
}

// Called every loop while asleep: gentle breathing + three Zs drifting up.
void animateSleep(bool force) {
  const uint32_t BREATH_MS = 3600, Z_MS = 3600, FRAME_MS = 80;
  static uint32_t lastFrame = 0;
  static int drawnLift = -1;
  uint32_t t = millis();
  if (!force && t - lastFrame < FRAME_MS) return;
  lastFrame = t;

  int lift = (t % BREATH_MS) < BREATH_MS / 2 ? 2 : 0;
  if (force || lift != drawnLift) {
    flickerFree(MASCOT_X, MASCOT_Y, MASCOT_W, MASCOT_H,
                [&](TFT_eSPI& g, int ox, int oy) { drawMascot(g, ox, oy, lift); });
    drawnLift = lift;
  }

  flickerFree(ZZZ_X, ZZZ_Y, ZZZ_W, ZZZ_H, [&](TFT_eSPI& g, int ox, int oy) {
    for (int k = 0; k < 3; k++) {
      float p = ((t + k * Z_MS / 3) % Z_MS) / (float)Z_MS;     // 0 -> 1 as the Z rises
      float fade = p < 0.2f ? p / 0.2f : (1 - p) / 0.8f;     // fade in, then out
      uint16_t c = tft.alphaBlend((uint8_t)(255 * fade), C_ZZZ, C_BG);
      float cx = ox + 18 + p * 50 + 4 * sinf(p * 6.3f);       // start above the head, drift up-right
      float cy = oy + ZZZ_H - 7 - p * 60;
      drawZ(g, cx, cy, 6 + p * 10, c);
    }
  });
}

void updateSleep() {
  bool want = wantSleep();
  if (want != sleeping) {
    sleeping = want;
    if (sleeping) {
      enterSleep();
      animateSleep(true);
    } else {
      setBrightness(BRIGHTNESS);
      drawAll();
    }
  }
  if (sleeping) animateSleep(false);
}

// ---------- push endpoint ----------

void handlePush() {
  if (server.header("X-Token") != PUSH_TOKEN) {
    server.send(403, "text/plain", "forbidden");
    return;
  }
  uint32_t now = millis();
  auto readLimit = [&](const char* pk, const char* rk, Limit& l) {
    if (server.hasArg(pk)) l.pct = constrain(server.arg(pk).toInt(), -1, 100);
    if (server.hasArg(rk)) l.resetSecs = max(-1L, (long)server.arg(rk).toInt());
    l.at = now;
  };
  String newErr = server.arg("err");
  if (newErr.length() > 40) newErr = newErr.substring(0, 40);
  if (server.hasArg("s")) {            // a push with data
    readLimit("s", "sr", sess);
    readLimit("w", "wr", week);
    haveData = true;
  }
  if (server.hasArg("a")) {
    macAwake = server.arg("a") == "1";
    if (macAwake) awakeAt = now;
  }
  if (server.hasArg("demo")) demoUntil = now + constrain(server.arg("demo").toInt(), 0, 600) * 1000UL;
  err = newErr;
  lastPush = now;
  fresh = true;
  server.send(200, "text/plain", "ok");
}

// ---------- setup / loop ----------

void onPortal(WiFiManager*) {
  bootScreen("Wi-Fi setup", "Join: SmallTV-Setup", "Then open 192.168.4.1", C_AMBER);
}

void setup() {
  Serial.begin(115200);
  pinMode(BACKLIGHT_PIN, OUTPUT);
  analogWriteRange(1023);
  analogWriteFreq(10000);                                  // high PWM freq = no visible flicker
  setBrightness(BRIGHTNESS);
  tft.init();
  tft.setRotation(0);
  bootScreen("Booting...", "", "", C_TEXT);

  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setAPCallback(onPortal);
  wm.setConnectTimeout(30);
  wm.setConfigPortalTimeout(300);
  wm.setShowInfoUpdate(false);
  if (!wm.autoConnect("SmallTV-Setup", SETUP_AP_PASS)) {
    bootScreen("No Wi-Fi", "Restarting...", "", C_RED);
    delay(2000);
    ESP.restart();
  }

  ArduinoOTA.setHostname("smalltv");
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() { setBrightness(BRIGHTNESS); bootScreen("Updating...", "Don't unplug", "", C_BLUE); });
  ArduinoOTA.begin();

  server.collectHeaders("X-Token");
  server.on("/u", HTTP_POST, handlePush);
  server.onNotFound([]() { server.send(404, "text/plain", ""); });
  server.begin();

  configTime(TZ_INFO, "pool.ntp.org", "time.google.com");   // clock for night mode
  drawAll();
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();

  updateSleep();
  static uint32_t lastTick = 0;
  if (!sleeping && (fresh || millis() - lastTick > 5000)) {
    fresh = false;
    lastTick = millis();
    refresh();
  }

  static uint32_t lost = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (!lost) lost = millis();
    if (millis() - lost > 120000) ESP.restart();
  } else {
    lost = 0;
  }
}
