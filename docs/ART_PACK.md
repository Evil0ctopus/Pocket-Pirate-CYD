# Pocket Pirate — Art Pack Guide

This firmware reaches "modern illustrated" quality by **displaying real image
files**, not by drawing shapes on the device. You generate the art (with an AI
image generator or an artist), run it through `tools/build_artpack.py`, and copy
the result to the microSD card's `/art/` folder. The firmware decodes and
animates it on-device.

If no art pack is present, the firmware falls back to the built-in vector
renderer — so the device still works with a blank SD card; the art pack is what
makes it look great.

> **Try the pipeline first (no art skills needed).** A generator ships a
> known-good *sample* pack so you can confirm decoding, animation, parallax and
> tap-to-wave all work on your board before you commission real art:
>
> ```
> python tools/make_sample_pack.py                              # writes artpack_sample_src/
> python tools/build_artpack.py artpack_sample_src artpack_sample
> # copy artpack_sample/* to the SD card's /art/ folder and reboot
> ```
>
> Preview any packed folder on your PC before touching the SD card — it
> composites the scene at the *exact* coordinates and speeds the firmware uses,
> so it also cross-checks your layer geometry:
>
> ```
> python tools/preview_world.py artpack_sample --avatar 0 --tier 2
> # writes artpack_sample/preview.gif (+ .png still)
> ```
>
> The sample art is deliberately simple placeholder rendering — it exists to
> validate the mechanism, not to be the final look. Replace it tier by tier with
> your generated illustration.

---

## 1. The look we're going for

Chunky **chibi "collectible-toy" pirates**, matching your reference images:

- Big head, big glossy eyes, soft 3D shading (like a vinyl figure / Pixar-ish).
- Tricorn hats with skull emblems, red bandanas, beards, gold-buckle belts.
- Rich, warm, painterly rendering — **not** flat vector, **not** pixel art.
- A believable 2.5D ocean world around them (sky, sea, parallax clouds, foam).

Keep every captain **on a transparent background, centered, full-body, same
camera framing** so the pipeline can stack animation frames cleanly.

---

## 2. The scene model (what the device composites)

The world is drawn back-to-front as layers, several of which animate:

| Layer        | File                    | Size (px)   | Notes                                   |
|--------------|-------------------------|-------------|-----------------------------------------|
| Sky+sea base | `sky.png`               | 320×240     | Opaque full backdrop (sky, horizon, sea)|
| Clouds       | `clouds.png`            | 480×100     | Transparent; scrolls slowly, wraps      |
| Waves/foam   | `waves.png`             | 480×70      | Transparent; scrolls faster (shimmer)   |
| Ship         | `ship_t<T>.png`         | 150×130     | Transparent; gently bobs                |
| Captain      | `cap<N>_t<T>_<clip>...`  | frames      | Transparent sprite-sheet, animated      |
| Deck (fore)  | `deck.png`              | 320×90      | Transparent foreground, static, optional|

- `<N>` = captain index **0–3** (see roster below).
- `<T>` = visual tier **0–4** (grows with level; you can start with just one
  tier, e.g. `t2`, and the firmware reuses it for the others).

### Captain roster (`<N>`)
| N | Name          | Trait         | Signature look                                  |
|---|---------------|---------------|-------------------------------------------------|
| 0 | Mara Tide     | Navigator     | teal coat, navy tricorn, no beard, braid        |
| 1 | Roan Redsail  | Gunner        | red coat, ginger beard, red bandana             |
| 2 | Iyla Dawn     | Lookout       | blue coat, blonde, purple bandana               |
| 3 | Bram Kettle   | Quartermaster | green coat, grey beard, eyepatch                |

### Tier progression (`<T>`) — same character, more rank/gear
- **t0** raft rookie: bandana or plain hat, simple shirt.
- **t1** deckhand: tricorn, earring.
- **t2** mate: tricorn + skull emblem, epaulettes, cutlass on belt.
- **t3** captain: plumed hat, telescope, ornate coat.
- **t4** legend: gold medal, shoulder parrot, the works.

---

## 3. Character animation (sprite sheets)

You chose **full character animation**. Each captain clip is a set of frames of
the *same character in the same framing*, just changing pose. Generate each
frame as its own transparent PNG; the pipeline crops, centers, and packs them
into one horizontal strip the firmware plays.

Clips the firmware understands:

- **`idle`** (required) — the looping resting animation. 4–8 frames:
  breathing bob, occasional blink, coat/beard drift. Even 2 frames
  (eyes-open / eyes-blink) already reads as alive.
- **`wave`** (optional) — a one-shot played when you tap the captain. 4–6
  frames of a friendly wave / hat tip / cutlass salute.

Input frame files (before packing):
```
cap0_t2_idle_0.png   cap0_t2_idle_1.png   cap0_t2_idle_2.png ...
cap0_t2_wave_0.png   cap0_t2_wave_1.png ...
```
Numbered in play order. The pipeline outputs the packed strips
> (`cap0_t2_idle_f6_w180_h210.png`, etc.) that go on the card.

