// ASCII art viewer. Art comes from ascii/*.json (tap = next) and
// gestures/shake/*.json (played on shake); tools/embed_art.py embeds them at
// build time. All settings are in config.h.
#include <Arduino.h>
#include <math.h>
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

// -------------------------------------------------------------------- main
static bool imuOk = false;

void setup() {
  Serial.begin(115200);
  lcd.init();
  lcd.setRotation(0);
  lcd.setBrightness(BRIGHTNESS);
  lcd.fillScreen(TFT_BLACK);
  buildLut();

  if (ASCII_COUNT > 0) load(cur, ASCII_PIECES[0]);
  if (SHAKE_ENABLED && SHAKE_COUNT > 0) {
    load(gest, SHAKE_PIECES[0]);
    imuOk = imuInit();
    Serial.println(imuOk ? "shake: IMU ready" : "shake: IMU not found, gesture disabled");
  }
}

void loop() {
  static int piece = 0, shakeIdx = 0, frame = 0, loops = 0;
  static bool gesturing = false, wasDown = false;
  static uint32_t next = 0, cooldownUntil = 0;
  uint32_t now = millis();

  if (!gesturing) {
    // Tap (finger-down edge) -> next piece.
    int32_t tx, ty;
    bool down = lcd.getTouch(&tx, &ty);
    if (down && !wasDown && ASCII_COUNT > 1) {
      piece = (piece + 1) % ASCII_COUNT;
      load(cur, ASCII_PIECES[piece]);
      lcd.fillScreen(TFT_BLACK);
      frame = 0;
      next = 0;
    }
    wasDown = down;

    // Shake -> play the next gesture animation.
    if (imuOk && gest.piece && now >= cooldownUntil && detectShake(now)) {
      if (SHAKE_COUNT > 1) {  // several gesture files: take them in turn
        shakeIdx = (shakeIdx + 1) % SHAKE_COUNT;
        load(gest, SHAKE_PIECES[shakeIdx]);
      }
      gesturing = true;
      frame = 0;
      loops = 0;
      next = 0;
      lcd.fillScreen(TFT_BLACK);
    }
  }

  const Loaded& L = gesturing ? gest : cur;
  if (!L.piece || L.count == 0) { delay(10); return; }
  if (now < next) { delay(2); return; }
  show(L, frame);
  next = now + max((uint32_t)1, (uint32_t)(L.piece->frames[frame].ms / SPEED));  // no catch-up bursts

  if (++frame >= L.count) {
    frame = 0;
    if (gesturing && ++loops >= SHAKE_LOOPS) {  // gesture done: back to the piece
      gesturing = false;
      cooldownUntil = now + SHAKE_COOLDOWN;
      lcd.fillScreen(TFT_BLACK);
      next = 0;
    }
  }
}
