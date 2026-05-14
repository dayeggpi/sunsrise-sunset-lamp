// Sunrise/Sunset Lamp — ESP32-S3 ZERO
// Requires: FastLED library (Arduino Library Manager)
//
// Adjust LED_PIN, NUM_LEDS, BUTTON_PIN for your hardware.
// Edit WIFI_SSID / WIFI_PASS below.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <FastLED.h>
#include <time.h>

#define LED_PIN      10
#define NUM_LEDS    288
#define LED_TYPE    WS2811
#define COLOR_ORDER RGB
#define BUTTON_PIN    9
#define NUM_STOPS    11

// ─── WiFi credentials ────────────────────────────────────────────────────────
#define WIFI_SSID "REPLACE_WIFI_SSID"
#define WIFI_PASS "REPLACE_WIFI_PWD"

// ─── Types ────────────────────────────────────────────────────────────────────

struct DayConfig {
  bool    sunriseEnabled;
  bool    sunsetEnabled;
  uint8_t wakeH, wakeM;
  uint8_t bedH,  bedM;
  uint8_t sunriseDeltaMin;
  uint8_t sunsetDeltaMin;
};

enum LampState : uint8_t { S_OFF, S_ON, S_SUNRISE, S_SUNSET, S_PREVIEW };

// ─── Globals ──────────────────────────────────────────────────────────────────

WebServer   server(80);
Preferences prefs;
CRGB        leds[NUM_LEDS];

DayConfig days[7];
int8_t    tzHours         = 0;
CRGB      defaultColor(255, 215, 130);
uint8_t   defaultBrightness = 255;
bool      onAfterSunrise  = false;

uint8_t corrR = 255, corrG = 255, corrB = 255;

static const uint8_t orderTable[6][3] = {
  {0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}
};
static const char *orderNames[6] = {"RGB","RBG","GRB","GBR","BRG","BGR"};
uint8_t ledOrderIdx = 0;

// Gradient stops — 11 per animation, positions user-editable (0-100)
uint8_t srPos[NUM_STOPS] = {0,10,20,30,40,50,60,70,80,90,100};
uint8_t srCol[NUM_STOPS][3] = {
  {  0,  0,  0},{  8,  1,  0},{ 25,  4,  0},{ 55,  8,  0},
  {110, 20,  0},{175, 40,  2},{230, 75,  5},{255,125, 12},
  {255,170, 30},{255,200, 75},{255,215,130},
};
uint8_t srBri[NUM_STOPS] = {255,255,255,255,255,255,255,255,255,255,255};

uint8_t ssPos[NUM_STOPS] = {0,10,20,30,40,50,60,70,80,90,100};
uint8_t ssCol[NUM_STOPS][3] = {
  {255,215,130},{255,200, 75},{255,170, 30},{255,125, 12},
  {230, 75,  5},{175, 40,  2},{110, 20,  0},{ 55,  8,  0},
  { 25,  4,  0},{  8,  1,  0},{  0,  0,  0},
};
uint8_t ssBri[NUM_STOPS] = {255,255,255,255,255,255,255,255,255,255,255};

LampState lampState    = S_OFF;
bool      userControlled = false;  // user took manual control; skip auto-start until window ends
uint32_t  previewStart = 0;
bool      autoEnabled  = true;     // global kill-switch for sunrise/sunset automation
uint32_t  lastNtpSync  = 0;        // millis() of last NTP configTime call

// ─── Colour ───────────────────────────────────────────────────────────────────

static CRGB lerpRGB(CRGB a, CRGB b, float t) {
  return CRGB(
    (uint8_t)(a.r + (b.r - a.r) * t),
    (uint8_t)(a.g + (b.g - a.g) * t),
    (uint8_t)(a.b + (b.b - a.b) * t)
  );
}

static CRGB sampleGrad(uint8_t col[][3], uint8_t bri[], uint8_t pos[], float t) {
  // Before first stop
  float p0f = pos[0] / 100.0f;
  if (t <= p0f) {
    CRGB c(col[0][0], col[0][1], col[0][2]);
    c.r = (uint8_t)((uint16_t)c.r * bri[0] / 255);
    c.g = (uint8_t)((uint16_t)c.g * bri[0] / 255);
    c.b = (uint8_t)((uint16_t)c.b * bri[0] / 255);
    return c;
  }
  // Interpolate between stops
  for (int i = 0; i < NUM_STOPS - 1; i++) {
    float pa = pos[i]   / 100.0f;
    float pb = pos[i+1] / 100.0f;
    if (pb <= pa) continue;
    if (t >= pa && t <= pb) {
      float lt = (t - pa) / (pb - pa);
      CRGB c = lerpRGB(CRGB(col[i][0],col[i][1],col[i][2]),
                       CRGB(col[i+1][0],col[i+1][1],col[i+1][2]), lt);
      float lb = (float)bri[i] + (float)((int)bri[i+1] - (int)bri[i]) * lt;
      uint8_t b = (uint8_t)constrain((int)lb, 0, 255);
      c.r = (uint8_t)((uint16_t)c.r * b / 255);
      c.g = (uint8_t)((uint16_t)c.g * b / 255);
      c.b = (uint8_t)((uint16_t)c.b * b / 255);
      return c;
    }
  }
  // After last stop
  CRGB last(col[NUM_STOPS-1][0], col[NUM_STOPS-1][1], col[NUM_STOPS-1][2]);
  last.r = (uint8_t)((uint16_t)last.r * bri[NUM_STOPS-1] / 255);
  last.g = (uint8_t)((uint16_t)last.g * bri[NUM_STOPS-1] / 255);
  last.b = (uint8_t)((uint16_t)last.b * bri[NUM_STOPS-1] / 255);
  return last;
}

