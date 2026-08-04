#!/usr/bin/env python3
"""generate_ttt_assets.py — paints the Lua Tic-Tac-Toe demo's texture atlas,
synthesizes its two WAV sound effects, and packs everything (plus the shared
Roboto label font) into the TSAB asset bundle the embedded Lua game reads.

Unlike the Dart-driven examples, the ttt demo's game logic lives entirely in
assets/ttt/game.lua: the script pulls each asset out of the bundle with
tessera.asset(name) and registers it itself, so the only committed artifacts
are the bundle (ttt.tsb) and the script. Re-run to regenerate; the painted
content and WAVs are deterministic (fixed seeds), but the atlas PNG's
compressed bytes — and therefore ttt.tsb — can differ across Pillow/zlib
builds, so only re-commit ttt.tsb when the assets intentionally change. See
tools/pack_bundle.py for the bundle format.

    python3 tool/generate_ttt_assets.py

Requires Pillow. Writes example/assets/ttt/ttt.tsb.
"""
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
REPO = os.path.join(ROOT, "..", "..", "..")
OUT = os.path.join(ROOT, "assets", "ttt")
FONT = os.path.join(ROOT, "assets", "fonts", "Roboto-Regular.ttf")
PACK = os.path.join(REPO, "tools", "pack_bundle.py")

# The atlas is four 256px quadrants; game.lua addresses them by UV rect:
#   top-left  X card face      top-right  O card face
#   bot-left  card back        bot-right  board tile top
CELL = 256


# ---------------------------------------------------------------------------
# small painting helpers
# ---------------------------------------------------------------------------
def lerp_rgb(c0, c1, t):
    return tuple(int(round(c0[i] + (c1[i] - c0[i]) * t)) for i in range(3))


def radial_shade(size, center_c, edge_c):
    """A square tile shaded from `center_c` out to `edge_c` at the corners."""
    img = Image.new("RGB", (size, size))
    px = img.load()
    half = (size - 1) / 2
    for y in range(size):
        for x in range(size):
            t = math.hypot(x - half, y - half) / (half * math.sqrt(2))
            px[x, y] = lerp_rgb(center_c, edge_c, min(1.0, t))
    return img


def speckle(img, rng, n, color, rmax=1.5):
    d = ImageDraw.Draw(img)
    for _ in range(n):
        x = rng.uniform(0, img.width)
        y = rng.uniform(0, img.height)
        r = rng.uniform(0.4, rmax)
        d.ellipse([x - r, y - r, x + r, y + r], fill=color)


# ---------------------------------------------------------------------------
# atlas quadrants. Every design is symmetric under horizontal/vertical flips,
# so the art reads correctly whatever way a card face or tile top is mapped.
# ---------------------------------------------------------------------------
def paint_face(glyph, seed):
    """A cream card face with a double border; `glyph` draws the mark."""
    img = radial_shade(CELL, (246, 240, 224), (216, 206, 182))
    rng = random.Random(seed)
    speckle(img, rng, 60, (206, 196, 172))
    d = ImageDraw.Draw(img)
    for inset, w in ((14, 5), (26, 2)):
        d.rounded_rectangle([inset, inset, CELL - 1 - inset, CELL - 1 - inset],
                            radius=24, outline=(172, 148, 108), width=w)
    glyph(img)
    return img


