# Machine catalogue

ZX Cradle aims at every Z80-family machine with enough documentation to be
emulated accurately. Each machine is a module in `libzxcore` plus a machine
manifest (see [manifests.md](manifests.md)). Machines that share hardware
share code: the 48K ULA serves the 16K, 48K, 128K and Timex machines with
different tables, the AY serves the 128K, CPC, MSX and Pentagon.

The order below is a proposal. It puts first the machines that reuse what
the core already has, then the large families, then the rest.

## Wave 1: Spectrum family (in progress)

| Id | Machine | Notes |
| --- | --- | --- |
| `zx48`, `zx16` | ZX Spectrum 48K and 16K, issue 2 and 3 boards | 48K done in the core; 16K is a 48K manifest with less RAM |
| `zx128`, `plus2` | Spectrum 128K, +2 | done in the core |
| `plus2a`, `plus3` | +2A, +2B, +3 | done except the +3 disk drive (uPD765) |
| `pentagon` | Pentagon 128 | done; Beta Disk to come |
| `next` | ZX Spectrum Next (TBBlue) | timing from the official FPGA sources |
| `scorpion` | Scorpion ZS-256 | Pentagon-like, extra paging |
| `ts2068`, `tc2048` | Timex Sinclair 2068, Timex Computer 2048 | ULA hi-res and hi-colour modes, cartridge slot |
| `zx80`, `zx81` | ZX80, ZX81 (Timex TS1000/TS1500) | the CPU generates the picture; needs the core's per-T-state bus |

## Wave 2: the big Z80 families

| Id | Machine | Notes |
| --- | --- | --- |
| `cpc464`, `cpc664`, `cpc6128` | Amstrad CPC | Gate Array, CRTC variants (types 0 to 4), uPD765 shared with +3 |
| `cpcplus` | CPC 6128 Plus, 464 Plus, GX4000 | ASIC sprites, cartridge slot |
| `msx1`, `msx2`, `msx2p` | MSX, MSX2, MSX2+ | `msx1`, `msx1eu`, `msx2`, `msx2eu` done in the core (C-BIOS by default, floppy, Sunrise IDE for Nextor and SymbOS); MSX2+ (V9958) and more variants as manifests to come |
| `sam` | SAM Coupé | Spectrum-compatible mode reuses the ULA work |
| `ace` | Jupiter Ace | Forth in ROM, small and well documented |

## Wave 3: games consoles and the rest

| Id | Machine | Notes |
| --- | --- | --- |
| `sms`, `gg`, `sg1000` | Sega Master System, Game Gear, SG-1000 | Z80 at 3.58 MHz, VDP derived from the TMS9918 |
| `coleco` | ColecoVision | TMS9918 + SN76489, shares the MSX video chip |
| `lynx48`, `lynx96` | Camputers Lynx | |
| `mz700`, `mz800` | Sharp MZ-700, MZ-800 | |
| `nascom` | Nascom 1 and 2 | |
| `trs80` | TRS-80 Model I, III | |
| `kc85` | Robotron KC 85/2 to 85/4 | |
| `cpm` | Generic CP/M 2.2 machine | no real hardware: a terminal, disks and a clock, for running CP/M software and tests |

## Beyond the Z80

The core has a cycle-exact 6502 too (`src/m6502`), so machines built
around it can join.

| Id | Machine | Notes |
| --- | --- | --- |
| `oric1`, `atmos` | Tangerine Oric-1, Oric Atmos | in the core: ULA, 6522 VIA, AY through the VIA, `.tap`; Microdisc to come. ROMs supplied by the user |

Machines where the Z80 sits beside a different main CPU (the Sega Mega
Drive's sound Z80 next to its 68000, a Z80 SoftCard in an Apple II) are out of scope until the host can run more than one core per
instance.

## What a machine needs before it ships

1. A public timing reference: schematics, an FPGA core, or measurements
   from real hardware, recorded in the machine's manifest under `sources`.
2. Test programs that pass: the CPU exercisers for every machine, plus the
   machine's own timing tests where they exist (for the Spectrums,
   RAXOFT z80test and the community contention and floating-bus tests; for other machines, the
   equivalent suites their emulation communities use).
3. ROMs that can be redistributed, or a clear message telling the user
   which ROM files to supply and their checksums.
4. A boot check for verified restart.
