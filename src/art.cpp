// NOTE: <FS.h>/<SD_MMC.h> must be included before LovyanGFX (pulled in by
// art.h) so LovyanGFX compiles its Arduino-FS DataWrapper<fs::SDMMCFS>
// specialization -- its guard keys off FS_H / _SDMMC_H_ being defined.
#include <FS.h>
#include <SD_MMC.h>

#include <string.h>

#include "art.h"

namespace art {
namespace {

// Parallax layer geometry (must match tools/build_artpack.py).
constexpr int kCloudsW = 480, kCloudsY = 18;
constexpr int kWavesW = 480, kWavesY = 150;

bool g_sd = false;
bool g_any = false;
bool g_sky = false, g_clouds = false, g_waves = false, g_deck = false;
bool g_ship[5] = {false};

// The sky is opaque and full-screen, so it is re-decoded on every world frame
// -- by far the heaviest layer. Decode it once into a PSRAM sprite at boot and
// blit that instead. (Transparent layers can't be cached this way: their PNG
// decode alpha-composites against the live canvas, which a pre-baked sprite
// would lose.)
LGFX_Sprite g_skyCache;
bool g_skyCached = false;

// The pixel art pack uses hard binary alpha (no partial transparency), so every
// other layer can ALSO be pre-decoded once into a PSRAM sprite and blitted with
// a transparent key color -- no SD/PNG work per frame. This keeps world redraws
// fast enough that touch polling between frames doesn't miss quick taps.
constexpr uint16_t kKey = 0xF81F;  // magenta: unused by the art palette

LGFX_Sprite g_cloudsSpr, g_wavesSpr, g_deckSpr, g_shipSpr;
bool g_cloudsOk = false, g_wavesOk = false, g_deckOk = false, g_shipOk = false;
int g_shipTierCached = -1;

LGFX_Sprite g_clipSpr[CLIP_COUNT];   // strips for the current avatar+tier
bool g_clipOk[CLIP_COUNT] = {false};
const void* g_clipSrc[CLIP_COUNT] = {nullptr};
int g_capCachedAvatar = -1, g_capCachedTier = -1;

bool cacheKeyed(LGFX_Sprite& s, bool& ok, const char* path, int w, int h) {
  if (ok) {
    s.deleteSprite();
    ok = false;
  }
  s.setColorDepth(16);
  s.setPsram(true);
  if (!s.createSprite(w, h)) return false;
  s.fillSprite(kKey);
  if (!s.drawPngFile(SD_MMC, path, 0, 0)) {
    s.deleteSprite();
    return false;
  }
  ok = true;
  return true;
}

void dropCaches() {
  if (g_skyCached) {
    g_skyCache.deleteSprite();
    g_skyCached = false;
  }
  auto drop = [](LGFX_Sprite& s, bool& ok) {
    if (ok) {
      s.deleteSprite();
      ok = false;
    }
  };
  drop(g_cloudsSpr, g_cloudsOk);
  drop(g_wavesSpr, g_wavesOk);
  drop(g_deckSpr, g_deckOk);
  drop(g_shipSpr, g_shipOk);
  g_shipTierCached = -1;
  for (int i = 0; i < CLIP_COUNT; i++) {
    drop(g_clipSpr[i], g_clipOk[i]);
    g_clipSrc[i] = nullptr;
  }
  g_capCachedAvatar = g_capCachedTier = -1;
}

struct CapClip {
  bool have = false;
  char file[48] = {0};
  int frames = 0, fw = 0, fh = 0;
};
CapClip g_cap[4][5][CLIP_COUNT];  // [avatar][tier][clip]

fs::FS& FS() { return SD_MMC; }

bool blit(lgfx::LGFXBase& g, const char* path, int x, int y) {
  return g.drawPngFile(FS(), path, x, y);
}

// Draw one WxH frame from a horizontal strip: offset into the source by
// frame*fw, clip the output to fw x fh.
bool blitFrame(lgfx::LGFXBase& g, const char* path, int x, int y, int fw,
               int fh, int frame) {
  return g.drawPngFile(FS(), path, x, y, fw, fh, frame * fw, 0);
}

// Draw a wide layer scrolled horizontally, wrapping so it tiles seamlessly.
bool scrollLayer(lgfx::LGFXBase& g, const char* path, int y, int imgW,
                 int scrollX) {
  int ox = -(scrollX % imgW);
  bool ok = g.drawPngFile(FS(), path, ox, y);
  g.drawPngFile(FS(), path, ox + imgW, y);
  return ok;
}

}  // namespace

void begin(bool sdReady) {
  g_sd = sdReady;
  g_any = g_sky = g_clouds = g_waves = g_deck = false;
  dropCaches();
  for (int i = 0; i < 5; i++) g_ship[i] = false;
  for (int a = 0; a < 4; a++)
    for (int t = 0; t < 5; t++)
      for (int c = 0; c < CLIP_COUNT; c++) g_cap[a][t][c] = CapClip{};
  if (!g_sd) return;

  File dir = SD_MMC.open("/art");
  if (!dir || !dir.isDirectory()) return;

  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String full = String(f.name());
    int slash = full.lastIndexOf('/');
    String name = slash >= 0 ? full.substring(slash + 1) : full;
    f.close();
    const char* n = name.c_str();

    if (name.equalsIgnoreCase("sky.png")) { g_sky = true; continue; }
    if (name.equalsIgnoreCase("clouds.png")) { g_clouds = true; continue; }
    if (name.equalsIgnoreCase("waves.png")) { g_waves = true; continue; }
    if (name.equalsIgnoreCase("deck.png")) { g_deck = true; continue; }

    int t = -1;
    if (sscanf(n, "ship_t%d.png", &t) == 1 && t >= 0 && t < 5) {
      g_ship[t] = true;
      continue;
    }

    int a = -1, F = 0, W = 0, H = 0;
    char clip[10] = {0};
    // cap<N>_t<T>_<clip>_f<F>_w<W>_h<H>.png
    if (sscanf(n, "cap%d_t%d_%9[a-z]_f%d_w%d_h%d.png", &a, &t, clip, &F, &W,
               &H) == 6 &&
        a >= 0 && a < 4 && t >= 0 && t < 5 && F > 0 && W > 0 && H > 0) {
      int c;
      if (strcmp(clip, "idle") == 0) c = IDLE;
      else if (strcmp(clip, "wave") == 0) c = WAVE;
      else if (strcmp(clip, "fight") == 0) c = FIGHT;
      else if (strcmp(clip, "jig") == 0) c = JIG;
      else if (strcmp(clip, "look") == 0) c = LOOK;
      else continue;  // unknown clip name: ignore the file
      CapClip& cc = g_cap[a][t][c];
      cc.have = true;
      cc.frames = F;
      cc.fw = W;
      cc.fh = H;
      snprintf(cc.file, sizeof(cc.file), "/art/%s", n);
    }
  }
  dir.close();

