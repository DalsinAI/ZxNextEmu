#!/bin/sh
# SAM Coupé disk and sound check (tests/sam.c) on a fresh SAMDOS disk.
# SAMDOS 2 comes from SimCoupé's resources into tests-data/ on first use.
set -e
BUILD=${1:-build}
DOS=tests-data/samdos2.sbt
if [ ! -f "$DOS" ]; then
    mkdir -p tests-data
    curl -sSfL -o "$DOS" https://raw.githubusercontent.com/simonowen/simcoupe/master/Resource/samdos2.sbt
fi
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cp "$DOS" "$T/samdos2"
"$BUILD/mkmgt" "$T/dos.mgt" "$T/samdos2"
"$BUILD/sam" roms "$T/dos.mgt"
