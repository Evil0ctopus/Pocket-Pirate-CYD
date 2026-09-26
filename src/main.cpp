#include <Arduino.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <LovyanGFX.hpp>

#include <stdarg.h>

#include "app.h"
#include "art.h"
#include "audio.h"
#include "board_pins.h"
#include "captains.h"
#include "chibi.h"
#include "game_state.h"
#include "gps.h"
#include "gfx_util.h"
#include "led.h"
#include "ota.h"
#include "pirate_theme.h"
#include "power.h"
#include "tools.h"
#include "touch.h"

using namespace CheapBlackDisplay;

#ifndef PP_VERSION
#define PP_VERSION "0.4.1"
#endif

using gfxu::blend565;
using gfxu::darken;
using gfxu::lighten;
using gfxu::rgb;

// ===========================================================================
//  Display panel (ILI9341 over SPI)
// ===========================================================================
class CheapBlackDisplayPanel : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI _bus;

 public:
  CheapBlackDisplayPanel() {
    auto bus_config = _bus.config();
    bus_config.spi_host = SPI2_HOST;
    bus_config.spi_mode = 0;
    bus_config.freq_write = 40000000;
    bus_config.freq_read = 16000000;
    bus_config.pin_sclk = TFT_SCK;
    bus_config.pin_mosi = TFT_MOSI;
    bus_config.pin_miso = TFT_MISO;
    bus_config.pin_dc = TFT_DC;
    _bus.config(bus_config);
    _panel.setBus(&_bus);

    auto panel_config = _panel.config();
    panel_config.pin_cs = TFT_CS;
    panel_config.pin_rst = -1;
    panel_config.pin_busy = -1;
    panel_config.memory_width = 240;
    panel_config.memory_height = 320;
    panel_config.panel_width = 240;
    panel_config.panel_height = 320;
    panel_config.readable = true;
    // This Hosyond 2.8" panel's controller runs inverted gamma: without INVON
    // (invert=true) every color displays as its negative.
    panel_config.invert = true;
    panel_config.rgb_order = false;
    panel_config.dlen_16bit = false;
    panel_config.bus_shared = true;
    _panel.config(panel_config);
    setPanel(&_panel);
  }
};

CheapBlackDisplayPanel display;

// Off-screen 320x240 canvas in PSRAM: everything renders here, then is pushed
// to the panel in one shot (flicker-free, allows smooth layering).
LGFX_Sprite canvas(&display);
bool g_canvasOk = false;

static inline void present() {
  if (g_canvasOk) canvas.pushSprite(0, 0);
}

// ===========================================================================
//  App services (backlight / sound / SD) shared with tool modules
// ===========================================================================
namespace {
Preferences settings;
uint8_t g_brightness = 90;
bool g_sound = true;
bool g_sdReady = false;
bool g_renamePending = false;  // set by app::requestRename(), consumed in loop
}  // namespace

namespace app {
void setBrightness(uint8_t pct) {
  if (pct < 5) pct = 5;
  if (pct > 100) pct = 100;
  g_brightness = pct;
  ledcWrite(TFT_BL, (uint32_t)pct * 255 / 100);
  settings.begin("set", false);
  settings.putUChar("bri", g_brightness);
  settings.end();
}
uint8_t brightness() { return g_brightness; }
void setSound(bool on) {
  g_sound = on;
  settings.begin("set", false);
  settings.putBool("snd", g_sound);
  settings.end();
}
bool sound() { return g_sound; }
bool sdReady() { return g_sdReady; }
void requestRename() { g_renamePending = true; }
}  // namespace app

// ===========================================================================
//  Toast overlay + gfx binding used by tool modules
// ===========================================================================
namespace {
char g_toast[48] = "";
uint32_t g_toastUntil = 0;
}  // namespace

namespace tools {
void toast(const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vsnprintf(g_toast, sizeof(g_toast), fmt, a);
  va_end(a);
  g_toastUntil = millis() + 2200;
}
}  // namespace tools

