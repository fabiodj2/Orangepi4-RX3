#!/bin/sh
# =============================================================================
# usb-watch.sh — Chromebit USB-A rekordbox stick hotplug -> rbp
#
# Presents a REAL rekordbox USB stick to `rbp` exactly like the XDJ-RX3 does:
#
#   attach:  mount  /dev/sdX1 -> /media/usb1/sda1   (RX3 vfat options)
#            bind   /media/usb1/sda1 -> /home/user/rbx3-run/media/usb1/sda1
#            write  "umount /media/usb1/sda1" then
#                   "mount  /media/usb1/sda1"  -> /tmp/udev_usb1   (rbp FIFO)
#   detach:  write  "umount /media/usb1/sda1" -> FIFO, lazy-unmount both.
#
# Any storage device on the watched port works, and a hub may expose several at
# once (stick + card reader).  Candidates are ranked: a device carrying
# PIONEER/rekordbox/export.pdb wins, otherwise the first one with a recognised
# filesystem is used (rbp then shows the FOLDER view).  Empty card readers are
# skipped, which is why the pick is re-evaluated whenever the device set
# changes rather than being decided once at boot.
#
# The `mount` line reaches rbp's UsbMountManager thread, which calls
# DbProxy::reqAttach -> db::DbIF::mount(type 3). DeviceSQL (edb_streamd) then
# imports PIONEER/rekordbox/export.pdb natively and the mount-info detect flag
# for media kind 2 becomes 2 => the UI shows "USB1 <volume label>" with the
# full database (Songs/Playlists), not just the FOLDER view.
#
# Chromebit specifics vs the Prime GO (PrimeBox):
#   * the only host controller is the dwc2 OTG port in host mode,
#     /sys/devices/platform/ff580000.usb/usb1 (root hub "usb1");
#     the stick hangs off the USB2.1 hub as 1-1.x. We therefore match the
#     "/usb1/" root-hub segment, not the Prime GO's usb3/usb4.
#   * root lives on the internal eMMC (mmcblk0p3), so a stick is never the
#     boot device — but we still skip any disk with mounted partitions.
#   * paths: chroot is /home/user/rbx3-run, log on the (persistent) root fs.
#
# The umount-before-mount order matters: rbp's PathDecider ignores a `mount`
# that is not preceded by an `umount` ("UNMOUNT was not executed before").
# Never write to /tmp/udev_usbctn* — that triggers the cosmetic
# "USB Error. Remove the device." caution.
#
# TWO SLOTS BY PHYSICAL PORT (2026-10-25, Orange Pi 3 LTS). With USBWATCH_PORT1
# set, each slot of rbp is fed from its own physical port(s) instead:
#   slot 1  /media/usb1/sda1  <- a device under any fragment in USBWATCH_PORT1
#   slot 2  /media/usb2/sda1  <- a device under any fragment in USBWATCH_PORT2
# Each list is space-separated sysfs path fragments, e.g. the H6's USB 3.0 port
# "5200000.usb/" and its second USB 2.0 port "5311000.usb/ 5311400.usb/" (EHCI
# and its OHCI companion: a full-speed device enumerates on the companion).
# Devices on any other port take no slot. Slot 2's FIFO is /tmp/udev_usb2 and
# its chroot path /media/usb2/sda1, rbp's own second media slot (the Pi 4 port,
# scripts/device/usb-watch.sh, fed both since 2026-10-07). Readiness is the
# Pi 4's witness: rbp holding THIS slot's export.pdb open, read from
# /proc/<pid>/fd as the chroot-bind path, plus slot 1's detect flag as before.
# USBWATCH_PORT1 unset = the single-slot behaviour above, on USBWATCH_SEG.
#
# Usage:  sh /home/user/usb-watch.sh start|stop|status|run
# Env:    USBWATCH_SEG="/usb1/"   sysfs path fragment of the watched root hub
#         USBWATCH_PORT1/PORT2    per-slot physical ports (see above)
#         USBWATCH_POLL=1         poll interval seconds
#         USBWATCH_HC_RECOVER     1 = rebind a USB host controller the kernel
#                                 declared dead (default 1 with PORT1, else 0)
# =============================================================================