  // Pre-bake the opaque sky into a PSRAM sprite so the per-frame world redraw
  // blits it instead of decoding sky.png from SD every time. Safe only because
  // the sky is fully opaque; if the decode or allocation fails we quietly fall
  // back to per-frame decoding in drawSky().
  if (g_sky) {
    g_skyCache.setColorDepth(16);
    g_skyCache.setPsram(true);
    if (g_skyCache.createSprite(320, 240)) {
      if (g_skyCache.drawPngFile(FS(), "/art/sky.png", 0, 0)) {
        g_skyCached = true;
      } else {
        g_skyCache.deleteSprite();
      }
    }
  }
  // Transparent layers: keyed PSRAM sprites (see kKey note above).
  if (g_clouds) cacheKeyed(g_cloudsSpr, g_cloudsOk, "/art/clouds.png", 480, 100);
  if (g_waves) cacheKeyed(g_wavesSpr, g_wavesOk, "/art/waves.png", 480, 70);
  if (g_deck) cacheKeyed(g_deckSpr, g_deckOk, "/art/deck.png", 320, 90);

  g_any = g_sky || g_clouds || g_waves || g_deck;
  for (int i = 0; i < 5; i++) g_any |= g_ship[i];
  for (int aa = 0; aa < 4 && !g_any; aa++)
    for (int tt = 0; tt < 5 && !g_any; tt++)
      if (g_cap[aa][tt][IDLE].have || g_cap[aa][tt][WAVE].have) g_any = true;
}

bool available() { return g_any; }

bool drawSky(lgfx::LGFXBase& g) {
  if (!g_sd || !g_sky) return false;
  if (g_skyCached) {
    g_skyCache.pushSprite(static_cast<lgfx::LovyanGFX*>(&g), 0, 0);
    return true;
  }
  return blit(g, "/art/sky.png", 0, 0);
}
bool drawClouds(lgfx::LGFXBase& g, int scrollX) {
  if (!g_sd || !g_clouds) return false;
  if (g_cloudsOk) {
    int ox = -(scrollX % kCloudsW);
    auto* dst = static_cast<lgfx::LovyanGFX*>(&g);
    g_cloudsSpr.pushSprite(dst, ox, kCloudsY, kKey);
    g_cloudsSpr.pushSprite(dst, ox + kCloudsW, kCloudsY, kKey);
    return true;
  }
  return scrollLayer(g, "/art/clouds.png", kCloudsY, kCloudsW, scrollX);
}
bool drawWaves(lgfx::LGFXBase& g, int scrollX) {
  if (!g_sd || !g_waves) return false;
  if (g_wavesOk) {
    int ox = -(scrollX % kWavesW);
    auto* dst = static_cast<lgfx::LovyanGFX*>(&g);
    g_wavesSpr.pushSprite(dst, ox, kWavesY, kKey);
    g_wavesSpr.pushSprite(dst, ox + kWavesW, kWavesY, kKey);
    return true;
  }
  return scrollLayer(g, "/art/waves.png", kWavesY, kWavesW, scrollX);
}
bool drawDeck(lgfx::LGFXBase& g) {
  if (!g_sd || !g_deck) return false;
  if (g_deckOk) {
    g_deckSpr.pushSprite(static_cast<lgfx::LovyanGFX*>(&g), 0, 240 - 90, kKey);
    return true;
  }
  return blit(g, "/art/deck.png", 0, 240 - 90);
}

