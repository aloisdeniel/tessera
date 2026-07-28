#!/usr/bin/env python3
"""generate_duel_assets.py — procedurally paints the Duel example's bundled
Flutter assets (card back, front-face template, one illustration per creature)
and synthesizes its WAV sound effects.

Unlike the other example games (which draw their art at runtime with a Canvas),
Duel showcases *bundled Flutter assets referenced from SDL*: these PNGs/WAVs
ship in the asset bundle, are loaded with rootBundle, and are handed to the
engine (registerAtlas / registerSound). Re-run to regenerate; output is
deterministic (fixed seeds).

    python3 tool/generate_duel_assets.py

Requires Pillow. Writes into example/assets/duel/{,sfx/}.
"""
import math
import os
import random
import struct
import wave

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "assets", "duel")
SFX = os.path.join(OUT, "sfx")
os.makedirs(SFX, exist_ok=True)

CARD_W, CARD_H = 512, 768
ART_W, ART_H = 512, 512


# ---------------------------------------------------------------------------
# small painting helpers
# ---------------------------------------------------------------------------
def lerp(a, b, t):
    return a + (b - a) * t


def lerp_rgb(c0, c1, t):
    return tuple(int(round(lerp(c0[i], c1[i], t))) for i in range(3))


def vgradient(size, stops):
    """Vertical gradient image from [(t, rgb), ...] stops (t ascending)."""
    w, h = size
    img = Image.new("RGB", size)
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
            if t <= t1 or (t1 == stops[-1][0]):
                if t1 == t0:
                    c = c1
                else:
                    c = lerp_rgb(c0, c1, max(0.0, min(1.0, (t - t0) / (t1 - t0))))
                break
        row = c
        for x in range(w):
            px[x, y] = row
    return img


def radial_glow(img, center, radius, color, peak):
    """Additively blend a soft radial glow onto img (RGB)."""
    glow = Image.new("L", img.size, 0)
    d = ImageDraw.Draw(glow)
    steps = 48
    for i in range(steps, 0, -1):
        r = radius * i / steps
        a = int(peak * (1 - i / steps) ** 2)
        d.ellipse([center[0] - r, center[1] - r, center[0] + r, center[1] + r],
                  fill=a)
    glow = glow.filter(ImageFilter.GaussianBlur(radius / 6))
    tint = Image.new("RGB", img.size, color)
    img.paste(Image.composite(tint, img, glow), (0, 0))


def ridge_points(rng, w, y_base, rough, segments=8):
    """A displaced mountain-ridge polyline across the width."""
    xs = [w * i / segments for i in range(segments + 1)]
    ys = [y_base + rng.uniform(-rough, rough) for _ in xs]
    # midpoint-subdivide twice for jaggedness
    for _ in range(2):
        nxs, nys = [xs[0]], [ys[0]]
        for i in range(len(xs) - 1):
            mx = (xs[i] + xs[i + 1]) / 2
            my = (ys[i] + ys[i + 1]) / 2 + rng.uniform(-rough, rough) * 0.4
            nxs += [mx, xs[i + 1]]
            nys += [my, ys[i + 1]]
        xs, ys = nxs, nys
    return list(zip(xs, ys))


def paint_ridge(img, rng, y_base, rough, color, blur=0):
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    pts = ridge_points(rng, img.width, y_base, rough)
    poly = pts + [(img.width, img.height), (0, img.height)]
    d.polygon(poly, fill=color + (255,))
    if blur:
        layer = layer.filter(ImageFilter.GaussianBlur(blur))
    img.paste(layer, (0, 0), layer)


def stars(img, rng, n, ymax, base_color=(255, 255, 255)):
    d = ImageDraw.Draw(img)
    for _ in range(n):
        x = rng.uniform(0, img.width)
        y = rng.uniform(0, ymax)
        r = rng.uniform(0.5, 1.8)
        a = rng.uniform(0.35, 1.0)
        c = tuple(int(v * a) for v in base_color)
        d.ellipse([x - r, y - r, x + r, y + r], fill=c)


