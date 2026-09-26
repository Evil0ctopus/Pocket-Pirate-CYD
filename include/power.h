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

// Idle-sleep preference (deep sleep after long idle). Default off.
void setIdleSleep(bool on);
bool idleSleep();

// Enter deep sleep now. Wake on touch INT (active-low) or reset button.
// Saves game state is the caller's responsibility before invoking.
void deepSleepNow();

}  // namespace power
