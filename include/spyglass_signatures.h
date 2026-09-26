#pragma once

#include <Arduino.h>

// Signature table for the Spyglass station: RF fingerprints of fixed
// surveillance cameras (Flock Safety ALPR and similar) so their *presence* can
// be flagged from public broadcast metadata. Detection only -- never access.
//
// IMPORTANT -- two kinds of entries, do not confuse them:
//
//   1. ouiPrefix  -- a MAC address vendor prefix ("AA:BB:CC"). This is a
//      strong, specific identifier. DO NOT invent these. Fabricated prefixes
//      create false "camera detected" hits, which defeats the whole point.
//      Populate them only from a source you trust, e.g. the DeFlock project
//      (https://deflock.me) and the community wardriving datasets it links.
//
//   2. ssidSubstr -- a case-insensitive SSID substring. This is a *heuristic*,
//      not proof: it matches any network whose broadcast name literally
//      contains the word. Label such entries "(heuristic)" so the UI never
//      asserts an identification -- it only raises a candidate for the user to
//      confirm (the Spyglass screen shows the real SSID + BSSID next to it).
//
// Verified OUI prefixes below come from @NitekryDPaul's field research
// (DeflockJoplin/flock-you datasets) plus DeFlock Joplin's 31st prefix.
// They are *field-observed Flock Safety Wi-Fi transmitters*, not IEEE
// registrant names for "Flock Safety". Several OEM module OUIs appear in the
// wild; we deliberately omit Espressif generics (A4:CF:12, 3C:71:BF) that
// would false-positive every ESP32 AP.
namespace spyglass {

struct Signature {
  const char* label;      // what it is, shown to the user
  const char* ouiPrefix;  // upper-case "AA:BB:CC" MAC prefix, or "" to skip
  const char* ssidSubstr; // case-insensitive SSID substring, or "" to skip
};

constexpr Signature kSignatures[] = {
    // --- Verified MAC prefixes (DeFlock / @NitekryDPaul field list) ----------
    {"Flock Safety (field OUI)", "70:C9:4E", ""},
    {"Flock Safety (field OUI)", "3C:91:80", ""},
    {"Flock Safety (field OUI)", "D8:F3:BC", ""},
    {"Flock Safety (field OUI)", "80:30:49", ""},
    {"Flock Safety (field OUI)", "B8:35:32", ""},
    {"Flock Safety (field OUI)", "14:5A:FC", ""},
    {"Flock Safety (field OUI)", "74:4C:A1", ""},
    {"Flock Safety (field OUI)", "08:3A:88", ""},
    {"Flock Safety (field OUI)", "9C:2F:9D", ""},
    {"Flock Safety (field OUI)", "C0:35:32", ""},
    {"Flock Safety (field OUI)", "94:08:53", ""},
    {"Flock Safety (field OUI)", "E4:AA:EA", ""},
    {"Flock Safety (field OUI)", "F4:6A:DD", ""},
    {"Flock Safety (field OUI)", "F8:A2:D6", ""},
    {"Flock Safety (field OUI)", "24:B2:B9", ""},
    {"Flock Safety (field OUI)", "00:F4:8D", ""},
    {"Flock Safety (field OUI)", "D0:39:57", ""},
    {"Flock Safety (field OUI)", "E8:D0:FC", ""},
    {"Flock Safety (field OUI)", "E0:4F:43", ""},
    {"Flock Safety (field OUI)", "B8:1E:A4", ""},
    {"Flock Safety (field OUI)", "70:08:94", ""},
    {"Flock Safety (field OUI)", "58:8E:81", ""},
    {"Flock Safety (field OUI)", "EC:1B:BD", ""},
    {"Flock Safety (field OUI)", "58:00:E3", ""},
    {"Flock Safety (field OUI)", "90:35:EA", ""},
    {"Flock Safety (field OUI)", "5C:93:A2", ""},
    {"Flock Safety (field OUI)", "64:6E:69", ""},
    {"Flock Safety (field OUI)", "48:27:EA", ""},
    // DeFlock Joplin field add (wildcard-probe camera)
    {"Flock Safety (DeFlock JO)", "82:6B:F2", ""},

    // --- Keyword heuristics (candidates to verify, not confirmations) --------
    {"Flock ALPR? (heuristic)",       "", "flock"},
    {"Vigilant/Motorola LPR? (heur)", "", "vigilant"},
    {"License-plate reader? (heur)",  "", "alpr"},
    {"License-plate reader? (heur)",  "", "lpr-"},
    {"Surveillance cam? (heuristic)", "", "surveil"},
    {"Camera/NVR? (heuristic)",       "", "cctv"},
    {"Camera/NVR? (heuristic)",       "", "-cam"},
    {"Camera/NVR? (heuristic)",       "", "ipcam"},
};

constexpr int kSignatureCount =
    sizeof(kSignatures) / sizeof(kSignatures[0]);

// True once at least one real MAC-prefix signature has been added, so the UI
// can distinguish "armed with verified signatures" from "keyword heuristics
// only". Kept simple: scan the table for any non-empty ouiPrefix at runtime.
inline bool hasVerifiedSignature() {
  for (int i = 0; i < kSignatureCount; i++) {
    if (kSignatures[i].ouiPrefix && kSignatures[i].ouiPrefix[0]) return true;
  }
  return false;
}

}  // namespace spyglass
