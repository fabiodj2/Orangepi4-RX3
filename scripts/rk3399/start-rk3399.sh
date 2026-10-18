#!/bin/sh
# start-rk3399.sh — run rbp on the Orange Pi 4 LTS (RK3399, HDMI 1920x1080x32)
# with rbpi4b's display + touch stack and the DDJ-400 controller/audio stack.
#
# Self-contained as of 2026-10-18 -- ddj400-bridge, usb-watch.sh, keyshim.c,
# memshim.c and audioshim.c were brought in from the sibling RX3-RK3399-DDJ400
# project (which this repo no longer depends on at build or run time; FLX4
# support there is unrelated and not needed here). netshim.so and
# librx3_core.so were never actually sourced from that repo despite an
# earlier version of this comment saying so -- both were already local/
# already deployed into the runtime chroot.
#
#   from rbpi4b (this repo): DirectFB 1.4.16 + directfb-full.diff (aspect-fit
#     present path, page-count buffer mode, pan pacer), fbshim.so (fb lies,
#     in-process tsc2007 + pointsrc touch, touch zones: menu, FX, hot cues...),
#     ddj400-bridge + keyshim.so (DDJ-400 map), audioshim.so (DDJ-400 USB
#     audio), memshim.so (RTC mmap), netshim.so, librx3_core.so (STEMS),
#     usb-watch.sh
#
# Usage: sudo sh scripts/rk3399/start-rk3399.sh start|stop|status|run
# docs/rk3399/00-port.md has the why of every choice below.
set -eu
ulimit -c 0

REPO=$(cd "$(dirname "$0")/../.." && pwd)
RUNTIME=${RX3_RUNTIME:-$REPO/work/runtime/rx3}
LOGDIR="$REPO/work/log"
DDJ="$REPO/work/build/host/ddj400-bridge"
WATCHER="$REPO/scripts/rk3399/device/usb-watch.sh"
HP_GAIN=${RX3_HP_GAIN_X:-4}
# Digital master gain in dB, applied in audioshim.c's scale_master_s24()
# (soft-knee limited, not hard-clamped -- see that file). The first +21dB
# pass (2026-10-18) was tuned with TRIM set low; re-tuned the same day with
# TRIM/MASTER LEVEL at their correct operating position (meter's orange
# segment on peaks, MASTER at max) -- that condition runs far hotter
# pre-gain, and +21 overshot into audible saturation. +22 is the value
# that stayed clean at that TRIM/MASTER position (operator confirmed "som
# limpo" each step up, master_limited read 0). See audioshim.c's comment
# on master_gain_linear() for the full numbers from both passes.
MASTER_GAIN_DB=${RX3_MASTER_GAIN_DB:-22}
USB_SEG=${USBWATCH_SEG:-/usb}

[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo)" >&2; exit 1; }

# The preload order is load-bearing. rbpi4b's fbshim goes BEFORE memshim so its
# open()/read()/close() own /dev/tsc2007_2-0048 (a pipe fed by pointsrc) and
# /dev/gpiodrv; memshim stays for the MAP_SHARED fd=-1 RTC mmap. keyshim must
# stay: it is the DDJ-400's path into IKeyManager.
#
# librx3_core.so goes LAST, and that is load-bearing too: preloaded constructors run
# in reverse list order, so last = first to initialise. The core (KEY/STEMS panel)
# and keyshim (its "native touchscreen performance pads") both hook rbp's
# solveCoordToKey @0x2dc104 and each refuses a prologue it does not recognise. With
# the core first in the list, keyshim hooked first and the core logged "rejected:
# unexpected solveCoordToKey prologue" and dropped its whole UI -- no KEY/STEMS tabs,
# no stems panel (2026-10-13; the old stack too, since keyshim gained that hook).
# Core first to initialise: its UI installs, keyshim's pad-touch hook steps aside
# ("native pad touch hook rejected") -- the hook that fired hot cues and pad-mode
# changes from the strip under the BEAT FX page. The core exports no functions, so
# its place in the list changes no symbol resolution.
# RX3_CORE picks the KEY/STEMS core: `toolkit` (default since 2026-10-14) is an
# rx3-toolkit build staged by
# scripts/rk3399/toolkit/stage-toolkit.py into /root/pdj/toolkit, with its rbp
# variant (the toolkit's image-table word) bind-mounted over /root/pdj/rbp for that
# run and its modules' settings from work/toolkit/toolkit.env. stop_all always
# takes the mount down, so a legacy start never sees the variant. `legacy` is the
# RX3-RK3399-DDJ400 core in /root/pdj (RX3_CORE=legacy to go back to it).
RX3_CORE=${RX3_CORE:-toolkit}
TOOLKIT_ENV="$REPO/work/toolkit/toolkit.env"
case "$RX3_CORE" in
    legacy)  CORE_LIB=/root/pdj/librx3_core.so ;;
    # rbpcomm.so (scripts/shims/rbpcomm.c) renames the process to "rbp": the
    # toolkit core only starts when /proc/self/comm says rbp, and here it says
    # ld-linux.so.3. Last in the list = first constructor to run.
    toolkit) CORE_LIB=/root/pdj/toolkit/librx3_core.so:/usr/lib/rbpi4b/rbpcomm.so ;;
    *) echo "RX3_CORE must be legacy or toolkit, not '$RX3_CORE'" >&2; exit 2 ;;
