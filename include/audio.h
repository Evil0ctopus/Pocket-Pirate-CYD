#pragma once

#include <Arduino.h>

// ES8311 codec + I2S playback: boot chime, tap chirp, level-up fanfare.
// The init sequence is transcribed from Espressif's Apache-2.0 es8311 driver
// (esp-bsp components/es8311), configured for 16 kHz / 16-bit, ESP32 as I2S
// master supplying a 256*fs MCLK, codec as slave. All play calls are no-ops
// when the codec failed to init or the sound preference is off.
namespace audio {

void begin();      // bring up the codec + I2S; safe to call once from setup()
bool ready();      // codec answered and I2S started

void setVolume(uint8_t pct);  // 0..100, persisted; applied to the codec DAC
uint8_t volume();

void tapChirp();   // short blip (tap feedback)
void chime();      // rising two/three-note "ahoy" (boot)
void fanfare();    // level-up jingle

void setAmp(bool on);  // power-amp enable pin (diagnostics)
void dumpRegs();       // print codec registers over Serial (diagnostics)

}  // namespace audio
