#!/bin/sh
# start-rk3399.sh — run rbp on the Orange Pi 4 LTS (RK3399, HDMI 1920x1080x32)
# with rbpi4b's display + touch stack and the RX3-RK3399-DDJ400 project's
# controller/audio stack.
#
#   from rbpi4b (this repo): DirectFB 1.4.16 + directfb-full.diff (aspect-fit
#     present path, page-count buffer mode, pan pacer), fbshim.so (fb lies,
#     in-process tsc2007 + pointsrc touch, touch zones: menu, FX, hot cues...)
#   from RX3-RK3399-DDJ400 ($OLD): ddj400-bridge + keyshim.so (DDJ-400 map),
#     audioshim.so (DDJ-400 USB audio), memshim.so (RTC mmap), netshim.so,
#     librx3_core.so (STEMS), usb-watch.sh
#
# Usage: sudo sh scripts/rk3399/start-rk3399.sh start|stop|status|run
# docs/rk3399/00-port.md has the why of every choice below.
set -eu
ulimit -c 0

REPO=$(cd "$(dirname "$0")/../.." && pwd)
OLD=${RX3_OLD_REPO:-$(cd "$REPO/../RX3-RK3399-DDJ400" && pwd)}
RUNTIME=${RX3_RUNTIME:-$REPO/work/runtime/rx3}
LOGDIR="$REPO/work/log"
DDJ="$OLD/work/build/host/ddj400-bridge"
WATCHER="$OLD/scripts/rb/device/usb-watch.sh"
OLD_LAUNCHER="$OLD/scripts/rb/start-rk3399-ddj400.sh"
HP_GAIN=${RX3_HP_GAIN_X:-4}
USB_SEG=${USBWATCH_SEG:-/usb}

[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo)" >&2; exit 1; }

# The preload order is load-bearing. rbpi4b's fbshim goes BEFORE memshim so its
# open()/read()/close() own /dev/tsc2007_2-0048 (a pipe fed by pointsrc) and
# /dev/gpiodrv; memshim stays for the MAP_SHARED fd=-1 RTC mmap. keyshim must
# stay: it is the DDJ-400's path into IKeyManager.
PRELOAD=/root/pdj/librx3_core.so:/usr/lib/rbpi4b/fbshim.so:/usr/lib/memshim.so:/usr/lib/audioshim.so:/usr/lib/keyshim.so:/usr/lib/netshim.so

stop_all() {
    # The old launcher's stop knows every process and pattern of both setups
    # (same rbp command line), and restores the console.
    sh "$OLD_LAUNCHER" stop >/dev/null 2>&1 || true
    env RX3_RUNTIME="$RUNTIME" USBWATCH_SEG="$USB_SEG" sh "$WATCHER" stop 2>/dev/null || true
}

status() {
    pgrep -af 'rbp -a$|edb_streamd|ddj400-bridge|usb-watch' || true
    echo '--- PRESENT (driver decision)'
    grep -a 'PRESENT' "$RUNTIME/tmp/dfbdig9.log" 2>/dev/null | tail -4 || true
    echo '--- pointsrc'
    tail -5 "$RUNTIME/tmp/pointsrc.log" 2>/dev/null || true
}

case "${1:-start}" in
    stop) stop_all; exit 0 ;;
    status) status; exit 0 ;;
    start|restart|run) ;;
    *) echo "Usage: $0 start|stop|status|run" >&2; exit 2 ;;
esac

mkdir -p "$LOGDIR"
stop_all

