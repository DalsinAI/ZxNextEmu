#!/bin/bash
# get.sh URL RELPATH -> $MSXDL/RELPATH (default msx-dl/)
out="${MSXDL:-msx-dl}/$2"
[ -s "$out" ] && exit 0
mkdir -p "$(dirname "$out")"
for i in 1 2 3 4; do
  curl -sS -f -m 600 -A 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36' -o "$out.part" "$1" && mv "$out.part" "$out" && exit 0
  sleep $((i*2))
done
echo "FAIL $2" >&2
