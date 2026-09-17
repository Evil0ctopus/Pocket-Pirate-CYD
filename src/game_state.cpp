#include "game_state.h"

#include <Preferences.h>

namespace game {

Profile profile;

namespace {
Preferences store;

// Smooth, slightly-accelerating XP curve. Level 10 (~4500 xp) is the max.
constexpr uint32_t kLevelXp[kMaxLevel + 1] = {
    0, 100, 250, 460, 740, 1100, 1550, 2100, 2800, 3600, 4500};
}  // namespace

uint32_t xpForLevel(uint8_t level) {
  if (level > kMaxLevel) level = kMaxLevel;
  return kLevelXp[level];
}

uint32_t xpToNext() {
  if (profile.level >= kMaxLevel) return 0;
  uint32_t next = kLevelXp[profile.level + 1];
  return profile.xp >= next ? 0 : next - profile.xp;
}

uint8_t levelProgressPct() {
  if (profile.level >= kMaxLevel) return 100;
  uint32_t base = kLevelXp[profile.level];
  uint32_t next = kLevelXp[profile.level + 1];
  if (next <= base) return 100;
  uint32_t span = next - base;
  uint32_t have = profile.xp > base ? profile.xp - base : 0;
  if (have >= span) return 100;
  return static_cast<uint8_t>((have * 100) / span);
}

bool awardXp(uint32_t amount) {
  profile.xp += amount;
  bool leveled = false;
  while (profile.level < kMaxLevel &&
         profile.xp >= kLevelXp[profile.level + 1]) {
    profile.level++;
    leveled = true;
    addLoot(Loot::Doubloon, 5);  // level-up bounty
  }
  return leveled;
}

void addLoot(Loot type, uint32_t count) {
  int i = static_cast<int>(type);
  if (i < 0 || i >= static_cast<int>(Loot::Count)) return;
  profile.loot[i] += count;
}

const char* lootName(Loot type) {
  switch (type) {
    case Loot::ChartFragment: return "Chart Fragment";
    case Loot::MessageBottle: return "Message Bottle";
    case Loot::Doubloon:      return "Doubloon";
    case Loot::Cargo:         return "Cargo";
    default:                  return "?";
  }
}

const char* rankTitle(uint8_t level) {
  if (level >= 10) return "Dread Captain";
  if (level >= 7)  return "Captain";
  if (level >= 4)  return "First Mate";
  if (level >= 1)  return "Deckhand";
  return "Cabin Kid";
}

const char* shipName(uint8_t level) {
  if (level >= 8) return "War Galleon";
  if (level >= 4) return "Deckhand's Sloop";
  if (level >= 1) return "Patched Raft";
  return "Driftwood Raft";
}

void load() {
  store.begin("pirate", true);
  profile.avatar = store.getUChar("avatar", 0);
  profile.level = store.getUChar("level", 0);
  profile.xp = store.getUInt("xp", 0);
  profile.apSeen = store.getUInt("ap", 0);
  profile.bleSeen = store.getUInt("ble", 0);
  size_t n = store.getString("name", profile.name, sizeof(profile.name));
  if (n == 0) strncpy(profile.name, "Mara Tide", sizeof(profile.name) - 1);
  for (int i = 0; i < static_cast<int>(Loot::Count); i++) {
    char key[8];
    snprintf(key, sizeof(key), "loot%d", i);
    profile.loot[i] = store.getUInt(key, 0);
  }
  store.end();
  if (profile.level > kMaxLevel) profile.level = kMaxLevel;
}

void save() {
  store.begin("pirate", false);
  store.putUChar("avatar", profile.avatar);
  store.putUChar("level", profile.level);
  store.putUInt("xp", profile.xp);
  store.putUInt("ap", profile.apSeen);
  store.putUInt("ble", profile.bleSeen);
  store.putString("name", profile.name);
  for (int i = 0; i < static_cast<int>(Loot::Count); i++) {
    char key[8];
    snprintf(key, sizeof(key), "loot%d", i);
    store.putUInt(key, profile.loot[i]);
  }
  store.end();
}

void resetProgress() {
  profile.level = 0;
  profile.xp = 0;
  profile.apSeen = 0;
  profile.bleSeen = 0;
  for (int i = 0; i < static_cast<int>(Loot::Count); i++) profile.loot[i] = 0;
  save();
}

}  // namespace game
