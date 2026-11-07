/*
  ============================================================
   BEATBUDDY v2  -  your little audio buddy between phone & buds
  ============================================================
   Hardware : ESP32 + ILI9341 (320x240) + FT6206 capacitive touch

   PLAY  : spinning vinyl whose label is Beat the cat. The tonearm
           lowers when music plays, the record slows down when you
           pause, Beat falls asleep. Tap the record to boop Beat.
   EQ    : the REAL frequency-response curve of your filters, with
           5 draggable glowing nodes + cute presets.
   FX    : two dial knobs (Bass Boost, Stereo Width) + Loud/Night/Mono.
   SETUP : link status, brightness, sleep timer, and Beat's color
           (the whole UI re-themes to match).

   DEMO_MODE 1 -> UI runs by itself (fake beat + fake progress)
   DEMO_MODE 0 -> Beat reacts to the audio you push through
                  processAudio().
  ============================================================
*/

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Adafruit_FT6206.h>
#include <math.h>

// ---- Types used in function signatures must come BEFORE any function (Arduino auto-prototypes) ----
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float z1[2] = {0, 0}, z2[2] = {0, 0};
  void set(float nb0, float nb1, float nb2, float a0, float na1, float na2) {
    b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0; a1 = na1 / a0; a2 = na2 / a0;
  }
  inline float process(int ch, float x) {
    float y = b0 * x + z1[ch];
    z1[ch] = b1 * x - a1 * y + z2[ch];
    z2[ch] = b2 * x - a2 * y;
    return y;
  }
};

// ======================= CONFIG ============================
#define TFT_DC        2
#define TFT_CS        15
#define TFT_RST       4
#define TFT_BL        -1      // backlight PWM pin (-1 = not wired / always on)
#define BATTERY_PIN   -1      // ADC pin of a 1:2 divider on the battery (-1 = hide battery)
#define DEMO_MODE     1

// ======================= COLORS ============================
constexpr uint16_t C565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
constexpr uint16_t COL_BG    = C565(14, 12, 30);
constexpr uint16_t COL_CARD  = C565(32, 28, 60);
constexpr uint16_t COL_CARD2 = C565(52, 46, 92);
constexpr uint16_t COL_TEXT  = C565(248, 244, 255);
constexpr uint16_t COL_DIM   = C565(150, 144, 190);
constexpr uint16_t COL_PINK  = C565(255, 138, 184);
constexpr uint16_t COL_MINT  = C565(116, 232, 188);
constexpr uint16_t COL_LAV   = C565(184, 156, 255);
constexpr uint16_t COL_YEL   = C565(255, 215, 110);
constexpr uint16_t COL_DARK  = C565(28, 24, 52);
constexpr uint16_t COL_CHEEK = C565(255, 96, 140);
constexpr float    D2R       = PI / 180.0f;

const uint16_t BUDDY[4] = { COL_PINK, COL_MINT, COL_LAV, COL_YEL };   // Beat's color choices
uint16_t ACC = COL_PINK;                                              // current theme color

uint16_t lerp565(uint16_t a, uint16_t b, int t) {                     // t = 0..255
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return ((ar + (br - ar) * t / 255) << 11) | ((ag + (bg - ag) * t / 255) << 5) | (ab + (bb - ab) * t / 255);
}

// ======================= HARDWARE ==========================
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
Adafruit_FT6206 ctouch = Adafruit_FT6206();
GFXcanvas16 bigCv(132, 120);     // shared off-screen canvas: vinyl + knobs (flicker-free)
GFXcanvas16 titleCv(168, 18);    // scrolling song title
Preferences prefs;

#if TFT_BL >= 0
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    inline void blInit()            { ledcAttach(TFT_BL, 5000, 8); }
    inline void blWrite(uint8_t v)  { ledcWrite(TFT_BL, v); }
  #else
    inline void blInit()            { ledcSetup(0, 5000, 8); ledcAttachPin(TFT_BL, 0); }
    inline void blWrite(uint8_t v)  { ledcWrite(0, v); }
  #endif
  const bool HAS_BL = true;
#else
  inline void blInit() {}
  inline void blWrite(uint8_t) {}
  const bool HAS_BL = false;
#endif

// ======================= SETTINGS ==========================
struct Settings {
  uint8_t magic;
  uint8_t volume;        // 0..100  (device master volume)
  int8_t  eq[5];         // -12..+12 dB
  uint8_t bassBoost;     // 0..100
  uint8_t width;         // 0..100 (50 = normal stereo)
  uint8_t loudness, night, mono;
  uint8_t brightness;    // 0..100
  int8_t  preset;        // -1 = custom
  uint8_t buddy;         // 0..3 color
};
constexpr uint8_t SETTINGS_MAGIC = 0xB8;
Settings S = { SETTINGS_MAGIC, 70, {0, 0, 0, 0, 0}, 20, 50, 0, 0, 0, 80, 0, 0 };
bool settingsDirty = false;
uint32_t saveAt = 0;

// ======================= AUDIO ENGINE (DSP) ================
// Call processAudio() on every block of interleaved 16-bit stereo PCM
// that flows from the phone side to the headphone side.
constexpr float FS = 44100.0f;

Biquad filt[6];                         // bass, low, mid, high, air, bass-boost
volatile bool  dspDirty = true;
volatile float audioLevel = 0;          // 0..1, drives Beat
float preamp = 1, master = 0.5f, widthF = 1, nightGain = 1;
bool  monoF = false, nightF = false;

void setPeaking(Biquad &f, float f0, float q, float dB) {
  float A = powf(10, dB / 40), w = 2 * PI * f0 / FS, al = sinf(w) / (2 * q), c = cosf(w);
  f.set(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}
void setLowShelf(Biquad &f, float f0, float dB) {
  float A = powf(10, dB / 40), w = 2 * PI * f0 / FS, c = cosf(w), s = sinf(w);
  float al = s / 1.41421356f, t = 2 * sqrtf(A) * al;
  f.set(A * ((A + 1) - (A - 1) * c + t), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - t),
        (A + 1) + (A - 1) * c + t, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - t);
}
void setHighShelf(Biquad &f, float f0, float dB) {
  float A = powf(10, dB / 40), w = 2 * PI * f0 / FS, c = cosf(w), s = sinf(w);
  float al = s / 1.41421356f, t = 2 * sqrtf(A) * al;
  f.set(A * ((A + 1) + (A - 1) * c + t), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - t),
        (A + 1) - (A - 1) * c + t, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - t);
}

