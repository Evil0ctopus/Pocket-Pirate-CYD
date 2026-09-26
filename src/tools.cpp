#include "tools.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <BLEDevice.h>
#include <SD_MMC.h>

#include <set>
#include <strings.h>

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
#define PP_VERSION "0.4.2"
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

// ---- shared Wi-Fi scan ----------------------------------------------------
void wifiScanBegin() {
  // Keep this cheap: callers paint station chrome first, then open.
  // Avoid disconnect() on every enter — it stalls the UI thread ~50-150ms.
  if (WiFi.getMode() != WIFI_STA) WiFi.mode(WIFI_STA);
  WiFi.scanDelete();
  WiFi.scanNetworks(true /*async*/, true /*show hidden*/);
}
int wifiScanState() { return WiFi.scanComplete(); }  // -1 run, -2 fail, >=0 n

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

// Short, periodic passive BLE listen. Blocks ~1s while sampling the air.
uint32_t g_bleLastScan = 0;
void bleTick(uint32_t now) {
  bleEnsureInit();
  if (now - g_bleLastScan < 3000) return;
  g_bleLastScan = now;
  g_bleCount = 0;
  g_bleScan->start(1, false);
  g_bleScan->clearResults();
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
  int maxScroll = max(0, cnVisibleCount - 8);
  if (cnScroll > maxScroll) cnScroll = maxScroll;
}

void cnCapture(int st) {
  cnCount = 0;
  for (int i = 0; i < st && cnCount < 48; i++) {
    CnNet& n = cnNets[cnCount];
    // Copy into fixed buffer without retaining Arduino String temporaries.
    {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0)
        strncpy(n.ssid, "<hidden>", sizeof(n.ssid) - 1);
      else
        strncpy(n.ssid, ssid.c_str(), sizeof(n.ssid) - 1);
      n.ssid[sizeof(n.ssid) - 1] = 0;
    }
    const uint8_t* bs = WiFi.BSSID(i);
    if (bs) memcpy(n.bssid, bs, 6);
    else memset(n.bssid, 0, 6);
    n.rssi = WiFi.RSSI(i);
    n.channel = (uint8_t)WiFi.channel(i);
    n.auth = WiFi.encryptionType(i);
    cnCount++;
  }
  g_lastWifiCount = cnCount;
  cnRebuildVisible();
}