static CRGB sunriseColor(float t) { return sampleGrad(srCol, srBri, srPos, t); }
static CRGB sunsetColor(float t)  { return sampleGrad(ssCol, ssBri, ssPos, t); }

// Applies logical correction then physical wire reorder.
static CRGB toWire(CRGB c) {
  c.r = (uint8_t)((uint16_t)c.r * corrR / 255);
  c.g = (uint8_t)((uint16_t)c.g * corrG / 255);
  c.b = (uint8_t)((uint16_t)c.b * corrB / 255);
  uint8_t ch[3] = {c.r, c.g, c.b};
  const uint8_t *o = orderTable[ledOrderIdx];
  return CRGB(ch[o[0]], ch[o[1]], ch[o[2]]);
}

// ─── Time helpers ─────────────────────────────────────────────────────────────

static struct tm nowTm() {
  time_t now = time(nullptr);
  struct tm t = {};
  localtime_r(&now, &t);
  return t;
}

static bool inDeltaWindow(bool *sunrise, float *progress) {
  if (!autoEnabled) return false;
  struct tm  t = nowTm();
  DayConfig &d = days[t.tm_wday];
  int now  = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
  int wake = (d.wakeH * 60 + d.wakeM) * 60;
  int bed  = (d.bedH  * 60 + d.bedM ) * 60;
  if (d.sunriseEnabled && d.sunriseDeltaMin > 0) {
    int dSec = d.sunriseDeltaMin * 60;
    if (now >= wake - dSec && now < wake) {
      *sunrise  = true;
      *progress = float(now - (wake - dSec)) / dSec;
      return true;
    }
  }
  if (d.sunsetEnabled && d.sunsetDeltaMin > 0) {
    int dSec = d.sunsetDeltaMin * 60;
    if (now >= bed - dSec && now < bed) {
      *sunrise  = false;
      *progress = float(now - (bed - dSec)) / dSec;
      return true;
    }
  }
  return false;
}

// ─── Persistent storage ───────────────────────────────────────────────────────

static void loadSettings() {
  prefs.begin("lamp", true);
  autoEnabled = prefs.getBool("auto", true);
  tzHours = prefs.getChar("tz", 0);

  uint8_t dc[3] = {255, 215, 130};
  prefs.getBytes("defcol", dc, 3);
  defaultColor      = CRGB(dc[0], dc[1], dc[2]);
  defaultBrightness = (uint8_t)prefs.getChar("dbri", (int8_t)255);

  uint8_t cr[3] = {255, 255, 255};
  prefs.getBytes("corr", cr, 3);
  corrR = cr[0]; corrG = cr[1]; corrB = cr[2];

  ledOrderIdx = (uint8_t)constrain((int)prefs.getChar("ordi", 0), 0, 5);

  prefs.getBytes("srpos", srPos, sizeof(srPos));
  prefs.getBytes("srcol", srCol, sizeof(srCol));
  prefs.getBytes("srbri", srBri, sizeof(srBri));
  prefs.getBytes("sspos", ssPos, sizeof(ssPos));
  prefs.getBytes("sscol", ssCol, sizeof(ssCol));
  prefs.getBytes("ssbri", ssBri, sizeof(ssBri));

  for (int i = 0; i < 7; i++) {
    char k[4]; snprintf(k, sizeof(k), "d%d", i);
    uint8_t b[8] = {1, 1, 7, 0, 22, 0, 20, 20};
    prefs.getBytes(k, b, 8);
    days[i] = {(bool)b[0], (bool)b[1], b[2], b[3], b[4], b[5], b[6], b[7]};
  }
  prefs.end();
}

