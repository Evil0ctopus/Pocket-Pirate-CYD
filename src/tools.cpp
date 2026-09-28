#include "tools.h"

#include <WiFi.h>
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
#define PP_VERSION "0.5.3"
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
  auto& g = G();
  g.setTextSize(size);
  g.setTextColor(fg);  // transparent bg — no Win95 opaque dump
  g.setCursor(x, y);
  g.print(b);
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
// v0.5.3: RF state machine runs on a pinned FreeRTOS task (core 0). UI only
// arms flags. Cached scanComplete so draws never call into the driver.
// One Wi-Fi opcode per worker wake; StartScan no longer doubles scanDelete.

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

void wifiScanBegin() {
  g_rfScanCached = WIFI_SCAN_RUNNING;
  g_rfSnapSt = -999;
  wifiRfRequest(RfWant::Scan);
}

void wifiPromiscBegin(wifi_promiscuous_cb_t cb, int channel) {
  g_rfPromiscCb = cb;
  g_rfPromiscChannel = channel < 1 ? 1 : (channel > 13 ? 13 : channel);
  wifiRfRequest(RfWant::Promisc);
}

void wifiRfLeave() {
  g_rfPromiscCb = nullptr;
  wifiRfRequest(RfWant::Leave);
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
        g_rfWant = RfWant::Idle;
        g_rfPhase = RfPhase::Idle;
        break;
      }
      if (st >= 0) {
        rfPublishScanSnap(st);
        break;
      }
      if (millis() - g_rfScanStartedAt > 12000) {
        WiFi.scanDelete();
        g_rfScanCached = WIFI_SCAN_FAILED;
        g_rfWant = RfWant::Idle;
        g_rfPhase = RfPhase::Idle;
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
      wifi_promiscuous_filter_t filt;
      filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
      esp_wifi_set_promiscuous_filter(&filt);
      esp_wifi_set_promiscuous_rx_cb(g_rfPromiscCb);
      esp_wifi_set_promiscuous(true);
      esp_wifi_set_channel(g_rfPromiscChannel, WIFI_SECOND_CHAN_NONE);
      g_rfPromiscReady = true;
      break;
    }

    case RfPhase::LeaveDone:
      g_rfWant = RfWant::Idle;
      g_rfPhase = RfPhase::Idle;
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
      vTaskDelay(pdMS_TO_TICKS(2));  // yield to UI between opcodes
    } else if (after == RfPhase::Scanning || after == RfPhase::PromiscOn) {
      // Poll scanComplete / hold promisc without starving UI.
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30));
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
    default: return "?";
  }
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
};
BleDev g_ble[48];
int g_bleCount = 0;
bool g_bleInited = false;
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
    if (g_bleCount >= (int)(sizeof(g_ble) / sizeof(g_ble[0]))) return;
    String mac = String(d.getAddress().toString().c_str());
    for (int i = 0; i < g_bleCount; i++)
      if (g_ble[i].mac == mac) return;  // already in this pass
    BleDev& e = g_ble[g_bleCount++];
    e.mac = mac;
    e.name = d.haveName() ? String(d.getName().c_str()) : String("");
    e.rssi = d.getRSSI();
    e.kind = classifyBle(d);
    decodeBle(d, e);

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
  bleEnsureInit();
  if (g_bleScanning) return;
  if (now - g_bleLastScan < 3000) return;
  g_bleLastScan = now;
  g_bleCount = 0;
  g_bleScanning = true;
  // duration=1s, callback form returns immediately (no UI freeze).
  g_bleScan->start(1, bleScanDone, false);
}

}  // namespace

// ===========================================================================
//  1. Crow's Nest -- Wi-Fi survey (sort / filter / detail / scroll)
// ===========================================================================
namespace {
struct CnNet {
  char ssid[33];
  uint8_t bssid[6];
  int32_t rssi;
  uint8_t channel;
  wifi_auth_mode_t auth;
};
CnNet cnNets[48];
int cnCount = 0;
int cnProcessed = -1;
uint32_t cnLast = 0;
int cnScroll = 0;          // first visible row
int cnSort = 0;            // 0=RSSI 1=CH 2=SSID
int cnFilter = 0;          // 0=All 1=Open 2=Secure
int cnDetail = -1;         // index into cnNets, or -1

int cnVisibleIdx[48];
int cnVisibleCount = 0;

void cnRebuildVisible() {
  cnVisibleCount = 0;
  for (int i = 0; i < cnCount; i++) {
    bool open = (cnNets[i].auth == WIFI_AUTH_OPEN);
    if (cnFilter == 1 && !open) continue;
    if (cnFilter == 2 && open) continue;
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
  }
  g_lastWifiCount = cnCount;
  cnRebuildVisible();
}

void cnOpen() {
  cnProcessed = -1;
  cnDetail = -1;
  cnScroll = 0;
  cnFilter = 0;  // never open into a filter that hides every row
  cnSort = 0;
  wifiScanBegin();  // arm only — RF deferred
  cnLast = millis();
}
void cnTick(uint32_t now) {
  int st = wifiScanState();
  if (st >= 0 && st != cnProcessed) {
    cnProcessed = st;
    cnCapture(st);
    for (int i = 0; i < cnCount; i++) {
      uint64_t k = bssidKey(cnNets[i].bssid);
      if (g_seenAp.insert(k).second) {
        game::profile.apSeen++;
        game::addLoot(game::Loot::ChartFragment, 1);
        if (game::awardXp(3)) tools::toast("Level up!");
      }
    }
    tools::toast("Charted %d networks", st);
  }
  if (st < 0 && now - cnLast > 8000) {
    wifiScanBegin();
    cnLast = now;
  }
}
void cnClose() {
  wifiRfLeave();
  cnDetail = -1;
}
void cnDrawDetail(int x, int y, int w, int h) {
  body(x, y, w, h);
  if (cnDetail < 0 || cnDetail >= cnCount) {
    txt(x + 8, y + 8, theme::kInkDim, 1, "No network selected.");
    return;
  }
  const CnNet& n = cnNets[cnDetail];
  char mac[18];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", n.bssid[0],
           n.bssid[1], n.bssid[2], n.bssid[3], n.bssid[4], n.bssid[5]);
  const char* ven = oui::vendor(n.bssid);
  // Detail sheet hero KPIs
  char vRssi[8], vCh[8], vRisk[8];
  snprintf(vRssi, sizeof(vRssi), "%d", (int)n.rssi);
  snprintf(vCh, sizeof(vCh), "%u", (unsigned)n.channel);
  snprintf(vRisk, sizeof(vRisk), "%s", encLabel(n.auth));
  const char* vals[] = {vRssi, vCh, vRisk};
  const char* labs[] = {"dBm", "channel", "auth"};
  uint16_t cols[] = {theme::kTeal, theme::kCyan, riskColor(encRisk(n.auth))};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 4;
  char buf[48];
  snprintf(buf, sizeof(buf), "%s", n.ssid);
  gfxu::drawKVRow(G(), x + 6, below, w - 12, 18, "SSID", buf, theme::kInk);
  snprintf(buf, sizeof(buf), "%s", mac);
  gfxu::drawKVRow(G(), x + 6, below + 22, w - 12, 18, "BSSID", buf, theme::kCyan);
  snprintf(buf, sizeof(buf), "%s", ven[0] ? ven : "unknown");
  gfxu::drawKVRow(G(), x + 6, below + 44, w - 12, 18, "Vendor", buf, theme::kTeal);
  rssiBars(x + w - 36, below + 70, n.rssi);
  btn(x + 8, y + h - 30, 90, 24, "BACK", false);
  btn(x + 110, y + h - 30, 100, 24, "RESCAN", true);
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