void cnOpen() {
  cnProcessed = -1;
  cnDetail = -1;
  cnScroll = 0;
  wifiScanBegin();
  cnLast = millis();
}
void cnTick(uint32_t now) {
  int st = wifiScanState();
  if (st >= 0 && st != cnProcessed) {
    cnProcessed = st;
    cnCapture(st);
    for (int i = 0; i < st; i++) {
      uint64_t k = bssidKey(WiFi.BSSID(i));
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
  WiFi.scanDelete();
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
  txt(x + 10, y + 6, theme::kGold, 2, "Look closer");
  char buf[48];
  snprintf(buf, sizeof(buf), "%s", n.ssid);
  gfxu::drawKVRow(G(), x + 8, y + 28, w - 16, 20, "SSID", buf, theme::kInk);
  snprintf(buf, sizeof(buf), "%s", mac);
  gfxu::drawKVRow(G(), x + 8, y + 52, w - 16, 20, "BSSID", buf, theme::kCyan);
  snprintf(buf, sizeof(buf), "%s", ven[0] ? ven : "unknown");
  gfxu::drawKVRow(G(), x + 8, y + 76, w - 16, 20, "Vendor", buf, theme::kTeal);
  snprintf(buf, sizeof(buf), "%s", encLabel(n.auth));
  gfxu::drawKVRow(G(), x + 8, y + 100, w - 16, 20, "Auth", buf,
                  riskColor(encRisk(n.auth)));
  snprintf(buf, sizeof(buf), "%d dBm  CH %u", (int)n.rssi, (unsigned)n.channel);
  gfxu::drawKVRow(G(), x + 8, y + 124, w - 16, 20, "Signal", buf, theme::kInk);
  rssiBars(x + w - 36, y + 130, n.rssi);
  btn(x + 8, y + h - 34, 90, 28, "BACK", false);
  btn(x + 110, y + h - 34, 100, 28, "RESCAN", true);
}

void cnDrawList(int x, int y, int w, int h) {
  body(x, y, w, h);
  int st = wifiScanState();
  if (st < 0 && cnCount == 0) {
    txt(x + 12, y + 10, theme::kInkDim, 1, "Sweeping the horizon...");
    return;
  }
  // Toolbar card
  G().fillRoundRect(x + 4, y + 2, w - 8, 22, 5, theme::kPanelSoft);
  txt(x + 10, y + 8, theme::kGold, 1, "%d nets", cnCount);
  const char* sorts[] = {"RSSI", "CH", "SSID"};
  for (int i = 0; i < 3; i++) {
    int bx = x + 68 + i * 40;
    chip(bx, y + 5, 38, theme::kChipH, sorts[i], cnSort == i, theme::kGold);
  }
  const char* filters[] = {"All", "Open", "Sec"};
  for (int i = 0; i < 3; i++) {
    int bx = x + 196 + i * 36;
    chip(bx, y + 5, 34, theme::kChipH, filters[i], cnFilter == i, theme::kTeal);
  }

  constexpr int kRows = 8;
  constexpr int kRh = theme::kRowH;
  int listTop = y + 28;
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
  // Scroll affordances (larger targets)
  if (cnScroll > 0)
    btn(x + w - 30, listTop, 26, 22, "^", false);
  if (cnScroll + kRows < cnVisibleCount)
    btn(x + w - 30, y + h - 36, 26, 22, "v", false);
  txt(x + 8, y + h - 12, theme::kInkMuted, 1, "Tap row = detail   empty = rescan");
}
void cnDraw(int x, int y, int w, int h) {
  if (cnDetail >= 0) cnDrawDetail(x, y, w, h);
  else cnDrawList(x, y, w, h);
}
bool cnTouch(int16_t x, int16_t y) {
  // Absolute screen coords; content region starts at y≈44.
  int ly = y;
  int cx = x;
  constexpr int kRows = 8;
  constexpr int kRh = theme::kRowH;
  constexpr int kListTop = 44 + 28;  // content y + toolbar
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
  if (ly >= 44 && ly <= 70) {
    for (int i = 0; i < 3; i++) {
      int bx = 68 + i * 40;
      if (cx >= bx && cx <= bx + 38) {
        cnSort = i;
        cnRebuildVisible();
        return true;
      }
    }
    for (int i = 0; i < 3; i++) {
      int bx = 196 + i * 36;
      if (cx >= bx && cx <= bx + 34) {
        cnFilter = i;
        cnScroll = 0;
        cnRebuildVisible();
        return true;
      }
    }
  }
  // Scroll buttons (right edge)
  if (cx >= 290) {
    if (ly < 120) {
      if (cnScroll > 0) cnScroll--;
      return true;
    }
    if (ly > 160) {
      if (cnScroll + kRows < cnVisibleCount) cnScroll++;
      return true;
    }
  }
  // Row tap -> detail
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
void hlClose() {}
void hlDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  G().fillRoundRect(x + 4, y + 2, w - 8, 18, 4, theme::kPanelSoft);
  txt(x + 10, y + 7, theme::kGold, 1, "%d bottles adrift", g_bleCount);
  txt(x + 160, y + 7, theme::kInkMuted, 1, "passive BLE");
  constexpr int kRh = theme::kRowHCompact;
  int rows = min(g_bleCount, 11);
  for (int i = 0; i < rows; i++) {
    int ry = y + 24 + i * kRh;
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
          "WigleWifi-1.4,appRelease=pocketpirate,model=ESP32-S3,release=0.4.2,"
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
        for (int i = 0; i < st; i++) {
          String ssid = WiFi.SSID(i);
          ssid.replace(",", " ");
          if (fix) {
            f.printf("%s,%s,%s,%s,%d,%d,%.6f,%.6f,%.1f,%.1f,WIFI\n",
                     WiFi.BSSIDstr(i).c_str(), ssid.c_str(),
                     wigleAuth(WiFi.encryptionType(i)), ts, WiFi.channel(i),
                     WiFi.RSSI(i), lat, lon, alt, acc);
          } else {
            f.printf("%s,%s,%s,%s,%d,%d,,,,,WIFI\n", WiFi.BSSIDstr(i).c_str(),
                     ssid.c_str(), wigleAuth(WiFi.encryptionType(i)), ts,
                     WiFi.channel(i), WiFi.RSSI(i));
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
void crClose() { WiFi.scanDelete(); }
void crDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  G().fillRoundRect(x + 4, y + 2, w - 8, 36, 6, theme::kPanelSoft);
  txt(x + 12, y + 8, theme::kGold, 1, "Chart Room");
  txt(x + 12, y + 22, theme::kInkMuted, 1, "WiGLE 1.4 wardrive → %s",
      kWigleFile);
  char v[40];
  gfxu::drawKVRow(G(), x + 6, y + 46, w - 12, 20, "SD card",
                  app::sdReady() ? "mounted" : "not found",
                  app::sdReady() ? theme::kGood : theme::kBad);
  snprintf(v, sizeof(v), "%lu", (unsigned long)crLoggedSession);
  gfxu::drawKVRow(G(), x + 6, y + 70, w - 12, 20, "Rows this voyage", v,
                  theme::kGold);
  bool fix = gps::hasFix();
  gfxu::drawKVRow(G(), x + 6, y + 94, w - 12, 20, "GPS", gps::statusLabel(),
                  fix ? theme::kGood : theme::kWarn);
  if (fix) {
    snprintf(v, sizeof(v), "%.5f, %.5f  %.0fm", gps::latitude(),
             gps::longitude(), gps::altitudeM());
    gfxu::drawKVRow(G(), x + 6, y + 118, w - 12, 20, "Fix", v, theme::kCyan);
  } else {
    gfxu::drawKVRow(G(), x + 6, y + 118, w - 12, 20, "Fix",
                    "lat/lon blank until fix", theme::kInkMuted);
  }
  txt(x + 12, y + 150, theme::kInkMuted, 1, "UART GPS GPIO43 RX / 44 TX");
  txt(x + 12, y + 166, theme::kInkMuted, 1, "Passive survey — no association.");
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
    for (int i = 0; i < st; i++) {
      int c = WiFi.channel(i);
      if (c >= 1 && c <= 13) {
        lkHist[c]++;
        if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) lkOpenHist[c]++;
      }
    }
    wifiScanBegin();
  }
}
void lkClose() { WiFi.scanDelete(); }
void lkDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  G().fillRoundRect(x + 4, y + 2, w - 8, 18, 4, theme::kPanelSoft);
  txt(x + 10, y + 7, theme::kGold, 1, "Channel occupancy");
  txt(x + 160, y + 7, theme::kInkMuted, 1, "tap a bar · n=%d", lkTotal);
  int maxv = 1;
  for (int c = 1; c <= 13; c++) maxv = max(maxv, lkHist[c]);
  int baseY = y + h - 40;
  int plotH = h - 68;
  int bw = 18;
  int gap = 3;
  // plot well
  G().fillRoundRect(x + 6, y + 24, w - 12, plotH + 18, 6, theme::kPanelSoft);
  for (int c = 1; c <= 13; c++) {
    int bx = x + 12 + (c - 1) * (bw + gap);
    int bh = maxv ? (lkHist[c] * plotH) / maxv : 0;
    bool sel = (lkSelected == c);
    uint16_t col = sel ? theme::kGold
                       : (lkHist[c] >= maxv && maxv > 1 ? theme::kWarn
                                                        : theme::kTeal);
    if (bh > 0)
      G().fillRoundRect(bx, baseY - bh, bw, bh, 3, col);
    if (lkOpenHist[c] > 0 && bh > 0) {
      int oh = max(2, (lkOpenHist[c] * plotH) / maxv);
      if (oh > bh) oh = bh;
      G().fillRoundRect(bx, baseY - oh, bw, oh, 2, theme::kBad);
    }
    if (sel) G().drawRoundRect(bx - 1, baseY - max(bh, 4) - 1, bw + 2,
                               max(bh, 4) + 2, 3, theme::kInk);
    txt(bx + (c >= 10 ? 1 : 5), baseY + 4, theme::kInkDim, 1, "%d", c);
    if (lkHist[c])
      txt(bx + 2, baseY - bh - 10, theme::kInk, 1, "%d", lkHist[c]);
  }
  // legend strip
  G().fillRoundRect(x + 4, y + h - 22, w - 8, 18, 4, theme::kBgDeep);
  G().fillRoundRect(x + 10, y + h - 16, 8, 8, 2, theme::kTeal);
  txt(x + 22, y + h - 16, theme::kInkDim, 1, "sec");
  G().fillRoundRect(x + 52, y + h - 16, 8, 8, 2, theme::kBad);
  txt(x + 64, y + h - 16, theme::kInkDim, 1, "open");
  if (lkSelected >= 1 && lkSelected <= 13) {
    txt(x + 110, y + h - 16, theme::kGold, 1, "CH%d: %d AP (%d open)",
        lkSelected, lkHist[lkSelected], lkOpenHist[lkSelected]);
  } else {
    txt(x + 110, y + h - 16, theme::kInkMuted, 1, "crowded = gold tip");
  }
}
bool lkTouch(int16_t x, int16_t y) {
  int baseY = 44 + 196 - 36;  // approximate content bottom
  (void)baseY;
  int bw = 18, gap = 3;
  // Content region y starts at 44; bars roughly from y+16 to y+h-36
  for (int c = 1; c <= 13; c++) {
    int bx = 10 + (c - 1) * (bw + gap);
    if (x >= bx && x <= bx + bw && y >= 60 && y <= 220) {
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
  int st = wifiScanState();
  if (st >= 0 && st != sgProcessed) {
    sgProcessed = st;
    sgHitCount = 0;
    for (int i = 0; i < st && sgHitCount < 12; i++) {
      String ssid = WiFi.SSID(i);
      String bssid = WiFi.BSSIDstr(i);
      for (int s = 0; s < spyglass::kSignatureCount; s++) {
        const auto& sig = spyglass::kSignatures[s];
        if (ouiMatch(bssid, sig.ouiPrefix) || ssidMatch(ssid, sig.ssidSubstr)) {
          Hit& hh = sgHits[sgHitCount++];
          hh.ssid = ssid.length() ? ssid : String("<hidden>");
          hh.bssid = bssid;
          hh.label = sig.label;
          hh.rssi = WiFi.RSSI(i);
          break;
        }
      }
    }
    wifiScanBegin();
  }
}
void sgClose() { WiFi.scanDelete(); }
void sgDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  G().fillRoundRect(x + 4, y + 2, w - 8, 40, 5, theme::kPanelSoft);
  txt(x + 10, y + 8, theme::kGold, 1, "Watchtowers spotted: %d", sgHitCount);
  if (spyglass::hasVerifiedSignature()) {
    txt(x + 10, y + 22, theme::kGood, 1, "Verified field OUIs armed (DeFlock).");
    txt(x + 10, y + 32, theme::kInkMuted, 1, "OUI hit = strong; keyword = candidate.");
  } else {
    txt(x + 10, y + 22, theme::kWarn, 1, "Keyword heuristics only — candidates.");
    txt(x + 10, y + 32, theme::kInkMuted, 1, "Verify against DeFlock.");
  }
  constexpr int kRh = theme::kRowH;
  int rows = min(sgHitCount, 7);
  for (int i = 0; i < rows; i++) {
    int ry = y + 48 + i * kRh;
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
void twClose() {}
void twDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  int n = 0;
  for (int i = 0; i < g_bleCount; i++)
    if (g_ble[i].kind == 1) n++;
  G().fillRoundRect(x + 4, y + 2, w - 8, 28, 5, theme::kPanelSoft);
  txt(x + 10, y + 8, n ? theme::kWarn : theme::kGood, 1,
      "Possible trackers nearby: %d", n);
  txt(x + 10, y + 20, theme::kInkMuted, 1, "AirTag / Tile / SmartTag signatures");
  int shown = 0;
  constexpr int kRh = theme::kRowH;
  for (int i = 0; i < g_bleCount && shown < 8; i++) {
    if (g_ble[i].kind != 1) continue;
    int ry = y + 36 + shown * kRh;
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
    txt(x + 12, y + 48, theme::kInkMuted, 1, "All clear — no trackers seen.");
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
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  esp_wifi_set_promiscuous(false);
  wifi_promiscuous_filter_t filt;
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&rwPromiscCb);
  esp_wifi_set_promiscuous(true);
  rwChannel = 1;
  esp_wifi_set_channel(rwChannel, WIFI_SECOND_CHAN_NONE);
}
void rwTick(uint32_t now) {
  if (now - rwHopAt > 300) {  // sweep channels so we cover the band
    rwHopAt = now;
    rwChannel = rwChannel >= 13 ? 1 : rwChannel + 1;
    esp_wifi_set_channel(rwChannel, WIFI_SECOND_CHAN_NONE);
  }
}
void rwClose() {
  esp_wifi_set_promiscuous(false);
  WiFi.mode(WIFI_STA);
}
void rwDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  bool recent = (millis() - rwLastHit) < 4000 && (rwDeauth + rwDisassoc) > 0;
  G().fillRoundRect(x + 4, y + 2, w - 8, 36, 6, theme::kPanelSoft);
  txt(x + 12, y + 8, theme::kGold, 1, "Rigging Watch");
  txt(x + 12, y + 22, theme::kInkMuted, 1, "Listening for deauth storms · ch %d",
      rwChannel);
  char v[24];
  snprintf(v, sizeof(v), "%lu", (unsigned long)rwDeauth);
  gfxu::drawKVRow(G(), x + 6, y + 46, w - 12, 22, "Deauth frames", v, theme::kBad);
  snprintf(v, sizeof(v), "%lu", (unsigned long)rwDisassoc);
  gfxu::drawKVRow(G(), x + 6, y + 72, w - 12, 22, "Disassoc frames", v,
                  theme::kWarn);
  gfxu::drawKVRow(G(), x + 6, y + 98, w - 12, 22, "Last source", rwLastSrc,
                  theme::kCyan);
  snprintf(v, sizeof(v), "%d dB", rwLastRssi);
  gfxu::drawKVRow(G(), x + 6, y + 124, w - 12, 22, "Last RSSI", v);
  if (recent) {
    G().fillRoundRect(x + 6, y + 154, w - 12, 28, 6, theme::kBad);
    G().setTextColor(theme::kInk);
    G().setTextSize(1);
    G().setCursor(x + 14, y + 164);
    G().print("ALERT: attack frames nearby!");
  } else {
    txt(x + 12, y + 160, theme::kGood, 1, "Calm seas — no attack detected.");
  }
}
bool rwTouch(int16_t, int16_t) { return false; }
}  // namespace

// ===========================================================================
//  8. Hull Inspection -- security posture audit of nearby networks
// ===========================================================================
namespace {
int hiProcessed = -1;
int hiOpen_ = 0, hiWeak = 0, hiStrong = 0, hiTotal = 0;
void hiOpen() {
  hiProcessed = -1;
  wifiScanBegin();
}
void hiTick(uint32_t now) {
  int st = wifiScanState();
  if (st >= 0 && st != hiProcessed) {
    hiProcessed = st;
    hiOpen_ = hiWeak = hiStrong = 0;
    hiTotal = st;
    for (int i = 0; i < st; i++) {
      int r = encRisk(WiFi.encryptionType(i));
      if (r == 3) hiOpen_++;
      else if (r == 2) hiWeak++;
      else if (r == 0) hiStrong++;
    }
  }
}
void hiClose() { WiFi.scanDelete(); }
void hiDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  G().fillRoundRect(x + 4, y + 2, w - 8, 28, 5, theme::kPanelSoft);
  txt(x + 10, y + 6, theme::kGold, 1, "Hull Inspection");
  txt(x + 10, y + 18, theme::kInkMuted, 1, "%d networks audited · passive",
      hiTotal);
  char v[12];
  snprintf(v, sizeof(v), "%d", hiOpen_);
  gfxu::drawKVRow(G(), x + 6, y + 36, w - 12, 18, "Open / WEP (risky)", v,
                  theme::kBad);
  snprintf(v, sizeof(v), "%d", hiWeak);
  gfxu::drawKVRow(G(), x + 6, y + 56, w - 12, 18, "WPA (aging)", v, theme::kWarn);
  snprintf(v, sizeof(v), "%d", hiStrong);
  gfxu::drawKVRow(G(), x + 6, y + 76, w - 12, 18, "WPA3 (strong)", v,
                  theme::kGood);
  txt(x + 10, y + 100, theme::kInkMuted, 1, "Prefer WPA2/3 · disable WPS · enable PMF");
  constexpr int kRh = theme::kRowHCompact;
  int rows = min(hiTotal, 5);
  for (int i = 0; i < rows; i++) {
    int ry = y + 116 + i * kRh;
    if (i & 1) G().fillRect(x + 4, ry, w - 8, kRh, theme::kPanelSoft);
    wifi_auth_mode_t m = WiFi.encryptionType(i);
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) ssid = "<hidden>";
    if (ssid.length() > 18) ssid = ssid.substring(0, 18);
    txt(x + 10, ry + 4, theme::kInk, 1, "%s", ssid.c_str());
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

  // List pane
  G().fillRoundRect(x + 4, y + 2, w - 8, 26, 5, theme::kPanelSoft);
  txt(x + 10, y + 6, theme::kGold, 1, "Captain's Log · %d items", clCount);
  txt(x + 10, y + 16, theme::kInkMuted, 1, "Tap row = preview   empty = refresh");
  const int visible = 8;
  constexpr int kRh = theme::kRowH;
  if (clScroll > clCount - visible) clScroll = max(0, clCount - visible);
  if (clScroll < 0) clScroll = 0;
  int rows = min(visible, clCount - clScroll);
  for (int i = 0; i < rows; i++) {
    int idx = clScroll + i;
    int ry = y + 32 + i * kRh;
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
    btn(x + 280, y + 32, 28, 22, "^", false);
    btn(x + 280, y + 160, 28, 22, "v", false);
  }
}

bool clTouch(int16_t x, int16_t y) {
  int ly = y - 44;  // content-local
  if (clMode == 1) {
    if (ly >= 0 && ly <= 28 && x >= 8 && x <= 90) {
      clMode = 0;
      return true;
    }
    return true;
  }
  // Scroll buttons
  if (clCount > 8 && x >= 270) {
    if (ly >= 28 && ly <= 60) {
      clScroll = max(0, clScroll - 3);
      return true;
    }
    if (ly >= 156 && ly <= 190) {
      clScroll = min(max(0, clCount - 8), clScroll + 3);
      return true;
    }
  }
  // Row tap (theme::kRowH = 18)
  if (ly >= 28 && ly <= 190 && x < 270) {
    int row = (ly - 28) / theme::kRowH;
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
  txt(x + 10, y + 6, theme::kGold, 1, "Ship's Systems");
  char v[40];
  snprintf(v, sizeof(v), "%s x%d @ %dMHz", ESP.getChipModel(),
           ESP.getChipCores(), getCpuFrequencyMhz());
  gfxu::drawKVRow(G(), x + 6, y + 24, w - 12, 18, "Chip", v, theme::kCyan);
  snprintf(v, sizeof(v), "%u KB free", (unsigned)(ESP.getFreeHeap() / 1024));
  gfxu::drawKVRow(G(), x + 6, y + 44, w - 12, 18, "Heap", v);
  snprintf(v, sizeof(v), "%u / %u KB", (unsigned)(ESP.getFreePsram() / 1024),
           (unsigned)(ESP.getPsramSize() / 1024));
  gfxu::drawKVRow(G(), x + 6, y + 64, w - 12, 18, "PSRAM", v, theme::kTeal);
  snprintf(v, sizeof(v), "%u MB",
           (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
  gfxu::drawKVRow(G(), x + 6, y + 84, w - 12, 18, "Flash", v);
  gfxu::drawKVRow(G(), x + 6, y + 104, w - 12, 18, "SD",
                  app::sdReady() ? "mounted" : "absent",
                  app::sdReady() ? theme::kGood : theme::kBad);
  uint32_t up = millis() / 1000;
  snprintf(v, sizeof(v), "%lu:%02lu:%02lu", (unsigned long)(up / 3600),
           (unsigned long)((up % 3600) / 60), (unsigned long)(up % 60));
  gfxu::drawKVRow(G(), x + 6, y + 124, w - 12, 18, "Uptime", v);
  gfxu::drawKVRow(G(), x + 6, y + 144, w - 12, 18, "GPS", gps::statusLabel(),
                  gps::hasFix() ? theme::kGood : theme::kInkDim);
  int pct = power::batteryPct();
  snprintf(v, sizeof(v), "%s  %lumV", power::powerLabel(),
           (unsigned long)power::batteryMv());
  gfxu::drawKVRow(G(), x + 6, y + 164, w - 12, 18, "Power", v, theme::kGold);
  G().drawRoundRect(x + 6, y + 186, 104, 10, 3, theme::kBorder);
  G().fillRoundRect(x + 8, y + 188, max(1, pct), 6, 2,
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
  txt(x + 8, y + 6, theme::kInk, 2, "Signal Lantern");

  char modeLab[24];
  snprintf(modeLab, sizeof(modeLab), "Mode: %s", led::modeName(led::mode()));
  btn(x + 8, y + 32, 150, 30, modeLab, false);
  txt(x + 168, y + 42, theme::kInkMuted, 1, "tap to cycle");

  // Color swatches
  txt(x + 8, y + 72, theme::kInkDim, 1, "Color:");
  for (int i = 0; i < led::kColorCount; i++) {
    uint32_t c = led::colorRgb(i);
    uint16_t col565 = G().color565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    int sx = x + 52 + i * 32;
    G().fillRoundRect(sx, y + 66, 26, 26, 5, col565);
    if (i == led::colorIdx())
      G().drawRoundRect(sx - 2, y + 64, 30, 30, 6, theme::kGold);
  }

  // LED brightness row
  txt(x + 8, y + 108, theme::kInk, 1, "Glow: %d%%", led::brightness());
  btn(x + 150, y + 102, 30, 24, "-", false);
  btn(x + 186, y + 102, 30, 24, "+", false);

  // Sound row
  btn(x + 8, y + 140, 150, 30, app::sound() ? "Sound: ON" : "Sound: off",
      false, app::sound() ? theme::kGood : theme::kLocked);

  txt(x + 8, y + 182, theme::kInkDim, 1,
      "Profile persists & glows on every screen.");
}
bool slTouch(int16_t x, int16_t y) {
  int ly = y - 44;  // content-local
  if (ly >= 26 && ly <= 68 && x <= 200) {  // mode row (generous)
    led::setMode((led::mode() + 1) % led::ModeCount);
    return true;
  }
  if (ly >= 58 && ly <= 98) {  // swatch band
    for (int i = 0; i < led::kColorCount; i++) {
      int sx = 52 + i * 32;
      if (x >= sx - 3 && x <= sx + 29) {
        led::setColorIdx(i);
        return true;
      }
    }
  }
  if (ly >= 96 && ly <= 132) {  // glow +/- row
    if (x >= 144 && x <= 182) {
      led::setBrightness(led::brightness() - 10);
      return true;
    }
    if (x >= 183 && x <= 222) {
      led::setBrightness(led::brightness() + 10);
      return true;
    }
  }
  if (ly >= 134 && ly <= 176 && x >= 8 && x <= 170) {  // sound toggle
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
  // Identity card
  G().fillRoundRect(x + 4, y + 2, w - 8, 34, 6, theme::kPanelSoft);
  txt(x + 12, y + 8, theme::kGold, 1, "Settings Cabin");
  txt(x + 12, y + 22, theme::kInkDim, 1, "%s · %s Lv%d · fw %s",
      game::profile.name, game::rankTitle(game::profile.level),
      game::profile.level, PP_VERSION);

  // Row: Rename + Brightness
  btn(x + 8, y + 42, 118, 26, "Rename…", false);
  char bri[16];
  snprintf(bri, sizeof(bri), "Bri %d", app::brightness());
  txt(x + 134, y + 50, theme::kInkDim, 1, "%s", bri);
  btn(x + 232, y + 42, 28, 26, "-", false);
  btn(x + 264, y + 42, 28, 26, "+", false);

  // Row: Sound + Idle sleep
  btn(x + 8, y + 74, 120, 26, app::sound() ? "Sound: ON" : "Sound: off",
      false, app::sound() ? theme::kGood : theme::kLocked);
  btn(x + 136, y + 74, 160, 26,
      power::idleSleep() ? "Idle sleep: ON" : "Idle sleep: off", false,
      power::idleSleep() ? theme::kWarn : theme::kPanelHi);

  // Row: Sleep / OTA
  btn(x + 8, y + 106, 140, 26, "Sleep now", false, theme::kTeal);
  bool otaOk = ota::wifiConfigured() && ota::urlConfigured();
  btn(x + 156, y + 106, 140, 26, seOtaBusy ? "OTA…" : "OTA Update", otaOk,
      otaOk ? 0 : theme::kLocked);

  // Status block
  G().fillRoundRect(x + 4, y + 138, w - 8, 28, 5, theme::kBgDeep);
  txt(x + 10, y + 144, theme::kInkMuted, 1, "OTA: %s", ota::status());
  if (ota::wifiConfigured())
    txt(x + 10, y + 154, theme::kInkMuted, 1, "WiFi:%s  URL:%s",
        ota::wifiSsid(), ota::urlConfigured() ? "set" : "none");
  else
    txt(x + 10, y + 154, theme::kInkMuted, 1,
        "Set WiFi+URL via companion WIFICFG/OTAURL");

  btn(x + 8, y + 172, 170, 24, "Reset progress", false, theme::kBad);
}

bool seTouch(int16_t x, int16_t y) {
  int ly = y - 44;
  if (ly >= 38 && ly <= 70) {
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
  if (ly >= 68 && ly <= 100) {
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
  if (ly >= 98 && ly <= 130) {
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
  if (ly >= 164 && ly <= 196 && x >= 8 && x <= 190) {
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
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  esp_wifi_set_promiscuous(false);
  wifi_promiscuous_filter_t filt;
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&pwPromiscCb);
  esp_wifi_set_promiscuous(true);
  pwChannel = 1;
  esp_wifi_set_channel(pwChannel, WIFI_SECOND_CHAN_NONE);
}
void pwTick(uint32_t now) {
  if (now - pwHopAt > 350) {  // sweep the band so we hear every client
    pwHopAt = now;
    pwChannel = pwChannel >= 13 ? 1 : pwChannel + 1;
    esp_wifi_set_channel(pwChannel, WIFI_SECOND_CHAN_NONE);
  }
  while (pwRewarded < pwCount) {  // xp for each new unique client seen
    pwRewarded++;
    game::awardXp(1);
  }
}
void pwClose() {
  esp_wifi_set_promiscuous(false);
  WiFi.mode(WIFI_STA);
}
void pwDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  G().fillRoundRect(x + 4, y + 2, w - 8, 26, 5, theme::kPanelSoft);
  txt(x + 10, y + 6, theme::kGold, 1, "Clients probing: %d  (ch %d)", pwCount,
      pwChannel);
  txt(x + 10, y + 16, theme::kInkMuted, 1, "%lu probe frames · passive",
      (unsigned long)pwTotal);
  constexpr int kRh = theme::kRowHCompact;
  int rows = min(pwCount, 10);
  for (int i = 0; i < rows; i++) {
    int ry = y + 32 + i * kRh;
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
    txt(x + 12, y + 48, theme::kInkMuted, 1, "Listening… hopping channels.");
}
bool pwTouch(int16_t, int16_t) { return false; }
}  // namespace

// ===========================================================================
//  14. Deep BLE ID -- advertisement decoder (company / iBeacon / Eddystone)
// ===========================================================================
namespace {
void dbOpen() { g_bleLastScan = 0; }
void dbTick(uint32_t now) { bleTick(now); }
void dbClose() {}
void dbDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  G().fillRoundRect(x + 4, y + 2, w - 8, 16, 4, theme::kPanelSoft);
  txt(x + 10, y + 6, theme::kGold, 1, "Decoding %d advertisers", g_bleCount);
  constexpr int kRh = theme::kRowH;
  int rows = min(g_bleCount, 9);
  for (int i = 0; i < rows; i++) {
    int ry = y + 22 + i * kRh;
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
    txt(x + 12, y + 28, theme::kInkMuted, 1, "Sampling the air...");
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
  txt(x + 10, y + 6, theme::kGold, 1, "Ship's Instruments");
  char v[40];
  snprintf(v, sizeof(v), "%.1f C / %.0f F", siTemp,
           siTemp * 9.0f / 5.0f + 32.0f);
  gfxu::drawKVRow(G(), x + 6, y + 24, w - 12, 20, "Core temp", v, theme::kGold);
  int pct = power::batteryPct();
  snprintf(v, sizeof(v), "%s  %lumV", power::powerLabel(),
           (unsigned long)power::batteryMv());
  gfxu::drawKVRow(G(), x + 6, y + 48, w - 12, 20, "Power", v, theme::kGold);
  G().drawRoundRect(x + 6, y + 72, 104, 10, 3, theme::kBorder);
  G().fillRoundRect(x + 8, y + 74, max(1, pct), 6, 2,
                          power::lowBattery() ? theme::kBad : theme::kGood);
  bool fix = gps::hasFix();
  gfxu::drawKVRow(G(), x + 6, y + 90, w - 12, 20, "GPS", gps::statusLabel(),
                  fix ? theme::kGood : theme::kWarn);
  if (fix) {
    snprintf(v, sizeof(v), "%.5f, %.5f", gps::latitude(), gps::longitude());
    gfxu::drawKVRow(G(), x + 6, y + 114, w - 12, 20, "Lat/Lon", v, theme::kCyan);
    snprintf(v, sizeof(v), "%.0fm  sats %lu  hdop %.1f", gps::altitudeM(),
             (unsigned long)gps::satellites(), gps::hdop());
    gfxu::drawKVRow(G(), x + 6, y + 138, w - 12, 20, "Alt/HD", v);
  } else {
    gfxu::drawKVRow(G(), x + 6, y + 114, w - 12, 20, "Wiring",
                    "TX->GPIO43 RX->GPIO44", theme::kInkMuted);
  }
  snprintf(v, sizeof(v), "%d%% / %s", app::brightness(),
           app::sdReady() ? "ok" : "no");
  gfxu::drawKVRow(G(), x + 6, y + 162, w - 12, 20, "Bri / SD", v);
  txt(x + 10, y + 188, theme::kInkMuted, 1, "Core temp is on-die (reads warm).");
}
bool siTouch(int16_t, int16_t) { return false; }
}  // namespace

// ===========================================================================
//  Registry
// ===========================================================================
namespace tools {

lgfx::LGFXBase* gfx = nullptr;

static const Tool kTools[] = {
    // title, subtitle, tile (short grid label), accent, handlers...
    {"Crow's Nest", "Wi-Fi survey", "Nest", theme::kSail, cnOpen, cnTick, cnClose,
     cnDraw, cnTouch},
    {"Harbor Ledger", "BLE discovery", "Harbor", theme::kSeaFoam, hlOpen, hlTick,
     hlClose, hlDraw, hlTouch},
    {"Chart Room", "Wardrive log", "Charts", theme::kWood, crOpen, crTick, crClose,
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
    {"Captain's Log", "SD + WiGLE", "Log", theme::kSail, clOpen, clTick, clClose,
     clDraw, clTouch},
    {"Ship's Systems", "Diagnostics", "Systems", theme::kSeaFoam, ssOpen, ssTick,
     ssClose, ssDraw, ssTouch},
    {"Signal Lantern", "RGB LED / FX", "Lantern", theme::kSun, slOpen, slTick,
     slClose, slDraw, slTouch},
    {"Probe Watch", "Client sniffer", "Probe", theme::kSail, pwOpen, pwTick,
     pwClose, pwDraw, pwTouch},
    {"Deep BLE ID", "Adv decoder", "BLE ID", theme::kSeaFoam, dbOpen, dbTick,
     dbClose, dbDraw, dbTouch},
    {"Instruments", "Onboard sensors", "Sensors", theme::kGold, siOpen, siTick,
     siClose, siDraw, siTouch},
    {"Settings Cabin", "Options", "Settings", theme::kInkDim, seOpen, seTick,
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

}  // namespace tools
