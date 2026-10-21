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
| `pentagon` | Pentagon 128 | done, with the Beta 128 disk interface (TR-DOS, .trd / .scl) when `trdos.rom` is supplied |
| `next` | ZX Spectrum Next (TBBlue) | timing from the official FPGA sources |
| `spec256` | Spec256, the 256-colour Spectrum | done in the core: a 48K or 128K (the snapshot decides) with eight shadow CPUs on bit-planes; see [../spec256.md](../spec256.md) |
| `scorpion` | Scorpion ZS-256 | done: 256K, port 1FFD paging, service ROM, Beta 128 disk |
| `atm2` | ATM Turbo 2+ | done: 1024K, xxF7 window paging, 7 MHz turbo, EGA / hi-res / 80-column text modes, 16-colour palette, Beta 128 disk |
| `ts2068`, `tc2048` | Timex Sinclair 2068, Timex Computer 2048 | ULA hi-res and hi-colour modes, cartridge slot |
| `zx80`, `zx81` | ZX80, ZX81 (Timex TS1000/TS1500) | the CPU generates the picture; needs the core's per-T-state bus |

Interfaces for the classic models (`zxm_attach`, which adds the
interface's slots and inputs to the instance):

| Id | Interface | Notes |
| --- | --- | --- |
| `divmmc` | divMMC | esxDOS ROM, 512K, slot `sd0` |
| `mb03` | MB03+ Ultimate | divMMC with `sd0` and `sd1`, TurboSound, SpecDrum/Covox/SounDrive, Kempston mouse, Timex screens; General Sound, SAA1099, SID, GigaScreen, MBIDE and the config registers to come |

## Wave 2: the big Z80 families

| Id | Machine | Notes |
| --- | --- | --- |
| `cpc464`, `cpc664`, `cpc6128` | Amstrad CPC | Gate Array, CRTC variants (types 0 to 4), uPD765 shared with +3 |
| `cpcplus` | CPC 6128 Plus, 464 Plus, GX4000 | ASIC sprites, cartridge slot |
| `msx1`, `msx2`, `msx2p` | MSX, MSX2, MSX2+ | `msx1`, `msx1eu`, `msx2`, `msx2eu` done in the core (C-BIOS by default, floppy, Sunrise IDE for Nextor and SymbOS); MSX2+ (V9958) and more variants as manifests to come |
| `sam` | SAM Coupé | done in the core (`src/sam`): ASIC modes 1 to 4, contention and interrupts after SimCoupé's measurements, SAA1099, two real-time VL1772 drives, tape; ROM 3.0 is distributable |
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
