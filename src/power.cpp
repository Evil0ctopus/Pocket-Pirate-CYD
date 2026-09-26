#include "power.h"

#include "app.h"
#include "board_pins.h"
#include "tools.h"

using namespace CheapBlackDisplay;

namespace {

constexpr uint32_t kSampleMs = 500;
constexpr uint32_t kIdleDimMs = 45000;
constexpr uint32_t kLowToastMs = 30000;
constexpr int kLowPct = 15;

uint32_t g_emaMv = 0;
uint32_t g_lastSample = 0;
uint32_t g_lastActivity = 0;
uint32_t g_lastLowToast = 0;
bool g_dimmed = false;
uint8_t g_savedBri = 90;

uint32_t readRawMv() {
  // Typical CYD battery path: ADC sees half of pack voltage via divider.
  uint32_t mv = analogReadMilliVolts(BATTERY_ADC) * 2;
  if (mv < 2500) mv = 2500;
  if (mv > 5200) mv = 5200;
  return mv;
}

int pctFromMv(uint32_t mv) {
  // LiPo curve approx: 3.30 V empty .. 4.20 V full. Clamp USB/charge highs.
  if (mv >= 4300) return 100;
  if (mv <= 3300) return 0;
  return (int)((mv - 3300) * 100 / 900);
}

}  // namespace

namespace power {

void begin() {
  analogReadResolution(12);
  g_emaMv = readRawMv();
  g_lastActivity = millis();
}

void tick(uint32_t nowMs) {
  if (nowMs - g_lastSample >= kSampleMs) {
    g_lastSample = nowMs;
    uint32_t sample = readRawMv();
    if (g_emaMv == 0) g_emaMv = sample;
    else g_emaMv = (g_emaMv * 7 + sample) / 8;  // light EMA
  }

  if (lowBattery() && nowMs - g_lastLowToast > kLowToastMs) {
    g_lastLowToast = nowMs;
    tools::toast("Low battery ~%d%%", batteryPct());
  }
}

int batteryPct() { return pctFromMv(g_emaMv ? g_emaMv : readRawMv()); }

uint32_t batteryMv() { return g_emaMv ? g_emaMv : readRawMv(); }

bool usbPowered() { return batteryMv() >= 4300; }

bool lowBattery() { return !usbPowered() && batteryPct() < kLowPct; }

const char* powerLabel() {
  static char buf[8];
  if (usbPowered()) {
    // Near-full while on USB often means charging / charged.
    if (batteryPct() >= 95) return "USB";
    return "CHG";
  }
  int p = batteryPct();
  if (p < kLowPct) {
    snprintf(buf, sizeof(buf), "LOW");
    return buf;
  }
  snprintf(buf, sizeof(buf), "%d%%", p);
  return buf;
}

void noteActivity(uint32_t nowMs) {
  g_lastActivity = nowMs;
  if (g_dimmed) {
    g_dimmed = false;
    app::setBrightness(g_savedBri);
  }
}

uint8_t applyIdleDim(uint8_t userBrightness, uint32_t nowMs) {
  if (nowMs - g_lastActivity < kIdleDimMs) {
    return userBrightness;
  }
  if (!g_dimmed) {
    g_savedBri = userBrightness;
    g_dimmed = true;
    uint8_t dim = userBrightness < 20 ? userBrightness : 20;
    // Drive backlight without writing NVS (avoid thrashing prefs on idle).
    ledcWrite(TFT_BL, (uint32_t)dim * 255 / 100);
  }
  return g_dimmed ? (uint8_t)(userBrightness < 20 ? userBrightness : 20)
                  : userBrightness;
}

}  // namespace power
