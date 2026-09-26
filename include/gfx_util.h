#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <stdarg.h>
#include <string.h>

#include "pirate_theme.h"

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
// step>1 trades smoothness for speed (world/HUD chrome uses 2).
inline void vGradient(lgfx::LGFXBase& g, int x, int y, int w, int h,
                      uint16_t top, uint16_t bot, int step = 1) {
  if (h <= 0 || w <= 0) return;
  if (step < 1) step = 1;
  for (int i = 0; i < h; i += step) {
    uint8_t t = (h <= 1) ? 0 : (uint8_t)(i * 255 / (h - 1));
    int run = step;
    if (i + run > h) run = h - i;
    if (run == 1) g.drawFastHLine(x, y + i, w, blend565(top, bot, t));
    else g.fillRect(x, y + i, w, run, blend565(top, bot, t));
  }
}

// Clip/ellipsis print so labels never spill past `maxW` pixels (6px/glyph @ size 1).
inline void printFit(lgfx::LGFXBase& g, int x, int y, int maxW, uint16_t fg,
                     uint8_t size, const char* s) {
  if (!s) return;
  g.setTextSize(size);
  g.setTextColor(fg);
  const int gw = 6 * (int)size;
  if (gw <= 0 || maxW < gw) return;
  int maxChars = maxW / gw;
  int n = (int)strlen(s);
  g.setCursor(x, y);
  if (n <= maxChars) {
    g.print(s);
    return;
  }
  if (maxChars <= 2) {
    g.print("..");
    return;
  }
  char buf[48];
  int keep = maxChars - 2;
  if (keep > (int)sizeof(buf) - 3) keep = (int)sizeof(buf) - 3;
  memcpy(buf, s, keep);
  buf[keep] = '.';
  buf[keep + 1] = '.';
  buf[keep + 2] = 0;
  g.print(buf);
}

// Center a label inside a box (uses 6px glyph width at size 1).
inline void printCentered(lgfx::LGFXBase& g, int x, int y, int w, int h,
                          uint16_t fg, uint8_t size, const char* s) {
  if (!s) return;
  g.setTextSize(size);
  g.setTextColor(fg);
  int tw = (int)strlen(s) * 6 * (int)size;
  int th = 8 * (int)size;
  g.setCursor(x + (w - tw) / 2, y + (h - th) / 2);
  g.print(s);
}

// Soft rounded card with optional left accent bar and subtle edge.
inline void drawCard(lgfx::LGFXBase& g, int x, int y, int w, int h,
                     uint16_t fill = theme::kPanel, uint16_t accent = 0,
                     int radius = theme::kRadiusCard) {
  g.fillRoundRect(x, y, w, h, radius, fill);
  g.drawRoundRect(x, y, w, h, radius, theme::kBorder);
  if (accent) {
    int aw = 4;
    g.fillRoundRect(x, y, aw + 2, h, radius, accent);
    g.fillRect(x + aw, y + 1, 2, h - 2, fill);  // clean inner edge
  }
}

// Content body panel (station screens). Solid fill (fast); thin top highlight.
inline void drawBody(lgfx::LGFXBase& g, int x, int y, int w, int h) {
  g.fillRect(x, y, w, h, theme::kBg);
  g.fillRect(x, y, w, 3, theme::kPanel);
  g.drawFastHLine(x, y, w, theme::kBorderHi);
}

// Primary CTA / secondary button.
inline void drawButton(lgfx::LGFXBase& g, int x, int y, int w, int h,
                       const char* label, bool primary = true,
                       uint16_t fillOverride = 0) {
  uint16_t fill = fillOverride
                      ? fillOverride
                      : (primary ? theme::kGold : theme::kPanelHi);
  uint16_t fg = primary ? theme::kOnGold : theme::kInk;
  g.fillRoundRect(x, y, w, h, theme::kRadiusBtn, fill);
  if (!primary) g.drawRoundRect(x, y, w, h, theme::kRadiusBtn, theme::kBorder);
  printCentered(g, x, y, w, h, fg, 1, label);
}

// Sort / filter chip (pill).
inline void drawChip(lgfx::LGFXBase& g, int x, int y, int w, int h,
                     const char* label, bool on, uint16_t onColor = theme::kGold) {
  uint16_t fill = on ? onColor : theme::kPanelSoft;
  uint16_t fg = on ? theme::kOnGold : theme::kInkDim;
  g.fillRoundRect(x, y, w, h, theme::kRadiusChip, fill);
  if (!on) g.drawRoundRect(x, y, w, h, theme::kRadiusChip, theme::kBorder);
  printCentered(g, x, y, w, h, fg, 1, label);
}

inline void drawSeparator(lgfx::LGFXBase& g, int x, int y, int w) {
  g.drawFastHLine(x, y, w, theme::kBorder);
  g.drawFastHLine(x, y + 1, w, darken(theme::kBgDeep, 40));
}

