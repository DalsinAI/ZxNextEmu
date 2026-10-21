# Spec256

Spec256 was a Spanish PC emulator from the late 1990s that played ordinary
48K Spectrum games in 256 colours with no colour clash. Each game came as
its normal snapshot plus graphics files that the Spec256 team (later
EmuZWin users) coloured by hand. ZxNextEmu runs those games as the machine
`spec256`.

## How it works

Think of a Z80 with a 72-bit data bus. Every byte of memory gets eight extra
bit-planes. The real CPU runs the game as usual, and eight shadow CPUs run
the same instructions beside it, each on one plane:

- opcodes come from real memory, so all nine CPUs run the same program;
- data (operands, loads, stores, the stack) comes from each CPU's own plane;
- the real CPU alone drives the address bus: every memory access a shadow
  makes goes to the address the real CPU used for the same access;
- before each instruction the shadows take PC, SP and the flags except carry
  from the real CPU, so they branch with it. Carry stays per plane, so
  multi-byte shifts of graphics work in every plane;
- the real CPU alone does I/O and timing. A shadow reading a port sees what
  the real CPU read; shadow writes to ports go nowhere.

A shadow's own registers can therefore hold colour (a sprite byte in `A`, a
row of pixels in `HL`) without changing what the game does.

The picture is built at the end of each frame. The colour of a paper-area
pixel is the eight plane bits at that pixel, an index into a 256-colour
palette. Attributes still matter in a few ways, which the `.CFG` controls:

