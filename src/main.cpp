// ASCII art viewer. Art comes from ascii/*.json (tap = next) and
// gestures/shake/*.json (played on shake); tools/embed_art.py embeds them at
// build time. All settings are in config.h.
#include <Arduino.h>
#include <math.h>
#include <Preferences.h>
#include "LGFX_Config.h"
#include "config.h"
#include "art_data.h"

static LGFX lcd;

// ------------------------------------------------------------ brightness map
static uint8_t lut[256];  // character -> final brightness, after global adjustments

static void buildLut() {
  uint8_t base[256] = {0};
  for (const RampEntry& e : RAMP) base[(uint8_t)e.c] = e.v;
  for (int i = 0; i < 256; i++) {
    float x = base[i] / 255.0f;
    x = (x - 0.5f) * ART_CONTRAST + 0.5f + ART_BRIGHT;
    x = constrain(x, 0.0f, 1.0f);
    x = powf(x, ART_GAMMA);
    if (INVERT) x = 1.0f - x;
    lut[i] = (uint8_t)(x * 255.0f + 0.5f);
  }
}

static inline uint16_t shade(uint8_t v) {
  return lcd.color565(v * TINT_R / 255, v * TINT_G / 255, v * TINT_B / 255);
}

// ---------------------------------------------------------------- rendering
static const int MAX_FRAMES = 32;

// A piece with every frame pre-scaled to RGB565 in PSRAM, plus its screen rect.
struct Loaded {
  const Piece* piece = nullptr;
  uint16_t* buf[MAX_FRAMES] = {};
  int count = 0, dw = 0, dh = 0, ox = 0, oy = 0;
};
static Loaded cur, gest;

// Fills one frame buffer from the piece's cols x rows grid. Text outside the
// grid is cropped, missing text is black; larger art is averaged down.
static bool renderFrame(const Loaded& L, const char* art, uint16_t* out) {
  const int rows = L.piece->rows, cols = L.piece->cols;
  uint8_t* g = (uint8_t*)ps_malloc((size_t)rows * cols);
  if (!g) return false;
  memset(g, lut[(uint8_t)' '], (size_t)rows * cols);
  int r = 0, c = 0;
  for (const char* s = art; *s; s++) {
    if (*s == '\n') { r++; c = 0; }
    else if (*s != '\r') { if (r < rows && c < cols) g[r * cols + c] = lut[(uint8_t)*s]; c++; }
  }
  for (int y = 0; y < L.dh; y++) {
    int r0 = y * rows / L.dh, r1 = max(r0 + 1, (y + 1) * rows / L.dh);
    for (int x = 0; x < L.dw; x++) {
      int c0 = x * cols / L.dw, c1 = max(c0 + 1, (x + 1) * cols / L.dw);
      uint32_t sum = 0;
      for (int rr = r0; rr < r1; rr++)
        for (int cc = c0; cc < c1; cc++) sum += g[rr * cols + cc];
      out[y * L.dw + x] = shade(sum / ((r1 - r0) * (c1 - c0)));
    }
  }
  free(g);
  return true;
}

static void unload(Loaded& L) {
  for (int f = 0; f < L.count; f++) { free(L.buf[f]); L.buf[f] = nullptr; }
  L.count = 0;
  L.piece = nullptr;
}

// Fits the piece inside the margin at its natural aspect ratio, centered.
static void load(Loaded& L, const Piece& p) {
  unload(L);
  if (p.rows <= 0 || p.cols <= 0 || p.count <= 0) return;
  L.piece = &p;
  const int W = lcd.width(), H = lcd.height();
  float cw = min((float)(W - 2 * MARGIN) / p.cols, (float)(H - 2 * MARGIN) / (p.rows * CELL_ASPECT));
  L.dw = max(1, (int)(p.cols * cw));
  L.dh = max(1, (int)(p.rows * cw * CELL_ASPECT));
  L.ox = (W - L.dw) / 2;
  L.oy = (H - L.dh) / 2;
  L.count = min(p.count, MAX_FRAMES);
  for (int f = 0; f < L.count; f++) {
    L.buf[f] = (uint16_t*)ps_malloc((size_t)L.dw * L.dh * 2);
    if (L.buf[f] && !renderFrame(L, p.frames[f].art, L.buf[f])) { free(L.buf[f]); L.buf[f] = nullptr; }
  }
}

