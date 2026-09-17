#pragma once

#include <Arduino.h>

// Persistent RGB status-LED profile engine. The profile (mode + color +
// brightness) is saved to NVS and runs continuously from the main loop, so the
// lantern keeps its setting across screens and reboots. The Signal Lantern
// station is the editor UI for it.
namespace led {

enum Mode : uint8_t { Off = 0, Solid, Beacon, Rainbow, Alarm, ModeCount };
constexpr int kColorCount = 8;

void begin();               // load profile from NVS and apply
void tick(uint32_t now);    // call every loop; animates + applies the profile

uint8_t mode();
void setMode(uint8_t m);    // persists
uint8_t colorIdx();
void setColorIdx(uint8_t c);  // persists
uint8_t brightness();
void setBrightness(uint8_t pct);  // 5..100, persists

const char* modeName(uint8_t m);
uint32_t colorRgb(uint8_t idx);  // 0xRRGGBB preset for swatch UI

}  // namespace led