| Setting | Default | Effect |
| --- | --- | --- |
| `Paper00InkFF` | 1 without a background, else 0 | colour 0 shows the attribute's paper, 0xFF its ink |
| `HideSameInkPaper` | 1 | where ink equals paper (hidden graphics), show that colour |
| `UpColorsMixed` | 64 | colours above 255 - n are averaged with the ink or paper the original pixel had |
| `DownColorsMixed` | 0 | the same for colours below n |
| `BkOverFF` | 0 | with a background, colour 0xFF is transparent as well as 0 |
| `NN: addr=VVh` | | show background `NN` while the byte at `addr` is `VV` |
| `zxpAlignRegs` | `1PSs` | which registers the shadows take from the real CPU (ZX-Poly's letters) |

`GFXLeveledXOR/OR/AND` and the other EmuZWin keys are read but not used; no
game in the collection sets them.

The Spec256 palette is 32 ramps of 8 shades; 136 to 255 are ramps of the
Spectrum's own colours. ZxNextEmu's picture is RGB333 (the Next's 512
colours), so the palette is rounded to the nearest of those.

### Files

| File | Holds |
| --- | --- |
| `NAME.SNA` | the game, 48K or 128K (the size picks the machine) |
| `NAME.GFX` | planes for RAM 5, RAM 2 and the page at C000, 3 x 128K |
| `NAME.GF0` to `.GF7` | planes for one RAM page each, overriding `.GFX` |
| `NAME.GFA`, `.GFB` | planes for ROM 0 and ROM 1 of a 128K; `.GFB` (or `ROM0.GFX`) is the 48K BASIC ROM |
| `NAME.B00` to `.B99` | 320x200 backgrounds, one palette index per pixel, shown through colour 0 |
| `NAME.CFG` | the settings above |
| `NAME.PAL` | a palette replacing the standard one (768 bytes) |

In the graphics files each byte of memory is eight bytes, and byte `i` of
those eight is the colour of pixel bit `i` of the original byte. Memory that
no file covers gets all eight planes equal to the original byte (colour
0xFF where a pixel is set), so code and data run the same on every plane.

`.EZX` is EmuZWin's own packed format and is not read yet.

### Using it

From the command line, give `zxrun` a folder:

    zxrun -g path/to/Jetpac -f 500 -t 15 -s jetpac.ppm

Through `zxm.h` (and so `zxcore.wasm` and ZX Cradle): create machine
`spec256`, insert every companion file into the `gfx` slot, then insert the
`.sna` into `snapshot`. The machine becomes a 48K or a 128K to suit the
snapshot. Ejecting `gfx` forgets the companion files. State save and load
include the planes and the shadow CPUs.

### Compared with ZX-Poly

ZX-Poly (github.com/raydac/zxpoly, GPL-3.0) is the open reference for
Spec256 and the source of the palette and of the file and colour rules here.
Its shadow CPUs form their own addresses unless a per-game setting says
otherwise, and it keeps a table of register settings by game. Sharing the
address bus needs no table. Knight Lore and Bubbler, which ZX-Poly lists as
wrong or patchy, match their published screenshots here, and Three Weeks in
Paradise, whose text is garbled with ZX-Poly's listed settings, comes out
clean. Only the `Paper00InkFF` hint for four games is kept from its table.

### The games

[mvvproject/Spec256-Games](https://github.com/mvvproject/Spec256-Games) has
36 folders; the 29 that include a snapshot all start and play here (the
other 7 ship only as EmuZWin .ezx files, not read yet). The
repository has no licence, and the snapshots are mostly commercial games
(its own readme says Ultimate and Codemasters refused distribution), so none
are bundled. Point the emulator at your own copy.

The cost is about nine times the CPU work: 3 to 4 ms a frame in
`zxcore.wasm` under Node, against 20 ms of real time.

## A converter for any game (plan)

Can the emulator colour a game that nobody made Spec256 files for? Partly,
and the shadow-CPU design is what makes it tractable: all a converter has to
produce is the planes for each graphics byte. Code and data keep identical
planes and run unchanged. The hard part is telling graphics from everything
else, and choosing colours good enough to beat the original attributes.

### 1. Find the graphics (automatic)

Run the game in an instrumented core and follow data, not guesses:

- mark every byte the CPU executes (code);
- give every memory byte a source tag and carry tags through loads, stores
  and ALU operations (taint tracking; the core already reports every bus
  access, so this is a bus wrapper like Spec256's own);
- when a value is written to the screen bitmap, record which source bytes it
  came from, and the attribute in force at that cell;
- a byte is graphics if it reaches the bitmap and never reaches an address,
  a branch, a port or a loop counter. Masks (bytes ANDed into the screen) are
  kept monochrome so masking still works.

Playing through the game (or a recorded input file) covers more of it. The
result is a map of graphics bytes, grouped into sprites and tiles by how the
draw loops walk them, with the colours the game gave them.

### 2. Colour them (automatic first pass)

- Each graphics pixel takes the Spectrum colour it was drawn in, mapped to
  that colour's ramp in the Spec256 palette. That alone removes colour clash
  and keeps the game's own look.
- Then shade along the ramp: lighter towards the top-left, darker at the
  edges, a dark outline where a sprite meets paper. Pre-shifted copies of a
  sprite (found by matching shifted bit patterns) get the same colours.
- Output `.GFX` and `.CFG`, playable here and in ZX-Poly and EmuZWin.

### 3. Paint the running game (by hand, or with an image model)

Because every screen pixel is traced back to the byte it came from, ZX Cradle
can let you click a pixel in the running game and recolour the sprite it
belongs to, live. The same map exports the graphics as a PNG sprite sheet for
an artist or an image model to colour, and imports it back into planes.

### What will not convert well

- Graphics built by code rather than copied from data (procedural fills,
  generated fonts): no source bytes to colour, though the drawing code's
  constants can sometimes be.
- XOR-drawn sprites: colours mix where sprites overlap. EmuZWin's
  `GFXLeveledXOR` mode was the answer; it needs an ALU hook in the shadow
  CPUs.
- Games that compare or checksum their own graphics, or use them as data.
- Screens drawn by the ROM's `PRINT` (the ROM font can be coloured, but
  every game sharing it gets the same colours).

### Order of work

1. Tracer and classifier in the core (a debug bus wrapper), with a report of
   graphics regions for one game. Check it against the hand-made `.GFX` files
   of the collection: their non-identical planes are exactly the graphics.
2. First-pass colouring, written as `.GFX`/`.CFG`; compare with the
   hand-coloured versions.
3. Sprite sheet export and import.
4. Live painting in ZX Cradle.

Step 1 is the real test of feasibility, and the existing collection gives it
an answer key.

### The Next

The Next shows 256 colours from 512 in Layer 2 at 256x192, the Spec256
screen size, so converted graphics could in principle be shown on real Next
hardware through Layer 2. Running nine CPUs there is out of reach, so a
Next-native version would mean rewriting a game's drawing code to Layer 2,
one game at a time. That is a separate project.