// ------------------------------------------------------------------ battery
// Battery percentage from the voltage divider on BATT_ADC_PIN, re-read every 5 s.
static int batteryPercent(uint32_t now) {
  static int pct = -1;
  static uint32_t last = 0;
  if (pct >= 0 && now - last < 5000) return pct;
  last = now;
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(BATT_ADC_PIN);
  float v = mv / 16.0f / 1000.0f * BATT_DIVIDER;
  pct = (int)constrain((v - BATT_EMPTY_V) / (BATT_FULL_V - BATT_EMPTY_V) * 100.0f + 0.5f, 0.0f, 100.0f);
  return pct;
}

static void show(const Loaded& L, int frame) {
  if (!L.buf[frame]) return;
  // Explicit native RGB565 type: a plain uint16_t* is treated as byte-swapped.
  lcd.pushImage(L.ox, L.oy, L.dw, L.dh, (const lgfx::rgb565_t*)L.buf[frame]);
}

// ------------------------------------------------------- shake (QMI8658 IMU)
// The IMU shares the touch controller's I2C bus (port 0, SDA 11, SCL 10).
static const int IMU_PORT = 0;
static int imuAddr = 0;

static bool imuInit() {
  for (int addr : {0x6B, 0x6A}) {
    auto id = lgfx::i2c::readRegister8(IMU_PORT, addr, 0x00);  // WHO_AM_I
    if (id.has_value() && id.value() == 0x05) { imuAddr = addr; break; }
  }
  if (!imuAddr) return false;
  lgfx::i2c::writeRegister8(IMU_PORT, imuAddr, 0x02, 0x40);  // CTRL1: address auto-increment
  lgfx::i2c::writeRegister8(IMU_PORT, imuAddr, 0x03, 0x16);  // CTRL2: accel +-4g, ~112 Hz
  lgfx::i2c::writeRegister8(IMU_PORT, imuAddr, 0x08, 0x01);  // CTRL7: accel on
  return true;
}

// Acceleration magnitude in g, or -1 on read failure.
static float imuReadG() {
  uint8_t d[6];
  if (!lgfx::i2c::readRegister(IMU_PORT, imuAddr, 0x35, d, 6).has_value()) return -1;
  float x = (int16_t)(d[0] | d[1] << 8), y = (int16_t)(d[2] | d[3] << 8), z = (int16_t)(d[4] | d[5] << 8);
  return sqrtf(x * x + y * y + z * z) / 8192.0f;  // 8192 LSB/g at +-4g
}

// True once SHAKE_JOLTS strong jolts land within SHAKE_WINDOW.
static bool detectShake(uint32_t now) {
  static uint32_t last = 0, jolts[16];
  static int n = 0;
  if (now - last < 10) return false;  // ~100 Hz polling
  last = now;
  float g = imuReadG();
  if (g < 0 || fabsf(g - 1.0f) < SHAKE_G) return false;
  if (n > 0 && now - jolts[n - 1] < 80) return false;  // same jolt, still ringing
  // drop jolts that fell out of the window, then record this one
  int k = 0;
  for (int i = 0; i < n; i++) if (now - jolts[i] <= SHAKE_WINDOW) jolts[k++] = jolts[i];
  n = k;
  if (n < 16) jolts[n++] = now;
  if (n >= SHAKE_JOLTS) { n = 0; return true; }
  return false;
}

// ----------------------------------------------------------------- settings
// Slideshow settings survive power cycles (stored in NVS).
static const uint32_t SLIDE_SECONDS[] = {5, 10, 30};
static const int SLIDE_OPTIONS = sizeof SLIDE_SECONDS / sizeof SLIDE_SECONDS[0];
static bool slideshow = false;
static int slideIdx = 0;
static Preferences prefs;

static void loadSettings() {
  prefs.begin("frame", false);
  slideshow = prefs.getBool("slide", false);
  slideIdx = constrain((int)prefs.getUChar("slideIdx", 0), 0, SLIDE_OPTIONS - 1);
}

static void saveSettings() {
  prefs.putBool("slide", slideshow);
  prefs.putUChar("slideIdx", (uint8_t)slideIdx);
}