CHROOT="${RX3_RUNTIME:-/home/orangepi/RX3-RBPI4B-RK3399/work/runtime/rx3}"
PORT1="${USBWATCH_PORT1:-}"
PORT2="${USBWATCH_PORT2:-}"
if [ -n "$PORT1" ] && [ -n "$PORT2" ]; then SLOTS="1 2"; else SLOTS="1"; fi
if [ -n "$PORT1" ]; then HC_RECOVER="${USBWATCH_HC_RECOVER:-1}"; else HC_RECOVER="${USBWATCH_HC_RECOVER:-0}"; fi
LOG="${USBWATCH_LOG:-$CHROOT/tmp/usbwatch.log}"
PIDFILE="${USBWATCH_PIDFILE:-$CHROOT/tmp/usbwatch.pid}"
SEG="${USBWATCH_SEG:-/usb5/}"
POLL="${USBWATCH_POLL:-1}"
TIMEOUT=$(command -v timeout 2>/dev/null || echo "")

log() { echo "$(date '+%F %T') $$ $*" >> "$LOG"; }

# --- the slots: rbp's fixed paths, one set per slot -------------------------
slot_mnt()   { [ "$1" = 2 ] && echo /media/usb2/sda1 || echo "${USBWATCH_MOUNT:-/media/usb1/sda1}"; }
slot_chmnt() { echo "$CHROOT/media/usb$1/sda1"; }
slot_path()  { echo "/media/usb$1/sda1"; }        # what rbp sees, in the chroot
slot_fifo()  { echo "$CHROOT/tmp/udev_usb$1"; }
slot_ports() {                                    # sysfs fragments the slot owns
    if [ "$1" = 2 ]; then echo "$PORT2"; else echo "${PORT1:-$SEG}"; fi
}

# --- candidates: every sd* disk on the watched port -------------------------
# A port can carry several storage devices at once (the user's hub exposes the
# rekordbox stick *and* a card reader).  Devices with no medium report size 0
# and must be skipped, otherwise the watcher latches onto the empty one and
# never looks at the stick.  System disks (root/boot) are never candidates.
is_system_disk() {
    dev=$1
    for part in /dev/"$dev" /dev/"$dev"?; do
        [ -e "$part" ] || continue
        for m in / /boot; do
            mp=$(awk -v d="$part" '$1==d {print $2}' /proc/mounts 2>/dev/null)
            [ "$mp" = "$m" ] && return 0
        done
    done
    return 1
}

find_sticks() {
    fports=$(slot_ports "$1")
    for blk in /sys/block/sd*; do
        [ -e "$blk" ] || continue
        tgt=$(readlink -f "$blk" 2>/dev/null) || continue
        hit=0
        for frag in $fports; do
            case "$tgt" in *"$frag"*) hit=1; break ;; esac
        done
        [ "$hit" = 1 ] || continue
        d=${blk##*/}
        is_system_disk "$d" && continue
        sz=$(cat "$blk/size" 2>/dev/null || echo 0)
        [ "${sz:-0}" -gt 0 ] 2>/dev/null || continue    # no medium (card reader)
        echo "$d"
    done
}

