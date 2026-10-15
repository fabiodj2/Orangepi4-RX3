# RK3399 port — rbpi4b on the Orange Pi 4 LTS

Branch `port/rk3399` of this rbpi4b clone runs rbp v1.20 on the Orange Pi 4 LTS
(RK3399, Armbian 6.18, HDMI 1920×1080×32 `rockchipdrmfb`, USB touch monitor
`wch.cn TouchScreen`, DDJ-400). It takes **display and touch from rbpi4b** and
**controller, audio and STEMS from `~/RX3-RK3399-DDJ400`** (the earlier port,
still intact and still the rollback).

| Layer | From | Notes |
|---|---|---|
| DirectFB 1.4.16 + `fbdev` driver | rbpi4b `directfb-full.diff` | present path auto-picks `scale`: 1728×1080, 96 px bars |
| `fbshim.so` | rbpi4b | fb lies, in-process tsc2007 + `pointsrc`, touch zones |
| `memshim.so` | DDJ400 | the RTC `mmap(MAP_SHARED, fd=-1)` redirect, after fbshim |
| `keyshim.so` + `ddj400-bridge` | DDJ400 | the DDJ-400 map, verified live there |
| `audioshim.so`, `netshim.so`, `librx3_core.so` | DDJ400 | DDJ-400 USB audio, STEMS |
| `usb-watch.sh` | DDJ400 | pointed here with `RX3_RUNTIME` |

Launcher: `sudo sh scripts/rk3399/start-rk3399.sh start|stop|status|run`.
Rollback to the previous stack: `sudo sh ~/RX3-RK3399-DDJ400/scripts/rb/start-rk3399-ddj400.sh restart`.

## First light (2026-10-10)

```
PRESENT: mode=scale angle=0 real_fb=1920x1080 pitch=7680 bpp=32 pages=1 pan=0
         (logical 1280x800) scale=1728x1080 dx=96 dy=0 fit=fit skip=2
pointsrc: absolute device /dev/input/event2 name='wch.cn TouchScreen'
pointsrc: raw range x=[0..16383] y=[0..9599]
USB1 database READY; keyshim fifo open ok
```

`skip=2` means the driver's pixel budget presents every other frame (30 Hz on
the panel) for a 565→8888 scale at 1728×1080. That is rbpi4b's default budget,
never measured at 32 bpp on their side; tune `DFB_PRESENT_PX_BUDGET` or move to
32 bpp (`patch-rbp-depth.py` + `FB_LIE_BPP=32`, which makes it 4→4, no convert).

## Runtime

`work/runtime/rx3` is an `rsync -aHx` copy of the DDJ400 chroot (no `tmp/*`),
with `root/pdj/RX3_STEMS` **hardlinked** (`cp -al`, 1.3 GB, zero extra space;
the two runtimes share those inodes). Changed in the copy:

* `usr/lib/libdirect{,fb}-1.4.so.0.0.0`, `libfusion-1.4.so.0.0.0`,
  `usr/lib/directfb-1.4-6/{systems,wm,inputdrivers}/*.so` — the rbpi4b build;
* `usr/lib/rbpi4b/fbshim.so` — beside, not over, the DDJ400 `fbshim.so`;
* `usr/etc/directfbrc` — **this** build reads `/usr/etc` (prefix `/usr`), not
  `/etc`. Without `module-dir=/usr/lib/directfb-1.4-6` no module loads
  (`dfb_system_lookup ret=17`, `opendir /lib/directfb-1.4-6 failed`). The
  original is kept as `directfbrc.ddj400-orig`.

## Building on the Orange Pi (Debian GCC 14, glibc 2.41 cross headers)

The chroot is glibc 2.13. Three host-header behaviours produce objects that
build fine and then fail inside the chroot; all three are handled by a
force-included header (`-include …/glibc213-compat.h`):