// Fills 6 filters from the current settings (the EQ graph uses this too).
void buildFilters(Biquad *f) {
  float g[5];
  for (int i = 0; i < 5; i++) g[i] = S.eq[i];
  if (S.loudness) { g[0] += 3; g[4] += 2; }
  setLowShelf(f[0], 80, g[0]);
  setPeaking(f[1], 250, 1.0f, g[1]);
  setPeaking(f[2], 1000, 1.0f, g[2]);
  setPeaking(f[3], 4000, 1.0f, g[3]);
  setHighShelf(f[4], 12000, g[4]);
  setLowShelf(f[5], 100, S.bassBoost * 0.09f);          // up to +9 dB extra low shelf
}

void dspRecalc() {
  buildFilters(filt);
  float g0 = S.eq[0] + (S.loudness ? 3 : 0) + S.bassBoost * 0.09f;
  float pk = fmaxf(g0, fmaxf(fmaxf((float)S.eq[1], (float)S.eq[2]), fmaxf((float)S.eq[3], S.eq[4] + (S.loudness ? 2.0f : 0.0f))));
  if (pk < 0) pk = 0;
  preamp  = powf(10, -pk / 20);                         // automatic headroom -> no clipping
  float v = S.volume / 100.0f;
  master  = v * v;
  widthF  = S.width / 50.0f;
  monoF   = S.mono;
  nightF  = S.night;
}

inline float softClip(float x) {
  float a = fabsf(x);
  if (a <= 0.8f) return x;
  float y = 0.8f + 0.2f * tanhf((a - 0.8f) * 5.0f);
  return x < 0 ? -y : y;
}

// buf = interleaved L,R int16 samples, frames = number of stereo frames
void processAudio(int16_t *buf, size_t frames) {
  if (dspDirty) { dspDirty = false; dspRecalc(); }
  const float k = 1.0f / 32768.0f;
  float g = preamp * master;
  float ng = nightF ? nightGain * 1.5f : 1.0f;
  float pk = 0, outPk = 0;
  for (size_t n = 0; n < frames; n++) {
    float l = buf[2 * n] * k, r = buf[2 * n + 1] * k;
    float m = (l + r) * 0.5f, s = (l - r) * 0.5f;
    s = monoF ? 0 : s * widthF;
    l = m + s; r = m - s;
    for (int i = 0; i < 6; i++) { l = filt[i].process(0, l); r = filt[i].process(1, r); }
    l *= g; r *= g;
    pk = fmaxf(pk, fmaxf(fabsf(l), fabsf(r)));
    l = softClip(l * ng); r = softClip(r * ng);
    outPk = fmaxf(outPk, fmaxf(fabsf(l), fabsf(r)));
    buf[2 * n]     = (int16_t)(l * 32767);
    buf[2 * n + 1] = (int16_t)(r * 32767);
  }
  if (nightF) {                                          // gentle night-time compressor
    const float thr = 0.25f;
    float tgt = pk > thr ? powf(thr / pk, 0.6f) : 1.0f;
    nightGain += (tgt - nightGain) * (tgt < nightGain ? 0.6f : 0.08f);
  } else nightGain = 1;
  audioLevel = fmaxf(outPk, audioLevel * 0.9f);
}

// ======================= BLUETOOTH HOOKS ===================
// Fill these in with your A2DP / AVRCP code.
void btPlayPause(bool play) { /* send AVRCP play/pause to the phone */ }
void btNext()               { /* AVRCP next  */ }
void btPrev()               { /* AVRCP prev  */ }

// ======================= APP STATE =========================
enum Page { PG_PLAY, PG_EQ, PG_FX, PG_SETUP };
enum Drag { DRAG_NONE, DRAG_VOL, DRAG_EQ, DRAG_KNOB, DRAG_BRIGHT };

int  page = PG_PLAY;
bool isPlaying = true;
bool phoneOK = true, budsOK = true;
int  progress = 0, totalDuration = 180;

struct Track { const char *title; const char *artist; uint16_t dur; };
Track tracks[] = { {"Linux Beats", "HackyCat", 180}, {"Midnight Boba Rain", "Lo-fi Kitty", 205}, {"Pixel Sunrise", "8-bit Bunny", 160} };
int trackIdx = 0;

const char *BAND_NAME[5] = {"BASS", "LOW", "MID", "HIGH", "AIR"};
const float  BAND_F[5]   = {80, 250, 1000, 4000, 12000};
const char *PRESET_NAME[5] = {"Flat", "Bass", "Vocal", "Treble", "Chill"};
const int8_t PRESETS[5][5] = { {0, 0, 0, 0, 0}, {8, 5, 1, 0, 1}, {-2, -1, 3, 4, 1}, {0, 0, 1, 4, 7}, {3, 2, 0, -2, -3} };
const uint8_t SLEEP_MIN[4] = {0, 15, 30, 60};

int  sleepSel = 0, selBand = 0;
uint32_t sleepDeadline = 0;
uint32_t lastTick = 0, lastVinyl = 0, lastMini = 0, lastMq = 0, lastActivity = 0, lastStatus = 0, boopUntil = 0;
bool dimmed = false, wasDown = false, swallow = false;
Drag dragMode = DRAG_NONE;
int  dragIdx = 0;

float spinDeg = 0, spinSpeed = 0, armT = 0;        // vinyl state
int   mqOff = 0, mqHold = 25, mqW = 0;             // title marquee
char  mqText[40];
uint8_t miniPrev[4] = {0, 0, 0, 0};

// ---- geometry ----
constexpr int VS_X = 62, VS_N = 14, VS_P = 16, VS_BASE = 198;     // volume staircase
constexpr int EQ_X = 16, EQ_W = 288, EQ_Y = 54, EQ_H = 104;       // EQ graph
constexpr float DBPX = 3.5f;                                       // pixels per dB
constexpr int KX[2] = {82, 238}, KY = 80;                          // FX knob centres
constexpr int VIN_X = 4, VIN_Y = 30;                               // vinyl canvas position

float tCos[EQ_W], tSin[EQ_W], tCos2[EQ_W], tSin2[EQ_W];            // EQ graph lookup tables
int   nodeX[5];

// ======================= SMALL HELPERS =====================
void markDirty() { dspDirty = true; settingsDirty = true; saveAt = millis() + 1500; }

void textAt(const char *s, int x, int y, uint8_t sz, uint16_t fg) {
  tft.setTextSize(sz); tft.setTextColor(fg); tft.setCursor(x, y); tft.print(s);
}
void textBg(const char *s, int x, int y, uint8_t sz, uint16_t fg, uint16_t bg) {
  tft.setTextSize(sz); tft.setTextColor(fg, bg); tft.setCursor(x, y); tft.print(s);
}
void textCentered(const char *s, int cx, int y, uint8_t sz, uint16_t fg) {
  textAt(s, cx - (int)strlen(s) * 3 * sz, y, sz, fg);
}
inline bool hit(int x, int y, int w, int h, int px, int py) {
  return px >= x && px <= x + w && py >= y && py <= y + h;
}
inline bool inCircle(int px, int py, int cx, int cy, int r) {
  int dx = px - cx, dy = py - cy;
  return dx * dx + dy * dy <= r * r;
}
void fmtTime(int s, char *out) { sprintf(out, "%d:%02d", s / 60, s % 60); }
int brightnessPWM() { return map(S.brightness, 0, 100, 25, 255); }

