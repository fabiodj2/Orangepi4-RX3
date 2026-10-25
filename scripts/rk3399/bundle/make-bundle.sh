#!/bin/sh
# make-bundle.sh -- pack this unit's working install (repo + work/: runtime
# chroot, DirectFB build, toolkit, ddj400-bridge) into one portable tarball that
# install-bundle.sh unpacks on a fresh Armbian (Orange Pi 4 LTS or 3 LTS).
#
#   sudo sh scripts/rk3399/bundle/make-bundle.sh <out-dir>
#
# Output, in <out-dir>/rx3-bundle-<date>/:
#   rx3-rk3399-bundle.tar.gz   the tree, relative to the user's home
#   install-bundle.sh          the installer (a copy of the one next to this)
#   MANIFEST.txt               source board, commit, BUILD, sizes
#   SHA256SUMS
#
# The tar reads through a NON-recursive bind mount of /, so it sees the files on
# the root filesystem and nothing mounted over them: not the USB stick bound at
# work/runtime/rx3/media/usb1/sda1, not /proc /sys /dev bound into the chroot,
# not the toolkit's rbp variant bound over root/pdj/rbp. Safe with rbp running.
#
# The bundle carries extracted XDJ-RX3 firmware (the chroot). It is for the
# owner's own units: keep it on private media, never in the git repo (NOTICE.md).
set -eu
[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
OUT=${1:?usage: make-bundle.sh <out-dir>}

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
PARENT=$(dirname "$REPO")
NAME=$(basename "$REPO")
DEST="$OUT/rx3-bundle-$(date +%Y%m%d-%H%M)"

SNAP=/run/rx3-bundle-root
mkdir -p "$SNAP"
mountpoint -q "$SNAP" || mount --bind / "$SNAP"
trap 'umount "$SNAP" 2>/dev/null; rmdir "$SNAP" 2>/dev/null' EXIT INT TERM
[ -d "$SNAP$REPO" ] || { echo "$REPO is not on the root filesystem" >&2; exit 1; }

mkdir -p "$DEST"
echo "== packing $REPO -> $DEST/rx3-rk3399-bundle.tar.gz"
# work/log: this unit's logs. rx3/dev/gpiodrv: a plain file rbp appends to
# forever (tens of MB); install-bundle.sh recreates it empty. __pycache__: noise.
tar -C "$SNAP$PARENT" --numeric-owner --xattrs -cpf - \
    --exclude="$NAME/work/log/*" \
    --exclude="$NAME/work/runtime/rx3/dev/gpiodrv" \
    --exclude='__pycache__' \
    "$NAME" | gzip -1 > "$DEST/rx3-rk3399-bundle.tar.gz"

install -m 755 "$HERE/install-bundle.sh" "$DEST/install-bundle.sh"

{
    echo "rx3 bundle, made $(date -Iseconds)"
    echo "source board: $(tr '\0' ' ' < /proc/device-tree/compatible)"
    echo "source os:    $(. /etc/os-release; echo "$PRETTY_NAME"), kernel $(uname -r)"
    echo "repo:         $REPO"
    echo "commit:       $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo ?) ($(git -C "$REPO" branch --show-current 2>/dev/null || echo ?))"
    echo "dirty files:  $(git -C "$REPO" status --short 2>/dev/null | wc -l)"
    echo "BUILD:        $(head -1 "$REPO/BUILD" 2>/dev/null || echo ?)"
    echo "unpacked:     $(du -sh --apparent-size "$SNAP$REPO" | cut -f1) (approx.)"
    echo "tarball:      $(du -h "$DEST/rx3-rk3399-bundle.tar.gz" | cut -f1)"
} > "$DEST/MANIFEST.txt"
(cd "$DEST" && sha256sum rx3-rk3399-bundle.tar.gz install-bundle.sh > SHA256SUMS)
cat "$DEST/MANIFEST.txt"
echo "done: $DEST"