def mist(img, rng, y0, y1, color, passes=5):
    layer = Image.new("L", img.size, 0)
    d = ImageDraw.Draw(layer)
    for _ in range(passes):
        cy = rng.uniform(y0, y1)
        rx = rng.uniform(img.width * 0.35, img.width * 0.7)
        ry = rng.uniform(14, 40)
        cx = rng.uniform(0, img.width)
        d.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], fill=60)
    layer = layer.filter(ImageFilter.GaussianBlur(24))
    tint = Image.new("RGB", img.size, color)
    img.paste(Image.composite(tint, img, layer), (0, 0))


def vignette(img, strength=140):
    mask = Image.new("L", img.size, 0)
    d = ImageDraw.Draw(mask)
    w, h = img.size
    d.ellipse([-w * 0.25, -h * 0.25, w * 1.25, h * 1.25], fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(w / 5))
    dark = Image.new("RGB", img.size, (0, 0, 0))
    inv = mask.point(lambda v: int((255 - v) * strength / 255))
    img.paste(Image.composite(dark, img, inv), (0, 0))


def speckle(img, rng, n, color, ymin, ymax, rmax=1.6):
    d = ImageDraw.Draw(img)
    for _ in range(n):
        x = rng.uniform(0, img.width)
        y = rng.uniform(ymin, ymax)
        r = rng.uniform(0.4, rmax)
        d.ellipse([x - r, y - r, x + r, y + r], fill=color)


def rounded_mask(size, radius):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size[0] - 1, size[1] - 1],
                                        radius=radius, fill=255)
    return m


# ---------------------------------------------------------------------------
# emblems — bold glyphs drawn on a transparent layer, glow added by caller
# ---------------------------------------------------------------------------
def _flame_poly(cx, cy, s):
    """A classic teardrop flame with a wind-licked tip, in unit coords."""
    shape = [(0.08, -1.05), (-0.14, -0.66), (-0.4, -0.3), (-0.56, 0.08),
             (-0.55, 0.45), (-0.36, 0.75), (0.0, 0.9), (0.36, 0.75),
             (0.56, 0.45), (0.6, 0.05), (0.46, -0.32), (0.24, -0.62)]
    return [(cx + x * s, cy + y * s) for x, y in shape]


def emblem_flame(d, cx, cy, s, color):
    d.polygon(_flame_poly(cx, cy, s), fill=color)
    d.polygon(_flame_poly(cx + s * 0.02, cy + s * 0.28, s * 0.55),
              fill=(255, 240, 190, 255))


def emblem_crescent(d, cx, cy, s, color):
    d.ellipse([cx - s, cy - s, cx + s, cy + s], fill=color)
    d.ellipse([cx - s + s * 0.55, cy - s - s * 0.12,
               cx + s + s * 0.55, cy + s - s * 0.12], fill=(0, 0, 0, 0))
    # three claw slashes
    for i, dx in enumerate((-0.45, 0.0, 0.45)):
        x0 = cx + s * (0.55 + dx * 0.5)
        d.line([x0, cy - s * 0.42 + i * 4, x0 + s * 0.5, cy + s * 0.55],
               fill=color, width=max(3, int(s * 0.09)))


def emblem_sword(d, cx, cy, s, color):
    w = s * 0.14
    d.polygon([(cx - w, cy + s * 0.55), (cx - w, cy - s * 0.65),
               (cx, cy - s * 1.05), (cx + w, cy - s * 0.65),
               (cx + w, cy + s * 0.55)], fill=color)
    d.rounded_rectangle([cx - s * 0.55, cy + s * 0.5, cx + s * 0.55, cy + s * 0.66],
                        radius=int(s * 0.08), fill=color)
    d.rectangle([cx - w * 0.6, cy + s * 0.66, cx + w * 0.6, cy + s * 0.98],
                fill=color)
    d.ellipse([cx - s * 0.14, cy + s * 0.94, cx + s * 0.14, cy + s * 1.2],
              fill=color)
    d.line([cx, cy - s * 0.9, cx, cy + s * 0.45],
           fill=(255, 255, 255, 200), width=max(2, int(s * 0.045)))


