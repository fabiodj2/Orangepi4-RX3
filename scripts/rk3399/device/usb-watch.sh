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
# Usage:  sh /home/user/usb-watch.sh start|stop|status|run
# Env:    USBWATCH_SEG="/usb1/"   sysfs path fragment of the watched root hub
#         USBWATCH_POLL=1         poll interval seconds
# =============================================================================

MNT="${USBWATCH_MOUNT:-/media/usb1/sda1}"
CHROOT="${RX3_RUNTIME:-/home/orangepi/RX3-RBPI4B-RK3399/work/runtime/rx3}"
CH_MNT="$CHROOT/media/usb1/sda1"
FIFO="$CHROOT/tmp/udev_usb1"
LOG="${USBWATCH_LOG:-$CHROOT/tmp/usbwatch.log}"
PIDFILE="${USBWATCH_PIDFILE:-$CHROOT/tmp/usbwatch.pid}"
SEG="${USBWATCH_SEG:-/usb5/}"
POLL="${USBWATCH_POLL:-1}"
TIMEOUT=$(command -v timeout 2>/dev/null || echo "")

log() { echo "$(date '+%F %T') $$ $*" >> "$LOG"; }

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
    for blk in /sys/block/sd*; do
        [ -e "$blk" ] || continue
        tgt=$(readlink -f "$blk" 2>/dev/null) || continue
        case "$tgt" in
            *"$SEG"*) : ;;
            *) continue ;;
        esac
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
    for d in $(find_sticks); do
        part=$(find_partition "$d") || continue
        fstype=$(blkid -s TYPE -o value "/dev/$part" 2>/dev/null)
        [ -n "$fstype" ] || continue
        if probe_rekordbox "$part"; then
            log "pick: $d ($part, $fstype) has a rekordbox database"
            echo "$d"
            return 0
        fi
        [ -z "$fallback" ] && fallback="$d ($part, $fstype)"
    done
    if [ -n "$fallback" ]; then
        log "pick: no rekordbox db found, using $fallback"
        echo "${fallback%% *}"
        return 0
    fi
    return 1
}

# --- FIFO notification (never block forever if rbp is down) -----------------
notify() {
    msg=$1
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
    dev=$1
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
    notify "umount $MNT"
    sleep 0.3
    notify "mount $MNT"
    log "attach: notified native mount $MNT"

    # diagnose the rekordbox DB import a few seconds later (best effort)
    if [ "${USBWATCH_PROBE:-1}" = 1 ] && [ -x /home/user/usb-probe.sh ]; then
        ( sleep 6; sh /home/user/usb-probe.sh -q 2>/dev/null | sed 's/^/probe: /' >> "$LOG" ) &
    fi
    return 0
}

# --- notify rbp + release mounts --------------------------------------------
detach() {
    log "detach: notifying rbp"
    notify "umount $MNT"
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
    v=$(dd if="/proc/$p/mem" bs=4 skip=$((0x03256888 / 4)) count=1 2>/dev/null \
        | od -An -tu4 2>/dev/null | tr -d ' \t')
    [ "$v" = "2" ]
}

run() {
    log "=== usb-watch run: seg '$SEG', poll ${POLL}s ==="
    cur=""
    devs_last=""
    dev=""
    last_rbp=$(rbp_pid)
    confirm_left=0      # re-notify attempts remaining for the current attach
    confirm_wait=0      # poll ticks until the next attempt

    while :; do
        # only re-probe (mounting candidates read-only) when the set of storage
        # devices changed - e.g. the empty card reader being joined by the stick
        devs=$(find_sticks | tr '\n' ' ')
        if [ "$devs" != "$devs_last" ]; then
            devs_last="$devs"
            log "devices on watched port: '${devs% }'"
            dev=$(find_stick) || dev=""
        fi
        rbp=$(rbp_pid)

        if [ -n "$dev" ]; then
            if [ "$dev" != "$cur" ]; then
                [ -n "$cur" ] && detach
                log "attach: detected $dev on watched port"
                if attach "$dev"; then
                    cur=$dev
                    confirm_left=${USBWATCH_CONFIRM_TRIES:-18}
                    confirm_wait=5
                else
                    cur=""
                    confirm_left=0
                fi
            elif [ -n "$rbp" ] && [ "$rbp" != "$last_rbp" ]; then
                log "attach: rbp restarted ($last_rbp -> $rbp), re-notifying mount"
                sleep 2
                notify "umount $MNT"
                sleep 0.3
                notify "mount $MNT"
                confirm_left=${USBWATCH_CONFIRM_TRIES:-18}
                confirm_wait=5
            elif [ "$confirm_left" -gt 0 ] && [ -n "$rbp" ]; then
                if usb_detected; then
                    log "confirm: USB1 database READY"
                    confirm_left=0
                elif [ "$confirm_wait" -le 0 ]; then
                    confirm_left=$((confirm_left - 1))
                    confirm_wait=5
                    log "confirm: not ready yet, re-notifying mount (${confirm_left} left)"
                    notify "umount $MNT"
                    sleep 0.3
                    notify "mount $MNT"
                else
                    confirm_wait=$((confirm_wait - 1))
                fi
            fi
        else
            if [ -n "$cur" ]; then
                detach
                cur=""
                confirm_left=0
            fi
        fi
        last_rbp=$rbp
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
    echo "watched:     usb segment '$SEG'"
    echo "candidates:  $(find_sticks | tr '\n' ' ')"
    echo "stick:       $(find_stick || echo none)"
    echo "host mount:  $(mountpoint -q "$MNT" && echo yes || echo no)  ($MNT)"
    echo "chroot bind: $(mountpoint -q "$CH_MNT" && echo yes || echo no)  ($CH_MNT)"
    [ -x /home/user/usb-probe.sh ] && sh /home/user/usb-probe.sh -q
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
