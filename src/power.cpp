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
constexpr uint32_t kLowToastMs = 30000;
constexpr int kLowPct = 15;
constexpr int kFullPct = 97;

uint32_t g_emaMv = 0;
uint32_t g_lastSample = 0;
uint32_t g_lastActivity = 0;
uint32_t g_lastLowToast = 0;
bool g_dimmed = false;
uint8_t g_savedBri = 90;
uint8_t g_idleSleepMin = 0;  // 0 / 1 / 3 / 5

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

uint8_t clampIdleMinutes(uint8_t m) {
  if (m == 1 || m == 3 || m == 5) return m;
  return 0;
}

void persistIdleMinutes() {
  Preferences p;
  p.begin("set", false);
  p.putUChar("idlesleep_m", g_idleSleepMin);
  p.putBool("idlesleep", g_idleSleepMin != 0);  // keep legacy key in sync
  p.end();
}

}  // namespace

namespace power {

void begin() {
  analogReadResolution(12);
  g_emaMv = readRawMv();
  g_lastActivity = millis();
  Preferences p;
  p.begin("set", true);
  // Prefer minutes key; migrate from legacy bool when absent.
  if (p.isKey("idlesleep_m")) {
    g_idleSleepMin = clampIdleMinutes(p.getUChar("idlesleep_m", 0));
  } else {
    g_idleSleepMin = p.getBool("idlesleep", false) ? 3 : 0;
  }
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

  if (g_idleSleepMin > 0) {
    uint32_t timeoutMs = (uint32_t)g_idleSleepMin * 60UL * 1000UL;
    if (nowMs - g_lastActivity >= timeoutMs) {
      tools::toast("Idle sleep… tap to wake");
      // Brief pause so the toast can flush; deep sleep follows immediately.
      delay(250);
      deepSleepNow();
    }
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

void setIdleSleepMinutes(uint8_t minutes) {
  g_idleSleepMin = clampIdleMinutes(minutes);
  persistIdleMinutes();
}

uint8_t idleSleepMinutes() { return g_idleSleepMin; }

void cycleIdleSleep() {
  // off → 1m → 3m → 5m → off
  if (g_idleSleepMin == 0) g_idleSleepMin = 1;
  else if (g_idleSleepMin == 1) g_idleSleepMin = 3;
  else if (g_idleSleepMin == 3) g_idleSleepMin = 5;
  else g_idleSleepMin = 0;
  persistIdleMinutes();
}

bool idleSleep() { return g_idleSleepMin != 0; }

const char* idleSleepLabel() {
  switch (g_idleSleepMin) {
    case 1: return "1m";
    case 3: return "3m";
    case 5: return "5m";
    default: return "off";
  }
}

void setIdleSleep(bool on) { setIdleSleepMinutes(on ? 3 : 0); }

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
