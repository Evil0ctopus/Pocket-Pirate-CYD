#include "tools.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <BLEDevice.h>
#include <SD_MMC.h>

#include <set>

#include "app.h"
#include "board_pins.h"
#include "game_state.h"
#include "led.h"
#include "oui.h"
#include "pirate_theme.h"
#include "spyglass_signatures.h"

using namespace CheapBlackDisplay;

// ===========================================================================
//  Shared drawing helpers
// ===========================================================================
namespace {

lgfx::LGFXBase& G() { return *tools::gfx; }

void body(int x, int y, int w, int h) { G().fillRect(x, y, w, h, theme::kPanel); }

void txt(int x, int y, uint16_t fg, uint8_t size, const char* fmt, ...) {
  char b[96];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof(b), fmt, a);
  va_end(a);
  auto& g = G();
  g.setTextSize(size);
  g.setTextColor(fg, theme::kPanel);
  g.setCursor(x, y);
  g.print(b);
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
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
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
//  1. Crow's Nest -- Wi-Fi survey
// ===========================================================================
namespace {
uint32_t cnLast = 0;
int cnProcessed = -1;

void cnOpen() {
  cnProcessed = -1;
  wifiScanBegin();
  cnLast = millis();
}
void cnTick(uint32_t now) {
  int st = wifiScanState();
  if (st >= 0 && st != cnProcessed) {
    cnProcessed = st;
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
  if (st < 0 && now - cnLast > 8000) {  // idle -> rescan
    wifiScanBegin();
    cnLast = now;
  }
}
void cnClose() { WiFi.scanDelete(); }
void cnDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  int st = wifiScanState();
  if (st < 0) {
    txt(x + 8, y + 6, theme::kInkDim, 1, "Sweeping the horizon...");
    return;
  }
  txt(x + 8, y + 4, theme::kGold, 1, "%d networks in sight", st);
  int rows = min(st, 14);
  for (int i = 0; i < rows; i++) {
    int ry = y + 18 + i * 12;
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) ssid = "<hidden>";
    if (ssid.length() > 15) ssid = ssid.substring(0, 15);
    rssiBars(x + 6, ry, WiFi.RSSI(i));
    txt(x + 26, ry, theme::kInk, 1, "%s", ssid.c_str());
    const uint8_t* bs = WiFi.BSSID(i);
    const char* ven = bs ? oui::vendor(bs) : "";
    if (ven[0]) txt(x + 128, ry, theme::kSeaFoam, 1, "%.9s", ven);
    txt(x + 196, ry, theme::kInkDim, 1, "c%02d", WiFi.channel(i));
    txt(x + 234, ry, riskColor(encRisk(WiFi.encryptionType(i))), 1, "%s",
        encLabel(WiFi.encryptionType(i)));
  }
}
bool cnTouch(int16_t, int16_t) {
  wifiScanBegin();
  cnLast = millis();
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
  txt(x + 8, y + 4, theme::kGold, 1, "%d bottles adrift", g_bleCount);
  int rows = min(g_bleCount, 14);
  for (int i = 0; i < rows; i++) {
    int ry = y + 18 + i * 12;
    rssiBars(x + 6, ry, g_ble[i].rssi);
    String label = g_ble[i].name.length() ? g_ble[i].name : g_ble[i].mac;
    if (label.length() > 18) label = label.substring(0, 18);
    uint16_t c = g_ble[i].kind == 1 ? theme::kWarn : theme::kInk;
    txt(x + 26, ry, c, 1, "%s", label.c_str());
    const char* ven = oui::vendorStr(g_ble[i].mac);
    if (ven[0]) txt(x + 178, ry, theme::kSeaFoam, 1, "%.8s", ven);
    txt(x + 250, ry, theme::kInkDim, 1, "%ddB", g_ble[i].rssi);
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

// WiGLE-style capability string from our auth mode (best effort; the CCMP/TKIP
// cipher detail isn't exposed by the Arduino scan API, so we approximate it).
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

// Synthetic "FirstSeen" from uptime -- there is no RTC/GPS on board, so this is
// a monotonic placeholder. Add a GPS module (with time) to get real fixes.
void firstSeen(char* out, size_t n) {
  uint32_t s = millis() / 1000;
  snprintf(out, n, "2000-01-01 %02lu:%02lu:%02lu", (unsigned long)(s / 3600),
           (unsigned long)((s % 3600) / 60), (unsigned long)(s % 60));
}

void crEnsureHeader() {
  if (crHeader || !app::sdReady()) return;
  if (!SD_MMC.exists(kWigleFile)) {
    File f = SD_MMC.open(kWigleFile, FILE_WRITE);
    if (f) {
      // WiGLE pre-header line, then the column header.
      f.println(
          "WigleWifi-1.4,appRelease=pocketpirate,model=ESP32-S3,release=1.0,"
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
  int st = wifiScanState();
  if (st >= 0 && st != crProcessed) {
    crProcessed = st;
    if (app::sdReady()) {
      File f = SD_MMC.open(kWigleFile, FILE_APPEND);
      if (f) {
        char ts[24];
        firstSeen(ts, sizeof(ts));
        for (int i = 0; i < st; i++) {
          // lat/lon/alt/accuracy left blank; wire a GPS module to fill them.
          String ssid = WiFi.SSID(i);
          ssid.replace(",", " ");  // keep the CSV well-formed
          f.printf("%s,%s,%s,%s,%d,%d,,,,,WIFI\n", WiFi.BSSIDstr(i).c_str(),
                   ssid.c_str(), wigleAuth(WiFi.encryptionType(i)), ts,
                   WiFi.channel(i), WiFi.RSSI(i));
          crLoggedSession++;
        }
        f.close();
        game::addLoot(game::Loot::Cargo, 1);
        tools::toast("Logged %d to hold", st);
      }
    }
    wifiScanBegin();  // continuous survey
  }
}
void crClose() { WiFi.scanDelete(); }
void crDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  txt(x + 8, y + 8, theme::kInk, 2, "Chart Room");
  txt(x + 8, y + 34, theme::kInkDim, 1, "WiGLE 1.4 wardrive log ->");
  txt(x + 8, y + 46, theme::kInkDim, 1, "%s (upload-ready CSV).", kWigleFile);
  txt(x + 8, y + 70, app::sdReady() ? theme::kGood : theme::kBad, 1,
      "SD card: %s", app::sdReady() ? "mounted" : "not found");
  txt(x + 8, y + 90, theme::kGold, 1, "Rows logged this voyage: %lu",
      (unsigned long)crLoggedSession);
  txt(x + 8, y + 110, theme::kInkDim, 1, "Lat/lon/time need a GPS module.");
  txt(x + 8, y + 140, theme::kInkDim, 1, "Passive survey - no association.");
}
bool crTouch(int16_t, int16_t) { return false; }
}  // namespace

// ===========================================================================
//  4. Lookout -- channel occupancy analyzer
// ===========================================================================
namespace {
int lkHist[14] = {0};
int lkProcessed = -1;
void lkOpen() {
  lkProcessed = -1;
  wifiScanBegin();
}
void lkTick(uint32_t now) {
  int st = wifiScanState();
  if (st >= 0 && st != lkProcessed) {
    lkProcessed = st;
    for (int c = 1; c <= 13; c++) lkHist[c] = 0;
    for (int i = 0; i < st; i++) {
      int c = WiFi.channel(i);
      if (c >= 1 && c <= 13) lkHist[c]++;
    }
    wifiScanBegin();
  }
}
void lkClose() { WiFi.scanDelete(); }
void lkDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  txt(x + 8, y + 4, theme::kGold, 1, "APs per channel (1-13)");
  int maxv = 1;
  for (int c = 1; c <= 13; c++) maxv = max(maxv, lkHist[c]);
  int baseY = y + h - 20;
  int bw = 20;
  for (int c = 1; c <= 13; c++) {
    int bx = x + 8 + (c - 1) * (bw + 2);
    int bh = (lkHist[c] * (h - 50)) / maxv;
    uint16_t col = lkHist[c] >= maxv && maxv > 1 ? theme::kWarn : theme::kGood;
    G().fillRect(bx, baseY - bh, bw, bh, col);
    txt(bx + 4, baseY + 4, theme::kInkDim, 1, "%d", c);
    if (lkHist[c]) txt(bx + 4, baseY - bh - 10, theme::kInk, 1, "%d", lkHist[c]);
  }
}
bool lkTouch(int16_t, int16_t) { return false; }
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
  txt(x + 8, y + 4, theme::kGold, 1, "Watchtowers spotted: %d", sgHitCount);
  if (!spyglass::hasVerifiedSignature()) {
    txt(x + 8, y + 20, theme::kWarn, 1, "Keyword heuristics only -- hits are");
    txt(x + 8, y + 32, theme::kInkDim, 1, "candidates. Verify against DeFlock.");
  }
  int rows = min(sgHitCount, 10);
  for (int i = 0; i < rows; i++) {
    int ry = y + 54 + i * 14;
    txt(x + 8, ry, theme::kBad, 1, "%s", sgHits[i].label.c_str());
    txt(x + 8, ry + 6, theme::kInkDim, 1, "%s  %s  %ddB",
        sgHits[i].ssid.c_str(), sgHits[i].bssid.c_str(), sgHits[i].rssi);
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
  txt(x + 8, y + 4, n ? theme::kWarn : theme::kGood, 1,
      "Possible trackers nearby: %d", n);
  txt(x + 8, y + 18, theme::kInkDim, 1, "AirTag / Tile / SmartTag signatures");
  int shown = 0;
  for (int i = 0; i < g_bleCount && shown < 12; i++) {
    if (g_ble[i].kind != 1) continue;
    int ry = y + 36 + shown * 13;
    rssiBars(x + 6, ry, g_ble[i].rssi);
    String label = g_ble[i].name.length() ? g_ble[i].name : g_ble[i].mac;
    txt(x + 26, ry, theme::kWarn, 1, "%s", label.c_str());
    txt(x + 250, ry, theme::kInkDim, 1, "%ddB", g_ble[i].rssi);
    shown++;
  }
  if (n == 0)
    txt(x + 8, y + 40, theme::kInkDim, 1, "All clear - no trackers seen.");
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
  txt(x + 8, y + 8, theme::kInk, 2, "Rigging Watch");
  txt(x + 8, y + 34, theme::kInkDim, 1, "Listening for deauth storms  ch %d",
      rwChannel);
  txt(x + 8, y + 60, theme::kBad, 1, "Deauth frames:   %lu",
      (unsigned long)rwDeauth);
  txt(x + 8, y + 74, theme::kWarn, 1, "Disassoc frames: %lu",
      (unsigned long)rwDisassoc);
  txt(x + 8, y + 96, theme::kInkDim, 1, "Last source: %s", rwLastSrc);
  txt(x + 8, y + 108, theme::kInkDim, 1, "Last RSSI:   %d dB", rwLastRssi);
  if (recent) {
    G().fillRoundRect(x + 8, y + 130, w - 16, 26, 4, theme::kBad);
    G().setTextColor(theme::kInk, theme::kBad);
    G().setTextSize(1);
    G().setCursor(x + 16, y + 139);
    G().print("ALERT: attack frames detected nearby!");
  } else {
    txt(x + 8, y + 134, theme::kGood, 1, "Calm seas - no attack detected.");
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
  txt(x + 8, y + 6, theme::kInk, 2, "Hull Inspection");
  txt(x + 8, y + 32, theme::kInkDim, 1, "%d networks audited", hiTotal);
  txt(x + 8, y + 52, theme::kBad, 1, "Open / WEP (risky):  %d", hiOpen_);
  txt(x + 8, y + 66, theme::kWarn, 1, "WPA (aging):         %d", hiWeak);
  txt(x + 8, y + 80, theme::kGood, 1, "WPA3 (strong):       %d", hiStrong);
  txt(x + 8, y + 104, theme::kInkDim, 1, "Your network should read WPA2/3,");
  txt(x + 8, y + 116, theme::kInkDim, 1, "never OPEN or WEP. Turn off WPS");
  txt(x + 8, y + 128, theme::kInkDim, 1, "and enable PMF on the router.");
  int rows = min(hiTotal, 6);
  for (int i = 0; i < rows; i++) {
    int ry = y + 150 + i * 12;
    wifi_auth_mode_t m = WiFi.encryptionType(i);
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) ssid = "<hidden>";
    if (ssid.length() > 18) ssid = ssid.substring(0, 18);
    txt(x + 8, ry, theme::kInk, 1, "%s", ssid.c_str());
    txt(x + 230, ry, riskColor(encRisk(m)), 1, "%s", encLabel(m));
  }
}
bool hiTouch(int16_t, int16_t) { return false; }
}  // namespace

// ===========================================================================
//  9. Captain's Log -- microSD file browser (read-only)
// ===========================================================================
namespace {
String clFiles[24];
long clSizes[24];
int clCount = 0;
void clRefresh() {
  clCount = 0;
  if (!app::sdReady()) return;
  File dir = SD_MMC.open("/");
  if (!dir) return;
  File f = dir.openNextFile();
  while (f && clCount < 24) {
    String n = String(f.name());
    int slash = n.lastIndexOf('/');
    if (slash >= 0) n = n.substring(slash + 1);
    clFiles[clCount] = (f.isDirectory() ? String("[") + n + "]" : n);
    clSizes[clCount] = f.isDirectory() ? -1 : (long)f.size();
    clCount++;
    f = dir.openNextFile();
  }
  dir.close();
}
void clOpen() { clRefresh(); }
void clTick(uint32_t) {}
void clClose() {}
void clDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  if (!app::sdReady()) {
    txt(x + 8, y + 10, theme::kBad, 1, "No card in the hold.");
    txt(x + 8, y + 26, theme::kInkDim, 1, "Insert a microSD and reopen.");
    return;
  }
  txt(x + 8, y + 4, theme::kGold, 1, "Ship's hold: %d items", clCount);
  int rows = min(clCount, 15);
  for (int i = 0; i < rows; i++) {
    int ry = y + 18 + i * 12;
    String nm = clFiles[i];
    if (nm.length() > 26) nm = nm.substring(0, 26);
    txt(x + 8, ry, theme::kInk, 1, "%s", nm.c_str());
    if (clSizes[i] >= 0)
      txt(x + 236, ry, theme::kInkDim, 1, "%ldB", clSizes[i]);
  }
}
bool clTouch(int16_t, int16_t) {
  clRefresh();
  return true;
}
}  // namespace

// ===========================================================================
//  10. Ship's Systems -- diagnostics
// ===========================================================================
namespace {
int ssBatteryPct() {
  uint32_t mv = analogReadMilliVolts(BATTERY_ADC) * 2;  // typical /2 divider
  if (mv < 3300) mv = 3300;
  if (mv > 4200) mv = 4200;
  return (int)((mv - 3300) * 100 / 900);
}
void ssOpen() {}
void ssTick(uint32_t) {}
void ssClose() {}
void ssDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  txt(x + 8, y + 6, theme::kInk, 2, "Ship's Systems");
  int ry = y + 34;
  txt(x + 8, ry, theme::kInkDim, 1, "Chip:  %s  x%d @ %dMHz",
      ESP.getChipModel(), ESP.getChipCores(), getCpuFrequencyMhz());
  ry += 14;
  txt(x + 8, ry, theme::kInkDim, 1, "Heap:  %u KB free",
      (unsigned)(ESP.getFreeHeap() / 1024));
  ry += 14;
  txt(x + 8, ry, theme::kInkDim, 1, "PSRAM: %u / %u KB free",
      (unsigned)(ESP.getFreePsram() / 1024),
      (unsigned)(ESP.getPsramSize() / 1024));
  ry += 14;
  txt(x + 8, ry, theme::kInkDim, 1, "Flash: %u MB",
      (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
  ry += 14;
  txt(x + 8, ry, theme::kInkDim, 1, "SD:    %s",
      app::sdReady() ? "mounted" : "absent");
  ry += 14;
  uint32_t up = millis() / 1000;
  txt(x + 8, ry, theme::kInkDim, 1, "Uptime:%lu:%02lu:%02lu",
      (unsigned long)(up / 3600), (unsigned long)((up % 3600) / 60),
      (unsigned long)(up % 60));
  ry += 20;
  int pct = ssBatteryPct();
  txt(x + 8, ry, theme::kGold, 1, "Battery: ~%d%%", pct);
  G().drawRoundRect(x + 100, ry - 2, 104, 12, 2, theme::kInk);
  G().fillRect(x + 102, ry, pct, 8,
               pct < 20 ? theme::kBad : theme::kGood);
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

  // Mode row (tap to cycle)
  G().fillRoundRect(x + 8, y + 32, 140, 30, 5, theme::kPanelHi);
  txt(x + 18, y + 42, theme::kGold, 1, "Mode: %s", led::modeName(led::mode()));
  txt(x + 158, y + 42, theme::kInkDim, 1, "(tap to cycle)");

  // Color swatches
  txt(x + 8, y + 72, theme::kInkDim, 1, "Color:");
  for (int i = 0; i < led::kColorCount; i++) {
    uint32_t c = led::colorRgb(i);
    uint16_t col565 = G().color565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    int sx = x + 52 + i * 32;
    G().fillRoundRect(sx, y + 66, 26, 26, 4, col565);
    if (i == led::colorIdx())
      G().drawRoundRect(sx - 2, y + 64, 30, 30, 5, theme::kInk);
  }

  // LED brightness row
  txt(x + 8, y + 108, theme::kInk, 1, "Glow: %d%%", led::brightness());
  G().fillRoundRect(x + 150, y + 102, 30, 24, 4, theme::kPanelHi);
  txt(x + 160, y + 108, theme::kInk, 2, "-");
  G().fillRoundRect(x + 186, y + 102, 30, 24, 4, theme::kPanelHi);
  txt(x + 196, y + 108, theme::kInk, 2, "+");

  // Sound row
  G().fillRoundRect(x + 8, y + 140, 150, 30, 5,
                    app::sound() ? theme::kGood : theme::kLocked);
  txt(x + 18, y + 150, theme::kInk, 1, "Sound: %s (tap)",
      app::sound() ? "ON" : "off");

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
void seOpen() {}
void seTick(uint32_t) {}
void seClose() {}
void seDraw(int x, int y, int w, int h) {
  body(x, y, w, h);
  txt(x + 8, y + 6, theme::kInk, 2, "Settings Cabin");
  txt(x + 8, y + 32, theme::kInkDim, 1, "Captain: %s  (%s Lv %d)",
      game::profile.name, game::rankTitle(game::profile.level),
      game::profile.level);

  // Rename row
  G().fillRoundRect(x + 8, y + 48, 170, 28, 5, theme::kPanelHi);
  txt(x + 18, y + 57, theme::kInk, 1, "Rename yer pirate...");

  // Brightness row
  txt(x + 8, y + 90, theme::kInk, 1, "Brightness: %d%%", app::brightness());
  G().fillRoundRect(x + 150, y + 84, 30, 22, 4, theme::kPanelHi);
  txt(x + 160, y + 90, theme::kInk, 2, "-");
  G().fillRoundRect(x + 186, y + 84, 30, 22, 4, theme::kPanelHi);
  txt(x + 196, y + 90, theme::kInk, 2, "+");

  // Sound row
  G().fillRoundRect(x + 8, y + 116, 150, 28, 5,
                    app::sound() ? theme::kGood : theme::kLocked);
  txt(x + 18, y + 124, theme::kInk, 1, "Sound: %s (tap)",
      app::sound() ? "ON" : "off");

  // Reset row
  G().fillRoundRect(x + 8, y + 152, 180, 28, 5, theme::kBad);
  txt(x + 18, y + 160, theme::kInk, 1, "Reset progress (tap)");
  txt(x + 8, y + 188, theme::kInkDim, 1,
      "LED profile lives in the Signal Lantern.");
}
bool seTouch(int16_t x, int16_t y) {
  int ly = y - 44;  // to content-local
  if (ly >= 42 && ly <= 82 && x >= 8 && x <= 190) {
    app::requestRename();
    return false;  // main switches screens; no tool redraw needed
  }
  if (ly >= 84 && ly <= 112) {
    if (x >= 144 && x <= 182) app::setBrightness(app::brightness() - 10);
    else if (x >= 183 && x <= 222) app::setBrightness(app::brightness() + 10);
    return true;
  }
  if (ly >= 114 && ly <= 148 && x >= 8 && x <= 170) {
    app::setSound(!app::sound());
    return true;
  }
  if (ly >= 150 && ly <= 184 && x >= 8 && x <= 200) {
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
  txt(x + 8, y + 4, theme::kGold, 1, "Clients probing: %d  (ch %d)", pwCount,
      pwChannel);
  txt(x + 8, y + 16, theme::kInkDim, 1, "%lu probe frames heard passively",
      (unsigned long)pwTotal);
  int rows = min(pwCount, 13);
  for (int i = 0; i < rows; i++) {
    int ry = y + 32 + i * 13;
    rssiBars(x + 6, ry, pwList[i].rssi);
    if (pwList[i].ssid[0]) {
      String s = String(pwList[i].ssid);
      if (s.length() > 16) s = s.substring(0, 16);
      txt(x + 26, ry, theme::kInk, 1, "%s", s.c_str());
    } else {
      txt(x + 26, ry, theme::kInkDim, 1, "<broadcast>");
    }
    const char* ven = oui::vendorStr(String(pwList[i].mac));
    txt(x + 150, ry, theme::kSeaFoam, 1, "%.8s",
        ven[0] ? ven : pwList[i].mac + 9);
    txt(x + 250, ry, theme::kInkDim, 1, "x%u", pwList[i].hits);
  }
  if (pwCount == 0)
    txt(x + 8, y + 40, theme::kInkDim, 1, "Listening... hop the channels.");
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
  txt(x + 8, y + 4, theme::kGold, 1, "Decoding %d advertisers", g_bleCount);
  int rows = min(g_bleCount, 11);
  for (int i = 0; i < rows; i++) {
    int ry = y + 20 + i * 15;
    String label = g_ble[i].name.length() ? g_ble[i].name : g_ble[i].mac;
    if (label.length() > 20) label = label.substring(0, 20);
    txt(x + 8, ry, theme::kInk, 1, "%s", label.c_str());
    txt(x + 252, ry, theme::kInkDim, 1, "%ddB", g_ble[i].rssi);
    const char* co = bleCompanyName(g_ble[i].company);
    if (g_ble[i].detail[0])
      txt(x + 16, ry + 7, theme::kSeaFoam, 1, "%s", g_ble[i].detail);
    else if (co[0])
      txt(x + 16, ry + 7, theme::kSeaFoam, 1, "%s", co);
    else if (g_ble[i].company)
      txt(x + 16, ry + 7, theme::kInkDim, 1, "company 0x%04X",
          g_ble[i].company);
    else
      txt(x + 16, ry + 7, theme::kInkDim, 1, "no manufacturer data");
  }
  if (g_bleCount == 0)
    txt(x + 8, y + 20, theme::kInkDim, 1, "Sampling the air...");
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
  txt(x + 8, y + 6, theme::kInk, 2, "Ship's Instruments");
  int ry = y + 38;
  txt(x + 8, ry, theme::kGold, 1, "Core temp: %.1f C  /  %.0f F", siTemp,
      siTemp * 9.0f / 5.0f + 32.0f);
  ry += 22;
  uint32_t mv = analogReadMilliVolts(BATTERY_ADC) * 2;  // typical /2 divider
  int pct = ssBatteryPct();
  txt(x + 8, ry, theme::kGold, 1, "Battery: ~%d%%  (%lu mV)", pct,
      (unsigned long)mv);
  G().drawRoundRect(x + 8, ry + 14, 104, 12, 2, theme::kInk);
  G().fillRect(x + 10, ry + 16, pct, 8, pct < 20 ? theme::kBad : theme::kGood);
  ry += 44;
  txt(x + 8, ry, theme::kInkDim, 1, "Core temp is the on-die sensor -- it");
  ry += 12;
  txt(x + 8, ry, theme::kInkDim, 1, "reads warm and is approximate.");
  ry += 16;
  txt(x + 8, ry, theme::kInkDim, 1, "Add a BME280 on I2C for real air");
  ry += 12;
  txt(x + 8, ry, theme::kInkDim, 1, "temp/humidity/pressure. Mic: unwired.");
}
bool siTouch(int16_t, int16_t) { return false; }
}  // namespace

// ===========================================================================
//  Registry
// ===========================================================================
namespace tools {

lgfx::LGFXBase* gfx = nullptr;

static const Tool kTools[] = {
    {"Crow's Nest", "Wi-Fi survey", theme::kSail, cnOpen, cnTick, cnClose,
     cnDraw, cnTouch},
    {"Harbor Ledger", "BLE discovery", theme::kSeaFoam, hlOpen, hlTick, hlClose,
     hlDraw, hlTouch},
    {"Chart Room", "Wardrive log -> SD", theme::kWood, crOpen, crTick, crClose,
     crDraw, crTouch},
    {"Lookout", "Channel analyzer", theme::kGood, lkOpen, lkTick, lkClose,
     lkDraw, lkTouch},
    {"Spyglass", "Surveillance spotter", theme::kWarn, sgOpen, sgTick, sgClose,
     sgDraw, sgTouch},
    {"Tracker Watch", "Anti-stalk BLE", theme::kWarn, twOpen, twTick, twClose,
     twDraw, twTouch},
    {"Rigging Watch", "Deauth detector", theme::kBad, rwOpen, rwTick, rwClose,
     rwDraw, rwTouch},
    {"Hull Inspection", "Network audit", theme::kGold, hiOpen, hiTick, hiClose,
     hiDraw, hiTouch},
    {"Captain's Log", "SD file browser", theme::kSail, clOpen, clTick, clClose,
     clDraw, clTouch},
    {"Ship's Systems", "Diagnostics", theme::kSeaFoam, ssOpen, ssTick, ssClose,
     ssDraw, ssTouch},
    {"Signal Lantern", "RGB LED / FX", theme::kSun, slOpen, slTick, slClose,
     slDraw, slTouch},
    {"Probe Watch", "Client sniffer", theme::kSail, pwOpen, pwTick, pwClose,
     pwDraw, pwTouch},
    {"Deep BLE ID", "Adv decoder", theme::kSeaFoam, dbOpen, dbTick, dbClose,
     dbDraw, dbTouch},
    {"Instruments", "Onboard sensors", theme::kGold, siOpen, siTick, siClose,
     siDraw, siTouch},
    {"Settings Cabin", "Options", theme::kInkDim, seOpen, seTick, seClose,
     seDraw, seTouch},
};

int count() { return sizeof(kTools) / sizeof(kTools[0]); }
const Tool& at(int i) {
  if (i < 0) i = 0;
  if (i >= count()) i = count() - 1;
  return kTools[i];
}

}  // namespace tools
