#!/usr/bin/env python3
"""Generate a known-good SAMPLE art pack for Pocket Pirate CYD.

This is a *smoke-test / reference* pack, not the final look. It renders tidy,
anti-aliased placeholder art (parallax sky/clouds/waves/deck, five ship tiers,
and animated captain sprites for the four-captain roster) so you can validate
the whole pipeline on real hardware -- build_artpack.py packing, SD /art/
decoding, sprite-sheet animation, parallax scroll and tap-to-wave -- before you
invest in AI-generated illustration.

Usage:
    python tools/make_sample_pack.py [src_dir]        # default: artpack_sample_src
    python tools/build_artpack.py artpack_sample_src artpack_sample
    # then copy artpack_sample/* to the SD card's /art/ folder and reboot

Palettes mirror include/captains.h so the placeholder crew reads like the real
roster. Every captain frame is composited over an identical silhouette base so
its bounding box is constant -- that keeps content_bbox_center() in the packer
from drifting the subject between animation frames.

Requires Pillow:  pip install pillow
"""

import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is required.  Install it with:  pip install pillow")

SS = 3  # supersample factor for smooth anti-aliased edges


def canvas(w, h):
    return Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))


def down(img, w, h):
    return img.resize((w, h), Image.LANCZOS)


def s(v):
    return int(round(v * SS))


def vgrad(w, h, top, bot):
    """Opaque vertical gradient, full res (no supersample needed)."""
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        r = int(top[0] + (bot[0] - top[0]) * t)
        g = int(top[1] + (bot[1] - top[1]) * t)
        b = int(top[2] + (bot[2] - top[2]) * t)
        for x in range(w):
            px[x, y] = (r, g, b)
    return img


# --- roster palettes (from include/captains.h) ------------------------------
CAPTAINS = [
    dict(name="Mara Tide", skin=(240, 197, 160), hair=(90, 55, 35),
         coat=(28, 122, 120), trim=(240, 198, 70), hat=(30, 42, 78),
         bandana=None, beard=False, patch=False),
    dict(name="Roan Redsail", skin=(232, 180, 145), hair=(178, 92, 40),
         coat=(176, 46, 40), trim=(240, 198, 70), hat=(34, 30, 30),
         bandana=(190, 52, 46), beard=True, patch=False),
    dict(name="Iyla Dawn", skin=(246, 210, 178), hair=(214, 176, 96),
         coat=(40, 78, 150), trim=(240, 198, 70), hat=(120, 70, 160),
         bandana=(150, 92, 190), beard=False, patch=False),
    dict(name="Bram Kettle", skin=(204, 156, 120), hair=(196, 196, 196),
         coat=(46, 104, 60), trim=(240, 198, 70), hat=(74, 54, 36),
         bandana=None, beard=True, patch=True),
]

CAP_W, CAP_H = 180, 210  # render size == packer CAP_CANVAS (no rescale)