def glyph_x(img):
    """A bold X: two diagonal bars in ink blue with a light core stroke."""
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    c, r, w = CELL / 2, CELL * 0.26, int(CELL * 0.13)
    for sx in (1, -1):
        d.line([c - r * sx, c - r, c + r * sx, c + r],
               fill=(42, 74, 150, 255), width=w)
        d.line([c - r * sx, c - r, c + r * sx, c + r],
               fill=(120, 154, 224, 255), width=max(2, w // 3))
    halo = layer.filter(ImageFilter.GaussianBlur(6))
    img.paste(halo, (0, 0), halo)
    img.paste(layer, (0, 0), layer)


def glyph_o(img):
    """A bold O ring in ember red with a light inner rim."""
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    c, r, w = CELL / 2, CELL * 0.27, int(CELL * 0.12)
    d.ellipse([c - r, c - r, c + r, c + r],
              outline=(188, 62, 44, 255), width=w)
    d.ellipse([c - r + w * 0.3, c - r + w * 0.3, c + r - w * 0.3, c + r - w * 0.3],
              outline=(238, 138, 96, 255), width=max(2, w // 4))
    halo = layer.filter(ImageFilter.GaussianBlur(6))
    img.paste(halo, (0, 0), halo)
    img.paste(layer, (0, 0), layer)


def paint_back(seed):
    """The concealing back: a woven lattice + diamond mandala in muted gold."""
    base, line = (36, 30, 52), (172, 146, 92)
    img = radial_shade(CELL, lerp_rgb(base, (255, 255, 255), 0.08), base)
    d = ImageDraw.Draw(img)
    lat = lerp_rgb(base, line, 0.25)
    for k in range(-CELL, CELL * 2, 24):
        d.line([k, 0, k + CELL, CELL], fill=lat, width=2)
        d.line([k + CELL, 0, k, CELL], fill=lat, width=2)
    c = CELL / 2

    def diamond(r, **kw):
        d.polygon([(c, c - r), (c + r, c), (c, c + r), (c - r, c)], **kw)

    diamond(92, outline=line, width=4)
    diamond(68, outline=line, width=2)
    d.ellipse([c - 46, c - 46, c + 46, c + 46], outline=line, width=3)
    diamond(26, fill=lerp_rgb(base, line, 0.6))
    diamond(12, fill=lerp_rgb(base, (255, 255, 255), 0.7))
    d.rounded_rectangle([10, 10, CELL - 11, CELL - 11], radius=20,
                        outline=line, width=4)
    speckle(img, random.Random(seed), 50, lerp_rgb(line, (255, 255, 255), 0.3), 1.1)
    return img


def paint_tile(seed):
    """The board tile top: bright near-neutral grain (game.lua tints the two
    checkerboard defs light/dark from this one texture)."""
    rng = random.Random(seed)
    img = radial_shade(CELL, (252, 250, 244), (222, 216, 202))
    px = img.load()
    # horizontal wood-ish grain: per-row brightness wobble
    for y in range(CELL):
        wob = (math.sin(y * 0.16 + rng.uniform(-0.4, 0.4)) * 5
               + rng.uniform(-3, 3))
        for x in range(CELL):
            r, g, b = px[x, y]
            px[x, y] = (max(0, min(255, r + int(wob))),
                        max(0, min(255, g + int(wob))),
                        max(0, min(255, b + int(wob * 0.8))))
    speckle(img, rng, 90, (208, 200, 184))
    d = ImageDraw.Draw(img)
    d.rectangle([4, 4, CELL - 5, CELL - 5], outline=(198, 190, 172), width=3)
    return img


def paint_atlas():
    atlas = Image.new("RGB", (CELL * 2, CELL * 2))
    atlas.paste(paint_face(glyph_x, seed=1), (0, 0))
    atlas.paste(paint_face(glyph_o, seed=2), (CELL, 0))
    atlas.paste(paint_back(seed=3), (0, CELL))
    atlas.paste(paint_tile(seed=4), (CELL, CELL))
    return atlas


# ---------------------------------------------------------------------------
# sounds — tiny synthesized WAVs (22050 Hz mono PCM16), duel-generator style
# ---------------------------------------------------------------------------
RATE = 22050


def write_wav(path, samples):
    clipped = [max(-1.0, min(1.0, s)) for s in samples]
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(s * 32000)) for s in clipped))


def env(t, dur, attack=0.005, release=None):
    release = release if release is not None else dur * 0.6
    if t < attack:
        return t / attack
    if t > dur - release:
        return max(0.0, (dur - t) / release)
    return 1.0


def sine_sweep(dur, f0, f1, vol=0.8, attack=0.005, release=None):
    out = []
    phase = 0.0
    for i in range(int(dur * RATE)):
        t = i / RATE
        f = f0 + (f1 - f0) * (t / dur)
        phase += 2 * math.pi * f / RATE
        out.append(math.sin(phase) * vol * env(t, dur, attack, release))
    return out


def noise_swish(dur, vol=0.5, cutoff=0.35, seed=1):
    rng = random.Random(seed)
    out = []
    prev = 0.0
    for i in range(int(dur * RATE)):
        t = i / RATE
        prev = prev * cutoff + rng.uniform(-1, 1) * (1 - cutoff)
        out.append(prev * vol * math.sin(math.pi * min(1.0, t / dur)) ** 2)
    return out


def mix(*tracks):
    out = [0.0] * max(len(t) for t in tracks)
    for t in tracks:
        for i, s in enumerate(t):
            out[i] += s
    return out


def delayed(track, seconds):
    return [0.0] * int(seconds * RATE) + track


def chord_arp(notes, step, dur, vol=0.5):
    return mix(*[delayed(sine_sweep(dur, f, f, vol=vol, attack=0.01,
                                    release=dur * 0.7), i * step)
                 for i, f in enumerate(notes)])


# ---------------------------------------------------------------------------
def main():
    os.makedirs(OUT, exist_ok=True)
    with tempfile.TemporaryDirectory() as td:
        print("painting atlas…")
        paint_atlas().save(os.path.join(td, "ttt_atlas.png"))
        print("synthesizing sounds…")
        # place: felt thump + low knock
        write_wav(os.path.join(td, "place.wav"), mix(
            sine_sweep(0.16, 150, 62, vol=0.9, release=0.12),
            noise_swish(0.08, vol=0.22, cutoff=0.2, seed=9)))
        # win: ascending fanfare arpeggio
        write_wav(os.path.join(td, "win.wav"),
                  chord_arp([392, 494, 587, 784], 0.11, 0.5, vol=0.42))
        shutil.copy(FONT, os.path.join(td, "Roboto-Regular.ttf"))

        print("packing bundle…")
        subprocess.run(
            [sys.executable, PACK, os.path.join(OUT, "ttt.tsb"),
             os.path.join(td, "ttt_atlas.png"),
             os.path.join(td, "place.wav"),
             os.path.join(td, "win.wav"),
             os.path.join(td, "Roboto-Regular.ttf")],
            check=True)
    print("done.")


if __name__ == "__main__":
    main()