namespace {

// ===========================================================================
//  UI state
// ===========================================================================
enum class Screen { Select, World, Menu, Tool, Name };
Screen g_screen = Screen::World;
int g_toolIndex = -1;
int g_menuPage = 0;  // paginated stations grid
bool g_menuDirty = true;  // menu is static — only redraw when dirty / toast
constexpr int kMenuPerPage = 9;  // 3 cols x 3 rows
int g_sel = 0;  // captain carousel index on the select screen

// ---- pirate-name keyboard state -------------------------------------------
char g_nameBuf[20] = {0};
bool g_nameFromSelect = false;  // true: first-boot flow; false: settings rename
uint32_t g_lastDraw = 0;
uint8_t g_anim = 0;
uint32_t g_frameMs = 0;    // duration of the last world redraw (perf telemetry)
bool g_seaView = false;    // world page: false = captain on deck, true = ship at sea
int g_actClip = -1;        // one-shot flourish clip now playing (-1 = idle)
uint32_t g_actStart = 0;   // millis when the flourish began
uint32_t g_nextActAt = 0;  // when the captain next does something on their own

// ---------------------------------------------------------------------------
//  Speech bubbles (PorkChop-style pirate quips)
// ---------------------------------------------------------------------------
const char* const kSayings[] = {
    "Arrr!",
    "Land ho!",
    "Where be me rum?",
    "Yo ho ho!",
    "Shiver me timbers!",
    "A fine day to sail!",
    "X marks the spot",
    "All hands on deck!",
    "Avast, ye scallywag!",
    "Hoist the colours!",
    "Steady as she goes",
    "Gold! I smell gold!",
    "The sea calls...",
    "Batten the hatches!",
    "Heave ho!",
    "Keep a weather eye",
    "Wind's pickin' up",
    "Treasure awaits!",
    "Dead men tell no tales",
    "Mind the boom!",
    "Crackers for the parrot",
    "Not all treasure be gold",
};
constexpr int kSayingCount = sizeof(kSayings) / sizeof(kSayings[0]);
char g_say[32] = {0};
uint32_t g_sayUntil = 0;

void sayRandom() {
  strncpy(g_say, kSayings[esp_random() % kSayingCount], sizeof(g_say) - 1);
  g_say[sizeof(g_say) - 1] = 0;
  g_sayUntil = millis() + 2800;
}

// Pixel-style speech bubble above the captain, with a stepped tail.
void drawSpeechBubble(uint32_t now) {
  if (now > g_sayUntil || g_say[0] == 0) return;
  uint16_t paper = rgb(250, 250, 244), ink = rgb(24, 18, 28);
  int w = (int)strlen(g_say) * 6 + 16;
  if (w > 300) w = 300;
  int x = 160 - w / 2;
  if (x + w > 206) x = 206 - w;  // keep clear of the DECK/SHIP tabs
  if (x < 4) x = 4;
  // Sit below the status strip so the bubble is never clipped.
  int y = theme::kHeaderH + theme::kStatusH + 6;
  canvas.fillSmoothRoundRect(x, y, w, 22, 6, paper);
  canvas.drawRoundRect(x, y, w, 22, 6, ink);
  // soft triangular tail
  canvas.fillTriangle(154, y + 21, 166, y + 21, 160, y + 30, paper);
  canvas.setTextColor(ink);
  canvas.setTextSize(1);
  canvas.setCursor(x + 8, y + 8);
  canvas.print(g_say);
}

// ---------------------------------------------------------------------------
//  World background (vector fallback when no /art/world.png)
// ---------------------------------------------------------------------------
void softCloud(int x, int y, int s) {
  uint16_t w = rgb(250, 252, 255);
  canvas.fillCircle(x, y, s, w);
  canvas.fillCircle(x + s, y + 2, s - 2, w);
  canvas.fillCircle(x - s, y + 3, s - 3, w);
  canvas.fillCircle(x, y + 4, s + 1, w);
}

void drawWorldBg() {
  const int horizon = 150;
  // step=2: half the scanline work, still reads as a soft sky/sea blend
  gfxu::vGradient(canvas, 0, 0, 320, horizon, rgb(120, 196, 236),
                  rgb(236, 226, 188), 2);
  int sx = 268, sy = 52;
  canvas.fillCircle(sx, sy, 28, blend565(rgb(255, 244, 190), rgb(236, 226, 188), 160));
  canvas.fillCircle(sx, sy, 16, rgb(255, 236, 150));
  softCloud(70, 44, 9);
  softCloud(180, 34, 7);

  gfxu::vGradient(canvas, 0, horizon, 320, 240 - horizon, rgb(46, 154, 176),
                  rgb(18, 92, 138), 2);
  for (int row = 0; row < 5; row++) {
    int y = horizon + 12 + row * 16;
    int off = ((g_anim + row) % 4) * 6;
    for (int x = -20 + off; x < 330; x += 40)
      canvas.fillRoundRect(x, y, 18, 4, 2, rgb(150, 208, 222));
  }
}

// ---------------------------------------------------------------------------
//  HUD
// ---------------------------------------------------------------------------
void coin(int x, int y, int r) {
  canvas.fillCircle(x, y, r, theme::kGold);
  canvas.fillCircle(x - r / 3, y - r / 3, max(1, r / 3), lighten(theme::kGold, 80));
  canvas.drawCircle(x, y, r, darken(theme::kGold, 60));
}

void drawHeaderBar() {
  // Flat fill + thin accents (smooth gradients every frame were a lag source).
  const int hh = theme::kHeaderH;
  canvas.fillRect(0, 0, theme::kScreenW, hh, theme::kBgDeep);
  canvas.fillRect(0, 0, theme::kScreenW, 3, theme::kPanel);
  canvas.drawFastHLine(0, hh - 1, theme::kScreenW, theme::kGold);
  canvas.drawFastHLine(0, hh, theme::kScreenW, theme::kTeal);

  // avatar chip
  canvas.fillCircle(18, hh / 2, 12, theme::kPanelSoft);
  canvas.drawCircle(18, hh / 2, 12, theme::kBorderHi);
  coin(18, hh / 2, 7);

  canvas.setTextColor(theme::kInk);
  canvas.setTextSize(1);
  canvas.setCursor(36, 4);
  canvas.printf("%s", game::profile.name);
  canvas.setTextColor(theme::kInkDim);
  canvas.setCursor(36, 16);
  canvas.printf("%s Lv%u", game::rankTitle(game::profile.level),
                game::profile.level);

  // xp bar
  int bx = 148, bw = 118, by = 6;
  canvas.fillRoundRect(bx, by, bw, 10, 4, theme::kBg);
  canvas.drawRoundRect(bx, by, bw, 10, 4, theme::kBorder);
  int fill = game::levelProgressPct() * (bw - 4) / 100;
  if (fill > 0) canvas.fillRoundRect(bx + 2, by + 2, fill, 6, 3, theme::kGold);
  canvas.setTextColor(theme::kInkMuted);
  canvas.setCursor(bx, 20);
  if (game::profile.level >= game::kMaxLevel)
    canvas.print("MAX RANK");
  else
    canvas.printf("%lu xp next", (unsigned long)game::xpToNext());

  // doubloons — keep inside the right edge
  coin(306, 10, 6);
  canvas.setTextColor(theme::kGold);
  canvas.setCursor(288, 20);
  canvas.printf("%lu",
                (unsigned long)game::profile.loot[(int)game::Loot::Doubloon]);
}

void drawToastOverlay() {
  if (millis() > g_toastUntil || g_toast[0] == 0) return;
  int ty = theme::kContentBottom - 30;
  canvas.fillRoundRect(20, ty, 280, 26, 6, theme::kBgDeep);
  canvas.drawRoundRect(20, ty, 280, 26, 6, theme::kGold);
  canvas.setTextColor(theme::kInk);
  canvas.setTextSize(1);
  canvas.setCursor(32, ty + 9);
  canvas.print(g_toast);
}

// Persistent world HUD strip: power / SD / brightness / GPS.
void drawStatusStrip() {
  const int ty = theme::kHeaderH + 2;
  canvas.fillRoundRect(2, ty, 198, theme::kStatusH, 3, theme::kBgDeep);
  canvas.drawRoundRect(2, ty, 198, theme::kStatusH, 3, theme::kBorder);
  canvas.setTextSize(1);
  uint16_t pc = power::lowBattery() ? theme::kBad
                                    : (power::usbPowered() ? theme::kGood
                                                           : theme::kInkDim);
  int textY = ty + 2;
  canvas.setTextColor(pc);
  canvas.setCursor(6, textY);
  canvas.printf("%s", power::powerLabel());
  canvas.setTextColor(app::sdReady() ? theme::kGood : theme::kInkMuted);
  canvas.setCursor(40, textY);
  canvas.print(app::sdReady() ? "SD" : "--");
  canvas.setTextColor(theme::kInkDim);
  canvas.setCursor(62, textY);
  canvas.printf("B%d", (int)app::brightness());
  bool fix = gps::hasFix();
  canvas.setTextColor(fix ? theme::kGood : theme::kInkMuted);
  canvas.setCursor(92, textY);
  canvas.printf("GPS:%s", gps::statusLabel());
}

// DECK / SHIP tabs — edge-aligned, labels centered so words always fit.
// Hit-tested taller than drawn (resistive touch scatter ~15px vertical).
void drawViewTabs() {
  const char* names[2] = {"DECK", "SHIP"};
  const int tw = 54, th = 20, gap = 2;
  const int tx0 = theme::kScreenW - theme::kPad - (tw * 2 + gap);  // 206
  const int ty = theme::kHeaderH + 2;
  for (int i = 0; i < 2; i++) {
    int tx = tx0 + i * (tw + gap);
    bool on = (g_seaView == (i == 1));
    canvas.fillRoundRect(tx, ty, tw, th, 5,
                         on ? theme::kGold : theme::kPanelSoft);
    if (!on) canvas.drawRoundRect(tx, ty, tw, th, 5, theme::kBorder);
    gfxu::printCentered(canvas, tx, ty, tw, th,
                        on ? theme::kOnGold : theme::kInk, 1, names[i]);
  }
}

void drawWorld() {
  uint32_t now = millis();
  int avatar = game::profile.avatar;
  int tier = pc::tierOf(game::profile.level);

  // --- background: art sky (cached blit) or vector fallback ---------------
  // Skipping drawWorldBg when sky art is present avoids a full-screen
  // per-scanline gradient every frame (major lag source on CYD).
  if (!art::drawSky(canvas)) drawWorldBg();
  art::drawClouds(canvas, (int)(now / 70));
  art::drawWaves(canvas, (int)(now / 28));

  int bob = (int)((now / 400) % 2) * 3;

  if (g_seaView) {
    // --- SHIP page: the vessel alone on open water, centered on the horizon
    if (!art::drawShip(canvas, tier, 85, 83 + bob))
      chibi::drawShip(canvas, 160, 150 + bob, tier);
    drawHeaderBar();
    canvas.setTextColor(theme::kInk);
    canvas.setTextSize(1);
    canvas.setCursor(6, theme::kHeaderH + 4);
    canvas.print(game::shipName(game::profile.level));
  } else {
    // --- DECK page: the captain on deck; no ship on this page --------------
    if (art::haveCaptain(avatar, tier)) {
      int clip = art::IDLE, frame = 0;
      if (g_actClip >= 0) {  // a one-shot flourish is playing (two loops)
        int fc = art::frameCount(avatar, tier, g_actClip);
        int f = (int)((now - g_actStart) / 150);
        if (fc > 0 && f < fc * 2) {
          clip = g_actClip;
          frame = f % fc;
        } else {
          g_actClip = -1;
          g_nextActAt = now + 5000 + (esp_random() % 9000);
        }
      }
      if (g_actClip < 0 && now >= g_nextActAt) {
        // the captain does something on their own: wave/fight/jig/look
        const int acts[4] = {art::WAVE, art::FIGHT, art::JIG, art::LOOK};
        int have[4], nh = 0;
        for (int i = 0; i < 4; i++)
          if (art::frameCount(avatar, tier, acts[i]) > 0) have[nh++] = acts[i];
        if (nh > 0) {
          g_actClip = have[esp_random() % nh];
          g_actStart = now;
          clip = g_actClip;
          frame = 0;
          if (esp_random() % 10 < 6) sayRandom();
        } else {
          g_nextActAt = now + 8000;
        }
      }
      if (clip == art::IDLE) {
        int fc = art::frameCount(avatar, tier, art::IDLE);
        if (fc < 1) fc = 1;
        frame = (int)((now / 140) % fc);
      }
      int fw = art::frameW(avatar, tier), fh = art::frameH(avatar, tier);
      art::drawCaptainFrame(canvas, avatar, tier, clip, frame, 160 - fw / 2,
                            theme::kContentBottom - fh);
    } else {
      chibi::drawCaptain(canvas, 128, 96 + bob, avatar, tier);
    }

    // --- foreground deck (drawn over the captain's feet) --------------------
    art::drawDeck(canvas);
    drawHeaderBar();
    drawSpeechBubble(now);
  }

  drawViewTabs();
  drawStatusStrip();

  // bottom bar: loot + Stations — edge-to-edge, slim chrome
  const int by = theme::kContentBottom;
  canvas.fillRect(0, by, theme::kScreenW, theme::kBottomBarH, theme::kBgDeep);
  canvas.drawFastHLine(0, by, theme::kScreenW, theme::kBorder);
  canvas.setTextColor(theme::kInkDim);
  canvas.setTextSize(1);
  canvas.setCursor(4, by + 9);
  canvas.printf("Charts %lu  Bottles %lu  Cargo %lu",
                (unsigned long)game::profile.loot[(int)game::Loot::ChartFragment],
                (unsigned long)game::profile.loot[(int)game::Loot::MessageBottle],
                (unsigned long)game::profile.loot[(int)game::Loot::Cargo]);
  canvas.fillRoundRect(228, by + 2, 88, theme::kBottomBarH - 4, 6, theme::kGold);
  gfxu::printCentered(canvas, 228, by + 2, 88, theme::kBottomBarH - 4,
                      theme::kOnGold, 1, "STATIONS");
}

// ---------------------------------------------------------------------------
//  Captain select (full-size carousel)
// ---------------------------------------------------------------------------
void drawArrow(int cx, int cy, bool right) {
  canvas.fillSmoothCircle(cx, cy, 16, theme::kPanelSoft);
  canvas.drawCircle(cx, cy, 16, theme::kGold);
  int d = right ? 5 : -5;
  canvas.fillTriangle(cx - d, cy - 8, cx - d, cy + 8, cx + d, cy, theme::kInk);
}

void drawSelect() {
  gfxu::vGradient(canvas, 0, 0, 320, 240, theme::kBg, theme::kBgDeep);
  canvas.setTextColor(theme::kGold);
  canvas.setTextSize(2);
  canvas.setCursor(64, 8);
  canvas.print("Choose Captain");

  const pc::Captain& c = pc::kCaptains[g_sel];
  // spotlight
  canvas.fillSmoothCircle(160, 118, 74, theme::kPanel);
  if (art::haveCaptain(g_sel, 2)) {
    int fc = art::frameCount(g_sel, 2, art::IDLE);
    if (fc < 1) fc = 1;
    int frame = (int)((millis() / 160) % fc);
    int fw = art::frameW(g_sel, 2), fh = art::frameH(g_sel, 2);
    art::drawCaptainFrame(canvas, g_sel, 2, art::IDLE, frame, 160 - fw / 2,
                          192 - fh);
  } else {
    chibi::drawCaptain(canvas, 160, 104, g_sel, 2);
  }

  drawArrow(24, 120, false);
  drawArrow(296, 120, true);

  canvas.setTextColor(theme::kInk);
  canvas.setTextSize(2);
  int nameW = strlen(c.name) * 12;
  canvas.setCursor(160 - nameW / 2, 176);
  canvas.print(c.name);
  canvas.setTextColor(theme::kInkDim);
  canvas.setTextSize(1);
  int trW = strlen(c.trait) * 6;
  canvas.setCursor(160 - trW / 2, 196);
  canvas.print(c.trait);

  canvas.fillSmoothRoundRect(112, 210, 96, 26, 7, theme::kGold);
  canvas.setTextColor(theme::kOnGold);
  canvas.setCursor(130, 218);
  canvas.print("SET SAIL");

  // dots
  for (int i = 0; i < pc::kCaptainCount; i++)
    canvas.fillSmoothCircle(148 + i * 8, 2, 2,
                            i == g_sel ? theme::kGold : theme::kLocked);
}

// ---------------------------------------------------------------------------
//  Pirate-name keyboard (first boot after SET SAIL, or Settings rename)
// ---------------------------------------------------------------------------
const char* kKeyRows[4] = {"ABCDEFG", "HIJKLMN", "OPQRSTU", "VWXYZ.-"};
constexpr int kNameMax = 16;

void drawName() {
  gfxu::vGradient(canvas, 0, 0, 320, 240, theme::kBg, theme::kBgDeep);
  canvas.setTextColor(theme::kGold);
  canvas.setTextSize(2);
  canvas.setCursor(66, 8);
  canvas.print("Name yer pirate");

  canvas.fillSmoothRoundRect(20, 30, 280, 28, 6, theme::kBgDeep);
  canvas.drawRoundRect(20, 30, 280, 28, 6, theme::kGold);
  canvas.setTextColor(theme::kInk);
  canvas.setCursor(30, 37);
  canvas.print(g_nameBuf);
  int len = (int)strlen(g_nameBuf);
  if ((millis() / 400) % 2 && len < kNameMax)
    canvas.fillRect(30 + len * 12, 36, 10, 16, theme::kGold);

  for (int r = 0; r < 4; r++) {
    int ry = 66 + r * 34;
    for (int c = 0; c < 7; c++) {
      int cx = 6 + c * 45;
      canvas.fillSmoothRoundRect(cx, ry, 43, 30, 5, theme::kPanel);
      canvas.setTextColor(theme::kInk);
      canvas.setTextSize(2);
      canvas.setCursor(cx + 16, ry + 8);
      canvas.printf("%c", kKeyRows[r][c]);
    }
  }
  canvas.fillSmoothRoundRect(6, 204, 130, 32, 5, theme::kPanel);
  canvas.setTextColor(theme::kInkDim);
  canvas.setCursor(41, 213);
  canvas.print("SPACE");
  canvas.fillSmoothRoundRect(142, 204, 80, 32, 5, theme::kBad);
  canvas.setTextColor(theme::kInk);
  canvas.setCursor(164, 213);
  canvas.print("DEL");
  canvas.fillSmoothRoundRect(228, 204, 86, 32, 5, theme::kGold);
  canvas.setTextColor(theme::kOnGold);
  canvas.setCursor(242, 213);
  canvas.print("DONE");
}

void nameCommit() {
  int len = (int)strlen(g_nameBuf);
  while (len > 0 && g_nameBuf[len - 1] == ' ') g_nameBuf[--len] = 0;
  if (len == 0)  // empty: fall back to the roster name
    strncpy(g_nameBuf, pc::kCaptains[game::profile.avatar].name,
            sizeof(g_nameBuf) - 1);
  strncpy(game::profile.name, g_nameBuf, sizeof(game::profile.name) - 1);
  game::profile.name[sizeof(game::profile.name) - 1] = 0;
  game::save();
  g_screen = Screen::World;
}

// ---------------------------------------------------------------------------
//  Stations menu
// ---------------------------------------------------------------------------
void drawMenu() {
  canvas.fillRect(0, 0, theme::kScreenW, theme::kScreenH, theme::kBgDeep);
  drawHeaderBar();

  int total = tools::count();
  int pages = (total + kMenuPerPage - 1) / kMenuPerPage;
  if (pages < 1) pages = 1;
  if (g_menuPage >= pages) g_menuPage = 0;
  int base = g_menuPage * kMenuPerPage;

  const int stripY = theme::kHeaderH + 2;
  canvas.fillRoundRect(2, stripY, theme::kScreenW - 4, 14, 3, theme::kPanelSoft);
  canvas.setTextColor(theme::kInkDim);
  canvas.setTextSize(1);
  canvas.setCursor(8, stripY + 3);
  canvas.print("Choose a station");
  canvas.setTextColor(theme::kInkMuted);
  canvas.setCursor(theme::kScreenW - 36, stripY + 3);
  canvas.printf("%d/%d", g_menuPage + 1, pages);

  // Edge-to-edge 3×3 grid. Short `tile` labels + fit print so words never clip.
  const int cols = 3, gap = 3;
  const int x0 = 2;
  const int y0 = theme::kHeaderH + 20;
  const int tileW = (theme::kScreenW - x0 * 2 - gap * (cols - 1)) / cols;  // 104
  const int rows = 3;
  const int bottomY = theme::kContentBottom;
  const int tileH = (bottomY - 4 - y0 - gap * (rows - 1)) / rows;  // ~48
  for (int li = 0; li < kMenuPerPage; li++) {
    int i = base + li;
    if (i >= total) break;
    int col = li % cols, row = li / cols;
    int tx = x0 + col * (tileW + gap);
    int ty = y0 + row * (tileH + gap);
    const tools::Tool& t = tools::at(i);
    canvas.fillRoundRect(tx, ty, tileW, tileH, 7, theme::kPanel);
    canvas.drawRoundRect(tx, ty, tileW, tileH, 7, theme::kBorder);
    canvas.fillRect(tx, ty + 2, 3, tileH - 4, t.accent);
    gfxu::drawGlyph(canvas, tx + 16, ty + tileH / 2, 10, i, t.accent);
    // Short tile label only — full title/subtitle live in the tool header.
    const char* label = (t.tile && t.tile[0]) ? t.tile : t.title;
    gfxu::printCentered(canvas, tx + 28, ty, tileW - 32, tileH, theme::kInk, 1,
                        label);
  }

  // bottom bar: back + pager, full width
  const int by = theme::kContentBottom;
  canvas.fillRect(0, by, theme::kScreenW, theme::kBottomBarH, theme::kBgDeep);
  canvas.drawFastHLine(0, by, theme::kScreenW, theme::kBorder);
  canvas.fillRoundRect(4, by + 2, 72, theme::kBottomBarH - 4, 6, theme::kGold);
  gfxu::printCentered(canvas, 4, by + 2, 72, theme::kBottomBarH - 4,
                      theme::kOnGold, 1, "< DECK");
  if (pages > 1) {
    canvas.fillRoundRect(148, by + 2, 36, theme::kBottomBarH - 4, 6,
                         theme::kPanelHi);
    gfxu::printCentered(canvas, 148, by + 2, 36, theme::kBottomBarH - 4,
                        theme::kInk, 1, "<");
    canvas.setTextColor(theme::kInkDim);
    canvas.setCursor(192, by + 9);
    canvas.printf("%d/%d", g_menuPage + 1, pages);
    canvas.fillRoundRect(236, by + 2, 36, theme::kBottomBarH - 4, 6,
                         theme::kPanelHi);
    gfxu::printCentered(canvas, 236, by + 2, 36, theme::kBottomBarH - 4,
                        theme::kInk, 1, ">");
  }
}

void drawToolHeader() {
  const tools::Tool& t = tools::at(g_toolIndex);
  const int hh = theme::kToolHeaderH;
  canvas.fillRect(0, 0, theme::kScreenW, hh, theme::kBgDeep);
  canvas.fillRect(0, 0, 4, hh, t.accent);
  canvas.drawFastHLine(0, hh - 1, theme::kScreenW, theme::kBorder);
  canvas.fillRoundRect(8, 6, 52, hh - 12, 6, theme::kPanelSoft);
  canvas.drawRoundRect(8, 6, 52, hh - 12, 6, theme::kBorder);
  gfxu::printCentered(canvas, 8, 6, 52, hh - 12, theme::kInk, 1, "< Back");
  gfxu::drawGlyph(canvas, 78, hh / 2, 9, g_toolIndex, t.accent);
  gfxu::printFit(canvas, 92, 6, theme::kScreenW - 98, theme::kInk, 2, t.title);
  gfxu::printFit(canvas, 92, 24, theme::kScreenW - 98, theme::kInkDim, 1,
                 t.subtitle);
}

// ---------------------------------------------------------------------------
//  Touch routing
// ---------------------------------------------------------------------------
void handleTap(int16_t x, int16_t y) {
  switch (g_screen) {
    case Screen::Select:
      if (y >= 90 && y <= 160 && x <= 50) {
        g_sel = (g_sel + pc::kCaptainCount - 1) % pc::kCaptainCount;
        drawSelect();
        present();
      } else if (y >= 90 && y <= 160 && x >= 270) {
        g_sel = (g_sel + 1) % pc::kCaptainCount;
        drawSelect();
        present();
      } else if (y >= 196 && x >= 100 && x <= 220) {  // SET SAIL (wide zone)
        game::profile.avatar = g_sel;
        strncpy(g_nameBuf, pc::kCaptains[g_sel].name, sizeof(g_nameBuf) - 1);
        g_nameBuf[sizeof(g_nameBuf) - 1] = 0;
        g_nameFromSelect = true;
        g_screen = Screen::Name;  // name (or keep) the pirate before sailing
        drawName();
        present();
      }
      break;

    case Screen::Name: {
      int len = (int)strlen(g_nameBuf);
      if (y >= 198) {                       // bottom row: SPACE / DEL / DONE
        if (x <= 138) {
          if (len > 0 && len < kNameMax) {  // no leading spaces
            g_nameBuf[len] = ' ';
            g_nameBuf[len + 1] = 0;
          }
        } else if (x <= 224) {
          if (len > 0) g_nameBuf[len - 1] = 0;
        } else {
          nameCommit();
          drawWorld();
          present();
          return;
        }
      } else if (y >= 62 && y < 198) {      // letter grid
        int row = (y - 62) / 34;
        int col = (x - 6) / 45;
        if (row < 0) row = 0;
        if (row > 3) row = 3;
        if (col < 0) col = 0;
        if (col > 6) col = 6;
        if (len < kNameMax) {
          g_nameBuf[len] = kKeyRows[row][col];
          g_nameBuf[len + 1] = 0;
        }
      }
      drawName();
      present();
      break;
    }

    case Screen::World: {
      // Tab hit zones taller/wider than drawn tabs (resistive scatter).
      const int tabY0 = theme::kHeaderH;
      const int tabY1 = theme::kHeaderH + 40;
      const int tw = 54, gap = 2;
      const int tx0 = theme::kScreenW - theme::kPad - (tw * 2 + gap);
      if (y >= tabY0 && y <= tabY1 && x >= tx0 && x < tx0 + tw + gap / 2) {
        g_seaView = false;
        drawWorld();
        present();
      } else if (y >= tabY0 && y <= tabY1 && x >= tx0 + tw) {
        g_seaView = true;
        drawWorld();
        present();
      } else if (x >= 220 && y >= theme::kContentBottom - 4) {
        g_screen = Screen::Menu;
        g_menuDirty = true;
        drawMenu();
        present();
      } else if (!g_seaView && y > theme::kHeaderH + 20 &&
                 y < theme::kContentBottom) {
        g_actClip = art::WAVE;  // tap: the captain waves back and quips
        g_actStart = millis();
        sayRandom();
        audio::tapChirp();
      }
      break;
    }

    case Screen::Menu: {
      int total = tools::count();
      int pages = (total + kMenuPerPage - 1) / kMenuPerPage;
      if (pages < 1) pages = 1;
      if (y >= theme::kContentBottom) {
        if (x >= 4 && x <= 80) {  // < DECK
          g_screen = Screen::World;
          drawWorld();
          present();
          return;
        }
        if (pages > 1 && x >= 148 && x <= 184) {  // prev page
          g_menuPage = (g_menuPage + pages - 1) % pages;
          g_menuDirty = true;
          drawMenu();
          present();
          return;
        }
        if (pages > 1 && x >= 236 && x <= 280) {  // next page
          g_menuPage = (g_menuPage + 1) % pages;
          g_menuDirty = true;
          drawMenu();
          present();
          return;
        }
      }
      const int cols = 3, gap = 3, x0 = 2;
      const int y0 = theme::kHeaderH + 20;
      const int tileW = (theme::kScreenW - x0 * 2 - gap * (cols - 1)) / cols;
      const int rows = 3;
      const int tileH =
          (theme::kContentBottom - 4 - y0 - gap * (rows - 1)) / rows;
      int base = g_menuPage * kMenuPerPage;
      for (int li = 0; li < kMenuPerPage; li++) {
        int i = base + li;
        if (i >= total) break;
        int col = li % cols, row = li / cols;
        int tx = x0 + col * (tileW + gap);
        int ty = y0 + row * (tileH + gap);
        if (x >= tx && x <= tx + tileW && y >= ty && y <= ty + tileH) {
          g_toolIndex = i;
          tools::at(i).onOpen();
          g_screen = Screen::Tool;
          drawToolHeader();
          tools::at(i).onDraw(0, theme::kToolHeaderH, theme::kScreenW,
                              theme::kScreenH - theme::kToolHeaderH);
          present();
          return;
        }
      }
      break;
    }

    case Screen::Tool:
      if (y < theme::kToolHeaderH && x < 64) {
        tools::at(g_toolIndex).onClose();
        g_screen = Screen::Menu;
        g_menuDirty = true;
        drawMenu();
        present();
        return;
      }
      if (tools::at(g_toolIndex).onTouch(x, y)) {
        drawToolHeader();
        tools::at(g_toolIndex).onDraw(0, theme::kToolHeaderH, theme::kScreenW,
                                      theme::kScreenH - theme::kToolHeaderH);
        present();
      }
      break;
  }
}

// ---------------------------------------------------------------------------
//  USB companion + test hooks (line protocol, newline-terminated).
//    INFO / STATUS / STATUSJ / HELP / LOGS / LOGGET <path>
//    TAP <x> <y>  SHOT  BEEP  SCAN
// ---------------------------------------------------------------------------
void companionStatusJson() {
  char ts[24] = "";
  bool haveTs = gps::formatTimestamp(ts, sizeof(ts));
  Serial.printf(
      "{\"ver\":\"%s\",\"screen\":%d,\"sea\":%d,\"lvl\":%u,\"xp\":%lu,"
      "\"avatar\":%d,\"sd\":%s,\"art\":%s,\"heap\":%u,\"bri\":%u,"
      "\"bat_pct\":%d,\"bat_mv\":%lu,\"usb\":%s,\"wifi\":%d,\"ble\":%d,"
      "\"gps\":\"%s\",\"lat\":%.6f,\"lon\":%.6f,\"alt\":%.1f,\"sats\":%lu"
      "%s%s%s}\n",
PP_VERSION, (int)g_screen, (int)g_seaView, game::profile.level,
      (unsigned long)game::profile.xp, game::profile.avatar,
      g_sdReady ? "true" : "false", art::available() ? "true" : "false",
      (unsigned)ESP.getFreeHeap(), (unsigned)app::brightness(),
      power::batteryPct(), (unsigned long)power::batteryMv(),
      power::usbPowered() ? "true" : "false", tools::lastWifiCount(),
      tools::lastBleCount(), gps::statusLabel(),
      gps::hasFix() ? gps::latitude() : 0.0,
      gps::hasFix() ? gps::longitude() : 0.0,
      gps::hasFix() ? gps::altitudeM() : 0.0,
      (unsigned long)gps::satellites(), haveTs ? ",\"time\":\"" : "",
      haveTs ? ts : "", haveTs ? "\"" : "");
}

void companionListLogs() {
  if (!g_sdReady) {
    Serial.println("LOGS err=no_sd");
    return;
  }
  File root = SD_MMC.open("/");
  if (!root || !root.isDirectory()) {
    Serial.println("LOGS err=open");
    return;
  }
  Serial.println("LOGS_BEGIN");
  File f = root.openNextFile();
  int n = 0;
  while (f && n < 64) {
    if (!f.isDirectory()) {
      Serial.printf("LOG %s %lu\n", f.name(), (unsigned long)f.size());
      n++;
    }
    f = root.openNextFile();
  }
  Serial.printf("LOGS_END count=%d\n", n);
}

void companionGetLog(const char* path) {
  if (!g_sdReady) {
    Serial.println("LOGGET err=no_sd");
    return;
  }
  if (!path || path[0] == 0) {
    Serial.println("LOGGET err=path");
    return;
  }
  char full[64];
  if (path[0] == '/')
    snprintf(full, sizeof(full), "%s", path);
  else
    snprintf(full, sizeof(full), "/%s", path);
  File f = SD_MMC.open(full, FILE_READ);
  if (!f) {
    Serial.printf("LOGGET err=missing path=%s\n", full);
    return;
  }
  size_t sz = f.size();
  Serial.printf("LOGGET_BEGIN path=%s size=%u\n", full, (unsigned)sz);
  uint8_t buf[128];
  while (true) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    Serial.write(buf, (size_t)n);
  }
  f.close();
  Serial.print("\nLOGGET_END\n");
}

