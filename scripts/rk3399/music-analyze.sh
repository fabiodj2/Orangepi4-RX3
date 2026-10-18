#!/bin/sh
# music-analyze.sh -- Tier 1 (light) analysis: BPM + approximate key for one
# audio file, via aubiotrack/aubiopitch. Measured on this unit (under the
# older rx3-rk3399-ddj400.service stack, with JuceALSA not yet contending
# for the same cores -- see docs/rk3399/01-music-analysis.md's correction
# before trusting these numbers under rx3-rbpi4b-rk3399.service): ~5-10s per
# 3-minute track on a single A53 core.
#
# Usage: music-analyze.sh <audio-file> <track-id> [output-dir]
#
# Writes <output-dir>/<track-id>.json. Idempotent: a track already analysed
# (file exists) or already being analysed (lock held) is skipped, not
# re-run -- safe to call from a watcher that may see the same file twice.
set -eu

AUDIO="$1"
TRACK_ID="$2"
OUTDIR="${3:-/tmp/rx3-analysis}"

mkdir -p "$OUTDIR"
OUT="$OUTDIR/$TRACK_ID.json"
LOCK="$OUTDIR/.lock-$TRACK_ID"

if [ -f "$OUT" ]; then
    exit 0
fi

if ! mkdir "$LOCK" 2>/dev/null; then
    exit 0
fi
trap 'rmdir "$LOCK" 2>/dev/null || true' EXIT INT TERM

if [ ! -f "$AUDIO" ]; then
    echo "music-analyze: $AUDIO not found" >&2
    exit 1
fi

TMP_BEATS=$(mktemp)
TMP_PITCH=$(mktemp)
trap 'rm -f "$TMP_BEATS" "$TMP_PITCH"; rmdir "$LOCK" 2>/dev/null || true' EXIT INT TERM

# aubiotrack prints one beat timestamp (seconds) per line.
aubiotrack "$AUDIO" >"$TMP_BEATS" 2>/dev/null || true
# aubiopitch prints "<time> <freq_hz>" pairs, one per analysis frame.
aubiopitch "$AUDIO" >"$TMP_PITCH" 2>/dev/null || true

# BPM = 60 / median inter-beat interval, rejecting outliers outside a
# plausible dance-music range (30-300 BPM <-> 0.2-2.0s between beats).
BPM=$(awk '
    { t[NR] = $1 }
    END {
        if (NR < 2) { print "0"; exit }
        n = 0
        for (i = 2; i <= NR; i++) {
            d = t[i] - t[i-1]
            if (d > 0.2 && d < 2.0) { s[n++] = d }
        }
        if (n < 1) { print "0"; exit }
        for (i = 0; i < n; i++)
            for (j = i + 1; j < n; j++)
                if (s[j] < s[i]) { tmp = s[i]; s[i] = s[j]; s[j] = tmp }
        med = s[int(n / 2)]
        printf "%.1f", 60.0 / med
    }
' "$TMP_BEATS")

# Rough tonal centre: histogram of frame pitches onto the 12 pitch classes,
# most common wins. This is NOT a chroma/Krumhansl key estimate -- see
# docs/rk3399/01-music-analysis.md for why that is Tier 2's job.
KEY=$(awk '
    function pc(f) {
        if (f <= 0) return -1
        midi = 69 + 12 * log(f / 440.0) / log(2)
        p = int(midi + 0.5) % 12
        if (p < 0) p += 12
        return p
    }
    {
        p = pc($2)
        if (p >= 0) cnt[p]++
    }
    END {
        names[0]="C"; names[1]="C#"; names[2]="D"; names[3]="D#"
        names[4]="E"; names[5]="F"; names[6]="F#"; names[7]="G"
        names[8]="G#"; names[9]="A"; names[10]="A#"; names[11]="B"
        best = -1; bestn = -1
        for (i = 0; i < 12; i++)
            if (cnt[i] > bestn) { bestn = cnt[i]; best = i }
        print (best < 0) ? "unknown" : names[best]
    }
' "$TMP_PITCH")

cat >"$OUT" <<EOF
{
  "track_id": "$TRACK_ID",
  "source": "$AUDIO",
  "bpm": $BPM,
  "key_approx": "$KEY",
  "analyzer": "rx3-analyze-tier1-aubio-v1",
  "analyzed_at": "$(date -Iseconds)"
}
EOF

echo "music-analyze: $TRACK_ID -> bpm=$BPM key_approx=$KEY" >&2
