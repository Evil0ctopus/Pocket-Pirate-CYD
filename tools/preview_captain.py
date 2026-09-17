#!/usr/bin/env python3
"""Render captain tier-progression sheets from a packed art pack.

Single captain (default): one captain at tiers 0..4 side by side (idle pose) so
you can see the level-up evolution -- deckhand -> coat+belt -> tricorn ->
telescope+epaulets -> parrot+medal.

--roster: stitch all four captains into one 4x5 montage (a row per captain,
labeled with the crew names) so you can compare the whole roster at a glance.

Usage:
    python tools/preview_captain.py [art_dir] [--avatar N] [--out FILE]
    python tools/preview_captain.py [art_dir] --roster [--out FILE]

Default: art_dir=artpack_sample. Single out=<art_dir>/captain<N>_tiers.png;
roster out=<art_dir>/roster_tiers.png.

Requires Pillow:  pip install pillow
"""

import argparse
import glob
import os
import re
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is required.  Install it with:  pip install pillow")

CELL_W, CELL_H, LABEL_H, NAME_W = 150, 210, 22, 96
TIER_GAINS = ["deckhand", "+coat/belt", "+tricorn", "+telescope", "+parrot/medal"]
# Crew names mirror include/captains.h (index order).
NAMES = ["Mara Tide", "Roan Redsail", "Iyla Dawn", "Bram Kettle"]
BG = (18, 24, 36)
GOLD = (240, 224, 150)
CAP_RE = re.compile(r"cap(\d+)_t(\d+)_idle_f(\d+)_w(\d+)_h(\d+)\.png$", re.I)


def load(p):
    return Image.open(p).convert("RGBA") if os.path.exists(p) else None


def backdrop(sky):
    cell = Image.new("RGB", (CELL_W, CELL_H))
    if sky is not None:
        cell.paste(sky.convert("RGB").resize((CELL_W, CELL_H)), (0, 0))
    else:
        cell.paste((120, 196, 236), [0, 0, CELL_W, CELL_H])
    return cell


def first_idle(art_dir, avatar, tier):
    for p in glob.glob(os.path.join(art_dir, f"cap{avatar}_t{tier}_idle_*.png")):
        m = CAP_RE.search(os.path.basename(p))
        if m:
            fw, fh = int(m.group(4)), int(m.group(5))
            return load(p).crop((0, 0, fw, fh))
    return None


def render_row(art_dir, sky, avatar):
    """A 5-cell (tiers 0..4) strip for one captain, CELL_W*5 x CELL_H. Reports
    how many tiers were present."""
    row = Image.new("RGB", (CELL_W * 5, CELL_H), BG)
    found = 0
    for t in range(5):
        cell = backdrop(sky)
        cap = first_idle(art_dir, avatar, t)
        if cap is not None:
            found += 1
            cell.paste(cap, ((CELL_W - cap.width) // 2, CELL_H - cap.height), cap)
        else:
            ImageDraw.Draw(cell).text((CELL_W // 2 - 18, CELL_H // 2), "(none)",
                                      fill=(230, 230, 230))
        row.paste(cell, (t * CELL_W, 0))
        ImageDraw.Draw(row).line([(t * CELL_W, 0), (t * CELL_W, CELL_H)],
                                 fill=BG, width=2)
    return row, found


def tier_labels(width, x0):
    band = Image.new("RGB", (width, LABEL_H), BG)
    d = ImageDraw.Draw(band)
    for t in range(5):
        d.text((x0 + t * CELL_W + 6, 6), f"t{t} {TIER_GAINS[t]}", fill=GOLD)
    return band


def render_single(art_dir, sky, avatar, out):
    row, found = render_row(art_dir, sky, avatar)
    sheet = Image.new("RGB", (CELL_W * 5, CELL_H + LABEL_H), BG)
    sheet.paste(row, (0, 0))
    sheet.paste(tier_labels(CELL_W * 5, 0), (0, CELL_H))
    sheet.save(out)
    print(f"Wrote {out} ({found}/5 tiers found for avatar {avatar})")


def render_roster(art_dir, sky, out):
    n = len(NAMES)
    sheet = Image.new("RGB", (NAME_W + CELL_W * 5, CELL_H * n + LABEL_H), BG)
    d = ImageDraw.Draw(sheet)
    total = 0
    for a in range(n):
        row, found = render_row(art_dir, sky, a)
        total += found
        y = a * CELL_H
        sheet.paste(row, (NAME_W, y))
        # name in the left margin, wrapped onto two lines, vertically centered
        parts = NAMES[a].split(" ", 1)
        ty = y + CELL_H // 2 - 6 * len(parts)
        for line in parts:
            d.text((8, ty), line, fill=GOLD)
            ty += 12
        d.line([(0, y), (NAME_W + CELL_W * 5, y)], fill=BG, width=2)
    sheet.paste(tier_labels(NAME_W + CELL_W * 5, NAME_W), (0, CELL_H * n))
    sheet.save(out)
    print(f"Wrote {out} ({total}/{n * 5} captain-tiers found)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("art_dir", nargs="?", default="artpack_sample")
    ap.add_argument("--avatar", type=int, default=0)
    ap.add_argument("--roster", action="store_true",
                    help="stitch all four captains into one 4x5 montage")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    if not os.path.isdir(a.art_dir):
        sys.exit(f"art dir not found: {a.art_dir}")

    sky = load(os.path.join(a.art_dir, "sky.png"))
    if a.roster:
        out = a.out or os.path.join(a.art_dir, "roster_tiers.png")
        render_roster(a.art_dir, sky, out)
    else:
        out = a.out or os.path.join(a.art_dir, f"captain{a.avatar}_tiers.png")
        render_single(a.art_dir, sky, a.avatar, out)


if __name__ == "__main__":
    main()
