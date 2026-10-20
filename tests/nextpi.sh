#!/bin/sh
# NextZXOS's Pi tools against the emulated NextPi: .pisend -q finds the Pi,
# then .piput, .piget and .pisend move files both ways.
# Needs a copy of the Next software: TBBLUE=path/to/tbblue (a clone of
# https://gitlab.com/thesmog358/tbblue).
set -e
ZXRUN=${1:-build/zxrun}
TBBLUE=${TBBLUE:-../tbblue}
[ -f "$TBBLUE/TBBLUE.FW" ] || { echo "nextpi: set TBBLUE to the Next software folder"; exit 2; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "nextpi: FAIL: $*"; exit 1; }

mkdir "$T/sd" "$T/pi" "$T/pi/ram"
(cd "$TBBLUE" && tar cf - --exclude=.git --exclude=TBBLUE.TBU .) | tar xf - -C "$T/sd"
# a first boot asks for the video mode; answer it once
"$ZXRUN" -m next -r "${ROMS:-roms}" -c "$T/sd" -f 900 -d 400 -t "
          y" > /dev/null
head -c 70000 /dev/urandom > "$T/sd/tmp/up.bin"
head -c 40000 /dev/urandom > "$T/pi/ram/down.bin"
head -c 3001 /dev/urandom > "$T/sd/tmp/b64.bin"

P='```````````````'     # each ` is a pause of 10 frames
"$ZXRUN" -m next -r "${ROMS:-roms}" -c "$T/sd" -p "$T/pi" -f 6500 -d 450 -t " $P""n$P.pisend -q
$P$P.piver -p
$P$P.piput /tmp/up.bin
$P$P$P$P$P$P$P$P$P$P.piget /ram/down.bin
$P$P$P$P$P$P$P$P$P$P.pisend /tmp/b64.bin
" > /dev/null

grep -q "^1.92" "$T/sd/tmp/piver.cache" || fail ".piver"
cmp -s "$T/pi/ram/up.bin" "$T/sd/tmp/up.bin" || fail ".piput"
cmp -s "$T/sd/down.bin" "$T/pi/ram/down.bin" || fail ".piget"
cmp -s "$T/pi/ram/tmp/b64.bin" "$T/sd/tmp/b64.bin" || fail ".pisend file"
echo "nextpi: all checks passed"
