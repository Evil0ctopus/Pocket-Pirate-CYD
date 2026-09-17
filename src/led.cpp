#include "led.h"

#include <Preferences.h>

#include "board_pins.h"

using namespace CheapBlackDisplay;

namespace led {
namespace {

uint8_t g_mode = Beacon;
uint8_t g_color = 0;
uint8_t g_bri = 60;  // percent
uint32_t g_lastApply = 0;

const char* kModeNames[ModeCount] = {"Off", "Solid", "Beacon", "Rainbow",
                                     "Alarm"};
// gold, red, green, blue, teal, purple, white, orange
const uint32_t kColors[kColorCount] = {0xF4CA4E, 0xD23C32, 0x2C9858, 0x2C54A0,
                                       0x1E8480, 0x8C46B4, 0xF0F0E6, 0xE07820};

void persist() {
  Preferences p;
  p.begin("led", false);
  p.putUChar("mode", g_mode);
  p.putUChar("col", g_color);
  p.putUChar("bri", g_bri);
  p.end();
}

void apply(uint32_t now) {
  uint32_t base = kColors[g_color % kColorCount];
  uint8_t r = (base >> 16) & 0xFF, g = (base >> 8) & 0xFF, b = base & 0xFF;
  float scale = g_bri / 100.0f;
  switch (g_mode) {
    case Off:
      r = g = b = 0;
      break;
    case Solid:
      break;
    case Beacon: {  // slow pulse of the chosen color
      float v = (sinf((now % 2400) / 2400.0f * TWO_PI) + 1) * 0.5f;
      scale *= 0.15f + 0.85f * v;
      break;
    }
    case Rainbow: {
      float hh = (now % 3000) / 3000.0f * 6.0f;
      int i = (int)hh;
      float f = hh - i;
      uint8_t q = 255 * (1 - f), t = 255 * f;
      switch (i % 6) {
        case 0: r = 255; g = t; b = 0; break;
        case 1: r = q; g = 255; b = 0; break;
        case 2: r = 0; g = 255; b = t; break;
        case 3: r = 0; g = q; b = 255; break;
        case 4: r = t; g = 0; b = 255; break;
        default: r = 255; g = 0; b = q; break;
      }
      break;
    }
    case Alarm:  // blink the chosen color
      if ((now / 300) % 2 == 0) r = g = b = 0;
      break;
    default:
      break;
  }
  rgbLedWrite(RGB_LED, (uint8_t)(r * scale), (uint8_t)(g * scale),
              (uint8_t)(b * scale));
}

}  // namespace

void begin() {
  Preferences p;
  p.begin("led", true);
  g_mode = p.getUChar("mode", Beacon);
  g_color = p.getUChar("col", 0);
  g_bri = p.getUChar("bri", 60);
  p.end();
  if (g_mode >= ModeCount) g_mode = Beacon;
  if (g_color >= kColorCount) g_color = 0;
  apply(millis());
}

void tick(uint32_t now) {
  if (now - g_lastApply < 40) return;
  g_lastApply = now;
  apply(now);
}

uint8_t mode() { return g_mode; }
void setMode(uint8_t m) {
  g_mode = m % ModeCount;
  persist();
  apply(millis());
}
uint8_t colorIdx() { return g_color; }
void setColorIdx(uint8_t c) {
  g_color = c % kColorCount;
  persist();
  apply(millis());
}
uint8_t brightness() { return g_bri; }
void setBrightness(uint8_t pct) {
  if (pct < 5) pct = 5;
  if (pct > 100) pct = 100;
  g_bri = pct;
  persist();
  apply(millis());
}
const char* modeName(uint8_t m) { return kModeNames[m % ModeCount]; }
uint32_t colorRgb(uint8_t idx) { return kColors[idx % kColorCount]; }

}  // namespace led
