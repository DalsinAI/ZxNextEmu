# ZxNextEmu

A ZX Spectrum Next (TBBlue) emulator that is timed to the T-state, with the
classic Spectrums along for the ride: 48K, 128K, +2, +2A, +3 and Pentagon 128,
and the 6502-based Oric-1 and Oric Atmos.

Written in portable C99 and built with GCC 16.2.0-open3
([DalsinAI/openamigagcc](https://github.com/DalsinAI/openamigagcc)) for
AmigaOS, and with any GCC for Linux, Windows and macOS.

Work in progress: see the status table below.

## Running

```sh
make                       # builds build/zxrun (headless) and the test tools
build/zxrun -m 128 -f 500 -s shot.ppm game.tap
```

`zxrun` options: `-m` model (`next`, `48`, `128`, `plus2`, `plus2a`, `plus3`,
`pentagon`), `-c` SD card for the Next (a folder or an image file), `-p`
a folder for the Next's Raspberry Pi, `-f` frames to run, `-t` text to type (after `-d` frames),
`-s` screenshot (PPM), `-w` audio (WAV), and a `.sna`, `.z80`, `.tap` or
`.tzx` file to load. Tapes load instantly through the ROM when they use the
standard loader, and play in real time for custom loaders.

### The Spectrum Next and its SD card

The Next boots its firmware and NextZXOS from an SD card, using the official
software distribution, [tbblue](https://gitlab.com/thesmog358/tbblue). The
card is just a folder:

```sh
git clone https://gitlab.com/thesmog358/tbblue next-sd
build/zxrun -m next -c next-sd -f 1500 -s shot.ppm
```

The emulator shows the folder to the Next as a FAT32 card made on the fly,
with long file names. No image is built: the Next reads the files in place.
What the Next writes (new files, changes, deletions, renames, directories)
is written back into the folder about a second after it stops writing, and
again when the emulator exits. Files and folders whose names start with a
dot (such as `.git`) are hidden from the Next and left alone. Each machine
gets its own folder, the way Amiga Cradle gives each machine its own
storage. `-c` also accepts a raw image file, and `tools/mksd.sh` builds one
from a folder.

### Wifi

The Next's ESP8266 module is emulated with Espressif's AT command set, and
its connections go out through the computer running the emulator, so
NextZXOS's network tools work as they do on a real Next with wifi set up.
In NextBASIC, `.http get -h example.com -u /file.txt -f file.txt` saves the
file to the SD card folder.

### Raspberry Pi

Give the Next a Raspberry Pi with `-p folder`: the folder holds the Pi's
files (`/ram`, `/NextPi` and so on). The emulator answers as NextPi's
supervisor shell does, so `.pisend -q` finds the Pi and `.piput`, `.piget`
and `.pisend` move files between the SD card and the Pi.

## The core in WebAssembly (for ZX Cradle)

`src/core/zxm.h` is the one interface every machine offers: look a machine
up by id (`zx48`, `zx128`, `plus2`, `plus2a`, `plus3`, `pentagon`, `next`,
`oric1`, `atmos`),
create it from ROM images, run it a frame at a time, read the picture and
sound out of it, send it input, insert media, and save or load its complete
state. The core does no file I/O: ROMs, tapes and snapshots come in as
bytes, SD cards as block callbacks, serial devices as byte callbacks.

`make wasm` builds that core as `build/zxcore.wasm` with clang and
wasi-libc's headers and malloc (about 150 KB, and it imports nothing).
`make wasmtest` runs every machine natively and under Node with typed input
and a state save and load part way, and checks the two give exactly the same
pictures and sound.

### Oric-1 and Oric Atmos

The Orics need their BASIC ROM, which is not in this repository because
its redistribution rights are unclear: `basic10.rom` (Oric-1, BASIC 1.0,
CRC32 `f18710b4`) and `basic11b.rom` (Atmos, BASIC 1.1, CRC32 `c3a92bef`),
16K each. Tapes are `.tap` files. With those two ROMs, CLOAD reads a tape
almost at once by handing bytes to the ROM's own routines; any other
loader gets the real 2400/1200 Hz signal on the VIA's CB1.

## How the timing works

The Z80 core never counts T-states itself. Every machine cycle (opcode
fetch, memory read and write, I/O, and the CPU's internal cycles) goes to the
machine's bus in the order the real CPU performs it, and the bus adds the
cycle's length plus any contention or wait states. Contention, the ULA's
video fetches, the floating bus, beeper and AY output are all computed at the
exact T-state they happen, so multicolour effects, border stripes and
timing-sensitive loaders behave as on the hardware.

The 6502 core works the same way: every clock cycle is one bus access,
dummy reads included, and the Oric's bus steps the VIA, the AY, the tape
and the video ULA once per cycle. The ULA draws one 6-pixel cell each
cycle, reading memory before the CPU's access, so serial attributes and
mid-frame changes land where they do on the hardware.

## Status

| Part | State |
| --- | --- |
| Z80 core | done: all documented and undocumented behaviour (X/Y flags, WZ, Q, block-instruction flags) |
| Z80N extensions | done, to be checked against the FPGA's T80N |
| 48K / 128K / +2 / +2A / +3 / Pentagon | boot; contention, ULA video, AY, beeper, paging |
| Tape | TAP and TZX (all common blocks), instant ROM load + real-time pulses |
| Snapshots | SNA (48K/128K), Z80 (v1-v3) |
| +3 disk drive | not yet |
| 6502 core | done: every opcode, documented and not, cycle by cycle on the bus; NMOS decimal mode; interrupt polling on the last cycle but one |
| Oric-1 / Atmos | ULA (text, HIRES, serial attributes, double height, blink, 50/60 Hz), 6522 VIA, AY through the VIA, 8x8 keyboard, `.tap` tape (fast ROM load + real signal). Checked with a test ROM; the real BASIC ROMs still to be tried. Microdisc not yet |
| Spectrum Next | boots NextZXOS to its menu. Built from the official FPGA sources: memory, ports, nextregs, ULA / LoRes / Timex / tilemap / layer 2 / sprites and the mixer, copper, DMA, CTC, IM2, divMMC automap, AY x3, DACs, both UARTs, Kempston mouse, ESP8266 wifi, Raspberry Pi (NextPi). Still to come: multiface, Pi audio |
| SD card | a host folder seen as a live FAT32 card, or an image file |
| Wifi | ESP8266 with the AT firmware on UART 0, connecting through the host: NextZXOS's `.http` fetches files |
| Raspberry Pi | NextPi's supervisor shell on UART 1, files in a host folder: `.pisend`, `.piver`, `.piput`, `.piget` work; the nextpi-* players are not there yet |
| Machine state | complete save and load for every machine (`zxm_state_save`) |
| WebAssembly core | `zxcore.wasm`, the zxm interface, identical output to native |
| Front ends | headless `zxrun` and desktop (SDL2) for testing; ZX Cradle's JavaScript runtime next |

## Tests

| Test | Result |
| --- | --- |
| ZEXDOC, ZEXALL | all pass (`make test`) |
| SingleStepTests/z80, every opcode: registers, memory, exact T-states | all pass (`make sst`) |
| SingleStepTests/65x02 (6502), every opcode: registers, memory, every bus cycle | all pass (`make sst6502`) |
| Oric test ROM (`tests/oric/testrom.s`): VIA T1 period, ULA attributes and HIRES, keyboard, AY, a tape read back through CB1 | all pass (`make oric`, needs xa65) |
| Folder-backed SD card: fsck.fat, then write back changes made with mtools | all pass (`make vfat`) |
| ESP8266 over UART 0: AT commands, baud change, TCP to a local echo server | all pass (`make esp`) |
| NextZXOS's Pi tools against the emulated NextPi (needs the tbblue software) | all pass (`make nextpi TBBLUE=...`) |
| Every machine: save state, run on, load, run again; same pictures and sound | all pass (`make state`) |
| zxcore.wasm under Node gives the same output as the native core | all pass (`make wasmtest`) |
| RAXOFT z80test 1.2a (z80full, z80doc, z80flags, z80memptr) on the emulated 48K | all pass |

The test programs are downloaded into `tests-data/`, see
[tests/README.md](tests/README.md).

## ZX Cradle

This emulator is the core of ZX Cradle, a planned machine manager for every
Z80 machine: named instances, media changer, snapshots, field reports, the
`zxc` shell and an MCP server. The design and the style guide are in
[docs/cradle/](docs/cradle/README.md).

## Licence

GPL-3.0-or-later (see `LICENSE`). The ROMs in `roms/` are not covered by the
GPL; see [roms/README.md](roms/README.md).