def emblem_tree(d, cx, cy, s, color):
    d.polygon([(cx - s * 0.12, cy + s * 1.0), (cx - s * 0.07, cy + s * 0.1),
               (cx + s * 0.07, cy + s * 0.1), (cx + s * 0.12, cy + s * 1.0)],
              fill=color)
    for ang in (-0.9, -0.2, 0.6):
        x1 = cx + math.sin(ang) * s * 0.55
        y1 = cy - s * 0.1 - math.cos(ang) * s * 0.35
        d.line([cx, cy + s * 0.25, x1, y1], fill=color,
               width=max(3, int(s * 0.09)))
    for (bx, by, br) in [(0, -0.55, 0.52), (-0.5, -0.3, 0.4), (0.5, -0.25, 0.42),
                         (-0.25, -0.7, 0.36), (0.3, -0.65, 0.38)]:
        r = s * br
        d.ellipse([cx + s * bx - r, cy + s * by - r,
                   cx + s * bx + r, cy + s * by + r], fill=color)


def emblem_bolt(d, cx, cy, s, color):
    """A bold filled lightning bolt."""
    shape = [(0.3, -1.05), (-0.42, 0.1), (-0.06, 0.1), (-0.3, 1.05),
             (0.46, -0.18), (0.08, -0.18)]
    d.polygon([(cx + x * s, cy + y * s) for x, y in shape], fill=color)
    d.line([cx + s * 0.14, cy - s * 0.82, cx - s * 0.2, cy - s * 0.02,
            cx + s * 0.12, cy - s * 0.02, cx - s * 0.12, cy + s * 0.7],
           fill=(255, 255, 235, 235), width=max(3, int(s * 0.06)))


def emblem_rock(d, cx, cy, s, color):
    pts = []
    rng = random.Random(7)
    for i in range(7):
        ang = math.pi * 2 * i / 7 - math.pi / 2
        r = s * rng.uniform(0.82, 1.0)
        pts.append((cx + math.cos(ang) * r, cy + math.sin(ang) * r * 0.9))
    d.polygon(pts, fill=color)
    crack = (30, 26, 34, 255)
    d.line([cx - s * 0.35, cy - s * 0.7, cx - s * 0.05, cy - s * 0.1,
            cx - s * 0.4, cy + s * 0.45], fill=crack, width=max(3, int(s * 0.05)))
    d.line([cx + s * 0.4, cy - s * 0.45, cx + s * 0.12, cy + s * 0.05,
            cx + s * 0.38, cy + s * 0.6], fill=crack, width=max(3, int(s * 0.05)))
    # glowing rune
    d.line([cx - s * 0.12, cy - s * 0.28, cx + s * 0.16, cy - s * 0.02,
            cx - s * 0.1, cy + s * 0.3], fill=(255, 210, 120, 255),
           width=max(4, int(s * 0.08)))


def emblem_wisp(d, cx, cy, s, color):
    for i in range(60):
        t = i / 59
        ang = t * math.pi * 3.4
        r = s * (0.15 + 0.85 * t)
        x = cx + math.cos(ang) * r
        y = cy + math.sin(ang) * r * 0.85
        rr = s * 0.12 * (1 - t * 0.7)
        d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=color)
    d.ellipse([cx - s * 0.2, cy - s * 0.2, cx + s * 0.2, cy + s * 0.2],
              fill=(255, 255, 255, 230))


def emblem_dragon_eye(d, cx, cy, s, color):
    d.polygon([(cx - s, cy)] +
              [(cx + math.cos(a) * s, cy - math.sin(a) * s * 0.55)
               for a in [math.pi * t / 20 for t in range(21)]][::-1] +
              [(cx + s, cy)] +
              [(cx + math.cos(a) * s, cy + math.sin(a) * s * 0.55)
               for a in [math.pi * t / 20 for t in range(21)]],
              fill=color)
    d.ellipse([cx - s * 0.62, cy - s * 0.5, cx + s * 0.62, cy + s * 0.5],
              fill=(250, 214, 120, 255))
    d.polygon([(cx, cy - s * 0.5), (cx + s * 0.13, cy),
               (cx, cy + s * 0.5), (cx - s * 0.13, cy)], fill=(24, 12, 10, 255))


