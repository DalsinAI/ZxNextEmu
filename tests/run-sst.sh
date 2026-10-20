#!/bin/sh
# Download SingleStepTests/z80 v1 files one at a time, run them, delete them.
#   tests/run-sst.sh [prefix ...]     e.g. tests/run-sst.sh "" cb dd ed fd "dd cb __" "fd cb __"
set -u
BIN=${BIN:-build/sst}
DIR=${DIR:-tests-data/sst}
URL=https://raw.githubusercontent.com/SingleStepTests/z80/main/v1
mkdir -p "$DIR"
[ $# -eq 0 ] && set -- "" cb dd ed fd "dd cb __" "fd cb __"
fail=0
for prefix in "$@"; do
  for hi in 0 1 2 3 4 5 6 7 8 9 a b c d e f; do
    for lo in 0 1 2 3 4 5 6 7 8 9 a b c d e f; do
      op=$hi$lo
      name=${prefix:+$prefix }$op
      case "$name" in cb|dd|ed|fd|"dd cb"|"fd cb"|"dd dd"|"dd ed"|"dd fd"|"fd dd"|"fd ed"|"fd fd") continue;; esac
      f="$DIR/$name.json"
      if [ ! -f "$f" ]; then
        enc=$(printf '%s' "$name" | sed 's/ /%20/g')
        code=$(curl -sS -o "$f" -w '%{http_code}' "$URL/$enc.json")
        [ "$code" = 200 ] || { rm -f "$f"; continue; }
      fi
      out=$("$BIN" "$f") || { fail=$((fail+1)); printf '%s\n' "$out"; }
      [ -n "${KEEP:-}" ] || rm -f "$f"
    done
  done
done
echo "files with failures: $fail"
exit $fail
