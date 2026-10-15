#!/usr/bin/env python3
"""capture-screens.py — automated screenshot tour of the live rbp UI.

Drives rbp's own key API through the keyshim FIFO (the same path
scripts/rb/device/press-key.sh in RX3-RK3399-DDJ400 uses: 12-byte
{int32 key, int32 ch, int32 down} records on .../tmp/rb-keys.fifo, read by
keyshim.so independently of the real DDJ-400 control traffic on
rb-ctrl.fifo -- so this never steps on a live controller), then pulls a
screenshot through fbshim.so's screenshot_tick() (touch
.../tmp/rx3-screenshot.request, read back .../tmp/rx3-screenshot.bmp) for
each screen and converts it to PNG with bmp2png.py.

Deliberately limited to view-navigation keys -- nothing that touches audio
output (no PLAY/CUE/FX ON) so it's safe to run against the live service
without affecting whatever is on the master.

UI navigation is stateful and was mapped live, one screenshot at a time, on
2026-10-15; only what was actually confirmed on hardware is encoded here:

  * K_BACK (0x420d), from a clean deck view with the browser closed, OPENS
    the plain library list. From any other screen it did nothing in this
    build/firmware -- not the generic "go back" its name suggests.
  * K_BROWSE (0x0202) reliably CLOSES an open library list/info panel back
    to the deck view, confirmed from both the plain list and the list+INFO
    panel. Used here as the universal "return to deck view" step.
  * K_SOURCE (0x0201), K_USB1 (0x0209), K_TAGLIST (0x0203), K_LINK (0x0207)
    and K_INFO (0x020b) are direct jumps: each one switches straight to its
    screen from wherever the UI currently is (deck view or another list).
  * K_MENU (0x0206) and K_REKORDBOX (0x0208) had NO observed effect against
    this build -- the live UTILITY/settings screen that this project's
    CROSSFADER CURVE work uses was reached by a physical touchscreen tap on
    "MY SETTINGS MENU" (visible on the SOURCE screen), not a controller key.
    Capture it by hand if you need a fresh one; see utility-mixer-settings.png
    for the one already in this directory.
  * Pad-bank keys (K_PADBANK_* 0x4113..0x4116, ch = deck 1|2) only visibly
    change anything while the deck view's pad strip is on screen -- they are
    silently ignored while a library list/sub-screen is open.

Run on the board, with the service up, from a fresh deck view (restart the
service first if unsure what screen rbp is currently on):

    python3 scripts/rk3399/capture-screens.py [output_dir]
"""
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bmp2png  # noqa: E402

RX3_TMP = os.path.join(
    os.path.dirname(os.path.dirname(HERE)), "work", "runtime", "rx3", "tmp"
)
KEY_FIFO = os.path.join(RX3_TMP, "rb-keys.fifo")
SCREENSHOT_REQUEST = os.path.join(RX3_TMP, "rx3-screenshot.request")
SCREENSHOT_BMP = os.path.join(RX3_TMP, "rx3-screenshot.bmp")

K_BACK, K_BROWSE, K_SOURCE, K_USB1 = 0x420d, 0x0202, 0x0201, 0x0209
K_TAGLIST, K_LINK, K_INFO = 0x0203, 0x0207, 0x020b
K_PAD_HOTCUE, K_PAD_BEATLOOP, K_PAD_SLIP, K_PAD_BEATJUMP = 0x4113, 0x4114, 0x4115, 0x4116

# (slug, title, key, ch) -- the exact, hardware-verified sequence from the
# module docstring. Order matters: each step assumes the state the previous
# step is known to leave the UI in.
SCREENS = [
    ("_reset1",          None,                             K_BACK,   0),  # open plain list (from deck view)
    ("deck-view",        "Deck / waveform view",            K_BROWSE, 0),  # close it -> deck view
    ("pads-beatloop-d1", "Pad bank: BEAT LOOP (deck 1)",     K_PAD_BEATLOOP,  1),
    ("pads-slip-d1",     "Pad bank: SLIP LOOP (deck 1)",     K_PAD_SLIP,      1),
    ("pads-beatjump-d1", "Pad bank: BEAT JUMP (deck 1)",     K_PAD_BEATJUMP,  1),
    ("_restore-hotcue",  None,                               K_PAD_HOTCUE,    1),  # leave deck 1 on HOT CUE
    ("tag-list",         "TAG LIST",                         K_TAGLIST, 0),
    ("source-device-picker", "SOURCE -- device picker",      K_SOURCE,  0),
    ("link-select-source",   "LINK",                         K_LINK,    0),
    ("browse-library",   "Library list (USB1, by file name)", K_USB1,   0),
    ("library-track-info", "Library list + INFO panel",      K_INFO,    0),
    ("_reset2",          None,                               K_BROWSE,  0),  # back to deck view, cleanup
]


def send_key(key, ch, down):
    rec = struct.pack("<iii", key, ch, 1 if down else 0)
    fd = os.open(KEY_FIFO, os.O_WRONLY | os.O_NONBLOCK)
    try:
        os.write(fd, rec)
    finally:
        os.close(fd)


def press(key, ch, hold_s=0.12):
    send_key(key, ch, True)
    time.sleep(hold_s)
    send_key(key, ch, False)


def grab_screenshot(out_bmp_path, timeout_s=2.0):
    # Drop any stale request/bmp so we know the one we read is fresh.
    try:
        os.unlink(SCREENSHOT_REQUEST)
    except FileNotFoundError:
        pass
    open(SCREENSHOT_REQUEST, "wb").close()
    deadline = time.time() + timeout_s
    while os.path.exists(SCREENSHOT_REQUEST) and time.time() < deadline:
        time.sleep(0.05)
    if os.path.exists(SCREENSHOT_REQUEST):
        raise TimeoutError("screenshot_tick() never picked up the request "
                            "(is the service running?)")
    with open(SCREENSHOT_BMP, "rb") as f:
        data = f.read()
    with open(out_bmp_path, "wb") as f:
        f.write(data)


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(HERE), "..", "docs", "rk3399", "screenshots"
    )
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    if not os.path.exists(KEY_FIFO):
        print("no %s -- is the service running?" % KEY_FIFO, file=sys.stderr)
        return 1

    manifest = []
    for slug, title, key, ch in SCREENS:
        label = title or "(state transition, not captured)"
        print("-> %-36s key=0x%04x ch=%d" % (label, key, ch))
        press(key, ch)
        time.sleep(0.5)  # let the UI finish redrawing
        if title is None:
            continue
        bmp_path = os.path.join(out_dir, slug + ".bmp")
        png_path = os.path.join(out_dir, slug + ".png")
        try:
            grab_screenshot(bmp_path)
        except TimeoutError as e:
            print("   SKIP: %s" % e, file=sys.stderr)
            continue
        width, height, rows = bmp2png.bmp_to_rgb_rows(open(bmp_path, "rb").read())
        bmp2png.write_png(png_path, width, height, rows)
        os.unlink(bmp_path)
        manifest.append((slug, title, png_path))
        print("   saved %s (%dx%d)" % (png_path, width, height))

    captured = sum(1 for s in SCREENS if s[1] is not None)
    print("\n%d/%d screens captured into %s" % (len(manifest), captured, out_dir))
    print("Not covered by this script (see module docstring): the UTILITY/\n"
          "settings screen (touchscreen-only entry point) and anything that\n"
          "touches audio (BEAT FX on/off, PLAY, CUE).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