bool haveShip(int tier) {
  if (tier < 0) tier = 0;
  if (tier > 4) tier = 4;
  for (int t = tier; t >= 0; t--)
    if (g_ship[t]) return true;
  return false;
}
bool drawShip(lgfx::LGFXBase& g, int tier, int dstX, int dstY) {
  if (!g_sd) return false;
  if (tier < 0) tier = 0;
  if (tier > 4) tier = 4;
  char path[24];
  for (int t = tier; t >= 0; t--) {
    if (g_ship[t]) {
      snprintf(path, sizeof(path), "/art/ship_t%d.png", t);
      if (!g_shipOk || g_shipTierCached != t) {
        if (cacheKeyed(g_shipSpr, g_shipOk, path, 150, 130)) g_shipTierCached = t;
      }
      if (g_shipOk) {
        g_shipSpr.pushSprite(static_cast<lgfx::LovyanGFX*>(&g), dstX, dstY, kKey);
        return true;
      }
      return blit(g, path, dstX, dstY);
    }
  }
  return false;
}

// Resolve a captain clip, stepping down tiers then falling back to idle/wave.
namespace {
const CapClip* resolveClip(int avatar, int tier, int clip) {
  if (avatar < 0 || avatar > 3) return nullptr;
  if (clip < 0 || clip >= CLIP_COUNT) clip = IDLE;
  if (tier < 0) tier = 0;
  if (tier > 4) tier = 4;
  for (int t = tier; t >= 0; t--) {
    if (g_cap[avatar][t][clip].have) return &g_cap[avatar][t][clip];
    if (g_cap[avatar][t][IDLE].have) return &g_cap[avatar][t][IDLE];
    if (g_cap[avatar][t][WAVE].have) return &g_cap[avatar][t][WAVE];
  }
  return nullptr;
}
}  // namespace

bool haveCaptain(int avatar, int tier) {
  return resolveClip(avatar, tier, IDLE) != nullptr;
}
int frameCount(int avatar, int tier, int clip) {
  if (avatar < 0 || avatar > 3) return 0;
  if (tier < 0) tier = 0;
  if (tier > 4) tier = 4;
  for (int t = tier; t >= 0; t--)
    if (g_cap[avatar][t][clip].have) return g_cap[avatar][t][clip].frames;
  return 0;
}
int frameW(int avatar, int tier) {
  const CapClip* c = resolveClip(avatar, tier, IDLE);
  return c ? c->fw : 0;
}
int frameH(int avatar, int tier) {
  const CapClip* c = resolveClip(avatar, tier, IDLE);
  return c ? c->fh : 0;
}

bool drawCaptainFrame(lgfx::LGFXBase& g, int avatar, int tier, int clip,
                      int frame, int dstX, int dstY) {
  if (!g_sd) return false;
  const CapClip* c = resolveClip(avatar, tier, clip);
  if (!c) return false;
  if (frame < 0) frame = 0;
  if (frame >= c->frames) frame %= c->frames;

  // Cache the strips of the current avatar+tier in PSRAM; a change of either
  // invalidates all slots (a strip is ~230 KB, five clips ~1.1 MB of PSRAM).
  if (avatar != g_capCachedAvatar || tier != g_capCachedTier) {
    for (int i = 0; i < CLIP_COUNT; i++) {
      if (g_clipOk[i]) g_clipSpr[i].deleteSprite();
      g_clipOk[i] = false;
      g_clipSrc[i] = nullptr;
    }
    g_capCachedAvatar = avatar;
    g_capCachedTier = tier;
  }
  int slot = (clip >= 0 && clip < CLIP_COUNT) ? clip : IDLE;
  if (!g_clipOk[slot] || g_clipSrc[slot] != c) {
    if (cacheKeyed(g_clipSpr[slot], g_clipOk[slot], c->file,
                   c->fw * c->frames, c->fh))
      g_clipSrc[slot] = c;
  }
  if (g_clipOk[slot]) {
    // Blit one frame out of the strip via a clip rect on the destination.
    g.setClipRect(dstX, dstY, c->fw, c->fh);
    g_clipSpr[slot].pushSprite(static_cast<lgfx::LovyanGFX*>(&g),
                               dstX - frame * c->fw, dstY, kKey);
    g.clearClipRect();
    return true;
  }
  return blitFrame(g, c->file, dstX, dstY, c->fw, c->fh, frame);
}

}  // namespace art
