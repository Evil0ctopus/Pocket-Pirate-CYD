#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>

// Tool framework for the Pocket Pirate. Each "station" is a Tool: it can run
// passive background work (onTick), draw its own panel (onDraw) and handle taps
// (onTouch). Tools never draw the world directly -- they report progress and
// hand loot/xp to game state, and the world reacts.
namespace tools {

// The active canvas, provided by main so tools can render into their panel.
// Typed as the LovyanGFX base so it can point at either the panel or an
// off-screen sprite (main composites everything into a PSRAM sprite).
extern lgfx::LGFXBase* gfx;

// Transient banner shown over the world (e.g. "New network charted!").
void toast(const char* fmt, ...);

struct Tool {
  const char* title;     // pirate-flavored station name
  const char* subtitle;  // plain description of what it actually does
  uint16_t accent;       // theme accent color for the tile/header
  void (*onOpen)();
  void (*onTick)(uint32_t nowMs);
  void (*onClose)();
  // Draw content within the given region (below the shared header bar).
  void (*onDraw)(int x, int y, int w, int h);
  // Handle a tap inside the content region; return true if consumed.
  bool (*onTouch)(int16_t x, int16_t y);
};

int count();
const Tool& at(int i);

}  // namespace tools