// push (part of) an off-screen canvas to the screen in one fast burst
void pushCanvas(GFXcanvas16 &c, int x, int y, int w, int h) {
  uint16_t *b = c.getBuffer();
  int stride = c.width();
  tft.startWrite();
  tft.setAddrWindow(x, y, w, h);
  if (w == stride) tft.writePixels(b, (uint32_t)w * h);
  else for (int r = 0; r < h; r++) tft.writePixels(b + r * stride, w);
  tft.endWrite();
}

void arcLine(GFXcanvas16 &c, int cx, int cy, int r, float a0, float a1, uint16_t col) {
  for (float a = a0; a < a1; a += 6) {
    float b = fminf(a + 6, a1);
    c.drawLine(cx + (int)lroundf(r * cosf(a * D2R)), cy + (int)lroundf(r * sinf(a * D2R)),
               cx + (int)lroundf(r * cosf(b * D2R)), cy + (int)lroundf(r * sinf(b * D2R)), col);
  }
}

void heart(GFXcanvas16 &c, int x, int y, uint16_t col) {
  c.fillCircle(x - 2, y, 2, col); c.fillCircle(x + 2, y, 2, col);
  c.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 6, col);
}

// ---------- transport icons ----------
void iconPlay(int cx, int cy, uint16_t c)  { tft.fillTriangle(cx - 6, cy - 11, cx - 6, cy + 11, cx + 11, cy, c); }
void iconPause(int cx, int cy, uint16_t c) { tft.fillRoundRect(cx - 9, cy - 11, 6, 22, 2, c); tft.fillRoundRect(cx + 3, cy - 11, 6, 22, 2, c); }
void iconNext(int cx, int cy, uint16_t c)  { tft.fillTriangle(cx - 8, cy - 8, cx - 8, cy + 8, cx + 3, cy, c); tft.fillRoundRect(cx + 5, cy - 8, 4, 16, 1, c); }
void iconPrev(int cx, int cy, uint16_t c)  { tft.fillTriangle(cx + 8, cy - 8, cx + 8, cy + 8, cx - 3, cy, c); tft.fillRoundRect(cx - 9, cy - 8, 4, 16, 1, c); }

// ---------- dock icons ----------
void iconRecord(int cx, int cy, uint16_t c) { tft.drawCircle(cx, cy, 8, c); tft.drawCircle(cx, cy, 7, c); tft.fillCircle(cx, cy, 2, c); }
void iconSliders(int cx, int cy, uint16_t c) {
  for (int k = -1; k <= 1; k++) {
    int x = cx + k * 6;
    tft.fillRect(x - 1, cy - 8, 2, 16, c);
    tft.fillCircle(x, cy + (k == 0 ? 3 : (k < 0 ? -3 : -1)), 3, c);
  }
}
void iconSpark(int cx, int cy, uint16_t c) {
  tft.fillTriangle(cx, cy - 9, cx - 3, cy, cx + 3, cy, c);
  tft.fillTriangle(cx, cy + 9, cx - 3, cy, cx + 3, cy, c);
  tft.fillTriangle(cx - 9, cy, cx, cy - 3, cx, cy + 3, c);
  tft.fillTriangle(cx + 9, cy, cx, cy - 3, cx, cy + 3, c);
}
void iconGear(int cx, int cy, uint16_t c) {
  for (int a = 0; a < 360; a += 45) {
    float r = a * D2R;
    int x0 = cx + (int)lroundf(6 * cosf(r)), y0 = cy + (int)lroundf(6 * sinf(r));
    int x1 = cx + (int)lroundf(9 * cosf(r)), y1 = cy + (int)lroundf(9 * sinf(r));
    tft.drawLine(x0, y0, x1, y1, c); tft.drawLine(x0 + 1, y0, x1 + 1, y1, c);
  }
  tft.drawCircle(cx, cy, 6, c); tft.drawCircle(cx, cy, 5, c); tft.fillCircle(cx, cy, 2, c);
}
void dockIcon(int i, int cx, int cy, uint16_t c) {
  if (i == 0) iconRecord(cx, cy, c); else if (i == 1) iconSliders(cx, cy, c);
  else if (i == 2) iconSpark(cx, cy, c); else iconGear(cx, cy, c);
}

// ======================= SETTINGS STORAGE ==================
void loadSettings() {
  prefs.begin("beatbuddy", false);
  Settings tmp;
  if (prefs.getBytes("s", &tmp, sizeof(tmp)) == sizeof(tmp) && tmp.magic == SETTINGS_MAGIC && tmp.buddy < 4) S = tmp;
  ACC = BUDDY[S.buddy];
}
void saveSettings() { prefs.putBytes("s", &S, sizeof(S)); }

// ======================= STATUS BAR & DOCK =================
int readBatteryPct() {
#if BATTERY_PIN >= 0
  float v = analogRead(BATTERY_PIN) / 4095.0f * 3.3f * 2.0f;
  return constrain((int)((v - 3.3f) / (4.2f - 3.3f) * 100), 0, 100);
#else
  return -1;
#endif
}

void drawStatus() {
  tft.fillRect(0, 0, 320, 24, COL_BG);
  textAt("Beat", 10, 4, 2, ACC);
  tft.setTextColor(COL_LAV); tft.print("buddy");
  tft.fillRoundRect(150, 4, 58, 16, 8, COL_CARD);
  tft.fillCircle(160, 12, 3, phoneOK ? COL_MINT : COL_CARD2);
  textAt("PHONE", 168, 8, 1, COL_DIM);
  tft.fillRoundRect(214, 4, 52, 16, 8, COL_CARD);
  tft.fillCircle(224, 12, 3, budsOK ? COL_MINT : COL_CARD2);
  textAt("BUDS", 232, 8, 1, COL_DIM);
  int b = readBatteryPct();
  if (b >= 0) {
    tft.drawRoundRect(274, 6, 28, 12, 3, COL_DIM);
    tft.fillRect(302, 10, 2, 4, COL_DIM);
    tft.fillRoundRect(276, 8, (24 * b) / 100, 8, 2, b > 20 ? COL_MINT : ACC);
  }
}

int dockW(int i) { return i == page ? 104 : 54; }
int dockX(int i) {
  int x = (320 - (104 + 3 * 54)) / 2;
  for (int k = 0; k < i; k++) x += dockW(k);
  return x;
}

