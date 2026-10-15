# rbpi4b — `master`: XDJ-RX3 player on the Orange Pi 4 LTS (RK3399) + DDJ-400

Credit to: [![Instagram: @i.erhan.es](https://img.shields.io/badge/Instagram-%40i.erhan.es-E4405F?logo=instagram&logoColor=white)](https://instagram.com/i.erhan.es) for the original base of this code.

**Run the Pioneer DJ XDJ-RX3 *rekordbox* standalone player on an Orange Pi 4 LTS
(Rockchip RK3399), driven by a Pioneer DDJ-400, with a USB touch monitor.**

This branch runs the ARM32 `rekordbox` player application (`rbp`) extracted from
**XDJ-RX3 firmware v1.20** on an **RK3399** board. It takes the display and touch
stack of rbpi4b (the Raspberry Pi 4B port, still on the `main` branch) and the
controller, audio and STEMS stack of the companion **RX3-RK3399-DDJ400** project,
and adds what the RK3399 needed: a KMS scanout instead of the deferred-I/O
framebuffer, a native 1280×800 HDMI mode, a fixed DirectFB blitter, and a touch
layer that knows which page of rbp's UI is on the glass.

> This is an **interoperability / preservation** project. It contains **no
> Pioneer/AlphaTheta firmware, no `rbp` binary and no rekordbox content.** You
> supply your own extracted assets. See [NOTICE.md](NOTICE.md).

The full engineering record — every measurement, decision and rollback — is
**[docs/rk3399/00-port.md](docs/rk3399/00-port.md)**.

---

## Specifications

| | |
|---|---|
| Board | Orange Pi 4 LTS — RK3399 (2× Cortex-A72 @ 1.8 GHz + 4× Cortex-A53 @ 1.4 GHz), 4 GB |
| OS | Armbian (Debian 13), kernel 6.18 `rockchip64`, aarch64 with 32-bit ARM support |
| Player | `rbp` from XDJ-RX3 firmware v1.20, in a soft-float glibc-2.13 chroot |
| Display | HDMI, **1280×800 @ 59.99 Hz** sent to the monitor (custom 74.25 MHz timing), presented 1:1 from rbp's 1280×800 surface onto a KMS dumb buffer on the primary plane — **60 fps**, vblank-paced |
| Touch | USB touch monitor (`wch.cn TouchScreen`, absolute), fitted to the picture the monitor shows (1728×1080 at x 96 on a 1920×1080 panel) |
| Controller | Pioneer **DDJ-400** over USB MIDI (`ddj400-bridge` + `keyshim.so`) |
| Audio | DDJ-400 USB audio (`audioshim.so`), master + headphones |
| KEY / STEMS | [rx3-toolkit](https://github.com/Tratosca/rx3-toolkit) core (`librx3_core.so`, MPL-2.0), staged by `scripts/rk3399/toolkit/` |
| CPU | `performance` governor while the player runs (fewer audio clicks) |
| Media | rekordbox-exported USB stick (`usb-watch.sh`, DeviceSQL import) |
| Service | systemd `rx3-rbpi4b-rk3399.service`, starts at boot, restarts the stack if rbp dies |

## What this branch adds over the Pi port

| Area | What | Why |
|---|---|---|
| Display path | `fbdev.c` presents into a KMS dumb buffer (`rx3_kms_scanout`) and waits for vblank (`rx3_kms_vsync`) instead of panning `/dev/fb0` | rockchip-drm's fb0 is a deferred-I/O shadow: ~35 000 page faults/s, flicker, ~15 fps |
| Native mode | `KMS_MODE=1280x800` programs the CRTC itself; DirectFB picks `mode=convert` (1:1 565→8888), no scaler | the monitor pillarboxes it on its own; CVT 83.5 MHz is refused by dw-hdmi, 74.25 MHz is not |
| Frame rate | rbp's own 16 ms frame limiter patched in memory (`RBP_FRAME_US=1000`); `gui_task` pinned to the A72 cores | 30 → 60 fps; worst second 59.9 |
| DirectFB fix | off-by-one in the 16-bit colorkey blitter (`tools/build-directfb/rk3399-colorkey16-fix.diff`) | wrote one pixel past every aligned colorkeyed blit — a garbage column beside the deck frames |
| Touch fit | `POINT_ABS_FIT=1` maps the panel onto the presented picture | taps land under the finger, edges included |
| Pad strip | the strip under the waveforms is read off the scanout (STATUS pads vs BEAT FX page); on **BEAT FX** the four bank slots are driven by the shim, on **STATUS** the strip ignores touch (`POINT_STATUS_TOUCH=1` to enable) | rbp keeps its STATUS pad cells and pad-mode caption live under the BEAT FX page, so a tap on FLANGER also fired a hot cue |
| Service | `scripts/rk3399/install-service.sh`, DDJ-400 interface on/off gesture kept working | autostart with crash recovery |
| KEY/STEMS core | the rx3-toolkit core (`RX3_CORE=toolkit`, default) with `rbpcomm.so` naming the process `rbp` | STEMS panel with levels, Key Shift/Sync, Browse columns |
| DDJ-400 STEMS/KEY | SHIFT+BEAT JUMP opens STEMS, pads 1-3 = INST/VOCAL/DRUMS; SHIFT+SAMPLER opens KEY, pads 1-3 = -1 / own key / +1; pad LEDs follow the state | the toolkit core takes no controller command: the bridge talks to the touch shim, which acts on the core's own state |
| Audio | `performance` cpufreq governor at start, restored on stop (`RX3_CPU_GOVERNOR`) | `ondemand` let the 2.9 ms audio path miss its time: clicks |

## State

**Verified on the unit by the operator (2026-10-13):** picture and 60 fps at
1280×800, no flicker; touch and the shim's menu band; DirectFB garbage column
gone; BEAT FX bank and X-PAD by touch without hot cues or pad-mode changes; STATUS
strip ignoring touch; systemd start and automatic restart after rbp is killed.

**Verified 2026-10-14:** the rx3-toolkit core as default; DDJ-400 STEMS page
(SHIFT+BEAT JUMP) and KEY page (SHIFT+SAMPLER) with sound, screen and pad LEDs in
step; SAMPLER pads 5-7 native; the `performance` governor cut the audio clicks
drastically.

**Open:** the remaining audio clicks (real-time priority for the audio thread needs
`rt_group_sched=0` on the kernel command line); the BEAT FX page's strip corners; a
full hot-cue pass. See the *Open* section of [docs/rk3399/00-port.md](docs/rk3399/00-port.md).

---

## Running it

The runtime chroot (`work/runtime/rx3`) and the companion project
(`../RX3-RK3399-DDJ400`, for `ddj400-bridge`, `usb-watch.sh` and the DDJ-400
shims) must be present; neither is in this repository.

```bash
# install, enable at boot and start
sudo sh scripts/rk3399/install-service.sh

# day to day
sudo systemctl restart rx3-rbpi4b-rk3399
sudo systemctl stop    rx3-rbpi4b-rk3399
journalctl -u rx3-rbpi4b-rk3399          # systemd's lines
less work/log/launcher-run.log           # the launcher's output

# by hand (stop the service first)
sudo sh scripts/rk3399/start-rk3399.sh start|stop|status|run

# remove the service
sudo sh scripts/rk3399/install-service.sh uninstall
```

Useful launcher knobs (environment, `RB_` / `RX3_` prefixed):

| Knob | Default | Effect |
|---|---|---|
| `RB_KMS_MODE` | `1280x800` | empty = old route: 1920×1080, scaled 1728×1080 with bars |
| `RB_KMS_SCANOUT` | `1` | `0` = present through `/dev/fb0` (slow, flickers) |
| `RB_RBP_FRAME_US` | `1000` | rbp's frame floor; `-1` leaves rbp's 16 ms limiter |
| `RX3_GUI_CPUS` | `4,5` | cores for `gui_task`; empty to leave it unpinned |
| `RX3_EXTRA_PRELOAD` | — | append diagnostic libraries (e.g. `tools/blittrace`) |
| `RX3_CORE` | `toolkit` | `legacy` = the RX3-RK3399-DDJ400 KEY/STEMS core |
| `RX3_CPU_GOVERNOR` | `performance` | cpufreq governor while running; empty = leave it |

## Building

Everything is built **on the board** with Debian's `arm-linux-gnueabi` GCC 14
against the chroot's glibc 2.13; the force-included
`tools/build-directfb/glibc213-compat.h` and `-D_TIME_BITS=32
-D_FILE_OFFSET_BITS=32` keep the objects loadable there. Recipes:

* shims (`scripts/shims`, `make … fbshim.so`) — [docs/rk3399/00-port.md](docs/rk3399/00-port.md#building-on-the-orange-pi-debian-gcc-14-glibc-241-cross-headers)
* DirectFB 1.4.16 — [tools/build-directfb/README.md](tools/build-directfb/README.md)
  plus `rk3399-kms-scanout.diff` and `rk3399-colorkey16-fix.diff`; build with the
  `F=` flags in the port doc and apply the README's soname fix-up.

## Repository layout (this branch's additions)

```
docs/rk3399/00-port.md            the port: measurements, decisions, resolved cases
scripts/rk3399/start-rk3399.sh    launcher (start|stop|status|run)
scripts/rk3399/install-service.sh systemd install / uninstall
scripts/rk3399/toggle-interface.sh DDJ-400 interface on/off gesture
scripts/rk3399/systemd/           the unit and the toggle drop-in
scripts/shims/drmband.c           KMS scanout, vblank, native mode, scanout peek
scripts/shims/pointsrc.c          touch: picture fit, pad strip rules, BEAT FX bank,
                                  DDJ-400 STEMS/KEY commands and LEDs (toolkit core)
scripts/shims/rbpcomm.c           names the player process "rbp" for the toolkit core
scripts/rk3399/toolkit/           stage the rx3-toolkit core; RX3STM2 -> RX3STM1 stems
tools/build-directfb/rk3399-*.diff DirectFB: KMS present, colorkey blit fix
tools/blittrace/                  diagnostic: log DirectFB blits on chosen columns
```

The Raspberry Pi 4B port, its docs (`docs/00`–`docs/18`) and its tutorial are
unchanged and remain the base of this branch; the `main` branch is the Pi version.

## What is *not* in this repo

* any `.UPD`, `.iso`, firmware image or `rbp` executable,
* rekordbox databases or media, any firmware key,
* the runtime chroot and built binaries (build them from source).

## Credits

* Pioneer DJ / AlphaTheta — XDJ-RX3 and its GPL source distribution.
* The rbpi4b Raspberry Pi 4B port and the Denon SC Live 4 / Prime GO work it grew from.
* Xunlong (Orange Pi), the Armbian project and the Rockchip mainline kernel developers.
* DirectFB, JUCE, ALSA and glibc maintainers.

See [NOTICE.md](NOTICE.md) for licensing details.