# --- best usable partition --------------------------------------------------
# Some macOS-created sticks have a small EFI partition as sdX1 and the actual
# rekordbox library as sdX2. Select the largest recognised filesystem.
find_partition() {
    dev=$1
    attempt=0

    while [ "$attempt" -lt 10 ]; do
        best=""
        best_size=0

        for node in /sys/class/block/"$dev"[0-9]*; do
            [ -e "$node" ] || continue

            part=${node##*/}
            fstype=$(blkid -s TYPE -o value "/dev/$part" 2>/dev/null)
            [ -n "$fstype" ] || continue

            size=$(cat "$node/size" 2>/dev/null || echo 0)
            case "$size" in
                ''|*[!0-9]*) size=0 ;;
            esac

            if [ "$size" -gt "$best_size" ]; then
                best="$part"
                best_size="$size"
            fi
        done

        if [ -n "$best" ]; then
            echo "$best"
            return 0
        fi

        sleep 0.1
        attempt=$((attempt + 1))
    done

    if blkid "/dev/$dev" >/dev/null 2>&1; then
        echo "$dev"
        return 0
    fi

    return 1
}

# --- is this a rekordbox stick?  (read-only probe for export.pdb) -----------
# If the candidate is already mounted (the normal case while it is in use),
# look at that mount point instead of trying to mount it again - a second
# mount would fail with "Can't mount, would change RO state" and its message
# would leak into the caller's output.
probe_rekordbox() {
    pdev=$1
    mp=$(awk -v d="/dev/$pdev" '$1==d {print $2}' /proc/mounts 2>/dev/null | head -1)
    if [ -n "$mp" ]; then
        [ -f "$mp/PIONEER/rekordbox/export.pdb" ] && return 0
        return 1
    fi
    mkdir -p /mnt/rbprobe
    if mount -o ro "/dev/$pdev" /mnt/rbprobe >/dev/null 2>&1; then
        rc=1
        [ -f /mnt/rbprobe/PIONEER/rekordbox/export.pdb ] && rc=0
        umount /mnt/rbprobe 2>/dev/null
        rmdir /mnt/rbprobe 2>/dev/null
        return $rc
    fi
    rm -rf /mnt/rbprobe 2>/dev/null
    return 1
}

# --- pick the best candidate -------------------------------------------------
# 1. a device carrying PIONEER/rekordbox/export.pdb (the real library)
# 2. otherwise any device with a recognised filesystem (rbp then shows FOLDER)
find_stick() {
    fallback=""
    for d in $(find_sticks "$1"); do
        part=$(find_partition "$d") || continue
        fstype=$(blkid -s TYPE -o value "/dev/$part" 2>/dev/null)
        [ -n "$fstype" ] || continue
        if probe_rekordbox "$part"; then
            log "pick: slot $1: $d ($part, $fstype) has a rekordbox database"
            echo "$d"
            return 0
        fi
        [ -z "$fallback" ] && fallback="$d ($part, $fstype)"
    done
    if [ -n "$fallback" ]; then
        log "pick: slot $1: no rekordbox db found, using $fallback"
        echo "${fallback%% *}"
        return 0
    fi
    return 1
}

# --- FIFO notification (never block forever if rbp is down) -----------------
notify() {
    FIFO=$(slot_fifo "$1")
    msg=$2
    [ -p "$FIFO" ] || { log "notify: $FIFO missing (rbp down?)"; return 1; }
    if [ -n "$TIMEOUT" ]; then
        "$TIMEOUT" 3 sh -c 'printf "%s" "$1" > "$2"' sh "$msg" "$FIFO" 2>/dev/null \
            && { log "notify: $msg"; return 0; }
    else
        ( printf "%s" "$msg" > "$FIFO" ) 2>/dev/null &
        p=$!
        ( sleep 3; kill -9 "$p" 2>/dev/null ) &
        w=$!
        if wait "$p" 2>/dev/null; then
            kill "$w" 2>/dev/null
            log "notify: $msg"; return 0
        fi
        kill "$w" 2>/dev/null
    fi
    log "notify: FAILED '$msg' (rbp not reading?)"
    return 1
}