void drawDock() {
  tft.fillRect(0, 204, 320, 36, COL_BG);
  tft.fillRoundRect(8, 206, 304, 32, 16, COL_CARD);
  const char *names[4] = {"PLAY", "EQ", "FX", "SETUP"};
  for (int i = 0; i < 4; i++) {
    int x = dockX(i), w = dockW(i), cy = 222;
    if (i == page) {
      tft.fillRoundRect(x + 2, 209, w - 4, 26, 13, ACC);
      int cw = 18 + 6 + (int)strlen(names[i]) * 12;
      int sx = x + (w - cw) / 2;
      dockIcon(i, sx + 9, cy, COL_DARK);
      textAt(names[i], sx + 24, cy - 7, 2, COL_DARK);
    } else dockIcon(i, x + w / 2, cy, COL_DIM);
  }
}

// ======================= PLAY PAGE =========================
// ---- vinyl + Beat ----
void renderVinyl(int px, int py) {
  GFXcanvas16 &c = bigCv;
  const int cx = 58, cy = 62;
  uint32_t now = millis();
  static uint32_t last = 0;
  float dt = (now - last) / 1000.0f; if (dt > 0.2f) dt = 0.2f; last = now;
  float t = now / 1000.0f, lvl = audioLevel;
  bool boop = now < boopUntil, awake = isPlaying;
  bool excited = boop || (awake && lvl > 0.55f);

  spinSpeed += ((awake ? 170.0f + lvl * 60.0f : 0.0f) - spinSpeed) * fminf(1.0f, dt * 3.0f);
  spinDeg = fmodf(spinDeg + spinSpeed * dt, 360.0f);
  armT += ((awake ? 1.0f : 0.0f) - armT) * fminf(1.0f, dt * 5.0f);

  c.fillScreen(COL_BG);
  // record
  c.fillCircle(cx, cy, 54, C565(8, 7, 18));
  c.drawCircle(cx, cy, 54, COL_CARD2);
  const int rings[5] = {51, 46, 41, 36, 31};
  for (int i = 0; i < 5; i++) c.drawCircle(cx, cy, rings[i], C565(26, 24, 46));
  uint16_t sheen = lerp565(C565(110, 104, 170), ACC, 110);
  for (int i = 0; i < 4; i++) {
    float a0 = spinDeg + i * 37.0f;
    arcLine(c, cx, cy, rings[i], a0, a0 + 34, i < 2 ? sheen : lerp565(sheen, COL_BG, 110));
    arcLine(c, cx, cy, rings[i], a0 + 180, a0 + 214, i < 2 ? sheen : lerp565(sheen, COL_BG, 110));
  }
  c.fillCircle(cx + (int)lroundf(47 * cosf(spinDeg * D2R)), cy + (int)lroundf(47 * sinf(spinDeg * D2R)), 2, COL_MINT);

  // Beat the cat = the record label
  uint16_t body = awake ? ACC : lerp565(ACC, C565(110, 104, 160), 150);
  uint16_t cups = (S.buddy == 1) ? COL_LAV : COL_MINT;
  int fx = cx;
  int fy = cy + (awake ? -(int)(fabsf(sinf(t * 7.0f)) * (1 + lvl * 4)) : (int)(sinf(t * 2.0f) * 1.2f));
  if (boop) fy -= 2;
  if (awake || boop) {
    c.fillTriangle(fx - 21, fy - 6, fx - 7, fy - 21, fx - 23, fy - 31, body);
    c.fillTriangle(fx + 21, fy - 6, fx + 7, fy - 21, fx + 23, fy - 31, body);
    c.fillTriangle(fx - 18, fy - 12, fx - 11, fy - 18, fx - 21, fy - 25, COL_CHEEK);
    c.fillTriangle(fx + 18, fy - 12, fx + 11, fy - 18, fx + 21, fy - 25, COL_CHEEK);
  } else {
    c.fillTriangle(fx - 21, fy - 6, fx - 7, fy - 21, fx - 34, fy - 18, body);
    c.fillTriangle(fx + 21, fy - 6, fx + 7, fy - 21, fx + 34, fy - 18, body);
  }
  c.fillCircle(fx, fy, 22, body);
  c.drawCircle(fx, fy, 22, lerp565(body, COL_DARK, 90));
  c.fillRoundRect(fx - 27, fy - 6, 7, 14, 3, cups);
  c.fillRoundRect(fx + 20, fy - 6, 7, 14, 3, cups);

  int ey = fy - 3;
  for (int s = -1; s <= 1; s += 2) {
    int ex = fx + s * 8;
    if (!awake && !boop) {                                   // sleepy  u u
      c.drawLine(ex - 3, ey, ex, ey + 2, COL_DARK); c.drawLine(ex, ey + 2, ex + 3, ey, COL_DARK);
    } else if (excited) {                                    // happy  ^ ^
      c.drawLine(ex - 3, ey + 2, ex, ey - 1, COL_DARK); c.drawLine(ex, ey - 1, ex + 3, ey + 2, COL_DARK);
    } else if ((now % 3200) < 120) {                         // blink
      c.drawFastHLine(ex - 3, ey + 1, 7, COL_DARK);
    } else {
      c.fillCircle(ex, ey + 1, 3, COL_DARK);
      c.drawPixel(ex - 1, ey, COL_TEXT);
    }
    c.fillCircle(fx + s * 13, fy + 5, 3, COL_CHEEK);
  }
  c.fillRect(fx - 1, fy + 2, 3, 2, COL_DARK);
  if (excited) c.fillCircle(fx, fy + 8, 3, C565(120, 30, 60));
  else {
    c.drawLine(fx - 4, fy + 5, fx - 2, fy + 8, COL_DARK); c.drawLine(fx - 2, fy + 8, fx, fy + 5, COL_DARK);
    c.drawLine(fx, fy + 5, fx + 2, fy + 8, COL_DARK);     c.drawLine(fx + 2, fy + 8, fx + 4, fy + 5, COL_DARK);
  }

  // extras: z's / notes / hearts
  if (!awake && !boop) {
    c.setTextSize(1); c.setTextColor(COL_DIM);
    c.setCursor(fx + 26, fy - 26 + (int)(sinf(t * 2) * 2)); c.print("z");
    c.setTextSize(2);
    c.setCursor(fx + 34, fy - 42 + (int)(sinf(t * 2 + 1) * 2)); c.print("Z");
  } else if (awake) {
    for (int k = 0; k < 2; k++) {
      float ph = fmodf(t * 0.7f + k * 0.5f, 1.0f);
      int nx = 6 + k * 9, ny = 116 - (int)(ph * 26);
      uint16_t nc = k ? COL_MINT : COL_YEL;
      c.fillCircle(nx, ny, 2, nc); c.drawFastVLine(nx + 2, ny - 7, 7, nc);
    }
  }
  if (boop) {
    heart(c, fx - 30, fy - 30 - (int)(fmodf(t * 1.8f, 1.0f) * 16), COL_PINK);
    heart(c, fx + 30, fy - 38 - (int)(fmodf(t * 1.8f + 0.5f, 1.0f) * 14), COL_PINK);
  }

  // tonearm
  const float pX = 122, pY = 16, L = 62;
  float ang = (92.0f + 27.0f * armT) * D2R;
  float dx = cosf(ang), dy = sinf(ang);
  float ex = pX + L * dx, ey2 = pY + L * dy;
  for (int k = -1; k <= 1; k++)
    c.drawLine((int)lroundf(pX - k * dy), (int)lroundf(pY + k * dx), (int)lroundf(ex - k * dy), (int)lroundf(ey2 + k * dx), C565(205, 200, 235));
  c.fillCircle((int)lroundf(pX - 14 * dx), (int)lroundf(pY - 14 * dy), 4, COL_DIM);
  c.fillCircle((int)pX, (int)pY, 8, COL_CARD2);
  c.fillCircle((int)pX, (int)pY, 3, COL_DIM);
  c.fillCircle((int)lroundf(ex), (int)lroundf(ey2), 4, ACC);
  c.fillCircle((int)lroundf(ex), (int)lroundf(ey2), 1, COL_DARK);

  pushCanvas(c, px, py, 132, 120);
}

