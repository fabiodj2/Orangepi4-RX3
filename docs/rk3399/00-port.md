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

**rbp itself paces at 30 fps**: its render thread `nanosleep`s to a ~33 ms frame
(strace: sleep ~9.3 ms, then FBIO_WAITFORVSYNC, which is ENOTTY here and returns at
once). So the panel now gets a steady 30 fps, every frame held exactly two vblanks;
before, `skip=2` halved that to ~15 and deferred I/O made it irregular.

Build note: the module is linked with `LIBS="-ldl -lpthread"` (a bare `LIBS=`
drops libtool's `-lpthread`), and then the soname fix-up — skipping it gives
`libdirect-1.4.so.6: cannot open shared object file` and rbp exits at
`DS_HW_Glib3_DFB.c <293>`.

Back-out without rebuilding: `RB_KMS_SCANOUT=0 RB_DFB_PRESENT_SKIP=` (the old
route, measured above). The pre-change binaries are kept beside the new ones as
`*.pre-kms` in the runtime.

## Open

* rbp's 30 fps frame limiter: 60 would need its frame period found in the binary.
* Overlay bands vs. KMS: the effect picker and the drawers can now get planes;
  BEAT FX / CUE POINT mix still to be re-checked with them.
* Touch zones, DDJ-400 controls, audio, STEMS: to be verified by hand.
