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
  title.json        what it is and what it runs on
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

## `title.json`

```json
{
  "format": 2,
  "id": "sanxion",
  "name": "Sanxion",
  "kind": "game",
  "year": 1986,
  "authors": ["Stavros Fasoulas"],
  "publisher": "Thalamus",
  "genres": ["shoot-em-up"],
  "tags": ["horizontal scroller", "AY music"],
  "summary": "Horizontal shooter with a top-down radar view.",
  "license": "freeware",
  "source": "https://...",
  "ids": { "zxdb": 4290 },
  "added": "2026-10-21",
  "runs": [
    { "platform": "zx-spectrum", "machines": ["zx128", "plus2", "plus2a", "plus3"],
      "files": ["files/sanxion128v3.tap"], "slot": "tape", "needs": ["ram:128", "ay"],
      "notes": "128K version with AY music." },
    { "platform": "zx-spectrum", "machines": ["zx48"],
      "files": ["files/sanxion48v3.tap"], "slot": "tape", "needs": [], "likes": ["kempston"] }
  ],
  "compat": { "zx128": { "status": "verified", "by": "cradle", "date": "2026-10-21" } },
  "controls": { "keyboard": "Q up, A down, O left, P right, Space fire", "joystick": "kempston" },
  "screens": [{ "file": "screens/1.png", "caption": "Title screen" }]
}
```

`kind` is one of game, demo, utility, application, music, system, education;
`license` one of freeware, public-domain, shareware, commercial, unknown.
Only `format`, `name` and one entry in `runs` are required. Everything else
helps search and filtering. Unknown keys are kept when the file is
rewritten, so newer versions and other Cradles can add fields. Folders with
the older `title.toml` (format 1, a single `file` per run) are still read and
are written back as `title.json`.

One `runs` entry per way to run it, best first. `machines` lists machine ids
from the machine manifests ([machines.md](machines.md)), best first. `slot`
is where the files go: `tape`, `snapshot`, `disk.a`, `beta.a`, `sd0`,
`cart1`, `md.1`, `disk.1`. A multi-disk game lists every file in `files`.

## What runs where

`needs` lists what a run must have beyond the bare machine, from a fixed list
shared with recipes: `ram:128`, `ram:256`, `ram:1024`, `ram:2048`, `ay`,
`turbosound`, `gs`, `covox`, `beta`, `interface1`, `divmmc`, `plus3dos`,
`kempston`, `mouse`, `ulaplus`, `timex-hires`, `spec256`, `pi`, `wifi`,
`mb03`, `next:core>=X`. `likes` lists extras it uses when they are there; it
never hides a title.

A set-up (an instance or a recipe) offers its machine plus what it has built
in or added. A title **runs** on it when a run lists the machine and every
need is offered, **needs** a change when a run lists the machine but
something is missing (or it is known broken there), and is left out
otherwise. In instance mode the library shows only titles that run, best
known first, with the near misses in a "would run with a change" row.

`compat` records what is known per machine: `verified` (Cradle loaded it on a
hidden machine and saw it run), `works` or `broken` (a person said so on the
title page), `partial`, or `untested` (the default, not written).

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
names. Words of the form `machine:next`, `kind:demo`, `needs:beta`,
`compat:verified`, `year:1984`, `decade:1990s`, `genre:platform` and
`fit:needs` filter, the same in the search box, the shell and MCP. Filters narrow by machine ("runs on this"), kind, genre and decade.
Results sort by name, year or date added.