// ---- title marquee ----
void titleRender() {
  GFXcanvas16 &c = titleCv;
  c.fillScreen(COL_BG);
  c.setTextWrap(false); c.setTextSize(2); c.setTextColor(COL_TEXT);
  if (mqW <= 168) { c.setCursor(0, 1); c.print(mqText); }
  else {
    c.setCursor(-mqOff, 1);            c.print(mqText);
    c.setCursor(-mqOff + mqW + 36, 1); c.print(mqText);
  }
  pushCanvas(c, 146, 44, 168, 18);
}

void drawArtist() {
  char buf[24];
  const char *a = tracks[trackIdx].artist;
  if (strlen(a) > 14) snprintf(buf, sizeof(buf), "%.12s..", a); else strcpy(buf, a);
  tft.fillRect(146, 64, 170, 18, COL_BG);
  textAt(buf, 146, 66, 2, COL_LAV);
}

void drawMiniEq() {                                       // tiny live equalizer next to "NOW PLAYING"
  float t = millis() / 1000.0f;
  for (int i = 0; i < 4; i++) {
    float h = isPlaying ? 2 + audioLevel * 16.0f * (0.55f + 0.45f * sinf(t * (6 + i * 2.3f) + i * 1.7f)) : 2;
    int hh = constrain((int)h, 2, 12), x = 288 + i * 7;
    if (hh > miniPrev[i])      tft.fillRect(x, 40 - hh, 5, hh - miniPrev[i], ACC);
    else if (hh < miniPrev[i]) tft.fillRect(x, 40 - miniPrev[i], 5, miniPrev[i] - hh, COL_BG);
    miniPrev[i] = hh;
  }
}

void drawProgress() {
  tft.fillRect(146, 88, 172, 28, COL_BG);
  tft.fillRoundRect(146, 94, 166, 6, 3, COL_CARD2);
  int fill = (166 * progress) / max(1, totalDuration);
  if (fill > 0) tft.fillRoundRect(146, 94, fill, 6, 3, ACC);
  tft.fillCircle(146 + fill, 97, 5, COL_TEXT);
  char a[8], b[8];
  fmtTime(progress, a); fmtTime(totalDuration, b);
  textAt(a, 146, 107, 1, COL_DIM);
  textAt(b, 312 - (int)strlen(b) * 6, 107, 1, COL_DIM);
}

void drawPlayBtn() {
  tft.fillCircle(229, 142, 28, COL_BG);
  tft.drawCircle(229, 142, 28, COL_CARD2);
  tft.fillCircle(229, 142, 24, isPlaying ? ACC : COL_MINT);
  if (isPlaying) iconPause(229, 142, COL_DARK); else iconPlay(229, 142, COL_DARK);
}

uint16_t segColor(int i) {
  return i < 8 ? lerp565(COL_MINT, COL_YEL, i * 255 / 7) : lerp565(COL_YEL, COL_PINK, (i - 7) * 255 / 6);
}

void drawVolume() {                                       // "volume staircase": each step is a rounded bar
  int lit = (S.volume * VS_N + 50) / 100;
  for (int i = 0; i < VS_N; i++) {
    int h = 6 + (i * 16) / (VS_N - 1);
    tft.fillRoundRect(VS_X + i * VS_P, VS_BASE - h, 12, h, 3, i < lit ? segColor(i) : COL_CARD2);
  }
  char b[8]; sprintf(b, "%-3d", S.volume);
  textBg(b, 10, 182, 2, COL_TEXT, COL_BG);
}

void drawTransport() {
  tft.fillCircle(170, 142, 17, COL_CARD2); iconPrev(170, 142, COL_TEXT);
  drawPlayBtn();
  tft.fillCircle(288, 142, 17, COL_CARD2); iconNext(288, 142, COL_TEXT);
}

void setTrack(int i) {
  trackIdx = (i + 3) % 3;
  progress = 0;
  totalDuration = tracks[trackIdx].dur;
  lastTick = millis();
  snprintf(mqText, sizeof(mqText), "%s", tracks[trackIdx].title);
  mqW = strlen(mqText) * 12; mqOff = 0; mqHold = 25;
  if (page == PG_PLAY) { titleRender(); drawArtist(); drawProgress(); }
}

void drawPlayPage() {
  textAt("NOW PLAYING", 146, 30, 1, COL_DIM);
  for (int i = 0; i < 4; i++) miniPrev[i] = 0;
  titleRender();
  drawArtist();
  drawProgress();
  drawTransport();
  textAt("VOL", 10, 168, 1, COL_DIM);
  drawVolume();
  renderVinyl(VIN_X, VIN_Y);
}

void togglePlay() {
  isPlaying = !isPlaying;
  btPlayPause(isPlaying);
  lastTick = millis();
  if (page == PG_PLAY) drawPlayBtn();
}

void setVolumeFromX(int x) {
  int v = constrain((x - VS_X) * 100 / (VS_N * VS_P - 4), 0, 100);
  if (v != S.volume) { S.volume = v; drawVolume(); markDirty(); }
}

// ======================= EQ PAGE ===========================
void eqInitTables() {
  for (int c = 0; c < EQ_W; c++) {
    float f = 20.0f * powf(10.0f, 3.0f * c / (EQ_W - 1));
    float w = 2 * PI * f / FS;
    tCos[c] = cosf(w); tSin[c] = sinf(w); tCos2[c] = cosf(2 * w); tSin2[c] = sinf(2 * w);
  }
  for (int i = 0; i < 5; i++) nodeX[i] = EQ_X + (int)(log10f(BAND_F[i] / 20.0f) / 3.0f * (EQ_W - 1) + 0.5f);
}

