#!/bin/sh
# The ESP8266 module against a local TCP echo server (needs python3).
set -e
TOOL=${1:-build/esp}
PORT=${PORT:-7190}   # ZX Cradle ports are 7000-7999 (docs/cradle)
python3 -c "
import socket, sys
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', $PORT)); s.listen(1); s.settimeout(30)
c, _ = s.accept()
while True:
    d = c.recv(4096)
    if not d: break
    c.sendall(d)
" &
SERVER=$!
sleep 0.5
"$TOOL" "${ROMS:-roms}" $PORT
STATUS=$?
kill $SERVER 2>/dev/null || true
exit $STATUS