esac
PRELOAD=/usr/lib/rbpi4b/fbshim.so:/usr/lib/memshim.so:/usr/lib/audioshim.so:/usr/lib/keyshim.so:/usr/lib/netshim.so:$CORE_LIB
# RX3_EXTRA_PRELOAD: diagnostic libraries appended for one run (e.g.
# /usr/lib/rbpi4b/blittrace.so, tools/blittrace). Empty by default.
PRELOAD="$PRELOAD${RX3_EXTRA_PRELOAD:+:$RX3_EXTRA_PRELOAD}"

stop_all() {
    # Inlined 2026-10-18 from RX3-RK3399-DDJ400's start-rk3399-ddj400.sh
    # stop_all() (this repo no longer shells out to that one): the same rbp
    # command line and helper processes exist here, so the same patterns
    # apply, plus restoring the console.
    pkill -TERM -f '^/lib/ld-linux\.so\.3 /root/pdj/rbp -a$' 2>/dev/null || true
    pkill -TERM -f '^/lib/ld-linux\.so\.3 /usr/bin/edb_streamd$' 2>/dev/null || true
    pkill -TERM -f 'work/build/host/ddj400-bridge' 2>/dev/null || true
    env RX3_RUNTIME="$RUNTIME" USBWATCH_SEG="$USB_SEG" sh "$WATCHER" stop 2>/dev/null || true
    sleep 2
    if [ -w /sys/class/vtconsole/vtcon1/bind ]; then
        echo 1 > /sys/class/vtconsole/vtcon1/bind 2>/dev/null || true
    fi
    # The toolkit core's rbp variant (RX3_CORE=toolkit) is only ever a bind mount.
    while mountpoint -q "$RUNTIME/root/pdj/rbp"; do umount "$RUNTIME/root/pdj/rbp" || break; done
    cpu_governor restore
}

