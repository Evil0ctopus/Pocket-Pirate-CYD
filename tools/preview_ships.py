#!/usr/bin/env python3
"""Render a contact sheet of the five ship tiers from a packed art pack.

Shows ship_t0..ship_t4 each on the sky/sea backdrop so you can judge the
raft -> galleon evolution at a glance.

Usage:
    python tools/preview_ships.py [art_dir] [--out FILE]

Default: art_dir=artpack_sample, out=<art_dir>/ships_check.png

Requires Pillow:  pip install pillow
"""

import argparse
import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is required.  Install it with:  pip install pillow")

TIER_NAMES = ["raft", "sloop", "brig", "frigate", "galleon"]
CELL_W, CELL_H, LABEL_H = 170, 150, 20


def load(path):
    return Image.open(path).convert("RGBA") if os.path.exists(path) else None


def backdrop(sky):
    """A CELL_W x CELL_H sky/sea slice, from sky.png if present else a gradient."""
    cell = Image.new("RGB", (CELL_W, CELL_H))
    if sky is not None:
        # sample the middle of the real sky so the horizon shows in the cell
        crop = sky.convert("RGB").resize((CELL_W, 240))
        cell.paste(crop.crop((0, 60, CELL_W, 60 + CELL_H)), (0, 0))
    else:
        px = cell.load()
        for y in range(CELL_H):
            t = y / (CELL_H - 1)
            if y < CELL_H * 0.6:
                px_row = (120, 196, 236)
            else:
                px_row = (30, 120, 155)
            for x in range(CELL_W):
                px[x, y] = px_row
    return cell


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("art_dir", nargs="?", default="artpack_sample")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    if not os.path.isdir(a.art_dir):
        sys.exit(f"art dir not found: {a.art_dir}")

    sky = load(os.path.join(a.art_dir, "sky.png"))
    sheet = Image.new("RGB", (CELL_W * 5, CELL_H + LABEL_H), (18, 24, 36))
    d = ImageDraw.Draw(sheet)
    found = 0
    for t in range(5):
        cell = backdrop(sky)
        ship = load(os.path.join(a.art_dir, f"ship_t{t}.png"))
        if ship is not None:
            found += 1
            cell.paste(ship, ((CELL_W - ship.width) // 2,
                              (CELL_H - ship.height) // 2 + 6), ship)
        else:
            ImageDraw.Draw(cell).text((CELL_W // 2 - 18, CELL_H // 2), "(none)",
                                      fill=(200, 200, 200))
        sheet.paste(cell, (t * CELL_W, 0))
        d.text((t * CELL_W + 8, CELL_H + 5),
               f"t{t}  {TIER_NAMES[t]}", fill=(240, 224, 150))
        d.line([(t * CELL_W, 0), (t * CELL_W, CELL_H)], fill=(18, 24, 36), width=2)

    out = a.out or os.path.join(a.art_dir, "ships_check.png")
    sheet.save(out)
    print(f"Wrote {out} ({found}/5 ship tiers found)")


if __name__ == "__main__":
    main()
