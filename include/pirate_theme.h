#pragma once

#include <Arduino.h>

// Shared RGB565 palette — pixel-art pirate UI.
// Midnight sea, bright mint, worn gold, and warm parchment text.
namespace theme {

// ---- World / art (kept for ship, chibi, speech) ----------------------------
constexpr uint16_t kSky = 0x659D;      // tropical sky blue
constexpr uint16_t kSea = 0x1D15;      // deep ocean
constexpr uint16_t kSeaFoam = 0x9F3D;  // wave crest
constexpr uint16_t kSun = 0xFE60;      // bright coin gold
constexpr uint16_t kGold = 0xFE60;     // primary loot / action
constexpr uint16_t kGoldDim = 0xCD00;  // aged gold
constexpr uint16_t kWood = 0x9A24;     // warm hull wood
constexpr uint16_t kWoodDark = 0x6100; // mast shadow
constexpr uint16_t kSail = 0xFFBA;     // canvas sail
constexpr uint16_t kSailRed = 0xF286;  // signal red
constexpr uint16_t kSkin = 0xFE31;     // pirate skin
constexpr uint16_t kCoat = 0x2ABA;     // pirate coat
constexpr uint16_t kHat = 0x20E6;      // hat / tricorn

// ---- UI chrome -------------------------------------------------------------
constexpr uint16_t kBgDeep = 0x0843;    // midnight ink
constexpr uint16_t kBg = 0x10A5;        // open-water blue
constexpr uint16_t kPanel = 0x216B;     // deep sea glass
constexpr uint16_t kPanelHi = 0x3A6D;   // lifted blue
constexpr uint16_t kPanelSoft = 0x10C7; // inset ocean
constexpr uint16_t kPanelElev = 0x21AB; // raised tile
constexpr uint16_t kBorder = 0x4B2D;    // muted sea-glass edge
constexpr uint16_t kBorderHi = 0x7F1D;  // bright turquoise edge

// ---- Accents ----------------------------------------------------------------
constexpr uint16_t kTeal = 0x46F5;      // bright mint
constexpr uint16_t kCyan = 0x7F1D;      // clear turquoise
constexpr uint16_t kInk = 0xFFDA;       // parchment white
constexpr uint16_t kInkDim = 0xB71A;    // weathered pale blue
constexpr uint16_t kInkMuted = 0x6B71;  // low-priority text
constexpr uint16_t kGood = 0x67C4;      // sea-glass green
constexpr uint16_t kWarn = 0xFCA0;      // amber warning
constexpr uint16_t kBad = 0xF164;       // signal red
constexpr uint16_t kLocked = 0x528A;    // disabled slate
constexpr uint16_t kOnGold = 0x20E6;    // ink on gold

// ---- Layout tokens (320×240) — Ship OS chrome -----------------------------
constexpr int kScreenW = 320;
constexpr int kScreenH = 240;
constexpr int kRadiusCard = 0;
constexpr int kRadiusBtn = 0;
constexpr int kRadiusChip = 0;
constexpr int kPad = 0;              // true edge-to-edge
constexpr int kRowH = 18;            // Field Tablet dense row
constexpr int kRowHCompact = 15;
constexpr int kChipH = 16;
constexpr int kHeaderH = 34;         // world HUD
constexpr int kToolHeaderH = 36;     // Ship OS tool chrome (was 40)
constexpr int kStatusH = 12;
constexpr int kBottomBarH = 26;
constexpr int kKpiH = 30;            // Bridge Console KPI strip (compact for list room)
constexpr int kKpiCardGap = 3;
constexpr int kAccentW = 3;          // Pirate Cabin hairline stripe (not crayon)
constexpr int kContentTop = kHeaderH;
constexpr int kContentBottom = kScreenH - kBottomBarH;  // 214
constexpr int kContentH = kContentBottom - kContentTop; // 180
constexpr int kToolBodyH = kScreenH - kToolHeaderH;     // 204

}  // namespace theme
