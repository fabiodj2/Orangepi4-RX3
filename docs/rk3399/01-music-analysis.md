# RK3399 — on-the-fly music analysis

## Goal

Compute BPM / approximate key / (optionally) structure for a USB1/USB2
track in the background, using spare CPU time rather than rbp's own.

rbp already has BPM/key/beatgrid from the track's ANLZ data on load. The
value here is analysis rekordbox does **not** give: a background
cross-check computed straight from the audio, cross-deck harmonic
compatibility, and (Tier 2, far more carefully) energy/structure curves.

## Correction against the benchmark this was first based on

The numbers below were measured on this same Orange Pi 4 LTS, but against
the **older `RX3-RK3399-DDJ400` stack** (`rx3-rk3399-ddj400.service`), not
this repo's `rx3-rbpi4b-rk3399.service` — the two are `Conflicts=`, only
one runs at a time, and **`start-rk3399.sh`'s own CPU-pinning decisions
change what "idle A53" means here**:

* `RX3_GUI_CPUS` (default `4,5`) pins `gui_task` (draw + present) to the
  two A72s — confirmed live, 59.9 fps avg vs 57.4 unpinned.
* `RX3_AUDIO_CPU` (default `0-3`) pins **`JuceALSA` to the A53 set** —
  also decided live, after a dedicated-A72-core attempt starved the
  decoders and dropped frames. See `start-rk3399.sh`'s own comment block
  for the by-ear verdict ("parece limpo, sem clique").

**This means cpu0-3 is not spare capacity by default under this stack —
it is where the live audio engine itself was deliberately moved.** The
Tier 1 "4 in parallel, no slowdown" result below was measured with no
`JuceALSA` contending for those same cores. Re-measure it against this
stack, with a track actually playing, before trusting it here — the
sibling benchmark's absence of contention is not evidence this one has
none.

## Tier 1 — light (aubio: beat tracking + pitch histogram)

*Measured on this unit*, but under `rx3-rk3399-ddj400.service` (see
correction above), 180 s / 44.1 kHz stereo test track:

| Test | Result |
|---|---|
| `mpg123` decode, MP3 320 kbps (4 cores) | 1.17 s |
| WAV read, real USB pendrive (10.9 MB file) | 0.627 s (~17.4 MB/s) |
| `aubiotrack` (beat/BPM), WAV, 1 core | 4.85 s |
| `aubiopitch` (pitch histogram → approx. key), WAV, 1 core | 10.40 s |
| 4× `aubiotrack` in parallel, one per A53 core | 4.78 s **total** (no slowdown, but `JuceALSA` was not running at the time) |

Per-track cost (worst case, one core, both algorithms) is ~15 s against a
180 s track. Whether that holds with `JuceALSA` live on the same four
cores is the first thing to re-measure under this stack — see
[Open measurement](#open-measurement-before-enabling-under-this-stack).

`aubiopitch` gives a dominant-pitch histogram, not a true chromatic key
(that needs chroma + template matching — Tier 2 territory). Treat
`key_approx` as a rough tonal-centre hint, not a KS-accurate key.

## Tier 2 — heavy (MFCC + chroma + self-similarity structure)

*Measured on this unit*, same caveat (under the older stack; no
`essentia-utils` package exists for this arch, so the real comparison was
a librosa-equivalent pipeline — MFCC, chroma, beat, self-similarity):

| Test | Result |
|---|---|
| Full pipeline, 1 A53, pinned | 372 s (6 min 17 s) |
| Full pipeline, free scheduler (picked an A72 on its own) | 80 s |
| 4 in parallel on the A53s | **OOM killer fired** — 2 of 4 processes killed; the live `rbp` process (under the older stack) was itself a candidate victim |

**This is the load-bearing finding and it does not depend on which stack
is running:** heavy multi-descriptor analysis is a genuine **memory**
hazard, not just a CPU one. Each process used 1.1–1.4 GiB RSS against the
unit's 3.8 GiB RAM + 1.9 GiB swap. That margin does not improve by
switching stacks, and `gui_task`/`JuceALSA`'s own RSS under this stack has
not been measured against it either — treat Tier 2 as equally dangerous
here until proven otherwise, not safer because the pinning moved.

## Design

```
USB1/USB2 mount ──► inotify watcher ──► FIFO queue (Tier 1 workers)
                                              │
                                              ▼
                              /tmp/rx3-analysis/<id>.json
                              { bpm, key_approx, analyzer, analyzed_at }

Tier 2: NEVER auto-triggered. Manual/operator-invoked only, one job at a
time, hard memory ceiling, OOM-preferred-victim, free CPU scheduling.
```

The watcher reads the filesystem only — it is **not** wired into
`usb-watch.sh`'s mount/FIFO flow (that script belongs to the sibling repo,
`$OLD/scripts/rb/device/usb-watch.sh`, and its FIFO protocol with rbp is
exactly the kind of thing that breaks badly on an unrecognised line — see
the rbpi4b `docs/18-prodjlink.md` for the general shape of that risk on a
different FIFO). This stays a separate watcher with no FIFO write of its
own, on either stack.

