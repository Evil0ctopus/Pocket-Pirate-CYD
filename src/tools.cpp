#include "tools.h"

#include <WiFi.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <BLEDevice.h>
#include <SD_MMC.h>

#include <set>
#include <strings.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "app.h"
#include "board_pins.h"
#include "game_state.h"
#include "gfx_util.h"
#include "gps.h"
#include "led.h"
#include "oui.h"
#include "ota.h"
#include "pirate_theme.h"
#include "power.h"
#include "spyglass_signatures.h"

#ifndef PP_VERSION
#define PP_VERSION "0.5.7"
#endif

using namespace CheapBlackDisplay;

// ===========================================================================
//  Shared drawing helpers
// ===========================================================================
namespace {

lgfx::LGFXBase& G() { return *tools::gfx; }

void body(int x, int y, int w, int h) { gfxu::drawBody(G(), x, y, w, h); }

void txt(int x, int y, uint16_t fg, uint8_t size, const char* fmt, ...) {
  char b[96];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof(b), fmt, a);
  va_end(a);
  char normalized[96];
  gfxu::normalizePixelText(b, normalized, sizeof(normalized));
  auto& g = G();
  g.setTextSize(size);
  g.setTextColor(fg);  // transparent bg — no Win95 opaque dump
  g.setCursor(x, y);
  g.print(normalized);
}

void btn(int x, int y, int w, int h, const char* label, bool primary = true,
         uint16_t fill = 0) {
  gfxu::drawButton(G(), x, y, w, h, label, primary, fill);
}

void chip(int x, int y, int w, int h, const char* label, bool on,
          uint16_t onColor = theme::kGold) {
  gfxu::drawChip(G(), x, y, w, h, label, on, onColor);
}

// Bridge Console: pack KPI values into fixed buffers and draw strip.
int kpiStrip(int x, int y, int w, int n, const char* const* values,
             const char* const* labels, const uint16_t* colors = nullptr) {
  return gfxu::drawKpiStrip(G(), x, y, w, n, values, labels, colors, nullptr);
}

int contentTop() { return theme::kToolHeaderH; }

// Signal-strength pips from an RSSI value.
void rssiBars(int x, int y, int rssi) {
  int bars = 0;
  if (rssi > -55) bars = 4;
  else if (rssi > -67) bars = 3;
  else if (rssi > -78) bars = 2;
  else if (rssi > -88) bars = 1;
  for (int i = 0; i < 4; i++) {
    uint16_t c = i < bars ? theme::kGood : theme::kLocked;
    G().fillRect(x + i * 4, y + 8 - i * 2, 3, 2 + i * 2, c);
  }
}

// ---- FreeRTOS Wi-Fi RF worker (UI loop never waits on WiFi.*) -------------
// v0.5.1 hang: stacked WiFi.mode/scanDelete/promiscuous while async scan alive.
// v0.5.2: one opcode per UI loop tick — fixed multi-open wedge, but a single
// EnsureSta/scanDelete can still block 1–3s and starve touch→present.
// v0.5.3: RF state machine on pinned FreeRTOS task (core 0). UI only arms flags.
// v0.5.4: channel hops go through the worker (no UI-core esp_wifi_*);
//         BLE stops when WiFi RF arms; LeaveDone respects a newer want;
//         UI does a single present per navigation (see main.cpp).

enum class RfWant : uint8_t { Idle = 0, Scan, Promisc, Leave };
enum class RfPhase : uint8_t {
  Idle = 0,
  StopPromisc,
  AbortScan,
  EnsureSta,
  StartScan,
  Scanning,
  ArmPromisc,
  PromiscOn,
  LeaveDone,
};

RfWant g_rfWant = RfWant::Idle;
RfPhase g_rfPhase = RfPhase::Idle;
uint32_t g_rfGen = 0;
uint32_t g_rfScanStartedAt = 0;
uint32_t g_rfLastPhaseMs = 0;
wifi_promiscuous_cb_t g_rfPromiscCb = nullptr;
int g_rfPromiscChannel = 1;
bool g_rfPromiscReady = false;
volatile int g_rfHopTo = 0;  // >0: apply esp_wifi_set_channel on worker
volatile int g_rfScanCached = WIFI_SCAN_FAILED;  // mirror of scanComplete()

// Snapshot of last completed scan — UI reads ONLY this, never WiFi.* from loop.
struct RfNetSnap {
  char ssid[33];
  uint8_t bssid[6];
  int32_t rssi;
  uint8_t channel;
  wifi_auth_mode_t auth;
};
RfNetSnap g_rfNets[48];
volatile int g_rfNetCount = 0;
uint32_t g_rfSnapGen = 0;
int g_rfSnapSt = -999;  // scanComplete value we last snapped

void rfPublishScanSnap(int st) {
  if (st < 0) {
    g_rfNetCount = 0;
    g_rfSnapSt = st;
    return;
  }
  if (st == g_rfSnapSt) return;
  int n = st < 48 ? st : 48;
  for (int i = 0; i < n; i++) {
    RfNetSnap& e = g_rfNets[i];
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0)
      strncpy(e.ssid, "<hidden>", sizeof(e.ssid) - 1);
    else
      strncpy(e.ssid, ssid.c_str(), sizeof(e.ssid) - 1);
    e.ssid[sizeof(e.ssid) - 1] = 0;
    const uint8_t* bs = WiFi.BSSID(i);
    if (bs) memcpy(e.bssid, bs, 6);
    else memset(e.bssid, 0, 6);
    e.rssi = WiFi.RSSI(i);
    e.channel = (uint8_t)WiFi.channel(i);
    e.auth = WiFi.encryptionType(i);
  }
  g_rfNetCount = n;
  g_rfSnapSt = st;
  g_rfSnapGen++;
}

SemaphoreHandle_t g_wifiMu = nullptr;
TaskHandle_t g_rfTask = nullptr;

void wifiLock() {
  if (g_wifiMu) xSemaphoreTake(g_wifiMu, portMAX_DELAY);
}
void wifiUnlock() {
  if (g_wifiMu) xSemaphoreGive(g_wifiMu);
}

void wifiRfRequest(RfWant want) {
  g_rfWant = want;
  g_rfGen++;
  g_rfPromiscReady = false;
  // Always re-enter teardown so a new request replaces in-flight work.
  g_rfPhase = RfPhase::StopPromisc;
  if (g_rfTask) xTaskNotifyGive(g_rfTask);
}

void bleStop();  // defined below — WiFi RF and BLE must not overlap

void wifiScanBegin() {
  bleStop();  // shared radio: kill BLE listen before WiFi scan
  g_rfScanCached = WIFI_SCAN_RUNNING;
  g_rfSnapSt = -999;
  wifiRfRequest(RfWant::Scan);
}

void wifiPromiscBegin(wifi_promiscuous_cb_t cb, int channel) {
  bleStop();
  g_rfPromiscCb = cb;
  g_rfPromiscChannel = channel < 1 ? 1 : (channel > 13 ? 13 : channel);
  g_rfHopTo = 0;
  wifiRfRequest(RfWant::Promisc);
}

void wifiRfLeave() {
  g_rfPromiscCb = nullptr;
  g_rfHopTo = 0;
  wifiRfRequest(RfWant::Leave);
}

// UI-safe channel hop — worker applies under its own WiFi ops.
void wifiPromiscHop(int channel) {
  if (channel < 1) channel = 1;
  if (channel > 13) channel = 13;
  g_rfPromiscChannel = channel;
  g_rfHopTo = channel;
  if (g_rfTask) xTaskNotifyGive(g_rfTask);
}

// UI-safe: never touches the driver (RF task publishes the mirror).
int wifiScanState() { return g_rfScanCached; }

void wifiRfServiceOnce() {
  // Exactly one potentially-blocking Wi-Fi call per invocation.
  uint32_t t0 = millis();
  switch (g_rfPhase) {
    case RfPhase::Idle:
      break;

    case RfPhase::StopPromisc:
      esp_wifi_set_promiscuous(false);
      g_rfPromiscReady = false;
      g_rfPhase = RfPhase::AbortScan;
      break;

    case RfPhase::AbortScan: {
      int st = WiFi.scanComplete();
      g_rfScanCached = st;
      if (st == WIFI_SCAN_RUNNING || st >= 0) {
        // Abort in-flight or free prior results before mode/scan churn.
        WiFi.scanDelete();
        g_rfScanCached = WIFI_SCAN_FAILED;
        g_rfNetCount = 0;
        g_rfSnapSt = -999;
      }
      g_rfPhase = RfPhase::EnsureSta;
      break;
    }

    case RfPhase::EnsureSta:
      // Skip WiFi.mode when already STA — mode() is the multi-second stall.
      if (WiFi.getMode() != WIFI_STA) {
        WiFi.mode(WIFI_STA);
      }
      if (g_rfWant == RfWant::Leave) {
        g_rfPhase = RfPhase::LeaveDone;
      } else if (g_rfWant == RfWant::Scan) {
        g_rfPhase = RfPhase::StartScan;
      } else if (g_rfWant == RfWant::Promisc) {
        g_rfPhase = RfPhase::ArmPromisc;
      } else {
        g_rfPhase = RfPhase::Idle;
      }
      break;

    case RfPhase::StartScan:
      // Async scan only. scanDelete already happened in AbortScan.
      g_rfScanCached = WIFI_SCAN_RUNNING;
      WiFi.scanNetworks(true /*async*/, true /*show hidden*/);
      g_rfScanStartedAt = millis();
      g_rfPhase = RfPhase::Scanning;
      break;

    case RfPhase::Scanning: {
      if (g_rfWant != RfWant::Scan) {
        g_rfPhase = RfPhase::StopPromisc;
        break;
      }
      int st = WiFi.scanComplete();
      g_rfScanCached = st;
      if (st == WIFI_SCAN_FAILED) {
        if (g_rfWant == RfWant::Scan) g_rfWant = RfWant::Idle;
        g_rfPhase = (g_rfWant == RfWant::Idle) ? RfPhase::Idle
                                               : RfPhase::StopPromisc;
        break;
      }
      if (st >= 0) {
        rfPublishScanSnap(st);
        break;
      }
      if (millis() - g_rfScanStartedAt > 12000) {
        WiFi.scanDelete();
        g_rfScanCached = WIFI_SCAN_FAILED;
        if (g_rfWant == RfWant::Scan) g_rfWant = RfWant::Idle;
        g_rfPhase = (g_rfWant == RfWant::Idle) ? RfPhase::Idle
                                               : RfPhase::StopPromisc;
      }
      break;
    }

    case RfPhase::ArmPromisc: {
      if (g_rfWant != RfWant::Promisc || !g_rfPromiscCb) {
        g_rfPhase = RfPhase::StopPromisc;
        break;
      }
      WiFi.disconnect(false, false);
      g_rfPhase = RfPhase::PromiscOn;
      break;
    }

    case RfPhase::PromiscOn: {
      if (g_rfWant != RfWant::Promisc || !g_rfPromiscCb) {
        g_rfPhase = RfPhase::StopPromisc;
        break;
      }
      if (!g_rfPromiscReady) {
        wifi_promiscuous_filter_t filt;
        filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
        esp_wifi_set_promiscuous_filter(&filt);
        esp_wifi_set_promiscuous_rx_cb(g_rfPromiscCb);
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_channel(g_rfPromiscChannel, WIFI_SECOND_CHAN_NONE);
        g_rfPromiscReady = true;
      } else {
        // Apply UI-requested hop without touching the radio from the UI core.
        int hop = g_rfHopTo;
        if (hop > 0) {
          g_rfHopTo = 0;
          g_rfPromiscChannel = hop;
          esp_wifi_set_channel(hop, WIFI_SECOND_CHAN_NONE);
        }
      }
      break;
    }

    case RfPhase::LeaveDone:
      // A newer request may have arrived while Leave was in flight — honor it.
      if (g_rfWant == RfWant::Leave) {
        g_rfWant = RfWant::Idle;
        g_rfPhase = RfPhase::Idle;
      } else {
        g_rfPhase = RfPhase::StopPromisc;
      }
      g_rfPromiscReady = false;
      break;
  }
  uint32_t dt = millis() - t0;
  if (dt > g_rfLastPhaseMs) g_rfLastPhaseMs = dt;
}

void rfTaskFn(void*) {
  for (;;) {
    // Run while there is work; sleep longer when idle/steady.
    wifiLock();
    RfPhase before = g_rfPhase;
    wifiRfServiceOnce();
    RfPhase after = g_rfPhase;
    wifiUnlock();

    bool busy = (after != RfPhase::Idle && after != RfPhase::Scanning &&
                 after != RfPhase::PromiscOn && after != before) ||
                (after == RfPhase::StopPromisc || after == RfPhase::AbortScan ||
                 after == RfPhase::EnsureSta || after == RfPhase::StartScan ||
                 after == RfPhase::ArmPromisc || after == RfPhase::LeaveDone);
    if (busy) {
      vTaskDelay(pdMS_TO_TICKS(5));  // yield hard after blocking WiFi.* opcodes
    } else if (after == RfPhase::Scanning || after == RfPhase::PromiscOn) {
      // Poll scanComplete / apply hops without starving UI.
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(40));
    } else {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
    }
  }
}

void wifiRfStartWorker() {
  if (g_rfTask) return;
  g_wifiMu = xSemaphoreCreateMutex();
  // Core 0, low-ish priority so Arduino loop (UI) on core 1 stays responsive.
  xTaskCreatePinnedToCore(rfTaskFn, "pp_rf", 4096, nullptr, 1, &g_rfTask, 0);
}

void wifiScanService() { /* RF worker pumps itself */ }

// Short chip label (list rows / KPIs) — from ESP32 wifi_auth_mode_t.
const char* encLabel(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-E";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
#ifdef WIFI_AUTH_WAPI_PSK
    case WIFI_AUTH_WAPI_PSK: return "WAPI";
#endif
#ifdef WIFI_AUTH_OWE
    case WIFI_AUTH_OWE: return "OWE";
#endif
    default: return "?";
  }
}
// Longer detail-sheet auth string (still ESP32 scan enum).
const char* encLabelLong(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN: return "Open (no auth)";
    case WIFI_AUTH_WEP: return "WEP (weak)";
    case WIFI_AUTH_WPA_PSK: return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK: return "WPA3-SAE";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
#ifdef WIFI_AUTH_WAPI_PSK
    case WIFI_AUTH_WAPI_PSK: return "WAPI-PSK";
#endif
#ifdef WIFI_AUTH_OWE
    case WIFI_AUTH_OWE: return "OWE (opp.)";
#endif
    default: return "Unknown auth";
  }
}
// Band hint from channel number (ESP32 scan channel field).
const char* bandFromChannel(uint8_t ch) {
  if (ch >= 1 && ch <= 14) return "2.4 GHz";
  if (ch >= 36 && ch <= 177) return "5 GHz";
  return "RF ?";
}
// 0 great, 1 ok, 2 warn, 3 danger
int encRisk(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN: return 3;
    case WIFI_AUTH_WEP: return 3;
    case WIFI_AUTH_WPA_PSK: return 2;
    case WIFI_AUTH_WPA_WPA2_PSK: return 1;
    case WIFI_AUTH_WPA2_PSK: return 1;
    case WIFI_AUTH_WPA3_PSK: return 0;
    case WIFI_AUTH_WPA2_WPA3_PSK: return 0;
    default: return 1;
  }
}
uint16_t riskColor(int r) {
  switch (r) {
    case 0: return theme::kGood;
    case 1: return theme::kInk;
    case 2: return theme::kWarn;
    default: return theme::kBad;
  }
}

uint64_t bssidKey(const uint8_t* b) {
  uint64_t k = 0;
  for (int i = 0; i < 6; i++) k = (k << 8) | b[i];
  return k;
}

// Session set of already-rewarded BSSIDs so xp is only granted for new finds.
std::set<uint64_t> g_seenAp;
int g_lastWifiCount = 0;

// ===========================================================================
//  Shared BLE scan (used by Harbor Ledger, Tracker Watch, Spyglass)
// ===========================================================================
struct BleDev {
  String mac;
  String name;
  int rssi;
  uint8_t kind;       // 0 generic, 1 tracker, 2 camera-ish
  uint16_t company;   // BLE SIG company id from manufacturer data (0 = none)
  char detail[22];    // decoded advertisement summary (iBeacon/Eddystone/...)
  char serviceUuid[40];
  uint8_t serviceCount;
  uint8_t addressType;
  uint16_t appearance;
  int8_t txPower;
  bool connectable;
  bool hasAppearance;
  bool hasTxPower;
};
BleDev g_ble[48];
int g_bleCount = 0;
bool g_bleInited = false;
BleDev g_bleWork[48];
int g_bleWorkCount = 0;
volatile bool g_blePublishPending = false;
uint32_t g_bleGeneration = 0;
std::set<uint64_t> g_seenBle;

// A small subset of the Bluetooth SIG "Company Identifiers" registry -- the
// vendors seen most in the wild. Not exhaustive; extend from the official list.
const char* bleCompanyName(uint16_t id) {
  switch (id) {
    case 0x004C: return "Apple";
    case 0x0006: return "Microsoft";
    case 0x00E0: return "Google";
    case 0x0075: return "Samsung";
    case 0x0087: return "Garmin";
    case 0x038F: return "Xiaomi";
    case 0x0499: return "Ruuvi";
    case 0x05A7: return "Sonos";
    case 0x004F: return "APT/Bose";
    case 0x0059: return "Nordic";
    case 0x000D: return "TI";
    case 0x0157: return "Anhui Huami";
    case 0x0001: return "Ericsson";
    case 0x0118: return "Fitbit";
    case 0x0171: return "Amazon";
    case 0x00D2: return "Logitech";
    default: return "";
  }
}

uint64_t macKey(const String& mac) {
  uint64_t k = 0;
  for (size_t i = 0; i < mac.length(); i++) {
    char c = mac[i];
    if (c == ':') continue;
    k = k * 31 + (uint8_t)c;
  }
  return k;
}

// Classify a BLE advertiser as a likely item tracker from public adv data.
uint8_t classifyBle(BLEAdvertisedDevice& d) {
  if (d.haveServiceUUID()) {
    BLEUUID u = d.getServiceUUID();
    String s = String(u.toString().c_str());
    s.toLowerCase();
    if (s.indexOf("feed") >= 0 || s.indexOf("feec") >= 0) return 1;  // Tile
    if (s.indexOf("fd5a") >= 0) return 1;                            // SmartTag
  }
  if (d.haveManufacturerData()) {
    String m = d.getManufacturerData();
    if (m.length() >= 2) {
      uint16_t company = (uint8_t)m[0] | ((uint8_t)m[1] << 8);
      // Apple Find My network beacons advertise company 0x004C.
      if (company == 0x004C && m.length() >= 3) {
        uint8_t type = (uint8_t)m[2];
        if (type == 0x12 || type == 0x07) return 1;  // Find My / pairing
      }
    }
  }
  return 0;
}

// Decode a BLE advertisement's manufacturer / service data into a short,
// human-readable summary from PUBLICLY BROADCAST fields only (company id,
// iBeacon major/minor, Apple Continuity type, Eddystone frame type). No
// connection is made and nothing private is read.
void decodeBle(BLEAdvertisedDevice& d, BleDev& e) {
  e.company = 0;
  e.detail[0] = 0;
  if (d.haveManufacturerData()) {
    String m = d.getManufacturerData();
    int n = m.length();
    if (n >= 2) {
      uint16_t co = (uint8_t)m[0] | ((uint8_t)m[1] << 8);
      e.company = co;
      if (co == 0x004C && n >= 3) {  // Apple: inspect the Continuity type byte
        uint8_t t = (uint8_t)m[2];
        if (t == 0x02 && n >= 24) {  // iBeacon
          uint16_t major = ((uint8_t)m[20] << 8) | (uint8_t)m[21];
          uint16_t minor = ((uint8_t)m[22] << 8) | (uint8_t)m[23];
          snprintf(e.detail, sizeof(e.detail), "iBeacon %u/%u", major, minor);
        } else if (t == 0x12) {
          strncpy(e.detail, "Find My", sizeof(e.detail) - 1);
        } else if (t == 0x10) {
          strncpy(e.detail, "Nearby", sizeof(e.detail) - 1);
        } else if (t == 0x0C) {
          strncpy(e.detail, "Handoff", sizeof(e.detail) - 1);
        } else if (t == 0x07 || t == 0x05) {
          strncpy(e.detail, "Pairing", sizeof(e.detail) - 1);
        } else {
          snprintf(e.detail, sizeof(e.detail), "Apple x%02X", t);
        }
      } else {
        const char* nm = bleCompanyName(co);
        if (nm[0])
          snprintf(e.detail, sizeof(e.detail), "%s", nm);
        else
          snprintf(e.detail, sizeof(e.detail), "Co x%04X", co);
      }
    }
  }
  if (e.detail[0] == 0 && d.haveServiceData()) {
    String us = String(d.getServiceDataUUID().toString().c_str());
    us.toLowerCase();
    if (us.indexOf("feaa") >= 0) {  // Eddystone
      String sd = d.getServiceData();
      uint8_t ft = sd.length() ? (uint8_t)sd[0] : 0xFF;
      const char* fn = ft == 0x00   ? "UID"
                       : ft == 0x10 ? "URL"
                       : ft == 0x20 ? "TLM"
                       : ft == 0x30 ? "EID"
                                    : "?";
      snprintf(e.detail, sizeof(e.detail), "Eddystone %s", fn);
    } else if (us.indexOf("fd5a") >= 0) {
      strncpy(e.detail, "Galaxy SmartTag", sizeof(e.detail) - 1);
    }
  }
}

class BleCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    if (g_bleWorkCount >= (int)(sizeof(g_bleWork) / sizeof(g_bleWork[0])))
      return;
    String mac = String(d.getAddress().toString().c_str());
    for (int i = 0; i < g_bleWorkCount; i++)
      if (g_bleWork[i].mac == mac) return;  // already in this pass
    BleDev& e = g_bleWork[g_bleWorkCount++];
    e.mac = mac;
    e.name = d.haveName() ? String(d.getName().c_str()) : String("");
    e.rssi = d.getRSSI();
    e.kind = classifyBle(d);
    decodeBle(d, e);
    e.serviceUuid[0] = 0;
    e.serviceCount = (uint8_t)min(d.getServiceUUIDCount(), 255);
    if (d.haveServiceUUID()) {
      String uuid = String(d.getServiceUUID().toString().c_str());
      snprintf(e.serviceUuid, sizeof(e.serviceUuid), "%s", uuid.c_str());
    } else if (d.getServiceDataCount() > 0) {
      String uuid = String(d.getServiceDataUUID(0).toString().c_str());
      snprintf(e.serviceUuid, sizeof(e.serviceUuid), "%s", uuid.c_str());
    }
      e.serviceCount = (uint8_t)min(d.getServiceUUIDCount() +
                                        d.getServiceDataCount(),
                                    255);
    e.addressType = d.getAddressType();
    e.connectable = d.isConnectable();
    e.hasAppearance = d.haveAppearance();
    e.appearance = e.hasAppearance ? d.getAppearance() : 0;
    e.hasTxPower = d.haveTXPower();
    e.txPower = e.hasTxPower ? d.getTXPower() : 0;

    uint64_t k = macKey(mac);
    if (g_seenBle.insert(k).second) {
      game::profile.bleSeen++;
      game::addLoot(game::Loot::MessageBottle, 1);
      game::awardXp(2);
    }
  }
};
BLEScan* g_bleScan = nullptr;