  // Field Tablet chip toolbar
  gfxu::drawToolbar(G(), x + 4, below, w - 8, 20);
  const char* sorts[] = {"RSSI", "CH", "SSID"};
  for (int i = 0; i < 3; i++) {
    int bx = x + 8 + i * 40;
    chip(bx, below + 2, 38, theme::kChipH, sorts[i], cnSort == i, theme::kGold);
  }
  const char* filters[] = {"All", "Open", "Sec"};
  for (int i = 0; i < 3; i++) {
    int bx = x + 136 + i * 36;
    chip(bx, below + 2, 34, theme::kChipH, filters[i], cnFilter == i,
         theme::kTeal);
  }

  constexpr int kRh = theme::kRowH;
  const int listTop = below + 22;
  const int listBot = y + h - 12;  // leave hint line
  const int listH = max(0, listBot - listTop);
  const int kRows = max(1, listH / kRh);  // fit remaining Y (KPI must not eat list)

  if (st < 0 && cnCount == 0) {
    txt(x + 12, listTop + 4, theme::kInkDim, 1, "Sweeping the horizon...");
    txt(x + 12, listTop + 16, theme::kInkMuted, 1, "passive scan · no associate");
    txt(x + 8, y + h - 10, theme::kInkMuted, 1, "Tap empty = rescan");
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
    rssiBars(x + 8, ry + 4, n.rssi);
    char ssid[18];
    strncpy(ssid, n.ssid, 17);
    ssid[17] = 0;
    txt(x + 28, ry + 2, theme::kInk, 1, "%s", ssid);
    const char* ven = oui::vendor(n.bssid);
    char sec[28];
    if (ven[0])
      snprintf(sec, sizeof(sec), "%s  ch%u", ven, (unsigned)n.channel);
    else
      snprintf(sec, sizeof(sec), "ch%u", (unsigned)n.channel);
    if (strlen(sec) > 22) sec[22] = 0;
    txt(x + 28, ry + 10, theme::kInkMuted, 1, "%s", sec);
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
  txt(x + 8, y + h - 10, theme::kInkMuted, 1, "Tap row = detail · empty = rescan");
}
void cnDraw(int x, int y, int w, int h) {
  if (cnDetail >= 0) cnDrawDetail(x, y, w, h);
  else cnDrawList(x, y, w, h);
}
bool cnTouch(int16_t x, int16_t y) {
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
      if (cx >= 8 && cx <= 100) {
        cnDetail = -1;
        return true;
      }
      if (cx >= 110 && cx <= 220) {
        cnDetail = -1;
        wifiScanBegin();
        cnLast = millis();
        cnProcessed = -1;
        return true;
      }
    }
    cnDetail = -1;
    return true;
  }
  // Sort / filter chips in toolbar
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
      int bx = 136 + i * 36;
      if (cx >= bx && cx <= bx + 34) {
        cnFilter = i;
        cnScroll = 0;
        cnRebuildVisible();
        return true;
      }
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
void hlOpen() { g_bleLastScan = 0; }
void hlTick(uint32_t now) { bleTick(now); }
void hlClose() { bleStop(); }
void hlDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  int trackers = 0;
  for (int i = 0; i < g_bleCount; i++)
    if (g_ble[i].kind == 1) trackers++;
  char vBottles[8], vTrack[8], vSeen[8];
  snprintf(vBottles, sizeof(vBottles), "%d", g_bleCount);
  snprintf(vTrack, sizeof(vTrack), "%d", trackers);
  snprintf(vSeen, sizeof(vSeen), "%lu", (unsigned long)g_seenBle.size());
  const char* vals[] = {vBottles, vTrack, vSeen};
  const char* labs[] = {"adrift", "trackers", "session"};
  uint16_t cols[] = {theme::kCyan, trackers ? theme::kWarn : theme::kGood,
                     theme::kTeal};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 2;
  txt(x + 8, below, theme::kInkMuted, 1, "passive BLE · Field Tablet");
  below += 12;
  constexpr int kRh = theme::kRowHCompact;
  int rows = min(g_bleCount, 9);
  for (int i = 0; i < rows; i++) {
    int ry = below + i * kRh;
    if (i & 1) G().fillRect(x + 4, ry, w - 8, kRh, theme::kPanelSoft);
    rssiBars(x + 8, ry + 3, g_ble[i].rssi);
    String label = g_ble[i].name.length() ? g_ble[i].name : g_ble[i].mac;
    if (label.length() > 16) label = label.substring(0, 16);
    uint16_t c = g_ble[i].kind == 1 ? theme::kWarn : theme::kInk;
    txt(x + 28, ry + 1, c, 1, "%s", label.c_str());
    const char* ven = oui::vendorStr(g_ble[i].mac);
    char meta[24];
    if (ven[0])
      snprintf(meta, sizeof(meta), "%.7s %ddB", ven, g_ble[i].rssi);
    else
      snprintf(meta, sizeof(meta), "%ddB", g_ble[i].rssi);
    int mw = (int)strlen(meta) * 6;
    txt(x + w - mw - 10, ry + 4, theme::kInkDim, 1, "%s", meta);
    G().drawFastHLine(x + 8, ry + kRh - 1, w - 16, theme::kBorder);
  }
}
bool hlTouch(int16_t, int16_t) { return false; }
}  // namespace

