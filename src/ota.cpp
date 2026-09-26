#include "ota.h"

#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

namespace ota {
namespace {

char g_ssid[33] = "";
char g_pass[65] = "";
char g_url[192] = "";
char g_status[48] = "idle";
int g_progress = -1;

void load() {
  Preferences p;
  p.begin("ota", true);
  String s = p.getString("ssid", "");
  String pw = p.getString("pass", "");
  String u = p.getString("url", "");
  p.end();
  strncpy(g_ssid, s.c_str(), sizeof(g_ssid) - 1);
  strncpy(g_pass, pw.c_str(), sizeof(g_pass) - 1);
  strncpy(g_url, u.c_str(), sizeof(g_url) - 1);
}

void save() {
  Preferences p;
  p.begin("ota", false);
  p.putString("ssid", g_ssid);
  p.putString("pass", g_pass);
  p.putString("url", g_url);
  p.end();
}

void setStatus(const char* s) {
  strncpy(g_status, s, sizeof(g_status) - 1);
  g_status[sizeof(g_status) - 1] = 0;
}

void onProgress(int cur, int total) {
  if (total > 0) g_progress = (int)((cur * 100LL) / total);
  else g_progress = -1;
}

bool isHttps(const char* url) {
  return strncmp(url, "https://", 8) == 0;
}

}  // namespace

void begin() {
  load();
  setStatus("idle");
  g_progress = -1;
}

bool wifiConfigured() { return g_ssid[0] != 0; }
bool urlConfigured() { return g_url[0] != 0; }
const char* wifiSsid() { return g_ssid; }
const char* url() { return g_url; }
const char* status() { return g_status; }
int progressPct() { return g_progress; }

void setWifi(const char* ssid, const char* pass) {
  strncpy(g_ssid, ssid ? ssid : "", sizeof(g_ssid) - 1);
  g_ssid[sizeof(g_ssid) - 1] = 0;
  strncpy(g_pass, pass ? pass : "", sizeof(g_pass) - 1);
  g_pass[sizeof(g_pass) - 1] = 0;
  save();
}

void setUrl(const char* url) {
  strncpy(g_url, url ? url : "", sizeof(g_url) - 1);
  g_url[sizeof(g_url) - 1] = 0;
  save();
}

bool runUpdate() {
  g_progress = 0;
  if (!wifiConfigured()) {
    setStatus("no WiFi cfg");
    return false;
  }
  if (!urlConfigured()) {
    setStatus("no OTA URL");
    return false;
  }

  setStatus("wifi…");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(50);
  WiFi.begin(g_ssid, g_pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) {
    setStatus("wifi fail");
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    g_progress = -1;
    return false;
  }

  setStatus("updating…");
  // HTTPUpdate expects an *application* image, not a merged factory bin.
  httpUpdate.rebootOnUpdate(true);
  httpUpdate.onProgress(onProgress);

  t_httpUpdate_return ret;
  if (isHttps(g_url)) {
    WiFiClientSecure client;
    client.setInsecure();  // release CDN certs vary; prefer HTTPS transport
    ret = httpUpdate.update(client, g_url);
  } else {
    WiFiClient client;
    ret = httpUpdate.update(client, g_url);
  }

  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  g_progress = -1;

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      snprintf(g_status, sizeof(g_status), "fail:%s",
               httpUpdate.getLastErrorString().c_str());
      g_status[sizeof(g_status) - 1] = 0;
      return false;
    case HTTP_UPDATE_NO_UPDATES:
      setStatus("no update");
      return false;
    case HTTP_UPDATE_OK:
      setStatus("ok reboot");
      return true;
  }
  setStatus("unknown");
  return false;
}

}  // namespace ota
