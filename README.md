# ZxNextEmu

A ZX Spectrum Next (TBBlue) emulator that is timed to the T-state, with the
classic Spectrums along for the ride: 48K, 128K, +2, +2A, +3 and Pentagon 128.

Written in portable C99 and built with GCC 16.2.0-open3
([DalsinAI/openamigagcc](https://github.com/DalsinAI/openamigagcc)) for
AmigaOS, and with any GCC for Linux, Windows and macOS.

Work in progress: see the status table below.

## Running

```sh
make                       # builds build/zxrun (headless) and the test tools
build/zxrun -m 128 -f 500 -s shot.ppm game.tap
```

`zxrun` options: `-m` model (`48`, `128`, `plus2`, `plus2a`, `plus3`,
`pentagon`), `-f` frames to run, `-t` text to type (after `-d` frames),
`-s` screenshot (PPM), `-w` audio (WAV), and a `.sna`, `.z80`, `.tap` or
`.tzx` file to load. Tapes load instantly through the ROM when they use the
standard loader, and play in real time for custom loaders.

## How the timing works

The Z80 core never counts T-states itself. Every machine cycle (opcode
fetch, memory read and write, I/O, and the CPU's internal cycles) goes to the
machine's bus in the order the real CPU performs it, and the bus adds the
cycle's length plus any contention or wait states. Contention, the ULA's
video fetches, the floating bus, beeper and AY output are all computed at the
exact T-state they happen, so multicolour effects, border stripes and
timing-sensitive loaders behave as on the hardware.

## Status

| Part | State |
| --- | --- |
| Z80 core | done: all documented and undocumented behaviour (X/Y flags, WZ, Q, block-instruction flags) |
| Z80N extensions | done, to be checked against the FPGA's T80N |
| 48K / 128K / +2 / +2A / +3 / Pentagon | boot; contention, ULA video, AY, beeper, paging |
| Tape | TAP and TZX (all common blocks), instant ROM load + real-time pulses |
| Snapshots | SNA (48K/128K), Z80 (v1-v3) |
| +3 disk drive | not yet |
| Spectrum Next | not yet: timing is being taken from the official FPGA sources |
| Front ends | headless `zxrun`; desktop (SDL2) and Amiga next |

## Tests

| Test | Result |
| --- | --- |
| ZEXDOC, ZEXALL | all pass (`make test`) |
| SingleStepTests/z80, every opcode: registers, memory, exact T-states | all pass (`make sst`) |
| RAXOFT z80test 1.2a (z80full, z80doc, z80flags, z80memptr) on the emulated 48K | all pass |

The test programs are downloaded into `tests-data/`, see
[tests/README.md](tests/README.md).

## Licence

GPL-3.0-or-later (see `LICENSE`). The ROMs in `roms/` are not covered by the
GPL; see [roms/README.md](roms/README.md).
