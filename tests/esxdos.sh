#!/bin/sh
# esxDOS on a divMMC (an MB03+ Ultimate) with a folder for its SD card:
# boot, SAVE a BASIC line from the Spectrum, and find the file in the folder.
# Needs tests-data/esxdos/ (esxmmc.bin plus the matching SYS/ and BIN/,
# see tests/README.md).
set -e
RUN=${1:-build/zxrun}
E=tests-data/esxdos
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "esxdos: FAIL: $*"; exit 1; }
[ -f $E/esxmmc.bin ] || { echo "esxdos: no $E/esxmmc.bin, skipped"; exit 0; }

for m in 48 128 plus3 pentagon; do
    rm -rf "$T/sd" && mkdir "$T/sd"
    cp -r $E/SYS $E/BIN "$T/sd/"
    # the 48K's RAM test after esxDOS boots takes until about frame 300
    "$RUN" -m $m -i mb03 -e $E/esxmmc.bin -c "$T/sd" -f 900 -d 500 \
        -t '10 ehello from esxdos
s*"hi.bas"
' >/dev/null    # "e" is REM and "s" SAVE in keyword mode
    [ -f "$T/sd/HI.BAS" ] || fail "$m: SAVE *\"hi.bas\" did not reach the folder"
    head -c 8 "$T/sd/HI.BAS" | grep -q PLUS3DOS || fail "$m: HI.BAS has no +3DOS header"
    grep -q "hello from esxdos" "$T/sd/HI.BAS" || fail "$m: HI.BAS does not hold the program"
    echo "esxdos: $m ok"
done
