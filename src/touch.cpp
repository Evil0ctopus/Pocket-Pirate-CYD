#include "touch.h"

#include <Wire.h>
#include <driver/gpio.h>

#include "board_pins.h"

using namespace CheapBlackDisplay;

namespace touch {

namespace {
// Edge state must be ISR-visible: release can happen while the UI core is
// blocked in a ~35ms SPI present, and a re-press before the next poll must
// still count as a new tap (v0.5.4 missed those).
volatile bool wasPressed = false;

// Small ring so genuine taps that arrive during present/draw are not lost.
// v0.5.5–0.5.7 flooded this: present()×2 + loop drain re-poll + ev==0 re-edge
// with no coalesce → one finger → 4–6 actions (Idle cycle, keyboard spam).
constexpr uint8_t kTapQ = 4;
int16_t qX[kTapQ] = {};
int16_t qY[kTapQ] = {};
uint8_t qHead = 0;
uint8_t qTail = 0;

// Coalesce window: one logical press → one queued edge. Long enough to absorb
// bounce / multi-poll during SPI+handleTap, short enough for intentional
// re-taps (~8/s). Does not reintroduce sticky-wasPressed miss (ISR still
// clears on release; cooldown only gates pushTap).
constexpr uint32_t kTapCooldownMs = 120;
uint32_t lastAcceptedTapMs = 0;

void pushTap(int16_t x, int16_t y) {
  uint32_t now = millis();
  if (lastAcceptedTapMs != 0 && (now - lastAcceptedTapMs) < kTapCooldownMs) {
    return;  // same physical press / bounce — drop
  }
  lastAcceptedTapMs = now;

  uint8_t next = static_cast<uint8_t>((qHead + 1) % kTapQ);
  if (next == qTail) {
    // Queue full — drop oldest so the newest edge still lands.
    qTail = static_cast<uint8_t>((qTail + 1) % kTapQ);
  }
  qX[qHead] = x;
  qY[qHead] = y;
  qHead = next;
}

bool intIsUp() {
  return gpio_get_level(static_cast<gpio_num_t>(TOUCH_INT)) != 0;
}

// FT6336 INT is active-low while a finger is down. CHANGE + clear-on-rise
// unsticks wasPressed the instant the finger lifts, even mid-present.
volatile bool irqLatch = false;
void IRAM_ATTR touchIsr() {
  irqLatch = true;
  if (gpio_get_level(static_cast<gpio_num_t>(TOUCH_INT)) != 0) {
    wasPressed = false;  // release — next press can edge-detect
  }
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
  // CHANGE: press (falling) and release (rising). Release clears wasPressed
  // in the ISR so rapid re-taps during SPI are not swallowed.
  attachInterrupt(digitalPinToInterrupt(TOUCH_INT), touchIsr, CHANGE);
}

Point read() {
  Point p;
  bool irq = irqLatch;
  irqLatch = false;

  // Soft-sync from the pin even if an ISR edge was missed.
  if (intIsUp()) {
    wasPressed = false;
  }

  Wire.beginTransmission(TOUCH_ADDRESS);
  Wire.write(0x02);  // TD_STATUS
  if (Wire.endTransmission(false) != 0) {
    // Do not clear wasPressed on I2C glitch while INT still says down —
    // that would double-fire on the next good read.
    if (intIsUp()) wasPressed = false;
    return p;
  }
  if (Wire.requestFrom(static_cast<int>(TOUCH_ADDRESS), 5) != 5) {
    if (intIsUp()) wasPressed = false;
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
    // hold the last point. Cooldown still applies via pushTap.
    if (irq && !wasPressed && millis() > 1500 && coordsPlausible(rawX, rawY)) {
      int16_t x = 0, y = 0;
      toLandscape(rawX, rawY, x, y);
      pushTap(x, y);
    }
    wasPressed = false;
    return p;
  }

  // Multitouch / palm: stay pressed, do not emit a new edge.
  if (touches > 1) {
    wasPressed = true;
    toLandscape(rawX, rawY, p.x, p.y);
    p.pressed = true;
    return p;
  }

  toLandscape(rawX, rawY, p.x, p.y);
  p.pressed = true;

  // Rising-edge only. Do NOT re-fire on FT6336 Down (ev==0) while already
  // pressed — that was a major multi-fire source in v0.5.5–0.5.7. Missed
  // releases are handled by ISR / soft-sync clearing wasPressed on INT rise.
  if (!wasPressed && millis() > 1500) {
    pushTap(p.x, p.y);
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