// ===========================================================================
//  3. Chart Room -- WiGLE 1.4 wardrive logger (beacon metadata -> SD)
// ===========================================================================
namespace {
const char* kWigleFile = "/wigle.csv";
uint32_t crLoggedSession = 0;
int crProcessed = -1;
bool crHeader = false;

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
          "WigleWifi-1.4,appRelease=pocketpirate,model=ESP32-S3,release=0.5.3,"
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
  crEnsureHeader();
  wifiScanBegin();
}
void crTick(uint32_t now) {
  (void)now;
  int st = wifiScanState();
  if (st >= 0 && st != crProcessed) {
    crProcessed = st;
    g_lastWifiCount = st;
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
        tools::toast(fix ? "Logged %d + GPS" : "Logged %d (no GPS)", st);
      }
    }
    wifiScanBegin();
  }
}
void crClose() { wifiRfLeave(); }
void crDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  bool fix = gps::hasFix();
  char vRows[10], vGps[8], vSd[8];
  snprintf(vRows, sizeof(vRows), "%lu", (unsigned long)crLoggedSession);
  snprintf(vGps, sizeof(vGps), "%s", gps::statusLabel());
  snprintf(vSd, sizeof(vSd), "%s", app::sdReady() ? "OK" : "--");
  const char* vals[] = {vRows, vGps, vSd};
  const char* labs[] = {"logged", "GPS", "SD"};
  uint16_t cols[] = {theme::kGold, fix ? theme::kGood : theme::kWarn,
                     app::sdReady() ? theme::kGood : theme::kBad};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 6;
  char v[48];
  gfxu::drawKVRow(G(), x + 6, below, w - 12, 18, "WiGLE file", kWigleFile,
                  theme::kCyan);
  if (fix) {
    snprintf(v, sizeof(v), "%.5f, %.5f  %.0fm", gps::latitude(),
             gps::longitude(), gps::altitudeM());
    gfxu::drawKVRow(G(), x + 6, below + 22, w - 12, 18, "Fix", v, theme::kCyan);
  } else {
    gfxu::drawKVRow(G(), x + 6, below + 22, w - 12, 18, "Fix",
                    "lat/lon blank until fix", theme::kInkMuted);
  }
  txt(x + 10, below + 50, theme::kInkMuted, 1, "UART GPS GPIO43 RX / 44 TX");
  txt(x + 10, below + 64, theme::kInkMuted, 1, "Passive survey — no association.");
  txt(x + 10, below + 80, theme::kTeal, 1, "Bridge Console · Chart Room");
}
bool crTouch(int16_t, int16_t) { return false; }
}  // namespace

// ===========================================================================
//  4. Lookout -- channel occupancy analyzer (histogram + detail)
// ===========================================================================
namespace {
int lkHist[14] = {0};
int lkOpenHist[14] = {0};
int lkProcessed = -1;
int lkSelected = 0;  // 0 = none, 1..13 = channel detail
int lkTotal = 0;

void lkOpen() {
  lkProcessed = -1;
  lkSelected = 0;
  wifiScanBegin();
}
void lkTick(uint32_t now) {
  (void)now;
  int st = wifiScanState();
  if (st >= 0 && st != lkProcessed) {
    lkProcessed = st;
    lkTotal = st;
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
        if (g_rfNets[i].auth == WIFI_AUTH_OPEN) lkOpenHist[c]++;
      }
    }
    wifiScanBegin();
  }
}
void lkClose() { wifiRfLeave(); }
void lkDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  int maxv = 1, openSum = 0, peakCh = 1;
  for (int c = 1; c <= 13; c++) {
    if (lkHist[c] > maxv) { maxv = lkHist[c]; peakCh = c; }
    openSum += lkOpenHist[c];
  }
  char vTot[8], vPeak[8], vOpen[8];
  snprintf(vTot, sizeof(vTot), "%d", lkTotal);
  snprintf(vPeak, sizeof(vPeak), "%d", peakCh);
  snprintf(vOpen, sizeof(vOpen), "%d", openSum);
  const char* vals[] = {vTot, vPeak, vOpen};
  const char* labs[] = {"APs", "peak CH", "open"};
  uint16_t cols[] = {theme::kTeal, theme::kGold, openSum ? theme::kWarn
                                                          : theme::kGood};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 2;
  int baseY = y + h - 36;
  int plotH = baseY - below - 14;
  if (plotH < 40) plotH = 40;
  int bw = 18, gap = 3;
  gfxu::drawElevated(G(), x + 4, below, w - 8, plotH + 16);
  for (int c = 1; c <= 13; c++) {
    int bx = x + 12 + (c - 1) * (bw + gap);
    int bh = maxv ? (lkHist[c] * plotH) / maxv : 0;
    bool sel = (lkSelected == c);
    uint16_t col = sel ? theme::kGold
                       : (lkHist[c] >= maxv && maxv > 1 ? theme::kWarn
                                                        : theme::kTeal);
    if (bh > 0) G().fillRoundRect(bx, baseY - bh, bw, bh, 2, col);
    if (lkOpenHist[c] > 0 && bh > 0) {
      int oh = max(2, (lkOpenHist[c] * plotH) / maxv);
      if (oh > bh) oh = bh;
      G().fillRoundRect(bx, baseY - oh, bw, oh, 2, theme::kBad);
    }
    if (sel)
      G().drawRoundRect(bx - 1, baseY - max(bh, 4) - 1, bw + 2, max(bh, 4) + 2,
                        2, theme::kInk);
    txt(bx + (c >= 10 ? 1 : 5), baseY + 3, theme::kInkDim, 1, "%d", c);
    if (lkHist[c])
      txt(bx + 2, baseY - bh - 9, theme::kInk, 1, "%d", lkHist[c]);
  }
  G().fillRoundRect(x + 4, y + h - 20, w - 8, 16, 3, theme::kBgDeep);
  G().fillRoundRect(x + 10, y + h - 15, 7, 7, 1, theme::kTeal);
  txt(x + 20, y + h - 14, theme::kInkDim, 1, "sec");
  G().fillRoundRect(x + 48, y + h - 15, 7, 7, 1, theme::kBad);
  txt(x + 58, y + h - 14, theme::kInkDim, 1, "open");
  if (lkSelected >= 1 && lkSelected <= 13)
    txt(x + 100, y + h - 14, theme::kGold, 1, "CH%d: %d (%d open)", lkSelected,
        lkHist[lkSelected], lkOpenHist[lkSelected]);
  else
    txt(x + 100, y + h - 14, theme::kInkMuted, 1, "tap a bar");
}
bool lkTouch(int16_t x, int16_t y) {
  int bw = 18, gap = 3;
  // Absolute coords; bars sit under KPI strip (contentTop + ~40).
  for (int c = 1; c <= 13; c++) {
    int bx = 10 + (c - 1) * (bw + gap);
    if (x >= bx && x <= bx + bw && y >= 70 && y <= 220) {
      lkSelected = (lkSelected == c) ? 0 : c;
      return true;
    }
  }
  wifiScanBegin();
  lkProcessed = -1;
  return true;
}
}  // namespace

