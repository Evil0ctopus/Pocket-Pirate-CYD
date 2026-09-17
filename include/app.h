#pragma once

#include <Arduino.h>

// App-wide services owned by main.cpp and shared with the tool modules:
// backlight, sound preference and SD availability.
namespace app {

void setBrightness(uint8_t pct);  // 5..100
uint8_t brightness();

void setSound(bool on);
bool sound();

bool sdReady();  // true when SD_MMC mounted successfully

// Ask main to open the pirate-name keyboard (used by Settings Cabin).
void requestRename();

}  // namespace app