# RX3_CPU_GOVERNOR: cpufreq governor while the player runs (default performance;
# empty = leave it). ondemand let the clock drop between loads, and rbp's audio
# path (JuceALSA + the FGPR decoders, 2 x 64-frame periods = 2.9 ms of buffer) then
# missed its time: clicks on KEY/STEMS changes and while browsing. Measured
# 2026-10-14: performance cut the clicks drastically and made controller commands
# answer at once. The previous governor is saved and put back on stop.
GOV_SAVE=/run/rx3-rbpi4b-governor
cpu_governor() {
    for pol in /sys/devices/system/cpu/cpufreq/policy*; do
        [ -w "$pol/scaling_governor" ] || continue
        n=${pol##*/}
        if [ "$1" = restore ]; then
            g=$(sed -n "s/^$n //p" "$GOV_SAVE" 2>/dev/null) || true
            if [ -n "$g" ]; then echo "$g" > "$pol/scaling_governor"; fi
        else
            grep -q "^$n " "$GOV_SAVE" 2>/dev/null ||
                echo "$n $(cat "$pol/scaling_governor")" >> "$GOV_SAVE"
            echo "$1" > "$pol/scaling_governor"
        fi
    done
    [ "$1" = restore ] && rm -f "$GOV_SAVE"
    return 0
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
# A start is an ON: a leftover OFF marker would park the supervisor below.
rm -f /run/rx3-interface-off

for d in proc sys dev/snd dev/shm dev/input tmp media/usb1/sda1; do mkdir -p "$RUNTIME/$d"; done
if [ "$RX3_CORE" = toolkit ]; then
    for f in "$TOOLKIT_ENV" "$RUNTIME/root/pdj/toolkit/rbp" "$RUNTIME/root/pdj/toolkit/librx3_core.so" "$RUNTIME/usr/lib/rbpi4b/rbpcomm.so"; do
        [ -e "$f" ] || { echo "RX3_CORE=toolkit: $f missing (run scripts/rk3399/toolkit/stage-toolkit.py)" >&2; exit 1; }
    done
    . "$TOOLKIT_ENV"
    mount --bind "$RUNTIME/root/pdj/toolkit/rbp" "$RUNTIME/root/pdj/rbp"
    # DDJ-400 STEMS page: SHIFT+BEAT JUMP opens the toolkit's STEMS panel and pads
    # 1-3 tap DRUMS/VOCAL/INST on it -- ddj400-bridge writes to a FIFO, fbshim's
    # pointsrc taps the panel (the toolkit core reads no controller command).
    export RX3_STEMS_PADS=touch
    # Light display mode (rx3-toolkit's theme-white module, already compiled into
    # the mounted core; see docs/rk3399/00-port.md "## Open" 2026-10-15). Letter
    # read by theme_read_mode: d=forced dark, l=light from the first frame,
    # w=light once the player has painted, anything else (our default, 's') =
    # dark at start and switchable from Utility (hold the shim's MENU control,
    # util_zone.c) -- the toolkit author's own README flags Utility switching as
    # unverified on real hardware, so this is a live experiment, not known-good.
    # RX3_THEME= (empty) disables the module outright.
    # stage-toolkit.py's toolkit.env (sourced above) ships RX3_THEME='' (off by
    # default) alongside the light/dark tab artwork pairs (RX3_TAB_LIGHT_*) it
    # already stages either way -- :- here, not -, because the var is SET
    # (empty) by that file, not unset, so a plain ${RX3_THEME-s} would never
    # override it.
    export RX3_THEME=${RX3_THEME:-s}
    echo "core: rx3-toolkit ($CORE_LIB, rbp variant mounted, stems $RX3_STEMS_DIR)"
else
    echo "core: legacy ($CORE_LIB)"
fi
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

# UTILITY -> GENERAL -> VERSION No. reads /tmp/smdj.rev if it exists (native
# ui::StatWatcher::getUtilVersion(), RE'd 2026-10-15: juce::File("/tmp/smdj.rev")
# .existsAsFile(), loads it, keeps printable bytes, caps at 32 chars) and shows
# "Ver <that>" -- otherwise "Ver Can't get version.", which is what this unit
# showed before. $REPO/BUILD is a plain integer (first line only), bumped by
# one on every commit to either this repo or RX3-RK3399-DDJ400 as part of
# this project.
printf '1.20 XDJ-400 v%s' "$(head -1 "$REPO/BUILD" 2>/dev/null || echo '?')" \
    > "$RUNTIME/tmp/smdj.rev"

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
# KMS_MODE=1280x800 (default; RB_KMS_MODE= for the 1920x1080 scaled route): the
# shim sets a 1280x800@60 mode on the HDMI (not in this monitor's EDID, 74.25 MHz
# timing) and rbp's frame goes out 1:1 -- mode=convert, no scaling on our side.
# The monitor itself fits it to 1728x1080 with 96 px bars, the same picture
# rectangle the scaled route drew, so touch keeps POINT_ABS_FIT=1 (fit computed
# from fb0's 1920x1080). Measured: 60.0 fps, worst second 59.9. RB_KMS_MODE_RB: 2 (default) 74.25 MHz, 1 CVT-RB 71 MHz, 0 CVT 83.5 MHz (refused).
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
GOV=${RX3_CPU_GOVERNOR-performance}
if [ -n "$GOV" ]; then cpu_governor "$GOV"; echo "cpufreq governor: $GOV"; fi
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
    KMS_MODE="${RB_KMS_MODE-1280x800}" \
    KMS_MODE_RB="${RB_KMS_MODE_RB:-2}" \
    POINT_ABS_FIT="${RB_POINT_ABS_FIT:-1}" \
    DFB_PRESENT_SKIP="${RB_DFB_PRESENT_SKIP:-1}" \
    RBP_FRAME_US="${RB_RBP_FRAME_US:-1000}" \
    FB_VSYNC_RATE="${RB_FB_VSYNC_RATE:-0}" \
    RX3_AUDIO_DEVICE='hw:CARD=DDJ400,DEV=0' \
    RX3_HP_GAIN_X="$HP_GAIN" \
    RX3_MASTER_GAIN_DB="$MASTER_GAIN_DB" \
    RX3_KEYSHIFT="${RX3_KEYSHIFT:-1}" \
    RX3_STEMS_DIR="${RX3_STEMS_DIR:-/root/pdj/RX3_STEMS}" \
    LD_PRELOAD="$PRELOAD" \
    /lib/ld-linux.so.3 /root/pdj/rbp -a \
    >"$LOGDIR/rbp.log" 2>&1 </dev/null &
RBP_PID=$!

# gui_task (draw + present) on the A72s. The audioshim swallows rbp's own
# affinity/priority calls (stick_this_thread_to_core would pick cores 1/3, which
# here are A53s), so every thread floats over 0-5; a frame drawn on an A53 misses
# vblank. Measured over 30 s: unpinned 57.4 fps avg, worst second 42, 9 seconds
# under 57; pinned 59.9 avg, worst 58, none under 57. RX3_GUI_CPUS= to skip.
GUI_CPUS=${RX3_GUI_CPUS-4,5}
if [ -n "$GUI_CPUS" ]; then
    for i in $(seq 1 40); do
        T=$(ps -L -o tid=,comm= -p "$RBP_PID" 2>/dev/null | awk '$2=="gui_task"{print $1; exit}')
        [ -n "$T" ] && break
        sleep 0.5
    done
    if [ -n "$T" ] && taskset -pc "$GUI_CPUS" "$T" >/dev/null; then
        echo "gui_task $T pinned to cpus $GUI_CPUS"
    else
        echo "gui_task not found: left unpinned" >&2
    fi
fi

# JuceALSA (rbp's audio thread: render + write, 2 x 64 frames = 2.9 ms to the
# DDJ-400) as SCHED_FIFO, on any core: pinning it alone to one core starved the
# decoders and clicked constantly (2026-10-14). Priority 45 stays under the kernel's
# IRQ threads (50), the USB audio completions among them. Needs rt_group_sched=0 on
# the kernel command line (CONFIG_RT_GROUP_SCHED + cgroup v2 cpu controller give a
# service cgroup no RT runtime: EPERM, even as root). RX3_AUDIO_RT_PRIO= to skip.
RT_PRIO=${RX3_AUDIO_RT_PRIO-45}
if [ -n "$RT_PRIO" ]; then
    for i in $(seq 1 40); do
        A=$(ps -L -o tid=,comm= -p "$RBP_PID" 2>/dev/null | awk '$2=="JuceALSA"{print $1; exit}')
        [ -n "$A" ] && break
        sleep 0.5
    done
    if [ -n "$A" ] && chrt -f -p "$RT_PRIO" "$A" 2>/dev/null; then
        echo "JuceALSA $A: SCHED_FIFO $RT_PRIO"
    else
        echo "JuceALSA ${A:-not found}: left SCHED_OTHER (rt_group_sched=0 on the kernel command line?)" >&2
    fi
    # RX3_AUDIO_CPU: CPU affinity for JuceALSA only (taskset -pc on that one
    # thread; does NOT restrict the rest of rbp -- CPUS/RX3_RBP_CPUS stays
    # whatever it already was). That distinction matters: the 2026-10-14
    # regression was JuceALSA forced onto an A72 core *while the rest of rbp
    # was separately confined to the A53s*, starving the decoders of A72 time
    # and causing constant clicks -- not this knob alone.
    # Default is now "0-3" (the A53 set), decided live 2026-10-15: first tried
    # giving JuceALSA its own dedicated A72 core (RX3_GUI_CPUS=4 RX3_AUDIO_CPU=5,
    # splitting the {4,5} set in half) -- worse fps (58.2 avg, 9/30 samples
    # under 57fps) because gui_task lost the ability to float to whichever A72
    # core was free. Then tried moving JuceALSA to the A53s instead, leaving
    # gui_task the full {4,5} set to itself: fps unaffected (59.8 avg over 40
    # one-second samples, basically identical to the plain SCHED_OTHER-any-core
    # baseline's 59.9), and the operator confirmed live by ear -- "tocando aqui,
    # parece limpo, sem clique" -- no audible clicks. A53 cores had ~80 %
    # headroom per core before this (measured same session), so JuceALSA's
    # SCHED_FIFO 45 render slice fits fine alongside the decoders. RX3_AUDIO_CPU=
    # (empty) restores the old any-core behavior if a future regression shows up.
    AUDIO_CPU=${RX3_AUDIO_CPU-0-3}
    if [ -n "$AUDIO_CPU" ] && [ -n "$A" ]; then
        if taskset -pc "$AUDIO_CPU" "$A" >/dev/null; then
            echo "JuceALSA $A: pinned to cpu $AUDIO_CPU"
        else
            echo "JuceALSA $A: taskset pin to cpu $AUDIO_CPU failed" >&2
        fi
    fi
fi

sleep 8
env RX3_RUNTIME="$RUNTIME" USBWATCH_SEG="$USB_SEG" sh "$WATCHER" start
sleep 4
status

if [ "${1:-start}" = run ]; then
    trap 'stop_all; exit 0' TERM INT
    while kill -0 "$RBP_PID" 2>/dev/null; do sleep 2; done
    # The DDJ-400 interface gesture turned the screen OFF (toggle-interface.sh):
    # keep ddj400-bridge up to hear the gesture that turns it back ON, which
    # restarts the service. Exiting here would make systemd restart rbp at once.
    if [ -e /run/rx3-interface-off ]; then
        echo 'rbp stopped by the interface toggle: waiting for ON'
        while [ -e /run/rx3-interface-off ]; do sleep 2; done
    fi
    echo 'rbp exited'
    stop_all
    exit 1
fi
