# 19 — DDJ-400 MIDI mapping and audio (the real production controller)

This is the controller/audio reference for what actually runs in
production: an **RK3399 (Orange Pi)** with a **Pioneer DDJ-400**, via
`scripts/rk3399/device/ddj400-bridge.c` → `/tmp/rb-ctrl.fifo` →
`scripts/shims/keyshim.c`. Brought into this repo 2026-10-18 from the
sibling `RX3-RK3399-DDJ400` project's `docs/12-ddj400.md`, where it was
generated directly from this same `ddj400-bridge.c` source and corrected
against it (§3 there found and fixed one real drift: BEAT SYNC short-press
was documented as `K_SYNC`, the code has always sent `K_MASTER_TEMPO`) —
**if this table ever disagrees with the code, trust the code first, then
this table**.

[15 — DDJ-FLX4 MIDI](15-flx4-midi.md) is the equivalent reference for the
original Raspberry Pi 4 + DDJ-FLX4 target this repo was ported from —
different controller, different note/CC layout, kept as its own doc rather
than merged into this one.

## Status

| Part | State |
|---|---|
| MIDI → engine control path (`bridge` → FIFO → `keyshim` → `sendKey`) | ✅ done, verified |
| DDJ-400 MIDI mapping (transport, mixer, jog, tempo, pads, loops, Beat FX) | ✅ written from the official MIDI list, verified on hardware for transport/browse/pads/mixer |
| Audio through the DDJ-400 (master out + headphone cue) | ✅ done — `audioshim.c` drives the DDJ-400's 4-channel USB audio device directly |

## Every mapped MIDI message

Two dispatch styles are used, and they behave differently under a held
button -- worth knowing before mapping anything new onto an unused note:

* **passthrough** -- press/release mirror the physical note on/off exactly;
  the engine sees the real hold duration (this is how AUTO CUE's short/long
  dual behavior works).
* **send_tap()** -- press+release are fired together, synthetically, the
  instant the physical note turns on; the physical release and any hold
  duration are ignored. Used for MASTER CUE, CHANNEL CUE, and SEARCH
  (marked below).

### Channel 1 / 2 -- DECK 1 / DECK 2 (identical on both, `ch` = deck)