// ---------------------------------------------------------------- touch input
enum Gesture { G_NONE, G_TAP, G_SWIPE_UP };
static int32_t tapX, tapY;  // where the last G_TAP started

// Reports one gesture per finger lift: a swipe up, a tap, or nothing.
static Gesture pollTouch() {
  static bool wasDown = false;
  static int32_t sx, sy, lx, ly;
  int32_t x, y;
  bool down = lcd.getTouch(&x, &y);
  Gesture g = G_NONE;
  if (down) {
    if (!wasDown) { sx = x; sy = y; }
    lx = x; ly = y;
  } else if (wasDown) {
    int dx = lx - sx, dy = ly - sy;
    if (dy <= -SWIPE_MIN_PX && abs(dx) < -dy) g = G_SWIPE_UP;
    else if (abs(dx) < TAP_SLOP_PX && abs(dy) < TAP_SLOP_PX) { g = G_TAP; tapX = sx; tapY = sy; }
  }
  wasDown = down;
  return g;
}

// ------------------------------------------------------------------ menu UI
// Layout stays inside x 20..220 so the screen's rounded corners don't clip it.
static const int TOG_X = 20, TOG_Y = 58, TOG_W = 200, TOG_H = 52;
static const int BTN_Y = 140, BTN_W = 60, BTN_H = 44;
static const int BTN_X[3] = {20, 90, 160};

static bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void drawMenuBattery(uint32_t now) {
  char s[24];
  snprintf(s, sizeof s, "Battery %d%%", batteryPercent(now));
  lcd.setTextSize(2);
  lcd.setTextDatum(middle_center);
  lcd.setTextPadding(170);  // clears leftovers when the text gets shorter
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.drawString(s, lcd.width() / 2, 215);
  lcd.setTextPadding(0);
}

static void drawMenu(uint32_t now) {
  const uint16_t gray = lcd.color565(140, 140, 140);
  const uint16_t accent = lcd.color565(40, 100, 220);
  const uint16_t togBg = slideshow ? lcd.color565(30, 140, 60) : lcd.color565(60, 60, 60);
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextDatum(middle_center);

  lcd.setTextSize(2);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.drawString("SETTINGS", lcd.width() / 2, 30);

  // Slideshow toggle
  lcd.fillRoundRect(TOG_X, TOG_Y, TOG_W, TOG_H, 12, togBg);
  lcd.setTextColor(TFT_WHITE, togBg);
  lcd.drawString(slideshow ? "Slideshow ON" : "Slideshow OFF", TOG_X + TOG_W / 2, TOG_Y + TOG_H / 2);

  // Interval buttons
  lcd.setTextSize(1);
  lcd.setTextColor(gray, TFT_BLACK);
  lcd.drawString("Change image every", lcd.width() / 2, 124);
  lcd.setTextSize(2);
  for (int i = 0; i < SLIDE_OPTIONS; i++) {
    bool sel = (i == slideIdx);
    uint16_t bg = sel ? (slideshow ? accent : lcd.color565(70, 70, 70)) : lcd.color565(30, 30, 30);
    lcd.fillRoundRect(BTN_X[i], BTN_Y, BTN_W, BTN_H, 10, bg);
    lcd.setTextColor(slideshow || sel ? TFT_WHITE : gray, bg);
    char s[8];
    snprintf(s, sizeof s, "%us", (unsigned)SLIDE_SECONDS[i]);
    lcd.drawString(s, BTN_X[i] + BTN_W / 2, BTN_Y + BTN_H / 2);
  }

  drawMenuBattery(now);

  lcd.setTextSize(1);
  lcd.setTextColor(gray, TFT_BLACK);
  lcd.drawString("swipe up to close", lcd.width() / 2, 250);
}

static void menuTap(int x, int y, uint32_t now) {
  if (inRect(x, y, TOG_X, TOG_Y, TOG_W, TOG_H)) {
    slideshow = !slideshow;
  } else {
    int hit = -1;
    for (int i = 0; i < SLIDE_OPTIONS; i++)
      if (inRect(x, y, BTN_X[i], BTN_Y, BTN_W, BTN_H)) hit = i;
    if (hit < 0) return;
    slideIdx = hit;
  }
  saveSettings();
  drawMenu(now);
}