### Safeguards

* Tier 1 workers: `Nice=10`. Pin them explicitly to the A53 set
  (`taskset -c 0-3`, matching `RX3_AUDIO_CPU`'s own default) rather than
  leaving them unpinned — unpinned is how `gui_task` ended up wandering
  onto the A53s and missing vblank in the first place (see `00-port.md`,
  "gui_task on the A72s"). The same lesson applies here: pin explicitly or
  inherit someone else's bug.
* Tier 2 job: `MemoryMax=1536M`, `OOMScoreAdjust=900` (killed first, by
  design, never by luck), `Nice=19`, no CPU pinning (the free scheduler
  picked the faster core on its own, measured), serialised to one job at a
  time via `flock`.
* Both `rx3-rbpi4b-rk3399.service` and `rx3-rk3399-ddj400.service` get an
  `OOMScoreAdjust=-900` drop-in — only one runs at a time, but which one is
  live can change, and the kernel's choice should be deliberate either way.
* Tier 2 is opt-in, per track, operator-triggered — never best-effort
  background work, and never while a deck is playing.

## Files

```
scripts/rk3399/music-analyze.sh         Tier 1: one file -> BPM + approx key JSON
scripts/rk3399/music-analyze-watch.sh   inotify watcher -> FIFO queue feeder
scripts/rk3399/music-analyze-worker.sh  Tier-1 FIFO worker, pinned to the A53 set
scripts/rk3399/music-analyze-heavy.sh   Tier 2: manual, serialised, memory-capped
scripts/rk3399/heavy_analysis.py        librosa feature extraction for Tier 2
scripts/rk3399/systemd/music-analyze-watch.service
scripts/rk3399/systemd/music-analyzer@.service        (template, instantiate N times)
scripts/rk3399/systemd/music-analyze-heavy@.service    (never enabled; started by hand)
scripts/rk3399/systemd/rx3-rbpi4b-rk3399.service.d/oom-protect.conf
scripts/rk3399/systemd/rx3-rk3399-ddj400.service.d/oom-protect.conf
```

## Open measurement, before enabling under this stack

1. Re-run the Tier 1 benchmark with `rx3-rbpi4b-rk3399.service` actually
   running and a track actually playing (`JuceALSA` live on cpu0-3), not
   against a quiet unit. The "no slowdown in parallel" result may not hold
   once it is sharing those cores with the audio engine it was pinned
   there specifically to protect.
2. Check free RAM under this stack (`free -m`, with the stack up) before
   ever trying Tier 2 once — `gui_task` and the `toolkit` core's own
   footprint under `rx3-rbpi4b-rk3399.service` have not been measured.
3. Confirm `aubio-tools` and `inotify-tools` are installed on this exact
   unit (they were, for the benchmark) and still are.