def draw_captain_base(cap, tier):
    """Silhouette shared by every frame of one captain at one tier. Accessories
    accrue with the tier (coat -> belt -> tricorn -> telescope/epaulets ->
    parrot/medal), mirroring the on-device level-up evolution. Everything here is
    static; the animated eyes and waving arm are drawn on top and kept strictly
    inside this footprint, so every frame of every clip shares one bounding box
    (no drift when the packer bbox-centers them, and no jump when a wave starts)."""
    img = canvas(CAP_W, CAP_H)
    d = ImageDraw.Draw(img)
    cx = CAP_W / 2

    def darker(c, f=0.62):
        return tuple(int(v * f) for v in c)

    has_coat = tier >= 1
    torso = cap["coat"] if has_coat else (228, 219, 198)

    # boots (lowest point -> constant bottom anchor across all tiers)
    for bx in (-15, 15):
        d.rounded_rectangle([s(cx + bx - 12), s(184), s(cx + bx + 12), s(204)],
                            radius=s(7), fill=(52, 36, 24))
    # legs
    d.rounded_rectangle([s(cx - 25), s(150), s(cx - 5), s(188)], radius=s(8),
                        fill=darker(cap["coat"], 0.45))
    d.rounded_rectangle([s(cx + 5), s(150), s(cx + 25), s(188)], radius=s(8),
                        fill=darker(cap["coat"], 0.45))

    # torso: full coat (t1+) or a plain deckhand vest (t0)
    if has_coat:
        d.rounded_rectangle([s(cx - 44), s(96), s(cx + 44), s(178)], radius=s(20),
                            fill=torso)
        d.rounded_rectangle([s(cx - 44), s(150), s(cx + 44), s(178)], radius=s(20),
                            fill=darker(torso, 0.78))
        d.rectangle([s(cx - 5), s(100), s(cx + 5), s(176)], fill=darker(torso, 0.7))
        for by in (112, 130, 148):
            d.ellipse([s(cx - 4), s(by - 4), s(cx + 4), s(by + 4)], fill=cap["trim"])
        # belt
        d.rectangle([s(cx - 44), s(150), s(cx + 44), s(160)], fill=(44, 30, 20))
        d.rounded_rectangle([s(cx - 8), s(149), s(cx + 8), s(161)], radius=s(3),
                            fill=cap["trim"])
    else:
        # deckhand vest: full shoulder width (so the silhouette bbox matches the
        # coated tiers and frames still register) but shorter, lighter, with a
        # color sash and an exposed shirt V-neck to read clearly as "no coat".
        d.rounded_rectangle([s(cx - 44), s(96), s(cx + 44), s(150)], radius=s(18),
                            fill=torso)
        d.rounded_rectangle([s(cx - 44), s(140), s(cx + 44), s(150)], radius=s(10),
                            fill=darker(torso, 0.8))
        d.polygon([(s(cx - 34), s(100)), (s(cx + 6), s(150)), (s(cx - 34), s(150))],
                  fill=cap["coat"])
        d.polygon([(s(cx - 10), s(100)), (s(cx + 10), s(100)), (s(cx), s(120))],
                  fill=(238, 232, 214))

    # neck + head + beard
    d.rectangle([s(cx - 10), s(84), s(cx + 10), s(100)], fill=cap["skin"])
    d.ellipse([s(cx - 32), s(40), s(cx + 32), s(104)], fill=cap["skin"])
    if cap["beard"]:
        d.chord([s(cx - 30), s(58), s(cx + 30), s(112)], 20, 160, fill=cap["hair"])

    # bandana (identity, always) then tricorn hat (t2+)
    if cap["bandana"]:
        d.pieslice([s(cx - 34), s(30), s(cx + 34), s(78)], 180, 360,
                   fill=cap["bandana"])
        d.polygon([(s(cx + 30), s(52)), (s(cx + 44), s(58)), (s(cx + 30), s(64))],
                  fill=cap["bandana"])
    if tier >= 2:
        d.ellipse([s(cx - 46), s(34), s(cx + 46), s(58)], fill=cap["hat"])
        d.pieslice([s(cx - 30), s(18), s(cx + 30), s(64)], 180, 360, fill=cap["hat"])
        d.arc([s(cx - 46), s(34), s(cx + 46), s(58)], 180, 360, fill=cap["trim"],
              width=s(2))

    # gold epaulets (t3+)
    if tier >= 3:
        for ex in (-39, 39):
            d.ellipse([s(cx + ex - 8), s(98), s(cx + ex + 8), s(108)], fill=cap["trim"])

    # left (static) arm + hand -- sleeve matches the torso
    sleeve = cap["coat"] if has_coat else (206, 196, 176)
    d.rounded_rectangle([s(cx - 52), s(104), s(cx - 34), s(150)], radius=s(9),
                        fill=sleeve)
    d.ellipse([s(cx - 54), s(146), s(cx - 34), s(166)], fill=cap["skin"])

    # brass telescope held across the chest (t3+)
    if tier >= 3:
        d.line([(s(cx - 46), s(158)), (s(cx - 8), s(112))], fill=(60, 42, 26), width=s(11))
        d.line([(s(cx - 46), s(158)), (s(cx - 8), s(112))], fill=(196, 150, 70), width=s(7))
        d.ellipse([s(cx - 14), s(104), s(cx + 0), s(118)], fill=(150, 110, 50))

    # medal on the chest (t4)
    if tier >= 4:
        d.polygon([(s(cx + 20), s(116)), (s(cx + 26), s(126)), (s(cx + 14), s(126))],
                  fill=(180, 40, 44))
        d.ellipse([s(cx + 14), s(124), s(cx + 26), s(136)], fill=cap["trim"])

    # parrot on the left shoulder (t4) -- stays inside the arm's left extent
    if tier >= 4:
        px, py = cx - 38, 84
        d.ellipse([s(px - 9), s(py - 12), s(px + 8), s(py + 10)], fill=(210, 60, 50))
        d.ellipse([s(px - 3), s(py - 20), s(px + 10), s(py - 6)], fill=(210, 60, 50))
        d.polygon([(s(px + 9), s(py - 16)), (s(px + 17), s(py - 12)),
                   (s(px + 9), s(py - 8))], fill=(240, 190, 60))
        d.ellipse([s(px - 7), s(py - 6), s(px + 2), s(py + 8)], fill=(40, 150, 80))
        d.ellipse([s(px + 2), s(py - 15), s(px + 6), s(py - 11)], fill=(20, 20, 20))
    return img