void bleEnsureInit() {
  if (g_bleInited) return;
  BLEDevice::init("");
  g_bleScan = BLEDevice::getScan();
  g_bleScan->setAdvertisedDeviceCallbacks(new BleCb(), true);
  g_bleScan->setActiveScan(false);  // passive: listen only, do not probe
  g_bleScan->setInterval(100);
  g_bleScan->setWindow(90);
  g_bleInited = true;
}

// Short, periodic passive BLE listen — async so Harbor never freezes UI.
uint32_t g_bleLastScan = 0;
bool g_bleScanning = false;

void bleScanDone(BLEScanResults) {
  g_bleScanning = false;
  g_blePublishPending = true;
  if (g_bleScan) g_bleScan->clearResults();
}

void bleStop() {
  if (!g_bleInited || !g_bleScan) return;
  if (g_bleScanning) {
    g_bleScan->stop();
    g_bleScanning = false;
  }
}

void bleTick(uint32_t now) {
  if (g_blePublishPending) {
    g_blePublishPending = false;
    g_bleCount = g_bleWorkCount;
    for (int i = 0; i < g_bleCount; i++) g_ble[i] = g_bleWork[i];
    g_bleGeneration++;
  }
  // Shared 2.4 GHz radio — never start BLE while WiFi scan/promisc is armed.
  if (g_rfWant != RfWant::Idle) return;
  bleEnsureInit();
  if (g_bleScanning) return;
  if (now - g_bleLastScan < 3000) return;
  g_bleLastScan = now;
  g_bleWorkCount = 0;
  g_bleScanning = true;
  // duration=1s, callback form returns immediately (no UI freeze).
  g_bleScan->start(1, bleScanDone, false);
}

}  // namespace

// ===========================================================================
//  1. Crow's Nest -- Wi-Fi survey (sort / filter / detail / watch / save)
// ===========================================================================
namespace {
struct CnNet {
  char ssid[33];
  uint8_t bssid[6];
  int32_t rssi;
  uint8_t channel;
  wifi_auth_mode_t auth;
  int8_t rssiDelta;
  bool isNew;
  bool channelChanged;
  bool securityChanged;
};
CnNet cnNets[48];
CnNet cnPreviousNets[48];
int cnCount = 0;
int cnPreviousCount = 0;
int cnNewCount = 0;
int cnGoneCount = 0;
int cnChannelChangeCount = 0;
int cnSecurityChangeCount = 0;
int cnProcessed = -1;
uint32_t cnLast = 0;
int cnScroll = 0;          // first visible row
int cnSort = 0;            // 0=RSSI 1=CH 2=SSID
int cnFilter = 0;          // 0=All 1=Open 2=Secure
int cnDetail = -1;         // index into cnNets, or -1
uint8_t cnDetailBssid[6] = {0};

int cnVisibleIdx[48];
int cnVisibleCount = 0;
int cnChannelFilter = 0;
bool cnSpectrumView = false;

// Pin/Watch — passive RSSI time series for one BSSID (no associate / TX).
bool cnWatching = false;
uint8_t cnWatchBssid[6] = {0};
static constexpr int kCnHist = 48;
int8_t cnRssiHist[kCnHist];
uint8_t cnRssiHistCount = 0;
uint8_t cnRssiHistHead = 0;
int32_t cnWatchLastRssi = -127;
bool cnWatchHaveSample = false;

void cnPushRssi(int32_t rssi) {
  int v = rssi;
  if (v < -127) v = -127;
  if (v > 0) v = 0;
  cnRssiHist[cnRssiHistHead] = (int8_t)v;
  cnRssiHistHead = (uint8_t)((cnRssiHistHead + 1) % kCnHist);
  if (cnRssiHistCount < kCnHist) cnRssiHistCount++;
  cnWatchLastRssi = rssi;
  cnWatchHaveSample = true;
}

void cnClearWatchHist() {
  cnRssiHistCount = 0;
  cnRssiHistHead = 0;
  cnWatchHaveSample = false;
  cnWatchLastRssi = -127;
}

int cnFindBssid(const uint8_t* b) {
  for (int i = 0; i < cnCount; i++) {
    if (memcmp(cnNets[i].bssid, b, 6) == 0) return i;
  }
  return -1;
}

bool cnBssidEq(const uint8_t* a, const uint8_t* b) {
  return memcmp(a, b, 6) == 0;
}

void cnFmtMac(char* out, size_t n, const uint8_t* b) {
  snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3],
           b[4], b[5]);
}

void cnDrawSparkline(int x, int y, int w, int h) {
  auto& g = G();
  g.fillRect(x, y, w, h, theme::kPanelSoft);
  g.drawRect(x, y, w, h, theme::kBorder);
  if (cnRssiHistCount < 2) {
    txt(x + 4, y + (h / 2) - 4, theme::kInkMuted, 1, "watching…");
    return;
  }
  // Map RSSI -90..-30 dBm into sparkline height.
  const int n = cnRssiHistCount;
  int start = ((int)cnRssiHistHead - n + kCnHist) % kCnHist;
  auto sampleY = [&](int8_t rssi) -> int {
    int r = (int)rssi;
    if (r < -90) r = -90;
    if (r > -30) r = -30;
    int t = r - (-90);           // 0..60
    int py = h - 3 - (t * (h - 6)) / 60;
    if (py < 2) py = 2;
    if (py > h - 3) py = h - 3;
    return y + py;
  };
  int x0 = x + 2;
  int prevX = x0;
  int prevY = sampleY(cnRssiHist[start]);
  for (int i = 1; i < n; i++) {
    int idx = (start + i) % kCnHist;
    int xi = x0 + (i * (w - 4)) / (n - 1);
    int yi = sampleY(cnRssiHist[idx]);
    g.drawLine(prevX, prevY, xi, yi, theme::kTeal);
    prevX = xi;
    prevY = yi;
  }
  // Latest pip
  g.fillRect(prevX - 1, prevY - 1, 3, 3, theme::kGold);
}

void cnRebuildVisible() {
  cnVisibleCount = 0;
  for (int i = 0; i < cnCount; i++) {
    bool open = (cnNets[i].auth == WIFI_AUTH_OPEN);
    if (cnFilter == 1 && !open) continue;
    if (cnFilter == 2 && open) continue;
    if (cnChannelFilter && cnNets[i].channel != cnChannelFilter) continue;
    cnVisibleIdx[cnVisibleCount++] = i;
  }
  auto cmp = [](int a, int b) {
    const CnNet& A = cnNets[a];
    const CnNet& B = cnNets[b];
    if (cnSort == 1) {
      if (A.channel != B.channel) return A.channel < B.channel;
      return A.rssi > B.rssi;
    }
    if (cnSort == 2) {
      int c = strcasecmp(A.ssid, B.ssid);
      if (c != 0) return c < 0;
      return A.rssi > B.rssi;
    }
    return A.rssi > B.rssi;
  };
  for (int i = 1; i < cnVisibleCount; i++) {
    int v = cnVisibleIdx[i], j = i;
    while (j > 0 && cmp(v, cnVisibleIdx[j - 1])) {
      cnVisibleIdx[j] = cnVisibleIdx[j - 1];
      j--;
    }
    cnVisibleIdx[j] = v;
  }
  int maxScroll = max(0, cnVisibleCount - 7);  // draw computes exact fit
  if (cnScroll > maxScroll) cnScroll = maxScroll;
}

void cnCapture(int st) {
  (void)st;
  int previousCount = cnCount;
  if (previousCount > 0)
    memcpy(cnPreviousNets, cnNets, previousCount * sizeof(CnNet));
  cnPreviousCount = previousCount;
  bool previousMatched[48] = {};
  cnNewCount = 0;
  cnChannelChangeCount = 0;
  cnSecurityChangeCount = 0;
  int n = g_rfNetCount;
  if (n < 0) n = 0;
  if (n > 48) n = 48;
  cnCount = n;
  for (int i = 0; i < n; i++) {
    CnNet& dst = cnNets[i];
    const RfNetSnap& s = g_rfNets[i];
    memcpy(dst.ssid, s.ssid, sizeof(dst.ssid));
    memcpy(dst.bssid, s.bssid, 6);
    dst.rssi = s.rssi;
    dst.channel = s.channel;
    dst.auth = s.auth;
    dst.rssiDelta = 0;
    dst.isNew = true;
    dst.channelChanged = false;
    dst.securityChanged = false;
    for (int old = 0; old < cnPreviousCount; old++) {
      const CnNet& prev = cnPreviousNets[old];
      if (memcmp(prev.bssid, dst.bssid, 6) != 0) continue;
      previousMatched[old] = true;
      dst.isNew = false;
      int delta = dst.rssi - prev.rssi;
      if (delta < -127) delta = -127;
      if (delta > 127) delta = 127;
      dst.rssiDelta = (int8_t)delta;
      dst.channelChanged = dst.channel != prev.channel;
      dst.securityChanged = dst.auth != prev.auth;
      if (dst.channelChanged) cnChannelChangeCount++;
      if (dst.securityChanged) cnSecurityChangeCount++;
      break;
    }
    if (dst.isNew) cnNewCount++;
  }
  cnGoneCount = 0;
  for (int i = 0; i < cnPreviousCount; i++)
    if (!previousMatched[i]) cnGoneCount++;
  g_lastWifiCount = cnCount;
  cnRebuildVisible();
  // Re-bind detail / watch by BSSID — scan order changes each sweep.
  if (cnDetail >= 0) {
    int i = cnFindBssid(cnDetailBssid);
    cnDetail = i;
  }
  if (cnWatching) {
    int i = cnFindBssid(cnWatchBssid);
    if (i >= 0) cnPushRssi(cnNets[i].rssi);
  }
}

// Snapshot current Nest buffer → /log/nest_*.csv (Chart Room / Captain's Log style).
bool cnSaveSurvey(char* pathOut, size_t pathN) {
  if (pathOut && pathN) pathOut[0] = 0;
  if (!app::sdReady()) {
    tools::toast("Save fail — no SD");
    return false;
  }
  if (cnCount <= 0) {
    tools::toast("Save fail — empty Nest");
    return false;
  }
  if (!SD_MMC.exists("/log")) {
    if (!SD_MMC.mkdir("/log")) {
      tools::toast("Save fail — mkdir /log");
      return false;
    }
  }
  char ts[24];
  char name[48];
  if (gps::formatTimestamp(ts, sizeof(ts))) {
    // YYYY-MM-DD HH:MM:SS → nest_YYYYMMDD_HHMMSS.csv
    char compact[20];
    int p = 0;
    for (const char* c = ts; *c && p < (int)sizeof(compact) - 1; ++c) {
      if (*c >= '0' && *c <= '9') compact[p++] = *c;
    }
    compact[p] = 0;
    if (p >= 14)
      snprintf(name, sizeof(name), "nest_%.8s_%.6s.csv", compact, compact + 8);
    else
      snprintf(name, sizeof(name), "nest_%lu.csv", (unsigned long)(millis() / 1000));
  } else {
    snprintf(name, sizeof(name), "nest_%lu.csv", (unsigned long)(millis() / 1000));
    snprintf(ts, sizeof(ts), "uptime-%lu", (unsigned long)(millis() / 1000));
  }
  char path[64];
  snprintf(path, sizeof(path), "/log/%s", name);
  if (pathOut && pathN) snprintf(pathOut, pathN, "%s", path);

  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    tools::toast("Save fail — open");
    return false;
  }
  f.println(
      "timestamp,SSID,BSSID,RSSI,channel,auth,vendor,lat,lon,alt");
  bool fix = gps::hasFix();
  double lat = fix ? gps::latitude() : 0.0;
  double lon = fix ? gps::longitude() : 0.0;
  double alt = fix ? gps::altitudeM() : 0.0;
  for (int i = 0; i < cnCount; i++) {
    const CnNet& n = cnNets[i];
    char bssid[18];
    cnFmtMac(bssid, sizeof(bssid), n.bssid);
    char ssid[33];
    strncpy(ssid, n.ssid, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = 0;
    for (char* p = ssid; *p; ++p)
      if (*p == ',' || *p == '\n' || *p == '\r') *p = ' ';
    const char* ven = oui::vendor(n.bssid);
    char vendor[24];
    strncpy(vendor, ven[0] ? ven : "unknown", sizeof(vendor) - 1);
    vendor[sizeof(vendor) - 1] = 0;
    for (char* p = vendor; *p; ++p)
      if (*p == ',') *p = ' ';
    if (fix) {
      f.printf("%s,%s,%s,%d,%u,%s,%s,%.6f,%.6f,%.1f\n", ts, ssid, bssid,
               (int)n.rssi, (unsigned)n.channel, encLabel(n.auth), vendor, lat,
               lon, alt);
    } else {
      f.printf("%s,%s,%s,%d,%u,%s,%s,,,\n", ts, ssid, bssid, (int)n.rssi,
               (unsigned)n.channel, encLabel(n.auth), vendor);
    }
  }
  f.close();
  tools::toast("Saved %d → %s", cnCount, name);
  return true;
}

void cnOpen() {
  cnProcessed = -1;
  cnDetail = -1;
  cnScroll = 0;
  cnFilter = 0;  // never open into a filter that hides every row
  cnSort = 0;
  cnChannelFilter = 0;
  cnSpectrumView = false;
  // Keep an active watch across reopen so Pin survives Back→Nest.
  wifiScanBegin();  // arm only — RF deferred
  cnLast = millis();
}
void cnTick(uint32_t now) {
  int st = wifiScanState();
  if (st >= 0 && st != cnProcessed) {
    cnProcessed = st;
    cnCapture(st);
    cnLast = now;
    for (int i = 0; i < cnCount; i++) {
      uint64_t k = bssidKey(cnNets[i].bssid);
      if (g_seenAp.insert(k).second) {
        game::profile.apSeen++;
        game::addLoot(game::Loot::ChartFragment, 1);
        if (game::awardXp(3)) tools::toast("Level up!");
      }
    }
    if (!cnWatching) tools::toast("Charted %d networks", st);
  }
  // Keep the AP list live; watch/detail views poll faster for RSSI history.
  const uint32_t refreshMs = cnWatching ? 3500u : (cnDetail >= 0 ? 6500u : 12000u);
  if (st >= 0 && st == cnProcessed && now - cnLast > refreshMs) {
    wifiScanBegin();
    cnLast = now;
    cnProcessed = -1;
  } else if (st < 0 && now - cnLast > 8000) {
    wifiScanBegin();
    cnLast = now;
  }
}
void cnClose() {
  wifiRfLeave();
  cnDetail = -1;
  // Watch state intentionally kept so reopening Nest resumes the pin.
}
void cnDrawDetail(int x, int y, int w, int h) {
  body(x, y, w, h);
  if (cnDetail < 0 || cnDetail >= cnCount) {
    txt(x + 8, y + 8, theme::kInkDim, 1, "No network selected.");
    btn(x + 8, y + h - 30, 90, 24, "BACK", false);
    return;
  }
  const CnNet& n = cnNets[cnDetail];
  char mac[18];
  cnFmtMac(mac, sizeof(mac), n.bssid);
  const char* ven = oui::vendor(n.bssid);
  const char* band = bandFromChannel(n.channel);
  int risk = encRisk(n.auth);
  uint16_t rc = riskColor(risk);

  // Detail sheet hero KPIs — RSSI / channel+band / auth (risk-colored)
  char vRssi[8], vCh[12], vAuth[10];
  snprintf(vRssi, sizeof(vRssi), "%d", (int)n.rssi);
  snprintf(vCh, sizeof(vCh), "%u", (unsigned)n.channel);
  snprintf(vAuth, sizeof(vAuth), "%s", encLabel(n.auth));
  const char* vals[] = {vRssi, vCh, vAuth};
  char labCh[12];
  snprintf(labCh, sizeof(labCh), "%s", band);  // e.g. "2.4 GHz"
  const char* labs[] = {"dBm", labCh, "auth"};
  uint16_t cols[] = {theme::kTeal, theme::kCyan, rc};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 2;

  char buf[48];
  snprintf(buf, sizeof(buf), "%s", n.ssid);
  gfxu::drawKVRow(G(), x + 6, below, w - 12, 16, "SSID", buf, theme::kInk);
  snprintf(buf, sizeof(buf), "%s", mac);
  gfxu::drawKVRow(G(), x + 6, below + 18, w - 12, 16, "BSSID", buf, theme::kCyan);
  snprintf(buf, sizeof(buf), "%s", ven[0] ? ven : "unknown");
  gfxu::drawKVRow(G(), x + 6, below + 36, w - 12, 16, "Vendor", buf, theme::kTeal);
  snprintf(buf, sizeof(buf), "ch%u · %s", (unsigned)n.channel, band);
  gfxu::drawKVRow(G(), x + 6, below + 54, w - 12, 16, "Band", buf, theme::kCyan);
  snprintf(buf, sizeof(buf), "%s", encLabelLong(n.auth));
  gfxu::drawKVRow(G(), x + 6, below + 72, w - 12, 16, "Auth", buf, rc);

  bool watchingThis = cnWatching && cnBssidEq(cnWatchBssid, n.bssid);
  int sparkY = below + 92;
  if (watchingThis) {
    char wr[40];
    if (cnWatchHaveSample)
      snprintf(wr, sizeof(wr), "Watch %d dBm · %u samples", (int)cnWatchLastRssi,
               (unsigned)cnRssiHistCount);
    else
      snprintf(wr, sizeof(wr), "Watch armed · waiting for sweep");
    txt(x + 8, sparkY, theme::kGold, 1, "%s", wr);
    cnDrawSparkline(x + 6, sparkY + 12, w - 48, 28);
    rssiBars(x + w - 36, sparkY + 18, cnWatchHaveSample ? (int)cnWatchLastRssi
                                                        : (int)n.rssi);
  } else {
    rssiBars(x + w - 36, sparkY, n.rssi);
    txt(x + 8, sparkY + 4, theme::kInkMuted, 1, "Tap WATCH to pin RSSI");
  }

  // BACK | WATCH | SAVE
  btn(x + 6, y + h - 30, 70, 24, "BACK", false);
  btn(x + 82, y + h - 30, 78, 24, watchingThis ? "UNPIN" : "WATCH",
      watchingThis, watchingThis ? theme::kGold : 0);
  btn(x + 166, y + h - 30, 70, 24, "SAVE", true);
  btn(x + 242, y + h - 30, 70, 24, "SCAN", false);
}

void cnDrawList(int x, int y, int w, int h) {
  body(x, y, w, h);
  int st = wifiScanState();

  // Bridge Console KPIs — always draw so the body is never an empty green void
  // while scanning (v0.5.0 early-return made Crow's Nest look blank).
  int openN = 0, best = -999;
  for (int i = 0; i < cnCount; i++) {
    if (cnNets[i].auth == WIFI_AUTH_OPEN) openN++;
    if (cnNets[i].rssi > best) best = cnNets[i].rssi;
  }
  char vNets[8], vOpen[8], vBest[8];
  if (st < 0 && cnCount == 0) {
    snprintf(vNets, sizeof(vNets), "-");
    snprintf(vOpen, sizeof(vOpen), "-");
    snprintf(vBest, sizeof(vBest), "-");
  } else {
    snprintf(vNets, sizeof(vNets), "%d", cnCount);
    snprintf(vOpen, sizeof(vOpen), "%d", openN);
    snprintf(vBest, sizeof(vBest), "%d", cnCount ? best : 0);
  }
  const char* vals[] = {vNets, vOpen, vBest};
  const char* labs[] = {"nets", "open", "best dB"};
  uint16_t cols[] = {theme::kTeal, openN ? theme::kWarn : theme::kGood,
                     theme::kCyan};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 2;

  // Field Tablet chip toolbar — sort / filter / SAVE
  gfxu::drawToolbar(G(), x + 4, below, w - 8, 20);
  const char* sorts[] = {"RSSI", "CH", "SSID"};
  for (int i = 0; i < 3; i++) {
    int bx = x + 8 + i * 40;
    chip(bx, below + 2, 38, theme::kChipH, sorts[i], cnSort == i, theme::kGold);
  }
  const char* filters[] = {"All", "Open", "Sec"};
  for (int i = 0; i < 3; i++) {
    int bx = x + 130 + i * 34;
    chip(bx, below + 2, 32, theme::kChipH, filters[i], cnFilter == i,
         theme::kTeal);
  }
  chip(x + 236, below + 2, 44, theme::kChipH, "SAVE", false, theme::kGold);
  chip(x + 284, below + 2, 28, theme::kChipH, "MAP", cnSpectrumView,
       theme::kCyan);

  constexpr int kRh = theme::kRowH;
  const int listTop = below + 22;
  const int listBot = y + h - 12;  // leave hint line
  const int listH = max(0, listBot - listTop);
  const int kRows = max(1, listH / kRh);  // fit remaining Y (KPI must not eat list)

  if (st < 0 && cnCount == 0) {
    txt(x + 12, listTop + 4, theme::kInkDim, 1, "Sweeping the horizon...");
    txt(x + 12, listTop + 16, theme::kInkMuted, 1, "passive scan · no associate");
    txt(x + 8, y + h - 10, theme::kInkMuted, 1, "SAVE=/log · tap empty=rescan");
    return;
  }

  // Clip list so rows never paint under the hint / past the panel.
  if (listH > 0) G().setClipRect(x + 4, listTop, w - 36, listH);

  int rows = min(cnVisibleCount - cnScroll, kRows);
  for (int r = 0; r < rows; r++) {
    int idx = cnVisibleIdx[cnScroll + r];
    const CnNet& n = cnNets[idx];
    int ry = listTop + r * kRh;
    if (r & 1) G().fillRect(x + 4, ry, w - 32, kRh, theme::kPanelSoft);
    bool pinned = cnWatching && cnBssidEq(cnWatchBssid, n.bssid);
    if (pinned) G().fillRect(x + 4, ry, 3, kRh, theme::kGold);
    rssiBars(x + 8, ry + 4, n.rssi);
    char ssid[18];
    strncpy(ssid, n.ssid, 17);
    ssid[17] = 0;
    txt(x + 28, ry + 2, pinned ? theme::kGold : theme::kInk, 1, "%s", ssid);
    const char* ven = oui::vendor(n.bssid);
    const char* band = bandFromChannel(n.channel);
    char sec[28];
    if (ven[0])
      snprintf(sec, sizeof(sec), "%s  ch%u", ven, (unsigned)n.channel);
    else
      snprintf(sec, sizeof(sec), "%s ch%u", band, (unsigned)n.channel);
    if (strlen(sec) > 22) sec[22] = 0;
    txt(x + 28, ry + 10, theme::kInkMuted, 1, "%s", sec);
    char change[8] = "";
    uint16_t changeColor = theme::kInkMuted;
    if (n.isNew) {
      snprintf(change, sizeof(change), "NEW");
      changeColor = theme::kGood;
    } else if (n.securityChanged) {
      snprintf(change, sizeof(change), "SEC");
      changeColor = theme::kBad;
    } else if (n.channelChanged) {
      snprintf(change, sizeof(change), "CHG");
      changeColor = theme::kWarn;
    } else if (n.rssiDelta >= 8 || n.rssiDelta <= -8) {
      snprintf(change, sizeof(change), "%+ddB", (int)n.rssiDelta);
      changeColor = n.rssiDelta > 0 ? theme::kGood : theme::kWarn;
    }
    if (change[0]) txt(x + 180, ry + 5, changeColor, 1, "%s", change);
    txt(x + 234, ry + 5, riskColor(encRisk(n.auth)), 1, "%s", encLabel(n.auth));
    G().drawFastHLine(x + 8, ry + kRh - 1, w - 40, theme::kBorder);
  }
  G().clearClipRect();

  if (cnVisibleCount == 0) {
    if (cnCount == 0)
      txt(x + 12, listTop + 4, theme::kInkDim, 1, "No networks in sight");
    else
      txt(x + 12, listTop + 4, theme::kWarn, 1, "No nets match filter");
  }

  if (cnScroll > 0)
    btn(x + w - 30, listTop, 26, 20, "^", false);
  if (cnScroll + kRows < cnVisibleCount)
    btn(x + w - 30, y + h - 28, 26, 20, "v", false);
  char hint[64];
  if (cnChannelFilter)
    snprintf(hint, sizeof(hint), "CH %02d · MAP=chart · sweep +%d/-%d",
             cnChannelFilter, cnNewCount, cnGoneCount);
  else if (cnWatching)
    snprintf(hint, sizeof(hint), "PIN active · row=detail · sweep +%d/-%d",
             cnNewCount, cnGoneCount);
  else
    snprintf(hint, sizeof(hint), "row=detail · MAP=channels · sweep +%d/-%d",
             cnNewCount, cnGoneCount);
  gfxu::printFit(G(), x + 8, y + h - 10, w - 16, theme::kInkMuted, 1, hint);
}

