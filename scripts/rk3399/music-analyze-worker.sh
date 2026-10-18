#!/bin/sh
# music-analyze-worker.sh -- one Tier-1 worker: blocks on the shared FIFO,
# runs music-analyze.sh for each path it receives.
#
# Pinned to the A53 set (same default as RX3_AUDIO_CPU in start-rk3399.sh,
# "0-3") rather than left to float: unpinned is exactly how gui_task ended
# up wandering onto the A53s in the display work (docs/rk3399/00-port.md),
# and JuceALSA now lives on these same four cores by design. Run 2-4 of
# these via the music-analyzer@.service template -- re-measure contention
# against a live track before assuming the sibling benchmark's "no
# slowdown" result holds here (docs/rk3399/01-music-analysis.md).
set -eu

QUEUE="${RX3_ANALYZE_QUEUE:-/run/rx3-analyze/queue.fifo}"
OUTDIR="${RX3_ANALYZE_OUTDIR:-/tmp/rx3-analysis}"
CPUS="${RX3_ANALYZE_CPUS:-0-3}"
SELF="$(dirname "$0")/music-analyze.sh"

mkdir -p "$(dirname "$QUEUE")"
[ -p "$QUEUE" ] || mkfifo -m 666 "$QUEUE"

while true; do
    if IFS=$(printf '\t') read -r path id < "$QUEUE"; then
        [ -n "${path:-}" ] || continue
        if command -v taskset >/dev/null 2>&1 && [ -n "$CPUS" ]; then
            taskset -c "$CPUS" "$SELF" "$path" "$id" "$OUTDIR" || \
                echo "music-analyze-worker: failed on $path" >&2
        else
            "$SELF" "$path" "$id" "$OUTDIR" || \
                echo "music-analyze-worker: failed on $path" >&2
        fi
    fi
done
