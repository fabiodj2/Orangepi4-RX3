#!/bin/sh
# toggle-interface.sh -- the DDJ-400's interface on/off gesture for this stack.
# Adapted from RX3-RK3399-DDJ400/scripts/rb/toggle-rk3399-interface.sh.
#
# OFF stops rbp (and edb_streamd) only and leaves /run/rx3-interface-off: the
# service's `run` supervisor sees that marker and waits instead of restarting, so
# ddj400-bridge stays up to hear the gesture that turns it back ON. ON clears the
# marker and restarts the service.
set -eu

REQUEST=/run/rx3-interface-toggle.request
STATE=/run/rx3-interface-off
MAIN_SERVICE=rx3-rbpi4b-rk3399.service

rm -f "$REQUEST"

if pgrep -f '^/lib/ld-linux\.so\.3 /root/pdj/rbp -a$' >/dev/null 2>&1; then
    echo 'RX3 interface: stopping the player and restoring the console'
    : > "$STATE"
    pkill -TERM -f '^/lib/ld-linux\.so\.3 /root/pdj/rbp -a$' 2>/dev/null || true
    pkill -TERM -f '^/lib/ld-linux\.so\.3 /usr/bin/edb_streamd$' 2>/dev/null || true
    sleep 2
    if [ -w /sys/class/vtconsole/vtcon1/bind ]; then
        echo 1 > /sys/class/vtconsole/vtcon1/bind || true
    fi
    if command -v chvt >/dev/null 2>&1; then
        chvt 1 >/dev/null 2>&1 || true
    fi
    echo 'RX3 interface: OFF'
else
    echo 'RX3 interface: restarting the stack'
    rm -f "$STATE"
    systemctl restart --no-block "$MAIN_SERVICE"
    echo 'RX3 interface: ON requested'
fi
