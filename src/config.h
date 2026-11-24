#pragma once
// All tunable settings live here. Art itself lives in ascii/ and gestures/.

// ---- Display ----------------------------------------------------------------
static const int   BRIGHTNESS   = 200;    // backlight, 0-255
static const int   MARGIN       = 12;     // px of black kept free on every side

// ---- Global ASCII adjustments (applied to every piece and gesture) -----------
static const bool  INVERT       = false;  // true: swap light and dark
static const float ART_BRIGHT   = 0.0f;   // -1.0 .. 1.0, added after contrast
static const float ART_CONTRAST = 1.0f;   // 1.0 = unchanged, >1 more contrast
static const float ART_GAMMA    = 1.0f;   // >1 darkens mid-tones, <1 lightens
static const uint8_t TINT_R = 255, TINT_G = 255, TINT_B = 255;  // 255,255,255 = grayscale
static const float CELL_ASPECT  = 2.0f;   // character cell height / width
static const float SPEED        = 1.0f;   // animation speed multiplier (2.0 = twice as fast)

// Brightness of each character, 0-255, before the adjustments above.
// Characters not listed are black.
struct RampEntry { char c; uint8_t v; };
static const RampEntry RAMP[] = {
  {'@', 255}, {'%', 235}, {'#', 210}, {'*', 170}, {'+', 140},
  {'=', 110}, {'-', 85},  {':', 55},  {'.', 30},
};

// ---- Gestures ---------------------------------------------------------------
static const bool     SHAKE_ENABLED  = true;
static const float    SHAKE_G        = 1.2f;   // |acceleration - 1g| that counts as a jolt
static const int      SHAKE_JOLTS    = 4;      // jolts needed...
static const uint32_t SHAKE_WINDOW   = 900;    // ...within this many ms
static const int      SHAKE_LOOPS    = 3;      // times the shake animation plays through
static const uint32_t SHAKE_COOLDOWN = 1500;   // ms before another shake can trigger