> Tip for consistent frames from an image generator: lock the character
> description, add `same character, identical outfit, centered, full body,
> transparent background, front view, fixed camera` to every frame prompt, and
> only change the pose clause ("eyes closed", "waving right hand", …). The
> pipeline auto-centers by content, which hides small drift.

---

## 4. Prompt pack

Paste the **style preamble** in front of every prompt, then the subject line.
Tuned for Midjourney / DALL·E / SDXL. Adjust to your generator's syntax.

### Style preamble (use for ALL images)
```
chibi collectible vinyl-toy pirate, big head, large glossy expressive eyes,
soft three-dimensional shading, smooth matte finish, warm cinematic lighting,
clean rounded forms, highly detailed, playful adventurous mood, subtle rim
light, painterly render, high quality, centered, full body, front view,
transparent background, fixed camera framing
--no text, watermark, signature, flat vector, pixel art, harsh outlines,
photo, realistic human skin pores, extra limbs, cluttered background
```

### Captains (base pose, tier 2 — repeat per captain/tier)
```
[STYLE PREAMBLE] Captain "Mara Tide", a confident young woman navigator, warm
teal naval coat with gold trim and epaulettes, navy-blue tricorn hat with a
small white skull emblem, dark braid over one shoulder, gold hoop earring,
brass telescope on her belt, no beard, friendly determined smile.

[STYLE PREAMBLE] Captain "Roan Redsail", a burly gunner, deep-red naval coat
with gold buttons, thick ginger beard, red bandana under a black tricorn with a
skull emblem, gold-buckle leather belt with a cutlass, one gold earring,
hearty grin.

[STYLE PREAMBLE] Captain "Iyla Dawn", a nimble lookout, blue coat with silver
trim, blonde hair, purple bandana, spyglass in hand, bright curious eyes,
light and agile posture.

[STYLE PREAMBLE] Captain "Bram Kettle", a grizzled quartermaster, forest-green
coat, grey beard, brown tricorn, black eyepatch over the left eye, gold-buckle
belt, weathered kindly face.
```

### Animation-frame variants (append ONE pose clause to a captain prompt)
```
idle_0: ...neutral resting pose, eyes open, arms relaxed
idle_1: ...same pose, eyes gently closed (mid-blink)
idle_2: ...same pose, slight breathing lean, coat drifting in the breeze
wave_0..n: ...raising right hand in a friendly wave / tipping the tricorn hat
```

### Ship (per tier)
```
[STYLE PREAMBLE] a small chibi pirate sailing ship, warm wooden hull, cream
sails, tiny red pennant, cartoonish and cute, side view, transparent
background   (t0 = simple raft/dinghy → t4 = grand galleon with many sails)
```

### World layers
```
sky.png:    [no chibi] painterly cartoon ocean backdrop, soft gradient sky,
            warm sunrise glow, distant calm sea and horizon, 320x240, opaque,
            no characters, no foreground objects
clouds.png: a horizontal band of soft fluffy stylized clouds, transparent
            background, seamless left-right tiling, 480x100
waves.png:  a horizontal band of stylized sea-foam wave crests, transparent
            background, seamless left-right tiling, 480x70
deck.png:   the wooden foreground railing/deck of a pirate ship seen from the
            captain's view, transparent above, 320x90 (optional)
```

---

## 5. Build & install

Requires Python 3 with Pillow (`pip install pillow`).

```
# 1. Put your generated images in a source folder using the input names above:
#    artpack_src/sky.png, artpack_src/cap0_t2_idle_0.png, ...
#
# 2. Pack them into the firmware layout:
python tools/build_artpack.py artpack_src artpack_out
#
# 3. Copy the CONTENTS of artpack_out/ to the SD card's /art/ folder:
#    /art/sky.png, /art/cap0_t2_idle_f6_w180_h210.png, ...
#
# 4. Insert the card and reboot the device.
```

The script resizes/crops everything to spec, auto-centers animation frames,
packs them into strips, and prints a summary of what it produced.

---

## 6. How the firmware consumes it

On boot, `art::begin()` lists `/art/`, records which layers/captains/clips
exist, and parses each sprite strip's `_f<frames>_w<framewidth>` suffix. The
world loop then blits the parallax layers (scrolling clouds/waves), plays the
captain's `idle` clip, bobs the ship, and switches to the `wave` clip on tap.
Anything missing falls back to the vector renderer, so a partial art pack is
fine — add one captain at a time.

---

## Shippable packs in this repo

You do **not** need commissioned art to ship a good look. Two packs are ready:

| Folder | Role |
|--------|------|
| `artpack_pixel/` | Primary shippable pixel pack (idle/wave/fight/jig/look, ships, sky/waves/deck) |
| `artpack_sample/` | Minimal pipeline-validation pack (idle + wave only) |

Quick gallery on your PC:

```bash
python tools/preview_world.py artpack_pixel --avatar 0 --tier 2
# refreshes artpack_pixel/preview.gif + preview.png
```

On device: copy `artpack_pixel/*` → microSD `/art/`, reboot. Firmware falls back
to vector chibi art if `/art/` is missing.