// Draws the live frequency-response curve + nodes, 16 columns at a time (no flicker, no big buffer).
void drawEQGraph() {
  Biquad f[6]; buildFilters(f);
  static uint16_t chunk[16 * EQ_H];
  const uint16_t PANEL = C565(20, 18, 42), GRID = C565(34, 31, 64), ZERO = C565(74, 68, 118);
  const int mid = EQ_H / 2;
  int nodeRow[5];
  for (int i = 0; i < 5; i++) nodeRow[i] = mid - (int)lroundf(S.eq[i] * DBPX);
  int vcol[3];
  const float vf[3] = {100, 1000, 10000};
  for (int i = 0; i < 3; i++) vcol[i] = (int)(log10f(vf[i] / 20.0f) / 3.0f * (EQ_W - 1) + 0.5f);

  int prevRow = -1;
  tft.startWrite();
  for (int c0 = 0; c0 < EQ_W; c0 += 16) {
    for (int k = 0; k < 16; k++) {
      int c = c0 + k;
      float prod = 1.0f;
      for (int i = 0; i < 6; i++) {
        float nr = f[i].b0 + f[i].b1 * tCos[c] + f[i].b2 * tCos2[c];
        float ni = -(f[i].b1 * tSin[c] + f[i].b2 * tSin2[c]);
        float dr = 1.0f + f[i].a1 * tCos[c] + f[i].a2 * tCos2[c];
        float di = -(f[i].a1 * tSin[c] + f[i].a2 * tSin2[c]);
        prod *= (nr * nr + ni * ni) / (dr * dr + di * di);
      }
      float db = 10.0f * log10f(prod);
      int yc = constrain(mid - (int)lroundf(db * DBPX), 1, EQ_H - 2);
      if (prevRow < 0) prevRow = yc;
      int lo = min(yc, prevRow), hi = max(yc, prevRow);
      prevRow = yc;
      uint16_t lineCol = db > 0.3f ? COL_MINT : (db < -0.3f ? COL_LAV : COL_DIM);
      bool vgrid = (c == vcol[0] || c == vcol[1] || c == vcol[2]);

      for (int row = 0; row < EQ_H; row++) {
        uint16_t col = vgrid ? GRID : PANEL;
        int dm = abs(row - mid);
        if (row == mid) col = ZERO;
        else if (dm == 21 || dm == 42) col = GRID;
        if (yc < mid && row >= yc && row < mid)
          col = lerp565(col, COL_MINT, 100 - (row - yc) * 70 / (mid - yc));
        else if (yc > mid && row > mid && row <= yc)
          col = lerp565(col, COL_LAV, 100 - (yc - row) * 70 / (yc - mid));
        if (row >= lo - 1 && row <= hi + 1)
          col = (row >= lo && row <= hi) ? lerp565(lineCol, COL_TEXT, 90) : lineCol;
        for (int i = 0; i < 5; i++) {                         // nodes
          int r = (i == selBand) ? 10 : 8;
          int dx = EQ_X + c - nodeX[i];
          if (dx < -r || dx > r) continue;
          int dy = row - nodeRow[i], d2 = dx * dx + dy * dy;
          if (d2 > r * r) continue;
          col = (d2 >= (r - 2) * (r - 2)) ? (i == selBand ? ACC : COL_TEXT) : (S.eq[i] < 0 ? COL_LAV : COL_MINT);
        }
        chunk[row * 16 + k] = col;
      }
    }
    tft.setAddrWindow(EQ_X + c0, EQ_Y, 16, EQ_H);
    tft.writePixels(chunk, 16 * EQ_H);
  }
  tft.endWrite();
}

void drawPresets() {
  for (int i = 0; i < 5; i++) {
    int x = 7 + i * 61;
    bool sel = (S.preset == i);
    tft.fillRoundRect(x, 28, 58, 20, 10, sel ? ACC : COL_CARD2);
    textCentered(PRESET_NAME[i], x + 29, 34, 1, sel ? COL_DARK : COL_DIM);
  }
}

void drawEQReadout() {
  tft.fillRoundRect(12, 170, 296, 32, 10, COL_CARD);
  textAt(BAND_NAME[selBand], 24, 178, 2, COL_TEXT);
  char b[16];
  float f = BAND_F[selBand];
  if (f >= 1000) sprintf(b, "%dk Hz", (int)(f / 1000)); else sprintf(b, "%d Hz", (int)f);
  textAt(b, 24 + (int)strlen(BAND_NAME[selBand]) * 12 + 8, 182, 1, COL_DIM);
  int g = S.eq[selBand];
  sprintf(b, "%+d dB", g);
  textAt(b, 164, 178, 2, g > 0 ? COL_MINT : (g < 0 ? COL_LAV : COL_TEXT));
  tft.drawRoundRect(236, 174, 64, 24, 12, ACC);
  textCentered("RESET", 268, 182, 1, ACC);
}

void drawEQPage() {
  drawPresets();
  tft.drawRoundRect(EQ_X - 3, EQ_Y - 3, EQ_W + 6, EQ_H + 6, 6, COL_CARD2);
  drawEQGraph();
  textCentered("100", EQ_X + (int)(log10f(5.0f) / 3.0f * (EQ_W - 1)), 161, 1, COL_DIM);
  textCentered("1k", EQ_X + (int)(log10f(50.0f) / 3.0f * (EQ_W - 1)), 161, 1, COL_DIM);
  textCentered("10k", EQ_X + (int)(log10f(500.0f) / 3.0f * (EQ_W - 1)), 161, 1, COL_DIM);
  drawEQReadout();
}

void applyPreset(int p) {
  for (int i = 0; i < 5; i++) S.eq[i] = PRESETS[p][i];
  S.preset = p;
  drawPresets(); drawEQGraph(); drawEQReadout();
  markDirty();
}

void setEQFromY(int band, int y, bool force) {
  int g = constrain((int)lroundf((EQ_Y + EQ_H / 2 - y) / DBPX), -12, 12);
  bool changed = (g != S.eq[band]);
  if (changed) {
    S.eq[band] = g;
    if (S.preset != -1) { S.preset = -1; drawPresets(); }
    markDirty();
  }
  if (changed || force) { drawEQGraph(); drawEQReadout(); }
}

