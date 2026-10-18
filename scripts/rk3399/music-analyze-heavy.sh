#!/bin/sh
# music-analyze-heavy.sh -- Tier 2 (heavy): MFCC + chroma + beat + structural
# segmentation, via librosa. MEASURED on this unit (under the older stack):
# 372s pinned to one A53, 80s on whatever core the scheduler picked (usually
# an A72) -- see docs/rk3399/01-music-analysis.md for the full numbers,
# including the OOM kill this tier triggered under naive parallel use.
#
# THIS SCRIPT IS NEVER AUTO-TRIGGERED. It is meant to be invoked by an
# operator action (one track, on demand), always through the systemd unit
# that carries the memory ceiling and OOM priority -- never run it bare in
# a shell on a unit that is also playing live.
#
# Usage: music-analyze-heavy.sh <audio-file> <track-id> [output-dir]
set -eu

AUDIO="$1"
TRACK_ID="$2"
OUTDIR="${3:-/tmp/rx3-analysis}"
LOCKFILE="${RX3_ANALYZE_HEAVY_LOCK:-/run/rx3-analyze/heavy.lock}"

mkdir -p "$OUTDIR" "$(dirname "$LOCKFILE")"
OUT="$OUTDIR/$TRACK_ID.heavy.json"

if [ ! -f "$AUDIO" ]; then
    echo "music-analyze-heavy: $AUDIO not found" >&2
    exit 1
fi

exec 9>"$LOCKFILE"
if ! flock -n 9; then
    echo "music-analyze-heavy: another heavy job is already running, refusing to start a second one" >&2
    exit 1
fi

PY="$(dirname "$0")/heavy_analysis.py"
RESULT=$(python3 "$PY" "$AUDIO")
STATUS=$?
if [ "$STATUS" -ne 0 ]; then
    echo "music-analyze-heavy: analysis failed for $AUDIO" >&2
    exit 1
fi

cat >"$OUT" <<EOF
{
  "track_id": "$TRACK_ID",
  "source": "$AUDIO",
  "analyzer": "rx3-analyze-tier2-librosa-v1",
  "analyzed_at": "$(date -Iseconds)",
  "raw": "$RESULT"
}
EOF

echo "music-analyze-heavy: $TRACK_ID done -> $OUT" >&2
