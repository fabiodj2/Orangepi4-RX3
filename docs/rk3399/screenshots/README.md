# Screenshots

Captured straight off the live KMS scanout — no camera, no re-encoding beyond
the lossless BMP→PNG step. The pipeline:

1. `fbshim.so`'s `screenshot_tick()` (in `scripts/shims/pointsrc.c`) polls
   `work/runtime/rx3/tmp/rx3-screenshot.request` every 50 ms; on a hit it reads
   the current scanout buffer straight out of the KMS dumb buffer and writes a
   24-bit BMP to `work/runtime/rx3/tmp/rx3-screenshot.bmp`.
2. `scripts/rk3399/capture-screens.py` drives rbp's own key API through the
   keyshim FIFO (`work/runtime/rx3/tmp/rb-keys.fifo` — the same 12-byte
   `{int32 key, ch, down}` record `scripts/rb/device/press-key.sh` in
   RX3-RK3399-DDJ400 uses) to move the UI through a known-good sequence of
   screens, grabbing one screenshot per screen and converting it with
   `scripts/rk3399/bmp2png.py` (pure stdlib — no Pillow/ImageMagick on the
   board).

This never touches `rb-ctrl.fifo`, the FIFO the real DDJ-400 bridge
(`ddj400-bridge`) writes to, so it's safe to run against the live service
without disturbing an attached controller. It's also limited to
view-navigation keys only — nothing that touches audio output (no PLAY, CUE
or FX ON) — see the long comment at the top of `capture-screens.py` for
exactly which keys were confirmed to do what on hardware, and which ones
(`MENU`, `REKORDBOX`) turned out not to do anything in this build.

## Regenerating

```sh
python3 scripts/rk3399/capture-screens.py
```

Run on the board itself, with `rx3-rbpi4b-rk3399.service` up. It leaves the UI
back on the deck view when it's done.

## Files

| File | Screen |
|---|---|
| `deck-view.png` | Deck/waveform view, HOT CUE pad bank, BEAT FX side panel |
| `pads-beatloop-d1.png` | Pad bank switched to BEAT LOOP (deck 1) |
| `pads-slip-d1.png` | Pad bank switched to SLIP LOOP (deck 1) |
| `pads-beatjump-d1.png` | Pad bank switched to BEAT JUMP (deck 1) |
| `tag-list.png` | TAG LIST screen (empty — no tags on this test stick) |
| `source-device-picker.png` | SOURCE screen, device picker + volume/song count |
| `link-select-source.png` | LINK screen ("Please select a source") |
| `browse-library.png` | Library list, USB1, sorted by file name |
| `library-track-info.png` | Library list with the INFO panel open (metadata, LOAD 1/2) |
| `utility-mixer-settings.png` | UTILITY → MIXER, captured by hand via the touchscreen (see below) |

`utility-mixer-settings.png` is the one screen `capture-screens.py` can't
reach on its own: the live UTILITY/settings screen has no confirmed
controller key that opens it in this build — on hardware it was reached by
touching **MY SETTINGS MENU** on the SOURCE screen. It's included here from
the CROSSFADER CURVE investigation (`docs/rk3399/00-port.md`) as the
reference shot of the CHANNEL FADER CURVE row.