void handleSerialDebug() {
  static char buf[220];
  static int len = 0;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch != '\n' && ch != '\r') {
      if (len < (int)sizeof(buf) - 1) buf[len++] = ch;
      continue;
    }
    buf[len] = 0;
    len = 0;
    if (buf[0] == 0) continue;
    int x = 0, y = 0;
    if (sscanf(buf, "TAP %d %d", &x, &y) == 2) {
      power::noteActivity(millis());
      handleTap((int16_t)x, (int16_t)y);
      Serial.printf("OK TAP %d %d\n", x, y);
    } else if (strcmp(buf, "SHOT") == 0) {
      if (g_canvasOk) {
        Serial.printf("SHOT_BEGIN %d %d\n", canvas.width(), canvas.height());
        Serial.write((const uint8_t*)canvas.getBuffer(),
                     (size_t)canvas.width() * canvas.height() * 2);
        Serial.print("\nSHOT_END\n");
      } else {
        Serial.println("ERR no canvas");
      }
    } else if (strcmp(buf, "BEEP") == 0) {
      audio::chime();
      Serial.printf("OK BEEP ready=%d sound=%d\n", (int)audio::ready(),
                    (int)app::sound());
    } else if (strcmp(buf, "SCAN") == 0) {
      Serial.print("SCAN:");
      for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
      }
      Serial.println();
    } else if (strcmp(buf, "INFO") == 0) {
      Serial.printf(
          "INFO ver=%s screen=%d sea=%d lvl=%u xp=%lu avatar=%d sd=%d art=%d "
          "heap=%u frame=%ums bri=%u bat=%d mv=%lu usb=%d wifi=%d ble=%d "
          "gps=%s\n",
PP_VERSION, (int)g_screen, (int)g_seaView, game::profile.level,
          (unsigned long)game::profile.xp, game::profile.avatar,
          (int)g_sdReady, (int)art::available(), (unsigned)ESP.getFreeHeap(),
          (unsigned)g_frameMs, (unsigned)app::brightness(), power::batteryPct(),
          (unsigned long)power::batteryMv(), (int)power::usbPowered(),
          tools::lastWifiCount(), tools::lastBleCount(), gps::statusLabel());
    } else if (strcmp(buf, "STATUS") == 0 || strcmp(buf, "STATUSJ") == 0 ||
               strcmp(buf, "SUMMARY") == 0) {
      companionStatusJson();
    } else if (strcmp(buf, "LOGS") == 0) {
      companionListLogs();
    } else if (strncmp(buf, "LOGGET ", 7) == 0) {
      companionGetLog(buf + 7);
    } else if (strncmp(buf, "WIFICFG ", 8) == 0) {
      // WIFICFG ssid|password   (pipe separates; spaces allowed in either)
      char* rest = buf + 8;
      char* bar = strchr(rest, '|');
      if (!bar) {
        Serial.println("ERR WIFICFG use: WIFICFG ssid|password");
      } else {
        *bar = 0;
        ota::setWifi(rest, bar + 1);
        Serial.printf("OK WIFICFG ssid=%s\n", ota::wifiSsid());
      }
    } else if (strncmp(buf, "OTAURL ", 7) == 0) {
      ota::setUrl(buf + 7);
      Serial.printf("OK OTAURL %s\n", ota::url());
    } else if (strcmp(buf, "OTARUN") == 0) {
      Serial.println("OK OTARUN starting");
      bool ok = ota::runUpdate();
      Serial.printf("OTARUN %s status=%s\n", ok ? "ok" : "fail", ota::status());
    } else if (strcmp(buf, "SLEEP") == 0) {
      Serial.println("OK SLEEP");
      Serial.flush();
      game::save();
      power::deepSleepNow();
    } else if (strcmp(buf, "HELP") == 0) {
      Serial.println(
          "HELP INFO STATUS LOGS LOGGET <path> "
          "WIFICFG ssid|pass OTAURL <url> OTARUN SLEEP "
          "TAP <x> <y> SHOT BEEP SCAN");
    } else {
      Serial.printf("ERR unknown cmd: %s\n", buf);
    }
  }
}

}  // namespace