# ---------------------------------------------------------------------------
# illustrations: layered scene + glowing emblem ring
# ---------------------------------------------------------------------------
CREATURES = [
    # key, emblem fn, sky stops (top→bottom), ridge colors, glow rgb, accents
    ("ember_whelp", emblem_flame,
     [(0.0, (28, 8, 24)), (0.55, (120, 30, 26)), (1.0, (242, 120, 40))],
     [(64, 18, 28), (40, 12, 22), (24, 8, 16)], (255, 150, 60),
     dict(body=(255, 190, 90), body_y=0.42, stars=40, sparks=(255, 170, 80))),
    ("arcane_wisp", emblem_wisp,
     [(0.0, (10, 12, 40)), (0.6, (42, 30, 90)), (1.0, (110, 80, 170))],
     [(30, 24, 66), (22, 18, 52), (14, 12, 38)], (170, 140, 255),
     dict(body=(220, 210, 255), body_y=0.3, stars=120, sparks=(190, 170, 255))),
    ("moon_wolf", emblem_crescent,
     [(0.0, (6, 10, 26)), (0.6, (18, 30, 62)), (1.0, (52, 74, 110))],
     [(16, 24, 44), (12, 18, 36), (8, 12, 26)], (180, 210, 255),
     dict(body=(228, 238, 255), body_y=0.24, stars=110, sparks=(200, 220, 255))),
    ("storm_archer", emblem_bolt,
     [(0.0, (16, 20, 30)), (0.55, (44, 56, 74)), (1.0, (96, 116, 128))],
     [(30, 38, 52), (22, 28, 40), (14, 18, 28)], (255, 245, 180),
     dict(body=(240, 240, 220), body_y=0.35, stars=25, sparks=(255, 245, 190))),
    ("silver_knight", emblem_sword,
     [(0.0, (24, 26, 44)), (0.55, (70, 80, 116)), (1.0, (172, 182, 205))],
     [(52, 58, 84), (38, 44, 66), (26, 30, 48)], (220, 228, 255),
     dict(body=(250, 250, 255), body_y=0.3, stars=45, sparks=(230, 235, 255))),
    ("forest_treant", emblem_tree,
     [(0.0, (10, 26, 20)), (0.55, (28, 66, 40)), (1.0, (110, 150, 74))],
     [(20, 44, 30), (14, 34, 24), (8, 22, 16)], (150, 220, 110),
     dict(body=(230, 255, 190), body_y=0.32, stars=35, sparks=(170, 230, 120))),
    ("stone_golem", emblem_rock,
     [(0.0, (20, 16, 22)), (0.55, (56, 44, 48)), (1.0, (130, 104, 84))],
     [(44, 34, 36), (32, 26, 28), (20, 16, 20)], (255, 190, 110),
     dict(body=(255, 214, 150), body_y=0.4, stars=20, sparks=(255, 200, 130))),
    ("elder_dragon", emblem_dragon_eye,
     [(0.0, (16, 6, 20)), (0.5, (74, 18, 40)), (1.0, (190, 80, 40))],
     [(56, 16, 30), (40, 12, 24), (26, 8, 18)], (255, 170, 70),
     dict(body=(255, 210, 120), body_y=0.35, stars=60, sparks=(255, 180, 90))),
]


def paint_illustration(key, emblem, sky, ridges, glow_rgb, accents, seed):
    rng = random.Random(seed)
    img = vgradient((ART_W, ART_H), sky)

    stars(img, rng, accents["stars"], ART_H * 0.55)
    body_y = ART_H * accents["body_y"]
    radial_glow(img, (ART_W * 0.5, body_y), ART_W * 0.55, glow_rgb, 120)

    # far → near ridges
    paint_ridge(img, rng, ART_H * 0.58, 34, ridges[0], blur=2)
    mist(img, rng, ART_H * 0.55, ART_H * 0.75, lerp_rgb(glow_rgb, (255, 255, 255), 0.3))
    paint_ridge(img, rng, ART_H * 0.72, 46, ridges[1], blur=1)
    paint_ridge(img, rng, ART_H * 0.86, 40, ridges[2])
    speckle(img, rng, 60, accents["sparks"], ART_H * 0.5, ART_H, rmax=1.4)

    # emblem ring + glyph
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    cx, cy, s = ART_W * 0.5, body_y + 14, ART_W * 0.17
    ring_c = accents["body"] + (235,)
    for rr, wdt in ((s * 1.55, 10), (s * 1.78, 4)):
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], outline=ring_c,
                  width=wdt)
    # ring ticks
    for i in range(8):
        a = math.pi * 2 * i / 8
        r0, r1 = s * 1.78, s * 1.95
        d.line([cx + math.cos(a) * r0, cy + math.sin(a) * r0,
                cx + math.cos(a) * r1, cy + math.sin(a) * r1],
               fill=ring_c, width=5)
    emblem(d, cx, cy, s, accents["body"] + (255,))
    halo = layer.filter(ImageFilter.GaussianBlur(10))
    img.paste(halo, (0, 0), halo)
    img.paste(layer, (0, 0), layer)

    vignette(img, 120)
    return img


