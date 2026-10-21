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

The Oric-1 and Oric Atmos BASIC ROMs are not stored here either: their
copyright holders are long gone and nobody has granted redistribution, so
supply your own `basic10.rom` (Oric-1, CRC32 `f18710b4`) and `basic11b.rom`
(Atmos, CRC32 `c3a92bef`).

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