def draw_eyes(d, cx, closed, cap):
    eye = (36, 30, 26)
    for ex in (cx - 12, cx + 12):
        if cap["patch"] and ex > cx:
            d.rectangle([s(ex - 8), s(70), s(ex + 8), s(78)], fill=(20, 20, 20))
            d.line([(s(ex - 12), s(66)), (s(cx + 30), s(50))], fill=(20, 20, 20),
                   width=s(2))
            continue
        if closed:
            d.line([(s(ex - 7), s(74)), (s(ex + 7), s(74))], fill=eye, width=s(3))
        else:
            d.ellipse([s(ex - 5), s(70), s(ex + 5), s(80)], fill=(250, 250, 250))
            d.ellipse([s(ex - 3), s(72), s(ex + 3), s(78)], fill=eye)


def draw_captain_frame(cap, tier, clip, phase):
    """clip 'idle': phase 0..2 (blink on 2). clip 'wave': phase 0..2 arm rise.
    The waving hand is capped inside the coat's right edge so the frame bbox
    equals the idle bbox exactly (no horizontal drift, no jump on wave start)."""
    img = draw_captain_base(cap, tier)
    d = ImageDraw.Draw(img)
    cx = CAP_W / 2
    draw_eyes(d, cx, closed=(clip == "idle" and phase == 2), cap=cap)
    d.arc([s(cx - 10), s(80), s(cx + 10), s(92)], 20, 160, fill=(120, 60, 50),
          width=s(2))  # smile
    if clip == "wave":
        sleeve = cap["coat"] if tier >= 1 else (206, 196, 176)
        hy = [140, 108, 76][phase]
        hx = cx + 18 + phase * 5           # hand right edge (hx+11) stays < coat (cx+44)
        d.line([(s(cx + 28), s(120)), (s(hx), s(hy))], fill=sleeve, width=s(12))
        d.ellipse([s(hx - 11), s(hy - 11), s(hx + 11), s(hy + 11)], fill=cap["skin"])
    return down(img, CAP_W, CAP_H)


