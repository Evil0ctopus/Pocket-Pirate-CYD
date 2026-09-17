#include "touch.h"

#include <Wire.h>

#include "board_pins.h"

using namespace CheapBlackDisplay;

namespace touch {

namespace {
bool wasPressed = false;
bool freshTap = false;
int16_t tapX = 0, tapY = 0;

// The FT6336 pulses INT on every touch event. Latching it in an ISR lets us
// recover taps that begin AND end while the main loop is busy drawing a frame
// (the coordinate registers retain the last touch after release).
volatile bool irqLatch = false;
void IRAM_ATTR touchIsr() { irqLatch = true; }

// The FT6336 reports coordinates in the panel's native 240(w) x 320(h) portrait
// orientation. The UI runs in landscape (rotation 1: 320 x 240), so map here.
// Verified against on-screen targets (SHIP tab / STATIONS / captain) 2026-09-16:
// x_land = 319 - rawY, y_land = rawX. Calibration capture showed touches land
// ~8-13 px below where the eye aims (finger-pad offset), so bias y upward.
void toLandscape(uint16_t rawX, uint16_t rawY, int16_t& outX, int16_t& outY) {
  outX = static_cast<int16_t>(319 - static_cast<int>(rawY));
  outY = static_cast<int16_t>(static_cast<int>(rawX) - 8);
  if (outX < 0) outX = 0;
  if (outX > 319) outX = 319;
  if (outY < 0) outY = 0;
  if (outY > 239) outY = 239;
}
}  // namespace

void begin() {
  // Wire is already started by main during board bring-up.
  pinMode(TOUCH_INT, INPUT);
  attachInterrupt(digitalPinToInterrupt(TOUCH_INT), touchIsr, FALLING);
}

Point read() {
  Point p;
  bool irq = irqLatch;
  irqLatch = false;
  Wire.beginTransmission(TOUCH_ADDRESS);
  Wire.write(0x02);  // TD_STATUS register: number of active touches
  if (Wire.endTransmission(false) != 0) {
    wasPressed = false;
    return p;
  }
  if (Wire.requestFrom((int)TOUCH_ADDRESS, 5) != 5) {
    wasPressed = false;
    return p;
  }
  uint8_t touches = Wire.read() & 0x0F;
  uint8_t xh = Wire.read();
  uint8_t xl = Wire.read();
  uint8_t yh = Wire.read();
  uint8_t yl = Wire.read();
  uint16_t rawX = ((xh & 0x0F) << 8) | xl;
  uint16_t rawY = ((yh & 0x0F) << 8) | yl;

  if (touches == 0) {
    // A tap that started and ended between polls (finger down during a long
    // frame draw) still latched the IRQ; the registers hold its coordinates.
    if (irq && !wasPressed && millis() > 1500) {
      toLandscape(rawX, rawY, tapX, tapY);
      freshTap = true;
      Serial.printf("[touch] latched raw=(%u,%u) mapped=(%d,%d)\n", rawX, rawY,
                    tapX, tapY);
    }
    wasPressed = false;
    return p;
  }

  toLandscape(rawX, rawY, p.x, p.y);
  p.pressed = true;

  if (!wasPressed) {
    freshTap = true;
    tapX = p.x;
    tapY = p.y;
    // Diagnostic: raw controller coords + the landscape mapping, for verifying
    // the panel's touch orientation against what the UI expects.
    Serial.printf("[touch] raw=(%u,%u) mapped=(%d,%d)\n", rawX, rawY, p.x, p.y);
  }
  wasPressed = true;
  return p;
}

bool wasTapped(int16_t& x, int16_t& y) {
  if (freshTap) {
    freshTap = false;
    x = tapX;
    y = tapY;
    return true;
  }
  return false;
}

}  // namespace touch
