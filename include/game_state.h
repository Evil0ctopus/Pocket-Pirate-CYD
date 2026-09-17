#pragma once

#include <Arduino.h>

// Central persistent game state for the pirate ecosystem: profile, progression
// and loot totals. Everything here is backed by NVS so it survives resets.
namespace game {

// Loot categories. All are derived from passively-observable, broadcast data or
// legitimate device activity -- no credential material is ever a loot source.
enum class Loot : uint8_t {
  ChartFragment = 0,  // a Wi-Fi AP discovered (SSID/BSSID/channel metadata)
  MessageBottle,      // a BLE advertiser seen nearby
  Doubloon,           // currency awarded for milestones
  Cargo,              // SD backups / logs / completed maintenance
  Count
};

constexpr uint8_t kMaxLevel = 10;

struct Profile {
  char name[20] = "Mara Tide";
  uint8_t avatar = 0;             // selected captain index
  uint8_t level = 0;              // 0..kMaxLevel
  uint32_t xp = 0;                // total lifetime xp
  uint32_t loot[static_cast<int>(Loot::Count)] = {0, 0, 0, 0};
  uint32_t apSeen = 0;            // lifetime unique APs charted
  uint32_t bleSeen = 0;          // lifetime unique BLE devices logged
};

extern Profile profile;

// XP required to *reach* the given level (index 0..kMaxLevel).
uint32_t xpForLevel(uint8_t level);
// XP still needed to reach the next level; 0 at max level.
uint32_t xpToNext();
// Progress toward next level as 0..100.
uint8_t levelProgressPct();

void load();
void save();
void resetProgress();

// Add xp and loot; returns true if a level-up occurred (so the world can react).
bool awardXp(uint32_t amount);
void addLoot(Loot type, uint32_t count = 1);

const char* lootName(Loot type);
// A short cosmetic rank title that changes as the pirate evolves.
const char* rankTitle(uint8_t level);
// The ship the pirate sails at this level (raft -> sloop -> galleon).
const char* shipName(uint8_t level);

}  // namespace game
