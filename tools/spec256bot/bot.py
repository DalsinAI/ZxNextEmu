#!/usr/bin/env python3
"""spec256bot: colour a classic 48K Spectrum game for Spec256.

    bot.py trace  game.sna work/            run the tracer (build/s256trace)
    bot.py sheet  work/                     group the graphics into sprites,
                                            draw work/sheet.png
    bot.py ai     work/ --title "Jetpac"    ask Claude to plan the colours
                                            (needs ANTHROPIC_API_KEY)
    bot.py emit   work/ game.sna out/       write out/NAME.SNA, .GFX, .CFG

`emit` uses work/plan.json when it is there (from `ai`, or written by hand)
and otherwise colours every sprite in the ramp of the Spectrum colour it was
drawn in. See README.md.
"""
import argparse
import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zlib
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# The Spec256 palette's ramps: first entry of 8, darkest first.
RAMPS = {
    "sky": 0, "rose": 8, "teal": 16, "orange": 24, "brown": 32, "purple": 40,
    "navy": 48, "lime": 56, "fire": 64, "khaki": 72, "grey": 80, "skin": 88,
    "cyan": 96, "green": 104, "slate": 112, "gold": 120, "aqua": 128,
    "zx_blue": 136, "zx_red": 144, "zx_magenta": 152, "zx_green": 160,
    "zx_cyan": 168, "zx_yellow": 176, "zx_white": 184,
    "bright_blue": 200, "bright_red": 208, "bright_magenta": 216,
    "bright_green": 224, "bright_cyan": 232, "bright_yellow": 240,
    # Greys that the picture averages with the cell's ink (UpColorsMixed=8):
    # shading in whatever colour the game gives that place on the screen.
    "ink": 248,
}
RAMP_LOOKS = {
    "sky": "black to deep blue to pale sky blue", "rose": "dark red to pink",
    "teal": "dark teal to mint", "orange": "orange to cream yellow",
    "brown": "dark brown to peach", "purple": "plum to pale pink",
    "navy": "navy to lavender", "lime": "moss green to lemon",
    "fire": "red through orange to yellow", "khaki": "olive drab to cream",
    "grey": "black to white", "skin": "tan to light skin",
    "cyan": "turquoise to pale cyan", "green": "grass green to pale green",
    "slate": "blue grey to pale lilac", "gold": "dark gold to pale yellow",
    "aqua": "dark teal to bright cyan",
}
STYLES = ("bevel", "gradient", "flat")
# Spectrum colour (0-7, +8 bright) to the ramp of that colour.
ZX_RAMP = ["ink", "zx_blue", "zx_red", "zx_magenta", "zx_green", "zx_cyan",
           "zx_yellow", "zx_white", "ink", "bright_blue", "bright_red", "bright_magenta", "bright_green", "bright_cyan",
           "bright_yellow", "grey"]
ZX_RGB = [(0, 0, 0), (0, 0, 0xd7), (0xd7, 0, 0), (0xd7, 0, 0xd7), (0, 0xd7, 0), (0, 0xd7, 0xd7),
          (0xd7, 0xd7, 0), (0xd7, 0xd7, 0xd7), (0, 0, 0), (0, 0, 0xff), (0xff, 0, 0), (0xff, 0, 0xff),
          (0, 0xff, 0), (0, 0xff, 0xff), (0xff, 0xff, 0), (0xff, 0xff, 0xff)]


# ---- tracing ------------------------------------------------------------

