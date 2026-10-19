# RK3399 tutorial — install from zero on an Orange Pi 4 LTS + DDJ-400

End-to-end: from a blank Orange Pi 4 LTS to `rbp` running under systemd,
controlled by a Pioneer DDJ-400. This repo is self-contained as of
2026-10-18 — it no longer needs the sibling `RX3-RK3399-DDJ400` project at
build or run time (see `docs/rk3399/00-port.md`'s consolidation note).

## How to read this doc

Following this project's own convention (`docs/README.md`, "How to read a
claim"): a step marked **verified** was actually run, on this exact unit,
during this port's history (not necessarily as part of the very first
bring-up — §3's chroot build, for instance, was run for real specifically
while writing this tutorial, 2026-10-18, using this unit's own already-
extracted firmware assets). A step without that marker combines pieces
that are each individually documented and verified *separately* but has
not been run start-to-finish as a single sequence. Treat those steps as
the best current understanding, not a transcript — if something doesn't
match, trust the referenced source doc over this one.

## 0. What you need

**Hardware:**
* Orange Pi 4 LTS (RK3399, 4 GB), a `≥`8 GB SD card or eMMC.
* An HDMI monitor, **or** a USB-C monitor that carries both video (DP Alt
  Mode) and touch over a single cable — both verified live on this unit
  (2026-10-18 for the single-cable USB-C setup; see
  `docs/rk3399/00-port.md`'s hardware note). Either way the touch digitizer
  shows up as the same evdev node regardless of which physical cable
  carries it.
* A USB touchscreen if not already combined into the monitor's own USB-C
  cable above (this port was built and verified against a
  `wch.cn TouchScreen`, 1920×1080 panel — a plain USB mouse also works,
  `rbp` is driven by any evdev pointer; see `docs/07-touch.md`).
* A Pioneer **DDJ-400**, connected **directly** to a USB port (not through a
  bus-powered hub — see `docs/rk3399/00-port.md`'s USB notes on the sibling
  project if you hit enumeration problems).

**External assets** (not in this repo — see `NOTICE.md`):
* An extracted XDJ-RX3 v1.20 firmware tree. Follow
  [docs/04-firmware-assets.md](../04-firmware-assets.md) to get
  `XDJRX3-rootfs/`, `XDJRX3/gui/`, `XDJRX3-gui/` (unpacked `gui.tar.gz`) and
  a patched `rbp-audio`. That doc is generic (board-independent) and does
  not need adapting for the RK3399.
* **Optional**, for the STEMS/KEY panel: a built `librx3_core.so` from
  `github.com/Tratosca/rx3-toolkit` (MPL-2.0, a genuinely separate project
  by a different author). This is gated separately from everything else in
  this tutorial — building or deploying code from that repo needs its own,
  explicit permission each time, not a one-time yes. Skip it for a first
  install (§8 below shows how) and come back to it later if you want the
  STEMS/KEY panel.

## 1. OS

**Verified**: Armbian (Debian 13 "trixie" userland), kernel 6.18
`rockchip64`, aarch64 with 32-bit ARM EL0 support (needed to run the
soft-float ARM32 chroot — see `docs/05-chroot.md`). Flash Armbian for the
Orange Pi 4 (not 4 LTS specifically if that image doesn't exist separately
— the RK3399 SoC and peripherals are the same), boot it, and update:

```sh
sudo apt-get update && sudo apt-get upgrade -y
```

Install the build tools this repo's own scripts need:

```sh
sudo apt-get install -y git gcc-arm-linux-gnueabi make gcc python3 \
    alsa-utils
```

(`gcc-arm-linux-gnueabi` is the soft-float ARM32 cross-compiler the shims
and the chroot build need; the host's own `gcc` builds `ddj400-bridge`,
which runs natively on the board's own aarch64 userland, not in the
chroot.)

## 2. Get this repo

```sh
git clone https://github.com/fabiodj2/Orangepi4-RX3 ~/RX3-RBPI4B-RK3399
cd ~/RX3-RBPI4B-RK3399
git checkout port/rk3399
```

The systemd unit this tutorial installs later (§10) hardcodes
`/home/orangepi/RX3-RBPI4B-RK3399` and `User=orangepi` — either clone to
exactly that path and user, or edit `scripts/rk3399/systemd/rx3-rbpi4b-rk3399.service`
to match your own.

## 3. Build the runtime chroot — **verified 2026-10-18**

`scripts/build-chroot.sh` (inherited from the upstream Raspberry Pi 4 port)
assembles a complete soft-float chroot from the extracted assets from §0
and tars it as `work/rbpi4b-pi4.tgz`, meant to be `scp`'d to a separate
Pi and untarred by `scripts/device/install.sh`. On the Orange Pi, build
host and target are the **same machine**, so instead of that remote-deploy
round trip, untar the chroot directly where the RK3399 launcher expects it.
**It needs root** (tar has to recreate real device nodes — `/dev/null`,
`/dev/tty`, `/dev/console` — inside the staged rootfs, which a non-root
`tar` silently can't do, failing the whole build with "Cannot mknod:
Operation not permitted"):

```sh
sudo RX3=/path/to/extracted OUT=work bash scripts/build-chroot.sh
sudo mkdir -p work/runtime
sudo tar -xzf work/rbpi4b-pi4.tgz -C work/runtime
sudo mv work/runtime/rbx3-run work/runtime/rx3
```

This exact combination (run this script, by hand, with real extracted
assets, as a single pass) had never been exercised end-to-end on this
unit before — the chroot that was actually running in production through
most of this port's history was produced by copying an already-built
chroot from the sibling `RX3-RK3399-DDJ400` project instead (`rsync -aHx`,
see `docs/rk3399/00-port.md`'s "Runtime" section). Run for real while
writing this tutorial, using this unit's own extracted firmware assets
(never uploaded anywhere — see `NOTICE.md`): it completed cleanly end to
end, and every hash matched `docs/04-firmware-assets.md`'s documented
table exactly at each stage (`rbp-audio` stage 1 `3dda2d4e...`, the final
patched `rbp` `990404244853a7d613be3d469ad1bc1d` — "stage 2+3, 32bpp, the
default and what the unit runs"). `chroot .../rbx3-run /bin/sh -c 'echo ok'`
on the result printed `ok`. Output was 33 MB compressed.

Two things to know about what this step does and doesn't give you yet:

* `build-chroot.sh` already builds and deploys its *own* copies of
  `audioshim.so`, `fbshim.so` and `knobshim.so` from `scripts/shims` as
  part of this same run — and since this repo's `audioshim.c` is the
  DDJ-400 one as of the 2026-10-18 consolidation, that automatic copy is
  already correct. But it deploys `fbshim.so` to `usr/lib/fbshim.so`,
  **not** `usr/lib/rbpi4b/fbshim.so`, which is the path the RK3399
  launcher actually expects (§5 below fixes this), and it does not build
  or deploy `keyshim.so`/`memshim.so`/`netshim.so` at all (FLX4 uses
  `knobshim.so` instead; those three are DDJ-400-specific, also added in
  §5).
* The DirectFB stack this step installs is the **generic**, unpatched
  1.4.16 build (whatever `$DFB` points at, or `work/dfb` if already built
  from a previous pass) — it works (the chroot loads, `rbp` would run),
  but without the RK3399 KMS/colorkey patches from §4 below, display is
  the slow deferred-I/O `/dev/fb0` path, not what's actually deployed in
  production.

Verify the chroot is sane before going further:

```sh
sudo chroot work/runtime/rx3 /bin/sh -c 'echo ok'
```

## 4. RK3399-specific DirectFB patches — **verified**

The generic chroot build's DirectFB targets a plain `/dev/fb0` pan, which
on this board's `rockchip-drm` is a deferred-I/O shadow (~35 000 page
faults/s, visible flicker, ~15 fps). Apply the RK3399 patches on top,
following `docs/rk3399/00-port.md`'s "Building on the Orange Pi" section
exactly (DirectFB 1.4.16 base build per `tools/build-directfb/README.md`
steps 0-3, then `tools/build-directfb/rk3399-kms-scanout.diff` and
`rk3399-colorkey16-fix.diff`, built with the `F=` flags documented there,
then the soname fix-up). Deploy the resulting libs into
`work/runtime/rx3/usr/lib/` (and the `directfb-1.4-6` module dirs),
replacing the generic chroot build's own copies.

Then fix the chroot's own DirectFB config, which this build reads from
`usr/etc`, not `/etc`:

```sh
cat > work/runtime/rx3/usr/etc/directfbrc <<'EOF'
# rbpi4b DirectFB (prefix /usr -> this file, not /etc/directfbrc).
no-hardware
no-cursor
system=fbdev
fbdev=/dev/fb0
module-dir=/usr/lib/directfb-1.4-6
no-vt
no-vt-switch
EOF
```

Without `module-dir` set correctly, no DirectFB module loads at all
(`dfb_system_lookup ret=17`).

TouchCalib files (`root/settings/TouchCalib_User.dat` and
`TouchCalib_Factory.dat`) must exist in the chroot for `rbp` to accept
touch input — `docs/07-touch.md` has their format if you need to generate
them for a different panel than the one this port was verified against.

## 5. Build the shims — **verified**

As of the 2026-10-18 consolidation, `scripts/shims/` builds everything
needed — including the DDJ-400 pieces (`audioshim.c`, `keyshim.c`,
`memshim.c`) that used to live only in the sibling project:

```sh
cd scripts/shims
make clean
make -j6 RX3="$PWD/../../work/runtime/rx3" fbshim.so audioshim.so \
    keyshim.so memshim.so netshim.so
```

Check every shim links only against the chroot's own glibc 2.13 before
deploying — a clean compile is not evidence of a working build (host
headers can silently stamp modern GLIBC symbol versions the chroot's
loader will refuse):

```sh
for f in fbshim.so audioshim.so keyshim.so memshim.so netshim.so; do
    echo "-- $f --"
    arm-linux-gnueabi-objdump -T "$f" | grep -o 'GLIBC_[0-9.]*' | sort -Vu
done
```

Every line must read `GLIBC_2.4` or `GLIBC_2.7`, nothing higher. Deploy:

```sh
cd ../..
sudo mkdir -p work/runtime/rx3/usr/lib/rbpi4b
sudo cp scripts/shims/fbshim.so  work/runtime/rx3/usr/lib/rbpi4b/fbshim.so
sudo cp scripts/shims/audioshim.so work/runtime/rx3/usr/lib/audioshim.so
sudo cp scripts/shims/keyshim.so  work/runtime/rx3/usr/lib/keyshim.so
sudo cp scripts/shims/memshim.so  work/runtime/rx3/usr/lib/memshim.so
sudo cp scripts/shims/netshim.so  work/runtime/rx3/usr/lib/netshim.so
```

(`fbshim.so` deploys under `usr/lib/rbpi4b/`, not `usr/lib/` directly — the
launcher's `PRELOAD` line names that exact path; the others go straight
into `usr/lib/`. Confirm against `scripts/rk3399/start-rk3399.sh`'s own
`PRELOAD=` line if this ever drifts.)

## 6. Build the host-side tools — **verified**

`ddj400-bridge` runs on the board's own native aarch64 userland (not in the
chroot) and talks MIDI to the DDJ-400:

```sh
sh scripts/rk3399/build-host-tools.sh
```

Produces `work/build/host/ddj400-bridge`.

## 7. KEY/STEMS core (optional) — gated, separate

`start-rk3399.sh` defaults to `RX3_CORE=toolkit`, which expects an
rx3-toolkit build staged into the chroot (`scripts/rk3399/toolkit/stage-toolkit.py`,
needing the external `rx3-toolkit` repo built separately — see §0). For a
first install, skip this entirely by setting `RX3_CORE=legacy` when you
start the service (§10), which expects `/root/pdj/librx3_core.so` already
present in the chroot instead.

**Neither core is required for `rbp` to run, control, and play audio** —
they only add the KEY/STEMS touch panel. If you have neither built yet,
this step genuinely has not been tested on a chroot missing both:
`PRELOAD` in `start-rk3399.sh` always appends `$CORE_LIB` whichever mode is
selected, so a missing file there would likely make the whole `LD_PRELOAD`
chain fail to load and `rbp` would not start at all. If you hit that on a
fresh install with no toolkit and no legacy core file, the fix is to
comment the `$CORE_LIB` entry out of `PRELOAD=` in `start-rk3399.sh`
until you have one of the two built — this has not been verified live
either, but follows directly from how `LD_PRELOAD` resolves its list.

## 8. Connect the hardware

Plug in the monitor (HDMI, or a single USB-C cable carrying both video and
touch — see §0), the USB touchscreen or mouse if it's a separate cable,
and the DDJ-400 **directly** into a USB-A port on the board (a bus-powered
hub was seen to fail enumeration in the sibling project's own testing).
Boot the board.

## 9. systemd service

```sh
sudo sh scripts/rk3399/install-service.sh
```

This installs and starts `rx3-rbpi4b-rk3399.service`, and (as of
2026-10-18) the base `rx3-interface-toggle.path`/`.service` pair too —
the DDJ-400's interface-toggle gesture (hold a button combination, see
`docs/08-controls.md`), pointing directly at this repo's own
`scripts/rk3399/toggle-interface.sh`. A fresh install needs nothing extra
for this: `install-service.sh` installs both units itself and enables
`rx3-interface-toggle.path`.

(Earlier versions of this script only installed a drop-in, which needed a
*base* unit from somewhere else to already exist — a real gap on a
genuinely fresh system, since nothing created that base pair either. The
drop-in is gone now; the base units it used to override are installed
directly, already pointing at the right place.)

If you're skipping the toolkit/legacy core (§7), start the service with
that override:

```sh
sudo systemctl stop rx3-rbpi4b-rk3399.service
sudo RX3_CORE=legacy sh scripts/rk3399/start-rk3399.sh run
```

(`systemctl edit rx3-rbpi4b-rk3399.service` to add `Environment=RX3_CORE=legacy`
permanently, once you've confirmed it starts.)

## 10. First start and verification

```sh
sudo systemctl status rx3-rbpi4b-rk3399.service
journalctl -u rx3-rbpi4b-rk3399 -f
less work/log/launcher-run.log
```

Checklist (see `docs/rk3399/00-port.md`'s "First light" for what a healthy
boot's log looks like):

* picture on the HDMI monitor, deck view;
* touch responds (tap a deck button);
* `USB1 database READY` appears once a rekordbox-exported stick is
  inserted (`docs/10-usb.md`/`docs/12-troubleshooting.md` if it doesn't —
  a plain folder of audio files with no `PIONEER/rekordbox/export.pdb` is
  not a rekordbox export and will not populate the browse screen);
* PLAY/CUE/jog/pads on the DDJ-400 all do something;
* audio out of the DDJ-400's RCA master out and headphone jack.

## 11. Audio gain staging — **verified**

Once a track plays: set each channel's **TRIM** so the CHANNEL LEVEL
meter's orange segment lights on peaks (stock Pioneer procedure, pure MIDI
passthrough, no code involved); leave **MASTER LEVEL** at max (it is a
pure attenuator, no boost above unity). The digital gain
(`RX3_MASTER_GAIN_DB`, default `22`) is already tuned *for* that operating
point — see [19 — DDJ-400 MIDI](../19-ddj400-midi.md)'s audio section for
the full tuning history if you ever need to re-tune it for different
monitors or a different unit.

## Troubleshooting

[docs/12-troubleshooting.md](../12-troubleshooting.md) is the symptom →
cause → fix table for the original Pi 4 + DDJ-FLX4 bring-up — most rows
apply here too (display, touch, audio xrun patterns), but read its own
historical-note disclaimer; anything DDJ-400-specific is in
[19 — DDJ-400 MIDI](../19-ddj400-midi.md) instead.
