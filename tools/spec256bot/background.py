"""Backgrounds for Spec256: 320x200 pictures of palette indices (.B00).

A background shows wherever a pixel's colour is 0, so behind everything the
game leaves as paper. paint(spec) draws one from a short description that
Claude picks for the game:

    {"kind": "nebula", "ramps": ["rose", "navy"], "brightness": 2, "seed": 7}

kind is one of KINDS; ramps (1 to 3) are names from RAMPS, first the main
colour; brightness 1 (barely there) to 4 (bold) caps how far up each ramp it
goes, so sprites stay readable on top. Levels between shades are dithered.
"""
import math
import random

W, H = 320, 200
# Ramps that start at black, so a background can stay dark.
RAMPS = ["zx_blue", "zx_red", "zx_magenta", "zx_green", "zx_cyan", "zx_yellow", "zx_white",
         "bright_blue", "bright_red", "bright_magenta", "bright_green", "bright_cyan",
         "bright_yellow", "grey", "sky"]
BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
KINDS = {
    "none": "no background, the game's own paper colours",
    "starfield": "black space with stars of different sizes and brightness",
    "nebula": "space with soft clouds of gas in the ramp colours, and stars",
    "sky": "sky graded down to the horizon, distant hills along the bottom",
    "hills": "rolling hills in layers, nearer ones darker, under a graded sky",
    "cave": "rough rock walls, lighter in the middle",
    "bricks": "a wall of bricks with dark mortar",
    "water": "underwater: light rays from the top, waving bands, bubbles",
    "gradient": "a plain gradient from dark at the bottom to the top colour",
}


class Noise:
    """Smooth 2D value noise, several octaves."""

    def __init__(self, seed, cell=40):
        r = random.Random(seed)
        self.cell = cell
        self.grid = [[r.random() for _ in range(64)] for _ in range(64)]

    def _at(self, x, y):
        xi, yi = int(x), int(y)
        fx, fy = x - xi, y - yi
        fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
        g = self.grid
        a = g[yi & 63][xi & 63] + (g[yi & 63][(xi + 1) & 63] - g[yi & 63][xi & 63]) * fx
        b = g[(yi + 1) & 63][xi & 63] + (g[(yi + 1) & 63][(xi + 1) & 63] - g[(yi + 1) & 63][xi & 63]) * fx
        return a + (b - a) * fy

    def __call__(self, x, y, octaves=4):
        v, amp, f, tot = 0.0, 1.0, 1.0 / self.cell, 0.0
        for o in range(octaves):
            v += amp * self._at(x * f + o * 17.3, y * f + o * 9.1)
            tot += amp
            amp *= 0.5
            f *= 2
        return v / tot


def paint(spec, ramps):
    """The background as bytes (320*200 palette indices), or None."""
    kind = spec.get("kind", "none")
    if kind == "none" or kind not in KINDS:
        return None
    names = [r for r in spec.get("ramps", []) if r in RAMPS] or ["zx_blue"]
    bases = [ramps[n] for n in names]
    top = (2, 3, 4, 6)[max(1, min(4, int(spec.get("brightness", 2)))) - 1]   # highest shade
    rnd = random.Random(spec.get("seed", 1))
    img = bytearray(W * H)

    def put(x, y, c):
        if 0 <= x < W and 0 <= y < H:
            img[y * W + x] = c

    def shade(i, level, x=0, y=0):
        """Ramp i at level 0..1 of the allowed brightness, dithered; 0 is black."""
        lv = int(level * top + (BAYER[y & 3][x & 3] + 0.5) / 16)
        lv = min(lv, top)
        return bases[i % len(bases)] + lv if lv > 0 else 0

    def stars(n):
        for _ in range(n):
            x, y = rnd.randrange(W), rnd.randrange(H)
            b = rnd.random()
            put(x, y, ramps["grey"] + (7 if b > 0.95 else 6 if b > 0.8 else 4))
            if b > 0.97:
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    put(x + dx, y + dy, ramps["grey"] + 3)

    if kind == "starfield":
        stars(260)
    elif kind == "nebula":
        n1, n2 = Noise(rnd.random(), 70), Noise(rnd.random(), 45)
        for y in range(H):
            for x in range(W):
                a = max(0.0, n1(x, y) - 0.45) * 2.2
                b = max(0.0, n2(x, y) - 0.55) * 2.5
                if a > b and a > 0.08:
                    img[y * W + x] = shade(0, min(1, a), x, y)
                elif b > 0.08:
                    img[y * W + x] = shade(1, min(1, b), x, y)
        stars(180)
    elif kind in ("sky", "hills", "gradient"):
        for y in range(H):
            t = y / (H - 1)
            lv = 0.25 + 0.75 * (1 - t) if kind != "gradient" else t
            for x in range(W):
                img[y * W + x] = shade(0, lv, x, y)
        if kind != "gradient":
            layers = 1 if kind == "sky" else 3
            for k in range(layers):
                n = Noise(rnd.random(), 90 - 20 * k)
                base = H - 30 - (layers - 1 - k) * 22
                for x in range(W):
                    h = base - int(40 * n(x, k * 50, 3))
                    for y in range(max(0, h), H):
                        img[y * W + x] = shade(1, 0.9 - 0.25 * k - 0.3 * (y - h) / H, x, y)
    elif kind == "cave":
        n = Noise(rnd.random(), 30)
        for y in range(H):
            for x in range(W):
                d = abs(x - W / 2) / (W / 2) * 0.5 + abs(y - H / 2) / (H / 2) * 0.3
                img[y * W + x] = shade(0, max(0.0, n(x, y, 5) - d * 0.6) * 1.3, x, y)
    elif kind == "bricks":
        bw, bh = 32, 12
        n = Noise(rnd.random(), 8)
        for y in range(H):
            row = y // bh
            for x in range(W):
                xx = x + (bw // 2 if row & 1 else 0)
                if y % bh == 0 or xx % bw == 0:
                    continue                    # mortar stays black
                img[y * W + x] = shade(0, 0.45 + 0.55 * n(x, y, 2) * (0.7 + 0.3 * ((xx // bw + row) % 3) / 2), x, y)
    elif kind == "water":
        for y in range(H):
            for x in range(W):
                band = 0.5 + 0.5 * math.sin(y / 9 + 1.5 * math.sin(x / 37))
                ray = max(0.0, math.sin(x / 23 + y / 60) - 0.6) * (1 - y / H) * 1.5
                img[y * W + x] = shade(0, min(1, 0.25 + 0.35 * band * (1 - y / H) + ray), x, y)
        for _ in range(40):
            x, y, r = rnd.randrange(W), rnd.randrange(H), rnd.choice((1, 1, 2, 3))
            for a in range(0, 360, 20):
                put(int(x + r * math.cos(math.radians(a))), int(y + r * math.sin(math.radians(a))),
                    shade(len(bases) - 1, 1.0))
    return bytes(img)
