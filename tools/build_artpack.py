#!/usr/bin/env python3
"""Pack generated images into the Pocket Pirate on-device art layout.

Usage:
    python tools/build_artpack.py <src_dir> <out_dir>

Reads loose images from <src_dir> (see docs/ART_PACK.md for the naming), resizes
world layers to spec, auto-centers and packs captain animation frames into
horizontal sprite strips, and writes everything to <out_dir>. Copy the contents
of <out_dir> to the SD card's /art/ folder.

Requires Pillow:  pip install pillow
"""

import os
import re
import sys
from collections import defaultdict

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required.  Install it with:  pip install pillow")

# --- target sizes (must match the firmware's expectations) -------------------
WORLD_SPECS = {
    "sky":    (320, 240, False),   # (w, h, keep_alpha)
    "clouds": (480, 100, True),
    "waves":  (480, 70, True),
    "deck":   (320, 90, True),
}
SHIP_SIZE = (150, 130)             # each ship_t<T>.png
CAP_CANVAS = (180, 210)            # per-frame canvas for captain sprites (w, h)

FRAME_RE = re.compile(r"^(cap\d+_t\d+_[a-z]+)_(\d+)$", re.IGNORECASE)


def load(path):
    return Image.open(path).convert("RGBA")


def fit_contain(img, size):
    """Resize to fit inside `size`, preserving aspect, centered on a canvas."""
    tw, th = size
    img = img.copy()
    img.thumbnail((tw, th), Image.LANCZOS)
    canvas = Image.new("RGBA", size, (0, 0, 0, 0))
    canvas.paste(img, ((tw - img.width) // 2, (th - img.height) // 2), img)
    return canvas


def content_bbox_center(img, size):
    """Place a frame on the fixed canvas, bottom-anchored and centered.
    A source that already fits is pasted WHOLE: the authored canvas itself is
    the registration, so clips may move limbs/props anywhere inside it without
    drifting between frames (the pixel pack relies on this). Oversized sources
    (arbitrary AI/artist exports) fall back to bbox-crop + scale."""
    tw, th = size
    if img.width > tw or img.height > th:
        bbox = img.getbbox()
        if bbox:
            img = img.crop(bbox)
        if img.width > tw or img.height > th:
            img.thumbnail((tw, th), Image.LANCZOS)
    canvas = Image.new("RGBA", size, (0, 0, 0, 0))
    # center horizontally, sit on the bottom (feet anchored) vertically
    x = (tw - img.width) // 2
    y = th - img.height
    canvas.paste(img, (x, y), img)
    return canvas


def save(img, out_dir, name):
    path = os.path.join(out_dir, name)
    img.save(path, "PNG")
    return path


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    if not os.path.isdir(src):
        sys.exit(f"source dir not found: {src}")
    os.makedirs(out, exist_ok=True)

    files = [f for f in os.listdir(src) if f.lower().endswith(".png")]
    stems = {os.path.splitext(f)[0].lower(): f for f in files}
    produced = []

    # --- world layers --------------------------------------------------------
    for key, (w, h, keep_alpha) in WORLD_SPECS.items():
        f = stems.get(key)
        if not f:
            continue
        img = fit_contain(load(os.path.join(src, f)), (w, h))
        if not keep_alpha:
            bg = Image.new("RGBA", (w, h), (0, 0, 0, 255))
            bg.alpha_composite(img)
            img = bg
        produced.append(save(img, out, f"{key}.png"))

    # --- ships ---------------------------------------------------------------
    for t in range(5):
        f = stems.get(f"ship_t{t}") or stems.get("ship")
        if not f:
            continue
        img = fit_contain(load(os.path.join(src, f)), SHIP_SIZE)
        produced.append(save(img, out, f"ship_t{t}.png"))

    # --- captain single stills (no _idle/_wave suffix) -----------------------
    for n in range(4):
        for t in range(5):
            f = stems.get(f"cap{n}_t{t}")
            if not f:
                continue
            img = content_bbox_center(load(os.path.join(src, f)), CAP_CANVAS)
            fw, fh = CAP_CANVAS
            produced.append(
                save(img, out, f"cap{n}_t{t}_idle_f1_w{fw}_h{fh}.png"))

    # --- captain animation clips (grouped numbered frames) -------------------
    clips = defaultdict(list)  # clip_stem -> list of (index, filename)
    for stem, fname in stems.items():
        m = FRAME_RE.match(stem)
        if m:
            clips[m.group(1).lower()].append((int(m.group(2)), fname))

    for clip, frames in sorted(clips.items()):
        frames.sort(key=lambda x: x[0])
        fw, fh = CAP_CANVAS
        strip = Image.new("RGBA", (fw * len(frames), fh), (0, 0, 0, 0))
        for i, (_, fname) in enumerate(frames):
            frame = content_bbox_center(load(os.path.join(src, fname)), CAP_CANVAS)
            strip.paste(frame, (i * fw, 0), frame)
        name = f"{clip}_f{len(frames)}_w{fw}_h{fh}.png"
        produced.append(save(strip, out, name))

    # --- summary -------------------------------------------------------------
    print(f"\nWrote {len(produced)} file(s) to {out}/:")
    for p in produced:
        print("  ", os.path.basename(p))
    if not produced:
        print("  (nothing matched — check your source filenames against "
              "docs/ART_PACK.md)")
    print("\nCopy the contents of that folder to the SD card's /art/ directory.")


if __name__ == "__main__":
    main()
