#pragma once

#include <Arduino.h>

// Compact MAC vendor (OUI) lookup. This is a HAND-CURATED SUBSET of the public
// IEEE MA-L registry (https://standards-oui.ieee.org/) -- common consumer and
// IoT vendors only, not exhaustive. Labels are best-effort identification of
// publicly-broadcast hardware addresses; nothing here is fabricated. Extend the
// table from the official IEEE list as needed.
//
// Randomized / locally-administered MACs (bit 0x02 of the first octet) are
// reported as "random" -- modern phones rotate these, so a blank/"random"
// vendor is expected and correct, not a lookup failure.
namespace oui {

struct Entry {
  uint32_t p;      // first 24 bits of the MAC (the OUI)
  const char* v;   // short vendor label
};

inline constexpr Entry kOui[] = {
    // Espressif (ESP32/ESP8266)
    {0x240AC4, "Espressif"}, {0x246F28, "Espressif"}, {0x30AEA4, "Espressif"},
    {0x7C9EBD, "Espressif"}, {0x84CCA8, "Espressif"}, {0x84F3EB, "Espressif"},
    {0xA4CF12, "Espressif"}, {0xB4E62D, "Espressif"}, {0xC44F33, "Espressif"},
    {0xECFABC, "Espressif"},
    // Raspberry Pi
    {0xB827EB, "Raspberry Pi"}, {0xDCA632, "Raspberry Pi"},
    {0xE45F01, "Raspberry Pi"}, {0x28CDC1, "Raspberry Pi"},
    {0x2CCF67, "Raspberry Pi"}, {0xD83ADD, "Raspberry Pi"},
    // Apple
    {0x001B63, "Apple"}, {0x001EC2, "Apple"}, {0x002312, "Apple"},
    {0x3C15C2, "Apple"}, {0x40CBC0, "Apple"}, {0x68ABBC, "Apple"},
    {0xA483E7, "Apple"}, {0xACBC32, "Apple"}, {0xF01898, "Apple"},
    {0xD0817A, "Apple"}, {0x7CD1C3, "Apple"}, {0x8863DF, "Apple"},
    {0xC82A14, "Apple"}, {0x60FACD, "Apple"},
    // Samsung
    {0x001632, "Samsung"}, {0x3423BA, "Samsung"}, {0x5C0A5B, "Samsung"},
    {0x8C71F8, "Samsung"}, {0xB407F9, "Samsung"}, {0xE8508B, "Samsung"},
    // Google / Nest
    {0x3C5AB4, "Google"}, {0x94EB2C, "Google"}, {0xF4F5D8, "Google"},
    {0x001A11, "Google"}, {0xD86C63, "Google"}, {0x546009, "Google"},
    {0x6466B3, "Google"}, {0x18B430, "Nest"}, {0x641666, "Nest"},
    // Amazon
    {0x44650D, "Amazon"}, {0x6837E9, "Amazon"}, {0xF0272D, "Amazon"},
    {0x34D270, "Amazon"}, {0x747548, "Amazon"}, {0xFC65DE, "Amazon"},
    {0x0C47C9, "Amazon"}, {0x40B4CD, "Amazon"}, {0x84D6D0, "Amazon"},
    {0xB47C9C, "Amazon"},
    // TP-Link
    {0x50C7BF, "TP-Link"}, {0x6032B1, "TP-Link"}, {0x98DAC4, "TP-Link"},
    {0xEC086B, "TP-Link"}, {0xA42BB0, "TP-Link"}, {0x14CC20, "TP-Link"},
    {0x0C8063, "TP-Link"}, {0x5C63BF, "TP-Link"}, {0x1C61B4, "TP-Link"},
    // Netgear
    {0x001B2F, "Netgear"}, {0x20E52A, "Netgear"}, {0xA040A0, "Netgear"},
    {0x4494FC, "Netgear"}, {0xC03F0E, "Netgear"}, {0x9C3DCF, "Netgear"},
    {0x2CB05D, "Netgear"},
    // Ubiquiti
    {0x0418D6, "Ubiquiti"}, {0x24A43C, "Ubiquiti"}, {0x788A20, "Ubiquiti"},
    {0xFCECDA, "Ubiquiti"}, {0xDC9FDB, "Ubiquiti"}, {0xB4FBE4, "Ubiquiti"},
    {0x68D79A, "Ubiquiti"}, {0x802AA8, "Ubiquiti"}, {0x74ACB9, "Ubiquiti"},
    {0x44D9E7, "Ubiquiti"},
    // Intel
    {0x001E64, "Intel"}, {0x3CA9F4, "Intel"}, {0x5CE0C5, "Intel"},
    {0x94659C, "Intel"}, {0xA08869, "Intel"}, {0x3413E8, "Intel"},
    {0x0C8BFD, "Intel"}, {0x7CB27D, "Intel"}, {0xE4B318, "Intel"},
    // Microsoft
    {0x00125A, "Microsoft"}, {0x00155D, "Microsoft"}, {0x281878, "Microsoft"},
    {0x7C1E52, "Microsoft"}, {0x985FD3, "Microsoft"}, {0x502F9B, "Microsoft"},
    // Sonos
    {0x000E58, "Sonos"}, {0x347E5C, "Sonos"}, {0x48A6B8, "Sonos"},
    {0x5CAAFD, "Sonos"}, {0x949F3E, "Sonos"}, {0xB8E937, "Sonos"},
    {0x542A1B, "Sonos"}, {0x78282D, "Sonos"},
    // Cisco / Meraki
    {0x00180A, "Meraki"}, {0xE0553D, "Meraki"}, {0x881544, "Meraki"},
    {0xAC17C8, "Meraki"},
    // Roku
    {0xB0A737, "Roku"}, {0xCC6DA0, "Roku"}, {0xD8318C, "Roku"},
    {0xDC3A5E, "Roku"},
    // Texas Instruments (common BLE)
    {0x00124B, "TI"}, {0x98072D, "TI"}, {0xA0E6F8, "TI"}, {0x2C6B7D, "TI"},
    // Xiaomi
    {0x286C07, "Xiaomi"}, {0x64B473, "Xiaomi"}, {0xF8A45F, "Xiaomi"},
};

inline constexpr int kOuiCount = sizeof(kOui) / sizeof(kOui[0]);

// Look up by the three OUI octets. Returns "" if unknown, "random" if the
// address is locally-administered (randomized).
inline const char* vendor(uint8_t b0, uint8_t b1, uint8_t b2) {
  if (b0 & 0x02) return "random";
  uint32_t p = ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | b2;
  for (int i = 0; i < kOuiCount; i++)
    if (kOui[i].p == p) return kOui[i].v;
  return "";
}

inline const char* vendor(const uint8_t mac[6]) {
  return vendor(mac[0], mac[1], mac[2]);
}

// Parse a "AA:BB:CC:DD:EE:FF" string (e.g. a BLE address) and look it up.
inline const char* vendorStr(const String& mac) {
  if (mac.length() < 8) return "";
  auto hx = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  int h0 = hx(mac[0]), l0 = hx(mac[1]);
  int h1 = hx(mac[3]), l1 = hx(mac[4]);
  int h2 = hx(mac[6]), l2 = hx(mac[7]);
  if ((h0 | l0 | h1 | l1 | h2 | l2) < 0) return "";
  return vendor((h0 << 4) | l0, (h1 << 4) | l1, (h2 << 4) | l2);
}

}  // namespace oui