# --- mount + chroot bind + notify ------------------------------------------
attach() {
    slot=$1
    dev=$2
    MNT=$(slot_mnt "$slot")
    CH_MNT=$(slot_chmnt "$slot")
    part=$(find_partition "$dev") || { log "attach: no usable partition on $dev"; return 1; }

    mkdir -p "$MNT"
    if mountpoint -q "$MNT"; then
        log "attach: $MNT already mounted (refreshing bind only)"
    else
        fstype=$(blkid -s TYPE -o value "/dev/$part" 2>/dev/null)
        [ -n "$fstype" ] || fstype=vfat
        case "$fstype" in
            vfat)    mount -t vfat -o flush,rw,noatime,shortname=mixed,dmask=000,fmask=000,codepage=437,iocharset=iso8859-1,usefree,utf8 "/dev/$part" "$MNT" ;;
            exfat)   mount -t exfat -o rw,noatime "/dev/$part" "$MNT" ;;
            hfsplus) mount -t hfsplus -o force,rw,noatime "/dev/$part" "$MNT" ;;
            *)       mount "/dev/$part" "$MNT" ;;
        esac
        rc=$?
        if [ $rc -ne 0 ]; then
            log "attach: mount /dev/$part -> $MNT failed rc=$rc"
            return 1
        fi
        label=$(blkid -s LABEL -o value "/dev/$part" 2>/dev/null)
        log "attach: mounted /dev/$part ($fstype${label:+ label=$label}) -> $MNT"
    fi

    mkdir -p "$CH_MNT"
    if ! mountpoint -q "$CH_MNT"; then
        if ! mount --bind "$MNT" "$CH_MNT"; then
            log "attach: chroot bind $MNT -> $CH_MNT failed"
            return 1
        fi
        log "attach: chroot bind ok ($CH_MNT)"
    fi

    # bind must exist BEFORE rbp inspects the stick's files (export.pdb etc.)
    notify "$slot" "umount $(slot_path "$slot")"
    sleep 0.3
    notify "$slot" "mount $(slot_path "$slot")"
    log "attach: slot $slot: notified native mount $(slot_path "$slot")"

    # diagnose the rekordbox DB import a few seconds later (best effort)
    if [ "${USBWATCH_PROBE:-1}" = 1 ] && [ -x /home/user/usb-probe.sh ]; then
        ( sleep 6; sh /home/user/usb-probe.sh -q 2>/dev/null | sed 's/^/probe: /' >> "$LOG" ) &
    fi
    return 0
}

# --- notify rbp + release mounts --------------------------------------------
detach() {
    slot=$1
    MNT=$(slot_mnt "$slot")
    CH_MNT=$(slot_chmnt "$slot")
    log "detach: slot $slot: notifying rbp"
    notify "$slot" "umount $(slot_path "$slot")"
    sleep 1
    if mountpoint -q "$CH_MNT"; then umount -l "$CH_MNT"; log "detach: umount -l $CH_MNT"; fi
    if mountpoint -q "$MNT";    then umount -l "$MNT";    log "detach: umount -l $MNT";    fi
    rmdir "$CH_MNT" 2>/dev/null
    rmdir "$MNT" 2>/dev/null
}

rbp_pid() {
    pgrep -n -f '^/lib/ld-linux\.so\.3 /root/pdj/rbp -a$' 2>/dev/null
}

# --- has rbp imported the DB? (mount-info detect flag for media kind 2 == 2) --
# A `mount` event delivered while rbp is still starting up is dropped, so the
# watcher keeps re-sending it until DeviceSQL reports the drive READY.
usb_detected() {
    p=$(rbp_pid); [ -n "$p" ] || return 1
    # Either slot: rbp holds this slot's export.pdb open (the Pi 4's witness).
    ls -l "/proc/$p/fd" 2>/dev/null |
        grep -Fq "$(slot_chmnt "$1")/PIONEER/rekordbox/export.pdb" && return 0
    [ "$1" = 1 ] || return 1
    v=$(dd if="/proc/$p/mem" bs=4 skip=$((0x03256888 / 4)) count=1 2>/dev/null \
        | od -An -tu4 2>/dev/null | tr -d ' \t')
    [ "$v" = "2" ]
}

