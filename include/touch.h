#pragma once

#include <Arduino.h>

// Minimal FT6336G capacitive-touch reader for the cheap black display.
// Reports a single active touch point in *rotated* screen coordinates so the
// rest of the UI can work in the same 320x240 landscape space it draws in.
namespace touch {

struct Point {
  bool pressed = false;
  int16_t x = 0;   // 0..319 (landscape)
  int16_t y = 0;   // 0..239
};

void begin();
// Poll the controller. Returns the current point; `pressed` is false when the
// panel is not being touched.
Point read();
// True on the transition from not-pressed to pressed (a fresh tap).
bool wasTapped(int16_t& x, int16_t& y);

}  // namespace touch