void cnDrawSpectrum(int x, int y, int w, int h) {
  body(x, y, w, h);
  int openCount = 0;
  int channelCount[15] = {};
  int channelStrong[15] = {};
  int channelMedium[15] = {};
  int channelWeak[15] = {};
  int maxLoad = 0;
  int busiestChannel = 0;
  for (int i = 0; i < cnCount; i++) {
    const CnNet& n = cnNets[i];
    if (n.auth == WIFI_AUTH_OPEN) openCount++;
    if (n.channel < 1 || n.channel > 14) continue;
    int ch = n.channel;
    channelCount[ch]++;
    if (n.rssi >= -55) channelStrong[ch]++;
    else if (n.rssi >= -75) channelMedium[ch]++;
    else channelWeak[ch]++;
    if (channelCount[ch] > maxLoad) {
      maxLoad = channelCount[ch];
      busiestChannel = ch;
    }
  }

  char vAps[8], vOpen[8], vDelta[16];
  snprintf(vAps, sizeof(vAps), "%d", cnCount);
  snprintf(vOpen, sizeof(vOpen), "%d", openCount);
  snprintf(vDelta, sizeof(vDelta), "+%d/-%d", cnNewCount, cnGoneCount);
  const char* values[] = {vAps, vOpen, vDelta};
  const char* labels[] = {"APs", "open", "new / lost"};
  const uint16_t colors[] = {theme::kCyan,
                             openCount ? theme::kBad : theme::kGood,
                             (cnNewCount || cnGoneCount) ? theme::kGold
                                                         : theme::kInkDim};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, values, labels, colors) + 2;
  txt(x + 8, below, theme::kInkDim, 1,
      "2.4 GHz AP channel occupancy · tap a channel to focus");

  const int chartX = x + 9;
  const int chartW = w - 18;
  const int top = y + 55;
  const int base = y + 130;
  const int plotHeight = base - top;
  auto& g = G();
  for (int line = 0; line < 4; line++) {
    int gy = base - (plotHeight * line) / 3;
    g.drawFastHLine(chartX, gy, chartW, line == 0 ? theme::kBorderHi
                                                  : theme::kBorder);
    if (line == 0 || line == 2 || line == 3) {
      int tick = line == 0 ? maxLoad : (line == 2 ? maxLoad / 2 : 0);
      char tickLabel[5];
      snprintf(tickLabel, sizeof(tickLabel), "%d", tick);
      gfxu::printFit(g, x + 1, gy - 4, 8, theme::kInkMuted, 1, tickLabel);
    }
  }
  const int slot = chartW / 14;
  const int barW = 10;
  for (int ch = 1; ch <= 14; ch++) {
    int center = chartX + (ch - 1) * slot + slot / 2;
    int weakH = maxLoad ? channelWeak[ch] * (plotHeight - 13) / maxLoad : 0;
    int medH = maxLoad ? channelMedium[ch] * (plotHeight - 13) / maxLoad : 0;
    int strongH = maxLoad ? channelStrong[ch] * (plotHeight - 13) / maxLoad : 0;
    int cursor = base;
    if (weakH) {
      cursor -= weakH;
      g.fillRect(center - barW / 2, cursor, barW, weakH, theme::kLocked);
    }
    if (medH) {
      cursor -= medH;
      g.fillRect(center - barW / 2, cursor, barW, medH, theme::kCyan);
    }
    if (strongH) {
      cursor -= strongH;
      g.fillRect(center - barW / 2, cursor, barW, strongH, theme::kGold);
    }
    if (channelCount[ch])
      gfxu::printCentered(g, center - slot / 2, cursor - 10, slot, 8,
                          theme::kInk, 1, String(channelCount[ch]).c_str());
    char channelLabel[4];
    snprintf(channelLabel, sizeof(channelLabel), "%d", ch);
    uint16_t labelColor = cnChannelFilter == ch ? theme::kGold
                                                : theme::kInkMuted;
    gfxu::printCentered(g, center - slot / 2, base + 2, slot, 9, labelColor, 1,
                        channelLabel);
  }

  int legendY = y + 144;
  g.fillRect(x + 10, legendY + 1, 5, 5, theme::kGold);
  txt(x + 18, legendY, theme::kInkMuted, 1, "strong");
  g.fillRect(x + 66, legendY + 1, 5, 5, theme::kCyan);
  txt(x + 74, legendY, theme::kInkMuted, 1, "medium");
  g.fillRect(x + 132, legendY + 1, 5, 5, theme::kLocked);
  txt(x + 140, legendY, theme::kInkMuted, 1, "weak");
  char summary[52];
  snprintf(summary, sizeof(summary), "Busy ch %02d (%d AP) · moved %d · auth changed %d",
           busiestChannel, maxLoad, cnChannelChangeCount, cnSecurityChangeCount);
  gfxu::printFit(g, x + 10, y + 160, w - 20, theme::kInkDim, 1, summary);

  const int buttonY = y + h - 28;
  btn(x + 6, buttonY, 68, 24, "LIST", true, theme::kCyan);
  btn(x + 82, buttonY, 68, 24, "SAVE", false);
  btn(x + 158, buttonY, 68, 24, "SCAN", false);
  btn(x + 234, buttonY, 78, 24, "ALL CH", false);
}

bool cnSpectrumTouch(int16_t x, int16_t y) {
  if (y >= 208) {
    if (x < 78) {
      cnSpectrumView = false;
    } else if (x < 154) {
      cnSaveSurvey(nullptr, 0);
    } else if (x < 230) {
      wifiScanBegin();
      cnLast = millis();
      cnProcessed = -1;
    } else {
      cnChannelFilter = 0;
      cnSpectrumView = false;
      cnScroll = 0;
      cnRebuildVisible();
    }
    return true;
  }
  const int chartX = 9;
  const int chartW = theme::kScreenW - 18;
  if (y >= 88 && y <= 176 && x >= chartX && x < chartX + chartW) {
    int slot = chartW / 14;
    int channel = (x - chartX) / slot + 1;
    if (channel >= 1 && channel <= 14) {
      cnChannelFilter = cnChannelFilter == channel ? 0 : channel;
      cnSpectrumView = false;
      cnScroll = 0;
      cnRebuildVisible();
      tools::toast(cnChannelFilter ? "Focus: channel %d" : "Channel focus cleared",
                   cnChannelFilter);
    }
    return true;
  }
  return true;
}

void cnDraw(int x, int y, int w, int h) {
  if (cnSpectrumView) cnDrawSpectrum(x, y, w, h);
  else if (cnDetail >= 0) cnDrawDetail(x, y, w, h);
  else cnDrawList(x, y, w, h);
}
bool cnTouch(int16_t x, int16_t y) {
  if (cnSpectrumView) return cnSpectrumTouch(x, y);
  // Absolute screen coords. Layout: KPI + gap + toolbar(20) + list.
  // Must match cnDrawList: below = ct+2+kKpiH+2, listTop = below+22.
  int ly = y;
  int cx = x;
  const int ct = contentTop();
  constexpr int kRh = theme::kRowH;
  const int kToolbarY = ct + 2 + theme::kKpiH + 2;
  const int kListTop = kToolbarY + 22;
  const int kListBot = theme::kScreenH - 12;
  const int kRows = max(1, (kListBot - kListTop) / kRh);
  if (cnDetail >= 0) {
    if (ly >= 204 && ly <= 240) {
      if (cx >= 6 && cx <= 76) {
        cnDetail = -1;
        return true;
      }
      if (cx >= 82 && cx <= 160) {
        // WATCH / UNPIN
        if (cnDetail >= 0 && cnDetail < cnCount) {
          const CnNet& n = cnNets[cnDetail];
          if (cnWatching && cnBssidEq(cnWatchBssid, n.bssid)) {
            cnWatching = false;
            cnClearWatchHist();
            tools::toast("Watch cleared");
          } else {
            memcpy(cnWatchBssid, n.bssid, 6);
            cnWatching = true;
            cnClearWatchHist();
            cnPushRssi(n.rssi);
            tools::toast("Watching BSSID");
            wifiScanBegin();
            cnLast = millis();
            cnProcessed = -1;
          }
        }
        return true;
      }
      if (cx >= 166 && cx <= 236) {
        cnSaveSurvey(nullptr, 0);
        return true;
      }
      if (cx >= 242 && cx <= 312) {
        wifiScanBegin();
        cnLast = millis();
        cnProcessed = -1;
        return true;
      }
    }
    // Tap elsewhere on detail keeps the sheet (don't dismiss accidentally).
    return true;
  }
  // Sort / filter / SAVE chips in toolbar
  if (ly >= kToolbarY && ly <= kToolbarY + 22) {
    for (int i = 0; i < 3; i++) {
      int bx = 8 + i * 40;
      if (cx >= bx && cx <= bx + 38) {
        cnSort = i;
        cnRebuildVisible();
        return true;
      }
    }
    for (int i = 0; i < 3; i++) {
      int bx = 130 + i * 34;
      if (cx >= bx && cx <= bx + 32) {
        cnFilter = i;
        cnScroll = 0;
        cnRebuildVisible();
        return true;
      }
    }
    if (cx >= 236 && cx <= 280) {
      cnSaveSurvey(nullptr, 0);
      return true;
    }
    if (cx >= 284 && cx <= 312) {
      cnSpectrumView = !cnSpectrumView;
      return true;
    }
  }
  if (cx >= 290) {
    if (ly < kListTop + 40) {
      if (cnScroll > 0) cnScroll--;
      return true;
    }
    if (ly > 180) {
      if (cnScroll + kRows < cnVisibleCount) cnScroll++;
      return true;
    }
  }
  if (ly >= kListTop && ly < kListTop + kRows * kRh) {
    int r = (ly - kListTop) / kRh;
    if (r >= 0 && cnScroll + r < cnVisibleCount) {
      cnDetail = cnVisibleIdx[cnScroll + r];
      if (cnDetail >= 0 && cnDetail < cnCount)
        memcpy(cnDetailBssid, cnNets[cnDetail].bssid, 6);
      return true;
    }
  }
  wifiScanBegin();
  cnLast = millis();
  cnProcessed = -1;
  return true;
}
}  // namespace

// ===========================================================================
//  2. Harbor Ledger -- BLE discovery
// ===========================================================================
namespace {
int hlFilter = 0;  // all, named, identified, tracker
int hlSort = 0;    // RSSI, name
int hlScroll = 0;
int hlVisible[48];
int hlVisibleCount = 0;
uint32_t hlGeneration = 0;
bool hlHasDetail = false;
BleDev hlDetail{};

void hlRebuild() {
  hlVisibleCount = 0;
  for (int i = 0; i < g_bleCount; i++) {
    const BleDev& d = g_ble[i];
    bool named = d.name.length() > 0;
    bool identified = d.company || d.serviceUuid[0] || d.detail[0];
    if (hlFilter == 1 && !named) continue;
    if (hlFilter == 2 && !identified) continue;
    if (hlFilter == 3 && d.kind != 1) continue;
    hlVisible[hlVisibleCount++] = i;
  }
  auto before = [](int a, int b) {
    if (hlSort == 1) {
      const char* an = g_ble[a].name.length() ? g_ble[a].name.c_str()
                                               : g_ble[a].mac.c_str();
      const char* bn = g_ble[b].name.length() ? g_ble[b].name.c_str()
                                               : g_ble[b].mac.c_str();
      int cmp = strcasecmp(an, bn);
      if (cmp) return cmp < 0;
    }
    return g_ble[a].rssi > g_ble[b].rssi;
  };
  for (int i = 1; i < hlVisibleCount; i++) {
    int index = hlVisible[i], j = i;
    while (j > 0 && before(index, hlVisible[j - 1])) {
      hlVisible[j] = hlVisible[j - 1];
      j--;
    }
    hlVisible[j] = index;
  }
  int maxScroll = max(0, hlVisibleCount - 7);
  if (hlScroll > maxScroll) hlScroll = maxScroll;
}

void hlOpen() {
  hlFilter = 0;
  hlSort = 0;
  hlScroll = 0;
  hlHasDetail = false;
  hlGeneration = g_bleGeneration;
  hlRebuild();
  g_bleLastScan = 0;
}
void hlTick(uint32_t now) {
  bleTick(now);
  if (hlGeneration != g_bleGeneration) {
    hlGeneration = g_bleGeneration;
    hlRebuild();
  }
}
void hlClose() {
  bleStop();
  hlHasDetail = false;
}

void hlDrawDetail(int x, int y, int w, int h) {
  body(x, y, w, h);
  const BleDev& d = hlDetail;
  char rssi[12], type[16], count[8], mac[20], service[44], vendor[24];
  snprintf(rssi, sizeof(rssi), "%d", d.rssi);
  snprintf(type, sizeof(type), "%s", d.kind == 1 ? "TRACKER" : "DEVICE");
  snprintf(count, sizeof(count), "%u", d.serviceCount);
  snprintf(mac, sizeof(mac), "%s", d.mac.c_str());
  snprintf(service, sizeof(service), "%s", d.serviceUuid[0] ? d.serviceUuid
                                                             : "not advertised");
  const char* vendorName = oui::vendorStr(d.mac);
  snprintf(vendor, sizeof(vendor), "%s", vendorName[0] ? vendorName : "unknown");
  const char* values[] = {rssi, type, count};
  const char* labels[] = {"RSSI dBm", "class", "services"};
  const uint16_t colors[] = {theme::kCyan,
                             d.kind == 1 ? theme::kWarn : theme::kGood,
                             d.serviceCount ? theme::kTeal : theme::kInkDim};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, values, labels, colors) + 3;
  gfxu::drawKVRow(G(), x + 6, below, w - 12, 18, "Address", mac, theme::kCyan);
  gfxu::drawKVRow(G(), x + 6, below + 20, w - 12, 18, "Vendor", vendor,
                  theme::kTeal);
  gfxu::drawKVRow(G(), x + 6, below + 40, w - 12, 18, "Service UUID", service,
                  theme::kInk);
  char power[20], addressType[20], appearance[20];
  snprintf(power, sizeof(power), "%s%d dBm", d.hasTxPower ? "" : "n/a ",
           d.hasTxPower ? d.txPower : 0);
  snprintf(addressType, sizeof(addressType), "%u", d.addressType);
  if (d.hasAppearance)
    snprintf(appearance, sizeof(appearance), "0x%04X", d.appearance);
  else
    snprintf(appearance, sizeof(appearance), "not advertised");
  gfxu::drawKVRow(G(), x + 6, below + 60, w - 12, 18, "TX power", power);
  gfxu::drawKVRow(G(), x + 6, below + 80, w - 12, 18, "Address type",
                  addressType);
  gfxu::drawKVRow(G(), x + 6, below + 100, w - 12, 18, "Appearance",
                  appearance);
  txt(x + 10, y + h - 42, theme::kInkMuted, 1,
      d.connectable ? "Advertising only · not connected"
                    : "Non-connectable advertisement · public fields only");
  btn(x + 6, y + h - 24, 72, 20, "BACK", false);
}

void hlDraw(int x, int y, int w, int h) {
  if (hlHasDetail) {
    hlDrawDetail(x, y, w, h);
    return;
  }
  body(x, y, w, h);
  int named = 0, identified = 0, trackers = 0;
  for (int i = 0; i < g_bleCount; i++) {
    if (g_ble[i].name.length()) named++;
    if (g_ble[i].company || g_ble[i].serviceUuid[0] || g_ble[i].detail[0])
      identified++;
    if (g_ble[i].kind == 1) trackers++;
  }
  char vSeen[8], vNamed[8], vIds[8], vTrack[8];
  snprintf(vSeen, sizeof(vSeen), "%d", g_bleCount);
  snprintf(vNamed, sizeof(vNamed), "%d", named);
  snprintf(vIds, sizeof(vIds), "%d", identified);
  snprintf(vTrack, sizeof(vTrack), "%d", trackers);
  const char* values[] = {vSeen, vNamed, vIds, vTrack};
  const char* labels[] = {"seen", "named", "decoded", "trackers"};
  const uint16_t colors[] = {theme::kCyan, theme::kInk, theme::kTeal,
                             trackers ? theme::kWarn : theme::kGood};
  int below = kpiStrip(x + 4, y + 2, w - 8, 4, values, labels, colors) + 2;
  gfxu::drawToolbar(G(), x + 4, below, w - 8, 18);
  chip(x + 6, below + 1, 38, 16, "ALL", hlFilter == 0);
  chip(x + 46, below + 1, 46, 16, "NAMED", hlFilter == 1);
  chip(x + 94, below + 1, 40, 16, "ID", hlFilter == 2);
  chip(x + 136, below + 1, 46, 16, "TAG", hlFilter == 3, theme::kWarn);
  char sort[10];
  snprintf(sort, sizeof(sort), "%s", hlSort ? "NAME" : "RSSI");
  chip(x + 184, below + 1, 52, 16, sort, false, theme::kGold);
  chip(x + 240, below + 1, 72, 16, "RESCAN", false, theme::kCyan);

  int listTop = below + 20;
  constexpr int rowH = 18;
  int rows = min(hlVisibleCount - hlScroll, max(0, (y + h - 12 - listTop) / rowH));
  if (!g_bleCount) {
    gfxu::printFit(G(), x + 12, listTop + 8, w - 24, theme::kInkDim, 1,
                   g_bleScanning ? "Scanning · awaiting first pass"
                                 : "No devices · waiting for BLE scan");
  }
  for (int row = 0; row < rows; row++) {
    int index = hlVisible[hlScroll + row];
    const BleDev& d = g_ble[index];
    int ry = listTop + row * rowH;
    if (row & 1) G().fillRect(x + 4, ry, w - 8, rowH, theme::kPanelSoft);
    rssiBars(x + 8, ry + 3, d.rssi);
    String label = d.name.length() ? d.name : d.mac;
    if (label.length() > 24) label = label.substring(0, 24);
    uint16_t color = d.kind == 1 ? theme::kWarn : theme::kInk;
    gfxu::printFit(G(), x + 30, ry + 1, 170, color, 1, label.c_str());
    char rssiText[12];
    snprintf(rssiText, sizeof(rssiText), "%d dBm", d.rssi);
    gfxu::printFit(G(), x + w - 52, ry + 1, 46, theme::kInkDim, 1, rssiText);
    const char* vendorName = oui::vendorStr(d.mac);
    char meta[44];
    if (d.detail[0])
      snprintf(meta, sizeof(meta), "%s%s", d.detail,
               d.connectable ? " · connectable" : "");
    else
      snprintf(meta, sizeof(meta), "%s%s", vendorName[0] ? vendorName : "BLE",
               d.connectable ? " · connectable" : " · beacon");
    gfxu::printFit(G(), x + 30, ry + 9, 170, theme::kInkMuted, 1, meta);
    if (d.serviceCount)
      txt(x + 206, ry + 9, theme::kCyan, 1, "%u svc", d.serviceCount);
    G().drawFastHLine(x + 8, ry + rowH - 1, w - 16, theme::kBorder);
  }
  if (hlScroll > 0) btn(x + w - 28, listTop, 24, 18, "^", false);
  if (hlScroll + rows < hlVisibleCount)
    btn(x + w - 28, y + h - 30, 24, 18, "v", false);
  const char* scanLabel = g_bleScanning ? "Passive scan · previous sweep kept"
                                        : "Passive scan · no connection";
  gfxu::printFit(G(), x + 8, y + h - 10, w - 16, theme::kInkMuted, 1,
                 scanLabel);
}

bool hlTouch(int16_t x, int16_t y) {
  if (hlHasDetail) {
    if (y >= theme::kScreenH - 34 && x < 90) hlHasDetail = false;
    return true;
  }
  const int toolbarY = contentTop() + 32;
  if (y >= toolbarY && y < toolbarY + 20) {
    if (x < 45) hlFilter = 0;
    else if (x < 93) hlFilter = 1;
    else if (x < 135) hlFilter = 2;
    else if (x < 183) hlFilter = 3;
    else if (x < 238) hlSort = !hlSort;
    else {
      bleStop();
      g_bleLastScan = 0;
    }
    hlScroll = 0;
    hlRebuild();
    return true;
  }
  const int listTop = toolbarY + 20;
  constexpr int rowH = 18;
  const int listBot = theme::kScreenH - 12;
  const int rows = max(0, (listBot - listTop) / rowH);
  if (x >= theme::kScreenW - 30) {
    if (y < listTop + rowH && hlScroll > 0) hlScroll--;
    else if (y > theme::kScreenH - 34 && hlScroll + rows < hlVisibleCount)
      hlScroll++;
    return true;
  }
  if (y >= listTop && y < listTop + rows * rowH) {
    int row = (y - listTop) / rowH;
    if (hlScroll + row < hlVisibleCount) {
      hlDetail = g_ble[hlVisible[hlScroll + row]];
      hlHasDetail = true;
    }
    return true;
  }
  return true;
}
}  // namespace

// ===========================================================================
//  3. Chart Room -- WiGLE 1.4 wardrive logger (beacon metadata -> SD)
// ===========================================================================
namespace {
const char* kWigleFile = "/wigle.csv";
uint32_t crLoggedSession = 0;
uint32_t crUniqueSession = 0;
uint32_t crSweepCount = 0;
uint32_t crNewLastSweep = 0;
uint32_t crLastSweepAt = 0;
int crLastSweepCount = 0;
bool crPaused = false;
int crProcessed = -1;
bool crHeader = false;
std::set<uint64_t> crSeenBssids;

const char* wigleAuth(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN: return "[ESS]";
    case WIFI_AUTH_WEP: return "[WEP][ESS]";
    case WIFI_AUTH_WPA_PSK: return "[WPA-PSK-TKIP][ESS]";
    case WIFI_AUTH_WPA2_PSK: return "[WPA2-PSK-CCMP][ESS]";
    case WIFI_AUTH_WPA_WPA2_PSK: return "[WPA-PSK-CCMP][WPA2-PSK-CCMP][ESS]";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "[WPA2-EAP-CCMP][ESS]";
    case WIFI_AUTH_WPA3_PSK: return "[WPA3-SAE-CCMP][ESS]";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]";
    default: return "[ESS]";
  }
}

void firstSeen(char* out, size_t n) {
  if (gps::formatTimestamp(out, n)) return;
  uint32_t s = millis() / 1000;
  snprintf(out, n, "2000-01-01 %02lu:%02lu:%02lu", (unsigned long)(s / 3600),
           (unsigned long)((s % 3600) / 60), (unsigned long)(s % 60));
}

