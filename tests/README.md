# Tests

Test programs are not kept in git; they are downloaded into `tests-data/`.

- **ZEXDOC / ZEXALL** (Frank Cringle's exerciser):
  `curl -o tests-data/zexdoc.com https://raw.githubusercontent.com/anotherlin/z80emu/master/testfiles/zexdoc.com`
  (and `zexall.com`), then `make test`.
- **SingleStepTests/z80**: `make sst` downloads each opcode's 1000 cases,
  checks registers, WZ, Q, memory and the exact T-state count, and deletes
  the file again. `KEEP=1` keeps the downloads.
- **RAXOFT z80test 1.2a**: unpack the release zip into `tests-data/z80test/`, then
  `build/zxrun -m 48 -n -f 150000 -t 'j""<newline>' -s out.ppm tests-data/z80test/z80test-1.2a/z80full.tap`
  and look at the screenshot: it ends with "Result: all tests passed."
- **esxDOS** (for `make esxdos`, and the MB03+ machines in `make state` and
  `make wasmtest`): the ROM and its `SYS/` and `BIN/` must be the same
  version. The ZX-Uno repository has a matching 0.8.9 set:
  ```sh
  git clone --filter=blob:none --no-checkout https://github.com/zxdos/zxuno.git /tmp/zxuno
  git -C /tmp/zxuno sparse-checkout set --no-cone 'SD/SYS/*' 'SD/BIN/*' SD/ESXMMC.BIN
  git -C /tmp/zxuno checkout
  mkdir -p tests-data/esxdos
  cp /tmp/zxuno/SD/ESXMMC.BIN tests-data/esxdos/esxmmc.bin
  cp -r /tmp/zxuno/SD/SYS /tmp/zxuno/SD/BIN tests-data/esxdos/
  ```
- **SAM Coupé**: `make sam` fetches SAMDOS 2 (`samdos2.sbt`, from SimCoupé's
  resources) into `tests-data/`, builds a boot disk with `tests/mkmgt.c`,
  boots it, saves a file from BASIC and reads it back out of the image.
- **Beta 128 / TR-DOS**: `make beta` (or `make beta CLONE_ROMS=dir`) builds
  disks with python3, then on the Pentagon, Scorpion and ATM Turbo loads,
  saves, catalogues and formats through the emulated WD1793 and checks what
  ended up in the images. Needs the clones' ROMs (`roms/README.md`); a
  machine without them is skipped. `make state` and `make wasmtest` take
  `CLONE_ROMS` too.