static void saveSettings() {
  prefs.begin("lamp", false);
  prefs.putBool("auto", autoEnabled);
  prefs.putChar("tz", tzHours);

  uint8_t dc[3] = {defaultColor.r, defaultColor.g, defaultColor.b};
  prefs.putBytes("defcol", dc, 3);
  prefs.putChar("dbri", (int8_t)defaultBrightness);

  uint8_t cr[3] = {corrR, corrG, corrB};
  prefs.putBytes("corr", cr, 3);
  prefs.putChar("ordi", (int8_t)ledOrderIdx);

  prefs.putBytes("srpos", srPos, sizeof(srPos));
  prefs.putBytes("srcol", srCol, sizeof(srCol));
  prefs.putBytes("srbri", srBri, sizeof(srBri));
  prefs.putBytes("sspos", ssPos, sizeof(ssPos));
  prefs.putBytes("sscol", ssCol, sizeof(ssCol));
  prefs.putBytes("ssbri", ssBri, sizeof(ssBri));

  for (int i = 0; i < 7; i++) {
    char k[4]; snprintf(k, sizeof(k), "d%d", i);
    uint8_t b[8] = {days[i].sunriseEnabled, days[i].sunsetEnabled,
                    days[i].wakeH,          days[i].wakeM,
                    days[i].bedH,           days[i].bedM,
                    days[i].sunriseDeltaMin, days[i].sunsetDeltaMin};
    prefs.putBytes(k, b, 8);
  }
  prefs.end();
}

// ─── Button ───────────────────────────────────────────────────────────────────

static void onPress() {
  if (lampState == S_PREVIEW) { lampState = S_OFF; return; }

  bool sr; float prog;
  bool inW = inDeltaWindow(&sr, &prog);

  if (!inW || userControlled) {
    // Simple toggle — user is in control
    if (lampState == S_OFF) {
      lampState       = S_ON;
      onAfterSunrise  = false;
    } else {
      lampState = S_OFF;
    }
    if (!inW) userControlled = false;  // leaving delta: reset for next window
  } else {
    // Inside delta, automation was running (or lamp was manually on) — take manual control
    lampState      = S_OFF;
    userControlled = true;
  }
}

static void handleButton() {
  static bool     stable  = HIGH;
  static bool     prev    = HIGH;
  static uint32_t lastChg = 0;
  bool cur = digitalRead(BUTTON_PIN);
  if (cur != stable) { stable = cur; lastChg = millis(); }
  if (millis() - lastChg > 50) {
    if (stable == LOW && prev == HIGH) onPress();
    prev = stable;
  }
}

// ─── Lamp update ──────────────────────────────────────────────────────────────

static void updateLamp() {
  CRGB col;

  if (lampState == S_PREVIEW) {
    float t = float(millis() - previewStart) / 20000.0f;
    if (t >= 1.0f)     { lampState = S_OFF; col = CRGB::Black; }
    else if (t < 0.5f) { col = sunriseColor(t * 2.0f); }
    else               { col = sunsetColor((t - 0.5f) * 2.0f); }
    fill_solid(leds, NUM_LEDS, toWire(col));
    FastLED.show();
    return;
  }

  bool sr; float prog;
  bool inW = inDeltaWindow(&sr, &prog);

  if (!inW) {
    userControlled = false;  // delta ended: re-enable automation for next window
    if      (lampState == S_SUNRISE) { onAfterSunrise = true;  lampState = S_ON;  }
    else if (lampState == S_SUNSET)  {                          lampState = S_OFF; }
  }

  if (inW && !userControlled) {
    // Auto-start sunrise: only from OFF
    if (sr && lampState == S_OFF)
      lampState = S_SUNRISE;
    // Auto-start sunset: from OFF, or from ON only if lamp came from a sunrise (not manual)
    if (!sr && (lampState == S_OFF || (lampState == S_ON && onAfterSunrise)))
      lampState = S_SUNSET;
  }

  switch (lampState) {
    case S_OFF:
      col = CRGB::Black;
      break;
    case S_ON:
      if (onAfterSunrise) {
        col = sunriseColor(1.0f);          // gradient endpoint color
      } else {
        col = defaultColor;
        col.r = (uint8_t)((uint16_t)col.r * defaultBrightness / 255);
        col.g = (uint8_t)((uint16_t)col.g * defaultBrightness / 255);
        col.b = (uint8_t)((uint16_t)col.b * defaultBrightness / 255);
      }
      break;
    case S_SUNRISE: col = sunriseColor(prog); break;
    case S_SUNSET:  col = sunsetColor(prog);  break;
    default:        col = CRGB::Black;        break;
  }
  fill_solid(leds, NUM_LEDS, toWire(col));
  FastLED.show();
}

// ─── Web server helpers ───────────────────────────────────────────────────────