void crEnsureHeader() {
  if (crHeader || !app::sdReady()) return;
  if (!SD_MMC.exists(kWigleFile)) {
    File f = SD_MMC.open(kWigleFile, FILE_WRITE);
    if (f) {
      f.println(
          "WigleWifi-1.4,appRelease=pocketpirate,model=ESP32-S3,release=0.5.7,"
          "device=CYD28,display=ILI9341,board=ESP32S3,brand=Hosyond");
      f.println(
          "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,"
          "CurrentLongitude,AltitudeMeters,AccuracyMeters,Type");
      f.close();
    }
  }
  crHeader = true;
}
void crOpen() {
  crProcessed = -1;
  crLoggedSession = 0;
  crUniqueSession = 0;
  crSweepCount = 0;
  crNewLastSweep = 0;
  crLastSweepAt = 0;
  crLastSweepCount = 0;
  crPaused = false;
  crSeenBssids.clear();
  crEnsureHeader();
  wifiScanBegin();
}
void crTick(uint32_t now) {
  (void)now;
  if (crPaused) return;
  int st = wifiScanState();
  if (st >= 0 && st != crProcessed) {
    crProcessed = st;
    g_lastWifiCount = st;
    crSweepCount++;
    crLastSweepAt = millis();
    crLastSweepCount = st;
    crNewLastSweep = 0;
    for (int i = 0; i < g_rfNetCount && i < st; i++) {
      if (crSeenBssids.insert(bssidKey(g_rfNets[i].bssid)).second) {
        crUniqueSession++;
        crNewLastSweep++;
      }
    }
    if (app::sdReady()) {
      File f = SD_MMC.open(kWigleFile, FILE_APPEND);
      if (f) {
        char ts[24];
        firstSeen(ts, sizeof(ts));
        bool fix = gps::hasFix();
        double lat = fix ? gps::latitude() : 0.0;
        double lon = fix ? gps::longitude() : 0.0;
        double alt = fix ? gps::altitudeM() : 0.0;
        double acc = fix ? gps::hdop() * 5.0 : 0.0;  // rough meters from HDOP
        for (int i = 0; i < g_rfNetCount && i < st; i++) {
          const RfNetSnap& s = g_rfNets[i];
          char bssid[18];
          snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                   s.bssid[0], s.bssid[1], s.bssid[2], s.bssid[3], s.bssid[4],
                   s.bssid[5]);
          char ssid[33];
          strncpy(ssid, s.ssid, sizeof(ssid) - 1);
          ssid[sizeof(ssid) - 1] = 0;
          for (char* p = ssid; *p; ++p)
            if (*p == ',') *p = ' ';
          if (fix) {
            f.printf("%s,%s,%s,%s,%d,%d,%.6f,%.6f,%.1f,%.1f,WIFI\n", bssid, ssid,
                     wigleAuth(s.auth), ts, (int)s.channel, (int)s.rssi, lat,
                     lon, alt, acc);
          } else {
            f.printf("%s,%s,%s,%s,%d,%d,,,,,WIFI\n", bssid, ssid,
                     wigleAuth(s.auth), ts, (int)s.channel, (int)s.rssi);
          }
          crLoggedSession++;
        }
        f.close();
        game::addLoot(game::Loot::Cargo, 1);
      }
    }
    wifiScanBegin();
  }
}
void crClose() { wifiRfLeave(); }
void crDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  bool fix = gps::hasFix();
  char vRows[10], vUnique[10], vNew[10], vGps[8];
  snprintf(vRows, sizeof(vRows), "%lu", (unsigned long)crLoggedSession);
  snprintf(vUnique, sizeof(vUnique), "%lu", (unsigned long)crUniqueSession);
  snprintf(vNew, sizeof(vNew), "+%lu", (unsigned long)crNewLastSweep);
  snprintf(vGps, sizeof(vGps), "%s", gps::statusLabel());
  const char* vals[] = {vRows, vUnique, vNew, vGps};
  const char* labs[] = {"rows", "unique AP", "last sweep", "GPS"};
  uint16_t cols[] = {theme::kGold, theme::kCyan,
                     crNewLastSweep ? theme::kGood : theme::kInkDim,
                     fix ? theme::kGood : theme::kWarn};
  int below = kpiStrip(x + 4, y + 2, w - 8, 4, vals, labs, cols) + 4;
  char value[48];
  snprintf(value, sizeof(value), "%s · %lu sats · HDOP %.1f",
           fix ? "FIX" : "NO FIX", (unsigned long)gps::satellites(),
           gps::hdop());
  gfxu::drawKVRow(G(), x + 6, below, w - 12, 18, "GPS", value,
                  fix ? theme::kGood : theme::kWarn);
  uint64_t bytes = 0;
  if (app::sdReady() && SD_MMC.exists(kWigleFile)) {
    File log = SD_MMC.open(kWigleFile, FILE_READ);
    if (log) {
      bytes = log.size();
      log.close();
    }
  }
  char size[20];
  if (bytes >= 1024 * 1024)
    snprintf(size, sizeof(size), "%lu KB", (unsigned long)(bytes / 1024));
  else
    snprintf(size, sizeof(size), "%lu B", (unsigned long)bytes);
  gfxu::drawKVRow(G(), x + 6, below + 20, w - 12, 18, "CSV / SD",
                  app::sdReady() ? size : "SD unavailable",
                  app::sdReady() ? theme::kGood : theme::kBad);
  int rowTop = below + 44;
  txt(x + 8, rowTop, theme::kTeal, 1, "%s · sweep %lu · %d APs · /wigle.csv",
      crPaused ? "PAUSED" : "RECORDING", (unsigned long)crSweepCount,
      crLastSweepCount);
  const int rowH = 17;
  int visibleNetworks = g_rfNetCount;
  if (visibleNetworks > 5) visibleNetworks = 5;
  for (int i = 0; i < visibleNetworks; i++) {
    int ry = rowTop + 12 + i * rowH;
    const RfNetSnap& net = g_rfNets[i];
    if (i & 1) G().fillRect(x + 4, ry, w - 8, rowH, theme::kPanelSoft);
    char label[22], metadata[30];
    const char* ssid = net.ssid[0] ? net.ssid : "<hidden>";
    snprintf(label, sizeof(label), "%.16s", ssid);
    snprintf(metadata, sizeof(metadata), "ch%u %ddB %s", net.channel,
             (int)net.rssi, encLabel(net.auth));
    gfxu::printFit(G(), x + 9, ry + 4, 128, theme::kInk, 1, label);
    gfxu::printFit(G(), x + 148, ry + 4, 164, theme::kInkDim, 1, metadata);
    G().drawFastHLine(x + 8, ry + rowH - 1, w - 16, theme::kBorder);
  }
  const int buttonY = y + h - 27;
  btn(x + 6, buttonY, 142, 22, crPaused ? "RESUME" : "PAUSE", !crPaused,
      crPaused ? theme::kGood : theme::kWarn);
  btn(x + 158, buttonY, 154, 22, "SCAN NOW", false, theme::kCyan);
}
bool crTouch(int16_t x, int16_t y) {
  if (y < theme::kScreenH - 32) return true;
  if (x < 154) {
    crPaused = !crPaused;
    if (crPaused) {
      wifiRfLeave();
      tools::toast("Wardrive paused");
    } else {
      crProcessed = -1;
      wifiScanBegin();
      tools::toast("Wardrive resumed");
    }
  } else {
    crPaused = false;
    crProcessed = -1;
    wifiScanBegin();
  }
  return true;
}
}  // namespace

// ===========================================================================
//  4. Lookout -- channel occupancy analyzer (histogram + detail)
// ===========================================================================
namespace {
int lkHist[14] = {0};
int lkOpenHist[14] = {0};
constexpr int kLkHistory = 18;
int8_t lkTimeline[14][kLkHistory] = {};
int8_t lkOpenTimeline[14][kLkHistory] = {};
int lkHistoryHead = kLkHistory - 1;
int lkHistoryCount = 0;
int lkProcessed = -1;
int lkSelected = 0;  // 0 = none, 1..13 = channel detail
int lkTotal = 0;
int lkSweepCount = 0;
int lkPeakChannel = 1;
int lkOpenTotal = 0;
bool lkPaused = false;

void lkOpen() {
  lkProcessed = -1;
  lkSelected = 0;
  lkHistoryHead = kLkHistory - 1;
  lkHistoryCount = 0;
  lkSweepCount = 0;
  lkPaused = false;
  memset(lkTimeline, 0, sizeof(lkTimeline));
  memset(lkOpenTimeline, 0, sizeof(lkOpenTimeline));
  wifiScanBegin();
}
void lkTick(uint32_t now) {
  (void)now;
  int st = wifiScanState();
  if (st >= 0 && st != lkProcessed) {
    lkProcessed = st;
    lkTotal = st;
    lkOpenTotal = 0;
    lkPeakChannel = 1;
    g_lastWifiCount = st;
    for (int c = 1; c <= 13; c++) {
      lkHist[c] = 0;
      lkOpenHist[c] = 0;
    }
    int n = g_rfNetCount;
    if (n > st) n = st;
    for (int i = 0; i < n; i++) {
      int c = g_rfNets[i].channel;
      if (c >= 1 && c <= 13) {
        lkHist[c]++;
        if (g_rfNets[i].auth == WIFI_AUTH_OPEN) {
          lkOpenHist[c]++;
          lkOpenTotal++;
        }
        if (lkHist[c] > lkHist[lkPeakChannel]) lkPeakChannel = c;
      }
    }
    lkHistoryHead = (lkHistoryHead + 1) % kLkHistory;
    for (int c = 1; c <= 13; c++) {
      lkTimeline[c][lkHistoryHead] = (int8_t)min(lkHist[c], 127);
      lkOpenTimeline[c][lkHistoryHead] = (int8_t)min(lkOpenHist[c], 127);
    }
    if (lkHistoryCount < kLkHistory) lkHistoryCount++;
    lkSweepCount++;
    if (!lkPaused) wifiScanBegin();
  }
}
void lkClose() { wifiRfLeave(); }
void lkDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  int maxv = 1;
  for (int c = 1; c <= 13; c++) {
    for (int i = 0; i < lkHistoryCount; i++) {
      int index = (lkHistoryHead - i + kLkHistory) % kLkHistory;
      if (lkTimeline[c][index] > maxv) maxv = lkTimeline[c][index];
    }
  }
  char vTot[8], vPeak[8], vOpen[8], vSweeps[8];
  snprintf(vTot, sizeof(vTot), "%d", lkTotal);
  snprintf(vPeak, sizeof(vPeak), "%d", lkPeakChannel);
  snprintf(vOpen, sizeof(vOpen), "%d", lkOpenTotal);
  snprintf(vSweeps, sizeof(vSweeps), "%d", lkSweepCount);
  const char* vals[] = {vTot, vPeak, vOpen, vSweeps};
  const char* labs[] = {"APs", "peak CH", "open", "sweeps"};
  uint16_t cols[] = {theme::kCyan, theme::kGold,
                     lkOpenTotal ? theme::kBad : theme::kGood, theme::kTeal};
  kpiStrip(x + 4, y + 2, w - 8, 4, vals, labs, cols);
  txt(x + 8, y + 36, theme::kInkDim, 1,
      "Channel × sweep · AP beacon counts, not RF energy");

  const int plotX = x + 34;
  const int plotY = y + 49;
  constexpr int rowH = 7;
  constexpr int cellW = 12;
  constexpr int cellGap = 2;
  const int cellStep = cellW + cellGap;
  const int firstColumn = kLkHistory - lkHistoryCount;
  G().fillRect(plotX, plotY, kLkHistory * cellStep - cellGap, 13 * rowH,
               theme::kBgDeep);
  for (int row = 0; row < 13; row++) {
    int channel = 13 - row;
    int rowY = plotY + row * rowH;
    uint16_t labelColor = lkSelected == channel ? theme::kGold : theme::kInkMuted;
    txt(x + 8, rowY, labelColor, 1, "%02d", channel);
    for (int column = 0; column < kLkHistory; column++) {
      int cellX = plotX + column * cellStep;
      uint16_t color = theme::kPanelSoft;
      if (column >= firstColumn) {
        int sweepIndex = (lkHistoryHead - (kLkHistory - 1 - column) +
                          kLkHistory * 2) % kLkHistory;
        int count = lkTimeline[channel][sweepIndex];
        int open = lkOpenTimeline[channel][sweepIndex];
        if (count > 0) {
          color = count >= max(3, maxv / 2) ? theme::kGold
                  : count >= 2              ? theme::kCyan
                                             : theme::kBorderHi;
          if (open > 0) color = theme::kBad;
        }
      }
      G().fillRect(cellX, rowY, cellW, rowH - 1, color);
      if (lkSelected == channel) G().drawRect(cellX, rowY, cellW, rowH - 1,
                                               theme::kInk);
    }
  }
  int axisY = plotY + 13 * rowH + 2;
  txt(plotX, axisY, theme::kInkMuted, 1, "old");
  gfxu::printFit(G(), plotX + kLkHistory * cellStep - 24, axisY, 24,
                 theme::kInkMuted, 1, "now");
  G().fillRect(x + 6, y + 150, 5, 5, theme::kBorderHi);
  txt(x + 13, y + 149, theme::kInkMuted, 1, "1 AP");
  G().fillRect(x + 54, y + 150, 5, 5, theme::kCyan);
  txt(x + 61, y + 149, theme::kInkMuted, 1, "2+");
  G().fillRect(x + 90, y + 150, 5, 5, theme::kGold);
  txt(x + 97, y + 149, theme::kInkMuted, 1, "busy");
  G().fillRect(x + 132, y + 150, 5, 5, theme::kBad);
  txt(x + 139, y + 149, theme::kInkMuted, 1, "open AP present");
  if (lkSelected >= 1 && lkSelected <= 13) {
    int trend = 0;
    if (lkHistoryCount >= 2) {
      int previous = (lkHistoryHead - 1 + kLkHistory) % kLkHistory;
      trend = lkHist[lkSelected] - lkTimeline[lkSelected][previous];
    }
    gfxu::printFit(G(), x + 8, y + 165, w - 16, theme::kGold, 1,
                   String("CH " + String(lkSelected) + ": " +
                          String(lkHist[lkSelected]) + " AP / " +
                          String(lkOpenHist[lkSelected]) + " open / " +
                          String(trend >= 0 ? "+" : "") + String(trend) +
                          " vs prev").c_str());
  } else {
    txt(x + 8, y + 165, theme::kInkMuted, 1, "Tap a channel row to inspect its trend");
  }
  const int buttonY = y + h - 27;
  btn(x + 6, buttonY, 94, 22, lkPaused ? "RESUME" : "PAUSE", !lkPaused,
      lkPaused ? theme::kGood : theme::kWarn);
  btn(x + 108, buttonY, 94, 22, "CLEAR", false);
  btn(x + 210, buttonY, 102, 22, "SCAN NOW", false, theme::kCyan);
}
bool lkTouch(int16_t x, int16_t y) {
  if (y >= theme::kScreenH - 32) {
    if (x < 104) {
      lkPaused = !lkPaused;
      if (lkPaused) wifiRfLeave();
      else {
        lkProcessed = -1;
        wifiScanBegin();
      }
    } else if (x < 208) {
      memset(lkTimeline, 0, sizeof(lkTimeline));
      memset(lkOpenTimeline, 0, sizeof(lkOpenTimeline));
      lkHistoryHead = kLkHistory - 1;
      lkHistoryCount = 0;
      lkSweepCount = 0;
      lkSelected = 0;
    } else {
      lkPaused = false;
      lkProcessed = -1;
      wifiScanBegin();
    }
    return true;
  }
  const int plotY = contentTop() + 49;
  constexpr int rowH = 7;
  if (y >= plotY && y < plotY + 13 * rowH && x < theme::kScreenW - 4) {
    int channel = 13 - (y - plotY) / rowH;
    lkSelected = lkSelected == channel ? 0 : channel;
    return true;
  }
  return true;
}
}  // namespace

// ===========================================================================
//  5. Spyglass -- surveillance-camera spotter (detection only)
// ===========================================================================
namespace {
int sgProcessed = -1;
struct Hit {
  String ssid;
  String bssid;
  String label;
  int rssi;
  uint8_t channel;
  wifi_auth_mode_t auth;
  uint8_t confidence;  // 2 = verified OUI prefix, 1 = SSID keyword only
  char evidence[24];
};
Hit sgHits[12];
int sgHitCount = 0;
int sgOuiCount = 0;
int sgKeywordCount = 0;
uint32_t sgSweepCount = 0;
int sgFilter = 0;
int sgSelected = -1;
int sgVisible[12];
int sgVisibleCount = 0;

bool ouiMatch(const String& bssid, const char* prefix) {
  if (!prefix || !prefix[0]) return false;
  String p = String(prefix);
  p.toUpperCase();
  String b = bssid;
  b.toUpperCase();
  return b.startsWith(p);
}
bool ssidMatch(const String& ssid, const char* sub) {
  if (!sub || !sub[0]) return false;
  String s = ssid;
  s.toLowerCase();
  String q = String(sub);
  q.toLowerCase();
  return s.indexOf(q) >= 0;
}

void sgRebuildVisible() {
  sgVisibleCount = 0;
  for (int i = 0; i < sgHitCount; i++) {
    if (sgFilter == 1 && sgHits[i].confidence != 2) continue;
    if (sgFilter == 2 && sgHits[i].confidence != 1) continue;
    sgVisible[sgVisibleCount++] = i;
  }
}

void sgOpen() {
  sgProcessed = -1;
  sgHitCount = 0;
  sgOuiCount = 0;
  sgKeywordCount = 0;
  sgSweepCount = 0;
  sgFilter = 0;
  sgSelected = -1;
  sgVisibleCount = 0;
  wifiScanBegin();
}
void sgTick(uint32_t now) {
  (void)now;
  int st = wifiScanState();
  if (st >= 0 && st != sgProcessed) {
    sgProcessed = st;
    sgHitCount = 0;
    sgOuiCount = 0;
    sgKeywordCount = 0;
    sgSweepCount++;
    for (int i = 0; i < g_rfNetCount && i < st && sgHitCount < 12; i++) {
      const RfNetSnap& net = g_rfNets[i];
      String ssid = String(net.ssid);
      char bssidBuf[18];
      snprintf(bssidBuf, sizeof(bssidBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
               net.bssid[0], net.bssid[1], net.bssid[2], net.bssid[3],
               net.bssid[4], net.bssid[5]);
      String bssid = String(bssidBuf);
      for (int s = 0; s < spyglass::kSignatureCount; s++) {
        const auto& sig = spyglass::kSignatures[s];
        bool isOui = ouiMatch(bssid, sig.ouiPrefix);
        bool isKeyword = ssidMatch(ssid, sig.ssidSubstr);
        if (!isOui && !isKeyword) continue;
        Hit& hit = sgHits[sgHitCount++];
        hit.ssid = ssid.length() ? ssid : String("<hidden>");
        hit.bssid = bssid;
        hit.label = sig.label;
        hit.rssi = net.rssi;
        hit.channel = net.channel;
        hit.auth = net.auth;
        hit.confidence = isOui ? 2 : 1;
        if (isOui) {
          snprintf(hit.evidence, sizeof(hit.evidence), "OUI %.8s", sig.ouiPrefix);
          sgOuiCount++;
        } else {
          snprintf(hit.evidence, sizeof(hit.evidence), "SSID has %.12s",
                   sig.ssidSubstr);
          sgKeywordCount++;
      }
      sgRebuildVisible();
        break;
      }
        }
    wifiScanBegin();
  }
}
void sgClose() { wifiRfLeave(); }

bool sgSave() {
  if (!app::sdReady()) {
    tools::toast("Save failed · no SD");
    return false;
  }
  if (!sgHitCount) {
    tools::toast("No Spyglass leads to save");
    return false;
  }
  if (!SD_MMC.exists("/log") && !SD_MMC.mkdir("/log")) {
    tools::toast("Save failed · /log");
    return false;
  }
  const char* path = "/log/spyglass.csv";
  bool header = !SD_MMC.exists(path);
  File file = SD_MMC.open(path, FILE_APPEND);
  if (!file) {
    tools::toast("Save failed · open");
    return false;
  }
  if (header)
    file.println("timestamp,match_type,evidence,label,SSID,BSSID,channel,RSSI,auth,latitude,longitude");
  char timestamp[24];
  if (!gps::formatTimestamp(timestamp, sizeof(timestamp)))
    snprintf(timestamp, sizeof(timestamp), "uptime-%lu",
             (unsigned long)(millis() / 1000));
  for (int i = 0; i < sgHitCount; i++) {
    const Hit& hit = sgHits[i];
    char ssid[40], label[44];
    snprintf(ssid, sizeof(ssid), "%s", hit.ssid.c_str());
    snprintf(label, sizeof(label), "%s", hit.label.c_str());
    for (char* p = ssid; *p; p++)
      if (*p == ',' || *p == '\n' || *p == '\r') *p = ' ';
    for (char* p = label; *p; p++)
      if (*p == ',' || *p == '\n' || *p == '\r') *p = ' ';
    file.printf("%s,%s,%s,%s,%s,%s,%u,%d,%s,", timestamp,
                hit.confidence == 2 ? "oui_lead" : "ssid_keyword_candidate",
                hit.evidence, label, ssid, hit.bssid.c_str(), hit.channel,
                hit.rssi, encLabel(hit.auth));
    if (gps::hasFix())
      file.printf("%.6f,%.6f\n", gps::latitude(), gps::longitude());
    else
      file.println(",");
  }
  file.close();
  tools::toast("Saved %d Spyglass leads", sgHitCount);
  return true;
}

void sgDrawDetail(int x, int y, int w, int h) {
  body(x, y, w, h);
  if (sgSelected < 0 || sgSelected >= sgHitCount) return;
  const Hit& hit = sgHits[sgSelected];
  uint16_t matchColor = hit.confidence == 2 ? theme::kGood : theme::kWarn;
  char rssi[12], channel[16], vendor[24];
  snprintf(rssi, sizeof(rssi), "%d dBm", hit.rssi);
  snprintf(channel, sizeof(channel), "%u · %s", hit.channel,
           bandFromChannel(hit.channel));
  const char* vendorName = oui::vendorStr(hit.bssid);
  snprintf(vendor, sizeof(vendor), "%s", vendorName[0] ? vendorName : "unknown");
  const char* values[] = {hit.confidence == 2 ? "OUI" : "KEYWORD", rssi,
                          encLabel(hit.auth)};
  const char* labels[] = {"match", "signal", "auth"};
  const uint16_t colors[] = {matchColor, theme::kCyan,
                             riskColor(encRisk(hit.auth))};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, values, labels, colors) + 3;
  gfxu::drawKVRow(G(), x + 6, below, w - 12, 18, "Candidate", hit.label.c_str(),
                  matchColor);
  gfxu::drawKVRow(G(), x + 6, below + 20, w - 12, 18, "Evidence", hit.evidence,
                  matchColor);
  gfxu::drawKVRow(G(), x + 6, below + 40, w - 12, 18, "SSID", hit.ssid.c_str());
  gfxu::drawKVRow(G(), x + 6, below + 60, w - 12, 18, "BSSID", hit.bssid.c_str(),
                  theme::kCyan);
  gfxu::drawKVRow(G(), x + 6, below + 80, w - 12, 18, "Vendor", vendor,
                  theme::kTeal);
  gfxu::drawKVRow(G(), x + 6, below + 100, w - 12, 18, "Channel", channel);
  txt(x + 10, y + h - 41, matchColor, 1,
      hit.confidence == 2 ? "OUI lead · not proof of camera identity"
                          : "Keyword candidate · verify independently");
  btn(x + 6, y + h - 24, 72, 20, "BACK", false);
  btn(x + w - 84, y + h - 24, 78, 20, "SAVE", false);
}

void sgDraw(int x, int y, int w, int h) {
  if (sgSelected >= 0) {
    sgDrawDetail(x, y, w, h);
    return;
  }
  body(x, y, w, h);
  bool armed = spyglass::hasVerifiedSignature();
  char vHits[8], vOui[8], vKeyword[8], vSig[8];
  snprintf(vHits, sizeof(vHits), "%d", sgHitCount);
  snprintf(vOui, sizeof(vOui), "%d", sgOuiCount);
  snprintf(vKeyword, sizeof(vKeyword), "%d", sgKeywordCount);
  snprintf(vSig, sizeof(vSig), "%d", spyglass::kSignatureCount);
  const char* vals[] = {vHits, vOui, vKeyword, vSig};
  const char* labs[] = {"leads", "OUI", "keyword", "sigs"};
  uint16_t cols[] = {sgHitCount ? theme::kWarn : theme::kGood,
                     sgOuiCount ? theme::kGood : theme::kInkDim,
                     sgKeywordCount ? theme::kWarn : theme::kInkDim,
                     armed ? theme::kTeal : theme::kWarn};
  int below = kpiStrip(x + 4, y + 2, w - 8, 4, vals, labs, cols) + 2;
  gfxu::drawToolbar(G(), x + 4, below, w - 8, 18);
  chip(x + 6, below + 1, 48, 16, "ALL", sgFilter == 0);
  chip(x + 58, below + 1, 48, 16, "OUI", sgFilter == 1, theme::kGood);
  chip(x + 110, below + 1, 64, 16, "KEYWORD", sgFilter == 2, theme::kWarn);
  chip(x + 240, below + 1, 72, 16, "SAVE", false, theme::kCyan);
  below += 20;
  constexpr int kRh = theme::kRowHCompact;
  int rows = min(sgVisibleCount, 7);
  for (int i = 0; i < rows; i++) {
    int ry = below + i * kRh;
    if (i & 1) G().fillRect(x + 4, ry, w - 8, kRh, theme::kPanelSoft);
    const Hit& hit = sgHits[sgVisible[i]];
    uint16_t color = hit.confidence == 2 ? theme::kGood : theme::kWarn;
    String lab = hit.label;
    if (lab.length() > 28) lab = lab.substring(0, 28);
    gfxu::printFit(G(), x + 10, ry + 1, w - 18, color, 1, lab.c_str());
    char sec[42];
    snprintf(sec, sizeof(sec), "%s · %.12s · ch%u %ddB", hit.evidence,
             hit.ssid.c_str(), hit.channel, hit.rssi);
    gfxu::printFit(G(), x + 10, ry + 10, w - 18, theme::kInkMuted, 1, sec);
    G().drawFastHLine(x + 8, ry + kRh - 1, w - 16, theme::kBorder);
  }
  if (!sgVisibleCount)
    txt(x + 10, below + 6, theme::kInkMuted, 1,
        "No candidates in the latest sweep");
  txt(x + 8, y + h - 10, theme::kInkMuted, 1,
      "Passive public metadata · leads only · sweep %lu",
      (unsigned long)sgSweepCount);
}
bool sgTouch(int16_t x, int16_t y) {
  if (sgSelected >= 0) {
    if (y >= theme::kScreenH - 34) {
      if (x < 90) sgSelected = -1;
      else sgSave();
    }
    return true;
  }
  const int toolbarY = contentTop() + 32;
  if (y >= toolbarY && y < toolbarY + 20) {
    if (x < 56) sgFilter = 0;
    else if (x < 108) sgFilter = 1;
    else if (x < 178) sgFilter = 2;
    else if (x >= 240) sgSave();
    sgRebuildVisible();
    return true;
  }
  const int listTop = toolbarY + 20;
  constexpr int rowH = theme::kRowHCompact;
  if (y >= listTop && y < listTop + 7 * rowH) {
    int row = (y - listTop) / rowH;
    if (row < sgVisibleCount) sgSelected = sgVisible[row];
  }
  return true;
}
}  // namespace

