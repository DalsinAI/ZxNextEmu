# Recipe: SymbOS

[SymbOS](http://www.symbos.de/) is Prodatron's multitasking, windowed
operating system for Z80 computers: a desktop with a start menu, a file
manager, a text editor, a web browser and a few hundred applications, on
machines with 128 KB to a few MB of banked RAM. It began on the Amstrad CPC,
came to the MSX and the Enterprise, and version 4.0 (31 January 2025) added
the ZX Spectrum Next. It is a good showcase for ZX Cradle because one
application runs, unchanged, on several of the machines Cradle emulates.

This document plans two recipes (see [../recipes.md](../recipes.md)):

| Recipe | Machine | Needs from the core |
| --- | --- | --- |
| `symbos-next` | Spectrum Next, 2 MB | one addition (the RTC), the rest is there |
| `symbos-msx2` | MSX2, 1 MB mapper | the MSX2 machine, being built in its own thread |

An MSX1 recipe is not possible: SymbOS needs the V9938's bitmap modes, its
128 KB of VRAM and its command engine. Prodatron has said a TMS9918 version
would be two-colour with no blitter, and none exists. The recipe engine
greys `symbos-*` out for MSX1 instances with that reason.

The SymbOS CPC, Enterprise, PCW and NC100 versions can become recipes the
same way once those machines arrive ([../machines.md](../machines.md)).

## Getting SymbOS

- **Licence.** symbos.de says SymbOS and the applications on the site are
  freeware. We have not found a statement allowing third parties to
  redistribute it, so ZX Cradle **does not ship or mirror SymbOS**. The
  recipe fetches it on demand from symbos.de, with a pinned sha256, after
  showing the user the source and the licence line.
- **To do before the recipe ships:** read the licence text inside the 4.0
  packages, and write to Prodatron to ask (a) whether a mirror is fine,
  which would make browser-only fetching work, and (b) whether he would like
  ZX Cradle listed as a supported way to run SymbOS. Record the answer in the
  recipe's `licence` field.
- **Packages used.**
  - Next: the 4.0 *mass storage* package for the Next. The download page's
    instructions are to copy its contents to the root of the SD card and run
    `SYMBOS.NEX` from the NextZXOS browser.
  - MSX: the 4.0 *hard disk image* for MSX (a ready-to-boot Nextor/FAT16
    image, also mirrored on File-Hunter) for the first version of the recipe,
    and the MSX mass storage package later for a folder-backed card.
- **symbos.de is plain HTTP and sends no CORS headers.** Under the Node host
  this does not matter. In a browser-only setup, Cradle Setup opens the
  download page and the user drops the zip onto the page, which is accepted
  if its checksum matches (see [../recipes.md](../recipes.md#fetching-in-the-browser)).
- **Updates.** A new SymbOS release changes the checksum, so it is a new
  recipe version. Existing instances keep running on the version they were
  made with; `zxc media rebase` moves an instance's card onto the new one
  and keeps the user's own files, as it does for NextZXOS.

## `symbos-next`

### Card layout

One folder-backed FAT32 card in `sd0`, built from three read-only layers and
the instance's overlay:

```
layer 4  overlay            instance: media/sd/main/overlay/  (everything SymbOS and NextZXOS write)
layer 3  recipe files/sd    nextzxos/autoexec.bas  (runs SymbOS at boot, see below)
layer 2  fetch:symbos-4.0-next   SYMBOS.NEX, the SymbOS system files and apps, as unpacked
layer 1  library:sd/nextzxos-<version>   the official tbblue distribution (TBBLUE.FW, NextZXOS)
```

What the Next sees is one card with NextZXOS and SymbOS side by side at the
root, as the SymbOS instructions ask. Settings SymbOS saves (`SYMBOS.INI`,
the desktop layout, documents) land in the overlay, so the shared NextZXOS
and SymbOS layers never change and ten SymbOS instances can share them.

Booting: the Next boot ROM loads `TBBLUE.FW`, which starts NextZXOS, which
runs `nextzxos/autoexec.bas`. The recipe's autoexec is one line, `10 .nexload
/SYMBOS.NEX` (the recipe stores the tokenised file; the exact path is checked
against the 4.0 package when the checksum is pinned). The recipe's notes say how to
skip the autoexec at boot on NextZXOS (to check), so NextZXOS stays reachable.

The first boot runs until the SymbOS desktop is up (boot check: the start
button or "SymbOS" on screen, timed out after two emulated minutes) and saves
the `desktop` snapshot. Later starts restore that snapshot, so SymbOS opens in
under a second instead of going through the Next's boot.

### Instance settings

| Setting | Value | Why |
| --- | --- | --- |
| RAM | 2 MB (expanded Next) | SymbOS uses all of it for applications; 1 MB works but fills quickly |
| CPU speed | left to SymbOS | SymbOS sets 28 MHz itself through nextreg 0x07 |
| Mouse | Kempston, pointer lock | SymbOS reads the relative Kempston mouse; the page captures the host pointer while the screen has focus |
| Keyboard | Next keyboard, PC key map | the extended keys (nextregs 0xB0, 0xB1) carry the cursor keys, Delete, Edit and the rest SymbOS uses |
| Display | 640x256, scale 2, no border | SymbOS uses Layer 2's 640x256 16-colour mode |
| Wifi | optional, off by default | the ESP8266 is there if a SymbOS network driver for the Next uses it (to check in 4.0) |

### What the Next core already has

From the README status and the sources in `src/next/`:

- Layer 2 at 640x256x4 (nextreg 0x70), palettes, the mixer, sprites, the copper.
- 28 MHz with the FPGA's wait states, 2 MB SRAM and all the MMU paging.
- DMA (SymbOS's blits are Z80 code or DMA; both are there).
- The Kempston mouse with wheel and three buttons.
- The Next keyboard including the extended keys.
- AY x3 (Turbosound) and the DACs for SymbOS's sound daemon.
- The SD card over SPI on ports 0xE7/0xEB, with the host folder seen as an
  MBR + FAT32 card, the layout SymbOS's FAT driver expects.
- The ESP8266 on UART 0, if a network driver needs it.

### Changes needed (for the emulator thread; this document changes no code)

| # | Where | Change | Why |
| --- | --- | --- | --- |
| N1 | core, `src/next/next.c` | **A DS1307 real-time clock on the I2C bus** (ports 0x103B SCL, 0x113B SDA, device 0x68). Today nothing answers and the lines float high. | SymbOS shows the time on the task bar and stamps files; NextZXOS's `RTC.SYS` also needs it. Without it SymbOS's clock and file dates are wrong. |
| N2 | `zxm.h` | A way to give the machine the time: the runtime passes wall-clock seconds at create (and on restore), the RTC counts from there in emulated time, and the value is in the saved state. | Keeps the core deterministic (no host clock inside the core), so recordings still replay exactly. |
| N3 | core, measurement | Measure a frame of the SymbOS desktop at 28 MHz with Layer 2 640x256 on and windows moving, in `zxcore.wasm`. The 3 ms figure in [../design.md](../design.md#performance) is NextZXOS's menu. | SymbOS keeps the CPU at 28 MHz all the time and draws in the 640 mode; if it costs more than about 10 ms a frame, the `mix()` speed work moves up. |
| N4 | runtime (JS) | The folder-backed FAT32 card in JavaScript, as in [../storage.md](../storage.md), with **stacked read-only layers** (the C `vfat.c` takes one folder). | The recipe's card is NextZXOS + SymbOS + recipe files + overlay. |
| N5 | runtime (JS) | Snapshots that hold a folder-backed card record the card's generation (overlay change counter). Restoring a snapshot whose card changed since is refused and the instance cold-boots. | SymbOS caches the FAT and directories in RAM; restoring a desktop snapshot over a card the host has since changed would corrupt it. |
| N6 | runtime (JS) | Pointer lock for the mouse, released with a host key (Right Alt by default) and on blur; wheel to `dz`. | The Kempston mouse is relative. |
| N7 | runtime (JS) | A PC key map for the Next that sends the extended keys (cursor keys, Delete, Tab, Caps Lock, symbols typed as Symbol Shift combinations). | SymbOS is a keyboard-heavy desktop. |
| N8 | host (JS), optional | The WebSocket relay for the ESP8266 in the browser, on a port in the 7000 range, if SymbOS 4.0 has a Next network driver. | Browsers cannot open raw TCP. |

N1 and N2 are the only core changes. Without them the recipe still works,
with a stopped clock.

## `symbos-msx2`

The MSX2 machine is in draft PR #11: machine `msx2` (`msx2eu` for 50 Hz),
a 1 MB mapper, slots `cart1`, `cart2`, `disk0`, `disk1` (`.dsk`) and `ide0`,
`ide1` (hard disk images). Its ROMs, in order, are `cbios_main_msx2.rom`,
`cbios_sub.rom`, `cbios_logo_msx2.rom` and `disk.rom`. The recipe uses
`msx2eu` with Nextor's Sunrise IDE ROM as `disk.rom` and the SymbOS hard disk
image in `ide0`. The table is what SymbOS needs from the machine:

| Part | Needed | Notes |
| --- | --- | --- |
| VDP | V9938, 128 KB VRAM, the command engine (HMMV, HMMM, LMMM, LMMV and the rest) with its timing | SymbOS's MSX screen driver uses the 512x212 bitmap modes and leaves fills and copies to the command engine. A command engine that finishes instantly will mostly work, but the timed one is the target. |
| RAM | a memory mapper, 1 MB (256 KB minimum) | Below 256 KB SymbOS's setup installs a reduced system (no background picture, no screen saver). |
| Mass storage | a Nextor-supported interface with a FAT16 volume. Proposal: the Sunrise IDE interface (well documented, Nextor and SymbOS both have drivers for it) | Which SymbOS 4.0 MSX storage drivers exist decides this: check the 4.0 package before the MSX thread picks an interface. |
| DOS | Nextor (or MSX-DOS 2) in ROM | SymbOS's MSX setup needs DOS 2 features (subdirectories). Nextor is free to download; its ROM can be fetched like SymbOS (its redistribution terms are to check too). |
| Mouse | MSX mouse on joystick port 1 (read through PSG registers 14 and 15) | SymbOS drives the MSX mouse directly. |
| Clock | RP5C01 RTC | Standard on MSX2; the same wall-clock input as N2. |
| Sound | PSG (AY-3-8910); OPLL and SCC optional | |
| BIOS | C-BIOS main, sub-ROM and logo ROM, plus `disk.rom` | C-BIOS is redistributable. `disk.rom` is Nextor's Sunrise IDE ROM, fetched like SymbOS (or supplied by the user). If SymbOS does not boot on C-BIOS, the recipe asks for a real MSX2 BIOS by checksum (main 32 KB, sub 16 KB, no logo ROM). To check once the image boots. |

### Media

- **First version, image-backed.** The SymbOS 4.0 MSX hard disk image goes
  in `ide0` as an image-backed medium (`image =
  "fetch:symbos-4.0-msx-hd/<file>"`, `write = "overlay"`). This needs nothing
  new in storage and is exactly what real users boot.
- **Later, folder-backed.** A folder-backed **FAT16** volume (Nextor does not
  read FAT32), laid out from the mass storage package the same way as the
  Next card. The JavaScript volume builder (N4) gets a FAT16 option: cluster
  size chosen for the card size, at most 4 GB.

Boot: Nextor boots `MSXDOS2.SYS`/`COMMAND2.COM` from the volume and an
`AUTOEXEC.BAT` there starts SymbOS. First boot snapshots the desktop, as on
the Next.

### Changes needed

| # | Where | Change |
| --- | --- | --- |
| M1 | MSX thread | The MSX2 machine (PR #11): check that the V9938 command engine, MSX mouse and RP5C01 are there, and boot the SymbOS 4.0 hard disk image in `ide0` |
| M2 | MSX thread and runtime (JS) | Folder-backed FAT16 (host folders come up as FAT32 today, which Nextor cannot boot); planned in PR #11's follow-up |
| M3 | runtime (JS) | A PC key map for the MSX matrix, and pointer lock for the MSX mouse (shared with N6) |
| M4 | [../storage.md](../storage.md) | Add the MSX to the `ide` row and FAT16 as a folder-backed format |

## Order of work

1. Pin the 4.0 package checksums, read their licence text, write to
   Prodatron (no code).
2. The recipe engine in the host: `recipe.list`, `recipe.make`, fetch and
   checksum, layered media, first-boot snapshot. Phase 2 of
   [../design.md](../design.md#phases), after folder-backed SD.
3. `symbos-next`, usable with a stopped clock; then N1 and N2 for the clock.
4. `symbos-msx2` with the hard disk image in `ide0` once PR #11 boots
   Nextor from it; then the folder-backed FAT16 card.

## Sources

- SymbOS home and downloads: http://www.symbos.de/ and
  http://www.symbos.de/download.htm (freeware; Next install instructions)
- SymbOS 4.0 release, with the Next, NC100/150/200 and SymbOSVM added:
  https://www.msx.org/news/en/symbos-40-released
- The earlier Next build ("SymbOS ZNX") using Layer 2 at 640x256x16 and the
  Kempston mouse: http://www.symbos.de/changelg.htm
- MSX requirements (setup under 256 KB, DOS 2, no MSX1 version):
  Prodatron's posts in the msx.org SymbOS thread, https://www.msx.org/node/4993
- SymbOS on MSX: https://www.cpcwiki.eu/index.php/SymbOS,
  https://download.file-hunter.com/OS/SymbOS/

These pages could not be fetched from the build environment, so the facts
above come from search summaries of them. Items marked "to check" must be
confirmed against the 4.0 packages before the recipes ship.