static void parseHexColor(const String &h, uint8_t *r, uint8_t *g, uint8_t *b) {
  if (h.length() == 7 && h[0] == '#') {
    *r = (uint8_t)strtol(h.substring(1,3).c_str(), NULL, 16);
    *g = (uint8_t)strtol(h.substring(3,5).c_str(), NULL, 16);
    *b = (uint8_t)strtol(h.substring(5,7).c_str(), NULL, 16);
  }
}

static String toHex(uint8_t r, uint8_t g, uint8_t b) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
  return String(buf);
}

// Appends one gradient stop: position input, color picker, hex input, brightness slider.
static void appendStop(String &body, int i,
                        const char *pfxC, const char *pfxH,
                        const char *pfxB, const char *pfxV, const char *pfxP,
                        const char *nameC, const char *nameB, const char *nameP,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t bri, uint8_t pos) {
  String hx = toHex(r, g, b);
  int    bp = (int)bri * 100 / 255;
  String ci = String(pfxC) + i;
  String hi = String(pfxH) + i;
  String bi = String(pfxB) + i;
  String vi = String(pfxV) + i;
  // Position input
  body += F("<div class=gs>");
  body += F("<input type=number name="); body += nameP; body += i;
  body += F(" min=0 max=100 value="); body += pos;
  body += F(" style='width:44px;font-size:.72em;margin-bottom:2px'>");
  // Color swatch + hidden color input
  String si = String("W") + String(pfxC).substring(1) + i;
  body += F("<div id="); body += si;
  body += F(" class=sw style='background:"); body += hx;
  body += F("' onclick='openCP(\""); body += ci; body += F("\",\""); body += hi; body += F("\",\""); body += si; body += F("\")'></div>");
  body += F("<input type=color id="); body += ci;
  body += F(" name="); body += nameC; body += i;
  body += F(" value='"); body += hx; body += F("'>");
  body += F("<input type=text id="); body += hi;
  body += F(" value='"); body += hx;
  body += F("' maxlength=7 onchange='sc(\""); body += hi; body += F("\",\""); body += ci; body += F("\",\""); body += si; body += F("\")'>");
  // Brightness
  body += F("<input type=range id="); body += bi;
  body += F(" name="); body += nameB; body += i;
  body += F(" min=0 max=255 value="); body += bri;
  body += F(" oninput='sb(\""); body += bi; body += F("\",\""); body += vi; body += F("\")'>");
  body += F("<span id="); body += vi; body += F(" class=pct>"); body += bp; body += F("%</span>");
  body += F("</div>");
}

// ─── Web handlers ─────────────────────────────────────────────────────────────

