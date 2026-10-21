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
- **Beta 128 / TR-DOS**: `make beta` (or `make beta CLONE_ROMS=dir`) builds
  disks with python3, then on the Pentagon, Scorpion and ATM Turbo loads,
  saves, catalogues and formats through the emulated WD1793 and checks what
  ended up in the images. Needs the clones' ROMs (`roms/README.md`); a
  machine without them is skipped. `make state` and `make wasmtest` take
  `CLONE_ROMS` too.
