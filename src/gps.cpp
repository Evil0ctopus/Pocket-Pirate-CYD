#include "gps.h"

#include <TinyGPSPlus.h>
#include <HardwareSerial.h>

#include "board_pins.h"

using namespace CheapBlackDisplay;

namespace {

HardwareSerial GpsSerial(1);
TinyGPSPlus parser;
bool g_ready = false;
uint32_t g_lastCharMs = 0;

}  // namespace

namespace gps {

void begin() {
  GpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  g_ready = true;
  g_lastCharMs = 0;
}

void tick(uint32_t nowMs) {
  if (!g_ready) return;
  while (GpsSerial.available() > 0) {
    char c = (char)GpsSerial.read();
    parser.encode(c);
    g_lastCharMs = nowMs;
  }
  (void)nowMs;
}

bool available() { return g_ready; }

bool hasFix() {
  return parser.location.isValid() && parser.location.age() < 5000;
}

double latitude() { return hasFix() ? parser.location.lat() : 0.0; }
double longitude() { return hasFix() ? parser.location.lng() : 0.0; }
double altitudeM() {
  return parser.altitude.isValid() ? parser.altitude.meters() : 0.0;
}
double hdop() { return parser.hdop.isValid() ? parser.hdop.hdop() : 0.0; }
uint32_t satellites() {
  return parser.satellites.isValid() ? parser.satellites.value() : 0;
}
uint32_t ageMs() {
  if (!parser.location.isValid()) return UINT32_MAX;
  return parser.location.age();
}

bool formatTimestamp(char* out, size_t n) {
  if (!out || n < 20) return false;
  if (!parser.date.isValid() || !parser.time.isValid()) return false;
  // TinyGPSPlus date/time are UTC from NMEA.
  snprintf(out, n, "%04u-%02u-%02u %02u:%02u:%02u",
           parser.date.year(), parser.date.month(), parser.date.day(),
           parser.time.hour(), parser.time.minute(), parser.time.second());
  return true;
}

const char* statusLabel() {
  static char buf[16];
  if (!g_ready) return "NO GPS";
  // No characters for a while => module likely absent.
  if (g_lastCharMs == 0 || (millis() - g_lastCharMs) > 5000) return "NO GPS";
  if (!hasFix()) {
    snprintf(buf, sizeof(buf), "NO FIX");
    return buf;
  }
  snprintf(buf, sizeof(buf), "FIX %lu", (unsigned long)satellites());
  return buf;
}

}  // namespace gps