// ======================= FX PAGE ===========================
void drawKnob(int idx) {
  GFXcanvas16 &c = bigCv;
  const int kc = 48;
  int val = idx == 0 ? S.bassBoost : S.width;
  uint16_t col = idx == 0 ? ACC : COL_LAV;
  c.fillRect(0, 0, 96, 96, COL_BG);
  for (int a = 0; a <= 270; a += 4)
    c.fillCircle(kc + (int)lroundf(40 * cosf((135 + a) * D2R)), kc + (int)lroundf(40 * sinf((135 + a) * D2R)), 3, COL_CARD2);
  int va = val * 270 / 100;
  for (int a = 0; a <= va; a += 4)
    c.fillCircle(kc + (int)lroundf(40 * cosf((135 + a) * D2R)), kc + (int)lroundf(40 * sinf((135 + a) * D2R)), 3, col);
  int ex = kc + (int)lroundf(40 * cosf((135 + va) * D2R)), ey = kc + (int)lroundf(40 * sinf((135 + va) * D2R));
  c.fillCircle(ex, ey, 6, COL_TEXT); c.fillCircle(ex, ey, 3, col);
  c.fillCircle(kc, kc, 27, COL_CARD); c.drawCircle(kc, kc, 27, COL_CARD2);
  char b[8]; sprintf(b, "%d", val);
  c.setTextWrap(false); c.setTextSize(2); c.setTextColor(COL_TEXT);
  c.setCursor(kc - (int)strlen(b) * 6, kc - 12); c.print(b);
  c.setTextSize(1); c.setTextColor(col);
  c.setCursor(kc - 3, kc + 6); c.print("%");
  pushCanvas(c, KX[idx] - 48, KY - 48, 96, 96);
}

int knobValueFromTouch(int cx, int cy, int x, int y) {      // dial angle -> 0..100 (-1 = too close to centre)
  float dx = x - cx, dy = y - cy;
  if (dx * dx + dy * dy < 100) return -1;
  float a = atan2f(dy, dx) / D2R;
  if (a < 0) a += 360;
  if (a < 135) a += 360;
  float v = (a > 405) ? (a < 450 ? 270.0f : 0.0f) : a - 135;
  return (int)lroundf(v * 100 / 270);
}

void drawToggles() {
  const char *names[3] = {"LOUD", "NIGHT", "MONO"};
  bool on[3] = {(bool)S.loudness, (bool)S.night, (bool)S.mono};
  for (int i = 0; i < 3; i++) {
    int x = 14 + i * 100;
    tft.fillRoundRect(x, 154, 92, 32, 16, on[i] ? COL_MINT : COL_CARD2);
    tft.fillCircle(x + 16, 170, 5, on[i] ? COL_DARK : COL_BG);
    textAt(names[i], x + 28, 163, 2, on[i] ? COL_DARK : COL_DIM);
  }
}

void drawFxPage() {
  drawKnob(0); drawKnob(1);
  textCentered("BASS BOOST", KX[0], 132, 1, COL_TEXT);
  textCentered("STEREO WIDTH", KX[1], 132, 1, COL_TEXT);
  drawToggles();
  textCentered("Loud = punchy when quiet   Night = calmer peaks", 160, 192, 1, COL_DIM);
}

void setKnobFromTouch(int i, int x, int y) {
  int v = knobValueFromTouch(KX[i], KY, x, y);
  if (v < 0) return;
  uint8_t &ref = (i == 0) ? S.bassBoost : S.width;
  if (v == ref) return;
  ref = v; drawKnob(i); markDirty();
}

// ======================= SETUP PAGE ========================
void drawConn() {
  for (int i = 0; i < 2; i++) {
    int x = 14 + i * 150;
    bool ok = i == 0 ? phoneOK : budsOK;
    tft.fillRoundRect(x, 30, 142, 40, 14, COL_CARD);
    tft.fillCircle(x + 20, 50, 9, ok ? COL_MINT : COL_CARD2);
    tft.fillCircle(x + 20, 50, 4, COL_CARD);
    textAt(i == 0 ? "PHONE" : "HEADPHONES", x + 36, 35, 1, COL_DIM);
    textAt(ok ? "Linked" : "Waiting", x + 36, 49, 2, COL_TEXT);
  }
}

void drawBright() {
  tft.fillRect(4, 94, 312, 20, COL_BG);
  tft.fillRoundRect(14, 101, 292, 6, 3, COL_CARD2);
  int kx = 14 + (292 * S.brightness) / 100;
  if (kx > 14) tft.fillRoundRect(14, 101, kx - 14, 6, 3, COL_YEL);
  tft.fillCircle(kx, 104, 8, COL_TEXT);
  tft.fillCircle(kx, 104, 4, COL_YEL);
}

void drawSleepChips() {
  const char *n[4] = {"Off", "15m", "30m", "60m"};
  for (int i = 0; i < 4; i++) {
    int x = 14 + i * 74;
    bool sel = i == sleepSel;
    tft.fillRoundRect(x, 138, 68, 24, 12, sel ? COL_LAV : COL_CARD2);
    textCentered(n[i], x + 34, 142, 2, sel ? COL_DARK : COL_DIM);
  }
}

void drawSwatches() {
  for (int i = 0; i < 4; i++) {
    int cx = 186 + i * 34, cy = 181;
    tft.fillCircle(cx, cy, 14, COL_BG);
    tft.fillCircle(cx, cy, 11, BUDDY[i]);
    if (i == S.buddy) { tft.drawCircle(cx, cy, 14, COL_TEXT); tft.drawCircle(cx, cy, 13, COL_TEXT); }
  }
}

void drawSetupPage() {
  drawConn();
  textAt("Brightness", 14, 76, 2, COL_TEXT);
  drawBright();
  textAt("Sleep timer", 14, 118, 2, COL_TEXT);
  drawSleepChips();
  textAt("Beat's color", 14, 173, 2, COL_TEXT);
  drawSwatches();
}

void setBright(int x) {
  int v = constrain((x - 14) * 100 / 292, 0, 100);
  if (v == S.brightness) return;
  S.brightness = v; drawBright(); blWrite(brightnessPWM()); markDirty();
}

void drawPage() {
  tft.fillRect(0, 24, 320, 182, COL_BG);
  switch (page) {
    case PG_PLAY:  drawPlayPage();  break;
    case PG_EQ:    drawEQPage();    break;
    case PG_FX:    drawFxPage();    break;
    case PG_SETUP: drawSetupPage(); break;
  }
  drawDock();
}

void wakeScreen() { dimmed = false; blWrite(brightnessPWM()); }

void playDown(int x, int y) {
  if (inCircle(x, y, 170, 142, 24))      { btPrev(); setTrack(trackIdx - 1); }
  else if (inCircle(x, y, 229, 142, 32)) { togglePlay(); }
  else if (inCircle(x, y, 288, 142, 24)) { btNext(); setTrack(trackIdx + 1); }
  else if (y >= 166 && x >= 52 && x <= 274) { dragMode = DRAG_VOL; setVolumeFromX(x); }
  else if (inCircle(x, y, VIN_X + 58, VIN_Y + 62, 56)) { boopUntil = millis() + 1400; }   // boop the buddy!
}