| Note/CC | Sent as | rbp key | Dispatch |
|---|---|---|---|
| Note 0x0B | PLAY/PAUSE | `K_PLAY 0x4101` | passthrough |
| Note 0x47 (SHIFT+PLAY, renumbered) | CENSOR/REVERSE | `K_REV 0x410f` | passthrough |
| Note 0x0C | CUE | `K_CUE 0x4102` | passthrough |
| Note 0x48 (SHIFT+CUE, renumbered) | VINYL | `K_VINYL 0x4104` | passthrough |
| Note 0x58 | BEAT SYNC | `K_MASTER_TEMPO 0x4108` | passthrough (LED-only in `led_handle_input()`, real state owned by engine) |
| Note 0x5C | BEAT SYNC, long-press | `K_MASTER 0x4111` | passthrough |
| Note 0x60 (SHIFT+SYNC, renumbered) | TEMPO RANGE | `K_TEMPO_RANGE 0x4107` | passthrough |
| Note 0x68 (SHIFT+CHANNEL CUE, renumbered) | QUANTIZE | `K_QUANTIZE 0x410b` | passthrough |
| Note 0x10 | LOOP IN / 4 BEAT | `K_LOOPIN 0x410c` | passthrough |
| Note 0x11 | LOOP OUT | `K_LOOPOUT 0x410d` | passthrough |
| Note 0x4D | RELOOP/EXIT | `K_RELOOP 0x410e` | passthrough |
| Note 0x50 (SHIFT+RELOOP/EXIT, separate note) | -- | `K_AUTOCUE 0x410a` | passthrough. Dual-purpose in the engine itself: tap -> TimeMode (REMAIN/TIME), hold ~1s -> AUTO CUE toggle |
| Note 0x51 | CUE/LOOP CALL 1/2X | `K_CALLPREV 0x4323` | passthrough |
| Note 0x53 | CUE/LOOP CALL 2X | `K_CALLNEXT 0x4322` | passthrough |
| Note 0x54 | CHANNEL CUE (headphone) | `K_HPCUECH 0x5020` param=deck+1 | **send_tap()**, in `led_handle_input()` -- not in `notemap[]` |
| Note 0x36 | jog plate touch | `K_JOG_TOUCH 0x4306` | passthrough |
| Note 0x67 (SHIFT+jog touch, renumbered) | -- | `K_JOG_TOUCH 0x4306` | passthrough (same key -- SHIFT adds no distinct behavior here) |
| Note 0x1B | PAD MODE announce: HOT CUE | `K_HOTCUE 0x4113` | passthrough, also switches pad bank |
| Note 0x6D | PAD MODE announce: BEAT LOOP | `K_ALOOP 0x4114` | passthrough, also switches pad bank |
| Note 0x20 | PAD MODE announce: BEAT JUMP | `K_BEATJUMP 0x4116` | passthrough, also switches pad bank |
| Note 0x22 | PAD MODE announce: SAMPLER | `K_SLIPLOOP 0x4115` (fallback, no RX3 equivalent) | passthrough, also switches pad bank |
| Notes 0x1E, 0x6F (PAD FX1 / KEY SHIFT mode announce) | -- | none | unmapped/no-op; mode is picked up from the pad note itself (upper nibble) on first pad press, not the announce |
| CC 0x13 (MSB) + 0x33 (LSB) | CHANNEL FADER | `K_FADER 0x501e` | op4 (14-bit absolute) |
| CC 0x04 + 0x24 | TRIM | `K_TRIM 0x5019` | op4 |
| CC 0x07 + 0x27 | EQ HI | `K_EQH 0x501a` | op4 |
| CC 0x0B + 0x2B | EQ MID | `K_EQM 0x501b` | op4 |
| CC 0x0F + 0x2F | EQ LOW | `K_EQL 0x501c` | op4 |
| CC 0x00 + 0x20 | TEMPO SLIDER | `K_TEMPO_SLIDER 0x4109` | op5, signed normalised -1..+1 |
| CC 0x21 | jog side rotate | `K_JOG_ROT 0x4305` | op4 relative, `f`=rev/s |
| CC 0x22 | jog platter rotate (vinyl) | `K_JOG_ROT` | op4 relative |
| CC 0x23 | jog platter rotate (non-vinyl) | `K_JOG_ROT` | op4 relative |
| CC 0x29 | SHIFT + platter rotate | `K_SEARCHFWD 0x411f` / `K_SEARCHREV 0x4120` | discrete press/release, direction-aware, auto-release after 150ms idle (`SEARCH_IDLE_MS`) |

### Channel 5 -- EFFECT (Beat FX)

| Note/CC | Sent as | rbp key | Dispatch |
|---|---|---|---|
| Note 0x63 | FX select | `K_BFXTYPE 0x448b` | cycles a local counter (`beat_fx_type`, 0..13) on press, sends op5 |
| Note 0x4A | FX beat < | `K_BEATPREV 0x4490` | passthrough |
| Note 0x4B | FX beat > | `K_BEATNEXT 0x4491` | passthrough |
| Note 0x6B (SHIFT+beat >, renumbered) | TAP | `K_TAP 0x4492` | passthrough |
| Note 0x10 | FX channel select CH1 | `K_BFXCH 0x448c` op5=0 | on press only; SHIFT held -> crossfader curve instead (below) |
| Note 0x11 | FX channel select CH2 | `K_BFXCH` op5=1 | same |
| Note 0x14 | FX channel select MASTER | `K_BFXCH` op5=5 | same |
| Note 0x10/0x11/0x14 **while SHIFT held** (`shift_held[]`, not a renumbered note) | crossfader curve | `K_FADERCURVE 0x6018` op5 = 1/2/3 | 0=THRU exists but unused here |
| Note 0x47 | FX on/off | `K_BFX 0x448d` | passthrough (LED-only in `led_handle_input()`) |
| CC 0x02 (+0x22 LSB) | FX level/depth | `K_DEPTH 0x448f` | op5, 14-bit |

### Channel 7 -- MIXER (browser, crossfader, master section)