// ===========================================================================
//  6. Tracker Watch -- nearby BLE item-tracker spotter (anti-stalking)
// ===========================================================================
namespace {
constexpr int kTwRecords = 24;
struct TwRecord {
  String mac;
  String name;
  char detail[22];
  int currentRssi;
  int strongestRssi;
  uint16_t sweepsSeen;
  uint32_t lastGeneration;
};
TwRecord twRecords[kTwRecords];
int twRecordCount = 0;
uint32_t twGeneration = 0;
uint32_t twSweepCount = 0;
uint32_t twLastSweepAt = 0;

void twOpen() {
  g_bleLastScan = 0;
  twRecordCount = 0;
  twGeneration = g_bleGeneration;
  twSweepCount = 0;
  twLastSweepAt = 0;
}
void twTick(uint32_t now) {
  bleTick(now);
  if (twGeneration == g_bleGeneration) return;
  twGeneration = g_bleGeneration;
  twSweepCount++;
  twLastSweepAt = now;
  for (int i = 0; i < g_bleCount; i++) {
    const BleDev& device = g_ble[i];
    if (device.kind != 1) continue;
    int record = -1;
    for (int r = 0; r < twRecordCount; r++)
      if (twRecords[r].mac == device.mac) { record = r; break; }
    if (record < 0) {
      if (twRecordCount >= kTwRecords) continue;
      record = twRecordCount++;
      TwRecord& item = twRecords[record];
      item.mac = device.mac;
      item.name = device.name;
      item.currentRssi = device.rssi;
      item.strongestRssi = device.rssi;
      item.sweepsSeen = 0;
      item.lastGeneration = 0;
      snprintf(item.detail, sizeof(item.detail), "%s", device.detail);
    }
    TwRecord& item = twRecords[record];
    item.currentRssi = device.rssi;
    if (device.rssi > item.strongestRssi) item.strongestRssi = device.rssi;
    if (item.sweepsSeen < 65535 && item.lastGeneration != twGeneration)
      item.sweepsSeen++;
    item.lastGeneration = twGeneration;
    item.name = device.name;
    snprintf(item.detail, sizeof(item.detail), "%s", device.detail);
  }
}
void twClose() { bleStop(); }
void twDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  int current = 0, repeat = 0, nearest = -127;
  for (int i = 0; i < g_bleCount; i++)
    if (g_ble[i].kind == 1) {
      current++;
      if (g_ble[i].rssi > nearest) nearest = g_ble[i].rssi;
    }
  for (int i = 0; i < twRecordCount; i++)
    if (twRecords[i].sweepsSeen > 1) repeat++;
  char vCurrent[8], vUnique[8], vRepeat[8], vSignal[12];
  snprintf(vCurrent, sizeof(vCurrent), "%d", current);
  snprintf(vUnique, sizeof(vUnique), "%d", twRecordCount);
  snprintf(vRepeat, sizeof(vRepeat), "%d", repeat);
  if (nearest > -127) snprintf(vSignal, sizeof(vSignal), "%d", nearest);
  else snprintf(vSignal, sizeof(vSignal), "--");
  const char* vals[] = {vCurrent, vUnique, vRepeat, vSignal};
  const char* labs[] = {"now", "session IDs", "repeat", "near dBm"};
  const uint16_t cols[] = {current ? theme::kWarn : theme::kGood,
                           theme::kCyan, repeat ? theme::kWarn : theme::kInkDim,
                           nearest > -60 ? theme::kWarn : theme::kGood};
  int below = kpiStrip(x + 4, y + 2, w - 8, 4, vals, labs, cols) + 2;
  gfxu::printFit(G(), x + 8, below, w - 16, theme::kInkMuted, 1,
                 "Likely tracker · passive ads only");
  below += 12;
  const int rowH = 20;
  int rows = min(twRecordCount, 5);
  for (int i = 0; i < rows; i++) {
    const TwRecord& item = twRecords[i];
    int ry = below + i * rowH;
    if (i & 1) G().fillRect(x + 4, ry, w - 8, rowH, theme::kPanelSoft);
    rssiBars(x + 8, ry + 3, item.currentRssi);
    String label = item.name.length() ? item.name : item.mac;
    gfxu::printFit(G(), x + 29, ry + 1, 148, theme::kWarn, 1, label.c_str());
    char meta[36];
    snprintf(meta, sizeof(meta), "%s · %u sweeps", item.detail[0] ? item.detail : "tracker-like",
             item.sweepsSeen);
    gfxu::printFit(G(), x + 29, ry + 10, 182, theme::kInkMuted, 1, meta);
    char signal[12];
    snprintf(signal, sizeof(signal), "%ddB", item.currentRssi);
    gfxu::printFit(G(), x + w - 42, ry + 2, 36, theme::kInkDim, 1, signal);
    G().drawFastHLine(x + 8, ry + rowH - 1, w - 16, theme::kBorder);
  }
  if (twRecordCount == 0)
    txt(x + 12, below + 8, theme::kInkMuted, 1,
        "No likely trackers in the latest passive sweep.");
  const int buttonY = y + h - 27;
  btn(x + 6, buttonY, 142, 22, "CLEAR SESSION", false);
  btn(x + 158, buttonY, 154, 22, "SCAN NOW", false, theme::kCyan);
}
bool twTouch(int16_t x, int16_t y) {
  if (y < theme::kScreenH - 32) return true;
  if (x < 154) {
    twRecordCount = 0;
    twSweepCount = 0;
    twLastSweepAt = 0;
    twGeneration = g_bleGeneration;
    tools::toast("Tracker session cleared");
  } else {
    bleStop();
    g_bleLastScan = 0;
  }
  return true;
}
}  // namespace

// ===========================================================================
//  7. Rigging Watch -- deauth / disassoc attack DETECTOR (defensive)
// ===========================================================================
namespace {
uint32_t rwDeauth = 0;
uint32_t rwDisassoc = 0;
int rwLastRssi = 0;
char rwLastSrc[18] = "--";
uint32_t rwLastHit = 0;
int rwChannel = 1;
uint32_t rwHopAt = 0;
constexpr int kRwBins = 16;
volatile uint16_t rwDeauthBins[kRwBins] = {};
volatile uint16_t rwDisassocBins[kRwBins] = {};
volatile uint32_t rwBinEpochs[kRwBins] = {};
uint32_t rwChannelEvents[14] = {};

void rwPromiscCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  auto* p = (wifi_promiscuous_pkt_t*)buf;
  if (p->rx_ctrl.sig_len < 24) return;
  const uint8_t* pl = p->payload;
  uint8_t subtype = (pl[0] >> 4) & 0x0F;
  // Inspect only management-frame subtype + transmitter address. No payload,
  // no data frames, nothing stored beyond counters -- this is an IDS, not a
  // capture tool.
  if (subtype == 12 || subtype == 10) {
    uint32_t now = millis();
    uint32_t epoch = now / 2000;
    int bin = epoch % kRwBins;
    if (rwBinEpochs[bin] != epoch) {
      rwDeauthBins[bin] = 0;
      rwDisassocBins[bin] = 0;
      rwBinEpochs[bin] = epoch;
    }
    if (subtype == 12) {
      rwDeauth++;
      uint16_t count = rwDeauthBins[bin];
      if (count < 65535) rwDeauthBins[bin] = count + 1;
    } else {
      rwDisassoc++;
      uint16_t count = rwDisassocBins[bin];
      if (count < 65535) rwDisassocBins[bin] = count + 1;
    }
    if (rwChannel >= 1 && rwChannel <= 13) rwChannelEvents[rwChannel]++;
    snprintf(rwLastSrc, sizeof(rwLastSrc), "%02X:%02X:%02X:%02X:%02X:%02X",
             pl[10], pl[11], pl[12], pl[13], pl[14], pl[15]);
    rwLastRssi = p->rx_ctrl.rssi;
    rwLastHit = now;
  }
}
void rwOpen() {
  rwDeauth = rwDisassoc = 0;
  memset((void*)rwDeauthBins, 0, sizeof(rwDeauthBins));
  memset((void*)rwDisassocBins, 0, sizeof(rwDisassocBins));
  memset((void*)rwBinEpochs, 0, sizeof(rwBinEpochs));
  memset(rwChannelEvents, 0, sizeof(rwChannelEvents));
  rwLastHit = 0;
  rwLastSrc[0] = '-';
  rwLastSrc[1] = '-';
  rwLastSrc[2] = 0;
  rwChannel = 1;
  wifiPromiscBegin(&rwPromiscCb, rwChannel);
}
void rwTick(uint32_t now) {
  if (!g_rfPromiscReady) return;
  if (now - rwHopAt > 300) {  // sweep channels so we cover the band
    rwHopAt = now;
    rwChannel = rwChannel >= 13 ? 1 : rwChannel + 1;
    wifiPromiscHop(rwChannel);
  }
}
void rwClose() { wifiRfLeave(); }
void rwDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  bool recent = rwLastHit && (millis() - rwLastHit) < 4000;
  uint32_t epoch = millis() / 2000;
  char vDe[10], vDi[10], vCh[8], vNow[8];
  snprintf(vDe, sizeof(vDe), "%lu", (unsigned long)rwDeauth);
  snprintf(vDi, sizeof(vDi), "%lu", (unsigned long)rwDisassoc);
  snprintf(vCh, sizeof(vCh), "%d", rwChannel);
  uint16_t recentEvents = 0;
  uint32_t currentEpoch = epoch;
  int currentBin = currentEpoch % kRwBins;
  if (rwBinEpochs[currentBin] == currentEpoch)
    recentEvents = (uint16_t)rwDeauthBins[currentBin] +
                   (uint16_t)rwDisassocBins[currentBin];
  snprintf(vNow, sizeof(vNow), "%u", recentEvents);
  const char* vals[] = {vDe, vDi, vNow, vCh};
  const char* labs[] = {"deauth", "disassoc", "last 2s", "CH"};
  uint16_t cols[] = {rwDeauth ? theme::kBad : theme::kGood,
                     rwDisassoc ? theme::kWarn : theme::kGood,
                     recentEvents ? theme::kBad : theme::kGood, theme::kCyan};
  kpiStrip(x + 4, y + 2, w - 8, 4, vals, labs, cols);
  txt(x + 8, y + 35, recent ? theme::kBad : theme::kGood, 1,
      recent ? "Recent management-frame burst · review in context"
             : "No recent deauth/disassoc frames · passive monitor");

  const int plotX = x + 10, plotY = y + 49, plotW = w - 20, plotH = 48;
  int maxBin = 1;
  for (int i = 0; i < kRwBins; i++) {
    uint32_t sampleEpoch = epoch - (kRwBins - 1 - i);
    int index = sampleEpoch % kRwBins;
    if (rwBinEpochs[index] != sampleEpoch) continue;
    int sum = (uint16_t)rwDeauthBins[index] + (uint16_t)rwDisassocBins[index];
    if (sum > maxBin) maxBin = sum;
  }
  G().fillRect(plotX, plotY, plotW, plotH, theme::kBgDeep);
  G().drawRect(plotX, plotY, plotW, plotH, theme::kBorder);
  G().drawFastHLine(plotX + 1, plotY + plotH - 1, plotW - 2, theme::kBorderHi);
  const int slot = plotW / kRwBins;
  for (int i = 0; i < kRwBins; i++) {
    uint32_t sampleEpoch = epoch - (kRwBins - 1 - i);
    int index = sampleEpoch % kRwBins;
    if (rwBinEpochs[index] != sampleEpoch) continue;
    int deauth = (uint16_t)rwDeauthBins[index];
    int disassoc = (uint16_t)rwDisassocBins[index];
    int dh = deauth * (plotH - 4) / maxBin;
    int sh = disassoc * (plotH - 4) / maxBin;
    int bx = plotX + i * slot + 2;
    int base = plotY + plotH - 2;
    if (dh) {
      base -= dh;
      G().fillRect(bx, base, max(2, slot - 3), dh, theme::kBad);
    }
    if (sh) {
      base -= sh;
      G().fillRect(bx, base, max(2, slot - 3), sh, theme::kWarn);
    }
  }
  txt(plotX, plotY + plotH + 1, theme::kInkMuted, 1, "32 sec · 2 sec bins");

  const int channelBase = y + 132;
  int channelMax = 1;
  for (int ch = 1; ch <= 13; ch++)
    if (rwChannelEvents[ch] > (uint32_t)channelMax)
      channelMax = (int)rwChannelEvents[ch];
  const int channelSlot = (w - 20) / 13;
  for (int ch = 1; ch <= 13; ch++) {
    int bx = x + 10 + (ch - 1) * channelSlot;
    int bh = rwChannelEvents[ch] * 20 / channelMax;
    if (bh) G().fillRect(bx + 3, channelBase - bh, channelSlot - 6, bh,
                         ch == rwChannel ? theme::kGold : theme::kCyan);
    char channelLabel[4];
    snprintf(channelLabel, sizeof(channelLabel), "%d", ch);
    txt(bx + (ch >= 10 ? 3 : 6), channelBase + 2, theme::kInkMuted, 1,
        "%s", channelLabel);
  }
  char source[52];
  if (rwLastHit)
    snprintf(source, sizeof(source), "Last %s · %d dBm · CH %d", rwLastSrc,
             rwLastRssi, rwChannel);
  else
    snprintf(source, sizeof(source), "No event yet · monitoring CH %d", rwChannel);
  gfxu::printFit(G(), x + 8, y + 158, w - 16, theme::kInkDim, 1, source);
  const int buttonY = y + h - 27;
  btn(x + 6, buttonY, 142, 22, "CLEAR COUNTERS", false);
  btn(x + 158, buttonY, 154, 22, "RESET CHANNEL HOP", false, theme::kCyan);
}
bool rwTouch(int16_t x, int16_t y) {
  if (y < theme::kScreenH - 32) return true;
  if (x < 154) {
    rwDeauth = rwDisassoc = 0;
    rwLastHit = 0;
    rwLastSrc[0] = '-';
    rwLastSrc[1] = '-';
    rwLastSrc[2] = 0;
    memset((void*)rwDeauthBins, 0, sizeof(rwDeauthBins));
    memset((void*)rwDisassocBins, 0, sizeof(rwDisassocBins));
    memset((void*)rwBinEpochs, 0, sizeof(rwBinEpochs));
    memset(rwChannelEvents, 0, sizeof(rwChannelEvents));
    tools::toast("Rigging counters cleared");
  } else {
    rwChannel = 1;
    rwHopAt = millis();
    wifiPromiscHop(rwChannel);
  }
  return true;
}
}  // namespace

// ===========================================================================
//  8. Hull Inspection -- security posture audit of nearby networks
// ===========================================================================
namespace {
int hiProcessed = -1;
int hiOpen_ = 0, hiLegacy = 0, hiStrong = 0, hiTotal = 0;
int hiStored = 0;
uint32_t hiLastScan = 0;
int hiFilter = 0;
int hiSelected = -1;
int hiScroll = 0;
int hiVisible[48];
int hiVisibleCount = 0;
struct HiRow {
  char ssid[33];
  uint8_t bssid[6];
  wifi_auth_mode_t auth;
  int32_t rssi;
  uint8_t channel;
};
HiRow hiRows[48];

bool hiIsRisky(wifi_auth_mode_t auth) {
  return auth == WIFI_AUTH_OPEN || auth == WIFI_AUTH_WEP ||
         auth == WIFI_AUTH_WPA_PSK || auth == WIFI_AUTH_WPA_WPA2_PSK;
}

void hiRebuildVisible() {
  hiVisibleCount = 0;
  for (int i = 0; i < hiStored; i++) {
    bool risky = hiIsRisky(hiRows[i].auth);
    if (hiFilter == 1 && !risky) continue;
    if (hiFilter == 2 && risky) continue;
    hiVisible[hiVisibleCount++] = i;
  }
  int maxScroll = max(0, hiVisibleCount - 6);
  if (hiScroll > maxScroll) hiScroll = maxScroll;
}

void hiOpen() {
  hiProcessed = -1;
  hiOpen_ = hiLegacy = hiStrong = hiTotal = hiStored = 0;
  hiFilter = 0;
  hiSelected = -1;
  hiScroll = 0;
  hiVisibleCount = 0;
  hiLastScan = millis();
  wifiScanBegin();
}
void hiTick(uint32_t now) {
  int st = wifiScanState();
  if (st >= 0 && st != hiProcessed) {
    hiProcessed = st;
    int n = g_rfNetCount;
    if (n > st) n = st;
    if (n > 48) n = 48;
    hiOpen_ = hiLegacy = hiStrong = 0;
    hiTotal = st;
    hiStored = n;
    for (int i = 0; i < n; i++) {
      hiRows[i].auth = g_rfNets[i].auth;
      strncpy(hiRows[i].ssid, g_rfNets[i].ssid, sizeof(hiRows[i].ssid) - 1);
      hiRows[i].ssid[sizeof(hiRows[i].ssid) - 1] = 0;
      memcpy(hiRows[i].bssid, g_rfNets[i].bssid, 6);
      hiRows[i].rssi = g_rfNets[i].rssi;
      hiRows[i].channel = g_rfNets[i].channel;
    }
    int scannedCount = g_rfNetCount;
    if (scannedCount > st) scannedCount = st;
    for (int i = 0; i < scannedCount; i++) {
      wifi_auth_mode_t auth = g_rfNets[i].auth;
      int risk = encRisk(auth);
      if (auth == WIFI_AUTH_OPEN || auth == WIFI_AUTH_WEP) hiOpen_++;
      else if (auth == WIFI_AUTH_WPA_PSK || auth == WIFI_AUTH_WPA_WPA2_PSK)
        hiLegacy++;
      else if (risk == 0) hiStrong++;
    }
    hiLastScan = now;
    hiRebuildVisible();
    if (hiSelected >= hiStored) hiSelected = -1;
  } else if (st >= 0 && st == hiProcessed && now - hiLastScan > 12000) {
    hiLastScan = now;
    hiProcessed = -1;
    wifiScanBegin();
  }
}
void hiClose() { wifiRfLeave(); hiSelected = -1; }

bool hiSave() {
  if (!app::sdReady()) {
    tools::toast("Audit save failed · no SD");
    return false;
  }
  if (!hiStored) {
    tools::toast("No audit rows to save");
    return false;
  }
  if (!SD_MMC.exists("/log") && !SD_MMC.mkdir("/log")) {
    tools::toast("Audit save failed · /log");
    return false;
  }
  const char* path = "/log/hull_audit.csv";
  bool header = !SD_MMC.exists(path);
  File file = SD_MMC.open(path, FILE_APPEND);
  if (!file) {
    tools::toast("Audit save failed · open");
    return false;
  }
  if (header)
    file.println("timestamp,SSID,BSSID,auth,channel,RSSI,risk,vendor,latitude,longitude");
  char timestamp[24];
  if (!gps::formatTimestamp(timestamp, sizeof(timestamp)))
    snprintf(timestamp, sizeof(timestamp), "uptime-%lu",
             (unsigned long)(millis() / 1000));
  for (int i = 0; i < hiStored; i++) {
    const HiRow& row = hiRows[i];
    char ssid[40], bssid[18];
    snprintf(ssid, sizeof(ssid), "%s", row.ssid);
    for (char* p = ssid; *p; p++)
      if (*p == ',' || *p == '\n' || *p == '\r') *p = ' ';
    cnFmtMac(bssid, sizeof(bssid), row.bssid);
    const char* vendorName = oui::vendor(row.bssid);
    file.printf("%s,%s,%s,%s,%u,%d,%s,%s,", timestamp, ssid, bssid,
                encLabel(row.auth), row.channel, (int)row.rssi,
                hiIsRisky(row.auth) ? "review" : "advertised-secure",
                vendorName[0] ? vendorName : "unknown");
    if (gps::hasFix())
      file.printf("%.6f,%.6f\n", gps::latitude(), gps::longitude());
    else
      file.println(",");
  }
  file.close();
  tools::toast("Saved %d audit rows", hiStored);
  return true;
}

void hiDrawDetail(int x, int y, int w, int h) {
  body(x, y, w, h);
  if (hiSelected < 0 || hiSelected >= hiStored) return;
  const HiRow& row = hiRows[hiSelected];
  char bssid[18], channel[16], signal[16], vendor[24];
  cnFmtMac(bssid, sizeof(bssid), row.bssid);
  snprintf(channel, sizeof(channel), "%u · %s", row.channel,
           bandFromChannel(row.channel));
  snprintf(signal, sizeof(signal), "%d dBm", (int)row.rssi);
  const char* vendorName = oui::vendor(row.bssid);
  snprintf(vendor, sizeof(vendor), "%s", vendorName[0] ? vendorName : "unknown");
  const char* riskLabel = row.auth == WIFI_AUTH_OPEN ? "OPEN"
                          : row.auth == WIFI_AUTH_WEP ? "WEP"
                          : hiIsRisky(row.auth)       ? "LEGACY WPA"
                                                     : "ADVERTISED SECURE";
  uint16_t riskColorValue = hiIsRisky(row.auth) ? theme::kWarn : theme::kGood;
  const char* values[] = {riskLabel, encLabel(row.auth), signal};
  const char* labels[] = {"review", "auth", "signal"};
  const uint16_t colors[] = {riskColorValue, riskColor(encRisk(row.auth)),
                             theme::kCyan};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, values, labels, colors) + 3;
  gfxu::drawKVRow(G(), x + 6, below, w - 12, 18, "SSID", row.ssid);
  gfxu::drawKVRow(G(), x + 6, below + 20, w - 12, 18, "BSSID", bssid,
                  theme::kCyan);
  gfxu::drawKVRow(G(), x + 6, below + 40, w - 12, 18, "Vendor", vendor,
                  theme::kTeal);
  gfxu::drawKVRow(G(), x + 6, below + 60, w - 12, 18, "Channel", channel);
  gfxu::drawKVRow(G(), x + 6, below + 80, w - 12, 18, "Advertised auth",
                  encLabelLong(row.auth), riskColorValue);
  txt(x + 10, y + h - 41, theme::kInkMuted, 1,
      "Passive beacon only · WPS/PMF not reported by scan");
  btn(x + 6, y + h - 24, 72, 20, "BACK", false);
  btn(x + w - 84, y + h - 24, 78, 20, "SAVE", false);
}

