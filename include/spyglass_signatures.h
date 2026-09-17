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
//      (https://deflock.me , https://github.com/frillip/DeFlock and the
//      community wardriving datasets it links). Left empty on purpose below.
//
//   2. ssidSubstr -- a case-insensitive SSID substring. This is a *heuristic*,
//      not proof: it matches any network whose broadcast name literally
//      contains the word. Label such entries "(heuristic)" so the UI never
//      asserts an identification -- it only raises a candidate for the user to
//      confirm (the Spyglass screen shows the real SSID + BSSID next to it).
//
// The keyword heuristics below are plain brand/category words, not invented
// hardware identifiers. They will produce false positives (a home "PoolCam"
// NVR, a shop's "CCTV-DVR") -- that is expected and disclosed on-screen.
namespace spyglass {

struct Signature {
  const char* label;      // what it is, shown to the user
  const char* ouiPrefix;  // upper-case "AA:BB:CC" MAC prefix, or "" to skip
  const char* ssidSubstr; // case-insensitive SSID substring, or "" to skip
};

constexpr Signature kSignatures[] = {
    // --- Verified MAC prefixes: add from DeFlock here (none shipped) ---------
    //   { "Flock Safety ALPR", "AA:BB:CC", "" },   // <- only with a real OUI

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