// ===========================================================================
//  Setup / loop
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  ledcAttach(TFT_BL, 20000, 8);

  settings.begin("set", true);
  g_brightness = settings.getUChar("bri", 90);
  g_sound = settings.getBool("snd", true);
  settings.end();

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_RST, HIGH);
  delay(200);
  touch::begin();

  display.init();
  display.setRotation(1);
  app::setBrightness(g_brightness);

  // Allocate the compositing canvas in PSRAM.
  canvas.setColorDepth(16);
  canvas.setPsram(true);
  g_canvasOk = canvas.createSprite(320, 240);
  tools::gfx = g_canvasOk ? (lgfx::LGFXBase*)&canvas : (lgfx::LGFXBase*)&display;

  game::load();
  led::begin();
  audio::begin();
  power::begin();
  ota::begin();
  gps::begin();

  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
  g_sdReady = SD_MMC.begin("/sdcard", false);
  art::begin(g_sdReady);

  g_screen = (game::profile.level == 0 && game::profile.xp == 0)
                 ? Screen::Select
                 : Screen::World;
  if (g_screen == Screen::Select)
    drawSelect();
  else
    drawWorld();
  present();

Serial.printf("Pocket Pirate %s ready. Captain %s Lv%u  SD:%s  art:%s\n",
                PP_VERSION, game::profile.name, game::profile.level,
                g_sdReady ? "ok" : "none", art::available() ? "yes" : "no");
  audio::chime();  // boot "ahoy" (no-op if codec absent or sound off)
}

