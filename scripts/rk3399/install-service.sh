#!/bin/sh
# install-service.sh -- install/enable the rbpi4b RK3399 stack as a systemd service.
#   sudo sh scripts/rk3399/install-service.sh            install, enable, start
#   sudo sh scripts/rk3399/install-service.sh uninstall  stop, disable, remove
# The old stack's unit (rx3-rk3399-ddj400.service) is left installed but disabled;
# the two conflict, so starting either stops the other.
#
# Also installs the base rx3-interface-toggle.path/.service pair (the
# DDJ-400 interface on/off gesture), pointing directly at this repo's own
# scripts/rk3399/toggle-interface.sh. Until 2026-10-18 this script only
# installed a drop-in over a base unit that had to already exist, pointing
# at a different (sibling) project's script -- a gap on a genuinely fresh
# install with nothing else set up. Installing the base pair directly here
# removes that gap; the drop-in is removed if found, since it is no longer
# needed once the base unit already points to the right place.
set -eu
[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo)" >&2; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
UNIT=rx3-rbpi4b-rk3399.service
TOGGLE_PATH_UNIT=rx3-interface-toggle.path
TOGGLE_SERVICE_UNIT=rx3-interface-toggle.service
DROPIN=/etc/systemd/system/rx3-interface-toggle.service.d

if [ "${1:-install}" = uninstall ]; then
    systemctl disable --now "$UNIT" "$TOGGLE_PATH_UNIT" 2>/dev/null || true
    rm -f "/etc/systemd/system/$UNIT" \
          "/etc/systemd/system/$TOGGLE_PATH_UNIT" \
          "/etc/systemd/system/$TOGGLE_SERVICE_UNIT" \
          "$DROPIN/rbpi4b.conf"
    rmdir "$DROPIN" 2>/dev/null || true
    systemctl daemon-reload
    echo "removed $UNIT and the interface-toggle units"
    exit 0
fi

install -m 644 "$HERE/systemd/$UNIT" "/etc/systemd/system/$UNIT"
install -m 644 "$HERE/systemd/$TOGGLE_PATH_UNIT" "/etc/systemd/system/$TOGGLE_PATH_UNIT"
install -m 644 "$HERE/systemd/$TOGGLE_SERVICE_UNIT" "/etc/systemd/system/$TOGGLE_SERVICE_UNIT"
rm -f "$DROPIN/rbpi4b.conf"
rmdir "$DROPIN" 2>/dev/null || true
systemctl daemon-reload
systemctl disable rx3-rk3399-ddj400.service 2>/dev/null || true
systemctl enable --now "$TOGGLE_PATH_UNIT"
systemctl enable "$UNIT"
systemctl restart "$UNIT"
echo "installed, enabled and started $UNIT and $TOGGLE_PATH_UNIT"
