#!/bin/sh
# Build a ZX Spectrum Next SD card image from a copy of the system/next
# distribution (https://gitlab.com/thesmog358/tbblue).
#
#   tools/mksd.sh out.img [tbblue-dir] [size-MB]
#
# The image has an MBR with one FAT32 partition from 1 MB, like a card
# prepared on a PC. Needs mkfs.fat (dosfstools) and mcopy (mtools).
set -e
out=${1:?usage: $0 out.img [tbblue-dir] [size-MB]}
src=${2:-../tbblue}
size=${3:-512}

[ -f "$src/TBBLUE.FW" ] || { echo "$src does not look like the tbblue distribution (no TBBLUE.FW)" >&2; exit 1; }

case "$out" in /*) ;; *) out="$PWD/$out" ;; esac
rm -f "$out"
truncate -s "${size}M" "$out"

# MBR: partition 1, type 0x0C (FAT32 LBA), sectors 2048 .. end.
sectors=$((size * 2048 - 2048))
python3 - "$out" "$sectors" <<'PY'
import struct, sys
path, n = sys.argv[1], int(sys.argv[2])
entry = struct.pack('<B3sB3sII', 0x80, b'\xfe\xff\xff', 0x0c, b'\xfe\xff\xff', 2048, n)
with open(path, 'r+b') as f:
    f.seek(446)
    f.write(entry + bytes(48) + b'\x55\xaa')
PY

mkfs.fat -F 32 -n NEXT --offset 2048 "$out" $((sectors / 2)) >/dev/null
(cd "$src" && for f in *; do
    case "$f" in .git|*.md|TBBLUE.TBU) continue ;; esac   # .TBU is a core update
    mcopy -s -i "$out@@1M" "$f" ::/
done)
echo "$out: ${size} MB, FAT32, from $src"
