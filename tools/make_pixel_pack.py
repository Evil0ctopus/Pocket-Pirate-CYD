#!/usr/bin/env python3
"""Generate the PIXEL-ART production pack for Pocket Pirate CYD (Bjorn-style).

The approved look (tools/pixel_captain_proto.py v3 "pirate pass"): authored at
native low resolution with a strict limited palette, hard edges, auto 1px dark
outline, then NEAREST-upscaled -- never smoothed. Every output file is emitted
at the packer's EXACT final size so build_artpack.py never resizes (its LANCZOS
would smear the pixels).

Native scales:
  captains 48x56  x3 -> 144x168   (fits CAP_CANVAS 180x210, no packer scaling)
  ships    50x43  x3 -> 150x129   (fits SHIP_SIZE 150x130)
  sky      64x48  x5 -> 320x240   clouds 96x20 x5 -> 480x100
  waves    96x14  x5 -> 480x70    deck   64x18 x5 -> 320x90

Registration: within one (captain,tier) group all 6 frames (idle 0-2, wave 0-2)
share one silhouette bbox -- the static base never changes, eyes are interior,
and the wave arm stays inside x<=40 / y>=13, within the base's extents. The
packer's content_bbox_center then can't drift frames.

Tier evolution (accessories accrue):
  t0 deckhand: shirt w/ rolled sleeves + sash        t1 +open coat/baldric
  t2 +tricorn                                        t3 +cutlass +feather
  t4 +skull cockade +parrot +medal

Usage:
    python tools/make_pixel_pack.py [src_dir]     # default: artpack_pixel_src
    python tools/build_artpack.py artpack_pixel_src artpack_pixel
    # copy artpack_pixel/* to the SD card's /art/ folder and reboot

Requires Pillow:  pip install pillow
"""

import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required.  Install it with:  pip install pillow")

# --- shared palette -----------------------------------------------------------
OUT = (24, 18, 28, 255)
GOLD = (244, 202, 78, 255)
GOLDS = (196, 150, 44, 255)
CUFF = (240, 236, 222, 255)
EYEW = (250, 250, 246, 255)
EYE = (28, 24, 34, 255)
MOUTH = (120, 44, 40, 255)
TROU = (44, 58, 70, 255)
BOOT = (70, 48, 34, 255)
BOOTC = (112, 80, 52, 255)
BOOTS = (46, 32, 22, 255)
VEST = (46, 38, 54, 255)
STRAP = (58, 40, 28, 255)
BLADE = (206, 214, 224, 255)
FEATH = (216, 60, 66, 255)
PATCH = (22, 20, 24, 255)
SHIRT = (226, 218, 198, 255)     # deckhand shirt (dimmer than lace CUFF)
SHIRTS = (196, 186, 164, 255)

WOOD = (122, 84, 48, 255)
WOODH = (156, 112, 66, 255)
WOODD = (84, 56, 32, 255)
SAIL = (240, 236, 222, 255)
SAILS = (208, 200, 180, 255)
FLAG = (206, 56, 50, 255)

# --- roster (mirrors include/captains.h) ---------------------------------------
CAPTAINS = [
    dict(name="Mara Tide", skin=(240, 198, 160, 255), skins=(198, 150, 118, 255),
         hair=(92, 58, 36, 255), coat=(30, 132, 128, 255), coats=(18, 90, 88, 255),
         coath=(82, 184, 176, 255), hat=(38, 50, 88, 255), hats=(24, 32, 60, 255),
         sash=(198, 54, 64, 255), bandana=None, beard=False, patch=False,
         female=True, sidehair=True),
    dict(name="Roan Redsail", skin=(232, 180, 145, 255), skins=(190, 138, 106, 255),
         hair=(188, 96, 40, 255), coat=(150, 38, 34, 255), coats=(104, 24, 22, 255),
         coath=(204, 84, 72, 255), hat=(38, 34, 34, 255), hats=(22, 20, 20, 255),
         sash=(212, 162, 52, 255), bandana=(200, 56, 48, 255), beard=True,
         patch=False, female=False, sidehair=False),
    dict(name="Iyla Dawn", skin=(246, 210, 178, 255), skins=(206, 164, 128, 255),
         hair=(214, 176, 96, 255), coat=(44, 84, 156, 255), coats=(28, 56, 110, 255),
         coath=(96, 138, 204, 255), hat=(122, 72, 162, 255), hats=(84, 46, 116, 255),
         sash=(150, 92, 190, 255), bandana=(150, 92, 190, 255), beard=False,
         patch=False, female=True, sidehair=True),
    dict(name="Bram Kettle", skin=(204, 156, 120, 255), skins=(166, 120, 88, 255),
         hair=(198, 198, 198, 255), coat=(48, 108, 62, 255), coats=(30, 72, 40, 255),
         coath=(96, 160, 106, 255), hat=(76, 56, 38, 255), hats=(50, 36, 24, 255),
         sash=(160, 44, 48, 255), bandana=None, beard=True, patch=True,
         female=False, sidehair=False),
]

