#!/bin/sh
# TR-DOS on the Beta 128 machines: load, save, catalogue and format through
# the emulated WD1793, on a .scl given as bytes and on .trd image files.
#
# usage: tests/beta.sh build/beta romdir
# romdir needs trdos.rom (Pentagon), 256s-0..3.rom (Scorpion) and
# atm2-0..3.rom (ATM Turbo 2+) besides the Sinclair ROMs; a machine whose
# ROMs are missing is skipped (see roms/README.md).
set -e
beta=$1
roms=${2:-roms}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# A disk with two BASIC programs: "boot" (runs when TR-DOS starts) and
# "hello" (both print HELLO FROM DISK).
python3 - "$tmp" <<'PY'
import struct, sys
d = sys.argv[1]
def basic(lines):
    return b''.join(struct.pack('>H', n) + struct.pack('<H', len(b) + 1) + b + b'\r' for n, b in lines)
prog = basic([(10, b'\xf5"HELLO FROM DISK"'), (20, b'\xf5"LINE TWO"')])
data = (prog + b'\x80\xaa\x0a\x00').ljust(256, b'\0')
def scl(names):
    out = b'SINCLAIR' + bytes([len(names)])
    for n in names:
        out += n.ljust(8).encode() + b'B' + struct.pack('<HH', len(prog), len(prog)) + b'\x01'
    out += data * len(names)
    return out + struct.pack('<I', sum(out) & 0xffffffff)
open(d + '/boot.scl', 'wb').write(scl(['boot', 'hello']))
open(d + '/hello.scl', 'wb').write(scl(['hello']))
# The same as a .trd: catalogue, then the file from track 1.
trd = bytearray(655360)
trd[0:16] = b'hello   B' + struct.pack('<HH', len(prog), len(prog)) + bytes([1, 0, 1])
trd[16 * 256:17 * 256] = data
trd[0x8e1:0x8e8] = bytes([1, 1, 0x16, 1]) + struct.pack('<H', 2543) + b'\x10'
open(d + '/hello.trd', 'wb').write(trd)
open(d + '/blank.trd', 'wb').write(bytes(655360))
PY

run() {
    r=0
    "$beta" "$roms" "$@" || r=$?
    [ $r = 0 ] || [ $r = 77 ] || exit 1
}

# Scorpion: "128 TR-DOS" from the menu runs "boot".
run scorpion "$tmp/boot.scl" '450:
' '1000?HELLO FROM DISK'
# Pentagon: 48 BASIC, RANDOMIZE USR 15616; SAVE, CAT, LOAD.
run pentagon "$tmp/hello.trd" '110:|||
' '250:t~l15616
' '600?TR-DOS' '650:s"copy"
' '950:~)
' '1200?copy    <B>' '1250:j"hello"
' '1500?HELLO FROM DISK'
# ATM Turbo 2+: TR-DOS 48 from the menu; CAT and LOAD.
run atm2 "$tmp/hello.scl" '160:|
' '400?A>' '500:~)
' '800?hello   <B>' '850:j"hello"
' '1100?HELLO FROM DISK'
# Scorpion: FORMAT a blank disk (double sided, 2544 sectors), then SAVE.
run scorpion "$tmp/blank.trd" '450:
' '900:~_"blank"
' '9000?2544/2544' '9100:s"prog"
' '9400?O.K.'

# What TR-DOS wrote must be in the images.
python3 - "$tmp" <<'PY'
import sys
d = sys.argv[1]
def cat(path):
    t = open(path, 'rb').read()
    return [t[i * 16:i * 16 + 8].decode('latin1').strip() for i in range(128) if t[i * 16]], t
names, t = cat(d + '/hello.trd')
if len(t) and t[0x8e4] != 1:     # skipped machines leave it alone
    assert names == ['hello', 'copy'], names
names, t = cat(d + '/blank.trd')
if t[0x8e7] == 0x10:
    assert names == ['prog'] and t[0x8e3] == 0x16 and t[0x8f5:0x8fa] == b'blank', (names, t[0x8e3])
print('disk images: ok')
PY
