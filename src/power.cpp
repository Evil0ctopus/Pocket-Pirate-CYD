#include "power.h"

#include <Preferences.h>
#include <esp_sleep.h>

#include "app.h"
#include "board_pins.h"
#include "tools.h"

using namespace CheapBlackDisplay;

namespace {

constexpr uint32_t kSampleMs = 500;
constexpr uint32_t kIdleDimMs = 45000;
constexpr uint32_t kIdleSleepMs = 180000;  // 3 min after last activity
constexpr uint32_t kLowToastMs = 30000;
constexpr int kLowPct = 15;
constexpr int kFullPct = 97;

uint32_t g_emaMv = 0;
uint32_t g_lastSample = 0;
uint32_t g_lastActivity = 0;
uint32_t g_lastLowToast = 0;
bool g_dimmed = false;
uint8_t g_savedBri = 90;
bool g_idleSleep = false;

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
  Preferences p;
  p.begin("set", true);
  g_idleSleep = p.getBool("idlesleep", false);
  p.end();
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

  if (g_idleSleep && nowMs - g_lastActivity >= kIdleSleepMs) {
    tools::toast("Idle sleep… tap to wake");
    delay(400);
    deepSleepNow();
  }
}

int batteryPct() { return pctFromMv(g_emaMv ? g_emaMv : readRawMv()); }

uint32_t batteryMv() { return g_emaMv ? g_emaMv : readRawMv(); }

bool usbPowered() { return batteryMv() >= 4300; }

bool lowBattery() { return !usbPowered() && batteryPct() < kLowPct; }

const char* powerLabel() {
  static char buf[8];
  if (usbPowered()) {
    int p = batteryPct();
    // Without a dedicated charge-status pin we approximate:
    // near-full on USB => FULL, otherwise CHG (charging assumed).
    if (p >= kFullPct) return "FULL";
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
    // Soft dim: floor at 12% so the screen stays readable as a night lamp.
    uint8_t dim = userBrightness < 12 ? userBrightness : 12;
    ledcWrite(TFT_BL, (uint32_t)dim * 255 / 100);
  }
  return g_dimmed ? (uint8_t)(userBrightness < 12 ? userBrightness : 12)
                  : userBrightness;
}

void setIdleSleep(bool on) {
  g_idleSleep = on;
  Preferences p;
  p.begin("set", false);
  p.putBool("idlesleep", g_idleSleep);
  p.end();
}

bool idleSleep() { return g_idleSleep; }

void deepSleepNow() {
  // Kill backlight + LED so sleep draws near-zero from the panel.
  ledcWrite(TFT_BL, 0);
  rgbLedWrite(RGB_LED, 0, 0, 0);

  // FT6336 INT is active-low on touch; wake the ESP32-S3 from deep sleep.
  pinMode(TOUCH_INT, INPUT_PULLUP);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)TOUCH_INT, 0);

  Serial.println("[power] deep sleep — touch to wake");
  Serial.flush();
  delay(50);
  esp_deep_sleep_start();
}

}  // namespace power
