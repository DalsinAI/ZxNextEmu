# spec256bot

Colours an ordinary 48K Spectrum game for Spec256 (see
[docs/spec256.md](../../docs/spec256.md)): it finds the game's graphics by
watching it play, has an AI model choose colours for each sprite and design a
background, and writes the `.GFX`, `.CFG` and `.B00` files that `zxrun -g`,
ZX Cradle, ZX-Poly and EmuZWin load.

    make build/s256trace
    B=tools/spec256bot/bot.py
    python3 $B trace JETPAC.SNA work/          # play it for 5 minutes, traced (.z80 works too)
    python3 $B sheet work/                     # sprites -> work/sheet.png
    python3 $B ai work/ --title "Jetpac"       # AI model -> work/plan.json
    python3 $B emit work/ JETPAC.SNA out/      # out/JETPAC.SNA .GFX .CFG .B00
    build/zxrun -g out -f 3000 -s shot.ppm

`ai` needs the `anthropic` package and an API key (`ANTHROPIC_API_KEY`).
Without it, skip that step: `emit` then colours each sprite in the ramp of
the colour the game drew it in. `plan.json` is plain JSON, so it can also be
written or touched up by hand.

`emit` checks that it is given the snapshot `trace` played, and the same
ROMs (`--roms`, default `roms/`), because the traced memory, ROM included,
becomes the uncoloured part of the graphics planes.

## How it works

**Trace.** `s256trace` runs the game on the Spec256 machine with the
shadow CPUs used as a tracer (`s256_trace` in `src/spec256/spec256.c`):
plane 0 and 1 of every byte hold its own address, so wherever the game
copies a byte, through registers, the stack or a buffer, the address goes
with it. Each write to the screen bitmap reports where the value came from.
A byte counts as graphics if it reaches the screen unchanged (loaded, ORed
or XORed onto blank screen). The tracer also records the attribute of the
cell at the end of the frame, which byte was drawn on the row below or above
(the sprite's width and whether it is stored upside down), and which byte
was drawn to its left (where its rows start, or, for sprites stored a
column at a time, how tall a column is). Keys and the Kempston joystick are
pressed at random so the game gets played: mostly the usual game keys, one
press in four any key, so a Y/N question gets answered.

**Group.** Nearby drawn bytes with the same width make one sprite.
`sheet.png` shows them all in the colour the game used.

**Plan.** The AI model gets the sheet and a list of the
sprites (size, how often drawn, in which colours) and answers, per sprite,
what it is, colour ramps by pixel row (a helmet over a suit over flames) and
a shading style: `bevel` (lit top left, dark bottom right), `gradient`
(light to dark down the band) or `flat`. The ramps are the Spec256
palette's: 17 rich ones (fire, skin, slate...), the Spectrum's own colours,
and `ink`, exactly the colour the game gives that spot (colour 0xFF), for
graphics the game draws in different colours, like the font. Under a
background 0xFF can't be the game's ink, so `ink` graphics then take the
colour the game mostly drew them in, shaded.

**Background.** In the same answer the model designs a background from
three gameplay screenshots: a kind (starfield, nebula, sky, hills, cave,
bricks, water, gradient or none), one to three colour ramps, a brightness
and a seed. `background.py` paints it as `NAME.B00` (320x200, dithered,
kept dark so the sprites stay readable), shown wherever the game leaves
paper. The model chooses none for games that colour the paper on purpose.
Without a plan, games played on black paper get a starfield.

**Emit.** Every set pixel of a coloured byte gets its colour, unset pixels
stay 0 (paper). Bytes the tracer never saw drawn keep identical planes, so
code, masks and data behave exactly as before.

## How well it works

On Jetpac, checked against the hand-coloured Spec256 version: of the 937
bytes the tracer marks as graphics, 913 (97%) are ones the artists
coloured too. It finds about half of what they coloured: Jetman, the font,
the platforms, explosions and the rocket, but not the aliens, which the game
shifts into place at run time (the tracer only follows bytes that arrive
unchanged). Longer play does not raise that.

Not yet: 128K games, sprites shifted or rotated by code, XOR overlap colours,
backgrounds that change with the level (Spec256 can switch them by a memory
value, `NN: addr=VVh` in the `.CFG`). Under a background, paper colours the
game sets (an inverse-video score box) give way to the background.
