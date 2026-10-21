# Tests

Test programs are not kept in git; they are downloaded into `tests-data/`.

- **ZEXDOC / ZEXALL** (Frank Cringle's exerciser):
  `curl -o tests-data/zexdoc.com https://raw.githubusercontent.com/anotherlin/z80emu/master/testfiles/zexdoc.com`
  (and `zexall.com`), then `make test`.
- **SingleStepTests/z80**: `make sst` downloads each opcode's 1000 cases,
  checks registers, WZ, Q, memory and the exact T-state count, and deletes
  the file again. `KEEP=1` keeps the downloads.
- **SingleStepTests/65x02** (the Oric's 6502): `make sst6502` downloads
  each opcode's 10000 cases, checks registers, memory and every bus cycle
  (address, data, read or write), and deletes the file again. `KEEP=1`
  keeps the downloads.
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
