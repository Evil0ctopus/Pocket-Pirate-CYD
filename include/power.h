#pragma once

#include <Arduino.h>

// Battery / USB power helpers used by the HUD, Instruments, and companion.
namespace power {

void begin();
void tick(uint32_t nowMs);

// Smoothed estimate 0..100. Above ~4.3 V treated as USB/charging.
int batteryPct();
uint32_t batteryMv();
bool usbPowered();       // ADC suggests USB / charger present
bool lowBattery();       // pct < 15 and not USB
// Labels: "USB" (powered, no charge sense), "CHG" (USB + charging),
// "FULL" (USB + ~full), "NN%", "LOW".
const char* powerLabel();

// Call when any user input happens (touch / serial TAP) so idle-dim resets.
void noteActivity(uint32_t nowMs);
// Apply optional dim-on-idle; returns the brightness currently driven to BL.
uint8_t applyIdleDim(uint8_t userBrightness, uint32_t nowMs);

// Idle-sleep after N minutes of inactivity. 0 = off. Supported: 0, 1, 3, 5.
void setIdleSleepMinutes(uint8_t minutes);
uint8_t idleSleepMinutes();
void cycleIdleSleep();         // off → 1m → 3m → 5m → off
bool idleSleep();              // true when minutes != 0
const char* idleSleepLabel();  // "off" / "1m" / "3m" / "5m"
// Legacy: maps true→3m, false→off. Prefer cycleIdleSleep / minutes helpers.
void setIdleSleep(bool on);

// Enter deep sleep now. Wake on touch INT (active-low) or reset button.
// Saves game state is the caller's responsibility before invoking.
void deepSleepNow();

}  // namespace power