# Per-slot state lives in variables named <field>_<slot> (cur_1, cur_2, ...):
# POSIX sh has no arrays, so get/set go through eval on these fixed names only.
sget() { eval "echo \"\${$1_$2}\""; }
sset() { eval "$1_$2=\$3"; }

renotify() {
    notify "$1" "umount $(slot_path "$1")"
    sleep 0.3
    notify "$1" "mount $(slot_path "$1")"
}

# One poll tick for one slot: the old single-slot body, unchanged in substance.
tick_slot() {
    n=$1
    rbp=$2
    devs=$(find_sticks "$n" | tr '\n' ' ')
    if [ "$devs" != "$(sget devs_last "$n")" ]; then
        sset devs_last "$n" "$devs"
        log "slot $n: devices on its port(s): '${devs% }'"
        sset dev "$n" "$(find_stick "$n" || true)"
    fi
    dev=$(sget dev "$n")
    cur=$(sget cur "$n")

    if [ -n "$dev" ]; then
        if [ "$dev" != "$cur" ]; then
            [ -n "$cur" ] && detach "$n"
            log "attach: slot $n: detected $dev"
            if attach "$n" "$dev"; then
                sset cur "$n" "$dev"
                sset confirm_left "$n" "${USBWATCH_CONFIRM_TRIES:-18}"
                sset confirm_wait "$n" 5
            else
                sset cur "$n" ""
                sset confirm_left "$n" 0
            fi
        elif [ -n "$rbp" ] && [ "$rbp" != "$last_rbp" ]; then
            log "attach: slot $n: rbp restarted ($last_rbp -> $rbp), re-notifying mount"
            sleep 2
            renotify "$n"
            sset confirm_left "$n" "${USBWATCH_CONFIRM_TRIES:-18}"
            sset confirm_wait "$n" 5
        elif [ "$(sget confirm_left "$n")" -gt 0 ] && [ -n "$rbp" ]; then
            if usb_detected "$n"; then
                log "confirm: USB$n database READY"
                sset confirm_left "$n" 0
            elif [ "$(sget confirm_wait "$n")" -le 0 ]; then
                sset confirm_left "$n" $(( $(sget confirm_left "$n") - 1 ))
                sset confirm_wait "$n" 5
                log "confirm: slot $n not ready yet, re-notifying mount ($(sget confirm_left "$n") left)"
                renotify "$n"
            else
                sset confirm_wait "$n" $(( $(sget confirm_wait "$n") - 1 ))
            fi
        fi
    elif [ -n "$cur" ]; then
        detach "$n"
        sset cur "$n" ""
        sset confirm_left "$n" 0
    fi
}

# --- dead host controller recovery -----------------------------------------
# The H6's xHCI (Orange Pi 3 LTS, legacy 6.12) can die when a hub is pulled out
# of the USB 3.0 port live: "xhci-hcd xhci-hcd.1.auto: xHCI host controller not
# responding, assume dead" / "HC died; cleaning up" (2026-10-25). The port then
# sees nothing until the driver is unbound and bound again -- done by hand that
# day, and the stick came straight back as USB1. This does the same: every few
# seconds count the kernel's "HC died" lines; for each new one, rebind that
# platform device, at most once per HC_RECOVER_GAP seconds per controller. The
# slot code needs nothing extra: the device vanishes (detach) and comes back
# (attach) like any unplug/replug.
HC_RECOVER_GAP=30
hc_seen=""          # count of "HC died" lines already handled (empty = baseline)

