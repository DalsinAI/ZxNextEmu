#!/bin/sh
# Extract the bytes of a romgen-style VHDL ROM (x"NN" literals) as raw binary.
#   tools/vhd2bin.sh bootrom.vhd > bootrom.bin
grep -o 'x"[0-9A-Fa-f][0-9A-Fa-f]"' "$1" | sed 's/x"\(..\)"/\1/' | tr -d '\n' |
  perl -ne 'print pack("H*", $_)'