// ===========================================================================
//  5. Spyglass -- surveillance-camera spotter (detection only)
// ===========================================================================
namespace {
int sgProcessed = -1;
struct Hit { String ssid; String bssid; String label; int rssi; };
Hit sgHits[12];
int sgHitCount = 0;

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
void sgOpen() {
  sgProcessed = -1;
  sgHitCount = 0;
  wifiScanBegin();
}
void sgTick(uint32_t now) {
  (void)now;
  int st = wifiScanState();
  if (st >= 0 && st != sgProcessed) {
    sgProcessed = st;
    sgHitCount = 0;
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
        if (ouiMatch(bssid, sig.ouiPrefix) || ssidMatch(ssid, sig.ssidSubstr)) {
          Hit& hh = sgHits[sgHitCount++];
          hh.ssid = ssid.length() ? ssid : String("<hidden>");
          hh.bssid = bssid;
          hh.label = sig.label;
          hh.rssi = net.rssi;
          break;
        }
      }
        }
    wifiScanBegin();
  }
}
void sgClose() { wifiRfLeave(); }
void sgDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  bool armed = spyglass::hasVerifiedSignature();
  char vHits[8], vMode[8], vSig[8];
  snprintf(vHits, sizeof(vHits), "%d", sgHitCount);
  snprintf(vMode, sizeof(vMode), "%s", armed ? "OUI" : "KW");
  snprintf(vSig, sizeof(vSig), "%d", spyglass::kSignatureCount);
  const char* vals[] = {vHits, vMode, vSig};
  const char* labs[] = {"hits", "mode", "sigs"};
  uint16_t cols[] = {sgHitCount ? theme::kBad : theme::kGood,
                     armed ? theme::kGood : theme::kWarn, theme::kTeal};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 2;
  txt(x + 8, below, theme::kInkMuted, 1,
      armed ? "DeFlock OUIs armed · OUI=strong" : "Keyword heuristics only");
  below += 12;
  constexpr int kRh = theme::kRowH;
  int rows = min(sgHitCount, 6);
  for (int i = 0; i < rows; i++) {
    int ry = below + i * kRh;
    if (i & 1) G().fillRect(x + 4, ry, w - 8, kRh, theme::kPanelSoft);
    String lab = sgHits[i].label;
    if (lab.length() > 28) lab = lab.substring(0, 28);
    txt(x + 10, ry + 1, theme::kBad, 1, "%s", lab.c_str());
    char sec[42];
    snprintf(sec, sizeof(sec), "%s  %ddB", sgHits[i].bssid.c_str(),
             sgHits[i].rssi);
    txt(x + 10, ry + 10, theme::kInkMuted, 1, "%s", sec);
    G().drawFastHLine(x + 8, ry + kRh - 1, w - 16, theme::kBorder);
  }
}
bool sgTouch(int16_t, int16_t) { return false; }
}  // namespace

// ===========================================================================
//  6. Tracker Watch -- nearby BLE item-tracker spotter (anti-stalking)
// ===========================================================================
namespace {
void twOpen() { g_bleLastScan = 0; }
void twTick(uint32_t now) { bleTick(now); }
void twClose() { bleStop(); }
void twDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  int n = 0;
  for (int i = 0; i < g_bleCount; i++)
    if (g_ble[i].kind == 1) n++;
  char vTrack[8], vBle[8], vStat[8];
  snprintf(vTrack, sizeof(vTrack), "%d", n);
  snprintf(vBle, sizeof(vBle), "%d", g_bleCount);
  snprintf(vStat, sizeof(vStat), "%s", n ? "ALERT" : "CLEAR");
  const char* vals[] = {vTrack, vBle, vStat};
  const char* labs[] = {"trackers", "BLE", "status"};
  uint16_t cols[] = {n ? theme::kWarn : theme::kGood, theme::kCyan,
                     n ? theme::kWarn : theme::kGood};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 2;
  txt(x + 8, below, theme::kInkMuted, 1, "AirTag / Tile / SmartTag · detect only");
  below += 12;
  int shown = 0;
  constexpr int kRh = theme::kRowH;
  for (int i = 0; i < g_bleCount && shown < 6; i++) {
    if (g_ble[i].kind != 1) continue;
    int ry = below + shown * kRh;
    if (shown & 1) G().fillRect(x + 4, ry, w - 8, kRh, theme::kPanelSoft);
    rssiBars(x + 8, ry + 4, g_ble[i].rssi);
    String label = g_ble[i].name.length() ? g_ble[i].name : g_ble[i].mac;
    if (label.length() > 22) label = label.substring(0, 22);
    txt(x + 28, ry + 5, theme::kWarn, 1, "%s", label.c_str());
    char meta[12];
    snprintf(meta, sizeof(meta), "%ddB", g_ble[i].rssi);
    txt(x + w - 40, ry + 5, theme::kInkDim, 1, "%s", meta);
    G().drawFastHLine(x + 8, ry + kRh - 1, w - 16, theme::kBorder);
    shown++;
  }
  if (n == 0)
    txt(x + 12, below + 8, theme::kInkMuted, 1, "All clear — no trackers seen.");
}
bool twTouch(int16_t, int16_t) { return false; }
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

void rwPromiscCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  auto* p = (wifi_promiscuous_pkt_t*)buf;
  const uint8_t* pl = p->payload;
  uint8_t subtype = (pl[0] >> 4) & 0x0F;
  // Inspect only management-frame subtype + transmitter address. No payload,
  // no data frames, nothing stored beyond counters -- this is an IDS, not a
  // capture tool.
  if (subtype == 12 || subtype == 10) {
    if (subtype == 12) rwDeauth++;
    else rwDisassoc++;
    snprintf(rwLastSrc, sizeof(rwLastSrc), "%02X:%02X:%02X:%02X:%02X:%02X",
             pl[10], pl[11], pl[12], pl[13], pl[14], pl[15]);
    rwLastRssi = p->rx_ctrl.rssi;
    rwLastHit = millis();
  }
}
void rwOpen() {
  rwDeauth = rwDisassoc = 0;
  rwChannel = 1;
  wifiPromiscBegin(&rwPromiscCb, rwChannel);
}
void rwTick(uint32_t now) {
  if (!g_rfPromiscReady) return;
  if (now - rwHopAt > 300) {  // sweep channels so we cover the band
    rwHopAt = now;
    rwChannel = rwChannel >= 13 ? 1 : rwChannel + 1;
    esp_wifi_set_channel(rwChannel, WIFI_SECOND_CHAN_NONE);
  }
}
void rwClose() { wifiRfLeave(); }
void rwDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  bool recent = (millis() - rwLastHit) < 4000 && (rwDeauth + rwDisassoc) > 0;
  char vDe[10], vDi[10], vCh[8];
  snprintf(vDe, sizeof(vDe), "%lu", (unsigned long)rwDeauth);
  snprintf(vDi, sizeof(vDi), "%lu", (unsigned long)rwDisassoc);
  snprintf(vCh, sizeof(vCh), "%d", rwChannel);
  const char* vals[] = {vDe, vDi, vCh};
  const char* labs[] = {"deauth", "disassoc", "CH hop"};
  uint16_t cols[] = {rwDeauth ? theme::kBad : theme::kGood,
                     rwDisassoc ? theme::kWarn : theme::kGood, theme::kCyan};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 6;
  char v[24];
  gfxu::drawKVRow(G(), x + 6, below, w - 12, 20, "Last source", rwLastSrc,
                  theme::kCyan);
  snprintf(v, sizeof(v), "%d dB", rwLastRssi);
  gfxu::drawKVRow(G(), x + 6, below + 24, w - 12, 20, "Last RSSI", v);
  if (recent) {
    G().fillRoundRect(x + 6, below + 52, w - 12, 28, 5, theme::kBad);
    G().setTextColor(theme::kInk);
    G().setTextSize(1);
    G().setCursor(x + 14, below + 62);
    G().print("ALERT: attack frames nearby!");
  } else {
    txt(x + 12, below + 58, theme::kGood, 1, "Calm seas — no attack detected.");
  }
  txt(x + 12, below + 82, theme::kInkMuted, 1, "IDS only · passive MGMT listen");
}
bool rwTouch(int16_t, int16_t) { return false; }
}  // namespace

