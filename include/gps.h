#pragma once

#include <Arduino.h>

// Optional UART GPS (NMEA via TinyGPSPlus). Absent module -> no fix forever.
namespace gps {

void begin();
void tick(uint32_t nowMs);

bool available();   // UART opened
bool hasFix();
double latitude();
double longitude();
double altitudeM();
double hdop();      // 0 if unknown
uint32_t satellites();
uint32_t ageMs();    // ms since last fix update; UINT32_MAX if never

// WiGLE-friendly "YYYY-MM-DD HH:MM:SS" from GPS UTC when valid, else false.
bool formatTimestamp(char* out, size_t n);

// Short HUD / Chart Room status ("FIX 12", "NO FIX", "NO GPS").
const char* statusLabel();

}  // namespace gps