uint32_t g_lastSave = 0;

void loop() {
  uint32_t now = millis();

  handleSerialDebug();
  led::tick(now);
  power::tick(now);
  gps::tick(now);
  power::applyIdleDim(app::brightness(), now);

  // Settings Cabin asked for the rename keyboard.
  if (g_renamePending) {
    g_renamePending = false;
    if (g_screen == Screen::Tool && g_toolIndex >= 0)
      tools::at(g_toolIndex).onClose();
    strncpy(g_nameBuf, game::profile.name, sizeof(g_nameBuf) - 1);
    g_nameBuf[sizeof(g_nameBuf) - 1] = 0;
    g_nameFromSelect = false;
    g_screen = Screen::Name;
    drawName();
    present();
  }

  touch::read();
  int16_t tx, ty;
  if (touch::wasTapped(tx, ty)) {
    power::noteActivity(now);
    handleTap(tx, ty);
  }

  if (g_screen == Screen::Tool && g_toolIndex >= 0)
    tools::at(g_toolIndex).onTick(now);

  // Redraw cadence: world anim ~6 fps is enough; menu is dirty-flagged;
  // tools refresh slower now that body() is a cheap solid fill.
  if (g_screen == Screen::World && now - g_lastDraw > 160) {
    g_lastDraw = now;
    g_anim = (g_anim + 1) % 8;
    drawWorld();
    drawToastOverlay();
    present();
    g_frameMs = millis() - now;
  } else if (g_screen == Screen::Select && now - g_lastDraw > 180) {
    g_lastDraw = now;
    drawSelect();
    present();
  } else if (g_screen == Screen::Name && now - g_lastDraw > 250) {
    g_lastDraw = now;  // cursor blink
    drawName();
    present();
  } else if (g_screen == Screen::Tool && now - g_lastDraw > 280) {
    g_lastDraw = now;
    tools::at(g_toolIndex).onDraw(0, theme::kToolHeaderH, theme::kScreenW,
                                  theme::kScreenH - theme::kToolHeaderH);
    drawToastOverlay();
    present();
  } else if (g_screen == Screen::Menu) {
    bool toastLive = (millis() <= g_toastUntil && g_toast[0]);
    if (g_menuDirty || (toastLive && now - g_lastDraw > 250)) {
      g_lastDraw = now;
      g_menuDirty = false;
      drawMenu();
      drawToastOverlay();
      present();
    }
  }

  // Level-up fanfare (XP is awarded inside the tool modules).
  static uint8_t s_lastLevel = 255;
  if (s_lastLevel == 255) s_lastLevel = game::profile.level;
  if (game::profile.level > s_lastLevel) {
    audio::fanfare();
    sayRandom();
  }
  s_lastLevel = game::profile.level;

  if (now - g_lastSave > 15000) {
    g_lastSave = now;
    game::save();
  }
}