def cmd_trace(a):
    exe = os.path.join(ROOT, "build", "s256trace")
    subprocess.check_call(["make", "-s", "-C", ROOT, "build/s256trace"])  # make rebuilds it only when a source changed
    os.makedirs(a.work, exist_ok=True)
    # A new trace makes the old sprites and plan wrong, so sheet has to run again.
    for f in ("sprites.json", "plan.json", "snapshot.sha256"):
        if os.path.exists(os.path.join(a.work, f)):
            os.remove(os.path.join(a.work, f))
    subprocess.check_call([exe, "-r", a.roms, "-f", str(a.frames), "-k", str(a.seed),
                           "-s", str(a.frames // 3), a.snapshot, a.work])
    with open(os.path.join(a.work, "snapshot.sha256"), "w") as f:
        f.write(sha256_of(a.snapshot) + "\n")


def sha256_of(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def load(work):
    mem = open(os.path.join(work, "mem.bin"), "rb").read()
    trace = {e["a"]: e for e in json.load(open(os.path.join(work, "trace.json")))}
    return mem, trace


# ---- sprites ------------------------------------------------------------

def best(counts):
    return max(range(len(counts)), key=lambda i: counts[i])


def find_sprites(mem, trace):
    """Group drawn bytes into sprites: runs of nearby addresses that the game
    draws with the same width (bytes per pixel row)."""
    addrs = sorted(a for a in trace if a >= 0x5b00 or 0x3d00 <= a < 0x4000)
    sprites = []
    cur = None
    for a in addrs:
        e = trace[a]
        st = e["stride"]
        w = int(max(st, key=st.get)) if st and max(st.values()) >= 2 else None
        if cur and a - cur["end"] <= max(4, cur["w"] or 1) and (w is None or cur["w"] is None or w == cur["w"]):
            cur["end"] = a
            cur["w"] = cur["w"] or w
            cur["bytes"].append(a)
        else:
            cur = {"start": a, "end": a, "w": w, "bytes": [a]}
            sprites.append(cur)
    out = []
    for i, s in enumerate(sprites):
        w = s["w"] or 1
        # Which bytes start a row: the ones never drawn just right of the byte before.
        def misses(p):
            return sum((trace[a]["left"] > 0) == ((a - p) % w == 0) for a in s["bytes"])
        phase = min(range(w), key=misses)
        start = s["start"] - (s["start"] - phase) % w
        h = (s["end"] - start) // w + 1
        ink = [0] * 16
        paper = [0] * 16
        n = up = down = 0
        for a in s["bytes"]:
            e = trace[a]
            n += e["n"]
            up += e.get("up", 0)
            down += sum(e["stride"].values()) - e.get("up", 0)
            for k in range(16):
                ink[k] += e["ink"][k]
                paper[k] += e["paper"][k]
        out.append({"id": len(out), "start": start, "w": w, "h": h,
                    "drawn": sorted(s["bytes"]), "times": n,
                    "ink": best(ink), "paper": best(paper),
                    "ink_share": round(max(ink) / max(1, sum(ink)), 2),
                    "upside_down": up > down})
    return out


def rows_of(s):
    """Memory row numbers from the top of the picture down."""
    r = range(s["h"])
    return reversed(r) if s.get("upside_down") else r


def pixels(mem, s):
    """The sprite as rows of 0/1, w*8 wide, top row first."""
    rows = []
    for y in rows_of(s):
        row = []
        for xb in range(s["w"]):
            a = s["start"] + y * s["w"] + xb
            v = mem[a] if a < 65536 else 0
            row.extend((v >> (7 - b)) & 1 for b in range(8))
        rows.append(row)
    return rows


# ---- PNG (no dependencies) --------------------------------------------------

def write_png(path, w, h, rgb):
    raw = b"".join(b"\0" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


# 3x5 digits for labels
DIGITS = ["111101101101111", "010110010010111", "111001111100111", "111001111001111",
          "101101111001001", "111100111001111", "111100111101111", "111001001001001",
          "111101111101111", "111101111001111"]


def draw_sheet(mem, sprites, path, scale=4, cols=1100):
    """Every sprite in its drawn colour on dark grey, with its id above it."""
    pad = 6
    x = y = pad
    line_h = 0
    places = []
    for s in sprites:
        w, h = s["w"] * 8 * scale, s["h"] * scale + 14
        if x + w + pad > cols:
            x, y = pad, y + line_h + pad
            line_h = 0
        places.append((x, y))
        x += max(w, 30) + pad
        line_h = max(line_h, h)
    H = y + line_h + pad
    W = cols
    img = bytearray([40, 40, 40] * W * H)

    def put(px, py, c):
        if 0 <= px < W and 0 <= py < H:
            img[(py * W + px) * 3:(py * W + px) * 3 + 3] = bytes(c)

    for s, (x0, y0) in zip(sprites, places):
        for i, ch in enumerate(str(s["id"])):
            for k, bit in enumerate(DIGITS[int(ch)]):
                if bit == "1":
                    for dy in range(2):
                        for dx in range(2):
                            put(x0 + i * 8 + (k % 3) * 2 + dx, y0 + (k // 3) * 2 + dy, (255, 255, 255))
        ink = ZX_RGB[s["ink"]] if s["ink"] % 8 else (255, 255, 255)
        paper = (0, 0, 0)
        for r, row in enumerate(pixels(mem, s)):
            for c, v in enumerate(row):
                for dy in range(scale):
                    for dx in range(scale):
                        put(x0 + c * scale + dx, y0 + 14 + r * scale + dy, ink if v else paper)
    write_png(path, W, H, img)


def cmd_sheet(a):
    mem, trace = load(a.work)
    sprites = find_sprites(mem, trace)
    json.dump(sprites, open(os.path.join(a.work, "sprites.json"), "w"))
    plan = os.path.join(a.work, "plan.json")
    if os.path.exists(plan):
        os.remove(plan)  # its sprite ids belonged to the old sheet; run ai again
        print(f"removed the old {plan}; run ai again for the new sprites")
    draw_sheet(mem, sprites, os.path.join(a.work, "sheet.png"))
    print(f"{len(sprites)} sprites, {sum(len(s['drawn']) for s in sprites)} bytes -> {a.work}/sheet.png")


# ---- colour plan --------------------------------------------------------------

def default_ramp(s):
    """The ramp of the colour the game drew it in; "ink" when the game uses
    it in several colours (a font, a platform tile), or for ROM graphics."""
    if s["start"] < 0x4000 or s["ink_share"] < 0.85:
        return "ink"
    return ZX_RAMP[s["ink"]]


def default_plan(sprites):
    return {"sprites": [{"id": s["id"], "what": "",
                         "style": "gradient" if s["start"] < 0x4000 else "bevel",
                         "bands": [{"from_row": 0, "ramp": default_ramp(s)}]} for s in sprites]}


PLAN_SCHEMA = {
    "type": "object",
    "properties": {
        "sprites": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "id": {"type": "integer"},
                    "what": {"type": "string"},
                    "style": {"type": "string", "enum": list(STYLES)},
                    "bands": {
                        "type": "array",
                        "items": {
                            "type": "object",
                            "properties": {
                                "from_row": {"type": "integer"},
                                "ramp": {"type": "string", "enum": list(RAMPS)},
                            },
                            "required": ["from_row", "ramp"],
                            "additionalProperties": False,
                        },
                    },
                },
                "required": ["id", "what", "style", "bands"],
                "additionalProperties": False,
            },
        }
    },
    "required": ["sprites"],
    "additionalProperties": False,
}


def ai_prompt(title, sprites):
    looks = "\n".join(f"- {k}: {v}" for k, v in RAMP_LOOKS.items())
    listing = "\n".join(
        f"- sprite {s['id']}: {s['w'] * 8}x{s['h']} pixels, drawn {s['times']} times, "
        f"in Spectrum ink {s['ink'] % 8}{' bright' if s['ink'] >= 8 else ''} on paper {s['paper'] % 8}"
        + (f" ({round(100 * s['ink_share'])}% of the time; the game also uses other colours for it)"
           if s["ink_share"] < 0.85 else "")
        + (" (from the ROM: the Spectrum's own font, shared by all text)" if s["start"] < 0x4000 else "")
        for s in sprites)
    return f"""You are colouring the ZX Spectrum game "{title}" for Spec256, which gives every
pixel its own colour from a 256-colour palette instead of one ink per 8x8 cell.

The image shows every graphic the game drew while it was played, each labelled
with its number and shown in the Spectrum ink colour the game gave it. Some
graphics are tall strips holding several frames of animation or several
characters of a font stacked on top of each other.

For each sprite, say what it is (a few words; "unknown" is fine) and choose
how to colour its set pixels:

- bands: colour ramps by pixel row, top to bottom. The first band starts at
  row 0; add more bands where the object changes material (a helmet over a
  suit, flames under a rocket). For a stack of frames or font characters,
  usually one band.
- style: "bevel" shades each pixel lighter on its top and left edges and
  darker on its bottom and right edges (good for solid objects), "gradient"
  goes light to dark down each band (good for metal, flames, text), "flat"
  uses one shade (good for thin lines and tiny sprites).

Keep the game's own colour scheme recognisable: an object the game drew in
red should stay mostly red, but pick the richer ramps where they suit the
object better (skin for faces, fire for flames, grey or slate for metal).
Ramps:
{looks}
- zx_blue, zx_red, zx_magenta, zx_green, zx_cyan, zx_yellow, zx_white: the
  Spectrum's own colours, black up to normal brightness
- bright_blue, bright_red, bright_magenta, bright_green, bright_cyan,
  bright_yellow: the same up to full brightness
- ink: shades of whatever ink colour the game gives that spot of the screen
  at the time. Use it for graphics the game draws in different colours
  (fonts, tiles shared by several objects), so the game's colour coding
  survives.

The sprites:
{listing}

Answer for every sprite listed."""


def cmd_ai(a):
    import anthropic

    mem, trace = load(a.work)
    sprites = json.load(open(os.path.join(a.work, "sprites.json")))
    png = open(os.path.join(a.work, "sheet.png"), "rb").read()
    client = anthropic.Anthropic()
    response = client.beta.messages.create(
        model="claude-opus-5-5",
        max_tokens=32000,
        betas=["server-side-fallback-2026-07-01"],
        fallbacks="default",
        output_config={"effort": "high", "format": {"type": "json_schema", "schema": PLAN_SCHEMA}},
        messages=[{"role": "user", "content": [
            {"type": "image", "source": {"type": "base64", "media_type": "image/png",
                                         "data": base64.standard_b64encode(png).decode()}},
            {"type": "text", "text": ai_prompt(a.title, sprites)},
        ]}],
    )
    if response.stop_reason == "refusal":
        sys.exit(f"Claude declined: {response.stop_details}")
    if response.stop_reason == "max_tokens":
        sys.exit("the answer was cut off; try fewer sprites")
    text = next(b.text for b in response.content if b.type == "text")
    plan = json.loads(text)
    json.dump(plan, open(os.path.join(a.work, "plan.json"), "w"), indent=1)
    print(f"plan for {len(plan['sprites'])} sprites -> {a.work}/plan.json")


# ---- colouring and output -----------------------------------------------------

def shade(rows, x, y, style, band_top, band_bottom):
    """Position along the ramp (1-7) for a set pixel."""
    h, w = len(rows), len(rows[0])

    def on(xx, yy):
        return 0 <= xx < w and 0 <= yy < h and rows[yy][xx]
    if style == "flat":
        return 6
    if style == "gradient":
        span = max(1, band_bottom - band_top)
        return 7 - round(3 * (y - band_top) / span)
    lit = not on(x, y - 1) or not on(x - 1, y)
    dark = not on(x, y + 1) or not on(x + 1, y)
    return 6 if lit == dark else 7 if lit else 4


def colour_of(ramp, level):
    if ramp == "ink" or (RAMPS[ramp] < 136 and ramp != "grey"):
        level -= 2                  # the ramps' pale ends wash colour out
    c = RAMPS[ramp] + level
    return min(c, 254)              # 255 is "ink" under Paper00InkFF


def colour_sprite(mem, s, p):
    """{address: [colour of pixel bit 0..7]} for the sprite."""
    rows = pixels(mem, s)
    my = list(rows_of(s))
    bands = sorted(p["bands"], key=lambda b: b["from_row"]) or [{"from_row": 0, "ramp": ZX_RAMP[s["ink"]]}]
    out = {}
    for y in range(s["h"]):
        bi = max(i for i, b in enumerate(bands) if b["from_row"] <= y or i == 0)
        top = bands[bi]["from_row"]
        bottom = bands[bi + 1]["from_row"] - 1 if bi + 1 < len(bands) else s["h"] - 1
        ramp = bands[bi]["ramp"] if bands[bi]["ramp"] in RAMPS else ZX_RAMP[s["ink"]]
        for xb in range(s["w"]):
            a = s["start"] + my[y] * s["w"] + xb
            cols = [0] * 8
            for b in range(8):
                x = xb * 8 + 7 - b
                if rows[y][x]:
                    cols[b] = colour_of(ramp, shade(rows, x, y, p["style"], top, bottom))
            out[a] = cols
    return out


def cmd_emit(a):
    # The graphics planes are built from the traced memory, so they must go with that same snapshot.
    stamp = os.path.join(a.work, "snapshot.sha256")
    if not os.path.exists(stamp) or open(stamp).read().strip() != sha256_of(a.snapshot):
        sys.exit(f"{a.snapshot} is not the snapshot traced into {a.work}; run trace on it again first.")
    mem, trace = load(a.work)
    sprites = json.load(open(os.path.join(a.work, "sprites.json")))
    path = os.path.join(a.work, "plan.json")
    plan = json.load(open(path)) if os.path.exists(path) and not a.default else default_plan(sprites)
    by_id = {p["id"]: p for p in plan["sprites"]}
    colours = {}
    for s in sprites:
        p = by_id.get(s["id"]) or default_plan([s])["sprites"][0]
        drawn = set(s["drawn"])
        for addr, cols in colour_sprite(mem, s, p).items():
            if addr < len(mem) and mem[addr] and (a.fill or addr in drawn):
                colours[addr] = cols

    name = os.path.splitext(os.path.basename(a.snapshot))[0].upper()
    os.makedirs(a.out, exist_ok=True)
    # zxrun -g loads every Spectrum or Spec256 file in the folder, so clear out an earlier package first.
    for f in os.listdir(a.out):
        old = os.path.join(a.out, f)
        if re.search(r"\.(sna|z80|gf[x0-9ab]|b\d\d|cfg|pal)$", f, re.I) and not os.path.samefile(old, a.snapshot):
            os.remove(old)
    dst = os.path.join(a.out, name + ".SNA")
    if not (os.path.exists(dst) and os.path.samefile(dst, a.snapshot)):
        shutil.copyfile(a.snapshot, dst)

    def page(base):
        buf = bytearray(0x4000 * 8)
        for o in range(0x4000):
            addr = base + o
            cols = colours.get(addr)
            v = mem[addr]
            buf[o * 8:o * 8 + 8] = bytes(cols) if cols else bytes(0xff if v >> b & 1 else 0 for b in range(8))
        return bytes(buf)
    with open(os.path.join(a.out, name + ".GFX"), "wb") as f:
        for base in (0x4000, 0x8000, 0xc000):
            f.write(page(base))
    gfb = os.path.join(a.out, name + ".GFB")
    if any(addr < 0x4000 for addr in colours):
        with open(gfb, "wb") as f:
            f.write(page(0))
    with open(os.path.join(a.out, name + ".CFG"), "w") as f:
        # Uncoloured bytes are 0 and 0xFF, drawn in the attribute's paper and ink, even for the
        # games the player otherwise draws plainly by name.
        f.write("UpColorsMixed=8\r\nPaper00InkFF=1\r\n")
    print(f"coloured {len(colours)} bytes -> {a.out}/{name}.GFX")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("trace")
    t.add_argument("snapshot")
    t.add_argument("work")
    t.add_argument("--frames", type=int, default=15000)
    t.add_argument("--seed", type=int, default=1)
    t.add_argument("--roms", default=os.path.join(ROOT, "roms"))
    s = sub.add_parser("sheet")
    s.add_argument("work")
    i = sub.add_parser("ai")
    i.add_argument("work")
    i.add_argument("--title", required=True)
    e = sub.add_parser("emit")
    e.add_argument("work")
    e.add_argument("snapshot")
    e.add_argument("out")
    e.add_argument("--default", action="store_true", help="ignore plan.json")
    e.add_argument("--fill", action="store_true",
                   help="also colour bytes inside a sprite that were never seen drawn")
    a = ap.parse_args()
    {"trace": cmd_trace, "sheet": cmd_sheet, "ai": cmd_ai, "emit": cmd_emit}[a.cmd](a)


if __name__ == "__main__":
    main()