static void handleRoot() {
  static const char *DN[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

  String body;
  body.reserve(22000);

  body += F("<!DOCTYPE html><html><head>"
    "<meta charset=UTF-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Sunrise Lamp</title>"
    "<link rel=icon type='image/png' sizes='16x16' href='data:image/png;base64,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' />"
    "<style>"
    "body{font-family:sans-serif;max-width:860px;margin:auto;padding:16px;"
         "background:#1a1a2e;color:#eee}"
    "h1{color:#f9a825;margin:0 0 16px}"
    "table{width:100%;border-collapse:collapse}"
    "td,th{padding:6px 3px;text-align:center;font-size:.84em}"
    "th{background:#2a2a4e;white-space:nowrap}"
    "tr:nth-child(even){background:#13132b}"
    "input[type=time]{background:#2a2a4e;color:#eee;"
      "border:1px solid #444;border-radius:4px;padding:3px;width:74px}"
    "input[type=number]{background:#2a2a4e;color:#eee;"
      "border:1px solid #444;border-radius:4px;padding:3px;width:52px}"
    "input[type=checkbox]{width:18px;height:18px;cursor:pointer}"
    "input[type=color]{display:none}"
    ".sw{width:48px;height:32px;border-radius:4px;cursor:pointer;display:block;"
      "margin:0 auto;border:2px solid #555}"
    "input[type=text]{background:#2a2a4e;color:#eee;border:1px solid #333;"
      "border-radius:3px;padding:2px;width:56px;font-size:.76em;"
      "box-sizing:border-box;display:block;margin:2px auto;text-align:center}"
    "input[type=range]{width:56px;display:block;margin:2px auto}"
    "select{background:#2a2a4e;color:#eee;border:1px solid #444;"
      "border-radius:4px;padding:4px;font-size:.9em}"
    ".btn{background:#f9a825;color:#000;border:none;border-radius:6px;"
         "padding:10px 24px;font-size:1em;cursor:pointer;width:100%;margin-top:14px}"
    ".btnp{background:#4a9eff;color:#fff;border:none;border-radius:6px;"
          "padding:10px 24px;font-size:1em;cursor:pointer;width:100%;margin-top:8px}"
    ".sec{background:#12122a;border-radius:8px;padding:14px;margin-bottom:14px}"
    "label{display:block;margin:8px 0 3px;font-size:.9em;color:#aaa}"
    ".info{font-size:.85em;color:#ccc;margin-top:10px;min-height:1.4em;"
          "background:#12122a;border-radius:6px;padding:8px}"
    ".note{font-size:.8em;color:#666;margin-top:6px}"
    ".row{display:flex;gap:20px;align-items:flex-start;flex-wrap:wrap}"
    ".grad{display:flex;gap:5px;margin-top:6px;overflow-x:auto;padding-bottom:4px}"
    ".gs{text-align:center;min-width:58px}"
    ".pct{font-size:.72em;color:#aaa;display:block}"
    ".sep{border-left:1px solid #333}"
    ".cr{display:flex;gap:8px;align-items:center;margin-top:4px}"
    ".cr span{font-size:.85em;min-width:12px}"
    "</style></head><body>"
    "<h1>&#127749; Sunrise Lamp</h1>"
    "<form method=POST action=/save>");

  // ── Settings ────────────────────────────────────────────────────────────────
  String dcHex = toHex(defaultColor.r, defaultColor.g, defaultColor.b);
  body += F("<div class=sec><b>Settings</b><div class=row>");

  // Default color + hex + brightness
  body += F("<div><label>Default lamp color</label>"
            "<div style='display:flex;gap:6px;align-items:center'>"
            "<div id=Wdc class=sw style='background:");
  body += dcHex;
  body += F("' onclick='openCP(\"Cdc\",\"Hdc\",\"Wdc\")'></div>"
            "<input type=color id=Cdc name=defcol value='");
  body += dcHex;
  body += F("'>"
            "<input type=text id=Hdc value='");
  body += dcHex;
  body += F("' maxlength=7 style='display:inline;width:62px' onchange='sc(\"Hdc\",\"Cdc\",\"Wdc\")'>"
            "</div>"
            "<label>Default brightness</label>"
            "<div style='display:flex;gap:8px;align-items:center'>"
            "<input type=range id=Bdc name=dbri min=0 max=255 value=");
  body += defaultBrightness;
  body += F(" oninput='sb(\"Bdc\",\"Vdc\")'>"
            "<span id=Vdc class=pct>");
  body += (int)defaultBrightness * 100 / 255;
  body += F("%</span></div></div>");

  // Timezone
  body += F("<div><label>Timezone (UTC+)</label>"
            "<input type=number name=tz value='");
  body += tzHours;
  body += F("' min=-12 max=14 style='width:64px'></div>");

  // Automation enable
  body += F("<div><label>Sunrise/Sunset automation</label>"
            "<label style='display:flex;gap:8px;align-items:center;cursor:pointer'>"
            "<input type=checkbox name=auto value=1");
  if (autoEnabled) body += F(" checked");
  body += F(">Enable scheduled automation</label>"
            "<div style='font-size:.76em;color:#777;margin-top:3px'>"
            "Uncheck to disable all effects (keeps your schedule intact).</div></div>");

  // Wire order
  body += F("<div><label>Physical LED wire order</label>"
            "<select name=ordi>");
  for (int i = 0; i < 6; i++) {
    body += F("<option value="); body += i;
    if (i == ledOrderIdx) body += F(" selected");
    body += F(">"); body += orderNames[i]; body += F("</option>");
  }
  body += F("</select>"
            "<div style='font-size:.76em;color:#777;margin-top:3px'>"
            "Warm looks cool? Try BGR. Red looks green? Try GRB.</div></div>");

  // Channel correction
  body += F("<div><label>Channel correction (0-255)</label>"
            "<div class=cr>"
            "<span style='color:#f77'>R</span>"
            "<input type=number name=corrr min=0 max=255 value='");
  body += corrR;
  body += F("' style='width:52px'>"
            "<span style='color:#7f7'>G</span>"
            "<input type=number name=corrg min=0 max=255 value='");
  body += corrG;
  body += F("' style='width:52px'>"
            "<span style='color:#77f'>B</span>"
            "<input type=number name=corrb min=0 max=255 value='");
  body += corrB;
  body += F("' style='width:52px'></div></div>");

  body += F("</div></div>");

  // ── Gradient colors ─────────────────────────────────────────────────────────
  body += F("<div class=sec><b>Gradient Colors</b>"
            "<label>Sunrise &mdash; position(%) | color | brightness</label>"
            "<div class=grad>");
  for (int i = 0; i < NUM_STOPS; i++)
    appendStop(body, i, "Csr","Hsr","Bsr","Vsr","Psr",
               "src","srb","srp",
               srCol[i][0], srCol[i][1], srCol[i][2], srBri[i], srPos[i]);
  body += F("</div>"
            "<label>Sunset &mdash; position(%) | color | brightness</label>"
            "<div class=grad>");
  for (int i = 0; i < NUM_STOPS; i++)
    appendStop(body, i, "Css","Hss","Bss","Vss","Pss",
               "ssc","ssb","ssp",
               ssCol[i][0], ssCol[i][1], ssCol[i][2], ssBri[i], ssPos[i]);
  body += F("</div></div>");

  // ── Schedule ────────────────────────────────────────────────────────────────
  body += F("<div class=sec><b>Schedule</b>"
            "<table><tr><th>Day</th>"
            "<th>&#9728; SR</th><th>Wake</th><th>Rise&Delta;(m)</th>"
            "<th class=sep>&#9790; SS</th><th>Bed</th><th>Set&Delta;(m)</th>"
            "</tr>");
  for (int i = 0; i < 7; i++) {
    char wk[6], bd[6];
    snprintf(wk, 6, "%02d:%02d", days[i].wakeH, days[i].wakeM);
    snprintf(bd, 6, "%02d:%02d", days[i].bedH,  days[i].bedM);
    body += "<tr><td>" + String(DN[i]) + "</td>"
      "<td><input type=checkbox name=sr" + i + " value=1"
      + (days[i].sunriseEnabled ? F(" checked") : F("")) + "></td>"
      "<td><input type=time name=wk" + i + " value='" + wk + "'></td>"
      "<td><input type=number name=rd" + i + " value='"
      + days[i].sunriseDeltaMin + "' min=1 max=120></td>"
      "<td class=sep><input type=checkbox name=ss" + i + " value=1"
      + (days[i].sunsetEnabled ? F(" checked") : F("")) + "></td>"
      "<td><input type=time name=bd" + i + " value='" + bd + "'></td>"
      "<td><input type=number name=sd" + i + " value='"
      + days[i].sunsetDeltaMin + "' min=1 max=120></td></tr>";
  }
  body += F("</table></div>");

  // ── Buttons + status + JS ───────────────────────────────────────────────────
  body += F("<button class=btn type=submit>Save &amp; Apply</button></form>"
            "<button id=btnOff class=btnp onclick='toggleLamp()'"
              " style='background:#e53935'>Turn OFF</button>"
            "<button class=btnp onclick='startPreview()'>"
              "&#9654; Preview Full Animation (20s)"
            "</button>"
            "<div class=info id=st>Loading...</div>"
            "<p class=note>WiFi: " WIFI_SSID
            " | <a href=/restart style='color:#f9a825'>Restart</a></p>"
            "<div id=cp onclick='if(event.target===this)closeCP()'"
              " style='display:none;position:fixed;top:0;left:0;width:100%;height:100%;"
              "background:rgba(0,0,0,.75);z-index:100;align-items:center;justify-content:center'>"
            "<div style='background:#1a1a2e;padding:18px;border-radius:12px;width:240px;"
              "box-shadow:0 4px 20px rgba(0,0,0,.6)'>"
            "<div id=cpPrev style='height:56px;border-radius:8px;margin-bottom:10px'></div>"
            "<div style='margin-bottom:8px'>"
            "<label style='color:#f77;font-size:.8em'>R <span id=cpRv></span></label>"
            "<input type=range id=cpR min=0 max=255 style='width:100%' oninput=upCP()>"
            "<label style='color:#7f7;font-size:.8em'>G <span id=cpGv></span></label>"
            "<input type=range id=cpG min=0 max=255 style='width:100%' oninput=upCP()>"
            "<label style='color:#77f;font-size:.8em'>B <span id=cpBv></span></label>"
            "<input type=range id=cpB min=0 max=255 style='width:100%' oninput=upCP()>"
            "</div>"
            "<input type=text id=cpHex maxlength=7 oninput=hexCP()"
              " style='width:100%;box-sizing:border-box;background:#2a2a4e;color:#eee;"
              "border:1px solid #444;border-radius:4px;padding:6px;text-align:center;"
              "font-size:.9em;margin-bottom:8px'>"
            "<div style='display:flex;gap:8px'>"
            "<button onclick=applyCP() style='flex:1;padding:8px;border-radius:6px;"
              "background:#4a9eff;color:#fff;border:none;cursor:pointer;font-size:1em'>OK</button>"
            "<button onclick=closeCP() style='flex:1;padding:8px;border-radius:6px;"
              "background:#333;color:#eee;border:none;cursor:pointer;font-size:1em'>Cancel</button>"
            "</div></div></div>"
            "<script>"
            "var SN=['OFF','ON','SUNRISE','SUNSET','PREVIEW'];"
            "function sh(c,h){document.getElementById(h).value=document.getElementById(c).value;}"
            "function sc(h,c,s){var v=document.getElementById(h).value;"
              "if(/^#[0-9a-fA-F]{6}$/.test(v)){"
              "document.getElementById(c).value=v;"
              "if(s)document.getElementById(s).style.background=v;}}"
            "function sb(b,v){document.getElementById(v).textContent="
              "Math.round(document.getElementById(b).value/2.55)+'%';}"
            "var cpC=null,cpH=null,cpS=null;"
            "function openCP(c,h,s){"
              "cpC=c;cpH=h;cpS=s;"
              "var hx=document.getElementById(h).value;"
              "document.getElementById('cpR').value=parseInt(hx.slice(1,3),16)||0;"
              "document.getElementById('cpG').value=parseInt(hx.slice(3,5),16)||0;"
              "document.getElementById('cpB').value=parseInt(hx.slice(5,7),16)||0;"
              "upCP();"
              "document.getElementById('cp').style.display='flex';}"
            "function upCP(){"
              "var r=+document.getElementById('cpR').value;"
              "var g=+document.getElementById('cpG').value;"
              "var b=+document.getElementById('cpB').value;"
              "var h='#'+[r,g,b].map(function(v){return v.toString(16).padStart(2,'0')}).join('');"
              "document.getElementById('cpPrev').style.background=h;"
              "document.getElementById('cpHex').value=h;"
              "document.getElementById('cpRv').textContent=r;"
              "document.getElementById('cpGv').textContent=g;"
              "document.getElementById('cpBv').textContent=b;}"
            "function hexCP(){"
              "var h=document.getElementById('cpHex').value;"
              "if(/^#[0-9a-fA-F]{6}$/.test(h)){"
              "document.getElementById('cpR').value=parseInt(h.slice(1,3),16);"
              "document.getElementById('cpG').value=parseInt(h.slice(3,5),16);"
              "document.getElementById('cpB').value=parseInt(h.slice(5,7),16);"
              "document.getElementById('cpPrev').style.background=h;"
              "document.getElementById('cpRv').textContent=document.getElementById('cpR').value;"
              "document.getElementById('cpGv').textContent=document.getElementById('cpG').value;"
              "document.getElementById('cpBv').textContent=document.getElementById('cpB').value;}}"
            "function applyCP(){"
              "var h=document.getElementById('cpHex').value;"
              "if(cpH)document.getElementById(cpH).value=h;"
              "if(cpC)document.getElementById(cpC).value=h;"
              "if(cpS)document.getElementById(cpS).style.background=h;"
              "closeCP();}"
            "function closeCP(){document.getElementById('cp').style.display='none';}"
            "function xget(url,cb){"
              "var x=new XMLHttpRequest();"
              "x.onreadystatechange=function(){"
                "if(x.readyState===4){"
                  "if(x.status===200&&cb)cb(x.responseText);"
                  "else if(x.status!==200)"
                    "document.getElementById('st').textContent='HTTP '+x.status;"
                "}"
              "};"
              "x.open('GET',url,true);x.send();"
            "}"
            "function toggleLamp(){xget('/toggle',function(){upd();});}"
            "function upd(){"
              "xget('/status',function(t){"
                "try{"
                  "var d=JSON.parse(t);"
                  "var s='Time: '+d.time+' | '+SN[d.state];"
                  "if(d.inDelta)s+=' | '+Math.round(d.progress*100)+'%';"
                  "if(!d.auto)s+=' | AUTO:OFF';"
                  "s+=' | Corr R:'+d.corrR+' G:'+d.corrG+' B:'+d.corrB;"
                  "document.getElementById('st').textContent=s;"
                  "var off=d.state===0;"
                  "var b=document.getElementById('btnOff');"
                  "b.textContent=off?'Turn ON':'Turn OFF';"
                  "b.style.background=off?'#4a9eff':'#e53935';"
                "}catch(e){document.getElementById('st').textContent='Bad JSON';}"
              "});"
            "}"
            "function startPreview(){"
              "xget('/preview',null);"
              "document.getElementById('st').textContent='Preview running... (20s)';"
            "}"
            "upd();setInterval(upd,3000);"
            "</script></body></html>");

  server.send(200, "text/html; charset=utf-8", body);
}

static void handleSave() {
  if (server.hasArg("defcol"))
    parseHexColor(server.arg("defcol"), &defaultColor.r, &defaultColor.g, &defaultColor.b);
  if (server.hasArg("dbri"))
    defaultBrightness = (uint8_t)constrain(server.arg("dbri").toInt(), 0, 255);
  if (server.hasArg("tz"))
    tzHours = (int8_t)constrain(server.arg("tz").toInt(), -12, 14);
  if (server.hasArg("corrr")) corrR = (uint8_t)constrain(server.arg("corrr").toInt(), 0, 255);
  if (server.hasArg("corrg")) corrG = (uint8_t)constrain(server.arg("corrg").toInt(), 0, 255);
  if (server.hasArg("corrb")) corrB = (uint8_t)constrain(server.arg("corrb").toInt(), 0, 255);
  if (server.hasArg("ordi"))
    ledOrderIdx = (uint8_t)constrain(server.arg("ordi").toInt(), 0, 5);

  for (int i = 0; i < NUM_STOPS; i++) {
    if (server.hasArg("src" + String(i)))
      parseHexColor(server.arg("src" + String(i)), &srCol[i][0], &srCol[i][1], &srCol[i][2]);
    if (server.hasArg("srb" + String(i)))
      srBri[i] = (uint8_t)constrain(server.arg("srb" + String(i)).toInt(), 0, 255);
    if (server.hasArg("srp" + String(i)))
      srPos[i] = (uint8_t)constrain(server.arg("srp" + String(i)).toInt(), 0, 100);
    if (server.hasArg("ssc" + String(i)))
      parseHexColor(server.arg("ssc" + String(i)), &ssCol[i][0], &ssCol[i][1], &ssCol[i][2]);
    if (server.hasArg("ssb" + String(i)))
      ssBri[i] = (uint8_t)constrain(server.arg("ssb" + String(i)).toInt(), 0, 255);
    if (server.hasArg("ssp" + String(i)))
      ssPos[i] = (uint8_t)constrain(server.arg("ssp" + String(i)).toInt(), 0, 100);
  }

  for (int i = 0; i < 7; i++) {
    days[i].sunriseEnabled = server.hasArg("sr" + String(i));
    days[i].sunsetEnabled  = server.hasArg("ss" + String(i));
    if (server.hasArg("wk" + String(i))) {
      String v = server.arg("wk" + String(i));
      days[i].wakeH = v.substring(0,2).toInt();
      days[i].wakeM = v.substring(3,5).toInt();
    }
    if (server.hasArg("bd" + String(i))) {
      String v = server.arg("bd" + String(i));
      days[i].bedH = v.substring(0,2).toInt();
      days[i].bedM = v.substring(3,5).toInt();
    }
    if (server.hasArg("rd" + String(i)))
      days[i].sunriseDeltaMin = (uint8_t)constrain(server.arg("rd" + String(i)).toInt(), 1, 120);
    if (server.hasArg("sd" + String(i)))
      days[i].sunsetDeltaMin  = (uint8_t)constrain(server.arg("sd" + String(i)).toInt(), 1, 120);
  }

  autoEnabled    = server.hasArg("auto");
  userControlled = false;   // re-enable auto cycle after manual save
  saveSettings();
  configTime(tzHours * 3600, 0, "pool.ntp.org");
  lastNtpSync = millis();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "Saved");
}