// -------------------------------------------------------------------- main
static bool imuOk = false;
static bool inMenu = false;
static int piece = 0, frame = 0;
static uint32_t nextFrame = 0, nextSlide = 0;

static void armSlideshow(uint32_t now) { nextSlide = now + SLIDE_SECONDS[slideIdx] * 1000UL; }

static void selectPiece(int p) {
  piece = p;
  load(cur, ASCII_PIECES[piece]);
  lcd.fillScreen(TFT_BLACK);
  frame = 0;
  nextFrame = 0;
}

void setup() {
  pinMode(BATT_HOLD_PIN, OUTPUT);
  digitalWrite(BATT_HOLD_PIN, HIGH);   // hold battery power on; to shut down: digitalWrite(BATT_HOLD_PIN, LOW)
  Serial.begin(115200);
  lcd.init();
  lcd.setRotation(0);
  lcd.setBrightness(BRIGHTNESS);
  lcd.fillScreen(TFT_BLACK);
  buildLut();
  loadSettings();

  if (ASCII_COUNT > 0) load(cur, ASCII_PIECES[0]);
  if (SHAKE_ENABLED && SHAKE_COUNT > 0) {
    load(gest, SHAKE_PIECES[0]);
    imuOk = imuInit();
    Serial.println(imuOk ? "shake: IMU ready" : "shake: IMU not found, gesture disabled");
  }
  armSlideshow(millis());
}

void loop() {
  static int shakeIdx = 0, loops = 0, shownPct = -1;
  static bool gesturing = false;
  static uint32_t cooldownUntil = 0;
  uint32_t now = millis();
  Gesture g = pollTouch();

  // ---- settings menu: swipe up closes it; the art is paused meanwhile
  if (inMenu) {
    if (g == G_SWIPE_UP) {
      inMenu = false;
      lcd.fillScreen(TFT_BLACK);
      nextFrame = 0;
      armSlideshow(now);
      return;
    }
    if (g == G_TAP) menuTap(tapX, tapY, now);
    int pct = batteryPercent(now);
    if (pct != shownPct) { shownPct = pct; drawMenuBattery(now); }
    delay(10);
    return;
  }

  // ---- swipe up in the viewer opens the menu (cancels any gesture in progress)
  if (g == G_SWIPE_UP) {
    inMenu = true;
    gesturing = false;
    frame = 0;
    cooldownUntil = now + SHAKE_COOLDOWN;
    shownPct = batteryPercent(now);
    drawMenu(now);
    return;
  }

  if (!gesturing) {
    // Tap -> next piece.
    if (g == G_TAP && ASCII_COUNT > 1) {
      selectPiece((piece + 1) % ASCII_COUNT);
      armSlideshow(now);
    }

    // Slideshow: advance on a timer, wrapping around after the last piece.
    if (slideshow && ASCII_COUNT > 1 && now >= nextSlide) {
      selectPiece((piece + 1) % ASCII_COUNT);
      armSlideshow(now);
    }

    // Shake -> play the next gesture animation.
    if (imuOk && gest.piece && now >= cooldownUntil && detectShake(now)) {
      if (SHAKE_COUNT > 1) {  // several gesture files: take them in turn
        shakeIdx = (shakeIdx + 1) % SHAKE_COUNT;
        load(gest, SHAKE_PIECES[shakeIdx]);
      }
      gesturing = true;
      frame = 0;
      loops = 0;
      nextFrame = 0;
      lcd.fillScreen(TFT_BLACK);
    }
  }

  const Loaded& L = gesturing ? gest : cur;
  if (!L.piece || L.count == 0) { delay(10); return; }
  if (now < nextFrame) { delay(2); return; }
  show(L, frame);
  nextFrame = now + max((uint32_t)1, (uint32_t)(L.piece->frames[frame].ms / SPEED));  // no catch-up bursts

  if (++frame >= L.count) {
    frame = 0;
    if (gesturing && ++loops >= SHAKE_LOOPS) {  // gesture done: back to the piece
      gesturing = false;
      cooldownUntil = now + SHAKE_COOLDOWN;
      lcd.fillScreen(TFT_BLACK);
      nextFrame = 0;
      armSlideshow(now);
    }
  }
}
