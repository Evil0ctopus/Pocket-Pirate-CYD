#!/usr/bin/env python3
"""PIXEL-ART captain style probe v3 -- PIRATE pass.

v2 read as pilgrims (flat-brimmed tall hat, buttoned-up coat, white collar).
v3 swaps in the pirate formula: swept-up scalloped tricorn with skull + feather,
OPEN coat with gold-trimmed edges over a shirt and waistcoat, a knotted waist
sash instead of a neat belt, a baldric strap across the chest, a cutlass at the
hip, and folded-cuff boots. Faces/personality from v2 kept.

Still native 48x56, strict palette, auto 1px outline, NEAREST upscale.

Output: pixel_proto.png -- Mara Tide and Roan Redsail at 6x, on-device size on
sea, and 1:1.

Requires Pillow:  pip install pillow
"""

import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required.  Install it with:  pip install pillow")

NAT_W, NAT_H = 48, 56

OUT = (24, 18, 28, 255)
GOLD = (244, 202, 78, 255)
GOLDS = (196, 150, 44, 255)
CUFF = (240, 236, 222, 255)
EYEW = (250, 250, 246, 255)
EYE = (28, 24, 34, 255)
MOUTH = (120, 44, 40, 255)
TROU = (44, 58, 70, 255)
BOOT = (70, 48, 34, 255)
BOOTC = (112, 80, 52, 255)      # folded boot cuff
BOOTS = (46, 32, 22, 255)
VEST = (46, 38, 54, 255)        # dark waistcoat
STRAP = (58, 40, 28, 255)       # leather baldric
BLADE = (206, 214, 224, 255)
FEATH = (216, 60, 66, 255)

MARA = dict(
    name="Mara", skin=(240, 198, 160, 255), skins=(198, 150, 118, 255),
    hair=(92, 58, 36, 255), coat=(30, 132, 128, 255), coats=(18, 90, 88, 255),
    coath=(82, 184, 176, 255), hat=(38, 50, 88, 255), hats=(24, 32, 60, 255),
    sash=(198, 54, 64, 255), bandana=None, beard=False,
)
ROAN = dict(
    name="Roan", skin=(232, 180, 145, 255), skins=(190, 138, 106, 255),
    hair=(188, 96, 40, 255), coat=(150, 38, 34, 255), coats=(104, 24, 22, 255),
    coath=(204, 84, 72, 255), hat=(38, 34, 34, 255), hats=(22, 20, 20, 255),
    sash=(212, 162, 52, 255), bandana=(200, 56, 48, 255), beard=True,
)


