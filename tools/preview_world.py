#!/usr/bin/env python3
"""Render a PC-side preview of the on-device world from a packed art pack.

This composites a packed /art/ folder (see tools/build_artpack.py) into an
animated GIF the *exact* way the firmware's drawWorld() does -- same layer
order, same draw coordinates, same parallax speeds, ship bob, idle loop and
tap-to-wave timing. So it doubles as a geometry cross-check: if the captain,
ship or deck sit wrong here, they sit wrong on the device.

Usage:
    python tools/preview_world.py [art_dir] [--avatar N] [--tier T] [--out FILE]

Defaults: art_dir=artpack_sample, avatar=0, tier=2, out=<art_dir>/preview.gif

Requires Pillow:  pip install pillow
"""

import argparse
import glob
import os
import re
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required.  Install it with:  pip install pillow")

W, H = 320, 240

# Draw coordinates / speeds mirrored from src/main.cpp drawWorld() and
# src/art.cpp (kCloudsY/kWavesY, layer widths).
CLOUDS_Y, CLOUDS_W = 18, 480
WAVES_Y, WAVES_W = 150, 480
SHIP_X, SHIP_Y = 196, 84
DECK_Y = 240 - 90
CAP_FEET_Y = 208  # captain drawn at (160 - fw/2, 208 - fh)

CAP_RE = re.compile(
    r"cap(\d+)_t(\d+)_(idle|wave)_f(\d+)_w(\d+)_h(\d+)\.png$", re.IGNORECASE)


def load(path):
    return Image.open(path).convert("RGBA") if os.path.exists(path) else None


def find_clip(art_dir, avatar, tier, clip):
    """Mirror art.cpp resolveClip: step down tiers, fall back idle<->wave."""
    cand = {}
    for p in glob.glob(os.path.join(art_dir, "cap*.png")):
        m = CAP_RE.search(os.path.basename(p))
        if m and int(m.group(1)) == avatar:
            cand[(int(m.group(2)), m.group(3).lower())] = (
                p, int(m.group(4)), int(m.group(5)), int(m.group(6)))
    for t in range(tier, -1, -1):
        if (t, clip) in cand:
            return cand[(t, clip)]
        other = "wave" if clip == "idle" else "idle"
        if (t, other) in cand:
            return cand[(t, other)]
    return None


def find_ship(art_dir, tier):
    for t in range(tier, -1, -1):
        p = os.path.join(art_dir, f"ship_t{t}.png")
        if os.path.exists(p):
            return load(p)
    return None


def paste(base, layer, x, y):
    if layer is not None:
        base.paste(layer, (int(x), int(y)), layer)


def scroll(base, layer, y, imgw, scrollx):
    if layer is None:
        return
    ox = -(scrollx % imgw)
    paste(base, layer, ox, y)
    paste(base, layer, ox + imgw, y)


def frame_at(now, art, avatar, tier, wave_start):
    """Build one 320x240 RGB frame for simulated millis `now`."""
    base = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    paste(base, art["sky"], 0, 0)
    scroll(base, art["clouds"], CLOUDS_Y, CLOUDS_W, now // 70)
    scroll(base, art["waves"], WAVES_Y, WAVES_W, now // 28)

    bob = (now // 400) % 2 * 3
    paste(base, art["ship"], SHIP_X, SHIP_Y + bob)

    # captain: idle loop, or the wave clip while it is playing
    idle = art["idle"]
    clip, fw, fh, frames, strip = "idle", 0, 0, 0, None
    if idle:
        _, frames, fw, fh = idle
        strip = load(idle[0])
    if wave_start is not None and art["wave"]:
        _, wframes, wfw, wfh = art["wave"]
        wf = (now - wave_start) // 90
        if wf < wframes:
            clip, fw, fh, frames = "wave", wfw, wfh, wframes
            strip = load(art["wave"][0])
            fidx = wf
    if clip == "idle" and strip:
        fidx = (now // 140) % max(1, frames)
    if strip:
        sub = strip.crop((fidx * fw, 0, (fidx + 1) * fw, fh))
        paste(base, sub, 160 - fw // 2, CAP_FEET_Y - fh)

    paste(base, art["deck"], 0, DECK_Y)
    return base.convert("RGB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("art_dir", nargs="?", default="artpack_sample")
    ap.add_argument("--avatar", type=int, default=0)
    ap.add_argument("--tier", type=int, default=2)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    if not os.path.isdir(a.art_dir):
        sys.exit(f"art dir not found: {a.art_dir} (run make_sample_pack + build_artpack first)")

    art = {
        "sky": load(os.path.join(a.art_dir, "sky.png")),
        "clouds": load(os.path.join(a.art_dir, "clouds.png")),
        "waves": load(os.path.join(a.art_dir, "waves.png")),
        "deck": load(os.path.join(a.art_dir, "deck.png")),
        "ship": find_ship(a.art_dir, a.tier),
        "idle": find_clip(a.art_dir, a.avatar, a.tier, "idle"),
        "wave": find_clip(a.art_dir, a.avatar, a.tier, "wave"),
    }
    if not any(art[k] for k in ("sky", "ship", "idle")):
        sys.exit("nothing to preview -- no sky/ship/captain art found in that folder")

    # ~4 s timeline at 40 ms/frame: idle, then a tap-to-wave around t=1200 ms.
    dt, total = 40, 4000
    wave_start = 1200
    frames = []
    for now in range(0, total, dt):
        ws = wave_start if now >= wave_start else None
        frames.append(frame_at(now, art, a.avatar, a.tier, ws))

    out = a.out or os.path.join(a.art_dir, "preview.gif")
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=dt,
                   loop=0, disposal=2, optimize=True)
    # also a still of the wave apex for a quick glance
    still = os.path.splitext(out)[0] + ".png"
    frame_at(wave_start + 180, art, a.avatar, a.tier, wave_start).save(still)
    print(f"Wrote {out} ({len(frames)} frames) and {still}")
    print("Open the GIF to see parallax, ship bob, idle loop and the wave gesture,")
    print("composited at the exact coordinates the firmware uses.")


if __name__ == "__main__":
    main()
