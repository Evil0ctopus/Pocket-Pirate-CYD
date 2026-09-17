#pragma once

#include <LovyanGFX.hpp>

// Built-in vector chibi renderer: big-eyed, shaded cartoon pirates drawn from
// smooth primitives so no art assets are required. The SD art-pack (art.h) can
// override these with photoreal PNGs when present.
namespace chibi {

// Draw a captain bust/figure with its head centered on (cx, cy).
// avatar = index into pc::kCaptains, tier = 0..4 (see pc::tierOf).
void drawCaptain(lgfx::LGFXBase& g, int cx, int cy, int avatar, int tier);

// A little sailing ship centered on (cx, cy), growing with tier.
void drawShip(lgfx::LGFXBase& g, int cx, int cy, int tier);

}  // namespace chibi