hc_check() {
    [ "$HC_RECOVER" = 1 ] || return 0
    lines=$(dmesg 2>/dev/null | grep 'HC died')
    count=$(printf '%s' "$lines" | grep -c . )
    if [ -z "$hc_seen" ] || [ "$count" -lt "$hc_seen" ]; then
        hc_seen=$count          # first look, or the ring buffer wrapped
        return 0
    fi
    [ "$count" -gt "$hc_seen" ] || return 0
    # "[ 1623.678551] xhci-hcd xhci-hcd.1.auto: HC died; cleaning up" -- the
    # timestamp may hold a space, so it is cut off before splitting.
    printf '%s\n' "$lines" | tail -n $((count - hc_seen)) |
    sed 's/^\[[^]]*\] *//' |
    while read -r drv devname _rest; do
        devname=${devname%:}
        sysdrv=/sys/bus/platform/drivers/$drv
        [ -e "$sysdrv/$devname" ] || [ -e "$sysdrv/bind" ] || {
            log "hc: $devname ($drv) died, no platform driver to rebind it"; continue; }
        stamp=/run/usbwatch-hc-$devname
        now=$(date +%s)
        last=$(cat "$stamp" 2>/dev/null || echo 0)
        if [ $((now - last)) -lt "$HC_RECOVER_GAP" ]; then
            log "hc: $devname died again within ${HC_RECOVER_GAP}s: not rebinding"
            continue
        fi
        echo "$now" > "$stamp"
        log "hc: $devname ($drv) died -- unbind + bind"
        echo "$devname" > "$sysdrv/unbind" 2>/dev/null
        sleep 2
        if echo "$devname" > "$sysdrv/bind" 2>/dev/null; then
            log "hc: $devname rebound"
        else
            log "hc: $devname bind FAILED"
        fi
    done
    hc_seen=$count
}

run() {
    if [ -n "$PORT1" ]; then
        log "=== usb-watch run: slot 1 ports '$PORT1', slot 2 ports '${PORT2:-none}', poll ${POLL}s ==="
    else
        log "=== usb-watch run: seg '$SEG', poll ${POLL}s ==="
    fi
    for n in $SLOTS; do
        sset cur "$n" ""; sset devs_last "$n" "unset"; sset dev "$n" ""
        sset confirm_left "$n" 0; sset confirm_wait "$n" 0
    done
    last_rbp=$(rbp_pid)
    hc_tick=0
    hc_check            # baseline: old deaths (before this run) are not ours

    while :; do
        hc_tick=$((hc_tick + 1))
        if [ "$hc_tick" -ge 5 ]; then hc_tick=0; hc_check; fi
        rbp_now=$(rbp_pid)
        for n in $SLOTS; do
            tick_slot "$n" "$rbp_now"
        done
        last_rbp=$rbp_now
        sleep "$POLL"
    done
}

start() {
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        echo "already running (pid $(cat "$PIDFILE"))"
        return 0
    fi
    log "=== usb-watch start ==="
    nohup sh "$0" run >/dev/null 2>&1 &
    echo $! > "$PIDFILE"
    sleep 1
    echo "started pid $(cat "$PIDFILE")"
}

stop() {
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        kill "$(cat "$PIDFILE")" 2>/dev/null
        rm -f "$PIDFILE"
        echo "stopped"
    else
        echo "not running"
    fi
}

status() {
    echo "pid:         $(cat "$PIDFILE" 2>/dev/null || echo none)"
    for n in $SLOTS; do
        echo "--- slot $n (USB$n)"
        echo "ports:       '$(slot_ports "$n")'"
        echo "candidates:  $(find_sticks "$n" | tr '\n' ' ')"
        echo "host mount:  $(mountpoint -q "$(slot_mnt "$n")" && echo yes || echo no)  ($(slot_mnt "$n"))"
        echo "chroot bind: $(mountpoint -q "$(slot_chmnt "$n")" && echo yes || echo no)  ($(slot_chmnt "$n"))"
        echo "ready:       $(usb_detected "$n" && echo yes || echo no)"
    done
    echo "--- log tail ---"
    tail -15 "$LOG" 2>/dev/null
}

case "$1" in
    start)  start ;;
    stop)   stop ;;
    status) status ;;
    run)    run ;;
    *) echo "usage: $0 start|stop|status"; exit 1 ;;
esac