| Note/CC | Sent as | rbp key | Dispatch |
|---|---|---|---|
| Note 0x41 | BROWSE rotary push | `K_BACK 0x420d` (1st/closed) or `K_SELECTOR 0x420c` (while open) | stateful, in `led_handle_input()`, not `notemap[]` |
| Note 0x42, held < 5s (SHIFT+BROWSE push) | SOURCE / BACK | `K_SOURCE 0x0201` (from deck view) or `K_BACK` (while browsing) | `handle_interface_toggle_note()`, fires on release |
| Note 0x42, held >= 5s | -- | none (writes `/run/rx3-interface-toggle.request`, GUI toggle, not an rbp key) | `interface_toggle_tick()` |
| Note 0x46 | LOAD deck 1 | `K_LOAD 0x4311` ch1 | passthrough (`notemap[]`); also clears browser state |
| Note 0x47 | LOAD deck 2 | `K_LOAD` ch2 | passthrough |
| Note 0x68 (SHIFT+LOAD1, renumbered) | SOURCE | `K_SOURCE 0x0201` | passthrough |
| Note 0x7A (SHIFT+LOAD2, renumbered) | MENU | `K_MENU 0x0206` | passthrough |
| Note 0x63 | MASTER CUE | `K_MASTERCUE 0x4407` | **send_tap()**, in `led_handle_input()` -- not in `notemap[]` |
| Note 0x78 (SHIFT+MASTER CUE, separate note) | SEARCH toggle | `K_SEARCH 0x0205` (on) / `K_BACK` (off) | **send_tap()**, stateful toggle, blinking LED while active |
| CC 0x40 | BROWSE rotary encoder | `K_SELECTOR 0x420c` | op4, relative, one send per detent (clamped ±16/msg) |
| CC 0x1F + 0x3F | CROSSFADER | `K_XFADER 0x6017` | op4 |
| CC 0x0C + 0x2C | HEADPHONES MIXING | `K_HPMIX 0x4405` | op4 |
| CC 0x0D + 0x2D | HEADPHONES LEVEL | `K_HPLEVEL 0x4406` | op4 |
| CC 0x17 | FILTER ch1 (Sound Color FX value) | `K_COLOR 0x509d` ch1 | op5, plain 7-bit (not part of `cc14[]`) |
| CC 0x18 | FILTER ch2 | `K_COLOR 0x509d` ch2 | op5, plain 7-bit |

### Channels 8-11 -- PADS (deck1, deck1+SHIFT, deck2, deck2+SHIFT)

Note = `mode*16 + (pad_index)`, mode read from the upper nibble. Direct
RX3 pad banks only exist for modes 0/2/3/6; the rest fall back or are
repurposed entirely (see `pad_rx3_bank()` and `handle_pad()`).

| DDJ pad mode (upper nibble) | RX3 target | rbp key | Notes |
|---|---|---|---|
| 0x0_ HOT CUE | HOT CUE (direct) | `K_PAD1 0x4117` + idx | shifted (channels 8/10) -> native hot cue delete instead, via STEMS command channel, not a pad key |
| 0x1_ PAD FX 1 (reached via SHIFT+BEAT LOOP) | Sound Color FX **type** select | `K_COLOR_TYPE 0x50a7` (keyshim-only pseudo-key, not a real rbp keycode) | pad 1-6 -> type 1-6; pads 7/8 unused |
| 0x2_ BEAT JUMP | BEAT JUMP (direct) | `K_PAD1` + idx | |
| 0x3_ SAMPLER | SLIP LOOP (fallback, no RX3 equivalent) | `K_PAD1` + idx | |
| 0x4_ KEYBOARD | HOT CUE (fallback) | `K_PAD1` + idx | |
| 0x5_ PAD FX 2 | **STEMS** DRUMS/VOCAL/INST toggles | none (`stems_pad_send_command()`, separate channel) | pads 1-3 only; 4-8 reserved |
| 0x6_ BEAT LOOP | BEAT LOOP (direct) | `K_PAD1` + idx | |
| 0x7_ KEY SHIFT (reached via SHIFT+SAMPLER) | same STEMS page as PAD FX2 | none | second entry point, by user request |

## Why a bridge is needed

Two facts shape the design:

