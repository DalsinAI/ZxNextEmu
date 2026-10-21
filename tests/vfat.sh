#!/bin/sh
# Folder-backed FAT32 volume: lay out a folder, change it with mtools the
# way a machine would, write it back and check the folder.
# Needs mtools and dosfstools. The second argument, 16, checks FAT16.
set -e
TOOL=${1:-build/vfattool}
FAT=${2:-}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export MTOOLS_SKIP_CHECK=1
I="-i $T/img@@1M"
fail() { echo "vfat: FAIL: $*"; exit 1; }

mkdir -p "$T/f/sub/deeper" "$T/f/Mixed Case"
echo hello > "$T/f/README.TXT"
echo lower > "$T/f/lower.txt"
head -c 100000 /dev/urandom > "$T/f/sub/big.bin"
cp "$T/f/sub/big.bin" "$T/big.orig"
echo x > "$T/f/A long file name.text"
echo y > "$T/f/Mixed Case/MiXeD.Txt"
echo z > "$T/f/sub/deeper/z"
echo z2 > "$T/f/sub/z"                  # same name as one in a subdirectory
echo hidden > "$T/f/.hidden"
head -c 300000 /dev/urandom > "$T/new.bin"

"$TOOL" dump$FAT "$T/f" 512 "$T/img"
dd if="$T/img" of="$T/part" bs=1M skip=1 conv=sparse status=none
fsck.fat -n "$T/part" || fail "fsck of the fresh volume"
mtype $I "::/A long file name.text" | grep -q x || fail "long name read"
mtype $I "::/Mixed Case/MiXeD.Txt" | grep -q y || fail "mixed case read"
mtype $I ::/lower.txt | grep -q lower || fail "lower case 8.3 read"
mcopy $I ::/sub/big.bin "$T/big.read"
cmp -s "$T/big.read" "$T/big.orig" || fail "file content"
mdir $I ::/.hidden > /dev/null 2>&1 && fail "dot file visible"

mcopy $I "$T/new.bin" ::/new.bin
mdel $I ::/lower.txt
mren $I "::/A long file name.text" "::/renamed long name.txt"
mmd $I ::/newdir
echo inside > "$T/in.txt"
mcopy $I "$T/in.txt" ::/newdir/inside.txt
mdeltree $I ::/sub/deeper
echo changed > "$T/README.TXT"
mcopy -o $I "$T/README.TXT" ::/README.TXT
dd if="$T/img" of="$T/part" bs=1M skip=1 conv=sparse status=none
fsck.fat -n "$T/part" > /dev/null || fail "fsck after mtools"

"$TOOL" apply$FAT "$T/f" 512 "$T/img" > /dev/null
cmp -s "$T/f/new.bin" "$T/new.bin" || fail "new file"
[ ! -e "$T/f/lower.txt" ] || fail "deleted file still there"
[ ! -e "$T/f/A long file name.text" ] || fail "renamed file still there"
grep -q x "$T/f/renamed long name.txt" || fail "rename"
grep -q inside "$T/f/newdir/inside.txt" || fail "new directory"
[ ! -e "$T/f/sub/deeper" ] || fail "deleted directory still there"
grep -q changed "$T/f/README.TXT" || fail "overwrite"
cmp -s "$T/f/sub/big.bin" "$T/big.orig" || fail "untouched file changed"
grep -q hidden "$T/f/.hidden" || fail "dot file touched"
grep -q z2 "$T/f/sub/z" || fail "file named like one in a subdirectory"
ls -a "$T/f" | grep -q zxc-sync && fail "temporary file left"
echo "vfat${FAT:+ (FAT16)}: all checks passed"
