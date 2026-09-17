#pragma once

#include <Arduino.h>

#include "gfx_util.h"

// The captain roster. Each captain is a distinct chibi pirate rendered by the
// chibi module (or overridden by SD art-pack PNGs). Colors are authored here so
// the four read as a crew, not clones.
namespace pc {

using gfxu::rgb;

enum CaptainFlags : uint8_t {
  kBeard = 1 << 0,
  kFemale = 1 << 1,
  kPatch = 1 << 2,
};

struct Captain {
  const char* name;
  const char* trait;
  uint16_t skin, skinSh, skinHi;  // skin base / shadow / highlight
  uint16_t hair;                  // hair + beard color
  uint16_t coat, coatSh, trim;    // coat base / shadow / gold trim
  uint16_t hat, hatSh;            // tricorn base / shadow
  uint16_t bandana;               // headscarf color (0 = none)
  uint16_t eyes;                  // iris color
  uint8_t flags;
};

// C++17 inline variable: one definition shared across translation units.
inline constexpr Captain kCaptains[] = {
    // 0 - Mara Tide: navigator, teal coat, navy tricorn, no beard.
    {"Mara Tide", "Navigator",
     rgb(240, 197, 160), rgb(198, 150, 120), rgb(255, 227, 200),
     rgb(90, 55, 35),
     rgb(28, 122, 120), rgb(16, 82, 82), rgb(240, 198, 70),
     rgb(30, 42, 78), rgb(18, 26, 52),
     0, rgb(60, 120, 170), kFemale},

    // 1 - Roan Redsail: gunner, red coat, ginger beard, red bandana.
    {"Roan Redsail", "Gunner",
     rgb(232, 180, 145), rgb(188, 135, 105), rgb(255, 215, 185),
     rgb(178, 92, 40),
     rgb(176, 46, 40), rgb(120, 28, 26), rgb(240, 198, 70),
     rgb(34, 30, 30), rgb(18, 16, 16),
     rgb(190, 52, 46), rgb(90, 60, 40), kBeard},

    // 2 - Iyla Dawn: lookout, blue coat, blonde, purple bandana.
    {"Iyla Dawn", "Lookout",
     rgb(246, 210, 178), rgb(206, 165, 135), rgb(255, 235, 212),
     rgb(214, 176, 96),
     rgb(40, 78, 150), rgb(26, 52, 104), rgb(240, 198, 70),
     rgb(120, 70, 160), rgb(80, 44, 110),
     rgb(150, 92, 190), rgb(70, 150, 120), kFemale},

    // 3 - Bram Kettle: quartermaster, green coat, grey beard, eyepatch.
    {"Bram Kettle", "Quartermaster",
     rgb(204, 156, 120), rgb(160, 116, 86), rgb(236, 196, 162),
     rgb(196, 196, 196),
     rgb(46, 104, 60), rgb(28, 70, 40), rgb(240, 198, 70),
     rgb(74, 54, 36), rgb(48, 34, 22),
     0, rgb(120, 96, 70), (uint8_t)(kBeard | kPatch)},
};

inline constexpr int kCaptainCount =
    sizeof(kCaptains) / sizeof(kCaptains[0]);

// Level (0..10) -> visual tier (0..4). Tier gates hat/coat/accessory detail.
inline int tierOf(int level) {
  if (level >= 10) return 4;
  if (level <= 0) return 0;
  int t = level / 3 + 1;  // 1..3 for levels 1..8, 4 for 9
  return t > 4 ? 4 : t;
}

}  // namespace pc