void hiDraw(int x, int y, int w, int h) {
  if (hiSelected >= 0) {
    hiDrawDetail(x, y, w, h);
    return;
  }
  body(x, y, w, h);
  char vOpen[8], vLegacy[8], vStrong[8], vTot[8];
  snprintf(vOpen, sizeof(vOpen), "%d", hiOpen_);
  snprintf(vLegacy, sizeof(vLegacy), "%d", hiLegacy);
  snprintf(vStrong, sizeof(vStrong), "%d", hiStrong);
  snprintf(vTot, sizeof(vTot), "%d", hiTotal);
  const char* vals[] = {vOpen, vLegacy, vStrong, vTot};
  const char* labs[] = {"open/WEP", "legacy WPA", "WPA2/3", "APs"};
  uint16_t cols[] = {hiOpen_ ? theme::kBad : theme::kGood,
                     hiLegacy ? theme::kWarn : theme::kInkDim, theme::kGood,
                     theme::kTeal};
  int below = kpiStrip(x + 4, y + 2, w - 8, 4, vals, labs, cols) + 2;
  gfxu::printFit(G(), x + 8, below, w - 16, theme::kInkMuted, 1,
                 "Beacon auth only · WPS/PMF not reported");
  int toolbarY = below + 10;
  gfxu::drawToolbar(G(), x + 4, toolbarY, w - 8, 18);
  chip(x + 6, toolbarY + 1, 42, 16, "ALL", hiFilter == 0);
  chip(x + 50, toolbarY + 1, 52, 16, "RISK", hiFilter == 1, theme::kWarn);
  chip(x + 104, toolbarY + 1, 72, 16, "SECURE", hiFilter == 2, theme::kGood);
  chip(x + 240, toolbarY + 1, 72, 16, "SAVE", false, theme::kCyan);
  int listTop = toolbarY + 20;
  constexpr int kRh = 18;
  int rows = min(hiVisibleCount - hiScroll,
                 max(0, (y + h - 18 - listTop) / kRh));
  for (int i = 0; i < rows; i++) {
    int index = hiVisible[hiScroll + i];
    const HiRow& row = hiRows[index];
    int ry = listTop + i * kRh;
    if (i & 1) G().fillRect(x + 4, ry, w - 8, kRh, theme::kPanelSoft);
    rssiBars(x + 8, ry + 3, row.rssi);
    gfxu::printFit(G(), x + 29, ry + 1, 124, theme::kInk, 1, row.ssid);
    char meta[38];
    snprintf(meta, sizeof(meta), "ch%u %ddBm · %s", row.channel, (int)row.rssi,
             oui::vendor(row.bssid)[0] ? oui::vendor(row.bssid) : "unknown");
    gfxu::printFit(G(), x + 29, ry + 9, 166, theme::kInkMuted, 1, meta);
    gfxu::printFit(G(), x + 224, ry + 4, 84, riskColor(encRisk(row.auth)), 1,
                   encLabel(row.auth));
    G().drawFastHLine(x + 8, ry + kRh - 1, w - 16, theme::kBorder);
  }
  if (hiScroll > 0) btn(x + w - 28, listTop, 24, 18, "^", false);
  if (hiScroll + rows < hiVisibleCount)
    btn(x + w - 28, y + h - 31, 24, 18, "v", false);
  if (!hiVisibleCount)
    txt(x + 10, listTop + 6, theme::kInkMuted, 1,
        hiTotal ? "No APs in this posture filter" : "Waiting for passive AP scan...");
  gfxu::printFit(G(), x + 8, y + h - 10, w - 16, theme::kInkMuted, 1,
                 "Row=details · scan every 12s · passive only");
}
bool hiTouch(int16_t x, int16_t y) {
  if (hiSelected >= 0) {
    if (y >= theme::kScreenH - 34) {
      if (x < 90) hiSelected = -1;
      else hiSave();
    }
    return true;
  }
  const int toolbarY = contentTop() + 36;
  if (y >= toolbarY && y < toolbarY + 20) {
    if (x < 48) hiFilter = 0;
    else if (x < 102) hiFilter = 1;
    else if (x < 178) hiFilter = 2;
    else if (x >= 240) hiSave();
    hiScroll = 0;
    hiRebuildVisible();
    return true;
  }
  const int listTop = toolbarY + 20;
  constexpr int rowH = 18;
  const int rows = max(1, (theme::kScreenH - 18 - listTop) / rowH);
  if (x >= theme::kScreenW - 30) {
    if (y < listTop + rowH && hiScroll > 0) hiScroll--;
    else if (y > theme::kScreenH - 34 && hiScroll + rows < hiVisibleCount)
      hiScroll++;
    return true;
  }
  if (y >= listTop && y < listTop + rows * rowH) {
    int row = (y - listTop) / rowH;
    if (hiScroll + row < hiVisibleCount)
      hiSelected = hiVisible[hiScroll + row];
  }
  return true;
}
}  // namespace

// ===========================================================================
//  9. Captain's Log -- microSD browser + WiGLE / text preview
// ===========================================================================
namespace {
String clFiles[32];
long clSizes[32];
int clCount = 0;
int clScroll = 0;
bool clSortBySize = false;
int clMode = 0;  // 0=list, 1=preview
String clOpenName;
String clLines[14];
int clLineCount = 0;
int clPreviewScroll = 0;
bool clIsWigle = false;
bool clIsCsv = false;
int clWigleRows = 0;
int clWigleUnique = 0;
int clCsvRows = 0;
int clCsvUnique = 0;
bool clCsvHasKeys = false;

void clAddEntry(const String& displayName, long size) {
  if (clCount >= 32) return;
  clFiles[clCount] = displayName;
  clSizes[clCount] = size;
  clCount++;
}

void clSortEntries() {
  for (int i = 1; i < clCount; i++) {
    String name = clFiles[i];
    long size = clSizes[i];
    int j = i;
    auto shouldShift = [&](int previous) {
      if (clSortBySize) {
        long prevSize = clSizes[previous] < 0 ? LONG_MAX : clSizes[previous];
        long nextSize = size < 0 ? LONG_MAX : size;
        return prevSize < nextSize;
      }
      return strcasecmp(clFiles[previous].c_str(), name.c_str()) > 0;
    };
    while (j > 0 && shouldShift(j - 1)) {
      clFiles[j] = clFiles[j - 1];
      clSizes[j] = clSizes[j - 1];
      j--;
    }
    clFiles[j] = name;
    clSizes[j] = size;
  }
}

void clScanDir(const char* dirPath, const char* prefix) {
  File dir = SD_MMC.open(dirPath);
  if (!dir) return;
  File f = dir.openNextFile();
  while (f && clCount < 32) {
    String n = String(f.name());
    int slash = n.lastIndexOf('/');
    if (slash >= 0) n = n.substring(slash + 1);
    if (f.isDirectory()) {
      // Skip nested dirs in /log; skip [log] at root (expanded below).
      if (prefix && prefix[0]) { /* ignore nested */ }
      else if (n == "log") { /* expanded via clScanDir("/log") */ }
      else
        clAddEntry(String("[") + n + "]", -1);
    } else {
      String shown = prefix && prefix[0] ? (String(prefix) + n) : n;
      clAddEntry(shown, (long)f.size());
    }
    f = dir.openNextFile();
  }
  dir.close();
}

void clRefresh() {
  clCount = 0;
  clScroll = 0;
  if (!app::sdReady()) return;
  clScanDir("/", "");
  // Crow's Nest / Chart Room style logs under /log (skip if already listed as [log]).
  if (SD_MMC.exists("/log")) clScanDir("/log", "log/");
  clSortEntries();
}

bool clLooksText(const String& name) {
  String l = name;
  l.toLowerCase();
  return l.endsWith(".csv") || l.endsWith(".txt") || l.endsWith(".log") ||
         l.endsWith(".json") || l.endsWith(".nmea") || l.endsWith(".md");
}

void clLoadPreview(const String& name) {
  clOpenName = name;
  clLineCount = 0;
  clPreviewScroll = 0;
  clIsWigle = false;
  clIsCsv = false;
  clWigleRows = 0;
  clWigleUnique = 0;
  clCsvRows = 0;
  clCsvUnique = 0;
  clCsvHasKeys = false;
  for (int i = 0; i < 14; i++) clLines[i] = "";
  if (!app::sdReady()) return;
  String path = name;
  if (!path.startsWith("/")) path = String("/") + path;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) {
    clLines[0] = "(could not open)";
    clLineCount = 1;
    return;
  }
  String lower = name;
  lower.toLowerCase();
  clIsCsv = lower.endsWith(".csv");
  clIsWigle = lower.endsWith(".csv") &&
              (lower.indexOf("wigle") >= 0 || lower == "wigle.csv");

  if (clIsWigle) {
    // Summarize WiGLE CSV: skip meta + header, collect unique SSIDs sample.
    uint32_t hashes[64];
    int hashCount = 0;
    auto ssidHash = [](const String& s) -> uint32_t {
      uint32_t h = 2166136261u;
      for (size_t i = 0; i < s.length(); i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
      }
      return h;
    };
    int lineNo = 0;
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (!line.length()) continue;
      lineNo++;
      if (lineNo == 1 && line.startsWith("WigleWifi")) continue;
      if (line.startsWith("MAC,SSID")) continue;
      clWigleRows++;
      // MAC,SSID,AuthMode,...
      int c1 = line.indexOf(',');
      int c2 = c1 >= 0 ? line.indexOf(',', c1 + 1) : -1;
      int c3 = c2 >= 0 ? line.indexOf(',', c2 + 1) : -1;
      int c4 = c3 >= 0 ? line.indexOf(',', c3 + 1) : -1;
      int c5 = c4 >= 0 ? line.indexOf(',', c4 + 1) : -1;
      int c6 = c5 >= 0 ? line.indexOf(',', c5 + 1) : -1;
      if (c1 < 0 || c2 < 0 || c6 < 0) continue;
      String ssid = line.substring(c1 + 1, c2);
      String auth = line.substring(c2 + 1, c3);
      String rssi = line.substring(c5 + 1, c6);
      if (!ssid.length()) ssid = "<hidden>";
      uint32_t h = ssidHash(ssid);
      bool found = false;
      for (int i = 0; i < hashCount; i++)
        if (hashes[i] == h) { found = true; break; }
      if (!found && hashCount < 64) hashes[hashCount++] = h;
      if (clLineCount < 14) {
        if (ssid.length() > 14) ssid = ssid.substring(0, 14);
        if (auth.length() > 10) auth = auth.substring(0, 10);
        clLines[clLineCount++] =
            ssid + "  " + rssi + "dB  " + auth;
      }
    }
    clWigleUnique = hashCount;
  } else if (clIsCsv) {
    uint32_t hashes[64];
    int hashCount = 0;
    int lineNo = 0;
    int keyColumn = -1;
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (!line.length()) continue;
      lineNo++;
      if (lineNo == 1) {
        String header = line;
        header.toLowerCase();
        int col = 0, start = 0;
        while (start <= header.length()) {
          int comma = header.indexOf(',', start);
          if (comma < 0) comma = header.length();
          String field = header.substring(start, comma);
          field.trim();
          if (field == "bssid" || field == "mac") keyColumn = col;
          col++;
          if (comma >= header.length()) break;
          start = comma + 1;
        }
        clCsvHasKeys = keyColumn >= 0;
        if (clLineCount < 14) clLines[clLineCount++] = line.substring(0, 42);
        continue;
      }
      clCsvRows++;
      if (clCsvHasKeys) {
        int start = 0, column = 0;
        while (start <= line.length()) {
          int comma = line.indexOf(',', start);
          if (comma < 0) comma = line.length();
          if (column == keyColumn) {
            String key = line.substring(start, comma);
            uint32_t hash = 2166136261u;
            for (size_t c = 0; c < key.length(); c++) {
              hash ^= (uint8_t)key[c];
              hash *= 16777619u;
            }
            bool found = false;
            for (int i = 0; i < hashCount; i++)
              if (hashes[i] == hash) { found = true; break; }
            if (!found && key.length() && hashCount < 64) hashes[hashCount++] = hash;
            break;
          }
          column++;
          if (comma >= line.length()) break;
          start = comma + 1;
        }
      }
      if (clLineCount < 14) clLines[clLineCount++] = line.substring(0, 42);
    }
    clCsvUnique = clCsvHasKeys ? hashCount : clCsvRows;
  } else {
    while (f.available() && clLineCount < 14) {
      String line = f.readStringUntil('\n');
      line.replace('\r', ' ');
      if (line.length() > 42) line = line.substring(0, 42);
      clLines[clLineCount++] = line;
    }
  }
  f.close();
}

void clOpen() {
  clMode = 0;
  clRefresh();
}
void clTick(uint32_t) {}
void clClose() { clMode = 0; }

void clDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  if (!app::sdReady()) {
    txt(x + 8, y + 10, theme::kBad, 1, "No card in the hold.");
    txt(x + 8, y + 26, theme::kInkDim, 1, "Insert a microSD and reopen.");
    return;
  }

  if (clMode == 1) {
    // Preview pane
    btn(x + 8, y + 4, 70, 20, "< Back", false);
    String title = clOpenName;
    if (title.length() > 22) title = title.substring(0, 22);
    txt(x + 86, y + 8, theme::kGold, 1, "%s", title.c_str());

    if (clIsWigle) {
      txt(x + 8, y + 28, theme::kGood, 1, "WiGLE: %d rows, %d unique SSIDs",
          clWigleRows, clWigleUnique);
      txt(x + 8, y + 40, theme::kInkDim, 1, "SSID           RSSI  Auth");
      int rows = min(10, clLineCount - clPreviewScroll);
      for (int i = 0; i < rows; i++) {
        int ry = y + 54 + i * 12;
        txt(x + 8, ry, theme::kInk, 1, "%s", clLines[clPreviewScroll + i].c_str());
      }
      if (clWigleRows == 0)
        txt(x + 8, y + 54, theme::kInkDim, 1, "(empty log — sail Chart Room)");
    } else if (clIsCsv) {
      txt(x + 8, y + 28, theme::kCyan, 1, "CSV: %d rows · %d %s", clCsvRows,
          clCsvUnique, clCsvHasKeys ? "unique keys" : "sample lines");
      int rows = min(11, clLineCount - clPreviewScroll);
      for (int i = 0; i < rows; i++) {
        int ry = y + 44 + i * 13;
        gfxu::printFit(G(), x + 8, ry, w - 16, theme::kInk, 1,
                       clLines[clPreviewScroll + i].c_str());
      }
      if (clCsvRows == 0)
        txt(x + 8, y + 44, theme::kInkDim, 1, "(CSV header only)");
    } else {
      txt(x + 8, y + 28, theme::kInkDim, 1, "Preview (first lines):");
      int rows = min(12, clLineCount - clPreviewScroll);
      for (int i = 0; i < rows; i++) {
        int ry = y + 42 + i * 12;
        gfxu::printFit(G(), x + 8, ry, w - 16, theme::kInk, 1,
                       clLines[clPreviewScroll + i].c_str());
      }
      if (clLineCount == 0)
        txt(x + 8, y + 42, theme::kInkDim, 1, "(empty or binary file)");
    }
    if (clPreviewScroll > 0) btn(x + w - 32, y + 28, 24, 18, "^", false);
    if (clPreviewScroll + 10 < clLineCount)
      btn(x + w - 32, y + h - 28, 24, 18, "v", false);
    return;
  }

  // List pane — storage overview, explicit refresh/sort controls, file rows.
  char vItems[8], vSd[8], vBytes[12];
  uint64_t totalBytes = 0;
  for (int i = 0; i < clCount; i++)
    if (clSizes[i] > 0) totalBytes += (uint64_t)clSizes[i];
  snprintf(vItems, sizeof(vItems), "%d", clCount);
  snprintf(vSd, sizeof(vSd), "%s", app::sdReady() ? "OK" : "--");
  if (totalBytes >= 1024 * 1024)
    snprintf(vBytes, sizeof(vBytes), "%llu", (unsigned long long)(totalBytes / (1024 * 1024)));
  else
    snprintf(vBytes, sizeof(vBytes), "%llu", (unsigned long long)(totalBytes / 1024));
  const char* vals[] = {vItems, vSd, vBytes};
  const char* labs[] = {"items", "SD", totalBytes >= 1024 * 1024 ? "MB" : "KB"};
  uint16_t cols[] = {theme::kTeal, theme::kGood, theme::kGold};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 2;
  gfxu::drawToolbar(G(), x + 4, below, w - 8, 18);
  chip(x + 6, below + 1, 76, 16, clSortBySize ? "SIZE" : "NAME", false,
       theme::kGold);
  chip(x + 88, below + 1, 88, 16, "REFRESH", false, theme::kCyan);
  int listTop = below + 20;
  const int visible = 6;
  constexpr int kRh = theme::kRowH;
  if (clScroll > clCount - visible) clScroll = max(0, clCount - visible);
  if (clScroll < 0) clScroll = 0;
  int rows = min(visible, clCount - clScroll);
  for (int i = 0; i < rows; i++) {
    int idx = clScroll + i;
    int ry = listTop + i * kRh;
    bool wigle = false;
    String low = clFiles[idx];
    low.toLowerCase();
    if (low.indexOf("wigle") >= 0 && low.endsWith(".csv")) wigle = true;
    String nm = clFiles[idx];
    if (nm.length() > 22) nm = nm.substring(0, 22);
    if (i & 1) G().fillRect(x + 4, ry, w - 36, kRh, theme::kPanelSoft);
    txt(x + 10, ry + 5, wigle ? theme::kGood : theme::kInk, 1, "%s",
        nm.c_str());
    if (clSizes[idx] >= 0) {
      char sz[16];
      snprintf(sz, sizeof(sz), "%ldB", clSizes[idx]);
      int mw = (int)strlen(sz) * 6;
      txt(x + w - mw - 40, ry + 5, theme::kInkDim, 1, "%s", sz);
    }
    G().drawFastHLine(x + 8, ry + kRh - 1, w - 40, theme::kBorder);
  }
  // Scroll affordances
  if (clCount > visible) {
    btn(x + 280, listTop, 28, 20, "^", false);
    btn(x + 280, y + h - 28, 28, 20, "v", false);
  }
}

bool clTouch(int16_t x, int16_t y) {
  int ly = y - contentTop();  // content-local
  if (clMode == 1) {
    if (ly >= 0 && ly <= 28 && x >= 8 && x <= 90) {
      clMode = 0;
      return true;
    }
    if (x >= theme::kScreenW - 40) {
      if (ly < 54) clPreviewScroll = max(0, clPreviewScroll - 8);
      else if (ly > theme::kToolBodyH - 42)
        clPreviewScroll = min(max(0, clLineCount - 10), clPreviewScroll + 8);
      return true;
    }
    return true;
  }
  const int toolbarY = theme::kKpiH + 4;
  if (ly >= toolbarY && ly < toolbarY + 20) {
    if (x >= 6 && x <= 84) {
      clSortBySize = !clSortBySize;
      clSortEntries();
      clScroll = 0;
    } else if (x >= 88 && x <= 178) {
      clRefresh();
    }
    return true;
  }
  const int listY = toolbarY + 20;
  if (clCount > 6 && x >= 270) {
    if (ly >= listY && ly <= listY + 24) {
      clScroll = max(0, clScroll - 3);
      return true;
    }
    if (ly >= theme::kToolBodyH - 36) {
      clScroll = min(max(0, clCount - 6), clScroll + 3);
      return true;
    }
  }
  // Row tap
  if (ly >= listY && ly <= listY + 6 * theme::kRowH && x < 270) {
    int row = (ly - listY) / theme::kRowH;
    int idx = clScroll + row;
    if (idx >= 0 && idx < clCount && clSizes[idx] >= 0) {
      if (clLooksText(clFiles[idx])) {
        clLoadPreview(clFiles[idx]);
        clMode = 1;
      } else {
        tools::toast("Binary — use companion LOGGET");
      }
      return true;
    }
  }
  // Empty space is inert; refresh is an explicit toolbar action.
  return true;
}
}  // namespace

// ===========================================================================
//  10. Ship's Systems -- diagnostics
// ===========================================================================
namespace {
void ssOpen() {}
void ssTick(uint32_t) {}
void ssClose() {}
void ssDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  const uint32_t heapTotal = ESP.getHeapSize();
  const uint32_t heapFree = ESP.getFreeHeap();
  const uint32_t psramTotal = ESP.getPsramSize();
  const uint32_t psramFree = ESP.getFreePsram();
  const int heapUsedPct = heapTotal ? (int)((heapTotal - heapFree) * 100 / heapTotal) : 0;
  const int psramUsedPct = psramTotal ? (int)((psramTotal - psramFree) * 100 / psramTotal) : 0;
  const int battery = power::batteryPct();
  char vHeap[10], vPsram[10], vPower[12];
  snprintf(vHeap, sizeof(vHeap), "%u", (unsigned)(heapFree / 1024));
  snprintf(vPsram, sizeof(vPsram), "%u", (unsigned)(psramFree / 1024));
  snprintf(vPower, sizeof(vPower), "%s", power::usbPowered() ? "USB" : "BAT");
  const char* values[] = {vHeap, vPsram, vPower};
  const char* labels[] = {"heap KB", "PSRAM KB", "power"};
  const uint16_t colors[] = {heapUsedPct > 85 ? theme::kWarn : theme::kTeal,
                             psramUsedPct > 85 ? theme::kWarn : theme::kCyan,
                             power::lowBattery() ? theme::kBad : theme::kGood};
  kpiStrip(x + 4, y + 2, w - 8, 3, values, labels, colors);