# ---------------------------------------------------------------------------
# card back / hidden face / front template
# ---------------------------------------------------------------------------
def filigree_corner(d, x, y, sx, sy, color, s=70):
    """A little corner flourish; sx/sy = ±1 mirror the quadrant."""
    for i, r in enumerate((s, s * 0.72, s * 0.46)):
        d.arc([x - r if sx > 0 else x - r, y - r if sy > 0 else y - r,
               x + r, y + r],
              start=0 if (sx > 0 and sy > 0) else 90 if (sx < 0 and sy > 0)
              else 180 if (sx < 0 and sy < 0) else 270,
              end=90 if (sx > 0 and sy > 0) else 180 if (sx < 0 and sy > 0)
              else 270 if (sx < 0 and sy < 0) else 360,
              fill=color, width=4 - i)


def diamond(d, cx, cy, rx, ry, **kw):
    d.polygon([(cx, cy - ry), (cx + rx, cy), (cx, cy + ry), (cx - rx, cy)], **kw)


def paint_back(base, line, glow_rgb, seed):
    rng = random.Random(seed)
    img = vgradient((CARD_W, CARD_H),
                    [(0.0, lerp_rgb(base, (0, 0, 0), 0.25)), (0.5, base),
                     (1.0, lerp_rgb(base, (0, 0, 0), 0.35))])
    d = ImageDraw.Draw(img)

    # woven lattice, faint
    lat = lerp_rgb(base, line, 0.22)
    for k in range(-CARD_H, CARD_W + CARD_H, 34):
        d.line([k, 0, k + CARD_H, CARD_H], fill=lat, width=2)
        d.line([k + CARD_H, 0, k, CARD_H], fill=lat, width=2)

    cx, cy = CARD_W / 2, CARD_H / 2
    radial_glow(img, (cx, cy), 300, glow_rgb, 70)
    d = ImageDraw.Draw(img)

    # borders
    d.rounded_rectangle([10, 10, CARD_W - 11, CARD_H - 11], radius=34,
                        outline=line, width=6)
    d.rounded_rectangle([26, 26, CARD_W - 27, CARD_H - 27], radius=24,
                        outline=line, width=2)

    # central mandala: diamonds + rings
    diamond(d, cx, cy, 150, 240, outline=line, width=5)
    diamond(d, cx, cy, 110, 178, outline=line, width=3)
    for r in (120, 86):
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=line, width=3)
    diamond(d, cx, cy, 64, 64, fill=lerp_rgb(base, line, 0.55))
    diamond(d, cx, cy, 40, 40, fill=lerp_rgb(base, (255, 255, 255), 0.75))
    diamond(d, cx, cy, 18, 18, fill=base)
    for i in range(12):
        a = math.pi * 2 * i / 12
        x0, y0 = cx + math.cos(a) * 124, cy + math.sin(a) * 124
        x1, y1 = cx + math.cos(a) * 146, cy + math.sin(a) * 146
        d.line([x0, y0, x1, y1], fill=line, width=4)

    # corner diamonds
    for qx, qy in ((70, 70), (CARD_W - 70, 70), (70, CARD_H - 70),
                   (CARD_W - 70, CARD_H - 70)):
        diamond(d, qx, qy, 26, 40, outline=line, width=3)
        diamond(d, qx, qy, 12, 20, fill=line)

    speckle(img, rng, 90, lerp_rgb(line, (255, 255, 255), 0.3), 0, CARD_H, 1.2)
    vignette(img, 110)

    out = Image.new("RGBA", (CARD_W, CARD_H), (0, 0, 0, 0))
    out.paste(img, (0, 0))
    out.putalpha(rounded_mask((CARD_W, CARD_H), 34))
    return out