* **`rbp` has no MIDI input at all.** It is the XDJ-RX3 application: it reads
  keycodes from Pioneer front-panel microcontrollers through its internal
  `IKeyManager::sendKey(keycode, op, ch, param, f, l)`. A DDJ-400 is a plain
  USB-MIDI class device, so nothing in the engine can see it.
* **`keyshim.so` already owns the injection path.** It is `LD_PRELOAD`ed into
  `rbp`, resolves the live `KeyManager` singleton and forwards records from
  `/tmp/rb-keys.fifo`. Adding a second FIFO for *values* (faders, jog speed,
  knobs) extends it to a full control surface without touching `rbp`.

So the DDJ-400 is translated, not emulated:

```
DDJ-400 ──USB MIDI──▶ ddj400-bridge ──24B records──▶ /tmp/rb-ctrl.fifo
                       (host rootfs)                          │
                                                               ▼
                    keyshim.so  ──▶  IKeyManager::sendKey(key, op, ch, param, f, l)
                    (LD_PRELOAD in the soft-float chroot)     │
                                                               ▼
                                                    rbp UI + audio engine
```

The bridge runs on the **host** rootfs, not in the soft-float chroot: it only
needs `/dev/snd/midiC*D*` and the shared `/tmp`. It has no libasound
dependency -- it reads the ALSA rawmidi character device directly and parses
MIDI itself.

## Audio (master out + headphone cue)

The DDJ-400 is a class-compliant USB audio interface: **main out on
channels 1-2, headphones on 3-4** (the Mixxx manual configures exactly
that). `audioshim.c` opens `hw:CARD=DDJ400,DEV=0` directly (4 ch, S24_LE)
and repacks `rbp`'s master and headphone/cue PCM streams into it:

| rbp device | DDJ-400 PCM channels |
|---|---|
| master | 0-1 → master out (RCA) |
| headphone/cue | 2-3 → headphone jack |

Real headphone cueing works with the DDJ-400's CUE buttons and
`HEADPHONES MIXING`/`LEVEL` knobs (`K_HPMIX`/`K_HPLEVEL`).

### Physical gain staging (TRIM / MASTER LEVEL)

TRIM and MASTER LEVEL are pure MIDI passthrough into the engine's own
mixer (`K_TRIM 0x5019`, see the CC table above) -- no code decides their
"ideal" value, same as on a real XDJ-RX3. The correct procedure is the
stock Pioneer one: set TRIM per channel so the CHANNEL LEVEL meter's
orange segment lights on peaks; MASTER LEVEL is a pure 0..−∞dB
attenuator with no boost above unity (confirmed against the official
DDJ-400 Operating Instructions), so run it at max unless deliberately
attenuating. The digital gain below is tuned *for* that operating point,
not instead of it.

### Digital gain (`audioshim.c`)

Both channels carry a digital gain stage, in dB, applied in-shim before the
real DAC gets the samples (on top of, not instead of, correct TRIM/MASTER
staging above):

* `RX3_MASTER_GAIN_DB` (default `22`, set in `scripts/rk3399/start-rk3399.sh`)
  -- soft-knee limited above 85% of full scale, not hard-clamped, so a loud
  transient compresses toward the ceiling instead of clicking.
  First tuned live by ear 2026-10-18 with TRIM set low: the engine's own
  master bus peaked around −23..−24 dBFS before this gain, and +21 dB
  maximised loudness with `master_limited` at 0 -- but that pre-gain
  reading was an artifact of TRIM being set too low, not the real
  operating condition.
  Corrected the same day with TRIM at its *correct* position (meter's
  orange segment on peaks) and MASTER LEVEL at max: pre-gain peaks ran far
  hotter (up to ≈ −14.8 dBFS), and +21 dB overshot the ceiling by several
  dB -- audible as saturation, with `master_limited` spiking into the tens
  of thousands per log window. Re-tuned live at that corrected position:
  **+22 dB** is the value that stayed clean.
* `RX3_HP_GAIN_X` (default `4`) -- headphone/cue gain multiplier, same
  pattern, no limiter (cue is binary on/off in practice, not mixed live).

See `scripts/shims/audioshim.c`'s `master_gain_linear()`/`scale_master_s24()`
for the implementation.
