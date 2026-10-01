#pragma once

#include <LovyanGFX.hpp>

// Built-in pixel-art fallback for pirates and ships when SD sprites are absent.
namespace chibi {

// Draw a captain bust/figure with its head centered on (cx, cy).
// avatar = index into pc::kCaptains, tier = 0..4 (see pc::tierOf).
// motion matches art::Clip (idle, wave, fight, jig, look); frame is 0..2.
void drawCaptain(lgfx::LGFXBase& g, int cx, int cy, int avatar, int tier,
				 int motion = 0, int frame = 0);

// A little sailing ship centered on (cx, cy), growing with tier.
void drawShip(lgfx::LGFXBase& g, int cx, int cy, int tier, int frame = 0);

}  // namespace chibi