for d in proc sys dev/snd dev/shm dev/input tmp media/usb1/sda1; do mkdir -p "$RUNTIME/$d"; done
mountpoint -q "$RUNTIME/proc"      || mount --bind /proc "$RUNTIME/proc"
mountpoint -q "$RUNTIME/sys"       || mount --bind /sys "$RUNTIME/sys"
mountpoint -q "$RUNTIME/dev/shm"   || mount --bind /dev/shm "$RUNTIME/dev/shm"
mountpoint -q "$RUNTIME/dev/snd"   || mount --bind /dev/snd "$RUNTIME/dev/snd"
# pointsrc scans /dev/input/event* itself (rbpi4b binds all of /dev; we bind
# only what rbp needs).
mountpoint -q "$RUNTIME/dev/input" || mount --bind /dev/input "$RUNTIME/dev/input"
[ -e "$RUNTIME/dev/fb0" ] || mknod "$RUNTIME/dev/fb0" c 29 0
mountpoint -q "$RUNTIME/dev/fb0"   || mount --bind /dev/fb0 "$RUNTIME/dev/fb0"
# The display's DRM node, for the shim's overlay planes (drmband.c: menu band,
# drawers, effect picker). Here card0 is rockchip-drm and card1 is panfrost (no
# CRTC) -- the reverse of the Pi's v3d/vc4 -- hence DRM_BAND_CARD below. Without
# a plane those panels are painted on rbp's own page and rbp repaints over them.
DRI_CARD=${RX3_DRI_CARD:-/dev/dri/card0}
mkdir -p "$RUNTIME/dev/dri"
[ -e "$RUNTIME$DRI_CARD" ] || touch "$RUNTIME$DRI_CARD"
mountpoint -q "$RUNTIME$DRI_CARD" || mount --bind "$DRI_CARD" "$RUNTIME$DRI_CARD"

# FIFOs still used: DDJ-400 bridge -> keyshim, and rbp's own control FIFO.
# NOT the touch FIFO: fbshim's tscfake serves /dev/tsc2007_2-0048 itself.
for f in "$RUNTIME/tmp/rb-ctrl.fifo" "$RUNTIME/dev/rx3-control"; do
    rm -f "$f"; mkfifo -m 0666 "$f"
done
# rbp's media channel (usb-watch writes mount/umount text here) and keyshim's
# key FIFO: created once, never recreated under a running reader (rbpi4b
# fix-dev.sh does the same for the udev_* set).
for f in udev_usb1 udev_usb2 udev_usbctn1 udev_usbctn2 rb-keys.fifo; do
    [ -p "$RUNTIME/tmp/$f" ] || { rm -f "$RUNTIME/tmp/$f"; mkfifo -m 0666 "$RUNTIME/tmp/$f"; }
done
rm -f "$RUNTIME/dev/tsc2007_2-0048"
rm -f "$RUNTIME/tmp/req_LocalDBServer" "$RUNTIME/tmp/guard_LocalDBServer" \
      "$RUNTIME/tmp/audioshim.log" "$RUNTIME/tmp/keyshim.log" \
      "$RUNTIME/tmp/dfbdig9.log" "$RUNTIME/tmp/pointsrc.log"

# Identity touch calibration: tscfake writes logical 1280x800 (rbpi4b docs/07).
CALIB='0
0
320
200
1280
800'
mkdir -p "$RUNTIME/root/settings"
for c in TouchCalib_Factory.dat TouchCalib_User.dat; do
    [ "$(cat "$RUNTIME/root/settings/$c" 2>/dev/null)" = "$CALIB" ] ||
        printf '%s\n' "$CALIB" > "$RUNTIME/root/settings/$c"
done

[ -w /sys/class/vtconsole/vtcon1/bind ] && echo 0 > /sys/class/vtconsole/vtcon1/bind || true

nohup chroot "$RUNTIME" /lib/ld-linux.so.3 /usr/bin/edb_streamd \
    >"$LOGDIR/edb-streamd.log" 2>&1 </dev/null &

nohup "$DDJ" -v -f "$RUNTIME/tmp/rb-ctrl.fifo" \
    >"$LOGDIR/ddj400-bridge.log" 2>&1 </dev/null &

