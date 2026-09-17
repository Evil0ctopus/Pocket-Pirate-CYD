#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>

// Small shared color / drawing helpers so every screen shades consistently.
namespace gfxu {

// Compile-time RGB888 -> RGB565 so palettes can be written as plain numbers.
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Blend two RGB565 colors. t = 0..255 is the amount of `b`.
inline uint16_t blend565(uint16_t a, uint16_t b, uint8_t t) {
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = (ar * (255 - t) + br * t) / 255;
  int g = (ag * (255 - t) + bg * t) / 255;
  int bl = (ab * (255 - t) + bb * t) / 255;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

inline uint16_t lighten(uint16_t c, uint8_t amt) {
  return blend565(c, 0xFFFF, amt);
}
inline uint16_t darken(uint16_t c, uint8_t amt) {
  return blend565(c, 0x0000, amt);
}

// Vertical gradient fill from top color to bottom color.
inline void vGradient(lgfx::LGFXBase& g, int x, int y, int w, int h,
                      uint16_t top, uint16_t bot) {
  if (h <= 0) return;
  for (int i = 0; i < h; i++) {
    uint8_t t = (h <= 1) ? 0 : (uint8_t)(i * 255 / (h - 1));
    g.drawFastHLine(x, y + i, w, blend565(top, bot, t));
  }
}

}  // namespace gfxu