  auto memoryBar = [&](int barY, const char* label, uint32_t freeBytes,
                       uint32_t totalBytes, int usedPct, uint16_t color) {
    char amount[28];
    snprintf(amount, sizeof(amount), "%s %u/%u KB free", label,
             (unsigned)(freeBytes / 1024), (unsigned)(totalBytes / 1024));
    txt(x + 8, barY, theme::kInkDim, 1, "%s", amount);
    const int barX = x + 8, barW = w - 16, barHeight = 7;
    G().fillRect(barX, barY + 10, barW, barHeight, theme::kBgDeep);
    G().drawRect(barX, barY + 10, barW, barHeight, theme::kBorder);
    int fill = totalBytes ? (barW - 2) * usedPct / 100 : 0;
    if (fill > 0) G().fillRect(barX + 1, barY + 11, fill, barHeight - 2, color);
  };
  memoryBar(y + 36, "RAM", heapFree, heapTotal, heapUsedPct,
            heapUsedPct > 85 ? theme::kWarn : theme::kTeal);
  memoryBar(y + 56, "PSRAM", psramFree, psramTotal, psramUsedPct,
            psramUsedPct > 85 ? theme::kWarn : theme::kCyan);

  int below = y + 78;
  char value[56];
  snprintf(value, sizeof(value), "%s · %d core · %d MHz", ESP.getChipModel(),
           ESP.getChipCores(), getCpuFrequencyMhz());
  gfxu::drawKVRow(G(), x + 6, below, w - 12, 16, "Chip", value, theme::kCyan);
  snprintf(value, sizeof(value), "%u MB · used %u MB",
           (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)),
           (unsigned)((ESP.getSketchSize() + 1024 * 1024 - 1) / (1024 * 1024)));
  gfxu::drawKVRow(G(), x + 6, below + 18, w - 12, 16, "Flash", value);
  snprintf(value, sizeof(value), "%s · %s", app::sdReady() ? "mounted" : "absent",
           app::sdReady() ? "FAT volume" : "check card");
  gfxu::drawKVRow(G(), x + 6, below + 36, w - 12, 16, "SD",
                  value, app::sdReady() ? theme::kGood : theme::kBad);
  snprintf(value, sizeof(value), "%s · %lu sats · HDOP %.1f",
           gps::hasFix() ? "FIX" : gps::statusLabel(),
           (unsigned long)gps::satellites(), gps::hdop());
  gfxu::drawKVRow(G(), x + 6, below + 54, w - 12, 16, "GPS", value,
                  gps::hasFix() ? theme::kGood : theme::kInkMuted);
  snprintf(value, sizeof(value), "W%u B%u · RF %d/%d · scan %d",
           tools::lastWifiCount(), tools::lastBleCount(), tools::rfWant(),
           tools::rfPhase(), tools::rfScanStatus());
  gfxu::drawKVRow(G(), x + 6, below + 72, w - 12, 16, "Radio", value,
                  tools::rfWant() ? theme::kCyan : theme::kInkDim);
  snprintf(value, sizeof(value), "%s · %lumV · %lu:%02lu:%02lu",
           power::powerLabel(), (unsigned long)power::batteryMv(),
           (unsigned long)(millis() / 3600000),
           (unsigned long)((millis() / 60000) % 60),
           (unsigned long)((millis() / 1000) % 60));
  gfxu::drawKVRow(G(), x + 6, below + 90, w - 12, 16, "Power / up", value,
                  power::lowBattery() ? theme::kBad : theme::kGold);
  snprintf(value, sizeof(value), "RF phase %lu ms · heap used %d%%",
           (unsigned long)tools::rfLastPhaseMs(), heapUsedPct);
  gfxu::printFit(G(), x + 8, below + 110, w - 16, theme::kInkMuted, 1, value);
}
bool ssTouch(int16_t, int16_t) { return false; }
}  // namespace

// ===========================================================================
//  11. Signal Lantern -- persistent RGB LED profile editor (+ sound preference)
// ===========================================================================
namespace {
void slOpen() {}
void slTick(uint32_t) {}   // the LED profile animates from the main loop
void slClose() {}          // profile persists; do NOT switch the LED off
void slDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  char vMode[12], vGlow[8], vSnd[8];
  snprintf(vMode, sizeof(vMode), "%s", led::modeName(led::mode()));
  snprintf(vGlow, sizeof(vGlow), "%d%%", led::brightness());
  snprintf(vSnd, sizeof(vSnd), "%s", app::sound() ? "ON" : "off");
  const char* vals[] = {vMode, vGlow, vSnd};
  const char* labs[] = {"mode", "glow", "sound"};
  uint16_t cols[] = {theme::kGold, theme::kTeal,
                     app::sound() ? theme::kGood : theme::kInkMuted};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 6;

  char modeLab[24];
  snprintf(modeLab, sizeof(modeLab), "Mode: %s", led::modeName(led::mode()));
  btn(x + 8, below, 150, 26, modeLab, false);
  txt(x + 168, below + 8, theme::kInkMuted, 1, "tap to cycle");

  txt(x + 8, below + 36, theme::kInkDim, 1, "Color: %s",
      led::colorName(led::colorIdx()));
  static const char* const kColorTags[led::kColorCount] = {
      "GD", "RD", "GN", "BL", "TL", "PU", "WH", "OR"};
  for (int i = 0; i < led::kColorCount; i++) {
    uint32_t c = led::colorRgb(i);
    uint16_t col565 = G().color565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    int sx = x + 52 + i * 32;
    G().fillRect(sx, below + 30, 26, 24, col565);
    G().drawRect(sx, below + 30, 26, 24, theme::kBorder);
    if (i == led::colorIdx())
      G().drawRect(sx - 2, below + 28, 30, 28, theme::kGold);
    gfxu::printCentered(G(), sx, below + 54, 26, 9, theme::kInkMuted, 1,
                        kColorTags[i]);
  }

  txt(x + 8, below + 68, theme::kInk, 1, "Glow: %d%%", led::brightness());
  btn(x + 150, below + 62, 30, 22, "-", false);
  btn(x + 186, below + 62, 30, 22, "+", false);

  btn(x + 8, below + 92, 150, 26, app::sound() ? "Sound: ON" : "Sound: off",
      false, app::sound() ? theme::kGood : theme::kLocked);

    txt(x + 8, below + 128, theme::kInkMuted, 1,
      "Saved to device · applies across all stations");
}
bool slTouch(int16_t x, int16_t y) {
  int ly = y - contentTop();  // content-local
  const int base = theme::kKpiH + 8;  // matches kpiStrip + 6
  if (ly >= base && ly <= base + 30 && x <= 200) {
    led::setMode((led::mode() + 1) % led::ModeCount);
    return true;
  }
  if (ly >= base + 28 && ly <= base + 60) {
    for (int i = 0; i < led::kColorCount; i++) {
      int sx = 52 + i * 32;
      if (x >= sx - 3 && x <= sx + 29) {
        led::setColorIdx(i);
        return true;
      }
    }
  }
  if (ly >= base + 58 && ly <= base + 90) {
    if (x >= 144 && x <= 182) {
      led::setBrightness(led::brightness() - 5);
      return true;
    }
    if (x >= 183 && x <= 222) {
      led::setBrightness(led::brightness() + 5);
      return true;
    }
  }
  if (ly >= base + 88 && ly <= base + 124 && x >= 8 && x <= 170) {
    app::setSound(!app::sound());
    return true;
  }
  return false;
}
}  // namespace

// ===========================================================================
//  12. Settings Cabin — Ship OS sections (Captain / Display / Power / System)
// ===========================================================================
namespace {
bool seOtaBusy = false;
int seScroll = 0;           // px into scrollable body (below sticky KPI+name)
uint32_t seSleepAt = 0;     // millis deadline for deferred Sleep now (0=none)

constexpr int kSeSecH = 13;
constexpr int kSeBtnH = 22;
constexpr int kSeGap = 4;
constexpr int kSeKvH = 15;
constexpr int kSeOtaH = 28;
constexpr int kSeNameH = 12;
constexpr int kSeStickyH = 2 + theme::kKpiH + 2 + kSeNameH;  // kpi y+2 + KPI + gap + name

// Logical layout of the scrollable region (y=0 at top of scroll content).
struct SeLayout {
  int captainSec, rename;
  int displaySec, bri, sound;
  int powerSec, powerRow;
  int systemSec, kv0;  // live device diagnostics
  int otaCard, actionRow;
  int contentH;
};

SeLayout seLayout() {
  SeLayout L{};
  int y = 0;
  L.captainSec = y; y += kSeSecH;
  L.rename = y; y += kSeBtnH + kSeGap;
  L.displaySec = y; y += kSeSecH;
  L.bri = y; y += kSeBtnH + 2;
  L.sound = y; y += kSeBtnH + kSeGap;
  L.powerSec = y; y += kSeSecH;
  L.powerRow = y; y += kSeBtnH + kSeGap;
  L.systemSec = y; y += kSeSecH;
  L.kv0 = y; y += kSeKvH * 6 + 2;
  L.otaCard = y; y += kSeOtaH + 2;
  L.actionRow = y; y += kSeBtnH + 2;
  L.contentH = y;
  return L;
}

void seSection(int x, int y, int w, const char* title) {
  txt(x + 8, y + 2, theme::kTeal, 1, "%s", title);
  int tw = (int)strlen(title) * 6;
  int lineX = x + 8 + tw + 6;
  int lineW = w - (lineX - x) - 36;  // leave room for scroll chevrons
  if (lineW > 8) G().drawFastHLine(lineX, y + 6, lineW, theme::kBorder);
}

void seFmtUptime(char* buf, size_t n) {
  uint32_t up = millis() / 1000;
  if (up < 3600)
    snprintf(buf, n, "%lu:%02lu", (unsigned long)(up / 60),
             (unsigned long)(up % 60));
  else
    snprintf(buf, n, "%luh%02lu", (unsigned long)(up / 3600),
             (unsigned long)((up % 3600) / 60));
}

void seFmtMac(char* buf, size_t n) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(buf, n, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
}

void seFmtSd(char* buf, size_t n) {
  if (!app::sdReady()) {
    snprintf(buf, n, "absent");
    return;
  }
  uint64_t total = SD_MMC.totalBytes();
  uint64_t used = SD_MMC.usedBytes();
  if (total == 0) {
    snprintf(buf, n, "mounted");
    return;
  }
  uint64_t freeMb = (total - used) / (1024ULL * 1024ULL);
  uint64_t totMb = total / (1024ULL * 1024ULL);
  snprintf(buf, n, "%llu/%llu MB", (unsigned long long)freeMb,
           (unsigned long long)totMb);
}

void seOpen() {
  seOtaBusy = false;
  seScroll = 0;
  seSleepAt = 0;
}

void seTick(uint32_t now) {
  if (seSleepAt != 0 && (int32_t)(now - seSleepAt) >= 0) {
    seSleepAt = 0;
    game::save();
    power::deepSleepNow();
  }
}

void seClose() { seSleepAt = 0; }

void seDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  SeLayout L = seLayout();

  // ---- Sticky KPI: heap free · power · uptime --------------------------------
  char vHeap[10], vBat[10], vUp[10];
  snprintf(vHeap, sizeof(vHeap), "%u",
           (unsigned)(ESP.getFreeHeap() / 1024));
  if (power::usbPowered())
    snprintf(vBat, sizeof(vBat), "%s", power::powerLabel());
  else
    snprintf(vBat, sizeof(vBat), "%d%%", power::batteryPct());
  seFmtUptime(vUp, sizeof(vUp));
  const char* vals[] = {vHeap, vBat, vUp};
  const char* labs[] = {"heap KB", "power", "up"};
  uint16_t cols[] = {
      theme::kTeal,
      power::lowBattery() ? theme::kBad : theme::kGold,
      theme::kCyan};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 2;
  txt(x + 8, below, theme::kInkDim, 1, "%s · %s · fw %s", game::profile.name,
      game::rankTitle(game::profile.level), PP_VERSION);
  below += kSeNameH;

  const int viewTop = below;
  const int viewH = (y + h) - viewTop - 2;
  if (viewH < 40) return;

  // Clamp scroll
  int maxScroll = L.contentH - viewH;
  if (maxScroll < 0) maxScroll = 0;
  if (seScroll > maxScroll) seScroll = maxScroll;
  if (seScroll < 0) seScroll = 0;

  // Clip scrollable region
  G().setClipRect(x, viewTop, w, viewH);

  auto cy = [&](int logicalY) { return viewTop + logicalY - seScroll; };

  // ---- Captain ---------------------------------------------------------------
  seSection(x, cy(L.captainSec), w, "Captain");
  btn(x + 8, cy(L.rename), 140, kSeBtnH, "Rename…", false);

  // ---- Display ---------------------------------------------------------------
  seSection(x, cy(L.displaySec), w, "Display");
  char bri[20];
  snprintf(bri, sizeof(bri), "Bri %d", app::brightness());
  txt(x + 8, cy(L.bri) + 7, theme::kInkDim, 1, "%s", bri);
  btn(x + 72, cy(L.bri), 28, kSeBtnH, "-", false);
  btn(x + 104, cy(L.bri), 28, kSeBtnH, "+", false);
  btn(x + 148, cy(L.sound), 140, kSeBtnH,
      app::sound() ? "Sound: ON" : "Sound: off", false,
      app::sound() ? theme::kGood : theme::kLocked);

  // ---- Power -----------------------------------------------------------------
  seSection(x, cy(L.powerSec), w, "Power");
  char idleLab[24];
  snprintf(idleLab, sizeof(idleLab), "Idle: %s", power::idleSleepLabel());
  btn(x + 8, cy(L.powerRow), 110, kSeBtnH, idleLab, false,
      power::idleSleep() ? theme::kWarn : theme::kPanelHi);
  btn(x + 126, cy(L.powerRow), 110, kSeBtnH, "Sleep now", false, theme::kTeal);

  // ---- System (device info + OTA + reset) ------------------------------------
  seSection(x, cy(L.systemSec), w, "System");
  char v[40];
  snprintf(v, sizeof(v), "%u / %u KB",
           (unsigned)(ESP.getFreeHeap() / 1024),
           (unsigned)(ESP.getMinFreeHeap() / 1024));
  gfxu::drawKVRow(G(), x + 6, cy(L.kv0), w - 40, kSeKvH - 1, "Heap free/min", v,
                  theme::kTeal);
  snprintf(v, sizeof(v), "%u / %u KB",
           (unsigned)(ESP.getFreePsram() / 1024),
           (unsigned)(ESP.getPsramSize() / 1024));
  gfxu::drawKVRow(G(), x + 6, cy(L.kv0 + kSeKvH), w - 40, kSeKvH - 1,
                  "PSRAM free/total", v, theme::kCyan);
  seFmtMac(v, sizeof(v));
  gfxu::drawKVRow(G(), x + 6, cy(L.kv0 + kSeKvH * 2), w - 40, kSeKvH - 1,
                  "WiFi MAC", v, theme::kInkDim);
  snprintf(v, sizeof(v), "AP %d · BLE %d · RF %d/%d", tools::lastWifiCount(),
           tools::lastBleCount(), tools::rfWant(), tools::rfPhase());
  gfxu::drawKVRow(G(), x + 6, cy(L.kv0 + kSeKvH * 3), w - 40, kSeKvH - 1,
                  "Radio", v, theme::kCyan);
  snprintf(v, sizeof(v), "%u MB",
           (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
  gfxu::drawKVRow(G(), x + 6, cy(L.kv0 + kSeKvH * 4), w - 40, kSeKvH - 1,
                  "Flash", v);
  seFmtSd(v, sizeof(v));
  gfxu::drawKVRow(G(), x + 6, cy(L.kv0 + kSeKvH * 5), w - 40, kSeKvH - 1,
                  "SD free", v, app::sdReady() ? theme::kGood : theme::kBad);

  gfxu::drawElevated(G(), x + 4, cy(L.otaCard), w - 36, kSeOtaH - 2);
  txt(x + 10, cy(L.otaCard) + 3, theme::kInkMuted, 1, "OTA: %s", ota::status());
  if (ota::wifiConfigured())
    txt(x + 10, cy(L.otaCard) + 14, theme::kInkMuted, 1, "WiFi:%s  URL:%s",
        ota::wifiSsid(), ota::urlConfigured() ? "set" : "none");
  else
    txt(x + 10, cy(L.otaCard) + 14, theme::kInkMuted, 1,
        "Companion: WIFICFG + OTAURL");

  bool otaOk = ota::wifiConfigured() && ota::urlConfigured();
  btn(x + 8, cy(L.actionRow), 130, kSeBtnH, seOtaBusy ? "OTA…" : "OTA Update",
      otaOk, otaOk ? 0 : theme::kLocked);
  btn(x + 146, cy(L.actionRow), 130, kSeBtnH, "Reset progress", false,
      theme::kBad);

  G().clearClipRect();

  // Scroll chevrons (fixed, outside clip)
  if (maxScroll > 0) {
    btn(x + w - 30, viewTop, 26, 18, "^", false);
    btn(x + w - 30, y + h - 22, 26, 18, "v", false);
  }
}

bool seTouch(int16_t x, int16_t y) {
  int ly = y - contentTop();
  SeLayout L = seLayout();
  const int sticky = kSeStickyH;
  const int viewTop = sticky;  // matches seDraw: KPI(30)+2+name(12)
  // Body height ≈ theme::kToolBodyH; viewH derived the same way as draw.
  const int bodyH = theme::kToolBodyH;
  const int viewH = bodyH - viewTop - 2;
  int maxScroll = L.contentH - viewH;
  if (maxScroll < 0) maxScroll = 0;

  // Scroll chevrons (screen-local within body; x is absolute ~0..320)
  if (maxScroll > 0 && x >= 290) {
    if (ly >= viewTop && ly <= viewTop + 20) {
      seScroll = max(0, seScroll - 40);
      return true;
    }
    if (ly >= bodyH - 24 && ly <= bodyH) {
      seScroll = min(maxScroll, seScroll + 40);
      return true;
    }
  }

  if (ly < viewTop) return false;  // sticky KPI/name — no hits
  int logicalY = (ly - viewTop) + seScroll;

  auto inRow = [&](int rowY, int rowH = kSeBtnH) {
    return logicalY >= rowY && logicalY <= rowY + rowH;
  };

  if (inRow(L.rename) && x >= 8 && x <= 152) {
    app::requestRename();
    return false;
  }
  if (inRow(L.bri)) {
    if (x >= 72 && x <= 104) {
      int next = max(5, (int)app::brightness() - 5);
      app::setBrightness((uint8_t)next);
      return true;
    }
    if (x >= 104 && x <= 136) {
      int next = min(100, (int)app::brightness() + 5);
      app::setBrightness((uint8_t)next);
      return true;
    }
  }
  if (inRow(L.sound) && x >= 148 && x <= 292) {
    app::setSound(!app::sound());
    return true;
  }
  if (inRow(L.powerRow)) {
    if (x >= 8 && x <= 122) {
      power::cycleIdleSleep();
      if (power::idleSleep())
        tools::toast("Idle sleep %s", power::idleSleepLabel());
      else
        tools::toast("Idle sleep off");
      return true;
    }
    if (x >= 126 && x <= 240) {
      // Defer sleep so toast can paint; avoid long UI block.
      tools::toast("Sleeping — tap screen to wake");
      seSleepAt = millis() + 350;
      return true;
    }
  }
  if (inRow(L.actionRow)) {
    if (x >= 8 && x <= 142) {
      if (!ota::wifiConfigured() || !ota::urlConfigured()) {
        tools::toast("Need WIFICFG + OTAURL first");
        return true;
      }
      seOtaBusy = true;
      tools::toast("OTA starting…");
      bool ok = ota::runUpdate();
      seOtaBusy = false;
      if (!ok) tools::toast("OTA: %s", ota::status());
      return true;
    }
    if (x >= 146 && x <= 280) {
      game::resetProgress();
      tools::toast("Progress reset");
      return true;
    }
  }
  return false;
}
}  // namespace

// ===========================================================================
//  13. Probe Watch -- passive probe-request client sniffer
// ===========================================================================
namespace {
struct Probe {
  char mac[18];
  uint32_t ssidHashes[8];
  uint8_t ssidHashCount;
  uint8_t sessionId;
  int8_t rssi;
  int8_t strongestRssi;
  uint16_t hits;
  uint32_t lastSeen;
};
Probe pwList[16];
int pwCount = 0;
uint32_t pwTotal = 0;
int pwChannel = 1;
uint32_t pwHopAt = 0;
uint8_t pwNextSessionId = 1;

// Runs in the Wi-Fi task: parse only the transmitter address + requested SSID
// from a broadcast probe-request management frame. No payload/data frames are
// touched -- this observes devices openly searching for known networks.
void pwPromiscCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  auto* p = (wifi_promiscuous_pkt_t*)buf;
  int len = p->rx_ctrl.sig_len;
  if (len < 24) return;
  const uint8_t* pl = p->payload;
  if (((pl[0] >> 4) & 0x0F) != 4) return;  // probe request subtype only
  pwTotal++;
  char ssid[26] = "";
  if (len >= 26 && pl[24] == 0) {  // first tagged element = SSID
    int sl = pl[25];
    if (sl > 25) sl = 25;
    if (26 + sl <= len) {
      int j = 0;
      for (int k = 0; k < sl; k++) {
        char c = (char)pl[26 + k];
        ssid[j++] = (c >= 32 && c < 127) ? c : '.';
      }
      ssid[j] = 0;
    }
  }
  uint32_t ssidHash = 2166136261u;
  for (const char* c = ssid; *c; c++) {
    ssidHash ^= (uint8_t)*c;
    ssidHash *= 16777619u;
  }
  char mac[18];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", pl[10], pl[11],
           pl[12], pl[13], pl[14], pl[15]);
  for (int i = 0; i < pwCount; i++) {
    if (strcmp(pwList[i].mac, mac) == 0) {
      Probe& probe = pwList[i];
      probe.rssi = p->rx_ctrl.rssi;
      if (probe.rssi > probe.strongestRssi) probe.strongestRssi = probe.rssi;
      if (probe.hits < 65535) probe.hits++;
      probe.lastSeen = millis();
      if (ssid[0] && probe.ssidHashCount < 8) {
        bool known = false;
        for (int j = 0; j < probe.ssidHashCount; j++)
          if (probe.ssidHashes[j] == ssidHash) known = true;
        if (!known) probe.ssidHashes[probe.ssidHashCount++] = ssidHash;
      }
      return;
    }
  }
  int idx = pwCount < 16 ? pwCount++ : (int)(pwTotal % 16);
  Probe& probe = pwList[idx];
  memset(&probe, 0, sizeof(probe));
  strncpy(probe.mac, mac, sizeof(probe.mac) - 1);
  probe.sessionId = pwNextSessionId++;
  if (pwNextSessionId == 0) pwNextSessionId = 1;
  probe.rssi = p->rx_ctrl.rssi;
  probe.strongestRssi = probe.rssi;
  probe.hits = 1;
  probe.lastSeen = millis();
  if (ssid[0]) {
    probe.ssidHashes[0] = ssidHash;
    probe.ssidHashCount = 1;
  }
}
void pwOpen() {
  pwCount = 0;
  pwTotal = 0;
  pwChannel = 1;
  pwNextSessionId = 1;
  wifiPromiscBegin(&pwPromiscCb, pwChannel);
}
void pwTick(uint32_t now) {
  if (g_rfPromiscReady && now - pwHopAt > 350) {
    pwHopAt = now;
    pwChannel = pwChannel >= 13 ? 1 : pwChannel + 1;
    wifiPromiscHop(pwChannel);
  }
}
void pwClose() { wifiRfLeave(); }
void pwDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  int uniqueNames = 0;
  for (int i = 0; i < pwCount; i++) uniqueNames += pwList[i].ssidHashCount;
  char vCli[8], vCh[8], vFrm[10], vNames[8];
  snprintf(vCli, sizeof(vCli), "%d", pwCount);
  snprintf(vCh, sizeof(vCh), "%d", pwChannel);
  snprintf(vFrm, sizeof(vFrm), "%lu", (unsigned long)pwTotal);
  snprintf(vNames, sizeof(vNames), "%d", uniqueNames);
  const char* vals[] = {vCli, vNames, vFrm, vCh};
  const char* labs[] = {"session IDs", "SSID hashes", "frames", "channel"};
  const uint16_t cols[] = {theme::kCyan, theme::kTeal, theme::kGold, theme::kInkDim};
  int below = kpiStrip(x + 4, y + 2, w - 8, 4, vals, labs, cols) + 2;
  gfxu::printFit(G(), x + 8, below, w - 16, theme::kInkMuted, 1,
                 "Session IDs only · SSID names discarded");
  below += 12;
  constexpr int kRh = 19;
  int rows = min(pwCount, 6);
  for (int i = 0; i < rows; i++) {
    int ry = below + i * kRh;
    if (i & 1) G().fillRect(x + 4, ry, w - 8, kRh, theme::kPanelSoft);
    rssiBars(x + 8, ry + 3, pwList[i].rssi);
    char alias[12], meta[30];
    snprintf(alias, sizeof(alias), "P-%02u", pwList[i].sessionId);
    gfxu::printFit(G(), x + 28, ry + 1, 46, theme::kCyan, 1, alias);
    const char* ven = oui::vendorStr(String(pwList[i].mac));
    snprintf(meta, sizeof(meta), "%s · %u names", ven[0] ? ven : "unknown vendor",
             pwList[i].ssidHashCount);
    gfxu::printFit(G(), x + 78, ry + 1, 166, theme::kInk, 1, meta);
    txt(x + 250, ry + 1, theme::kGold, 1, "x%u", pwList[i].hits);
    txt(x + 250, ry + 10, theme::kInkMuted, 1, "%ddB", pwList[i].rssi);
    G().drawFastHLine(x + 8, ry + kRh - 1, w - 16, theme::kBorder);
  }
  if (pwCount == 0)
    gfxu::printFit(G(), x + 12, below + 8, w - 24, theme::kInkMuted, 1,
                   "Listening passively · waiting for probes");
  int buttonY = y + h - 27;
  btn(x + 6, buttonY, 142, 22, "CLEAR SESSION", false);
  btn(x + 158, buttonY, 154, 22, "RESET CHANNEL HOP", false, theme::kCyan);
}
bool pwTouch(int16_t x, int16_t y) {
  if (y < theme::kScreenH - 32) return true;
  if (x < 154) {
    pwCount = 0;
    pwTotal = 0;
    pwNextSessionId = 1;
    return true;
  }
  pwChannel = 1;
  pwHopAt = millis();
  wifiPromiscHop(pwChannel);
  return true;
}
}  // namespace