def draw_ship(tier):
    """Five visually distinct tiers: raft -> sloop -> brig -> frigate -> galleon.
    Masts, sails-per-mast, hull size and detailing all grow with the tier."""
    W, H = 150, 130
    img = canvas(W, H)
    d = ImageDraw.Draw(img)
    cx = W / 2
    wood = (120, 82, 46)
    woodhi = (150, 108, 66)
    wooddk = (86, 58, 32)
    sail = (245, 240, 226)
    sailsh = (214, 206, 186)
    trim = (240, 198, 70)

    if tier == 0:
        # raft: bound logs + tiny lateen sail, no hull
        for i in range(-2, 3):
            d.rounded_rectangle([s(cx + i * 15 - 7), s(96), s(cx + i * 15 + 7), s(122)],
                                radius=s(6), fill=woodhi if i % 2 else wood)
        d.rectangle([s(cx - 2), s(52), s(cx + 2), s(98)], fill=wooddk)
        d.polygon([(s(cx + 2), s(54)), (s(cx + 2), s(92)), (s(cx + 30), s(74))],
                  fill=sail)
        d.polygon([(s(cx + 2), s(54)), (s(cx + 2), s(72)), (s(cx + 30), s(74))],
                  fill=sailsh)
        return down(img, W, H)

    # hull grows with tier
    hull_w = 34 + tier * 12
    d.polygon([(s(cx - hull_w), s(94)), (s(cx + hull_w), s(94)),
               (s(cx + hull_w - 16), s(122)), (s(cx - hull_w + 16), s(122))],
              fill=wood)
    d.rectangle([s(cx - hull_w), s(88), s(cx + hull_w), s(96)], fill=woodhi)
    d.line([(s(cx - hull_w), s(94)), (s(cx + hull_w), s(94))], fill=trim, width=s(3))
    # gunports appear on frigate+ ; a stern castle on the galleon
    if tier >= 3:
        for gx in range(int(cx - hull_w + 14), int(cx + hull_w - 10), 16):
            d.rectangle([s(gx), s(100), s(gx + 7), s(107)], fill=wooddk)
    if tier >= 4:
        d.rectangle([s(cx + hull_w - 18), s(78), s(cx + hull_w + 2), s(96)], fill=woodhi)
        d.polygon([(s(cx - hull_w - 12), s(94)), (s(cx - hull_w + 2), s(84)),
                   (s(cx - hull_w + 2), s(96))], fill=wooddk)  # bowsprit rise

    masts = [0, 1, 2, 3, 3][tier]
    rows = [0, 2, 2, 3, 3][tier]      # square sails stacked per mast
    sw = 15 + tier * 2                # sail half-width
    spacing = min(46, (2 * hull_w - 20) / max(1, masts - 1)) if masts > 1 else 0
    x0 = cx - spacing * (masts - 1) / 2
    mast_top = 20 if tier >= 4 else 26
    for m in range(masts):
        mx = x0 + m * spacing
        top = mast_top + (0 if m == (masts // 2) else 6)
        d.rectangle([s(mx - 2), s(top), s(mx + 2), s(92)], fill=wooddk)
        for j in range(rows):
            sy = top + 6 + j * 26
            w = sw - j * 2
            d.rounded_rectangle([s(mx - w), s(sy), s(mx + w), s(sy + 22)],
                                radius=s(4), fill=sail if j % 2 == 0 else sailsh)
            d.line([(s(mx - w), s(sy)), (s(mx + w), s(sy))], fill=wooddk, width=s(2))
        d.polygon([(s(mx), s(top)), (s(mx + 13), s(top + 5)), (s(mx), s(top + 10))],
                  fill=(200, 60, 54))  # pennant
    return down(img, W, H)


def draw_sky():
    W, H = 320, 240
    horizon = 150
    base = Image.new("RGB", (W, H))
    base.paste(vgrad(W, horizon, (120, 196, 236), (232, 224, 190)), (0, 0))
    base.paste(vgrad(W, H - horizon, (46, 154, 176), (18, 92, 138)), (0, horizon))
    base = base.convert("RGBA")
    hi = canvas(W, H)
    d = ImageDraw.Draw(hi)
    # sun with soft glow
    sx, sy = 262, 54
    for r in range(46, 16, -4):
        a = int(70 * (1 - (r - 16) / 34))
        d.ellipse([s(sx - r), s(sy - r), s(sx + r), s(sy + r)],
                  fill=(255, 244, 190, a))
    d.ellipse([s(sx - 17), s(sy - 17), s(sx + 17), s(sy + 17)], fill=(255, 236, 150))
    # sun glitter on the sea
    for i, gy in enumerate(range(158, 205, 10)):
        w = 30 - i * 4
        d.ellipse([s(sx - w), s(gy), s(sx + w), s(gy + 4)], fill=(210, 235, 240, 90))
    base.alpha_composite(down(hi, W, H))
    return base.convert("RGB")


def _puff(d, x, y, r, col):
    for dx, dy, rr in [(-r, 4, r), (0, 0, int(r * 1.25)), (r, 3, r),
                       (int(r * 0.4), -r // 2, int(r * 0.8))]:
        d.ellipse([s(x + dx - rr), s(y + dy - rr), s(x + dx + rr), s(y + dy + rr)],
                  fill=col)


def draw_clouds():
    W, H = 480, 100
    img = canvas(W, H)
    d = ImageDraw.Draw(img)
    # kept clear of the x=0/W seam so the twice-drawn wrap has no hard edge
    for x, y, r in [(70, 46, 22), (180, 34, 26), (300, 52, 20), (400, 38, 24)]:
        _puff(d, x, y, r, (255, 255, 255, 235))
    return down(img, W, H)


def draw_waves():
    W, H = 480, 70
    img = canvas(W, H)
    d = ImageDraw.Draw(img)
    foam = (210, 236, 240, 150)
    for row, yy in enumerate((14, 34, 54)):
        for x in range(30 + row * 18, W - 30, 76):
            d.arc([s(x), s(yy), s(x + 46), s(yy + 20)], 200, 340, fill=foam,
                  width=s(3))
    return down(img, W, H)


def draw_deck():
    W, H = 320, 90
    img = canvas(W, H)
    d = ImageDraw.Draw(img)
    wood = (122, 84, 48)
    woodhi = (150, 108, 66)
    # Low foreground floor: only the bottom ~40px is opaque so the sea, waves and
    # ship's hull stay visible above it (it renders on-device at y=150..240, so
    # keeping content low means the deck reads as a floor edge, not a wall).
    top = 52
    d.polygon([(s(-40), s(top + 6)), (s(W + 40), s(top + 6)),
               (s(W + 40), s(H)), (s(-40), s(H))], fill=wood)
    # curved front lip catching the light
    d.pieslice([s(-60), s(top - 10), s(W + 60), s(top + 30)], 180, 360,
               fill=woodhi)
    d.rectangle([s(0), s(top + 4), s(W), s(H)], fill=wood)
    for x in range(0, W, 26):  # plank seams
        d.line([(s(x), s(top + 4)), (s(x), s(H))], fill=(96, 64, 36), width=s(2))
    d.line([(s(0), s(top + 4)), (s(W), s(top + 4))], fill=woodhi, width=s(3))
    return down(img, W, H)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "artpack_sample_src"
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

    # All five tiers per captain so the level-up evolution shows at every level
    # (the firmware's resolveClip steps down, so gaps still resolve gracefully).
    for i, cap in enumerate(CAPTAINS):
        for t in range(5):
            for phase in range(3):
                put(draw_captain_frame(cap, t, "idle", phase),
                    f"cap{i}_t{t}_idle_{phase}.png")
                put(draw_captain_frame(cap, t, "wave", phase),
                    f"cap{i}_t{t}_wave_{phase}.png")

    print(f"Wrote {n} source image(s) to {src}/")
    print("Next:")
    print(f"  python tools/build_artpack.py {src} artpack_sample")
    print("  copy artpack_sample/* to the SD card's /art/ folder, then reboot")


if __name__ == "__main__":
    main()
