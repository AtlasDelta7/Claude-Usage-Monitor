// SmallTV Ultra - Claude usage display
//
// - Wi-Fi via WiFiManager ("SmallTV-Setup" hotspot if no known network)
// - Password-protected OTA updates (smalltv.local)
// - Receives usage from the laptop bridge: POST http://smalltv.local/u
//   with header X-Token: PUSH_TOKEN and form fields
//   s, sr (session %, seconds to reset), w, wr (weekly %, seconds to reset), err
// - Counts reset timers down locally between pushes

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <TFT_eSPI.h>
#include "secrets.h"

#ifndef PUSH_TOKEN
#error "Add  #define PUSH_TOKEN \"...\"  to src/secrets.h - see README"
#endif

#define BACKLIGHT_PIN 5          // active low
#define BRIGHTNESS 40            // backlight, percent (1-100)
#define STALE_MS (10UL * 60UL * 1000UL)   // no push for 10 min -> warn

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
uint32_t lastPush = 0;
String err = "";
String drawnReset[2] = {"", ""};
String drawnFooter = "";

// Layout (baselines)
const int X0 = 16, X1 = 224, BAR_W = X1 - X0;
const int SEC_Y[2] = {48, 126};   // top of each section

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

uint16_t barColour(int pct) {
  if (pct >= 90) return C_RED;
  if (pct >= 75) return C_AMBER;
  return C_BLUE;
}

void drawLogo(int cx, int cy) {
  // simple 8-ray asterisk
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4 + PI / 8;
    float x0 = cx + 2.5f * cos(a), y0 = cy + 2.5f * sin(a);
    float x1 = cx + 10.0f * cos(a), y1 = cy + 10.0f * sin(a);
    tft.drawWideLine(x0, y0, x1, y1, 3, C_ACCENT, C_BG);
  }
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
  // circular arrow
  g.drawArc(cx, cy, 7, 5, 40, 330, c, C_BG, false);
  g.fillTriangle(cx - 9, cy - 4, cx - 1, cy - 4, cx - 5, cy + 1, c);
}

void drawResetLine(int i, const Limit& l) {
  String t = resetText(l);
  flickerFree(0, SEC_Y[i] + 46, 240, 24, [&](TFT_eSPI& g, int ox, int oy) {
    drawResetIcon(g, ox + X0 + 8, oy + 11, C_SUB);
    g.setFreeFont(&FreeSans9pt7b);
    g.setTextColor(C_SUB);
    g.setTextDatum(ML_DATUM);
    g.drawString(t, ox + X0 + 24, oy + 11);
  });
  drawnReset[i] = t;
}

void drawPctAndBar(int i, const Limit& l) {
  int y = SEC_Y[i];
  int pct = shownPct(l);
  String p = (haveData && pct >= 0) ? String(pct) + "% used" : "--";
  flickerFree(X1 - 118, y, 118, 28, [&](TFT_eSPI& g, int ox, int oy) {
    g.setTextDatum(BR_DATUM);
    g.setFreeFont(&FreeSansBold12pt7b);
    g.setTextColor(C_TEXT);
    g.drawString(p, ox + 118, oy + 24);
  });

  // progress bar (drawn directly - takes ~1 ms, not visible)
  int by = y + 32, bh = 12;
  tft.fillSmoothRoundRect(X0, by, BAR_W, bh, bh / 2, C_TRACK, C_BG);
  if (haveData && pct > 0) {
    int w = (long)BAR_W * min(pct, 100) / 100;
    if (w < bh) w = bh;
    tft.fillSmoothRoundRect(X0, by, w, bh, bh / 2, barColour(pct), C_TRACK);
  }
  drawnPct[i] = haveData ? pct : -2;
}

void drawSection(int i, const char* label, const Limit& l) {
  int y = SEC_Y[i];
  tft.fillRect(0, y, 240, 46, C_BG);
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
  flickerFree(0, 206, 240, 34, [&](TFT_eSPI& g, int ox, int oy) {
    if (!t.length()) return;
    g.setTextFont(2);
    g.setTextColor(c, C_BG);
    g.setTextDatum(MC_DATUM);
    g.drawString(t, ox + 120, oy + 16);
  });
  drawnFooter = t;
}

// Full redraw - only at boot
void drawAll() {
  tft.fillScreen(C_BG);
  drawLogo(X0 + 10, 24);
  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(C_TEXT);
  tft.drawString("Claude", X0 + 30, 23);
  drawSection(0, "Session", sess);
  drawSection(1, "Weekly", week);
  drawFooter(true);
}

// Redraw only the parts whose content changed
void refresh() {
  Limit* ls[2] = {&sess, &week};
  for (int i = 0; i < 2; i++) {
    Limit& l = *ls[i];
    if (haveData && l.resetSecs >= 0 && remaining(l) == 0) l.pct = 0;   // window just reset
    int want = haveData ? shownPct(l) : -2;
    if (want != drawnPct[i]) drawPctAndBar(i, l);
    if (resetText(l) != drawnReset[i]) drawResetLine(i, l);
  }
  drawFooter();
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
  analogWrite(BACKLIGHT_PIN, 1023 - (1023 * constrain(BRIGHTNESS, 1, 100)) / 100);  // inverted
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
  ArduinoOTA.onStart([]() { bootScreen("Updating...", "Don't unplug", "", C_BLUE); });
  ArduinoOTA.begin();

  server.collectHeaders("X-Token");
  server.on("/u", HTTP_POST, handlePush);
  server.onNotFound([]() { server.send(404, "text/plain", ""); });
  server.begin();

  drawAll();
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();

  static uint32_t lastTick = 0;
  if (fresh || millis() - lastTick > 5000) {
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