// Structured list row: optional zebra, primary + secondary labels, right meta.
inline void drawListRow(lgfx::LGFXBase& g, int x, int y, int w, int h, int index,
                        const char* primary, const char* secondary,
                        const char* meta, uint16_t primaryColor = theme::kInk,
                        uint16_t metaColor = theme::kInkDim) {
  if (index & 1) g.fillRect(x, y, w, h, theme::kPanelSoft);
  g.setTextSize(1);
  g.setTextColor(primaryColor);
  g.setCursor(x + 6, y + 2);
  g.print(primary);
  if (secondary && secondary[0]) {
    g.setTextColor(theme::kInkMuted);
    g.setCursor(x + 6, y + 10);
    g.print(secondary);
  }
  if (meta && meta[0]) {
    int mw = (int)strlen(meta) * 6;
    g.setTextColor(metaColor);
    g.setCursor(x + w - mw - 6, y + (secondary && secondary[0] ? 2 : (h - 8) / 2));
    g.print(meta);
  }
  g.drawFastHLine(x + 4, y + h - 1, w - 8, theme::kBorder);
}

// Setting / info row with left label and optional right value.
inline void drawKVRow(lgfx::LGFXBase& g, int x, int y, int w, int h,
                      const char* label, const char* value,
                      uint16_t valueColor = theme::kInk) {
  g.fillRoundRect(x, y, w, h, 5, theme::kPanelSoft);
  g.setTextSize(1);
  g.setTextColor(theme::kInkDim);
  g.setCursor(x + 8, y + (h - 8) / 2);
  g.print(label);
  if (value && value[0]) {
    int vw = (int)strlen(value) * 6;
    g.setTextColor(valueColor);
    g.setCursor(x + w - vw - 8, y + (h - 8) / 2);
    g.print(value);
  }
}

// Tiny station glyph inside a circle (vector fallbacks — no SD art needed).
inline void drawGlyph(lgfx::LGFXBase& g, int cx, int cy, int r, int kind,
                      uint16_t accent) {
  g.fillCircle(cx, cy, r, theme::kPanelSoft);
  g.drawCircle(cx, cy, r, accent);
  uint16_t c = accent;
  switch (kind % 14) {
    case 0:  // Crow's Nest — eye
      g.drawEllipse(cx, cy, r - 3, r / 2, c);
      g.fillCircle(cx, cy, 2, c);
      break;
    case 1:  // Harbor — bottle waves
      g.drawFastHLine(cx - 5, cy - 2, 10, c);
      g.drawFastHLine(cx - 4, cy + 1, 8, c);
      g.drawFastHLine(cx - 5, cy + 4, 10, c);
      break;
    case 2:  // Chart Room — map folds
      g.drawRect(cx - 5, cy - 4, 10, 8, c);
      g.drawFastVLine(cx - 1, cy - 4, 8, c);
      g.drawFastHLine(cx - 5, cy, 10, c);
      break;
    case 3:  // Lookout — bars
      for (int i = 0; i < 4; i++)
        g.fillRect(cx - 5 + i * 3, cy + 3 - i * 2, 2, 2 + i * 2, c);
      break;
    case 4:  // Spyglass — circle + tube
      g.drawCircle(cx - 2, cy, 4, c);
      g.drawFastHLine(cx + 2, cy, 5, c);
      break;
    case 5:  // Tracker — radar blip
      g.drawCircle(cx, cy, 5, c);
      g.drawCircle(cx, cy, 2, c);
      g.fillCircle(cx + 4, cy - 3, 1, c);
      break;
    case 6:  // Rigging — shield
      g.fillTriangle(cx, cy - 5, cx - 5, cy - 1, cx + 5, cy - 1, c);
      g.fillTriangle(cx - 5, cy - 1, cx + 5, cy - 1, cx, cy + 5, c);
      break;
    case 7:  // Probe — antenna
      g.drawFastVLine(cx, cy - 5, 10, c);
      g.drawCircle(cx, cy - 5, 2, c);
      g.drawFastHLine(cx - 4, cy + 4, 8, c);
      break;
    case 8:  // Log — book
      g.drawRoundRect(cx - 5, cy - 5, 10, 10, 1, c);
      g.drawFastVLine(cx, cy - 4, 8, c);
      break;
    case 9:  // Systems — gear-ish
      g.drawCircle(cx, cy, 4, c);
      g.fillCircle(cx, cy, 1, c);
      g.drawFastHLine(cx - 6, cy, 12, c);
      g.drawFastVLine(cx, cy - 6, 12, c);
      break;
    case 10:  // Lantern — bulb
      g.fillCircle(cx, cy - 1, 4, c);
      g.fillRect(cx - 2, cy + 3, 4, 3, c);
      break;
    case 11:  // Settings — sliders
      g.drawFastHLine(cx - 6, cy - 3, 12, c);
      g.drawFastHLine(cx - 6, cy + 3, 12, c);
      g.fillCircle(cx - 2, cy - 3, 2, c);
      g.fillCircle(cx + 3, cy + 3, 2, c);
      break;
    case 12:  // Instruments
      g.drawCircle(cx, cy, 6, c);
      g.fillTriangle(cx, cy, cx + 1, cy - 5, cx - 1, cy - 5, c);
      break;
    default:  // Gallery / fallback star
      g.fillTriangle(cx, cy - 5, cx - 3, cy + 3, cx + 3, cy + 3, c);
      break;
  }
}

// printf-style text with transparent background (modern overlay-friendly).
inline void textAt(lgfx::LGFXBase& g, int x, int y, uint16_t fg, uint8_t size,
                   const char* fmt, ...) {
  char b[96];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof(b), fmt, a);
  va_end(a);
  g.setTextSize(size);
  g.setTextColor(fg);
  g.setCursor(x, y);
  g.print(b);
}

}  // namespace gfxu
