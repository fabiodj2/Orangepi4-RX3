# Screenshots

Captured straight off the live KMS scanout — no camera, no re-encoding beyond
the lossless BMP→PNG step. Three separate injection primitives feed this,
each hardware-verified on 2026-10-15 and each reaching a different layer of
the stack — see the long header comment in
`scripts/rk3399/capture-screens.py` for the full story and exactly which
screens each one reaches:

1. **Key presses** through the keyshim FIFO (`.../tmp/rb-keys.fifo`, the
   same 12-byte record `scripts/rb/device/press-key.sh` in RX3-RK3399-DDJ400
   uses) — page buttons, pad banks, LOAD, SEARCH, and the UTILITY-opening
   MENU *hold*. Independent of the real DDJ-400's own `rb-ctrl.fifo`, so it
   never touches a live controller.
2. **Raw touch taps** through a new debug hook in `fbshim.so`
   (`touch_inject_tick()` in `scripts/shims/pointsrc.c`, request file
   `.../tmp/rx3-touch.request`, `"<down> <x> <y>"` in logical 1280×800) —
   reaches rbp's touch zones, used here for BROWSE's left-column sort tabs.
   It does **not** reach the shim's own swipe-down top menu band
   (`menu_zone.c`): that band intercepts the real touchscreen's raw reports
   a layer upstream of this hook, so **PLAYLIST** and **USB STOP** (two of
   that band's seven columns) aren't reachable this way — not captured here.
3. **STEMS-panel command packets** through `.../tmp/rx3-stems-touch.fifo`
   (`stems_touch_send()` in `ddj400-bridge.c`'s protocol) — opens the
   STEMS and KEY panels. That FIFO is root-owned 0600, so this part of the
   tour needs `sudo`.

`fbshim.so`'s `screenshot_tick()` (polling `.../tmp/rx3-screenshot.request`,
writing a 24-bit BMP to `.../tmp/rx3-screenshot.bmp`) pulls the frame itself;
`scripts/rk3399/bmp2png.py` (pure stdlib — no Pillow/ImageMagick on the
board) converts it.

Limited to view-navigation, with one exception: the BEAT FX on/off step is
real audio engage/disengage (it's the only way to show that screen) — it's
pulsed on, captured, and immediately pulsed back off. Nothing here touches
PLAY or CUE, and nothing here loads or changes a track.

## Regenerating

```sh
python3 scripts/rk3399/capture-screens.py        # everything except STEMS/KEY
sudo python3 scripts/rk3399/capture-screens.py   # + STEMS/KEY panels
```

Run on the board itself, with `rx3-rbpi4b-rk3399.service` up. It leaves the
UI back on the deck view, HOT CUE pad bank, when it's done.

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
| `browse-artist.png` / `-albums` / `-key` / `-bitrate` / `-matching` / `-folder` / `-rec` / `-track` | Library list, each of BROWSE's other left-column sort tabs |
| `search-keyboard.png` | SEARCH — the on-screen keyboard |
| `utility-deck-settings.png` | UTILITY → DECK category (top of the list) |
| `utility-mixer-settings.png` | UTILITY → MIXER category (captured by hand — see below) |
| `utility-general-settings.png` | UTILITY → MIXER's tail + GENERAL category (scrolled with the browse selector) |
| `stems-panel-d1.png` | STEMS panel, deck 1 (no track loaded — "NO STEMS") |
| `key-panel-d1.png` | KEY panel, deck 1 (no track loaded — "KEY --") |
| `beatfx-on.png` | BEAT FX engaged (DELAY, deck 1) — pulsed on/off, no track playing |

`utility-mixer-settings.png` is the one screen `capture-screens.py` doesn't
capture itself: it's a hand-touched screenshot from the CROSSFADER CURVE
investigation (`docs/rk3399/00-port.md`), kept here as the reference shot of
the CHANNEL FADER CURVE row rather than re-walked by script (the script's
own `utility-deck-settings.png` + `utility-general-settings.png` pair
already demonstrates the same UTILITY → scroll → category mechanism).

**Not captured**: PLAYLIST and USB STOP (the swipe-down top menu band — see
above) and anything inside the STEMS/KEY panels beyond the panel opening
itself (INST/VOCAL/DRUMS mute toggles and KEY −1/+1 both touch live audio,
so they're left for a hands-on session rather than scripted).
