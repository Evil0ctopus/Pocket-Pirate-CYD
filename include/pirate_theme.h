#pragma once

#include <Arduino.h>

// Shared 16-bit RGB565 palette for the Pocket Pirate world so every screen
// reads as one hand-drawn cartoon set. Values are pre-computed color565().
namespace theme {

constexpr uint16_t kSky = 0x661D;      // warm tropical sky blue
constexpr uint16_t kSea = 0x1595;      // deep ocean
constexpr uint16_t kSeaFoam = 0x9F3D;  // wave crest highlight
constexpr uint16_t kSun = 0xFEC7;      // sun / gold
constexpr uint16_t kGold = 0xFEC7;     // loot gold / accents
constexpr uint16_t kWood = 0x5A05;     // hull / deck wood
constexpr uint16_t kWoodDark = 0x4A05; // mast wood
constexpr uint16_t kSail = 0xFF9A;     // canvas sail
constexpr uint16_t kSailRed = 0xE28A;  // red sail stripe
constexpr uint16_t kSkin = 0xFE31;     // pirate skin
constexpr uint16_t kCoat = 0x2ABA;     // pirate coat
constexpr uint16_t kHat = 0x20E6;      // hat / tricorn
constexpr uint16_t kPanel = 0x1B49;    // UI panel background
constexpr uint16_t kPanelHi = 0x2C6D;  // UI panel highlight
constexpr uint16_t kInk = 0xFFFF;      // primary text
constexpr uint16_t kInkDim = 0xAD55;   // secondary text
constexpr uint16_t kGood = 0x3F0B;     // success green
constexpr uint16_t kWarn = 0xFB20;     // warning orange
constexpr uint16_t kBad = 0xF9A6;      // danger red
constexpr uint16_t kLocked = 0x8410;   // locked / disabled grey

}  // namespace theme
