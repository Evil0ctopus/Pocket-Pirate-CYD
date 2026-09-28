#pragma once

#include <Arduino.h>

// Shared 16-bit RGB565 palette — Ship OS 2026.
// Deep navy + cyber-teal + gold loot. Opaque elevated panels, high contrast.
// No gray-on-gray Win95 chrome; no heavy blur.
namespace theme {

// ---- World / art (kept for ship, chibi, speech) ----------------------------
constexpr uint16_t kSky = 0x661D;      // warm tropical sky blue
constexpr uint16_t kSea = 0x1595;      // deep ocean
constexpr uint16_t kSeaFoam = 0x9F3D;  // wave crest / teal accent
constexpr uint16_t kSun = 0xFEC7;      // sun / gold
constexpr uint16_t kGold = 0xFEC7;     // loot gold / primary CTA
constexpr uint16_t kGoldDim = 0xC4A0;  // muted gold
constexpr uint16_t kWood = 0x5A05;     // hull / deck wood
constexpr uint16_t kWoodDark = 0x4A05; // mast wood
constexpr uint16_t kSail = 0xFF9A;     // canvas sail
constexpr uint16_t kSailRed = 0xE28A;  // red sail stripe
constexpr uint16_t kSkin = 0xFE31;     // pirate skin
constexpr uint16_t kCoat = 0x2ABA;     // pirate coat
constexpr uint16_t kHat = 0x20E6;      // hat / tricorn

// ---- UI chrome (deep ink / Discord-like shell) ----------------------------
constexpr uint16_t kBgDeep = 0x08A5;   // #0A1528 deep ink
constexpr uint16_t kBg = 0x1129;       // #102148 panel night
constexpr uint16_t kPanel = 0x19AC;    // #183258 soft card face
constexpr uint16_t kPanelHi = 0x2A70;  // #285080 raised chip / hover
constexpr uint16_t kPanelSoft = 0x14EA; // #142850 inset well
constexpr uint16_t kPanelElev = 0x1D0E; // #1C2858 elevated Material-ish
constexpr uint16_t kBorder = 0x3B74;   // soft teal-navy edge
constexpr uint16_t kBorderHi = 0x54F8; // cyan edge highlight (hairline)

// ---- Accents (cyber-teal semantics) ---------------------------------------
constexpr uint16_t kTeal = 0x2617;     // #24C4BC
constexpr uint16_t kCyan = 0x5E7C;     // #5BCFDF
constexpr uint16_t kInk = 0xFFFF;      // primary text
constexpr uint16_t kInkDim = 0x9CD5;   // secondary text (cool gray-blue)
constexpr uint16_t kInkMuted = 0x6B71; // tertiary / hint
constexpr uint16_t kGood = 0x3F0B;     // success green
constexpr uint16_t kWarn = 0xFB20;     // warning orange
constexpr uint16_t kBad = 0xF9A6;      // danger red
constexpr uint16_t kLocked = 0x528A;   // disabled (blue-gray)
constexpr uint16_t kOnGold = 0x18C3;   // dark ink on gold buttons

// ---- Layout tokens (320×240) — Ship OS chrome -----------------------------
constexpr int kScreenW = 320;
constexpr int kScreenH = 240;
constexpr int kRadiusCard = 6;
constexpr int kRadiusBtn = 5;
constexpr int kRadiusChip = 4;
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
