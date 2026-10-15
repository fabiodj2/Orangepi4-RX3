#!/usr/bin/env python3
"""capture-screens.py — automated screenshot tour of the live rbp UI.

Three separate injection primitives, all hardware-verified on 2026-10-15,
each feeding a different layer of the stack:

  1. Key presses through the keyshim FIFO (.../tmp/rb-keys.fifo, 12-byte
     {int32 key, ch, down} records -- the same path
     scripts/rb/device/press-key.sh in RX3-RK3399-DDJ400 uses). Independent
     of the real DDJ-400's .../tmp/rb-ctrl.fifo, so it never touches a live
     controller. Drives rbp's own key API: page buttons, pad banks, LOAD,
     SEARCH, the UTILITY-opening MENU hold, and the host-only "rotate browse
     knob" trigger (key 0x7f02, see keyshim.c's key_thread()).
  2. Raw touch taps through fbshim.so's own debug hook, added for this tour
     (scripts/shims/pointsrc.c's touch_inject_tick(), request file
     .../tmp/rx3-touch.request, format "<down> <x> <y>\\n", logical
     1280x800). Feeds tscfake_emit() -- the same call pointsrc.c's own STEMS
     panel taps use -- so it reaches rbp's *touch* zones (BROWSE's sort
     tabs). It does NOT reach the shim's own swipe-down top menu band
     (menu_zone.c): that band intercepts the *real* touchscreen's raw
     reports before they become synthetic taps, a layer upstream of this
     hook, so PLAYLIST and USB STOP (columns of that band) aren't reachable
     this way. Capture those by hand, or extend touch_inject_tick() to feed
     menu_zone's own entry point if that's ever worth it.
  3. STEMS-panel command packets through .../tmp/rx3-stems-touch.fifo (12
     bytes: {magic "SCMD", sequence, deck, control, reserved} -- see
     ddj400-bridge.c's stems_touch_send()). That FIFO is root-owned 0600
     (unlike the other two, which are 0666), so this script's STEMS/KEY
     steps need to run as root; if they can't open it they print why and
     skip instead of failing the whole run.

Deliberately limited to view-navigation -- the one exception is the BEAT FX
on/off step, which is real audio engage/disengage (it's the only way to
document that screen); it's pulsed on, screenshotted, and immediately
pulsed back off. Nothing here touches PLAY or CUE.

UI navigation is stateful; only sequences actually confirmed on hardware are
encoded here. Notably:

  * K_BACK (0x420d), from a clean deck view with the browser closed, OPENS
    the plain library list; from most other screens it does nothing.
  * K_BROWSE (0x0202) reliably closes an open list back to the deck view.
  * K_SOURCE/K_USB1/K_TAGLIST/K_LINK/K_INFO are direct jumps from almost
    anywhere.
  * K_MENU (0x0206) tapped does nothing by itself in this build (it's
    documented as opening "MY SETTINGS" on the SOURCE screen specifically,
    not confirmed here); **held ~400-1100 ms** it opens the real UTILITY
    screen from any screen, on rbp's own timer (docs/07-touch.md,
    docs/08-controls.md). UTILITY takes no touch at all -- it's driven
    entirely by the browse selector (key 0x420c for push-select, 0x7f02 for
    relative rotate) -- and a second MENU hold exits it back to the last
    library list, not the deck view.
  * Pad-bank keys and the STEMS/KEY panel toggles only visibly do anything
    while the deck view's pad strip is on screen.

Run on the board, with the service up, from a fresh deck view (restart the
service first if unsure what screen rbp is currently on):

    python3 scripts/rk3399/capture-screens.py [output_dir]
    sudo python3 scripts/rk3399/capture-screens.py [output_dir]   # + STEMS/KEY
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
TOUCH_REQUEST = os.path.join(RX3_TMP, "rx3-touch.request")
STEMS_TOUCH_FIFO = os.path.join(RX3_TMP, "rx3-stems-touch.fifo")
SCREENSHOT_REQUEST = os.path.join(RX3_TMP, "rx3-screenshot.request")
SCREENSHOT_BMP = os.path.join(RX3_TMP, "rx3-screenshot.bmp")

K_BACK, K_BROWSE, K_SOURCE, K_USB1 = 0x420d, 0x0202, 0x0201, 0x0209
K_TAGLIST, K_LINK, K_INFO, K_MENU = 0x0203, 0x0207, 0x020b, 0x0206
K_SEARCH = 0x0205
K_PAD_HOTCUE, K_PAD_BEATLOOP, K_PAD_SLIP, K_PAD_BEATJUMP = 0x4113, 0x4114, 0x4115, 0x4116
K_ROTATE = 0x7f02          # host trigger: relative rotate of the browse selector
K_BFX = 0x448d             # BEAT FX on/off -- real audio engage, see module docstring
CH_GLOBAL = 1

STEMS_PAD_COMMAND_MAGIC = 0x53434d44  # "SCMD"
STEMS_TOUCH_OPEN, STEMS_TOUCH_KEY_OPEN = 0x20, 0x21


def send_key(key, ch, down):
    fd = os.open(KEY_FIFO, os.O_WRONLY | os.O_NONBLOCK)
    try:
        os.write(fd, struct.pack("<iii", key, ch, 1 if down else 0))
    finally:
        os.close(fd)


def press(key, ch, hold_s=0.12):
    send_key(key, ch, True)
    time.sleep(hold_s)
    send_key(key, ch, False)


def hold(key, ch, ms):
    press(key, ch, hold_s=ms / 1000.0)


def rotate(n, step_s=0.03):
    """Move the UTILITY/browse selector n rows. One FIFO record per row,
    like a real detent: a single send with a large param (e.g. n=15 in one
    shot) was measured 2026-10-15 to sometimes move only 1 row -- whatever
    is throttling it, a burst of unit steps is what hardware would send
    anyway, and it was reliable every time it was tried."""
    step = 1 if n >= 0 else -1
    for _ in range(abs(n)):
        send_key(K_ROTATE, 0, step)
        time.sleep(step_s)


def tap(x, y, hold_s=0.06):
    with open(TOUCH_REQUEST, "w") as f:
        f.write("1 %d %d\n" % (x, y))
    time.sleep(hold_s)
    with open(TOUCH_REQUEST, "w") as f:
        f.write("0 %d %d\n" % (x, y))


_stems_seq = 0


def stems_touch_send(deck, control):
    """Needs root: rx3-stems-touch.fifo is 0600. Returns False (and prints
    why) instead of raising, so a non-root run can skip just this step."""
    global _stems_seq
    _stems_seq += 1
    packet = struct.pack("<IIBBH", STEMS_PAD_COMMAND_MAGIC, _stems_seq, deck, control, 0)
    try:
        fd = os.open(STEMS_TOUCH_FIFO, os.O_WRONLY | os.O_NONBLOCK)
    except OSError as e:
        print("   SKIP (%s): rerun as root for the STEMS/KEY panel steps" % e,
              file=sys.stderr)
        return False
    try:
        os.write(fd, packet)
    finally:
        os.close(fd)
    return True


def grab_screenshot(out_bmp_path, timeout_s=2.0):
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

    def shot(slug, title):
        bmp_path = os.path.join(out_dir, slug + ".bmp")
        png_path = os.path.join(out_dir, slug + ".png")
        try:
            grab_screenshot(bmp_path)
        except TimeoutError as e:
            print("   SKIP: %s" % e, file=sys.stderr)
            return
        width, height, rows = bmp2png.bmp_to_rgb_rows(open(bmp_path, "rb").read())
        bmp2png.write_png(png_path, width, height, rows)
        os.unlink(bmp_path)
        manifest.append((slug, title, png_path))
        print("   saved %s (%dx%d)" % (png_path, width, height))

    def step(title, fn):
        print("-> %s" % title)
        fn()
        time.sleep(0.5)

    # --- deck view + pad banks -------------------------------------------
    step("(reset) open plain list", lambda: press(K_BACK, 0))
    step("Deck / waveform view", lambda: press(K_BROWSE, 0))
    shot("deck-view", "Deck / waveform view")
    step("Pad bank: BEAT LOOP (deck 1)", lambda: press(K_PAD_BEATLOOP, 1))
    shot("pads-beatloop-d1", "Pad bank: BEAT LOOP (deck 1)")
    step("Pad bank: SLIP LOOP (deck 1)", lambda: press(K_PAD_SLIP, 1))
    shot("pads-slip-d1", "Pad bank: SLIP LOOP (deck 1)")
    step("Pad bank: BEAT JUMP (deck 1)", lambda: press(K_PAD_BEATJUMP, 1))
    shot("pads-beatjump-d1", "Pad bank: BEAT JUMP (deck 1)")
    step("(restore) HOT CUE (deck 1)", lambda: press(K_PAD_HOTCUE, 1))

    # --- top-row screens ----------------------------------------------
    step("TAG LIST", lambda: press(K_TAGLIST, 0))
    shot("tag-list", "TAG LIST")
    step("SOURCE device picker", lambda: press(K_SOURCE, 0))
    shot("source-device-picker", "SOURCE -- device picker")
    step("LINK", lambda: press(K_LINK, 0))
    shot("link-select-source", "LINK")
    step("Library list (USB1, file name)", lambda: press(K_USB1, 0))
    shot("browse-library", "Library list (USB1, by file name)")
    step("Library + INFO panel", lambda: press(K_INFO, 0))
    shot("library-track-info", "Library list + INFO panel")

    # --- BROWSE sort tabs, by touch (left column, x~50) -------------------
    for slug, title, y in [
        ("browse-artist", "ARTIST", 124), ("browse-albums", "ALBUMS", 196),
        ("browse-key", "KEY", 268), ("browse-bitrate", "BITRATE", 340),
        ("browse-matching", "MATCHING", 412), ("browse-folder", "FOLDER", 484),
        ("browse-rec", "REC", 556), ("browse-track", "TRACK", 628),
    ]:
        step("BROWSE tab: %s" % title, lambda y=y: tap(50, y))
        shot(slug, "Library sorted by %s" % title)

    step("SEARCH (on-screen keyboard)", lambda: press(K_SEARCH, CH_GLOBAL))
    shot("search-keyboard", "SEARCH -- on-screen keyboard")
    step("(close) SEARCH", lambda: press(K_SEARCH, CH_GLOBAL))
    step("(reset) back to deck view", lambda: press(K_BROWSE, 0))

    # --- UTILITY: MENU held ~650ms opens it; 0x7f02 rotates the selector --
    step("UTILITY: DECK category", lambda: hold(K_MENU, 0, 650))
    shot("utility-deck-settings", "UTILITY -- DECK category")
    step("UTILITY: scroll to MIXER/GENERAL", lambda: rotate(40))
    shot("utility-general-settings", "UTILITY -- MIXER tail + GENERAL category")
    step("(exit) UTILITY via MENU hold", lambda: hold(K_MENU, 0, 650))
    step("(reset) back to deck view", lambda: press(K_BROWSE, 0))

    # --- STEMS / KEY panels (needs root for rx3-stems-touch.fifo) --------
    step("STEMS panel (deck 1)", lambda: stems_touch_send(0, STEMS_TOUCH_OPEN))
    shot("stems-panel-d1", "STEMS panel (deck 1)")
    step("KEY panel (deck 1)", lambda: stems_touch_send(0, STEMS_TOUCH_KEY_OPEN))
    shot("key-panel-d1", "KEY panel (deck 1)")
    # closing these two is not a clean toggle (see module docstring); the
    # pad-bank restore below forces the strip back regardless.

    # --- BEAT FX: real audio engage, pulsed straight back off ------------
    step("BEAT FX on", lambda: press(K_BFX, CH_GLOBAL))
    shot("beatfx-on", "BEAT FX engaged (DELAY, deck 1)")
    step("BEAT FX off", lambda: press(K_BFX, CH_GLOBAL))

    # --- cleanup: force the pad strip back to HOT CUE on both decks ------
    step("(cleanup) HOT CUE both decks", lambda: (press(K_PAD_HOTCUE, 1),
                                                    press(K_PAD_HOTCUE, 2)))

    print("\n%d screens captured into %s" % (len(manifest), out_dir))
    print("Not covered (see module docstring): PLAYLIST and USB STOP (the\n"
          "shim's own swipe-down top menu band -- upstream of the touch\n"
          "injection hook used here).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
