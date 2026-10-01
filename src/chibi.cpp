#include "chibi.h"

#include "captains.h"
#include "gfx_util.h"

using gfxu::darken;
using gfxu::lighten;
using gfxu::rgb;

namespace chibi {

namespace {
constexpr int kPixelScale = 3;

void pixelRect(lgfx::LGFXBase& g, int originX, int originY, int x, int y,
               int width, int height, uint16_t color) {
  if (width <= 0 || height <= 0) return;
  g.fillRect(originX + x * kPixelScale, originY + y * kPixelScale,
             width * kPixelScale, height * kPixelScale, color);
}
}  // namespace

void drawCaptain(lgfx::LGFXBase& g, int cx, int cy, int avatar, int tier,
                 int motion, int frame) {
    if (avatar < 0) avatar = 0;
    if (avatar >= pc::kCaptainCount) avatar = pc::kCaptainCount - 1;
    if (tier < 0) tier = 0;
    if (tier > 4) tier = 4;
    if (motion < 0 || motion > 4) motion = 0;
    if (frame < 0) frame = 0;
    frame %= 3;

    const pc::Captain& captain = pc::kCaptains[avatar];
    const bool beard = (captain.flags & pc::kBeard) != 0;
    const bool patch = (captain.flags & pc::kPatch) != 0;
    const bool longHair = (captain.flags & pc::kFemale) != 0;
    const uint16_t outline = rgb(24, 18, 28);
    const uint16_t shirt = rgb(226, 218, 198);
    const uint16_t shirtShade = rgb(196, 186, 164);
    int originX = cx - 72;
    int originY = cy - 56;
    if (motion == 3 && frame == 1) originY -= kPixelScale;

    auto rect = [&](int x, int y, int width, int height, uint16_t color) {
      pixelRect(g, originX, originY, x, y, width, height, color);
    };

    rect(14, 46, 8, 10, outline);
    rect(26, 46, 8, 10, outline);
    rect(15, 47, 6, 7, rgb(70, 48, 34));
    rect(27, 47, 6, 7, rgb(70, 48, 34));
    rect(13, 53, 11, 3, rgb(46, 32, 22));
    rect(25, 53, 11, 3, rgb(46, 32, 22));
    rect(16, 47, 2, 3, rgb(112, 80, 52));
    rect(28, 47, 2, 3, rgb(112, 80, 52));

    rect(10, 27, 28, 22, outline);
    rect(12, 28, 24, 19, tier >= 1 ? captain.coat : shirt);
    rect(33, 29, 3, 17, tier >= 1 ? captain.coatSh : shirtShade);
    rect(13, 29, 2, 16, tier >= 1 ? lighten(captain.coat, 40) : shirt);
    rect(21, 28, 7, 14, shirt);
    rect(21, 28, 2, 7, captain.coat);
    rect(27, 28, 2, 7, captain.coatSh);
    rect(23, 30, 3, 10, shirtShade);
    rect(24, 31, 1, 1, captain.trim);
    rect(24, 34, 1, 1, captain.trim);
    rect(24, 37, 1, 1, captain.trim);

    if (motion == 3 && frame > 0) {
      rect(8, 30, 6, 7, outline);
      rect(9, 31, 4, 5, tier >= 1 ? captain.coat : shirt);
      int armY = frame == 1 ? 24 : 18;
      rect(7, armY, 6, 9, outline);
      rect(8, armY + 1, 4, 7, tier >= 1 ? captain.coat : shirt);
      rect(7, armY - 2, 6, 2, captain.trim);
      rect(8, armY - 6, 5, 4, captain.skin);
    } else {
      rect(8, 30, 6, 15, outline);
      rect(9, 31, 4, 11, tier >= 1 ? captain.coat : shirt);
      rect(9, 42, 4, 2, captain.trim);
      rect(9, 44, 4, 4, captain.skin);
    }

    int rightPose = 0;
    if (motion == 1) rightPose = frame;
    if (motion == 2 || motion == 3) rightPose = 2;
    if (motion == 4) rightPose = 3;
    if (rightPose == 0) {
      rect(36, 30, 6, 15, outline);
      rect(37, 31, 4, 11, tier >= 1 ? captain.coat : shirt);
      rect(37, 42, 4, 2, captain.trim);
      rect(37, 44, 4, 4, captain.skin);
    } else if (rightPose == 1) {
      rect(36, 30, 6, 7, outline);
      rect(37, 31, 4, 5, tier >= 1 ? captain.coat : shirt);
      rect(38, 24, 5, 9, outline);
      rect(39, 25, 3, 7, tier >= 1 ? captain.coat : shirt);
      rect(38, 22, 5, 2, captain.trim);
      rect(39, 18, 5, 4, captain.skin);
    } else if (rightPose == 2) {
      rect(36, 30, 6, 7, outline);
      rect(37, 31, 4, 5, tier >= 1 ? captain.coat : shirt);
      rect(38, 19, 5, 13, outline);
      rect(39, 20, 3, 11, tier >= 1 ? captain.coat : shirt);
      rect(38, 18, 5, 2, captain.trim);
      rect(39, 13, 5, 5, captain.skin);
    } else {
      rect(36, 30, 6, 7, outline);
      rect(37, 31, 4, 5, captain.coat);
      rect(38, 19, 5, 13, outline);
      rect(39, 20, 3, 11, captain.coat);
      rect(38, 18, 5, 2, captain.trim);
      rect(31, 13, 9, 4, captain.skin);
    }
    if (motion == 2) {
      rect(40, 13, 3, 2, captain.trim);
      rect(41, 7, 2, 7, outline);
      rect(42, 5, 2, 8, rgb(206, 214, 224));
      rect(41, 5, 4, 1, rgb(206, 214, 224));
    }

    rect(12, 42, 24, 5, captain.bandana ? captain.bandana : captain.trim);
    rect(12, 46, 24, 2, darken(captain.bandana ? captain.bandana : captain.trim, 40));
    rect(32, 46, 3, 5, captain.bandana ? captain.bandana : captain.trim);
    rect(22, 42, 5, 6, captain.trim);
    rect(23, 43, 3, 4, outline);

    if (tier >= 2) {
      rect(11, 29, 7, 3, captain.trim);
      rect(30, 29, 7, 3, captain.trim);
      rect(13, 32, 2, 4, darken(captain.trim, 40));
      rect(33, 32, 2, 4, darken(captain.trim, 40));
    }
    if (tier >= 4) {
      rect(20, 30, 2, 4, rgb(200, 40, 50));
      rect(21, 34, 3, 3, captain.trim);
    }
    if (tier >= 3) {
      rect(8, 45, 5, 2, captain.trim);
      rect(8, 47, 2, 2, rgb(206, 214, 224));
      rect(7, 49, 2, 2, rgb(206, 214, 224));
      rect(6, 51, 2, 2, rgb(206, 214, 224));
    }

    rect(21, 24, 7, 6, outline);
    rect(22, 24, 5, 5, captain.skinSh);
    rect(13, 9, 22, 19, outline);
    rect(15, 11, 18, 15, captain.skin);
    rect(30, 12, 3, 13, captain.skinSh);
    rect(16, 12, 15, 2, captain.skinHi);
    rect(13, 17, 2, 5, captain.skin);
    rect(33, 17, 2, 5, captain.skin);
    rect(16, 14, 6, 2, captain.hair);
    rect(27, 14, 6, 2, captain.hair);
    rect(17, 17, 5, 4, rgb(250, 250, 246));
    rect(27, 17, 5, 4, rgb(250, 250, 246));
    if (motion == 4) {
      rect(20, 18, 2, 3, captain.eyes);
      rect(28, 18, 2, 3, captain.eyes);
    } else {
      rect(19, 18, 2, 3, captain.eyes);
      rect(29, 18, 2, 3, captain.eyes);
    }
    rect(20, 19, 1, 2, outline);
    if (!patch) rect(30, 19, 1, 2, outline);
    if (motion == 0 && frame == 2) {
      rect(17, 18, 5, 3, captain.skin);
      if (!patch) rect(27, 18, 5, 3, captain.skin);
      rect(17, 20, 5, 1, outline);
      if (!patch) rect(27, 20, 5, 1, outline);
    }
    rect(23, 21, 3, 2, captain.skinSh);
    if (beard) {
      rect(15, 21, 19, 7, captain.hair);
      rect(17, 27, 15, 2, captain.hair);
      rect(20, 22, 8, 2, outline);
      rect(22, 22, 5, 1, rgb(240, 236, 222));
    } else {
      rect(22, 23, 5, 1, rgb(120, 44, 40));
    }
    if (patch) {
      rect(27, 16, 6, 6, outline);
      rect(14, 15, 18, 1, outline);
    }
    if (longHair) {
      rect(13, 13, 2, 14, captain.hair);
      rect(33, 13, 2, 14, captain.hair);
    } else if (!beard) {
      rect(14, 13, 2, 7, captain.hair);
      rect(32, 13, 2, 7, captain.hair);
    }
    if (captain.bandana) {
      rect(14, 9, 20, 4, captain.bandana);
      rect(34, 11, 3, 4, captain.bandana);
      rect(36, 14, 2, 3, captain.bandana);
    } else if (tier < 2) {
      rect(16, 8, 16, 4, captain.hair);
    }
    if (tier >= 2) {
      rect(10, 5, 28, 6, outline);
      rect(11, 4, 26, 5, captain.hat);
      rect(33, 5, 4, 4, captain.hatSh);
      rect(9, 9, 30, 3, captain.hatSh);
      rect(10, 9, 28, 1, captain.trim);
      rect(18, 3, 12, 2, captain.hat);
      if (tier >= 3) {
        uint16_t feather = captain.bandana ? rgb(240, 240, 245)
                                           : rgb(214, 70, 70);
        rect(36, 2, 2, 2, feather);
        rect(37, 1, 2, 2, feather);
        rect(38, 0, 2, 2, feather);
      }
      if (tier >= 4) {
        rect(22, 5, 5, 3, rgb(238, 238, 232));
        rect(23, 5, 1, 1, outline);
        rect(25, 5, 1, 1, outline);
        rect(23, 8, 3, 1, rgb(238, 238, 232));
      }
    }
    if (tier >= 4) {
      rect(10, 23, 6, 6, rgb(210, 60, 50));
      rect(11, 20, 5, 4, rgb(210, 60, 50));
      rect(12, 21, 1, 1, outline);
      rect(15, 22, 3, 1, captain.trim);
      rect(10, 25, 3, 3, rgb(44, 152, 82));
    }
  }

void drawShip(lgfx::LGFXBase& g, int cx, int cy, int tier, int frame) {
    if (tier < 0) tier = 0;
    if (tier > 4) tier = 4;
    int originX = cx - 64;
    int originY = cy - 48;
    auto rect = [&](int x, int y, int width, int height, uint16_t color) {
      g.fillRect(originX + x * 4, originY + y * 4, width * 4, height * 4,
                 color);
    };

    const uint16_t outline = rgb(24, 18, 28);
    const uint16_t hull = rgb(122, 84, 48);
    const uint16_t sail = rgb(240, 236, 222);
    const uint16_t sailShade = rgb(208, 200, 180);
    rect(8, 18, 32, 5, outline);
    int halfWidth = 9 + tier * 2;
    for (int row = 0; row < 6; row++) {
      int shrink = row > 3 ? (row - 3) * 2 : 0;
      int width = halfWidth * 2 - shrink * 2;
      rect(24 - halfWidth + shrink, 17 + row, width, 1,
           row == 0 ? rgb(156, 112, 66) : hull);
    }
    rect(11, 17, 26, 1, rgb(244, 202, 78));
    rect(12, 23, 24, 2, rgb(84, 56, 32));
    for (int port = 0; port < (tier >= 3 ? 5 : 3); port++)
      rect(15 + port * 4, 19, 1, 1, outline);

    int mastCount = tier >= 3 ? 3 : (tier >= 2 ? 2 : 1);
    for (int mast = 0; mast < mastCount; mast++) {
      int mastX = mastCount == 1 ? 24 :
                  (mastCount == 2 ? 16 + mast * 16 : 10 + mast * 14);
      int mastTop = tier >= 3 ? 1 : 3;
      rect(mastX - 1, mastTop, 2, 17 - mastTop, rgb(84, 56, 32));
      for (int row = 0; row < 8; row++) {
        int sailWidth = 3 + row;
        rect(mastX - sailWidth, mastTop + row, sailWidth + 1, 1,
             row > 4 ? sailShade : sail);
      }
      if (tier >= 2) {
        for (int row = 0; row < 5; row++) {
          int sailWidth = 2 + row;
          rect(mastX - sailWidth, mastTop + 9 + row, sailWidth + 1, 1,
               row > 2 ? sailShade : sail);
        }
      }
      if (mast == mastCount - 1) {
        int wind = (frame % 3) - 1;
        rect(mastX + 1 + wind, mastTop - 2, 4, 1, rgb(206, 56, 50));
        rect(mastX + 1 + wind, mastTop - 1, 2, 1, rgb(206, 56, 50));
      }
    }
  }

}  // namespace chibi
