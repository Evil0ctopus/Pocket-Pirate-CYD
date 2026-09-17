#include "chibi.h"

#include "captains.h"
#include "gfx_util.h"

using gfxu::blend565;
using gfxu::darken;
using gfxu::lighten;
using gfxu::rgb;

namespace chibi {

void drawCaptain(lgfx::LGFXBase& g, int cx, int cy, int avatar, int tier) {
  if (avatar < 0) avatar = 0;
  if (avatar >= pc::kCaptainCount) avatar = pc::kCaptainCount - 1;
  if (tier < 0) tier = 0;
  if (tier > 4) tier = 4;
  const pc::Captain& c = pc::kCaptains[avatar];
  const bool beard = c.flags & pc::kBeard;
  const bool patch = c.flags & pc::kPatch;

  const int R = 27;                 // head radius (chibi: big head)
  const int torsoW = 76, torsoH = 66;
  const int torsoX = cx - torsoW / 2;
  const int torsoTop = cy + 30;
  const int shoulderY = cy + 40;
  const int beltY = cy + 74;

  const uint16_t boot = rgb(70, 46, 28);
  const uint16_t bootHi = rgb(110, 78, 50);
  const uint16_t shirt = rgb(238, 232, 214);
  const uint16_t leather = rgb(60, 40, 25);

  // ---- boots / legs -------------------------------------------------------
  g.fillSmoothRoundRect(cx - 22, beltY + 18, 18, 30, 8, boot);
  g.fillSmoothRoundRect(cx + 4, beltY + 18, 18, 30, 8, boot);
  g.fillSmoothRoundRect(cx - 22, beltY + 40, 20, 9, 5, darken(boot, 40));
  g.fillSmoothRoundRect(cx + 4, beltY + 40, 20, 9, 5, darken(boot, 40));
  g.fillSmoothCircle(cx - 15, beltY + 22, 3, bootHi);
  g.fillSmoothCircle(cx + 11, beltY + 22, 3, bootHi);

  // ---- coat torso ---------------------------------------------------------
  g.fillSmoothRoundRect(torsoX, torsoTop, torsoW, torsoH, 20, c.coat);
  g.fillSmoothRoundRect(cx + 6, torsoTop + 4, torsoW / 2 - 4, torsoH - 8, 16,
                        blend565(c.coat, c.coatSh, 150));
  g.fillSmoothRoundRect(torsoX + 4, torsoTop + 6, 14, torsoH - 18, 7,
                        lighten(c.coat, 40));

  // ---- arms + cuffs + hands ----------------------------------------------
  g.fillSmoothRoundRect(torsoX - 6, shoulderY - 2, 20, 46, 10, c.coat);
  g.fillSmoothRoundRect(torsoX + torsoW - 14, shoulderY - 2, 20, 46, 10,
                        blend565(c.coat, c.coatSh, 90));
  g.fillSmoothRoundRect(torsoX - 6, shoulderY + 36, 20, 9, 5, c.trim);
  g.fillSmoothRoundRect(torsoX + torsoW - 14, shoulderY + 36, 20, 9, 5, c.trim);
  g.fillSmoothCircle(torsoX + 3, shoulderY + 48, 6, c.skin);
  g.fillSmoothCircle(torsoX + torsoW - 5, shoulderY + 48, 6, c.skin);

  // ---- shirt chest + lapels + buttons ------------------------------------
  g.fillSmoothRoundRect(cx - 14, torsoTop + 6, 28, 44, 10, shirt);
  g.fillTriangle(cx - 15, torsoTop + 4, cx - 1, torsoTop + 4, cx - 15,
                 torsoTop + 40, c.coat);
  g.fillTriangle(cx + 15, torsoTop + 4, cx + 1, torsoTop + 4, cx + 15,
                 torsoTop + 40, blend565(c.coat, c.coatSh, 90));
  for (int i = 0; i < 3; i++)
    g.fillSmoothCircle(cx, torsoTop + 16 + i * 10, 2, c.trim);

  // ---- belt + buckle ------------------------------------------------------
  g.fillSmoothRoundRect(torsoX, beltY, torsoW, 14, 4, leather);
  g.fillSmoothRoundRect(cx - 10, beltY - 1, 20, 16, 4, c.trim);
  g.drawRoundRect(cx - 6, beltY + 2, 12, 10, 2, darken(c.trim, 90));

  // ---- rank flair ---------------------------------------------------------
  if (tier >= 2) {  // epaulettes
    g.fillSmoothCircle(torsoX + 8, shoulderY, 6, c.trim);
    g.fillSmoothCircle(torsoX + torsoW - 8, shoulderY, 6, c.trim);
    for (int i = -2; i <= 2; i++) {
      g.drawWideLine(torsoX + 8 + i * 2, shoulderY + 4, torsoX + 8 + i * 2,
                     shoulderY + 10, 1, darken(c.trim, 40));
      g.drawWideLine(torsoX + torsoW - 8 + i * 2, shoulderY + 4,
                     torsoX + torsoW - 8 + i * 2, shoulderY + 10, 1,
                     darken(c.trim, 40));
    }
  }
  if (tier >= 4) {  // captain's medal
    g.fillSmoothRoundRect(cx - 10, torsoTop + 2, 6, 8, 2, rgb(200, 40, 50));
    g.fillSmoothCircle(cx - 7, torsoTop + 12, 4, c.trim);
  }

  // ---- held cutlass (tier>=2) --------------------------------------------
  if (tier >= 2) {
    g.fillSmoothRoundRect(cx - 50, beltY - 12, 14, 6, 3, c.trim);      // guard
    g.drawWideLine(cx - 43, beltY - 10, cx - 56, beltY - 54, 5,
                   rgb(214, 220, 230));                                // blade
    g.drawWideLine(cx - 44, beltY - 12, cx - 55, beltY - 50, 1,
                   rgb(255, 255, 255));                                // shine
    g.fillSmoothCircle(cx - 43, beltY - 4, 3, darken(c.trim, 40));     // pommel
  }
  // ---- held telescope (tier>=3) ------------------------------------------
  if (tier >= 3) {
    g.drawWideLine(cx + 18, beltY - 6, cx + 46, beltY - 20, 9,
                   rgb(170, 120, 45));
    g.drawWideLine(cx + 26, beltY - 10, cx + 34, beltY - 14, 9,
                   darken(rgb(170, 120, 45), 40));
    g.fillSmoothCircle(cx + 46, beltY - 20, 4, rgb(185, 222, 255));
  }

  // ---- neck ---------------------------------------------------------------
  g.fillSmoothRoundRect(cx - 8, cy + 18, 16, 14, 5, c.skinSh);

  // ---- head base + shading -----------------------------------------------
  g.fillSmoothCircle(cx, cy, R, c.skin);
  g.fillSmoothCircle(cx + R - 9, cy + 3, 9, blend565(c.skin, c.skinSh, 120));
  g.fillSmoothCircle(cx - 8, cy - 9, R - 9, c.skinHi);
  g.fillSmoothCircle(cx - 11, cy + 6, 7, lighten(c.skin, 30));

  // ---- ears + earring -----------------------------------------------------
  g.fillSmoothCircle(cx - R + 2, cy + 3, 5, c.skin);
  g.fillSmoothCircle(cx + R - 2, cy + 3, 5, c.skin);
  if (tier >= 1) {
    g.fillSmoothCircle(cx + R - 1, cy + 10, 3, c.trim);
    g.fillSmoothCircle(cx + R - 1, cy + 10, 1, c.skin);
  }

  // ---- hair sideburns / beard --------------------------------------------
  if (beard) {
    g.fillArc(cx, cy + 2, R - 10, R + 3, 18, 162, c.hair);      // jaw beard
    g.fillSmoothCircle(cx - R + 3, cy + 2, 5, c.hair);          // sideburns
    g.fillSmoothCircle(cx + R - 3, cy + 2, 5, c.hair);
    g.fillSmoothCircle(cx - 6, cy + 12, 4, c.hair);             // mustache
    g.fillSmoothCircle(cx + 6, cy + 12, 4, c.hair);
    g.fillSmoothCircle(cx, cy + 20, 6, c.hair);                 // chin tuft
  } else {
    g.fillSmoothCircle(cx - R + 2, cy - 2, 5, c.hair);          // hair by ears
    g.fillSmoothCircle(cx + R - 2, cy - 2, 5, c.hair);
  }

  // ---- cheeks -------------------------------------------------------------
  g.fillSmoothCircle(cx - 14, cy + 8, 4, blend565(c.skin, rgb(255, 120, 120), 90));
  g.fillSmoothCircle(cx + 14, cy + 8, 4, blend565(c.skin, rgb(255, 120, 120), 90));

  // ---- eyes (big + glossy) ------------------------------------------------
  const int ey = cy + 1;
  auto eye = [&](int ex, bool covered) {
    if (covered) return;
    g.fillSmoothCircle(ex, ey, 9, darken(c.skin, 25));           // socket
    g.fillSmoothCircle(ex, ey, 8, rgb(250, 250, 252));           // white
    int ix = ex + (ex < cx ? 1 : -1);
    g.fillSmoothCircle(ix, ey + 1, 5, c.eyes);                   // iris
    g.fillSmoothCircle(ix, ey + 1, 3, rgb(22, 20, 28));          // pupil
    g.fillSmoothCircle(ex - 2, ey - 2, 2, rgb(255, 255, 255));   // highlight
    g.fillSmoothCircle(ex + 3, ey + 4, 1, rgb(255, 255, 255));
    g.fillArc(ex, ey - 1, 7, 9, 200, 340, c.skin);              // upper lid
  };
  eye(cx - 10, false);
  eye(cx + 10, patch);
  // eyebrows
  g.drawWideLine(cx - 16, cy - 8, cx - 5, cy - 10, 3, c.hair);
  g.drawWideLine(cx + 5, cy - 10, cx + 16, cy - 8, 3, c.hair);

  // ---- eyepatch -----------------------------------------------------------
  if (patch) {
    g.fillSmoothCircle(cx + 10, ey, 7, rgb(20, 20, 22));
    g.drawWideLine(cx + 3, cy - 9, cx + R + 2, cy + 3, 3, rgb(20, 20, 22));
    g.drawWideLine(cx + 10, cy - 9, cx - R, cy - 3, 2, rgb(28, 28, 30));
  }

  // ---- nose + mouth -------------------------------------------------------
  g.fillSmoothCircle(cx, cy + 8, 3, blend565(c.skin, c.skinSh, 120));
  g.fillSmoothCircle(cx - 1, cy + 7, 1, c.skinHi);
  if (beard) {
    g.fillArc(cx, cy + 13, 3, 5, 15, 165, rgb(120, 40, 40));
  } else {
    g.fillArc(cx, cy + 12, 5, 8, 12, 168, rgb(110, 40, 45));
    g.fillArc(cx, cy + 12, 5, 6, 12, 168, rgb(252, 252, 252));  // teeth
  }

  // ---- bandana ------------------------------------------------------------
  if (c.bandana) {
    g.fillSmoothRoundRect(cx - R, cy - R + 6, 2 * R, 11, 5, c.bandana);
    g.fillSmoothRoundRect(cx - R, cy - R + 12, 2 * R, 5, 3,
                          darken(c.bandana, 40));
    g.fillSmoothCircle(cx - R + 1, cy - R + 15, 5, c.bandana);          // knot
    g.fillTriangle(cx - R + 1, cy - R + 15, cx - R - 10, cy - R + 20,
                   cx - R - 2, cy - R + 26, c.bandana);                 // tail
    for (int i = -2; i <= 2; i++)
      g.fillSmoothCircle(cx + i * 9, cy - R + 11, 1, rgb(250, 250, 250));
  }

  // ---- tricorn hat --------------------------------------------------------
  if (tier >= 1 || !c.bandana) {
    int hw = 32 + tier * 3;
    g.fillSmoothRoundRect(cx - hw, cy - R + 2, 2 * hw, 15, 7, c.hatSh);
    g.fillTriangle(cx - hw, cy - R + 9, cx + hw, cy - R + 9, cx, cy - R - 18,
                   c.hat);
    g.fillTriangle(cx - hw, cy - R + 9, cx - hw + 6, cy - R - 8, cx - hw + 20,
                   cy - R + 6, c.hat);
    g.fillTriangle(cx + hw, cy - R + 9, cx + hw - 6, cy - R - 8, cx + hw - 20,
                   cy - R + 6, c.hat);
    g.fillSmoothRoundRect(cx - 18, cy - R - 12, 36, 18, 11, c.hat);
    g.fillTriangle(cx - hw + 8, cy - R + 6, cx, cy - R - 12, cx + 4, cy - R + 4,
                   lighten(c.hat, 24));  // brim highlight
    g.drawWideLine(cx - hw + 5, cy - R + 8, cx, cy - R - 14, 2, c.trim);
    g.drawWideLine(cx, cy - R - 14, cx + hw - 5, cy - R + 8, 2, c.trim);
    if (tier >= 2) {  // skull emblem
      int sx = cx, sy = cy - R - 3;
      g.fillSmoothCircle(sx, sy, 5, rgb(238, 238, 232));
      g.fillSmoothRoundRect(sx - 3, sy + 2, 6, 4, 2, rgb(238, 238, 232));
      g.fillSmoothCircle(sx - 2, sy - 1, 1, rgb(20, 20, 20));
      g.fillSmoothCircle(sx + 2, sy - 1, 1, rgb(20, 20, 20));
      g.drawWideLine(sx - 7, sy + 4, sx + 7, sy - 5, 2, rgb(238, 238, 232));
      g.drawWideLine(sx - 7, sy - 5, sx + 7, sy + 4, 2, rgb(238, 238, 232));
    }
    if (tier >= 3) {  // plume feather
      uint16_t fc = c.bandana ? rgb(240, 240, 245) : rgb(214, 70, 70);
      g.drawWideLine(cx - hw + 10, cy - R + 2, cx - hw + 2, cy - R - 24, 4, fc);
      for (int i = 0; i < 4; i++)
        g.fillSmoothCircle(cx - hw + 8 - i, cy - R - 4 - i * 5, 3,
                           lighten(fc, 20));
    }
  }

  // ---- parrot on shoulder (tier>=4) --------------------------------------
  if (tier >= 4) {
    int px = torsoX + 4, py = shoulderY + 2;
    g.fillTriangle(px + 5, py, px + 18, py - 5, px + 16, py + 8, rgb(40, 90, 200));
    g.fillSmoothCircle(px, py, 8, rgb(40, 170, 70));
    g.fillSmoothCircle(px - 2, py + 2, 5, rgb(245, 210, 60));
    g.fillArc(px, py, 3, 7, 250, 30, rgb(30, 140, 60));
    g.fillSmoothCircle(px - 6, py - 6, 5, rgb(210, 50, 50));
    g.fillTriangle(px - 10, py - 7, px - 15, py - 5, px - 10, py - 2,
                   rgb(240, 190, 70));
    g.fillSmoothCircle(px - 6, py - 7, 1, rgb(20, 20, 20));
  }
}

void drawShip(lgfx::LGFXBase& g, int cx, int cy, int tier) {
  const uint16_t hull = rgb(96, 62, 36);
  const uint16_t hullSh = rgb(66, 42, 24);
  const uint16_t sail = rgb(242, 236, 220);
  const uint16_t sailSh = rgb(206, 198, 178);
  const uint16_t mast = rgb(74, 54, 36);

  int masts = tier >= 2 ? 2 : 1;
  for (int m = 0; m < masts; m++) {
    int mx = cx - 10 + m * 22;
    g.fillRect(mx - 2, cy - 34, 4, 40, mast);
    g.fillSmoothRoundRect(mx - 24, cy - 32, 24, 30, 5, sail);
    g.fillSmoothRoundRect(mx - 24, cy - 18, 24, 16, 5, sailSh);
    if (m == masts - 1) {  // pennant
      g.fillTriangle(mx + 2, cy - 34, mx + 2, cy - 26, mx + 16, cy - 30,
                     rgb(200, 50, 50));
    }
  }
  g.fillSmoothRoundRect(cx - 36, cy + 2, 72, 20, 8, hull);
  g.fillSmoothRoundRect(cx - 36, cy + 12, 72, 10, 6, hullSh);
  g.fillTriangle(cx - 30, cy + 20, cx + 30, cy + 20, cx, cy + 32, hullSh);
  g.drawFastHLine(cx - 32, cy + 6, 64, rgb(214, 178, 90));
  for (int i = -2; i <= 2; i++)
    g.fillSmoothCircle(cx + i * 12, cy + 10, 1, rgb(30, 20, 12));  // portholes
}

}  // namespace chibi
