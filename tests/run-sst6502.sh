#!/bin/sh
# Download SingleStepTests/65x02 6502 files one at a time, run them, delete them.
#   tests/run-sst6502.sh [opcode ...]     e.g. tests/run-sst6502.sh 69 e9
set -u
BIN=${BIN:-build/sst6502}
DIR=${DIR:-tests-data/sst6502}
URL=https://raw.githubusercontent.com/SingleStepTests/65x02/main/6502/v1
mkdir -p "$DIR"
if [ $# -eq 0 ]; then
  set --
  for hi in 0 1 2 3 4 5 6 7 8 9 a b c d e f; do
    for lo in 0 1 2 3 4 5 6 7 8 9 a b c d e f; do set -- "$@" $hi$lo; done
  done
fi
fail=0
for op in "$@"; do
  f="$DIR/$op.json"
  if [ ! -f "$f" ]; then
    code=$(curl -sS -o "$f" -w '%{http_code}' "$URL/$op.json")
    [ "$code" = 200 ] || { rm -f "$f"; echo "$op: download failed"; fail=$((fail+1)); continue; }
  fi
  out=$("$BIN" "$f") || { fail=$((fail+1)); printf '%s\n' "$out"; }
  [ -n "${KEEP:-}" ] || rm -f "$f"
done
echo "files with failures: $fail"
exit $fail