// ===========================================================================
//  14. Deep BLE ID -- advertisement decoder (company / iBeacon / Eddystone)
// ===========================================================================
namespace {
int dbFilter = 0;  // all, named, decoded, connectable
int dbSort = 0;    // RSSI, name
int dbScroll = 0;
int dbVisible[48];
int dbVisibleCount = 0;
uint32_t dbGeneration = 0;
int dbSelected = -1;

void dbRebuild() {
  dbVisibleCount = 0;
  for (int i = 0; i < g_bleCount; i++) {
    const BleDev& device = g_ble[i];
    if (dbFilter == 1 && !device.name.length()) continue;
    if (dbFilter == 2 && !device.detail[0] && !device.company &&
        !device.serviceUuid[0]) continue;
    if (dbFilter == 3 && !device.connectable) continue;
    dbVisible[dbVisibleCount++] = i;
  }
  auto before = [](int a, int b) {
    if (dbSort == 1) {
      const char* an = g_ble[a].name.length() ? g_ble[a].name.c_str()
                                               : g_ble[a].mac.c_str();
      const char* bn = g_ble[b].name.length() ? g_ble[b].name.c_str()
                                               : g_ble[b].mac.c_str();
      int cmp = strcasecmp(an, bn);
      if (cmp) return cmp < 0;
    }
    return g_ble[a].rssi > g_ble[b].rssi;
  };
  for (int i = 1; i < dbVisibleCount; i++) {
    int item = dbVisible[i], j = i;
    while (j > 0 && before(item, dbVisible[j - 1])) {
      dbVisible[j] = dbVisible[j - 1];
      j--;
    }
    dbVisible[j] = item;
  }
  int maxScroll = max(0, dbVisibleCount - 6);
  if (dbScroll > maxScroll) dbScroll = maxScroll;
}

void dbOpen() {
  dbFilter = 0;
  dbSort = 0;
  dbScroll = 0;
  dbSelected = -1;
  dbGeneration = g_bleGeneration;
  dbRebuild();
  g_bleLastScan = 0;
}
void dbTick(uint32_t now) {
  bleTick(now);
  if (dbGeneration != g_bleGeneration) {
    dbGeneration = g_bleGeneration;
    dbRebuild();
  }
}
void dbClose() { bleStop(); dbSelected = -1; }
void dbDrawDetail(int x, int y, int w, int h) {
  body(x, y, w, h);
  if (dbSelected < 0 || dbSelected >= g_bleCount) return;
  const BleDev& device = g_ble[dbSelected];
  char rssi[12], txPower[12], services[8], company[20], appearance[18];
  snprintf(rssi, sizeof(rssi), "%d dBm", device.rssi);
  snprintf(txPower, sizeof(txPower), "%s", device.hasTxPower ? "present" : "n/a");
  snprintf(services, sizeof(services), "%u", device.serviceCount);
  snprintf(company, sizeof(company), "%s", bleCompanyName(device.company));
  if (!company[0] && device.company)
    snprintf(company, sizeof(company), "0x%04X", device.company);
  if (!company[0]) snprintf(company, sizeof(company), "not advertised");
  if (device.hasAppearance)
    snprintf(appearance, sizeof(appearance), "0x%04X", device.appearance);
  else
    snprintf(appearance, sizeof(appearance), "not advertised");
  const char* values[] = {rssi, device.connectable ? "YES" : "NO", services};
  const char* labels[] = {"signal", "connectable", "services"};
  const uint16_t colors[] = {theme::kCyan,
                             device.connectable ? theme::kGood : theme::kInkDim,
                             device.serviceCount ? theme::kTeal : theme::kInkDim};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, values, labels, colors) + 3;
  String name = device.name.length() ? device.name : String("(unnamed)");
  gfxu::drawKVRow(G(), x + 6, below, w - 12, 18, "Name", name.c_str());
  gfxu::drawKVRow(G(), x + 6, below + 20, w - 12, 18, "Address",
                  device.mac.c_str(), theme::kCyan);
  gfxu::drawKVRow(G(), x + 6, below + 40, w - 12, 18, "Company", company,
                  theme::kTeal);
  gfxu::drawKVRow(G(), x + 6, below + 60, w - 12, 18, "Advertisement",
                  device.detail[0] ? device.detail : "generic", theme::kInkDim);
  gfxu::drawKVRow(G(), x + 6, below + 80, w - 12, 18, "Service UUID",
                  device.serviceUuid[0] ? device.serviceUuid : "not advertised",
                  theme::kInk);
  gfxu::drawKVRow(G(), x + 6, below + 100, w - 12, 18, "TX power",
                  txPower, device.hasTxPower ? theme::kGold : theme::kInkMuted);
  gfxu::drawKVRow(G(), x + 6, below + 120, w - 12, 18, "Appearance",
                  appearance);
  btn(x + 6, y + h - 24, 72, 20, "BACK", false);
}

void dbDraw(int x, int y, int w, int h) {
  if (dbSelected >= 0) {
    dbDrawDetail(x, y, w, h);
    return;
  }
  body(x, y, w, h);
  int named = 0, decoded = 0, connectable = 0;
  for (int i = 0; i < g_bleCount; i++) {
    if (g_ble[i].name.length()) named++;
    if (g_ble[i].detail[0] || g_ble[i].company || g_ble[i].serviceUuid[0]) decoded++;
    if (g_ble[i].connectable) connectable++;
  }
  char vAdv[8], vDec[8], vNam[8], vConn[8];
  snprintf(vAdv, sizeof(vAdv), "%d", g_bleCount);
  snprintf(vDec, sizeof(vDec), "%d", decoded);
  snprintf(vNam, sizeof(vNam), "%d", named);
  snprintf(vConn, sizeof(vConn), "%d", connectable);
  const char* vals[] = {vAdv, vDec, vNam, vConn};
  const char* labs[] = {"seen", "decoded", "named", "connectable"};
  const uint16_t cols[] = {theme::kCyan, theme::kTeal, theme::kGold,
                           connectable ? theme::kGood : theme::kInkDim};
  int below = kpiStrip(x + 4, y + 2, w - 8, 4, vals, labs, cols) + 2;
  gfxu::drawToolbar(G(), x + 4, below, w - 8, 18);
  chip(x + 6, below + 1, 38, 16, "ALL", dbFilter == 0);
  chip(x + 46, below + 1, 46, 16, "NAMED", dbFilter == 1);
  chip(x + 94, below + 1, 42, 16, "DECODED", dbFilter == 2, theme::kTeal);
  chip(x + 138, below + 1, 56, 16, "CONNECT", dbFilter == 3, theme::kGood);
  chip(x + 196, below + 1, 42, 16, dbSort ? "NAME" : "RSSI", false,
       theme::kGold);
  chip(x + 240, below + 1, 72, 16, "RESCAN", false, theme::kCyan);
  int listTop = below + 20;
  constexpr int rowH = 20;
  int rows = min(dbVisibleCount - dbScroll,
                 max(0, (y + h - 12 - listTop) / rowH));
  for (int i = 0; i < rows; i++) {
    const BleDev& device = g_ble[dbVisible[dbScroll + i]];
    int ry = listTop + i * rowH;
    if (i & 1) G().fillRect(x + 4, ry, w - 8, rowH, theme::kPanelSoft);
    String label = device.name.length() ? device.name : device.mac;
    gfxu::printFit(G(), x + 9, ry + 1, 190, theme::kInk, 1, label.c_str());
    const char* vendorName = bleCompanyName(device.company);
    char summary[36];
    if (device.detail[0])
      snprintf(summary, sizeof(summary), "%s · %s", device.detail,
               device.connectable ? "connectable" : "beacon");
    else
      snprintf(summary, sizeof(summary), "%s · %u svc",
               vendorName[0] ? vendorName : "unknown", device.serviceCount);
    gfxu::printFit(G(), x + 9, ry + 10, 208, theme::kInkMuted, 1, summary);
    char rssi[12];
    snprintf(rssi, sizeof(rssi), "%d", device.rssi);
    gfxu::printFit(G(), x + w - 44, ry + 2, 38, theme::kCyan, 1, rssi);
    G().drawFastHLine(x + 8, ry + rowH - 1, w - 16, theme::kBorder);
  }
  if (!dbVisibleCount)
    gfxu::printFit(G(), x + 10, listTop + 8, w - 20, theme::kInkMuted, 1,
                   g_bleScanning ? "Passive scan · previous completed pass retained"
                                 : "No advertisements in last completed pass");
  if (dbScroll > 0) btn(x + w - 28, listTop, 24, 18, "^", false);
  if (dbScroll + rows < dbVisibleCount)
    btn(x + w - 28, y + h - 30, 24, 18, "v", false);
  gfxu::printFit(G(), x + 8, y + h - 10, w - 16, theme::kInkMuted, 1,
                 "Public advertisements only · no GATT connection");
}

bool dbTouch(int16_t x, int16_t y) {
  if (dbSelected >= 0) {
    if (y >= theme::kScreenH - 34 && x < 90) dbSelected = -1;
    return true;
  }
  const int toolbarY = contentTop() + 32;
  if (y >= toolbarY && y < toolbarY + 20) {
    if (x < 45) dbFilter = 0;
    else if (x < 93) dbFilter = 1;
    else if (x < 138) dbFilter = 2;
    else if (x < 196) dbFilter = 3;
    else if (x < 239) dbSort = !dbSort;
    else {
      bleStop();
      g_bleLastScan = 0;
    }
    dbScroll = 0;
    dbRebuild();
    return true;
  }
  const int listTop = toolbarY + 20;
  constexpr int rowH = 20;
  const int rows = max(0, (theme::kScreenH - 12 - listTop) / rowH);
  if (x >= theme::kScreenW - 30) {
    if (y < listTop + rowH && dbScroll > 0) dbScroll--;
    else if (y > theme::kScreenH - 34 && dbScroll + rows < dbVisibleCount)
      dbScroll++;
    return true;
  }
  if (y >= listTop && y < listTop + rows * rowH) {
    int row = (y - listTop) / rowH;
    if (dbScroll + row < dbVisibleCount) dbSelected = dbVisible[dbScroll + row];
  }
  return true;
}
}  // namespace

// ===========================================================================
//  15. Ship's Instruments -- onboard sensors
// ===========================================================================
namespace {
float siTemp = 0;
uint32_t siLast = 0;
constexpr int kSiTempHistory = 48;
float siTempHistory[kSiTempHistory] = {};
int siTempHistoryCount = 0;
int siTempHistoryHead = 0;
bool siFahrenheit = false;
uint32_t siSavedFixes = 0;

void siOpen() {
  siLast = 0;
  siTempHistoryCount = 0;
  siTempHistoryHead = 0;
  siSavedFixes = 0;
}
void siTick(uint32_t now) {
  if (now - siLast > 1000) {
    siLast = now;
    siTemp = temperatureRead();  // on-die core temperature sensor
    siTempHistory[siTempHistoryHead] = siTemp;
    siTempHistoryHead = (siTempHistoryHead + 1) % kSiTempHistory;
    if (siTempHistoryCount < kSiTempHistory) siTempHistoryCount++;
  }
}
void siClose() {}

bool siSaveFix() {
  if (!gps::hasFix()) {
    tools::toast("No GPS fix to save");
    return false;
  }
  if (!app::sdReady()) {
    tools::toast("Save failed · no SD");
    return false;
  }
  if (!SD_MMC.exists("/log") && !SD_MMC.mkdir("/log")) {
    tools::toast("Save failed · /log");
    return false;
  }
  const char* path = "/log/instruments.csv";
  bool header = !SD_MMC.exists(path);
  File file = SD_MMC.open(path, FILE_APPEND);
  if (!file) {
    tools::toast("Save failed · open");
    return false;
  }
  if (header)
    file.println("timestamp,latitude,longitude,altitude_m,satellites,hdop,battery_mv,battery_pct");
  char timestamp[24];
  if (!gps::formatTimestamp(timestamp, sizeof(timestamp)))
    snprintf(timestamp, sizeof(timestamp), "uptime-%lu",
             (unsigned long)(millis() / 1000));
  file.printf("%s,%.6f,%.6f,%.1f,%lu,%.1f,%lu,%d\n", timestamp,
              gps::latitude(), gps::longitude(), gps::altitudeM(),
              (unsigned long)gps::satellites(), gps::hdop(),
              (unsigned long)power::batteryMv(), power::batteryPct());
  file.close();
  siSavedFixes++;
  tools::toast("Saved GPS fix · %lu", (unsigned long)siSavedFixes);
  return true;
}
void siDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  int pct = power::batteryPct();
  bool fix = gps::hasFix();
  float shownTemp = siFahrenheit ? siTemp * 9.0f / 5.0f + 32.0f : siTemp;
  char vTemp[10], vBat[10], vGps[8];
  snprintf(vTemp, sizeof(vTemp), "%.0f%s", shownTemp,
           siFahrenheit ? "F" : "C");
  if (power::usbPowered())
    snprintf(vBat, sizeof(vBat), "%s", power::powerLabel());
  else
    snprintf(vBat, sizeof(vBat), "%d%%", pct);
  snprintf(vGps, sizeof(vGps), "%s", gps::statusLabel());
  const char* vals[] = {vTemp, vBat, vGps};
  const char* labs[] = {"internal", "power", "GPS"};
  uint16_t cols[] = {theme::kGold,
                     power::lowBattery() ? theme::kBad : theme::kTeal,
                     fix ? theme::kGood : theme::kWarn};
  kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols);
  txt(x + 8, y + 35, theme::kInkMuted, 1,
      "ESP32 on-die temperature · not ambient air temperature");

  const int chartX = x + 8, chartY = y + 48, chartW = w - 16, chartH = 38;
  G().fillRect(chartX, chartY, chartW, chartH, theme::kBgDeep);
  G().drawRect(chartX, chartY, chartW, chartH, theme::kBorder);
  for (int line = 1; line <= 2; line++)
    G().drawFastHLine(chartX + 1, chartY + line * (chartH - 2) / 3,
                      chartW - 2, theme::kBorder);
  if (siTempHistoryCount >= 2) {
    int start = (siTempHistoryHead - siTempHistoryCount + kSiTempHistory) %
                kSiTempHistory;
    int prevX = chartX + 2;
    auto tempY = [&](float celsius) {
      float scaled = siFahrenheit ? celsius * 9.0f / 5.0f + 32.0f : celsius;
      float minValue = siFahrenheit ? 86.0f : 30.0f;
      float maxValue = siFahrenheit ? 194.0f : 90.0f;
      if (scaled < minValue) scaled = minValue;
      if (scaled > maxValue) scaled = maxValue;
      return chartY + chartH - 3 - (int)((scaled - minValue) * (chartH - 6) /
                                         (maxValue - minValue));
    };
    int prevY = tempY(siTempHistory[start]);
    for (int i = 1; i < siTempHistoryCount; i++) {
      int index = (start + i) % kSiTempHistory;
      int px = chartX + 2 + i * (chartW - 4) / (siTempHistoryCount - 1);
      int py = tempY(siTempHistory[index]);
      G().drawLine(prevX, prevY, px, py, theme::kGold);
      prevX = px;
      prevY = py;
    }
  } else {
    gfxu::printFit(G(), chartX + 5, chartY + 15, chartW - 10, theme::kInkMuted,
                   1, "Collecting 1-second samples...");
  }

  char value[48];
  snprintf(value, sizeof(value), "%s · %lu sats · HDOP %.1f",
           fix ? "FIX" : gps::statusLabel(),
           (unsigned long)gps::satellites(), gps::hdop());
  gfxu::drawKVRow(G(), x + 6, y + 90, w - 12, 17, "GPS", value,
                  fix ? theme::kGood : theme::kWarn);
  if (fix) {
    snprintf(value, sizeof(value), "%.5f, %.5f · %.0fm", gps::latitude(),
             gps::longitude(), gps::altitudeM());
    gfxu::drawKVRow(G(), x + 6, y + 109, w - 12, 17, "Position", value,
                    theme::kCyan);
  } else {
    gfxu::drawKVRow(G(), x + 6, y + 109, w - 12, 17, "Position",
                    "No fix · TX->GPIO43 RX->GPIO44", theme::kInkMuted);
  }
  snprintf(value, sizeof(value), "%lu mV · %d%% · %s",
           (unsigned long)power::batteryMv(), pct, power::powerLabel());
  gfxu::drawKVRow(G(), x + 6, y + 128, w - 12, 17, "Battery", value,
                  power::lowBattery() ? theme::kBad : theme::kGold);
  btn(x + 6, y + h - 27, 94, 22, siFahrenheit ? "TEMP: F" : "TEMP: C", false);
  btn(x + 108, y + h - 27, 204, 22,
      fix ? "SAVE GPS FIX" : "SAVE FIX · NO GPS", fix, theme::kCyan);
}
bool siTouch(int16_t x, int16_t y) {
  if (y < theme::kScreenH - 32) return true;
  if (x < 104) {
    siFahrenheit = !siFahrenheit;
  } else {
    siSaveFix();
  }
  return true;
}
}  // namespace

// ===========================================================================
//  Registry
// ===========================================================================
namespace tools {

lgfx::LGFXBase* gfx = nullptr;

static const Tool kTools[] = {
    // title, subtitle, tile, accent (cyber-teal stripe — Pirate Cabin), handlers
    {"Crow's Nest", "Wi-Fi survey", "Nest", theme::kTeal, cnOpen, cnTick, cnClose,
     cnDraw, cnTouch},
    {"Harbor Ledger", "BLE discovery", "Harbor", theme::kCyan, hlOpen, hlTick,
     hlClose, hlDraw, hlTouch},
    {"Chart Room", "Wardrive log", "Charts", theme::kGold, crOpen, crTick, crClose,
     crDraw, crTouch},
    {"Lookout", "Channel analyzer", "Lookout", theme::kGood, lkOpen, lkTick,
     lkClose, lkDraw, lkTouch},
    {"Spyglass", "Surveillance", "Spyglass", theme::kWarn, sgOpen, sgTick,
     sgClose, sgDraw, sgTouch},
    {"Tracker Watch", "Anti-stalk BLE", "Tracker", theme::kWarn, twOpen, twTick,
     twClose, twDraw, twTouch},
    {"Rigging Watch", "Deauth detector", "Rigging", theme::kBad, rwOpen, rwTick,
     rwClose, rwDraw, rwTouch},
    {"Hull Inspection", "Network audit", "Hull", theme::kGold, hiOpen, hiTick,
     hiClose, hiDraw, hiTouch},
    {"Captain's Log", "SD + WiGLE", "Log", theme::kBorderHi, clOpen, clTick,
     clClose, clDraw, clTouch},
    {"Ship's Systems", "Diagnostics", "Systems", theme::kCyan, ssOpen, ssTick,
     ssClose, ssDraw, ssTouch},
    {"Signal Lantern", "RGB LED / FX", "Lantern", theme::kGold, slOpen, slTick,
     slClose, slDraw, slTouch},
    {"Probe Watch", "Client sniffer", "Probe", theme::kTeal, pwOpen, pwTick,
     pwClose, pwDraw, pwTouch},
    {"Deep BLE ID", "Adv decoder", "BLE ID", theme::kCyan, dbOpen, dbTick,
     dbClose, dbDraw, dbTouch},
    {"Instruments", "Onboard sensors", "Sensors", theme::kGold, siOpen, siTick,
     siClose, siDraw, siTouch},
    {"Settings Cabin", "Options", "Settings", theme::kBorderHi, seOpen, seTick,
     seClose, seDraw, seTouch},
};

int count() { return sizeof(kTools) / sizeof(kTools[0]); }
const Tool& at(int i) {
  if (i < 0) i = 0;
  if (i >= count()) i = count() - 1;
  return kTools[i];
}

int lastWifiCount() { return g_lastWifiCount; }
int lastBleCount() { return g_bleCount; }

void rfBegin() { wifiRfStartWorker(); }
void rfService() { /* worker task owns the radio */ }
int rfPhase() { return (int)g_rfPhase; }
int rfWant() { return (int)g_rfWant; }
int rfScanStatus() { return g_rfScanCached; }
uint32_t rfLastPhaseMs() { return g_rfLastPhaseMs; }
bool rfPromiscReady() { return g_rfPromiscReady; }

}  // namespace tools