CAP_W, CAP_H = 48, 56
SHIP_W, SHIP_H = 50, 43


class Px:
    """Tiny pixel canvas with rect/point/clear helpers."""

    def __init__(self, w, h):
        self.img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        self.px = self.img.load()
        self.w, self.h = w, h

    def R(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                if 0 <= x < self.w and 0 <= y < self.h:
                    self.px[x, y] = c

    def P(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[x, y] = c

    def clear(self, x, y):
        self.P(x, y, (0, 0, 0, 0))


def outline(img, color=OUT):
    """1px dark outline on transparent pixels adjacent to opaque ones."""
    src = img.load()
    w, h = img.size
    res = img.copy()
    dst = res.load()
    for y in range(h):
        for x in range(w):
            if src[x, y][3] != 0:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1),
                           (1, 1), (1, -1), (-1, 1), (-1, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < h and src[nx, ny][3] != 0:
                    dst[x, y] = color
                    break
    return res


def up(img, s):
    return img.resize((img.width * s, img.height * s), Image.NEAREST)


# ==============================================================================
# CAPTAINS
# ==============================================================================
def draw_captain(cap, tier, rarm="side", eyes="open", larm="side"):
    """One 48x56 frame.
    rarm: side|mid|high|shade|guard|slash|thrust   (fight poses hold a cutlass)
    larm: side|mid|high
    eyes: open|glance|glanceL|blink
    Registration comes from the shared authored canvas (the packer pastes whole
    frames that fit), so poses may extend anywhere inside the 48x56 canvas."""
    c = Px(CAP_W, CAP_H)
    has_coat = tier >= 1
    has_hat = tier >= 2
    sleeve = cap["coat"] if has_coat else SHIRT
    sleeveh = cap["coath"] if has_coat else SHIRTS
    if tier >= 4:
        larm = "side"                            # the parrot owns that shoulder

    # ---- boots + trousers ----------------------------------------------------
    c.R(16, 45, 22, 50, TROU)
    c.R(26, 45, 32, 50, TROU)
    c.R(13, 49, 23, 50, BOOTC)
    c.R(25, 49, 35, 50, BOOTC)
    c.R(14, 51, 22, 53, BOOT)
    c.R(26, 51, 34, 53, BOOT)
    c.R(14, 54, 23, 55, BOOTS)
    c.R(26, 54, 34, 55, BOOTS)

    # ---- torso ---------------------------------------------------------------
    if has_coat:
        c.R(12, 28, 36, 47, cap["coat"])
        c.R(33, 28, 36, 47, cap["coats"])
        c.R(12, 28, 13, 47, cap["coath"])
        c.R(21, 28, 27, 36, CUFF)               # open shirt
        c.R(21, 37, 27, 41, VEST)               # waistcoat
        c.P(24, 38, GOLD); c.P(24, 40, GOLD)
        c.R(20, 28, 20, 43, GOLD)               # gold-trimmed coat edges
        c.R(28, 28, 28, 43, GOLD)
        c.R(22, 29, 24, 29, cap["skin"])        # open collar
        # baldric strap across the chest
        for i in range(21):
            x = 34 - i
            y = 29 + (i * 14) // 20
            c.P(x, y, STRAP)
            c.P(x, y + 1, STRAP)
        c.R(23, 36, 24, 37, GOLD)
    else:
        c.R(12, 28, 36, 47, SHIRT)              # deckhand shirt
        c.R(33, 28, 36, 47, SHIRTS)
        c.R(12, 28, 13, 47, CUFF)
        c.R(23, 29, 25, 40, SHIRTS)             # lacing shadow
        c.P(24, 30, WOODD); c.P(24, 33, WOODD)  # lace ties
        c.R(22, 29, 24, 29, cap["skin"])        # open collar

    # ---- waist sash with hanging knot ---------------------------------------
    c.R(12, 42, 36, 46, cap["sash"])
    c.R(12, 46, 36, 46, tuple(int(v * 0.72) for v in cap["sash"][:3]) + (255,))
    c.R(32, 46, 34, 51, cap["sash"])
    c.P(33, 52, cap["sash"])

    # ---- LEFT arm (side, or raised for the jig) --------------------------------
    if larm == "side":
        c.R(8, 30, 12, 43, sleeve)
        c.R(8, 30, 9, 43, sleeveh)
        if has_coat:
            c.R(8, 41, 12, 43, CUFF)
            c.R(8, 44, 12, 47, cap["skin"])
        else:                                    # rolled sleeves: bare forearm
            c.R(8, 38, 12, 39, CUFF)
            c.R(8, 40, 12, 47, cap["skin"])
    else:
        c.R(8, 30, 12, 33, sleeve)               # shoulder stub
        if larm == "mid":
            c.R(8, 24, 10, 30, sleeve)
            c.R(8, 23, 10, 23, CUFF)
            c.R(8, 19, 11, 22, cap["skin"])
        else:                                    # high
            c.R(8, 19, 10, 30, sleeve)
            c.R(8, 18, 10, 18, CUFF)
            c.R(8, 13, 11, 17, cap["skin"])
            c.P(12, 14, cap["skin"])

    # ---- cutlass at the left hip (t3+) ---------------------------------------
    if tier >= 3:
        c.R(10, 47, 13, 48, GOLD)
        c.P(11, 46, GOLDS)
        c.P(10, 49, BLADE); c.P(9, 49, BLADE)
        c.P(9, 50, BLADE); c.P(8, 50, BLADE)
        c.P(8, 51, BLADE); c.P(7, 51, BLADE)
        c.P(7, 52, BLADE)

    # ---- neck + head ---------------------------------------------------------
    c.R(21, 25, 27, 28, cap["skin"])
    c.R(15, 11, 33, 25, cap["skin"])
    c.R(30, 12, 33, 25, cap["skins"])
    for cx, cy in [(15, 11), (16, 11), (15, 12), (33, 11), (32, 11), (33, 12),
                   (15, 25), (33, 25)]:
        c.clear(cx, cy)
    c.R(19, 24, 29, 24, cap["skins"])

    # ---- brows ---------------------------------------------------------------
    c.R(16, 14, 20, 15, cap["hair"])
    c.P(21, 16, cap["hair"])
    c.R(28, 14, 32, 15, cap["hair"])
    c.P(27, 16, cap["hair"])

    # ---- eyes (animated, interior only) --------------------------------------
    def eye_at(x0, shift):
        c.R(x0, 17, x0 + 4, 20, EYEW)
        c.R(x0 + 2 + shift, 18, x0 + 3 + shift, 19, EYE)
        c.P(x0 + 2 + shift, 18, EYEW)

    shift = {"glance": 1, "glanceL": -1}.get(eyes, 0)
    if eyes == "blink":
        c.R(17, 17, 21, 20, cap["skin"])
        c.R(18, 19, 21, 19, EYE)
        if not cap["patch"]:
            c.R(27, 17, 31, 20, cap["skin"])
            c.R(28, 19, 31, 19, EYE)
    else:
        eye_at(17, shift)
        if not cap["patch"]:
            eye_at(27, shift)
    if cap["female"]:                            # outer-corner lashes
        c.P(16, 17, EYE)
        if not cap["patch"]:
            c.P(32, 17, EYE)
    if cap["patch"]:                             # eyepatch + strap (static)
        c.R(27, 16, 31, 20, PATCH)
        c.R(15, 16, 26, 16, PATCH)
        c.P(32, 15, PATCH); c.P(33, 14, PATCH)
    c.P(23, 21, cap["skins"]); c.P(24, 21, cap["skins"])   # nose

    # ---- mouth / beard --------------------------------------------------------
    if cap["beard"]:
        c.R(15, 20, 33, 27, cap["hair"])
        c.R(17, 28, 31, 29, cap["hair"])
        c.R(19, 30, 29, 30, cap["hair"])
        c.P(16, 28, cap["hair"]); c.P(32, 28, cap["hair"])
        c.R(21, 22, 27, 23, (40, 22, 22, 255))
        c.R(22, 22, 26, 22, CUFF)
        c.P(25, 22, GOLD)
        c.R(20, 20, 28, 20, cap["hair"])
        c.P(19, 21, cap["hair"]); c.P(29, 21, cap["hair"])
    else:
        c.R(21, 22, 26, 22, MOUTH)
        c.P(27, 21, MOUTH)
        c.R(22, 23, 26, 23, CUFF)

    # ---- ear + earring --------------------------------------------------------
    c.R(14, 18, 15, 21, cap["skin"])
    c.P(13, 20, GOLD); c.P(13, 21, GOLD); c.P(14, 22, GOLD)

    # ---- hair / bandana -------------------------------------------------------
    if cap["bandana"]:
        if has_hat:
            c.R(14, 9, 34, 12, cap["bandana"])
        else:                                    # full wrap when no hat
            c.R(14, 7, 34, 12, cap["bandana"])
            c.clear(14, 7); c.clear(34, 7)
        c.P(35, 11, cap["bandana"]); c.P(36, 12, cap["bandana"])
        c.P(37, 13, cap["bandana"]); c.P(37, 14, cap["bandana"])
        c.P(36, 15, cap["bandana"])
        if cap["sidehair"]:
            c.R(13, 13, 15, 27, cap["hair"])
            c.R(33, 13, 35, 27, cap["hair"])
            c.P(12, 27, cap["hair"]); c.P(36, 27, cap["hair"])
    else:
        if has_hat:
            c.R(16, 11, 32, 12, cap["hair"])     # fringe under the hat
        else:                                    # bare head: full hair top
            c.R(16, 8, 32, 12, cap["hair"])
            c.clear(16, 8); c.clear(32, 8)
        if cap["sidehair"]:
            c.R(13, 13, 15, 27, cap["hair"])
            c.R(33, 13, 35, 27, cap["hair"])
            c.P(12, 27, cap["hair"]); c.P(36, 27, cap["hair"])
        else:
            c.R(14, 13, 15, 19, cap["hair"])     # short sideburns
            c.R(33, 13, 34, 19, cap["hair"])

    # ---- pirate tricorn (t2+): swept scalloped shape ---------------------------
    if has_hat:
        c.R(10, 3, 38, 9, cap["hat"])
        c.R(32, 4, 38, 9, cap["hats"])
        for x in range(17, 21):
            c.clear(x, 3)
        for x in range(28, 32):
            c.clear(x, 3)
        c.clear(10, 3); c.clear(38, 3)
        c.P(10, 2, cap["hat"]); c.P(11, 2, cap["hat"])
        c.P(37, 2, cap["hats"]); c.P(38, 2, cap["hats"])
        if tier >= 4:                            # skull cockade
            c.R(22, 4, 26, 6, CUFF)
            c.P(23, 5, OUT); c.P(25, 5, OUT)
            c.P(22, 4, cap["hat"]); c.P(26, 4, cap["hat"])
        if tier >= 3:                            # red feather
            c.P(36, 1, FEATH); c.P(37, 0, FEATH); c.P(38, 0, FEATH)
            c.P(37, 1, FEATH); c.P(35, 2, FEATH)

    # ---- parrot on the left shoulder + medal (t4) ------------------------------
    if tier >= 4:
        c.R(10, 23, 15, 29, (210, 60, 50, 255))          # body
        c.R(11, 20, 15, 24, (210, 60, 50, 255))          # head
        c.P(16, 22, GOLD); c.P(17, 22, GOLDS)            # beak
        c.P(13, 21, OUT)                                 # eye
        c.R(10, 25, 12, 28, (44, 152, 82, 255))          # wing
        c.P(9, 27, (44, 152, 82, 255))                   # tail tip
        c.P(22, 39, FEATH); c.R(21, 40, 23, 41, GOLD)    # medal on waistcoat

    # ---- RIGHT arm (animated pose; fight poses hold a cutlass) -----------------
    if rarm == "side":
        c.R(36, 30, 40, 43, sleeve)
        c.R(39, 30, 40, 43, cap["coats"] if has_coat else SHIRTS)
        if has_coat:
            c.R(36, 41, 40, 43, CUFF)
            c.R(36, 44, 40, 47, cap["skin"])
        else:
            c.R(36, 38, 40, 39, CUFF)
            c.R(36, 40, 40, 47, cap["skin"])
    elif rarm in ("mid", "high"):
        c.R(36, 30, 40, 33, sleeve)              # shoulder stub
        if rarm == "mid":
            c.R(38, 24, 40, 30, sleeve)          # raised forearm
            c.R(38, 23, 40, 23, CUFF)
            c.R(37, 19, 40, 22, cap["skin"])     # hand
        else:                                    # high
            c.R(38, 19, 40, 30, sleeve)
            c.R(38, 18, 40, 18, CUFF)
            c.R(37, 13, 40, 17, cap["skin"])
            c.P(36, 14, cap["skin"])             # open palm hint
    elif rarm == "shade":                        # hand shading the eyes
        c.R(36, 30, 40, 33, sleeve)
        c.R(38, 17, 40, 30, sleeve)
        c.R(38, 16, 40, 16, CUFF)
        c.R(32, 13, 39, 15, cap["skin"])         # flat hand over the brow
    elif rarm == "guard":                        # cutlass at the ready
        c.R(36, 30, 40, 38, sleeve)
        c.R(36, 37, 40, 38, CUFF)
        c.R(38, 26, 40, 29, cap["skin"])         # hand gripping high
        c.R(38, 24, 41, 25, GOLD)                # crossguard
        for i, (bx, by) in enumerate([(41, 23), (42, 22), (43, 21), (44, 20),
                                      (45, 19)]):
            c.P(bx, by, BLADE)
            c.P(bx + 1, by, BLADE)
        c.P(46, 18, BLADE)
    elif rarm == "slash":                        # blade swung overhead
        c.R(36, 30, 40, 33, sleeve)
        c.R(38, 19, 40, 30, sleeve)
        c.R(38, 18, 40, 18, CUFF)
        c.R(37, 14, 40, 17, cap["skin"])
        c.R(37, 12, 41, 13, GOLD)                # crossguard
        c.R(38, 5, 40, 11, BLADE)                # blade straight up
        c.R(38, 4, 39, 4, BLADE)
    else:                                        # thrust: blade straight out
        c.R(36, 31, 40, 34, sleeve)
        c.R(36, 31, 36, 34, sleeveh)
        c.R(41, 31, 43, 34, cap["skin"])         # fist
        c.R(44, 30, 44, 35, GOLD)                # crossguard
        c.R(45, 31, 46, 32, BLADE)
        c.P(47, 31, BLADE)

    return up(outline(c.img), 3)


# ==============================================================================
# SHIPS
# ==============================================================================
def draw_ship(tier):
    """Ships sit ON the waterline: the firmware blits the 150x130 canvas at
    y=84 and the sea horizon is y=150 (canvas row ~22 at 3x = native row ~29 of
    43), so all hulls bottom out at native row 29 and the rows below stay
    transparent -- otherwise the hull hides behind the foreground deck."""
    c = Px(SHIP_W, SHIP_H)
    WL = 29                                      # waterline row

    def mast(mx, ytop, ybot):
        c.R(mx, ytop, mx + 1, ybot, WOODD)

    def square_sail(mx, y0, y1, half):
        c.R(mx - half, y0, mx + 1 + half, y1, SAIL)
        c.R(mx - half, y0 + (y1 - y0) // 2 + 1, mx + 1 + half, y1, SAILS)
        c.R(mx - half, y0, mx + 1 + half, y0, WOODD)   # yard

    def pennant(mx, ytop):
        y = max(0, ytop)
        c.P(mx + 2, y, FLAG); c.P(mx + 3, y, FLAG)
        c.P(mx + 2, y + 1, FLAG)

    if tier == 0:
        for i, ly in enumerate(range(WL - 4, WL + 1, 2)):
            c.R(12, ly, 38, ly + 1, WOODH if i % 2 else WOOD)
        c.R(16, WL - 5, 17, WL, WOODD)           # lashings
        c.R(33, WL - 5, 34, WL, WOODD)
        mast(24, 10, WL - 4)
        for i in range(12):                      # lateen triangle
            y = 11 + i
            c.R(26, y, min(26 + (i * 9) // 11, 36), y,
                SAIL if i < 6 else SAILS)
        pennant(24, 8)
        return up(outline(c.img), 3)

    hw = [0, 13, 17, 20, 23][tier]               # half width at the gunwale
    top = [0, 21, 20, 19, 17][tier]
    cx = 25
    for i, y in enumerate(range(top, WL + 1)):
        shrink = (i * 5) // max(1, WL - top)
        c.R(cx - hw + shrink, y, cx + hw - shrink, y, WOOD)
    c.R(cx - hw, top, cx + hw, top, WOODH)       # gunwale
    c.R(cx - hw, top + 1, cx + hw, top + 1, GOLD)  # gold stripe
    if tier >= 3:                                # gunports
        for gx in range(cx - hw + 4, cx + hw - 3, 6):
            c.R(gx, top + 3, gx + 1, top + 4, WOODD)
    if tier >= 4:
        c.R(cx - hw + 1, top + 5, cx + hw - 2, top + 5, GOLD)  # second stripe
        for gx in range(cx - hw + 5, cx + hw - 4, 6):          # lower ports
            c.R(gx, top + 7, gx + 1, top + 8, WOODD)
        c.R(38, top - 5, 46, top - 1, WOODH)     # stern castle
        c.R(39, top - 4, 40, top - 3, GOLD)      # windows
        c.R(43, top - 4, 44, top - 3, GOLD)
        c.R(2, top - 2, 6, top, WOODD)           # bowsprit rise
        c.P(1, top - 3, WOODD); c.P(0, top - 4, WOODD)

    masts_x = {1: [24], 2: [16, 32], 3: [12, 24, 36], 4: [12, 24, 35]}[tier]
    mtop = [0, 2, 1, 0, 0][tier]
    half = [0, 8, 6, 5, 6][tier]
    for k, mx in enumerate(masts_x):
        t0 = mtop + (0 if k == len(masts_x) // 2 else 2)
        mast(mx, t0, top)
        if tier == 1:
            square_sail(mx, t0 + 2, top - 2, half)
        else:
            square_sail(mx, t0 + 2, t0 + 8, half - 1)
            square_sail(mx, t0 + 10, top - 1, half)
        pennant(mx, t0 - 1)
    return up(outline(c.img), 3)


# ==============================================================================
# WORLD LAYERS
# ==============================================================================
def dither(c, y, x0, x1, c1, c2):
    for x in range(x0, x1 + 1):
        c.P(x, y, c1 if (x + y) % 2 == 0 else c2)


def draw_sky():
    W, H = 64, 48
    c = Px(W, H)
    sky = [(96, 178, 232, 255), (112, 190, 238, 255),
           (130, 202, 242, 255), (156, 214, 244, 255)]
    sea = [(46, 160, 176, 255), (34, 140, 168, 255),
           (24, 116, 152, 255), (18, 96, 138, 255)]
    bands = [(0, 7), (8, 15), (16, 22), (23, 28)]
    for col, (y0, y1) in zip(sky, bands):
        c.R(0, y0, W - 1, y1, col)
    for i in range(3):                           # dithered band seams
        y = bands[i][1]
        dither(c, y, 0, W - 1, sky[i], sky[i + 1])
    c.R(0, 29, W - 1, 29, (238, 228, 196, 255))  # warm horizon line
    seab = [(30, 34), (35, 39), (40, 43), (44, 47)]
    for col, (y0, y1) in zip(sea, seab):
        c.R(0, y0, W - 1, y1, col)
    for i in range(3):
        y = seab[i][1]
        dither(c, y, 0, W - 1, sea[i], sea[i + 1])
    # blocky sun, top right
    sx, sy = 51, 7
    c.R(sx - 3, sy - 4, sx + 3, sy + 4, (255, 238, 150, 255))
    c.R(sx - 4, sy - 3, sx + 4, sy + 3, (255, 238, 150, 255))
    c.R(sx - 2, sy - 5, sx + 2, sy + 5, (255, 238, 150, 255))
    c.R(sx - 1, sy - 2, sx + 2, sy + 1, (255, 250, 208, 255))
    # glitter trail under the sun + scattered sparkles
    for gy in range(31, 42, 2):
        c.P(sx - 1 + (gy % 3), gy, (214, 240, 244, 255))
        c.P(sx + 2 - (gy % 2), gy + 1, (170, 214, 226, 255))
    for gx, gy in [(8, 33), (18, 37), (28, 32), (38, 41), (12, 43), (33, 45)]:
        c.P(gx, gy, (170, 214, 226, 255))
    return up(c.img, 5).convert("RGB")


def draw_cloud(c, x, y, w, h):
    c.R(x, y + 2, x + w, y + h, (250, 252, 255, 255))
    c.R(x + 2, y, x + w - 3, y + 2, (250, 252, 255, 255))
    c.clear(x, y + 2); c.clear(x + w, y + 2)
    c.R(x + 1, y + h, x + w - 1, y + h, (208, 222, 236, 255))   # flat shadow base


def draw_clouds():
    c = Px(96, 20)
    draw_cloud(c, 6, 8, 15, 4)
    draw_cloud(c, 40, 3, 19, 5)
    draw_cloud(c, 74, 11, 10, 3)
    return up(outline(c.img), 5)


def draw_waves():
    c = Px(96, 14)
    foam = (222, 244, 248, 255)
    mid = (164, 216, 226, 255)
    for row, yy in enumerate((2, 6, 10)):
        for x in range(row * 4, 96 - 6, 12):
            c.R(x, yy, x + 4, yy, foam)
            c.P(x + 5, yy + 1, mid)
            c.P(x + 6, yy + 1, mid)
            c.P(x - 1, yy + 1, mid)
    return up(c.img, 5)


def draw_deck():
    c = Px(64, 18)
    c.R(0, 6, 63, 6, WOODH)                      # lit front edge
    c.R(0, 7, 63, 17, WOOD)
    for x in range(7, 64, 8):                    # plank seams
        c.R(x, 7, x, 17, WOODD)
    for x in range(3, 64, 8):                    # nail dots
        c.P(x, 9, WOODD)
    c.R(0, 12, 63, 12, WOODD)                    # plank row line
    return up(c.img, 5)


# ==============================================================================
def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "artpack_pixel_src"
    os.makedirs(src, exist_ok=True)
    n = 0

    def put(img, name):
        nonlocal n
        img.save(os.path.join(src, name), "PNG")
        n += 1

    put(draw_sky(), "sky.png")
    put(draw_clouds(), "clouds.png")
    put(draw_waves(), "waves.png")
    put(draw_deck(), "deck.png")
    for t in range(5):
        put(draw_ship(t), f"ship_t{t}.png")

    # clip -> [(larm, rarm, eyes), ...]  (fight poses draw the cutlass in hand)
    clips = {
        "idle": [("side", "side", "open"), ("side", "side", "glance"),
                 ("side", "side", "blink")],
        "wave": [("side", "side", "open"), ("side", "mid", "open"),
                 ("side", "high", "open")],
        "fight": [("side", "guard", "open"), ("side", "slash", "open"),
                  ("side", "thrust", "open")],
        "jig": [("mid", "side", "open"), ("side", "mid", "glance"),
                ("high", "high", "open")],
        "look": [("side", "shade", "glanceL"), ("side", "shade", "open"),
                 ("side", "shade", "glance")],
    }
    for i, cap in enumerate(CAPTAINS):
        for t in range(5):
            for clip, frames in clips.items():
                for ph, (la, ra, e) in enumerate(frames):
                    put(draw_captain(cap, t, rarm=ra, eyes=e, larm=la),
                        f"cap{i}_t{t}_{clip}_{ph}.png")

    print(f"Wrote {n} source image(s) to {src}/")
    print("Next:")
    print(f"  python tools/build_artpack.py {src} artpack_pixel")
    print("  copy artpack_pixel/* to the SD card's /art/ folder, then reboot")


if __name__ == "__main__":
    main()