# KMS_SCANOUT=1: rockchip-drm's /dev/fb0 is a deferred-I/O shadow (every page
# write faults; the kernel copies to the real buffer on its own clock), so the
# shim puts a dumb buffer on the primary plane and the driver presents there
# (drmband.c rx3_kms_scanout, fbdev.c fbdev_kms_scanout). DFB_PRESENT_SKIP=1
# because the pixel budget's skip=2 was sized for the slow route. Either at 0
# (RB_KMS_SCANOUT=0, RB_DFB_PRESENT_SKIP=) puts back the measured old path.
# RBP_FRAME_US=1000: rbp's own limiter sleeps until 16 ms after the previous
# flip RETURNED; with a flip that waits for vblank that is 30 fps. The shim
# rewrites its 16000 to 1000 in memory (fb_shim.c) so vblank alone paces it.
# RB_RBP_FRAME_US= (empty) leaves rbp untouched.
# FB_LIE_BPP=16 must agree with rbp's layer word at 0x1a3ab8 (RGB16 in our
# binary); rbpi4b's patch-rbp-depth.py is the way to 32 (both or neither).
# DFB_PRESENT unset = 'off', which the driver auto-upgrades to 'scale' on a fb
# that is not 1280x800 -> aspect-fit 1728x1080 with 96 px bars.
# RX3_RBP_CPUS: CPU list for rbp and every thread it starts (taskset -c). The
# RK3399 is big.LITTLE: cpu0-3 Cortex-A53 @1.4 GHz, cpu4-5 Cortex-A72 @1.8 GHz;
# unpinned, gui_task (which draws and presents) wanders onto the A53s.
CPUS=${RX3_RBP_CPUS:-}
nohup ${CPUS:+taskset -c "$CPUS"} chroot "$RUNTIME" env \
    PATH=/bin:/sbin:/usr/bin:/usr/sbin \
    DFBARGS="system=fbdev,mode=1280x800,pixelformat=RGB16,primary-only,no-hardware" \
    FB_LIE_BPP=16 \
    DFB_PRESENT_FIT="${RB_DFB_PRESENT_FIT:-fit}" \
    PAN_PACER_MS="${RB_PAN_PACER_MS:-16.666666}" \
    POINT_KIND="${RB_POINT_KIND:-abs}" \
    POINT_DEBUG="${RB_POINT_DEBUG:-0}" \
    POINT_MENU="${RB_POINT_MENU:-1}" \
    POINT_FX_TOUCH="${RB_POINT_FX_TOUCH:-1}" \
    POINT_HOTCUE_TOUCH="${RB_POINT_HOTCUE_TOUCH:-1}" \
    DRM_BAND_CARD="$DRI_CARD" \
    KMS_SCANOUT="${RB_KMS_SCANOUT:-1}" \
    DFB_PRESENT_SKIP="${RB_DFB_PRESENT_SKIP:-1}" \
    RBP_FRAME_US="${RB_RBP_FRAME_US:-1000}" \
    FB_VSYNC_RATE="${RB_FB_VSYNC_RATE:-0}" \
    RX3_AUDIO_DEVICE='hw:CARD=DDJ400,DEV=0' \
    RX3_HP_GAIN_X="$HP_GAIN" \
    RX3_KEYSHIFT=1 \
    RX3_STEMS_DIR=/root/pdj/RX3_STEMS \
    LD_PRELOAD="$PRELOAD" \
    /lib/ld-linux.so.3 /root/pdj/rbp -a \
    >"$LOGDIR/rbp.log" 2>&1 </dev/null &
RBP_PID=$!

sleep 8
env RX3_RUNTIME="$RUNTIME" USBWATCH_SEG="$USB_SEG" sh "$WATCHER" start
sleep 4
status

if [ "${1:-start}" = run ]; then
    trap 'stop_all; exit 0' TERM INT
    while kill -0 "$RBP_PID" 2>/dev/null; do sleep 2; done
    echo 'rbp exited'
    stop_all
    exit 1
fi
