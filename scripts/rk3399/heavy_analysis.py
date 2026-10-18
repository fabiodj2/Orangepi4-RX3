#!/usr/bin/env python3
"""
heavy_analysis.py -- Tier 2 feature extraction (MFCC, chroma, beat,
self-similarity structure) for exactly one audio file. Not meant to be
invoked directly in production; music-analyze-heavy.sh wraps it with the
single-job lock. See docs/rk3399/01-music-analysis.md for the measured
cost (372s/80s per 3-minute track on this unit) and why this is never run
in parallel.
"""
import sys
import json
import time

t0 = time.time()
import librosa  # noqa: E402
import numpy as np  # noqa: E402  (kept for parity with the benchmark script)
t_import = time.time() - t0


def main(path: str) -> int:
    t0 = time.time()
    y, sr = librosa.load(path, sr=44100, mono=True)
    t_load = time.time() - t0

    t0 = time.time()
    mfcc = librosa.feature.mfcc(y=y, sr=sr, n_mfcc=13)
    chroma = librosa.feature.chroma_cqt(y=y, sr=sr)
    t_features = time.time() - t0

    t0 = time.time()
    tempo, beats = librosa.beat.beat_track(y=y, sr=sr)
    t_beat = time.time() - t0

    t0 = time.time()
    bounds = librosa.segment.agglomerative(chroma, k=8)
    t_structure = time.time() - t0

    result = {
        "tempo_bpm": float(tempo),
        "n_beats": int(len(beats)),
        "n_segments": int(len(bounds)),
        "segment_frames": [int(b) for b in bounds],
        "timing": {
            "import_s": round(t_import, 2),
            "load_s": round(t_load, 2),
            "features_s": round(t_features, 2),
            "beat_s": round(t_beat, 2),
            "structure_s": round(t_structure, 2),
        },
    }
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: heavy_analysis.py <audio-file>", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
