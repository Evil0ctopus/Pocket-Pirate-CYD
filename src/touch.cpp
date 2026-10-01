#include "touch.h"

#include <Wire.h>
#include <driver/gpio.h>

#include "board_pins.h"

using namespace CheapBlackDisplay;

namespace touch {

namespace {
// Contact state is released only after a stable zero-touch report; this keeps
// brief FT6336 register/INT glitches from turning one hold into multiple taps.
volatile bool wasPressed = false;
uint32_t releaseStartedAt = 0;
uint32_t lastTapAt = 0;
constexpr uint32_t kReleaseDebounceMs = 60;
constexpr uint32_t kTapDebounceMs = 180;

// Small ring so taps that arrive during present/draw are not overwritten.
constexpr uint8_t kTapQ = 4;
int16_t qX[kTapQ] = {};
int16_t qY[kTapQ] = {};
uint8_t qHead = 0;
uint8_t qTail = 0;

void pushTap(int16_t x, int16_t y) {
  uint8_t next = static_cast<uint8_t>((qHead + 1) % kTapQ);
  if (next == qTail) {
    // Queue full — drop oldest so the newest edge still lands.
    qTail = static_cast<uint8_t>((qTail + 1) % kTapQ);
  }
  qX[qHead] = x;
  qY[qHead] = y;
  qHead = next;
}

// FT6336 may pulse INT while a contact remains active. Keep only an edge latch
// for short taps that begin and end during a blocked display present.
volatile bool irqLatch = false;
void IRAM_ATTR touchIsr() {
  irqLatch = true;
}

// Panel native 240×320 portrait → UI landscape 320×240 (rotation 1).
// Verified 2026-09-16; y biased up for finger-pad offset.
void toLandscape(uint16_t rawX, uint16_t rawY, int16_t& outX, int16_t& outY) {
  outX = static_cast<int16_t>(319 - static_cast<int>(rawY));
  outY = static_cast<int16_t>(static_cast<int>(rawX) - 8);
  if (outX < 0) outX = 0;
  if (outX > 319) outX = 319;
  if (outY < 0) outY = 0;
  if (outY > 239) outY = 239;
}

bool coordsPlausible(uint16_t rawX, uint16_t rawY) {
  // Reject empty/cleared registers so IRQ recovery does not fake a corner tap.
  if (rawX == 0 && rawY == 0) return false;
  if (rawX > 240 || rawY > 320) return false;
  return true;
}
}  // namespace

void begin() {
  pinMode(TOUCH_INT, INPUT);
  // CHANGE: latch both contact edges. The poller confirms a stable release
  // before it rearms, while irqLatch preserves taps during a display present.
  attachInterrupt(digitalPinToInterrupt(TOUCH_INT), touchIsr, CHANGE);
}

Point read() {
  Point p;
  bool irq = irqLatch;
  irqLatch = false;

  Wire.beginTransmission(TOUCH_ADDRESS);
  Wire.write(0x02);  // TD_STATUS
  if (Wire.endTransmission(false) != 0) {
    return p;
  }
  if (Wire.requestFrom(static_cast<int>(TOUCH_ADDRESS), 5) != 5) {
    return p;
  }
  uint8_t touches = Wire.read() & 0x0F;
  uint8_t xh = Wire.read();
  uint8_t xl = Wire.read();
  uint8_t yh = Wire.read();
  uint8_t yl = Wire.read();
  uint16_t rawX = static_cast<uint16_t>(((xh & 0x0F) << 8) | xl);
  uint16_t rawY = static_cast<uint16_t>(((yh & 0x0F) << 8) | yl);
  if (touches == 0) {
    // Short tap entirely inside a long present: IRQ latched; regs may still
    // hold the last point.
    uint32_t now = millis();
    if (!wasPressed && irq && now > 1500 &&
        now - lastTapAt >= kTapDebounceMs && coordsPlausible(rawX, rawY)) {
      int16_t x = 0, y = 0;
      toLandscape(rawX, rawY, x, y);
      pushTap(x, y);
      lastTapAt = now;
    }
    if (wasPressed) {
      if (releaseStartedAt == 0) releaseStartedAt = now;
      if (now - releaseStartedAt >= kReleaseDebounceMs) {
        wasPressed = false;
        releaseStartedAt = 0;
      }
    } else {
      releaseStartedAt = 0;
    }
    return p;
  }

  // Multitouch / palm: stay pressed, do not emit a new edge.
  if (touches > 1) {
    releaseStartedAt = 0;
    wasPressed = true;
    toLandscape(rawX, rawY, p.x, p.y);
    p.pressed = true;
    return p;
  }

  toLandscape(rawX, rawY, p.x, p.y);
  p.pressed = true;
  releaseStartedAt = 0;

  // A repeated Down register value or INT pulse while held is not another tap.
  bool newEdge = !wasPressed;
  uint32_t now = millis();
  if (newEdge && now > 1500 && now - lastTapAt >= kTapDebounceMs) {
    pushTap(p.x, p.y);
    lastTapAt = now;
  }
  wasPressed = true;
  return p;
}

bool wasTapped(int16_t& x, int16_t& y) {
  if (qTail == qHead) return false;
  x = qX[qTail];
  y = qY[qTail];
  qTail = static_cast<uint8_t>((qTail + 1) % kTapQ);
  return true;
}

}  // namespace touch
