#!/bin/sh
# install-service.sh -- install/enable the rbpi4b RK3399 stack as a systemd service.
#   sudo sh scripts/rk3399/install-service.sh            install, enable, start
#   sudo sh scripts/rk3399/install-service.sh uninstall  stop, disable, remove
# The old stack's unit (rx3-rk3399-ddj400.service) is left installed but disabled;
# the two conflict, so starting either stops the other.
set -eu
[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
UNIT=rx3-rbpi4b-rk3399.service
DROPIN=/etc/systemd/system/rx3-interface-toggle.service.d

if [ "${1:-install}" = uninstall ]; then
    systemctl disable --now "$UNIT" 2>/dev/null || true
    rm -f "/etc/systemd/system/$UNIT" "$DROPIN/rbpi4b.conf"
    rmdir "$DROPIN" 2>/dev/null || true
    systemctl daemon-reload
    echo "removed $UNIT and the toggle drop-in"
    exit 0
fi

install -m 644 "$HERE/systemd/$UNIT" "/etc/systemd/system/$UNIT"
mkdir -p "$DROPIN"
install -m 644 "$HERE/systemd/rx3-interface-toggle.service.d/rbpi4b.conf" "$DROPIN/rbpi4b.conf"
systemctl daemon-reload
systemctl disable rx3-rk3399-ddj400.service 2>/dev/null || true
systemctl enable "$UNIT"
systemctl restart "$UNIT"
echo "installed, enabled and started $UNIT"