// ===========================================================================
//  8. Hull Inspection -- security posture audit of nearby networks
// ===========================================================================
namespace {
int hiProcessed = -1;
int hiOpen_ = 0, hiWeak = 0, hiStrong = 0, hiTotal = 0;
struct HiRow { char ssid[20]; wifi_auth_mode_t auth; };
HiRow hiRows[16];
void hiOpen() {
  hiProcessed = -1;
  wifiScanBegin();
}
void hiTick(uint32_t now) {
  (void)now;
  int st = wifiScanState();
  if (st >= 0 && st != hiProcessed) {
    hiProcessed = st;
    hiOpen_ = hiWeak = hiStrong = 0;
    int n = g_rfNetCount;
    if (n > st) n = st;
    if (n > 16) n = 16;
    hiTotal = n;
    for (int i = 0; i < n; i++) {
      hiRows[i].auth = g_rfNets[i].auth;
      strncpy(hiRows[i].ssid, g_rfNets[i].ssid, sizeof(hiRows[i].ssid) - 1);
      hiRows[i].ssid[sizeof(hiRows[i].ssid) - 1] = 0;
      int r = encRisk(g_rfNets[i].auth);
      if (r == 3) hiOpen_++;
      else if (r == 2) hiWeak++;
      else if (r == 0) hiStrong++;
    }
  }
}
void hiClose() { wifiRfLeave(); }
void hiDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  char vOpen[8], vWeak[8], vStrong[8], vTot[8];
  snprintf(vOpen, sizeof(vOpen), "%d", hiOpen_);
  snprintf(vWeak, sizeof(vWeak), "%d", hiWeak);
  snprintf(vStrong, sizeof(vStrong), "%d", hiStrong);
  snprintf(vTot, sizeof(vTot), "%d", hiTotal);
  const char* vals[] = {vOpen, vWeak, vStrong, vTot};
  const char* labs[] = {"risky", "aging", "WPA3", "total"};
  uint16_t cols[] = {hiOpen_ ? theme::kBad : theme::kGood,
                     hiWeak ? theme::kWarn : theme::kInkDim, theme::kGood,
                     theme::kTeal};
  int below = kpiStrip(x + 4, y + 2, w - 8, 4, vals, labs, cols) + 4;
  txt(x + 8, below, theme::kInkMuted, 1, "Prefer WPA2/3 · disable WPS · PMF");
  below += 12;
  constexpr int kRh = theme::kRowHCompact;
  int rows = min(hiTotal, 7);
  for (int i = 0; i < rows; i++) {
    int ry = below + i * kRh;
    if (i & 1) G().fillRect(x + 4, ry, w - 8, kRh, theme::kPanelSoft);
    wifi_auth_mode_t m = hiRows[i].auth;
    char ssid[20];
    strncpy(ssid, hiRows[i].ssid, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = 0;
    if (strlen(ssid) > 18) ssid[18] = 0;
    txt(x + 10, ry + 4, theme::kInk, 1, "%s", ssid);
    txt(x + 230, ry + 4, riskColor(encRisk(m)), 1, "%s", encLabel(m));
    G().drawFastHLine(x + 8, ry + kRh - 1, w - 16, theme::kBorder);
  }
}
bool hiTouch(int16_t, int16_t) { return false; }
}  // namespace