def draw_captain(c):
    img = Image.new("RGBA", (NAT_W, NAT_H), (0, 0, 0, 0))
    px = img.load()

    def R(x0, y0, x1, y1, col):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                if 0 <= x < NAT_W and 0 <= y < NAT_H:
                    px[x, y] = col

    def P(x, y, col):
        if 0 <= x < NAT_W and 0 <= y < NAT_H:
            px[x, y] = col

    def clear(x, y):
        P(x, y, (0, 0, 0, 0))

    # ---- boots with folded cuffs, trousers ----------------------------------
    R(16, 45, 22, 50, TROU)
    R(26, 45, 32, 50, TROU)
    R(13, 49, 23, 50, BOOTC)
    R(25, 49, 35, 50, BOOTC)
    R(14, 51, 22, 53, BOOT)
    R(26, 51, 34, 53, BOOT)
    R(14, 54, 23, 55, BOOTS)
    R(26, 54, 34, 55, BOOTS)

    # ---- OPEN coat over shirt + waistcoat -----------------------------------
    R(12, 28, 36, 47, c["coat"])
    R(33, 28, 36, 47, c["coats"])
    R(12, 28, 13, 47, c["coath"])
    R(21, 28, 27, 36, CUFF)             # open shirt
    R(21, 37, 27, 41, VEST)             # dark waistcoat
    P(24, 38, GOLD); P(24, 40, GOLD)    # waistcoat buttons
    R(20, 28, 20, 43, GOLD)             # gold-trimmed coat edges
    R(28, 28, 28, 43, GOLD)
    P(22, 29, c["skin"]); P(23, 29, c["skin"])  # open collar, bit of chest
    P(24, 29, c["skin"])

    # ---- baldric strap across the chest -------------------------------------
    for i in range(21):
        x = 34 - i
        y = 29 + (i * 14) // 20
        P(x, y, STRAP)
        P(x, y + 1, STRAP)
    R(23, 36, 24, 37, GOLD)             # strap buckle

    # ---- waist sash with hanging knot ---------------------------------------
    R(12, 42, 36, 46, c["sash"])
    R(12, 46, 36, 46, tuple(int(v * 0.72) for v in c["sash"][:3]) + (255,))
    R(32, 46, 34, 51, c["sash"])        # knot tail
    P(33, 52, c["sash"])

    # ---- arms + rolled cuffs + hands ----------------------------------------
    R(8, 30, 12, 43, c["coat"])
    R(8, 30, 9, 43, c["coath"])
    R(8, 41, 12, 43, CUFF)
    R(8, 44, 12, 47, c["skin"])
    R(36, 30, 40, 43, c["coat"])
    R(39, 30, 40, 43, c["coats"])
    R(36, 41, 40, 43, CUFF)
    R(36, 44, 40, 47, c["skin"])

    # ---- cutlass hanging at the left hip ------------------------------------
    R(10, 47, 13, 48, GOLD)             # guard
    P(11, 46, GOLDS)                    # grip peeking above
    P(10, 49, BLADE); P(9, 49, BLADE)
    P(9, 50, BLADE); P(8, 50, BLADE)
    P(8, 51, BLADE); P(7, 51, BLADE)
    P(7, 52, BLADE)

    # ---- neck ---------------------------------------------------------------
    R(21, 25, 27, 28, c["skin"])

    # ---- head ---------------------------------------------------------------
    R(15, 11, 33, 25, c["skin"])
    R(30, 12, 33, 25, c["skins"])
    for cx, cy in [(15, 11), (16, 11), (15, 12), (33, 11), (32, 11), (33, 12),
                   (15, 25), (33, 25)]:
        clear(cx, cy)
    R(19, 24, 29, 24, c["skins"])

    # ---- eyes + brows (kept from v2) ----------------------------------------
    R(17, 17, 21, 20, EYEW)
    R(19, 18, 21, 19, EYE)
    P(19, 18, EYEW)
    R(27, 17, 31, 20, EYEW)
    R(28, 18, 30, 19, EYE)
    P(28, 18, EYEW)
    R(16, 14, 20, 15, c["hair"])
    P(21, 16, c["hair"])
    R(28, 14, 32, 15, c["hair"])
    P(27, 16, c["hair"])
    P(23, 21, c["skins"]); P(24, 21, c["skins"])

    # ---- mouth / beard ------------------------------------------------------
    if c["beard"]:
        R(15, 20, 33, 27, c["hair"])
        R(17, 28, 31, 29, c["hair"])
        R(19, 30, 29, 30, c["hair"])
        P(16, 28, c["hair"]); P(32, 28, c["hair"])
        R(21, 22, 27, 23, (40, 22, 22, 255))
        R(22, 22, 26, 22, CUFF)
        P(25, 22, GOLD)
        R(20, 20, 28, 20, c["hair"])
        P(19, 21, c["hair"]); P(29, 21, c["hair"])
    else:
        R(21, 22, 26, 22, MOUTH)
        P(27, 21, MOUTH)
        R(22, 23, 26, 23, CUFF)

    # ---- ear + gold hoop earring --------------------------------------------
    R(14, 18, 15, 21, c["skin"])
    P(13, 20, GOLD); P(13, 21, GOLD); P(14, 22, GOLD)

    # ---- hair or bandana under the hat --------------------------------------
    if c["bandana"]:
        R(14, 9, 34, 12, c["bandana"])
        P(35, 11, c["bandana"]); P(36, 12, c["bandana"])
        P(37, 13, c["bandana"]); P(37, 14, c["bandana"])
        P(36, 15, c["bandana"])
    else:
        R(16, 11, 32, 12, c["hair"])
        R(13, 13, 15, 27, c["hair"])
        R(33, 13, 35, 27, c["hair"])
        P(12, 27, c["hair"]); P(36, 27, c["hair"])

    # ---- PIRATE tricorn: one wide swept shape, scalloped top into 3 points,
    #      skull front-center, red feather -- no flat pilgrim brim ------------
    R(10, 3, 38, 9, c["hat"])
    R(32, 4, 38, 9, c["hats"])
    # scallop the top edge: clear two dips -> three upswept points
    for x in range(17, 21):
        clear(x, 3)
    for x in range(28, 32):
        clear(x, 3)
    # round outer corners, flare the tips upward
    clear(10, 3); clear(38, 3)
    P(10, 2, c["hat"]); P(11, 2, c["hat"])
    P(37, 2, c["hats"]); P(38, 2, c["hats"])
    # skull emblem front-center
    R(22, 4, 26, 6, CUFF)
    P(23, 5, OUT); P(25, 5, OUT)
    P(22, 4, c["hat"]); P(26, 4, c["hat"])
    # red feather off the right point
    P(36, 1, FEATH); P(37, 0, FEATH); P(38, 0, FEATH)
    P(37, 1, FEATH); P(35, 2, FEATH)

    # ---- auto 1px outline ---------------------------------------------------
    src = img.load()
    outl = img.copy()
    dst = outl.load()
    for y in range(NAT_H):
        for x in range(NAT_W):
            if src[x, y][3] != 0:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1),
                           (1, 1), (1, -1), (-1, 1), (-1, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < NAT_W and 0 <= ny < NAT_H and src[nx, ny][3] != 0:
                    dst[x, y] = OUT
                    break
    return outl


def sea_cell(w, h):
    cell = Image.new("RGB", (w, h))
    p = cell.load()
    for y in range(h):
        band = (118, 194, 234) if y < h * 0.42 else (
            (34, 150, 172) if y < h * 0.6 else (20, 96, 140))
        for x in range(w):
            p[x, y] = band
    return cell


def main():
    caps = [draw_captain(MARA), draw_captain(ROAN)]
    sheet = Image.new("RGB", (960, 360), (22, 26, 34))
    for i, sp in enumerate(caps):
        big = sp.resize((NAT_W * 6, NAT_H * 6), Image.NEAREST)
        sheet.paste(big, (14 + i * 310, 12), big)
        dev = sp.resize((NAT_W * 3, NAT_H * 3), Image.NEAREST)
        cell = sea_cell(150, 200)
        cell.paste(dev, ((150 - dev.width) // 2, 200 - dev.height - 6), dev)
        sheet.paste(cell, (640 + i * 160, 12))
        one = sp.resize((NAT_W, NAT_H), Image.NEAREST)
        sheet.paste(one, (660 + i * 160, 240), one)

    out = os.path.join(os.getcwd(), "pixel_proto.png")
    sheet.save(out)
    print(f"Wrote {out}")


if __name__ == "__main__":
    main()