static void handleStatus() {
  bool sr; float prog = 0;
  bool inW = inDeltaWindow(&sr, &prog);
  struct tm t = nowTm();
  char ts[9]; strftime(ts, sizeof(ts), "%H:%M:%S", &t);
  String json = "{\"time\":\"" + String(ts) + "\","
                "\"state\":"    + (int)lampState + ","
                "\"inDelta\":"  + (inW ? "true" : "false") + ","
                "\"progress\":" + String(prog, 2) + ","
                "\"auto\":"     + (autoEnabled ? "true" : "false") + ","
                "\"corrR\":"    + corrR + ","
                "\"corrG\":"    + corrG + ","
                "\"corrB\":"    + corrB + "}";
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "application/json", json);
}

static void handleRestart() {
  server.send(200, "text/plain", "Restarting...");
  delay(500);
  ESP.restart();
}

static void handlePreview() {
  lampState    = S_PREVIEW;
  previewStart = millis();
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "text/plain", "ok");
}

static void handleToggle() {
  onPress();
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200, "text/plain", lampState == S_OFF ? "off" : "on");
}

// ─── Setup / Loop ─────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(220);
  FastLED.setCorrection(UncorrectedColor);
  FastLED.clear();
  FastLED.show();

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  loadSettings();

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000)
    delay(500);

  if (WiFi.status() == WL_CONNECTED) {
    configTime(tzHours * 3600, 0, "pool.ntp.org");
    lastNtpSync = millis();
    Serial.print("IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi failed -- check WIFI_SSID / WIFI_PASS");
  }

  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/save",    HTTP_POST, handleSave);
  server.on("/status",  HTTP_GET,  handleStatus);
  server.on("/restart", HTTP_GET,  handleRestart);
  server.on("/preview", HTTP_GET,  handlePreview);
  server.on("/toggle",  HTTP_GET,  handleToggle);
  server.begin();
}

void loop() {
  server.handleClient();
  handleButton();
  updateLamp();
  // 10800000UL = every 3 hours, 3600000UL = every hour and so on
  if (WiFi.status() == WL_CONNECTED && millis() - lastNtpSync >= 10800000UL) {
    configTime(tzHours * 3600, 0, "pool.ntp.org");
    lastNtpSync = millis();
  }
  delay(100);
}