| Symbol | Why | Fix |
|---|---|---|
| `__isoc23_strtol/strtoul/sscanf` | glibc ≥ 2.38 redirects under `_GNU_SOURCE` | `__GLIBC_USE_C23_STRTOL 0` after `features.h` |
| `__clock_gettime64`, `__nanosleep64`, … | Debian defaults to 64-bit `time_t` | `-U_TIME_BITS -D_TIME_BITS=32 -U_FILE_OFFSET_BITS -D_FILE_OFFSET_BITS=32` |
| `fstat` | real `fstat@GLIBC_2.33` | shims: inline `syscall(SYS_fstat)`; DirectFB: the diff's `compat_shim.c` |

The header defines `_GNU_SOURCE` before `features.h`, so the GNU extensions
(`PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP`, `EAI_NODATA`) stay visible.

Shims:

```sh
cd scripts/shims && make clean
make -j6 RX3=$PWD/../../work/runtime/rx3 CFLAGS="-O2 -march=armv5t -mfloat-abi=soft \
  -fno-stack-protector -fPIC -Wall -Wextra -Wno-unused-parameter \
  -U_TIME_BITS -D_TIME_BITS=32 -U_FILE_OFFSET_BITS -D_FILE_OFFSET_BITS=32 \
  -include $PWD/glibc213-compat.h" all
```

A `make` that failed half-way leaves `.o` files built **without** these flags,
and the next `make` reuses them: `make clean` first (this is how
`__clock_gettime64` got into the first build).

DirectFB (tools/build-directfb/README.md steps 0-3 unchanged: DirectFB
`363739298`, flux `10ad2eb`, fluxcomp built natively), then:

```sh
F="-march=armv5t -mfloat-abi=soft --sysroot=$RX3 -O2 -std=gnu11 \
   -U_TIME_BITS -D_TIME_BITS=32 -U_FILE_OFFSET_BITS -D_FILE_OFFSET_BITS=32 \
   -include $REPO/tools/build-directfb/glibc213-compat.h \
   -Wno-error=int-conversion -Wno-error=incompatible-pointer-types \
   -Wno-error=implicit-function-declaration -Wno-error=implicit-int \
   -Wno-error=return-mismatch"
for d in lib src systems wm inputdrivers; do make -j6 -C $d CFLAGS="$F"; done
```

GCC 14 makes those legacy-C diagnostics errors. `interfaces/` and `tools/` are
skipped: the image providers need libpng/libjpeg that the chroot lacks, and the
DDJ400 runtime never had them either. Then the README's soname fix-up.

Checks that passed: every shim and DirectFB object references GLIBC_2.4/2.7
only, no `__isoc23`/`*64`/`fstat`; every undefined symbol of `fbshim.so`
resolves in the chroot's libc/libpthread/libm/libdl/librt; every address in
`scripts/shims/rbp_abi.h` checked lands on the same-named symbol in our rbp.

## The display path: KMS scanout, not /dev/fb0 (2026-10-13)

**rockchip-drm's `/dev/fb0` is not the screen's memory.** Its framebuffer has a
`dirty()` hook, so fbdev emulation hands out a vmalloc shadow with deferred I/O:
each page written faults, and a kernel worker copies the damage to the real
buffer on its own clock. Measured from rbp: **35,770 minor faults/s**, the fb0
mapping all `Private_Dirty` pages with no `VM_PFNMAP`, a 565→8888 scale at
15.6 ms (the Pi: 3.3 ms), `skip=2`, torn frames and flicker on every touch.
Pinning rbp to the A72s changed nothing (17.7 ms): it was never the CPU.

The fix moves the picture onto KMS and leaves fb0 out of it:

| Piece | Where | What |
|---|---|---|
| `rx3_kms_scanout()` | `scripts/shims/drmband.c` | dumb buffer of the fb's geometry on the **primary** plane of the live CRTC, through the same device holder (and DRM master) the overlay bands use; `KMS_SCANOUT=1` |
| `rx3_kms_vsync()` | same | `DRM_IOCTL_WAIT_VBLANK` on that CRTC: the pace the fb route got from the pan pacer |
| page painters | `menu_draw.c`, `fb_cursor.c` | draw on the scanout instead of fb0 when there is one |
| present | `tools/build-directfb/rk3399-kms-scanout.diff` | `fbdev.c` finds both with `dlsym`, waits for vblank, presents into the scanout, does not pan; plus a fast 565→8888 scaler (64 K LUT, per-geometry column table, each source row converted once and `memcpy`'d out to write-combined memory). `DFB_PRESENT_SLOW=1` = the old walk |
| card | `DRM_BAND_CARD` | `/dev/dri/card0` here (rockchip-drm); card1 is panfrost, no CRTC. The Pi's card1 stays the default |

Result, same unit: `PRESENT: 300 presented / 300 flips, avg 5.00 ms, worst
14.54 ms, skip 1`; 0 faults/s; gui_task 54% → 37%; no flicker. Overlay bands
(menu `plane 40`) stay above the picture (`plane 33`, primary).

**rbp's own limiter, and 60 fps.** `DS_HW_UpdateScreen` (VA `0x1a6844`) does
`if (now - t_prev_flip_returned < 16000) usleep(16000 - elapsed)` before each flip
-- 60 fps on the RX3, where a flip is a free pan. Here the flip waits for vblank and
presents (~13.5 ms), the two pacers add up, and the UI ran at 30 fps (found by
sampling gui_task's stack from `/proc/<pid>/task/<tid>/syscall` + `/proc/<pid>/mem`
while it slept: `usleep` ← `DS_HW_UpdateScreen+0x3f8` ← `ui_com_draw`).
`RBP_FRAME_US=1000` makes the shim rewrite the immediate in both instructions at
load (`fb_shim.c rbp_frame_limiter_patch`; `e3530dfa`/`e2630dfa` →
`e3530ffa`/`e2630ffa`, only if the words match v1.20), leaving vblank as the only
pace. The binary on disk is untouched.

**gui_task on the A72s.** The audioshim answers rbp's affinity/priority calls
with success and does nothing, so every rbp thread floats over cpus 0-5 at
SCHED_OTHER; a frame drawn on an A53 (cpu0-3) misses vblank. Reading the scanout
through DRM GETFB/MAP_DUMB showed the stutter: 16.7 ms frames with bursts of 33 ms,
and a waveform step that alternates 3/4 px (rbp's own 2/3 px at 1280, not ours).
The launcher pins gui_task to `RX3_GUI_CPUS` (default `4,5`). 30 s each, rbp's
`FB_VSYNC_RATE` counter: unpinned avg 57.4 fps, worst second 42, 9 seconds under
57; pinned avg 59.9, worst 58, none under 57.

Measured with `FB_VSYNC_RATE=1`: 60.0/s in most seconds, 58-60 in the rest;
`avg 4.21 ms, worst 15.34 ms, skip 1`; gui_task ~72% (twice the frames), system
~70% idle; no audio xruns.

Build note: the module is linked with `LIBS="-ldl -lpthread"` (a bare `LIBS=`
drops libtool's `-lpthread`), and then the soname fix-up — skipping it gives
`libdirect-1.4.so.6: cannot open shared object file` and rbp exits at
`DS_HW_Glib3_DFB.c <293>`.

Back-out without rebuilding: `RB_KMS_SCANOUT=0 RB_DFB_PRESENT_SKIP=` (the old
route, measured above). The pre-change binaries are kept beside the new ones as
`*.pre-kms` in the runtime.

## 1280x800 on the wire (2026-10-13): no scaling on our side

The monitor (EDID: 1920x1080, 1280x720, 1024x768, 800x600, 720x576/480) does not
list 1280x800, but it takes one. `KMS_MODE=1280x800` (launcher default) makes the
shim program it on the CRTC with `SETCRTC` and scan out a 1280x800 buffer; the
driver sees that geometry (`KMS_MODE` overrides `real_var` in `fbdev.c`) and picks
`mode=convert`, a 1:1 565→8888 copy. The monitor fits it to 1728x1080 with 96 px
bars itself -- the same rectangle the scaled route drew.

* **Pixel clock.** CVT 1280x800@60 (83.5 MHz) is refused with EINVAL:
  dw-hdmi-rockchip rejects a clock its PLL cannot hit within 0.1%. 74.25 MHz is
  one it can (this monitor's 1280x720 runs on it), so the default timing is
  CVT-RB porches with htotal stretched to 1504: `1280 1328 1360 1504 / 800 803
  809 823, +h -v`, 74.25e6/(1504*823) = 59.99 Hz. `KMS_MODE_RB=1` (71 MHz) and
  `0` (83.5) stay selectable.
* **Touch** keeps `POINT_ABS_FIT=1`: the panel is physical, the picture on it is
  still 1728x1080 at 96 (fit computed from fb0's 1920x1080).
* **Page painters** (menu band, cursor) take the mode's size from
  `rx3_kms_mode_geom()`, so their picture is 1280x800 at 0,0 and the band is a
  1280-wide plane. Without that the band was laid out 1728 wide at x=96.
* **Ordering.** With `KMS_MODE` set, only a caller asking for exactly that size
  creates the scanout: the cursor/menu used to get there first with 1920x1080.

Measured: 60.0 fps, worst second 59.9 (scaled route: worst 58). Picture and
touch confirmed by the operator. `RB_KMS_MODE=` brings back the scaled route.

## Garbage column right of the deck frames (2026-10-13): RESOLVED

**Symptom.** A 1-px column of random coloured pixels just right of the
selected-deck frame: x=183 beside the DECK 1/2 info panels, x=631 and x=1271
beside the lower deck panels. Its colours changed every time the selection
moved or the screen was redrawn, and once the selection moved away the column
stayed on screen for good (rbp repaints the panel only up to x=182).

**Ruled out.** The present path: `mode=convert` copies the whole frame 1:1, and
the KMS buffer showed the same pixels as rbp's surface. Waveform scrolling: 25 s
of playback recorded frame by frame, the column never changed.

**Found with** `tools/blittrace` (LD_PRELOAD'ed for one run via
`RX3_EXTRA_PRELOAD=/usr/lib/rbpi4b/blittrace.so`; interposes the `dfb_gfxcard_*`
calls libdirectfb makes through its own PLT and logs any operation whose right
edge lands on the suspect columns). The frame's right edge is
`blit dst=1280x800 d=182,57 1x206 src=(1x206) bf=8` -- a 1-px-wide image with
`DSBLIT_SRC_COLORKEY` -- and nothing else touches x=183.

**Cause.** DirectFB's software blitter, `src/gfx/generic/template_colorkey_16.h`
(`Bop_16_Kto_Aop` / `Bop_16_toK_Aop` and the 12/14/15-bit instances). `l` holds
pixel count + 1 for the `while (--l)` loops, but the aligned path split it into
pairs as if it were the count: `ceil(n/2)` pairs for odd n, or n/2 pairs plus the
"odd" tail for even n. Every colorkeyed blit whose source and destination were
both 4-byte aligned wrote one pixel past its right edge, taken from beyond the
source row. A 1x206 source at x=182 (even, aligned) hits it on every row.

**Fix.** `tools/build-directfb/rk3399-colorkey16-fix.diff`: `--l` before the pair
split in both functions. Host test of the template over lengths 1-11, both
alignments, both directions: original wrong in 88/176 cases, fixed 0/176. Built
with the exact `F=` flags above (`make -C src CFLAGS="$F"`); without the patch
that build is function-for-function identical to the installed library, with it
only the `Bop_*_Kto_Aop`/`Bop_*_toK_Aop` sizes change. Then the README's soname
fix-up (`--set-soname libdirectfb-1.4.so.0`, `--replace-needed` libdirect/
libfusion `.so.6`→`.so.0`). Installed as `usr/lib/libdirectfb-1.4.so.0.0.0`;
the previous one is `libdirectfb-1.4.so.0.0.0.pre-ckfix`.

**Verified.** Columns 183/631 empty after the restart (1271: one corner pixel of
the frame); operator switched deck selection and BROWSE and back: clean.

## BEAT FX page vs. STATUS pads on touch (2026-10-13): RESOLVED

**Symptom.** With the strip's STATUS | BEAT FX toggle on BEAT FX, taps on the BEAT FX
BANK (FLANGER/ECHO/VINYL BRAKE/DELAY) or the X-PAD flipped the strip back to the pads,
changed a deck's pad mode (HOT CUE -> GATE CUE -> BEAT LOOP -> SLIP LOOP -> BEAT JUMP),
or fired a hot cue.

**Two causes, measured with a monitor that logged every raw touch (mapped to logical
x,y), the page on the glass, both pad modes and pointsrc's log, with timestamps:**

1. *The shim's HOT CUE row (hc_zone) was gated on the performance screen only.* Its
   sixteen cells lie under the bank buttons, so on the BEAT FX page it took those taps:
   rbp never saw them, and a registered cue under the finger fired (`hot cue deck1 pad4
   -> key 0x411a` for a tap on DELAY; the pad key also makes rbp show the pads).
2. *rbp's own pad-mode hit zone stays live under the BEAT FX page.* On STATUS a tap on
   the caption row changes the deck's pad mode (rbp's behaviour, kept). On BEAT FX the
   same zone answers under the "BEAT FX BANK / X-PAD" caption and the top edge of the
   buttons: taps at y 508 and 517 flipped the strip and moved deck 2's pad mode; from
   y 529 down never.

**Fix (fbshim.so, pointsrc.c):**

* `strip_page()` reads WHICH PAGE IS DRAWN off the KMS scanout (`rx3_kms_peek()`, new
  in drmband.c): eight probes on the caption row, y 500/501, are rbp's grey hatch on
  the BEAT FX page and black on the pads, chosen from 245 captures (3x3 neighbourhood
  stable in every frame, so the scaled route reads the same). 6 of 8 decide, else
  unknown.
* The HOT CUE row acts only when `strip_page() == 0`; unknown stands it down.
* On the BEAT FX page a report with y 495..528 is moved to y 536, the middle row of the
  same button (`strip_bfx_y()`), so the press lands on the button aimed at and never
  in rbp's pad-mode zone.

**Not rbp memory.** IsBeatFxSelected's word and setBeatFxSelected's lag the strip, and
the best .bss-diff candidate (a Winscape window field, 1770/1771 over 90 s) later read
STATUS under a drawn BEAT FX page and let cue B fire for two taps on ECHO. rbp_abi.h
keeps that record.

**Follow-up the same day -- rbp's STATUS controls are live under the BEAT FX page,
so the page is driven from the shim, not by moving taps around:**

* *STATUS page is dead to touch* (`POINT_STATUS_TOUCH`, default 0, the operator's
  call): a gesture that starts on the strip (y 486..575; rbp's caption zone answers
  from ~492, above the drawn 495) while the pads are drawn is swallowed whole -- no
  HOT CUE row, no pad-mode change. `POINT_STATUS_TOUCH=1` restores both.
* *The BEAT FX BANK is shim-driven*: on the BEAT FX page a press on any of the four
  slots is withheld from rbp and the shim selects the slot's effect itself --
  `UiGetMixBeatFXBank(slot)` @0xfe664 (rbp's own read in PlayInfoDataUpdate) gives the
  switch position, `K_BFXTYPE` carries it (the picker's path). Needed because a tap
  rbp sees on FLANGER also fires deck 1's hot cue A from the hidden pad under it, and
  no y in that box misses the pad (moving to the gutter between the pad rows, y 542,
  still fired it with both decks paused). Off in the trash icon's delete mode.
* *Other BEAT FX taps on the strip* (X-PAD, trash) go to rbp at y 542.
* *The drawers' entry columns* (x < 56, x > ~1224) used to replay a tap to rbp
  unmoved, reaching the hidden caption from the strip's corners on both pages; a
  replay that lands on the strip now obeys the same rules (dropped on STATUS, moved
  on BEAT FX).

Verified: STATUS strip and its corners ignored; all four bank slots select their
effect with the decks paused and no waveform jump. BEAT FX page corners after the
last change: not yet re-tested on the glass.

**Verified by the operator:** 7 toggles, all four bank buttons (top edge included),
29 X-PAD taps -- no flip to pads, no pad-mode change, no cue; on STATUS the pad hot
cue fires. Previous shim: `fbshim.so.pre-stripgate`.

## Service (2026-10-13)

`sudo sh scripts/rk3399/install-service.sh` installs and enables
`rx3-rbpi4b-rk3399.service` (unit in `scripts/rk3399/systemd/`), disables the old
`rx3-rk3399-ddj400.service` and starts the new one; `... install-service.sh uninstall`
removes it. The two units `Conflicts=`, so starting either stops the other.

* `ExecStart` is `start-rk3399.sh run`: it supervises and exits 1 when rbp dies, so
  `Restart=on-failure` brings the whole stack back 10 s later (tested: rbp SIGKILLed,
  NRestarts=1, back up). `systemctl stop` sends SIGTERM to the launcher only
  (`KillMode=mixed`); its trap runs `stop_all`, which restores the console.
* Output goes to `work/log/launcher-run.log` (truncated at each start), not the
  journal; systemd's own lines are in `journalctl -u rx3-rbpi4b-rk3399`.
* The DDJ-400 interface gesture (ddj400-bridge writes
  `/run/rx3-interface-toggle.request`, the old project's `rx3-interface-toggle.path`
  fires) now runs `scripts/rk3399/toggle-interface.sh` through a drop-in
  (`rx3-interface-toggle.service.d/rbpi4b.conf`). OFF stops rbp only and leaves
  `/run/rx3-interface-off`; the supervisor sees the marker and waits instead of
  exiting, so ddj400-bridge stays up to hear the gesture again. ON clears the marker
  and restarts the service. A start always clears a stale marker.
* Manual runs: `sudo systemctl restart|stop|start rx3-rbpi4b-rk3399`. A diagnostic run
  with extra env (e.g. `RX3_EXTRA_PRELOAD`) is `systemctl stop` first, then the
  launcher by hand.

## rx3-toolkit core (2026-10-13; the default since 2026-10-14)

The KEY/STEMS panel had never appeared on this stack: `librx3_core.so` (legacy core,
RX3-RK3399-DDJ400 overlay `key-stems-0cb402e0`) and keyshim both hook
`solveCoordToKey` @0x2dc104, keyshim initialised first and the core logged
`rejected: unexpected solveCoordToKey prologue` and dropped its UI (the old stack
too, since keyshim gained its pad-touch hook). Fix: the core is now LAST in
`LD_PRELOAD` (preload constructors run in reverse order), so it hooks first and
keyshim's pad-touch hook steps aside -- the hook that had been firing hot cues and
pad-mode changes from the strip under the BEAT FX page. The shim's strip rules now
also require the STATUS | BEAT FX toggle half to be lit (0x7b7d7b at x 1140 /
1225, y 445/470): the KEY and STEMS pages share BEAT FX's hatch.

The operator then asked for the current rx3-toolkit (github.com/Tratosca/rx3-toolkit,
MPL-2.0): STEMS panel with levels and Slip Loop pads 5/6/7, Key Shift/Sync/Match,
Beat Jump 32, Samples, Browse columns, theme/logo. Staged beside the legacy core:

* Build: `make hook` in an export of `toolkit/main` (commit in
  `work/toolkit/TOOLKIT_COMMIT`), board clang 19 + lld. Imports: libc/pthread only.
* Compatibility (our rbp `d111195...` vs stock 1.20 `3513d34...`, 49 differing
  ranges): Beat Jump 32 and no-quantize are already in our rbp byte for byte; of 70
  addresses the toolkit hooks, only the image-table word @0x1d192c (legacy value
  `0a3601e3`, toolkit `a53601e3`) and solveCoordToKey (keyshim, handled by order)
  overlap.
* `scripts/rk3399/toolkit/stage-toolkit.py` writes `root/pdj/toolkit/`
  (librx3_core.so, rbp with that one word, panel art via `RX3_TAB_*`), the glyph
  atlases at their fixed paths, and `work/toolkit/toolkit.env` (each module.sh's
  exports).
* Stems: the toolkit reads `RX3STM1` role files or `RX3PKG2`, not the legacy
  `RX3STM2` (64-byte header + DRUMS + VOCAL). `stm2-to-stm1.py` splits each into
  `<base>.rx3stem` (vocal) + `<base>.rx3drums` without re-separating, into
  `root/pdj/RX3_STEMS_TK` (12/12 tracks, 1.3 GB; originals untouched).
* Launcher: `RX3_CORE=toolkit` (the default since 2026-10-14; `RX3_CORE=legacy` goes
  back) preloads `/root/pdj/toolkit/librx3_core.so` and `rbpcomm.so`, sources
  `toolkit.env`, bind-mounts the rbp variant over `/root/pdj/rbp`; `stop_all` always
  unmounts it.
* The core was silent at first: it only starts when `/proc/self/comm` is `rbp`, and
  this port runs rbp through the chroot's loader (`ld-linux.so.3`). `rbpcomm.so`
  (`scripts/shims/rbpcomm.c`, last in `LD_PRELOAD` so its constructor runs first)
  renames the thread to `rbp` when the command line is exactly that loader + rbp.
  Its log, `/tmp/rx3-stems.log`, stops at 128 KB (the Key Shift statistics fill it in
  minutes): move it aside to read a new session.
* DDJ-400: see the next section. Lost with the toolkit core: SHIFT+pad hot-cue delete
  (a legacy-core command).

## DDJ-400 STEMS and KEY pages on the toolkit core (2026-10-14)

Operator's map, verified on the unit (sound, screen and LEDs together):

| DDJ-400 | Opens on screen | Pad 1 | Pad 2 | Pad 3 |
|---|---|---|---|---|
| SHIFT+BEAT JUMP (PAD FX 2, note 0x6B) | STEMS panel | INST | VOCAL | DRUMS |
| SHIFT+SAMPLER (KEY SHIFT, note 0x6F) | KEY panel | -1 | the track's own key | +1 |

LEDs: STEMS pad lit = that stem playing; KEY pad 1 lit below the track's key, pad 2
lit at it, pad 3 above. SAMPLER pads 5-7 stay native SLIP LOOP / RELEASE FX.

The toolkit core takes no controller command (its stems answer Slip Loop pads 5-7,
which switch the deck to SLIP LOOP, and a pad-mode key closes its panel), so:

* `ddj400-bridge` (RX3-RK3399-DDJ400), with `RX3_STEMS_PADS=touch` (exported by the
  launcher in toolkit mode), writes its `stems_pad_command_packet` to the FIFO
  `/tmp/rx3-stems-touch.fifo`: control 0x20 open STEMS, 0x21 open KEY, 0-2 a stem in
  screen order, 0x30-0x32 a KEY stepper part. No pad-mode key is sent.
* fbshim's pointsrc (`stems_thread`) reads it. The page buttons tap the toolkit's
  tab (KEY x 1135, STEMS x 1225, y 407) only when it is not already lit (a tap on a
  selected tab closes it; lit = grey 0x7b7d7b at the tab's corners). The pads do not
  tap: a stem is flipped as the toolkit's `stems_toggle` does (atomic 0 <-> 100 of
  the role's 7-bit level in `levels`, under its pad gate: status 2, reader, armed,
  role available), KEY calls the toolkit's own `keyshift_fire(deck, 0, part)`.
  Taps were tried first and were late under load, needed the panel open and the glass
  free, and a colour test for the DRUMS cell dropped DRUMS whenever VOCAL was off.
* The toolkit's symbols are found by name in the loaded library's `.symtab`
  (`dl_iterate_phdr` + the file's section table; the build is not stripped), each
  checked against its expected size: `stems_decks` (2 x 0x4e0), `keyshift_decks`
  (2 x 0x50), `keyshift_fire`, `performance_refresh_pending`, `pad_client_count`,
  `pad_clients`, `slip_light`. A rebuilt core moves nothing here; a changed layout
  turns the feature off with a line in pointsrc.log.
* Screen: the toolkit's watcher asks for a panel repaint only once
  `performance_window_ready`, which here never comes up (the image it waits for,
  0x159a, is never drawn), so a pad change played but the panel kept the old state.
  The shim raises `performance_refresh_pending` (what a touch raises) whenever it
  sees the stems or key state change.
* LEDs, back the other way: the shim writes `/tmp/rx3-stems-pad-state.bin` (the
  legacy core's file, "SMTS", read by the bridge's `stems_pad_state_tick`) and
  `/tmp/rx3-key-pad-state.bin` ("KEYS", `key_pad_state_tick`, KEY SHIFT notes
  0x70-0x72) from the toolkit's state. The DDJ darkens a pressed pad by itself, so
  the bridge resends a page's LEDs after every pad edge (else pad 2 at the track's own
  key went dark when pressed).
* SAMPLER pads 5-7: the toolkit's stems client on its onKey_Pad hook (the hook's only
  client) and its Slip Loop LED colours are retired by the shim
  (`pad_client_count` 0, `slip_light` 0, the toolkit's own "no client" paths), only
  when that one client is the stems one.

Rollback: `RX3_CORE=legacy`; shim backups `fbshim.so.pre-stemstouch`, bridge backup
`ddj400-bridge.pre-stemsslip` (RX3-RK3399-DDJ400 work/build/host).

## Audio clicks: CPU governor (2026-10-14)

Clicks while changing KEY/STEMS and while browsing. rbp's audio runs on `JuceALSA`
with 2 periods of 64 frames (2.9 ms of buffer to the DDJ-400); audioshim logged no
xrun, while `JuceALSA` and the `FGPR` decoders waited 5-18 ms for a CPU in each
0.2 s window. Measured on the unit:

* Pinning `JuceALSA` alone to cpu5 and the rest of rbp to the A53s made it worse
  (constant clicks: the decoders need the A72s). Reverted at once.
* SCHED_FIFO for `JuceALSA` is refused (EPERM, even as root): the kernel has
  `CONFIG_RT_GROUP_SCHED=y` and the cgroup v2 `cpu` controller is on, so a task in a
  service cgroup gets no RT runtime. Needs `rt_group_sched=0` on the kernel command
  line (next step, not applied yet).
* `performance` governor on both clusters: clicks drastically fewer, controller
  commands answer at once. The launcher now sets it at start
  (`RX3_CPU_GOVERNOR`, default `performance`, empty = leave it) and restores the saved
  governor (`/run/rx3-rbpi4b-governor`) on stop.

## Open

* Remaining audio clicks: RT priority for `JuceALSA` (`rt_group_sched=0`, reboot).
* Touch zones on the BEAT FX page corners, a full hot-cue pass: to be verified by hand.
