#pragma once

#include <LovyanGFX.hpp>

// SD art-pack: illustrated, animated overrides for the world and captains.
// Generate art with tools/build_artpack.py (see docs/ART_PACK.md), copy the
// output to the card's /art/ folder, and the firmware decodes and animates it
// on-device. Everything is optional -- anything missing falls back to the
// built-in vector renderer, so a partial pack is fine.
//
// Layers (all authored transparent unless noted):
//   /art/sky.png                     320x240 opaque backdrop (sky + sea)
//   /art/clouds.png                  480x100 parallax, scrolls + wraps
//   /art/waves.png                   480x70  parallax, scrolls faster
//   /art/deck.png                    320x90  static foreground
//   /art/ship_t<T>.png               150x130 ship for tier T
//   /art/cap<N>_t<T>_<clip>_f<F>_w<W>_h<H>.png
//                                    captain sprite strip: F frames, each WxH,
//                                    clip in {idle, wave, fight, jig, look}
namespace art {

enum Clip { IDLE = 0, WAVE, FIGHT, JIG, LOOK, CLIP_COUNT };

// Scan the SD card for available art. Safe to call with sd=false (no-op).
void begin(bool sdReady);

// True if any art files were found (world uses this to switch to the art path).
bool available();

// ---- background layers: each returns true if it drew from an art file -------
bool drawSky(lgfx::LGFXBase& g);
bool drawClouds(lgfx::LGFXBase& g, int scrollX);
bool drawWaves(lgfx::LGFXBase& g, int scrollX);
bool drawDeck(lgfx::LGFXBase& g);

// ---- ship: drawn with its top-left at (dstX, dstY) --------------------------
bool haveShip(int tier);
bool drawShip(lgfx::LGFXBase& g, int tier, int dstX, int dstY);

// ---- captain sprite ---------------------------------------------------------
bool haveCaptain(int avatar, int tier);           // any clip present
int frameCount(int avatar, int tier, int clip);   // 0 if the clip is absent
int frameW(int avatar, int tier);
int frameH(int avatar, int tier);
// Blit one frame with its top-left at (dstX, dstY). Returns true if drawn.
bool drawCaptainFrame(lgfx::LGFXBase& g, int avatar, int tier, int clip,
                      int frame, int dstX, int dstY);

}  // namespace art
