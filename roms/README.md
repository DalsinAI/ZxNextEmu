# ROMs

The Sinclair / Amstrad ROMs here are copyright Amstrad, who allow them to be
distributed but keep the copyright. You may not sell the ROMs or embed them in
hardware, though a product containing them may be sold as long as the charge
is for the product and not the ROMs. They are taken unchanged from the Fuse
emulator's `roms/` directory.

| File | Machine |
| --- | --- |
| `48.rom` | 16K/48K |
| `128-0.rom`, `128-1.rom` | 128K (also used by the Pentagon 128) |
| `plus2-0.rom`, `plus2-1.rom` | +2 (grey) |
| `plus3-0.rom` ... `plus3-3.rom` | +2A / +3 |

`samcoupe.rom` is the SAM Coupé's ROM 3.0 (32K: ROM 0 then ROM 1),
copyright Miles Gordon Technology. Dr Andy Wright, its author, gave
permission for the SAM ROMs to be distributed; this copy comes from
SimCoupé's resources, which ship it on that basis.

| File | Machine |
| --- | --- |
| `samcoupe.rom` | SAM Coupé (ROM 3.0) |

The Spectrum Next's NextZXOS and TBBlue firmware are not stored here; they
come from the official distribution SD image (see the top-level README).

## MSX: C-BIOS

The MSX machines boot [C-BIOS](https://cbios.sourceforge.net/) 0.29a, a free
MSX BIOS, by default. Its licence (BSD, two clauses) is in
[CBIOS-LICENSE.txt](CBIOS-LICENSE.txt). C-BIOS has no BASIC and no disk
support of its own: it starts cartridges.

| File | Machine |
| --- | --- |
| `cbios_main_msx1.rom`, `cbios_main_msx1_eu.rom`, `cbios_logo_msx1.rom` | `msx1`, `msx1eu` |
| `cbios_main_msx2.rom`, `cbios_main_msx2_eu.rom`, `cbios_sub.rom`, `cbios_logo_msx2.rom` | `msx2`, `msx2eu` |

A real BIOS (with MSX-BASIC) replaces these with your own dumps in the same
places: the 32K main ROM, the 16K MSX2 sub ROM, and no logo ROM. The disk
ROM, `disk.rom`, is also yours to supply: a 16K Philips-style (WD2793) disk
ROM for the floppy drives, or a Sunrise IDE flash ROM, such as Nextor's
`Nextor-*.SunriseIDE.ROM`, for the hard disks.

## Russian clones (not stored here)

The Beta 128 machines need ROMs that are not in this repository: TR-DOS is
Technology Research's and the Scorpion and ATM ROMs their makers'; none of
them has a licence that allows redistribution (Fuse does not ship them
either). Put these files in this folder (or a folder given with
`make CLONE_ROMS=dir`); a machine whose files are missing is skipped by the
tests and refused by the frontends.

| File | Machine | md5 |
| --- | --- | --- |
| `trdos.rom` | Pentagon 128 (TR-DOS 5.03 or 5.04T; optional, no disk without it) | `0c42e3b9ab8dd91ea096f1d0c07c55e5` (5.03) |
| `256s-0.rom` ... `256s-3.rom` | Scorpion ZS-256: 128 BASIC, 48 BASIC, service monitor, TR-DOS | `b9fda5b6…`, `fbca50ef…`, `7cb480da…`, `40dccd1b…` (ROM 2.95) |
| `atm2-0.rom` ... `atm2-3.rom` | ATM Turbo 2+: 48 BASIC / SOS, TR-DOS, 128 BASIC, BIOS | `25e7ad37…`, `26b2b42f…`, `a249565f…`, `889a717a…` (v7.10) |

Emulators usually carry the Scorpion and ATM ROMs as one 64K file; split it
into 16K pages in order:

    split -b 16384 -d -a 1 scorpion_2.95.rom 256s- && for i in 0 1 2 3; do mv 256s-$i 256s-$i.rom; done
    split -b 16384 -d -a 1 atm2.rom atm2- && for i in 0 1 2 3; do mv atm2-$i atm2-$i.rom; done
