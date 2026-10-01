#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <stdarg.h>
#include <string.h>

#include "pirate_theme.h"

// Shared color and pixel-panel helpers for the 320x240 pirate UI.
namespace gfxu {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

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

inline void normalizePixelText(const char* input, char* output, size_t capacity) {
  if (!capacity) return;
  size_t source = 0, target = 0;
  while (input && input[source] && target + 1 < capacity) {
    if ((uint8_t)input[source] == 0xC2 &&
        (uint8_t)input[source + 1] == 0xB7) {
      output[target++] = '-';
      source += 2;
    } else {
      output[target++] = input[source++];
    }
  }
  output[target] = 0;
}

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

inline void printFit(lgfx::LGFXBase& g, int x, int y, int maxW, uint16_t fg,
                     uint8_t size, const char* s) {
  if (!s) return;
  char normalized[128];
  normalizePixelText(s, normalized, sizeof(normalized));
  s = normalized;
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

inline void printCentered(lgfx::LGFXBase& g, int x, int y, int w, int h,
                          uint16_t fg, uint8_t size, const char* s) {
  if (!s) return;
  char normalized[128];
  normalizePixelText(s, normalized, sizeof(normalized));
  s = normalized;
  g.setTextSize(size);
  g.setTextColor(fg);
  int tw = (int)strlen(s) * 6 * (int)size;
  int th = 8 * (int)size;
  g.setCursor(x + (w - tw) / 2, y + (h - th) / 2);
  g.print(s);
}

// Soft rounded card with optional left accent bar (hairline, not crayon).
inline void drawCard(lgfx::LGFXBase& g, int x, int y, int w, int h,
                     uint16_t fill = theme::kPanel, uint16_t accent = 0,
                     int radius = theme::kRadiusCard) {
  (void)radius;
  g.fillRect(x + 2, y + 2, w, h, theme::kBgDeep);
  g.fillRect(x, y, w, h, fill);
  g.drawRect(x, y, w, h, theme::kBorder);
  g.drawFastHLine(x + 1, y + 1, w - 2, lighten(fill, 28));
  if (accent) {
    int aw = theme::kAccentW;
    g.fillRect(x + 2, y + 2, aw, h - 4, accent);
  }
}

// Content body panel (station screens). Solid fill; hairline top edge.
inline void drawBody(lgfx::LGFXBase& g, int x, int y, int w, int h) {
  g.fillRect(x, y, w, h, theme::kBg);
  g.drawFastHLine(x, y, w, theme::kBorderHi);
  g.drawFastHLine(x, y + 1, w, theme::kBorder);
}

// Elevated opaque panel (Material 3 Expressive — no blur).
inline void drawElevated(lgfx::LGFXBase& g, int x, int y, int w, int h,
                         int radius = theme::kRadiusCard) {
  (void)radius;
  g.fillRect(x + 2, y + 2, w, h, theme::kBgDeep);
  g.fillRect(x, y, w, h, theme::kPanelElev);
  g.drawRect(x, y, w, h, theme::kBorderHi);
  g.drawFastHLine(x + 1, y + 1, w - 2, lighten(theme::kPanelElev, 36));
}

// Primary CTA / secondary button.
inline void drawButton(lgfx::LGFXBase& g, int x, int y, int w, int h,
                       const char* label, bool primary = true,
                       uint16_t fillOverride = 0) {
  uint16_t fill = fillOverride
                      ? fillOverride
                      : (primary ? theme::kGold : theme::kPanelHi);
  uint16_t fg = primary ? theme::kOnGold : theme::kInk;
  if (primary) g.fillRect(x + 2, y + 2, w, h, theme::kBgDeep);
  g.fillRect(x, y, w, h, fill);
  g.drawRect(x, y, w, h, primary ? lighten(fill, 48) : theme::kBorder);
  g.drawFastHLine(x + 1, y + 1, w - 2, lighten(fill, 70));
  printCentered(g, x, y, w, h, fg, 1, label);
}

// Sort / filter chip (pill) — cyber-teal hairline when off.
inline void drawChip(lgfx::LGFXBase& g, int x, int y, int w, int h,
                     const char* label, bool on, uint16_t onColor = theme::kGold) {
  uint16_t fill = on ? onColor : theme::kPanelSoft;
  uint16_t fg = on ? theme::kOnGold : theme::kInkDim;
  g.fillRect(x, y, w, h, fill);
  g.drawRect(x, y, w, h, on ? lighten(onColor, 40) : theme::kBorderHi);
  printCentered(g, x, y, w, h, fg, 1, label);
}

inline void drawSeparator(lgfx::LGFXBase& g, int x, int y, int w) {
  g.drawFastHLine(x, y, w, theme::kBorder);
  g.drawFastHLine(x, y + 1, w, darken(theme::kBgDeep, 40));
}

// Status badge pill (Ship OS chrome: BAT / GPS / SD).
inline void drawBadge(lgfx::LGFXBase& g, int x, int y, int w, int h,
                      const char* label, uint16_t fg,
                      uint16_t fill = theme::kPanelSoft) {
  g.fillRect(x, y, w, h, fill);
  g.drawRect(x, y, w, h, theme::kBorder);
  printCentered(g, x, y, w, h, fg, 1, label);
}

// MiniDash-style KPI card: large value + tiny label.
inline void drawKpiCard(lgfx::LGFXBase& g, int x, int y, int w, int h,
                        const char* value, const char* label,
                        uint16_t valueColor = theme::kTeal,
                        uint16_t accent = 0) {
  g.fillRect(x + 2, y + 2, w, h, theme::kBgDeep);
  g.fillRect(x, y, w, h, theme::kPanelElev);
  g.drawRect(x, y, w, h, theme::kBorderHi);
  if (accent) g.fillRect(x + 1, y + 2, theme::kAccentW, h - 4, accent);
  int inset = accent ? 6 : 4;
  g.setTextSize(2);
  g.setTextColor(valueColor);
  int vw = (int)strlen(value) * 12;
  g.setCursor(x + inset + (w - inset - 4 - vw) / 2, y + 4);
  g.print(value);
  g.setTextSize(1);
  g.setTextColor(theme::kInkMuted);
  int lw = (int)strlen(label) * 6;
  g.setCursor(x + inset + (w - inset - 4 - lw) / 2, y + h - 11);
  g.print(label);
}

// Bridge Console KPI strip: N equal cards across content width.
// values/labels arrays of length n (2..4). Returns strip bottom y.
inline int drawKpiStrip(lgfx::LGFXBase& g, int x, int y, int w, int n,
                        const char* const* values, const char* const* labels,
                        const uint16_t* colors = nullptr,
                        const uint16_t* accents = nullptr) {
  if (n < 1) n = 1;
  if (n > 4) n = 4;
  const int gap = theme::kKpiCardGap;
  const int h = theme::kKpiH;
  const int cw = (w - gap * (n - 1)) / n;
  for (int i = 0; i < n; i++) {
    int cx = x + i * (cw + gap);
    uint16_t vc = (colors && colors[i]) ? colors[i] : theme::kTeal;
    uint16_t ac = (accents) ? accents[i] : 0;
    drawKpiCard(g, cx, y, cw, h, values[i] ? values[i] : "-",
                labels[i] ? labels[i] : "", vc, ac);
  }
  return y + h;
}

// Field Tablet dense list row: zebra, primary + secondary, right meta + optional bars area.
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
  g.fillRect(x, y, w, h, theme::kPanelSoft);
  g.drawRect(x, y, w, h, theme::kBorder);
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

// Toolbar well for chips / filters.
inline void drawToolbar(lgfx::LGFXBase& g, int x, int y, int w, int h) {
  g.fillRect(x, y, w, h, theme::kPanelSoft);
  g.drawRect(x, y, w, h, theme::kBorder);
}

// Station glyphs sit on square, two-tone pixel backplates.
inline void drawGlyph(lgfx::LGFXBase& g, int cx, int cy, int r, int kind,
                      uint16_t accent) {
  uint16_t ink = theme::kInk;
  uint16_t dim = theme::kInkDim;
  uint16_t gold = theme::kGold;
  uint16_t deep = theme::kBgDeep;
  uint16_t elev = theme::kPanelElev;
  g.fillRect(cx - r + 2, cy - r + 2, r * 2, r * 2, deep);
  g.fillRect(cx - r, cy - r, r * 2, r * 2, elev);
  g.drawRect(cx - r, cy - r, r * 2, r * 2, accent);
  g.drawRect(cx - r + 2, cy - r + 2, r * 2 - 4, r * 2 - 4,
             darken(accent, 80));
  g.fillRect(cx + r - 3, cy - r + 2, 2, 2, gold);

  uint16_t c = accent;
  switch (kind % 15) {
    case 0: {  // Crow's Nest — spyglass + crow eye
      g.fillCircle(cx - 1, cy, r - 4, theme::kPanelSoft);
      g.drawEllipse(cx - 1, cy, r - 4, r / 2 - 1, c);
      g.fillCircle(cx - 1, cy, 2, gold);
      g.fillCircle(cx - 1, cy, 1, deep);
      g.fillRect(cx + 3, cy - 1, r - 3, 3, c);
      g.drawRect(cx + 3, cy - 1, r - 3, 3, lighten(c, 60));
      g.fillTriangle(cx + r - 3, cy - 2, cx + r - 1, cy, cx + r - 3, cy + 2, gold);
      break;
    }
    case 1: {  // Harbor — bottle on waves
      g.fillRoundRect(cx - 3, cy - 5, 6, 9, 1, c);
      g.fillTriangle(cx - 2, cy - 7, cx + 2, cy - 7, cx, cy - 5, gold);
      g.drawFastHLine(cx - 6, cy + 4, 12, lighten(c, 40));
      g.drawFastHLine(cx - 5, cy + 6, 10, c);
      g.fillCircle(cx + 1, cy - 2, 1, theme::kCyan);
      break;
    }
    case 2: {  // Chart Room — folded chart + X
      g.fillRoundRect(cx - 6, cy - 5, 12, 10, 1, theme::kPanelSoft);
      g.drawRoundRect(cx - 6, cy - 5, 12, 10, 1, c);
      g.drawFastVLine(cx - 1, cy - 5, 10, dim);
      g.drawFastHLine(cx - 6, cy, 12, dim);
      g.drawLine(cx - 4, cy - 3, cx + 4, cy + 3, gold);
      g.drawLine(cx + 4, cy - 3, cx - 4, cy + 3, gold);
      break;
    }
    case 3: {  // Lookout — signal bars + crow perch
      for (int i = 0; i < 4; i++) {
        int bh = 3 + i * 2;
        g.fillRoundRect(cx - 6 + i * 3, cy + 5 - bh, 2, bh, 1,
                        i >= 2 ? gold : c);
      }
      g.fillCircle(cx + 5, cy - 4, 2, c);
      g.fillTriangle(cx + 5, cy - 2, cx + 3, cy + 1, cx + 7, cy + 1, dim);
      break;
    }
    case 4: {  // Spyglass — telescope with lens flare
      g.fillCircle(cx - 3, cy, 5, theme::kPanelSoft);
      g.drawCircle(cx - 3, cy, 5, c);
      g.drawCircle(cx - 3, cy, 3, gold);
      g.fillCircle(cx - 3, cy, 1, ink);
      g.fillRoundRect(cx + 1, cy - 2, 7, 4, 1, c);
      g.fillRect(cx + 7, cy - 3, 2, 6, gold);
      break;
    }
    case 5: {  // Tracker — radar rings + blip
      g.drawCircle(cx, cy, 6, c);
      g.drawCircle(cx, cy, 3, dim);
      g.drawFastHLine(cx - 6, cy, 12, dim);
      g.drawFastVLine(cx, cy - 6, 12, dim);
      g.fillCircle(cx + 4, cy - 3, 2, theme::kBad);
      g.fillCircle(cx + 4, cy - 3, 1, gold);
      break;
    }
    case 6: {  // Rigging — shield + bolt
      g.fillTriangle(cx, cy - 6, cx - 6, cy - 1, cx + 6, cy - 1, c);
      g.fillTriangle(cx - 6, cy - 1, cx + 6, cy - 1, cx, cy + 6, darken(c, 40));
      g.drawLine(cx - 2, cy - 1, cx + 1, cy + 1, gold);
      g.drawLine(cx + 1, cy + 1, cx - 1, cy + 4, gold);
      break;
    }
    case 7: {  // Probe — antenna dish
      g.fillCircle(cx, cy + 1, 4, theme::kPanelSoft);
      g.drawCircle(cx, cy + 1, 4, c);
      g.drawFastVLine(cx, cy - 6, 5, c);
      g.fillCircle(cx, cy - 6, 2, gold);
      g.drawFastHLine(cx - 5, cy + 6, 10, dim);
      break;
    }
    case 8: {  // Log — open book + quill
      g.fillRoundRect(cx - 6, cy - 5, 6, 10, 1, theme::kPanelSoft);
      g.fillRoundRect(cx, cy - 5, 6, 10, 1, elev);
      g.drawRoundRect(cx - 6, cy - 5, 12, 10, 1, c);
      g.drawFastVLine(cx, cy - 4, 8, gold);
      g.drawLine(cx + 4, cy - 6, cx + 6, cy + 2, dim);
      g.fillCircle(cx + 6, cy + 3, 1, c);
      break;
    }
    case 9: {  // Systems — gear
      g.fillCircle(cx, cy, 4, c);
      g.fillCircle(cx, cy, 2, deep);
      for (int a = 0; a < 6; a++) {
        // 6 teeth via offsets
        static const int8_t ox[6] = {0, 5, 5, 0, -5, -5};
        static const int8_t oy[6] = {-6, -3, 3, 6, 3, -3};
        g.fillRect(cx + ox[a] - 1, cy + oy[a] - 1, 3, 3, c);
      }
      g.fillCircle(cx, cy, 1, gold);
      break;
    }
    case 10: {  // Lantern — glowing lamp
      g.fillRoundRect(cx - 4, cy - 6, 8, 3, 1, gold);
      g.fillCircle(cx, cy + 1, 5, c);
      g.fillCircle(cx - 1, cy, 2, lighten(c, 100));
      g.fillRect(cx - 2, cy + 5, 4, 3, dim);
      g.drawFastVLine(cx, cy - 8, 2, dim);
      break;
    }
    case 11: {  // Settings — sliders + knobs
      g.drawFastHLine(cx - 6, cy - 3, 12, c);
      g.drawFastHLine(cx - 6, cy + 3, 12, c);
      g.fillCircle(cx - 2, cy - 3, 2, gold);
      g.fillCircle(cx + 3, cy + 3, 2, theme::kCyan);
      g.drawCircle(cx - 2, cy - 3, 2, ink);
      g.drawCircle(cx + 3, cy + 3, 2, ink);
      break;
    }
    case 12: {  // Instruments — compass rose
      g.drawCircle(cx, cy, 6, c);
      g.fillTriangle(cx, cy - 5, cx + 2, cy, cx - 2, cy, gold);
      g.fillTriangle(cx, cy + 5, cx + 2, cy, cx - 2, cy, dim);
      g.fillTriangle(cx - 5, cy, cx, cy - 2, cx, cy + 2, c);
      g.fillTriangle(cx + 5, cy, cx, cy - 2, cx, cy + 2, c);
      g.fillCircle(cx, cy, 1, ink);
      break;
    }
    case 13: {  // Gallery / Art — framed skull-lite
      g.drawRoundRect(cx - 6, cy - 6, 12, 12, 1, c);
      g.fillCircle(cx - 2, cy - 1, 1, gold);
      g.fillCircle(cx + 2, cy - 1, 1, gold);
      g.fillTriangle(cx - 3, cy + 3, cx + 3, cy + 3, cx, cy + 5, dim);
      break;
    }
    default: {  // Star / loot
      g.fillTriangle(cx, cy - 6, cx - 4, cy + 2, cx + 4, cy + 2, gold);
      g.fillTriangle(cx, cy + 5, cx - 4, cy - 1, cx + 4, cy - 1, c);
      break;
    }
  }
}

inline void textAt(lgfx::LGFXBase& g, int x, int y, uint16_t fg, uint8_t size,
                   const char* fmt, ...) {
  char b[96];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof(b), fmt, a);
  va_end(a);
  char normalized[96];
  normalizePixelText(b, normalized, sizeof(normalized));
  g.setTextSize(size);
  g.setTextColor(fg);
  g.setCursor(x, y);
  g.print(normalized);
}

}  // namespace gfxu
