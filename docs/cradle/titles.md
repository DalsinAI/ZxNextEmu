# The software library

The media changer holds what is in a machine's slots. The **software
library** holds everything you own to run: games, demos, utilities,
operating systems. Each one is a **title**: the software's files, its
screenshots, and a small file that says what it is and what it runs on.

The format is not tied to the Spectrum. Machine ids are the only
platform-specific part, so Amiga Cradle can use the same layout with its own
machine ids (`a500`, `a1200`) and its own media kinds (`adf`, `hdf`, `lha`).

## A title's folder

```
library/titles/sanxion/
  title.toml        what it is and what it runs on
  about.md          optional: longer notes, controls, history, credits
  files/            the software, exactly as downloaded
    sanxion48v3.tap
    sanxion128v3.tap
  screens/          PNG screenshots; the first is the cover
    1.png
    2.png
```

The folder name is the title's id: lower case, digits and dashes. Titles live
in `~/.zxcradle/library/titles/` under Node.js and in the browser's own
storage (IndexedDB) in the browser. **Export** writes a title as a `.zip` of
exactly this folder; **Import** reads such a `.zip`, a folder, or loose files.

## `title.toml`

```toml
format    = 1
name      = "Sanxion"
kind      = "game"            # game, demo, utility, application, music, system, education
year      = 1986
authors   = ["Stavros Fasoulas"]
publisher = "Thalamus"
genres    = ["shoot-em-up"]
tags      = ["horizontal scroller", "AY music"]
summary   = "Horizontal shooter with a top-down radar view."
license   = "freeware"        # freeware, public-domain, shareware, commercial, unknown
source    = "https://..."     # where the files came from
added     = 2026-10-21

# One [[runs]] table per way to run it, best first.
[[runs]]
platform = "zx-spectrum"
machines = ["zx128", "plus2", "plus2a", "plus3"]
file     = "files/sanxion128v3.tap"
slot     = "tape"
notes    = "128K version with AY music."

[[runs]]
platform = "zx-spectrum"
machines = ["zx48"]
file     = "files/sanxion48v3.tap"
slot     = "tape"

[controls]
keyboard = "Q up, A down, O left, P right, Space fire"
joystick = "kempston"         # kempston, sinclair, cursor, none

[[screens]]
file    = "screens/1.png"
caption = "Title screen"
```

Only `format`, `name` and one `[[runs]]` entry are required. Everything else
helps search and filtering. Unknown keys are kept when the file is
rewritten, so newer versions and other Cradles can add fields.

`machines` lists machine ids from the machine manifests
([machines.md](machines.md)). `slot` is where `file` goes: `tape`,
`snapshot`, `disk.a`, `sd0`, `cart`. A title that needs several files (a
multi-disk game, a tape plus a disk) lists them in `files`:

```toml
[[runs]]
machines = ["plus3"]
files    = ["files/side-a.dsk", "files/side-b.dsk"]
slot     = "disk.a"
```

## Filling it in

Cradle fills in what it can when a title is added, and the user corrects the
rest:

- **What it runs on**, from the files: a `.z80` header names its machine; a
  `.sna` of 49,179 bytes is a 48K snapshot and anything longer a 128K one; a
  `.tap` or `.tzx` is offered to the 48K and the 128K family; `.nex` is the
  Next; `.dsk` the +3; `.trd` and `.scl` the Pentagon and Scorpion.
- **The name**, from the file name, tidied (`sanxion48v3.tap` becomes
  "Sanxion48v3" until edited).
- **Screenshots**, by running the title on a hidden machine of its own, as
  fast as the core will go, and taking a picture a few seconds after it
  loads. The user can also take one from a running machine at any time.
- **The rest** (year, authors, genre, summary) from `about.md`, a
  `readme.txt` next to the files, or by hand.

## Searching

Search matches the name, authors, publisher, genres, tags, summary and file
names. Filters narrow by machine ("runs on this"), kind, genre and decade.
Results sort by name, year or date added.
