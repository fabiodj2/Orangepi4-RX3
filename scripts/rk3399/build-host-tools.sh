#!/bin/sh
# build-host-tools.sh -- build native (aarch64, not cross-compiled) host-side
# helpers that run alongside the chroot, outside LD_PRELOAD.
#
# Brought in from RX3-RK3399-DDJ400 2026-10-18 so this repo no longer depends
# on that one at runtime (start-rk3399.sh used to exec $OLD/work/build/host/
# ddj400-bridge and $OLD/scripts/rb/device/usb-watch.sh directly).
#
# Usage: sh scripts/rk3399/build-host-tools.sh
# Output: work/build/host/ddj400-bridge
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$REPO/work/build/host}

mkdir -p "$OUT"

echo "== ddj400-bridge"
gcc -O2 -Wall -Wextra \
    -o "$OUT/ddj400-bridge" \
    "$REPO/scripts/rk3399/device/ddj400-bridge.c"

file "$OUT/ddj400-bridge"
echo "PASS: $OUT/ddj400-bridge"