def paint_front_template():
    """The shared front face: frame, banners and stat gems. The art window is
    TRANSPARENT — the app paints the illustration behind it, then draws the
    name/cost/stats on top at composite time."""
    frame_base = (36, 30, 46)
    gold = (208, 172, 92)
    gold_hi = (244, 214, 140)
    parchment = (222, 208, 178)
    parchment_dark = (198, 182, 150)

    img = vgradient((CARD_W, CARD_H),
                    [(0.0, lerp_rgb(frame_base, (0, 0, 0), 0.1)),
                     (0.55, frame_base),
                     (1.0, lerp_rgb(frame_base, (0, 0, 0), 0.4))])
    d = ImageDraw.Draw(img)

    # frame texture: subtle diagonal weave
    weave = lerp_rgb(frame_base, gold, 0.10)
    for k in range(-CARD_H, CARD_W + CARD_H, 22):
        d.line([k, 0, k + CARD_H, CARD_H], fill=weave, width=1)

    # outer border
    d.rounded_rectangle([8, 8, CARD_W - 9, CARD_H - 9], radius=34,
                        outline=gold, width=6)
    d.rounded_rectangle([20, 20, CARD_W - 21, CARD_H - 21], radius=26,
                        outline=lerp_rgb(gold, frame_base, 0.45), width=2)

    art = (52, 118, CARD_W - 52, 448)  # the transparent window

    # name banner
    d.rounded_rectangle([40, 44, CARD_W - 40, 102], radius=16,
                        fill=lerp_rgb(frame_base, (0, 0, 0), 0.35),
                        outline=gold, width=3)

    # art frame (drawn around the future hole)
    d.rectangle([art[0] - 8, art[1] - 8, art[2] + 8, art[3] + 8],
                outline=gold, width=4)
    d.rectangle([art[0] - 3, art[1] - 3, art[2] + 3, art[3] + 3],
                outline=lerp_rgb(gold, (0, 0, 0), 0.4), width=2)

    # text box (parchment)
    tb = (44, 476, CARD_W - 44, 664)
    d.rounded_rectangle(tb, radius=14, fill=parchment, outline=gold, width=3)
    for y in range(tb[1] + 26, tb[3] - 16, 30):
        d.line([tb[0] + 18, y, tb[2] - 18, y], fill=parchment_dark, width=1)
    # divider flourish
    d.line([tb[0] + 60, tb[1] + 2, tb[2] - 60, tb[1] + 2], fill=gold_hi, width=1)

    # bottom band
    d.rounded_rectangle([40, 682, CARD_W - 40, 726], radius=12,
                        fill=lerp_rgb(frame_base, (0, 0, 0), 0.3),
                        outline=lerp_rgb(gold, frame_base, 0.5), width=2)

    # stat gems: cost (top-right, blue), attack (bottom-left, ember),
    # health (bottom-right, heart-green)
    def gem(cx, cy, r, c0, c1):
        d.ellipse([cx - r - 5, cy - r - 5, cx + r + 5, cy + r + 5],
                  fill=gold, outline=gold_hi, width=2)
        for i in range(r, 0, -1):
            t = i / r
            d.ellipse([cx - i, cy - i, cx + i, cy + i],
                      fill=lerp_rgb(c1, c0, t))
        d.ellipse([cx - r * 0.55, cy - r * 0.72, cx + r * 0.1, cy - r * 0.2],
                  fill=lerp_rgb(c0, (255, 255, 255), 0.55))

    gem(CARD_W - 64, 73, 34, (70, 120, 220), (16, 34, 90))       # cost
    gem(66, 704, 34, (226, 108, 58), (110, 28, 18))              # attack
    gem(CARD_W - 66, 704, 34, (96, 200, 96), (18, 84, 40))       # health

    # cut the art window + round the card
    alpha = rounded_mask((CARD_W, CARD_H), 34)
    ImageDraw.Draw(alpha).rectangle(art, fill=0)
    out = Image.new("RGBA", (CARD_W, CARD_H), (0, 0, 0, 0))
    out.paste(img, (0, 0))
    out.putalpha(alpha)
    return out


# ---------------------------------------------------------------------------
# sounds — tiny synthesized WAVs (22050 Hz mono PCM16)
# ---------------------------------------------------------------------------
RATE = 22050


def write_wav(name, samples):
    path = os.path.join(SFX, name)
    clipped = [max(-1.0, min(1.0, s)) for s in samples]
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(s * 32000)) for s in clipped))
    print(f"  {os.path.relpath(path, ROOT)}  ({len(samples) / RATE:.2f}s)")


def env(t, dur, attack=0.005, release=None):
    release = release if release is not None else dur * 0.6
    if t < attack:
        return t / attack
    if t > dur - release:
        return max(0.0, (dur - t) / release)
    return 1.0