// ===========================================================================
//  9. Captain's Log -- microSD browser + WiGLE / text preview
// ===========================================================================
namespace {
String clFiles[32];
long clSizes[32];
int clCount = 0;
int clScroll = 0;
int clMode = 0;  // 0=list, 1=preview
String clOpenName;
String clLines[14];
int clLineCount = 0;
int clPreviewScroll = 0;
bool clIsWigle = false;
int clWigleRows = 0;
int clWigleUnique = 0;

void clRefresh() {
  clCount = 0;
  clScroll = 0;
  if (!app::sdReady()) return;
  File dir = SD_MMC.open("/");
  if (!dir) return;
  File f = dir.openNextFile();
  while (f && clCount < 32) {
    String n = String(f.name());
    int slash = n.lastIndexOf('/');
    if (slash >= 0) n = n.substring(slash + 1);
    if (f.isDirectory()) {
      clFiles[clCount] = String("[") + n + "]";
      clSizes[clCount] = -1;
    } else {
      clFiles[clCount] = n;
      clSizes[clCount] = (long)f.size();
    }
    clCount++;
    f = dir.openNextFile();
  }
  dir.close();
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
  clWigleRows = 0;
  clWigleUnique = 0;
  for (int i = 0; i < 14; i++) clLines[i] = "";
  if (!app::sdReady()) return;
  String path = String("/") + name;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) {
    clLines[0] = "(could not open)";
    clLineCount = 1;
    return;
  }
  String lower = name;
  lower.toLowerCase();
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
      int rows = min(clLineCount, 10);
      for (int i = 0; i < rows; i++) {
        int ry = y + 54 + i * 12;
        txt(x + 8, ry, theme::kInk, 1, "%s", clLines[i].c_str());
      }
      if (clWigleRows == 0)
        txt(x + 8, y + 54, theme::kInkDim, 1, "(empty log — sail Chart Room)");
    } else {
      txt(x + 8, y + 28, theme::kInkDim, 1, "Preview (first lines):");
      int rows = min(clLineCount, 12);
      for (int i = 0; i < rows; i++) {
        int ry = y + 42 + i * 12;
        txt(x + 8, ry, theme::kInk, 1, "%s", clLines[i].c_str());
      }
      if (clLineCount == 0)
        txt(x + 8, y + 42, theme::kInkDim, 1, "(empty or binary file)");
    }
    return;
  }

  // List pane — KPI then Field Tablet rows
  char vItems[8], vSd[8], vMode[8];
  snprintf(vItems, sizeof(vItems), "%d", clCount);
  snprintf(vSd, sizeof(vSd), "%s", app::sdReady() ? "OK" : "--");
  snprintf(vMode, sizeof(vMode), "LOG");
  const char* vals[] = {vItems, vSd, vMode};
  const char* labs[] = {"items", "SD", "hold"};
  uint16_t cols[] = {theme::kTeal, theme::kGood, theme::kGold};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 2;
  txt(x + 8, below, theme::kInkMuted, 1, "Tap row = preview · empty = refresh");
  below += 12;
  const int visible = 6;
  constexpr int kRh = theme::kRowH;
  if (clScroll > clCount - visible) clScroll = max(0, clCount - visible);
  if (clScroll < 0) clScroll = 0;
  int rows = min(visible, clCount - clScroll);
  for (int i = 0; i < rows; i++) {
    int idx = clScroll + i;
    int ry = below + i * kRh;
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
    btn(x + 280, below, 28, 20, "^", false);
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
    return true;
  }
  // Scroll buttons — list starts after KPI(~36)+hint(~14) ≈ 50
  const int listY = theme::kKpiH + 16;
  if (clCount > 6 && x >= 270) {
    if (ly >= listY && ly <= listY + 40) {
      clScroll = max(0, clScroll - 3);
      return true;
    }
    if (ly >= 150) {
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
  // Empty tap refreshes
  clRefresh();
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
  int pct = power::batteryPct();
  char vHeap[10], vBat[10], vGps[8];
  snprintf(vHeap, sizeof(vHeap), "%u", (unsigned)(ESP.getFreeHeap() / 1024));
  if (power::usbPowered())
    snprintf(vBat, sizeof(vBat), "%s", power::powerLabel());
  else
    snprintf(vBat, sizeof(vBat), "%d%%", pct);
  snprintf(vGps, sizeof(vGps), "%s", gps::statusLabel());
  const char* vals[] = {vHeap, vBat, vGps};
  const char* labs[] = {"heap KB", "power", "GPS"};
  uint16_t cols[] = {theme::kTeal,
                     power::lowBattery() ? theme::kBad : theme::kGold,
                     gps::hasFix() ? theme::kGood : theme::kInkDim};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 4;
  char v[40];
  snprintf(v, sizeof(v), "%s x%d @ %dMHz", ESP.getChipModel(),
           ESP.getChipCores(), getCpuFrequencyMhz());
  gfxu::drawKVRow(G(), x + 6, below, w - 12, 16, "Chip", v, theme::kCyan);
  snprintf(v, sizeof(v), "%u / %u KB", (unsigned)(ESP.getFreePsram() / 1024),
           (unsigned)(ESP.getPsramSize() / 1024));
  gfxu::drawKVRow(G(), x + 6, below + 18, w - 12, 16, "PSRAM", v, theme::kTeal);
  snprintf(v, sizeof(v), "%u MB",
           (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
  gfxu::drawKVRow(G(), x + 6, below + 36, w - 12, 16, "Flash", v);
  gfxu::drawKVRow(G(), x + 6, below + 54, w - 12, 16, "SD",
                  app::sdReady() ? "mounted" : "absent",
                  app::sdReady() ? theme::kGood : theme::kBad);
  uint32_t up = millis() / 1000;
  snprintf(v, sizeof(v), "%lu:%02lu:%02lu", (unsigned long)(up / 3600),
           (unsigned long)((up % 3600) / 60), (unsigned long)(up % 60));
  gfxu::drawKVRow(G(), x + 6, below + 72, w - 12, 16, "Uptime", v);
  snprintf(v, sizeof(v), "%s  %lumV", power::powerLabel(),
           (unsigned long)power::batteryMv());
  gfxu::drawKVRow(G(), x + 6, below + 90, w - 12, 16, "Power", v, theme::kGold);
  G().drawRoundRect(x + 6, below + 112, 104, 10, 3, theme::kBorder);
  G().fillRoundRect(x + 8, below + 114, max(1, pct), 6, 2,
                    power::lowBattery() ? theme::kBad : theme::kGood);
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
  if (strlen(vMode) > 6) vMode[6] = 0;
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

  txt(x + 8, below + 36, theme::kInkDim, 1, "Color:");
  for (int i = 0; i < led::kColorCount; i++) {
    uint32_t c = led::colorRgb(i);
    uint16_t col565 = G().color565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    int sx = x + 52 + i * 32;
    G().fillRoundRect(sx, below + 30, 26, 24, 4, col565);
    if (i == led::colorIdx())
      G().drawRoundRect(sx - 2, below + 28, 30, 28, 5, theme::kGold);
  }

  txt(x + 8, below + 68, theme::kInk, 1, "Glow: %d%%", led::brightness());
  btn(x + 150, below + 62, 30, 22, "-", false);
  btn(x + 186, below + 62, 30, 22, "+", false);

  btn(x + 8, below + 92, 150, 26, app::sound() ? "Sound: ON" : "Sound: off",
      false, app::sound() ? theme::kGood : theme::kLocked);

  txt(x + 8, below + 128, theme::kInkMuted, 1,
      "Profile persists across every station.");
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
      led::setBrightness(led::brightness() - 10);
      return true;
    }
    if (x >= 183 && x <= 222) {
      led::setBrightness(led::brightness() + 10);
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
//  12. Settings Cabin
// ===========================================================================
namespace {
bool seOtaBusy = false;

void seOpen() { seOtaBusy = false; }
void seTick(uint32_t) {}
void seClose() {}
void seDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  char vLv[8], vFw[8], vBri[8];
  snprintf(vLv, sizeof(vLv), "%d", game::profile.level);
  snprintf(vFw, sizeof(vFw), "%s", PP_VERSION);
  snprintf(vBri, sizeof(vBri), "%d", app::brightness());
  const char* vals[] = {vLv, vFw, vBri};
  const char* labs[] = {"level", "fw", "bri"};
  uint16_t cols[] = {theme::kGold, theme::kTeal, theme::kCyan};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 2;
  txt(x + 8, below, theme::kInkDim, 1, "%s · %s", game::profile.name,
      game::rankTitle(game::profile.level));
  below += 12;

  btn(x + 8, below, 118, 24, "Rename…", false);
  char bri[16];
  snprintf(bri, sizeof(bri), "Bri %d", app::brightness());
  txt(x + 134, below + 8, theme::kInkDim, 1, "%s", bri);
  btn(x + 232, below, 28, 24, "-", false);
  btn(x + 264, below, 28, 24, "+", false);
  below += 28;

  btn(x + 8, below, 120, 24, app::sound() ? "Sound: ON" : "Sound: off", false,
      app::sound() ? theme::kGood : theme::kLocked);
  btn(x + 136, below, 160, 24,
      power::idleSleep() ? "Idle sleep: ON" : "Idle sleep: off", false,
      power::idleSleep() ? theme::kWarn : theme::kPanelHi);
  below += 28;

  btn(x + 8, below, 140, 24, "Sleep now", false, theme::kTeal);
  bool otaOk = ota::wifiConfigured() && ota::urlConfigured();
  btn(x + 156, below, 140, 24, seOtaBusy ? "OTA…" : "OTA Update", otaOk,
      otaOk ? 0 : theme::kLocked);
  below += 28;

  gfxu::drawElevated(G(), x + 4, below, w - 8, 26);
  txt(x + 10, below + 4, theme::kInkMuted, 1, "OTA: %s", ota::status());
  if (ota::wifiConfigured())
    txt(x + 10, below + 14, theme::kInkMuted, 1, "WiFi:%s  URL:%s",
        ota::wifiSsid(), ota::urlConfigured() ? "set" : "none");
  else
    txt(x + 10, below + 14, theme::kInkMuted, 1,
        "Set WiFi+URL via companion WIFICFG/OTAURL");
  below += 30;

  btn(x + 8, below, 170, 22, "Reset progress", false, theme::kBad);
}

bool seTouch(int16_t x, int16_t y) {
  int ly = y - contentTop();
  const int row0 = theme::kKpiH + 14;  // rename / bri
  const int row1 = row0 + 28;
  const int row2 = row1 + 28;
  const int row3 = row2 + 28 + 30;  // after OTA status block
  if (ly >= row0 && ly <= row0 + 28) {
    if (x >= 8 && x <= 165) {
      app::requestRename();
      return false;
    }
    if (x >= 226 && x <= 262) {
      app::setBrightness(app::brightness() - 10);
      return true;
    }
    if (x >= 260 && x <= 300) {
      app::setBrightness(app::brightness() + 10);
      return true;
    }
  }
  if (ly >= row1 && ly <= row1 + 28) {
    if (x >= 8 && x <= 132) {
      app::setSound(!app::sound());
      return true;
    }
    if (x >= 136 && x <= 300) {
      power::setIdleSleep(!power::idleSleep());
      tools::toast(power::idleSleep() ? "Idle sleep ON (3m)" : "Idle sleep off");
      return true;
    }
  }
  if (ly >= row2 && ly <= row2 + 28) {
    if (x >= 8 && x <= 152) {
      game::save();
      tools::toast("Sleeping — tap screen to wake");
      delay(500);
      power::deepSleepNow();
      return true;
    }
    if (x >= 156 && x <= 300) {
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
  }
  if (ly >= row3 && ly <= row3 + 28 && x >= 8 && x <= 190) {
    game::resetProgress();
    tools::toast("Progress reset");
    return true;
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
  char ssid[26];
  int8_t rssi;
  uint16_t hits;
};
Probe pwList[16];
int pwCount = 0;
int pwRewarded = 0;
uint32_t pwTotal = 0;
int pwChannel = 1;
uint32_t pwHopAt = 0;

// Runs in the Wi-Fi task: parse only the transmitter address + requested SSID
// from a broadcast probe-request management frame. No payload/data frames are
// touched -- this observes devices openly searching for known networks.
void pwPromiscCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  auto* p = (wifi_promiscuous_pkt_t*)buf;
  const uint8_t* pl = p->payload;
  if (((pl[0] >> 4) & 0x0F) != 4) return;  // probe request subtype only
  pwTotal++;
  int len = p->rx_ctrl.sig_len;
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
  char mac[18];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", pl[10], pl[11],
           pl[12], pl[13], pl[14], pl[15]);
  for (int i = 0; i < pwCount; i++) {
    if (strcmp(pwList[i].mac, mac) == 0 && strcmp(pwList[i].ssid, ssid) == 0) {
      pwList[i].rssi = p->rx_ctrl.rssi;
      if (pwList[i].hits < 65535) pwList[i].hits++;
      return;
    }
  }
  int idx = pwCount < 16 ? pwCount++ : (int)(pwTotal % 16);
  strncpy(pwList[idx].mac, mac, sizeof(pwList[idx].mac) - 1);
  pwList[idx].mac[sizeof(pwList[idx].mac) - 1] = 0;
  strncpy(pwList[idx].ssid, ssid, sizeof(pwList[idx].ssid) - 1);
  pwList[idx].ssid[sizeof(pwList[idx].ssid) - 1] = 0;
  pwList[idx].rssi = p->rx_ctrl.rssi;
  pwList[idx].hits = 1;
}
void pwOpen() {
  pwCount = 0;
  pwRewarded = 0;
  pwTotal = 0;
  pwChannel = 1;
  wifiPromiscBegin(&pwPromiscCb, pwChannel);
}
void pwTick(uint32_t now) {
  if (g_rfPromiscReady && now - pwHopAt > 350) {
    pwHopAt = now;
    pwChannel = pwChannel >= 13 ? 1 : pwChannel + 1;
    esp_wifi_set_channel(pwChannel, WIFI_SECOND_CHAN_NONE);
  }
  while (pwRewarded < pwCount) {  // xp for each new unique client seen
    pwRewarded++;
    game::awardXp(1);
  }
}
void pwClose() { wifiRfLeave(); }
void pwDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  char vCli[8], vCh[8], vFrm[10];
  snprintf(vCli, sizeof(vCli), "%d", pwCount);
  snprintf(vCh, sizeof(vCh), "%d", pwChannel);
  snprintf(vFrm, sizeof(vFrm), "%lu", (unsigned long)pwTotal);
  const char* vals[] = {vCli, vCh, vFrm};
  const char* labs[] = {"clients", "CH hop", "frames"};
  uint16_t cols[] = {theme::kTeal, theme::kCyan, theme::kGold};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 2;
  txt(x + 8, below, theme::kInkMuted, 1, "passive probe-request sniff");
  below += 12;
  constexpr int kRh = theme::kRowHCompact;
  int rows = min(pwCount, 8);
  for (int i = 0; i < rows; i++) {
    int ry = below + i * kRh;
    if (i & 1) G().fillRect(x + 4, ry, w - 8, kRh, theme::kPanelSoft);
    rssiBars(x + 8, ry + 3, pwList[i].rssi);
    if (pwList[i].ssid[0]) {
      String s = String(pwList[i].ssid);
      if (s.length() > 14) s = s.substring(0, 14);
      txt(x + 28, ry + 4, theme::kInk, 1, "%s", s.c_str());
    } else {
      txt(x + 28, ry + 4, theme::kInkDim, 1, "<broadcast>");
    }
    const char* ven = oui::vendorStr(String(pwList[i].mac));
    txt(x + 150, ry + 4, theme::kTeal, 1, "%.8s",
        ven[0] ? ven : pwList[i].mac + 9);
    txt(x + 250, ry + 4, theme::kInkDim, 1, "x%u", pwList[i].hits);
    G().drawFastHLine(x + 8, ry + kRh - 1, w - 16, theme::kBorder);
  }
  if (pwCount == 0)
    txt(x + 12, below + 8, theme::kInkMuted, 1, "Listening… hopping channels.");
}
bool pwTouch(int16_t, int16_t) { return false; }
}  // namespace

// ===========================================================================
//  14. Deep BLE ID -- advertisement decoder (company / iBeacon / Eddystone)
// ===========================================================================
namespace {
void dbOpen() { g_bleLastScan = 0; }
void dbTick(uint32_t now) { bleTick(now); }
void dbClose() { bleStop(); }
void dbDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  int named = 0, decoded = 0;
  for (int i = 0; i < g_bleCount; i++) {
    if (g_ble[i].name.length()) named++;
    if (g_ble[i].detail[0] || g_ble[i].company) decoded++;
  }
  char vAdv[8], vDec[8], vNam[8];
  snprintf(vAdv, sizeof(vAdv), "%d", g_bleCount);
  snprintf(vDec, sizeof(vDec), "%d", decoded);
  snprintf(vNam, sizeof(vNam), "%d", named);
  const char* vals[] = {vAdv, vDec, vNam};
  const char* labs[] = {"adverts", "decoded", "named"};
  uint16_t cols[] = {theme::kCyan, theme::kTeal, theme::kGold};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 2;
  constexpr int kRh = theme::kRowH;
  int rows = min(g_bleCount, 7);
  for (int i = 0; i < rows; i++) {
    int ry = below + i * kRh;
    if (i & 1) G().fillRect(x + 4, ry, w - 8, kRh, theme::kPanelSoft);
    String label = g_ble[i].name.length() ? g_ble[i].name : g_ble[i].mac;
    if (label.length() > 20) label = label.substring(0, 20);
    txt(x + 10, ry + 1, theme::kInk, 1, "%s", label.c_str());
    txt(x + w - 40, ry + 1, theme::kInkDim, 1, "%ddB", g_ble[i].rssi);
    const char* co = bleCompanyName(g_ble[i].company);
    if (g_ble[i].detail[0])
      txt(x + 16, ry + 10, theme::kTeal, 1, "%s", g_ble[i].detail);
    else if (co[0])
      txt(x + 16, ry + 10, theme::kTeal, 1, "%s", co);
    else if (g_ble[i].company)
      txt(x + 16, ry + 10, theme::kInkMuted, 1, "company 0x%04X",
          g_ble[i].company);
    else
      txt(x + 16, ry + 10, theme::kInkMuted, 1, "no manufacturer data");
    G().drawFastHLine(x + 8, ry + kRh - 1, w - 16, theme::kBorder);
  }
  if (g_bleCount == 0)
    txt(x + 12, below + 8, theme::kInkMuted, 1, "Sampling the air...");
}
bool dbTouch(int16_t, int16_t) { return false; }
}  // namespace

// ===========================================================================
//  15. Ship's Instruments -- onboard sensors
// ===========================================================================
namespace {
float siTemp = 0;
uint32_t siLast = 0;
void siOpen() { siLast = 0; }
void siTick(uint32_t now) {
  if (now - siLast > 1000) {
    siLast = now;
    siTemp = temperatureRead();  // on-die core temperature sensor
  }
}
void siClose() {}
void siDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  int pct = power::batteryPct();
  bool fix = gps::hasFix();
  char vTemp[10], vBat[10], vGps[8];
  snprintf(vTemp, sizeof(vTemp), "%.0fC", siTemp);
  if (power::usbPowered())
    snprintf(vBat, sizeof(vBat), "%s", power::powerLabel());
  else
    snprintf(vBat, sizeof(vBat), "%d%%", pct);
  snprintf(vGps, sizeof(vGps), "%s", gps::statusLabel());
  const char* vals[] = {vTemp, vBat, vGps};
  const char* labs[] = {"core", "power", "GPS"};
  uint16_t cols[] = {theme::kGold,
                     power::lowBattery() ? theme::kBad : theme::kTeal,
                     fix ? theme::kGood : theme::kWarn};
  int below = kpiStrip(x + 4, y + 2, w - 8, 3, vals, labs, cols) + 4;
  char v[40];
  snprintf(v, sizeof(v), "%.1f C / %.0f F", siTemp,
           siTemp * 9.0f / 5.0f + 32.0f);
  gfxu::drawKVRow(G(), x + 6, below, w - 12, 18, "Core temp", v, theme::kGold);
  snprintf(v, sizeof(v), "%s  %lumV", power::powerLabel(),
           (unsigned long)power::batteryMv());
  gfxu::drawKVRow(G(), x + 6, below + 22, w - 12, 18, "Power", v, theme::kGold);
  G().drawRoundRect(x + 6, below + 46, 104, 10, 3, theme::kBorder);
  G().fillRoundRect(x + 8, below + 48, max(1, pct), 6, 2,
                    power::lowBattery() ? theme::kBad : theme::kGood);
  gfxu::drawKVRow(G(), x + 6, below + 64, w - 12, 18, "GPS", gps::statusLabel(),
                  fix ? theme::kGood : theme::kWarn);
  if (fix) {
    snprintf(v, sizeof(v), "%.5f, %.5f", gps::latitude(), gps::longitude());
    gfxu::drawKVRow(G(), x + 6, below + 86, w - 12, 18, "Lat/Lon", v,
                    theme::kCyan);
    snprintf(v, sizeof(v), "%.0fm  sats %lu  hdop %.1f", gps::altitudeM(),
             (unsigned long)gps::satellites(), gps::hdop());
    gfxu::drawKVRow(G(), x + 6, below + 108, w - 12, 18, "Alt/HD", v);
  } else {
    gfxu::drawKVRow(G(), x + 6, below + 86, w - 12, 18, "Wiring",
                    "TX->GPIO43 RX->GPIO44", theme::kInkMuted);
  }
  snprintf(v, sizeof(v), "%d%% / %s", app::brightness(),
           app::sdReady() ? "ok" : "no");
  gfxu::drawKVRow(G(), x + 6, below + 130, w - 12, 18, "Bri / SD", v);
  txt(x + 10, below + 156, theme::kInkMuted, 1, "Core temp is on-die (reads warm).");
}
bool siTouch(int16_t, int16_t) { return false; }
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
