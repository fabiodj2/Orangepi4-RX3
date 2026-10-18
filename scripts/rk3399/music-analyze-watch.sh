#!/bin/sh
# music-analyze-watch.sh -- watches the USB mount points for new audio files
# and feeds their paths into a FIFO that the Tier-1 workers drain.
#
# Deliberately does NOT touch $OLD/scripts/rb/device/usb-watch.sh or rbp's
# own mount FIFOs (/tmp/udev_usb1, /tmp/udev_usbctn1) -- those are
# load-bearing protocol channels for rbp itself. This script only ever
# reads the filesystem.
#
# The worker-queue trick: multiple reader processes blocked on `read line
# < "$QUEUE"` against the same named FIFO each get distinct lines -- the
# kernel hands each write to exactly one waiting reader. That is what gives
# N parallel workers with no extra locking.
set -eu

WATCH_DIRS="${RX3_USB_MOUNTS:-/media/usb1 /media/usb2}"
QUEUE="${RX3_ANALYZE_QUEUE:-/run/rx3-analyze/queue.fifo}"
EXT_PATTERN='\.(wav|WAV|mp3|MP3|aiff|AIFF|flac|FLAC)$'

mkdir -p "$(dirname "$QUEUE")"
[ -p "$QUEUE" ] || mkfifo -m 666 "$QUEUE"

track_id_for() {
    # Stable id from path + size + mtime, so a re-mounted stick does not
    # force every track to re-analyse, but a genuinely different file at
    # the same path does.
    stat -c '%n-%s-%Y' "$1" 2>/dev/null | md5sum | cut -d' ' -f1
}

enqueue() {
    path="$1"
    [ -f "$path" ] || return 0
    id=$(track_id_for "$path")
    # Single-line, under PIPE_BUF (4096 on Linux) -- the write is atomic.
    printf '%s\t%s\n' "$path" "$id" > "$QUEUE"
}

# Pick up what is already on the stick at startup (covers a stick that was
# plugged in before this service started).
for d in $WATCH_DIRS; do
    [ -d "$d" ] || continue
    find "$d" -type f 2>/dev/null | grep -E "$EXT_PATTERN" | while IFS= read -r f; do
        enqueue "$f"
    done
done

# Then watch for anything new.
# shellcheck disable=SC2086
inotifywait -m -r -e close_write -e moved_to --format '%w%f' $WATCH_DIRS 2>/dev/null |
while IFS= read -r path; do
    case "$path" in
        *.wav|*.WAV|*.mp3|*.MP3|*.aiff|*.AIFF|*.flac|*.FLAC)
            enqueue "$path"
            ;;
    esac
done