def sine_sweep(dur, f0, f1, vol=0.8, attack=0.005, release=None, vib=0.0):
    out = []
    phase = 0.0
    n = int(dur * RATE)
    for i in range(n):
        t = i / RATE
        f = f0 + (f1 - f0) * (t / dur)
        if vib:
            f *= 1 + 0.02 * math.sin(2 * math.pi * vib * t)
        phase += 2 * math.pi * f / RATE
        out.append(math.sin(phase) * vol * env(t, dur, attack, release))
    return out

def noise_swish(dur, vol=0.5, cutoff=0.35, seed=1):
    """Amplitude-shaped, crudely low-passed noise: a card-sliding swish."""
    rng = random.Random(seed)
    out = []
    n = int(dur * RATE)
    prev = 0.0
    for i in range(n):
        t = i / RATE
        prev = prev * cutoff + rng.uniform(-1, 1) * (1 - cutoff)
        shape = math.sin(math.pi * min(1.0, t / dur)) ** 2
        out.append(prev * vol * shape)
    return out


def mix(*tracks):
    n = max(len(t) for t in tracks)
    out = [0.0] * n
    for t in tracks:
        for i, s in enumerate(t):
            out[i] += s
    return out


def delayed(track, seconds):
    return [0.0] * int(seconds * RATE) + track


def chord_arp(notes, step, dur, vol=0.5):
    tracks = []
    for i, f in enumerate(notes):
        tracks.append(delayed(
            sine_sweep(dur, f, f, vol=vol, attack=0.01, release=dur * 0.7),
            i * step))
    return mix(*tracks)


def make_sounds():
    # draw: a quick soft swish
    write_wav("draw.wav", noise_swish(0.22, vol=0.55, cutoff=0.55, seed=3))
    # play: felt thump + low knock
    write_wav("play.wav", mix(
        sine_sweep(0.16, 150, 62, vol=0.9, release=0.12),
        noise_swish(0.08, vol=0.22, cutoff=0.2, seed=9)))
    # attack: whoosh into an impact crack
    write_wav("attack.wav", mix(
        noise_swish(0.28, vol=0.5, cutoff=0.7, seed=5),
        delayed(sine_sweep(0.12, 220, 70, vol=0.9, release=0.1), 0.16),
        delayed(noise_swish(0.06, vol=0.6, cutoff=0.15, seed=6), 0.16)))
    # hurt: dull hit + descending groan
    write_wav("hurt.wav", mix(
        noise_swish(0.09, vol=0.5, cutoff=0.2, seed=11),
        sine_sweep(0.3, 180, 90, vol=0.6, release=0.24, vib=9)))
    # fanfare up / down
    write_wav("win.wav", chord_arp([392, 494, 587, 784], 0.11, 0.5, vol=0.42))
    write_wav("lose.wav", chord_arp([330, 262, 208, 165], 0.16, 0.55, vol=0.42))
    # turn chime: two gentle bell partials
    write_wav("turn.wav", mix(
        sine_sweep(0.35, 660, 660, vol=0.28, release=0.3),
        sine_sweep(0.35, 990, 990, vol=0.12, release=0.3)))


# ---------------------------------------------------------------------------
def main():
    print("painting card faces…")
    paint_front_template().save(os.path.join(OUT, "card_front.png"))
    print(f"  assets/duel/card_front.png")
    paint_back((44, 22, 66), (208, 172, 92), (150, 100, 220), seed=1).save(
        os.path.join(OUT, "card_back.png"))
    print(f"  assets/duel/card_back.png")
    # the concealing front shown for face-down cards: a steel variant
    paint_back((38, 44, 56), (140, 158, 178), (120, 160, 200), seed=2).save(
        os.path.join(OUT, "card_hidden.png"))
    print(f"  assets/duel/card_hidden.png")

    print("painting illustrations…")
    for i, (key, emblem, sky, ridges, glow_rgb, accents) in enumerate(CREATURES):
        img = paint_illustration(key, emblem, sky, ridges, glow_rgb, accents,
                                 seed=100 + i)
        img.save(os.path.join(OUT, f"art_{key}.png"))
        print(f"  assets/duel/art_{key}.png")

    print("synthesizing sounds…")
    make_sounds()
    print("done.")


if __name__ == "__main__":
    main()
