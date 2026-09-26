#pragma once

#include <Arduino.h>

// Safe HTTPS/HTTP firmware update from a user-configured URL.
// Credentials + URL live in NVS (set via companion WIFICFG / OTAURL).
// Detection-only RF posture is unchanged: OTA uses STA Wi-Fi to pull a
// binary, never transmits attack frames.
namespace ota {

void begin();  // load prefs

bool wifiConfigured();
bool urlConfigured();
const char* wifiSsid();   // may be empty
const char* url();        // may be empty

void setWifi(const char* ssid, const char* pass);
void setUrl(const char* url);

// Human-readable status for Settings Cabin ("idle", "wifi…", "updating…", …).
const char* status();
int progressPct();  // 0..100 while updating, else -1

// Blocking update attempt. Returns true if the device will reboot into the
// new image. On failure, status() explains why and Wi-Fi is disconnected.
bool runUpdate();

}  // namespace ota