void eqDown(int x, int y) {
  if (y >= 26 && y <= 49) {
    int i = (x - 7) / 61;
    if (i >= 0 && i < 5 && (x - 7) % 61 < 58) applyPreset(i);
  } else if (y >= 50 && y <= EQ_Y + EQ_H + 8) {
    int best = 0, bd = 9999;
    for (int i = 0; i < 5; i++) { int d = abs(x - nodeX[i]); if (d < bd) { bd = d; best = i; } }
    selBand = best; dragMode = DRAG_EQ; dragIdx = best;
    setEQFromY(best, y, true);
  } else if (hit(236, 174, 64, 24, x, y)) {
    S.eq[selBand] = 0; S.preset = -1;
    drawPresets(); drawEQGraph(); drawEQReadout(); markDirty();
  }
}

void fxDown(int x, int y) {
  for (int i = 0; i < 2; i++)
    if (inCircle(x, y, KX[i], KY, 52)) { dragMode = DRAG_KNOB; dragIdx = i; setKnobFromTouch(i, x, y); return; }
  if (y >= 152 && y <= 188) {
    int i = (x - 14) / 100;
    if (i >= 0 && i < 3 && (x - 14) % 100 < 92) {
      if (i == 0) S.loudness = !S.loudness; else if (i == 1) S.night = !S.night; else S.mono = !S.mono;
      drawToggles(); markDirty();
    }
  }
}

void setupDown(int x, int y) {
  if (y >= 90 && y <= 116) { dragMode = DRAG_BRIGHT; setBright(x); }
  else if (y >= 136 && y <= 164) {
    int i = (x - 14) / 74;
    if (i >= 0 && i < 4 && (x - 14) % 74 < 68) {
      sleepSel = i;
      sleepDeadline = SLEEP_MIN[i] ? millis() + SLEEP_MIN[i] * 60000UL : 0;
      drawSleepChips();
    }
  } else if (y >= 164 && y <= 200) {
    for (int i = 0; i < 4; i++)
      if (inCircle(x, y, 186 + i * 34, 181, 16)) {
        S.buddy = i; ACC = BUDDY[i]; markDirty();
        drawStatus(); drawPage();                             // re-theme everything
      }
  }
}

void onTouchDown(int x, int y) {
  lastActivity = millis();
  if (dimmed) { wakeScreen(); swallow = true; return; }       // first tap just wakes the screen
  swallow = false;
  if (y >= 204) {
    int idx = 3;
    for (int i = 0; i < 4; i++) if (x < dockX(i) + dockW(i)) { idx = i; break; }
    if (idx != page) { page = idx; drawPage(); }
    return;
  }
  switch (page) {
    case PG_PLAY:  playDown(x, y);  break;
    case PG_EQ:    eqDown(x, y);    break;
    case PG_FX:    fxDown(x, y);    break;
    case PG_SETUP: setupDown(x, y); break;
  }
}

void onTouchMove(int x, int y) {
  if (swallow) return;
  lastActivity = millis();
  switch (dragMode) {
    case DRAG_VOL:    setVolumeFromX(x); break;
    case DRAG_EQ:     setEQFromY(dragIdx, y, false); break;
    case DRAG_KNOB:   setKnobFromTouch(dragIdx, x, y); break;
    case DRAG_BRIGHT: setBright(x); break;
    default: break;
  }
}

void onTouchUp() { dragMode = DRAG_NONE; }


void splash() {
  tft.fillScreen(COL_BG);
  bool keep = isPlaying; isPlaying = true; audioLevel = 0.3f;
  textAt("Beat", 79, 160, 3, ACC);
  tft.setTextColor(COL_LAV); tft.print("buddy");
  textCentered("your little audio buddy", 160, 192, 1, COL_DIM);
  for (int i = 0; i < 24; i++) { renderVinyl(94, 30); delay(40); }
  isPlaying = keep; audioLevel = 0; spinSpeed = 0; armT = 0;
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  tft.begin();
  tft.setRotation(3);
  blInit();
  loadSettings();
  blWrite(brightnessPWM());

  if (!ctouch.begin(40)) {
    Serial.println("Touch controller failure.");
    tft.fillScreen(COL_BG);
    textCentered("Touch panel not found :(", 160, 110, 2, ACC);
    while (1) delay(1000);
  }

  eqInitTables();
  dspRecalc();
  splash();
  tft.fillScreen(COL_BG);
  setTrack(0);
  drawStatus();
  drawPage();
  lastActivity = lastTick = millis();
}

void loop() {
  uint32_t now = millis();

  // ---- touch ----
  bool down = ctouch.touched();
  if (down) {
    TS_Point p = ctouch.getPoint();
    int x = constrain(320 - p.y, 0, 319), y = constrain((int)p.x, 0, 239);
    if (!wasDown) onTouchDown(x, y); else onTouchMove(x, y);
  } else if (wasDown) onTouchUp();
  wasDown = down;

  if (isPlaying && now - lastTick >= 1000) {
    lastTick += 1000;
    progress++;
    if (progress >= totalDuration) setTrack(trackIdx + 1);
    else if (page == PG_PLAY) drawProgress();
  }


#if DEMO_MODE
  {
    float ph = (now % 500) / 500.0f;
    float target = isPlaying ? (0.25f + 0.6f * expf(-ph * 5.0f)) * (0.6f + S.bassBoost / 250.0f) : 0;
    audioLevel = audioLevel + (target - audioLevel) * 0.5f;
  }
#else
  if (!isPlaying) audioLevel = audioLevel * 0.85f;
#endif

  
  if (page == PG_PLAY) {
    bool moving = isPlaying || spinSpeed > 3 || fabsf(armT - (isPlaying ? 1.0f : 0.0f)) > 0.02f || now < boopUntil;
    if (now - lastVinyl >= (moving ? 40u : 120u)) { lastVinyl = now; renderVinyl(VIN_X, VIN_Y); }
    if (now - lastMini >= 70) { lastMini = now; drawMiniEq(); }
    if (mqW > 168 && isPlaying && now - lastMq >= 45) {
      lastMq = now;
      if (mqHold > 0) mqHold--;
      else { mqOff += 2; if (mqOff >= mqW + 36) { mqOff = 0; mqHold = 25; } }
      titleRender();
    }
  }

  if (sleepDeadline && now >= sleepDeadline) {
    sleepDeadline = 0; sleepSel = 0;
    if (isPlaying) togglePlay();
    if (page == PG_SETUP) drawSleepChips();
    if (HAS_BL) { dimmed = true; blWrite(10); }
  }


  if (HAS_BL && !dimmed && now - lastActivity > 25000) { dimmed = true; blWrite(20); }

  
  if (now - lastStatus > 30000) { lastStatus = now; drawStatus(); }
  if (settingsDirty && now >= saveAt) { settingsDirty = false; saveSettings(); }

  delay(5);
}
