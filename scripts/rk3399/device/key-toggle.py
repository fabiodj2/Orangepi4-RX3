#!/usr/bin/env python3
"""key-toggle.py -- keyboard hotkey for the player: hold DEL for 5 s to stop it
(console back) or start it again.

Run by rx3-key-toggle.service, as root, outside the player's own service so it
keeps listening while the player is down. It does not stop or start anything
itself: it creates /run/rx3-interface-toggle.request, the same request the
DDJ-400's interface gesture makes (ddj400-bridge.c), and rx3-interface-toggle.path
runs toggle-interface.sh, which decides OFF or ON from whether rbp is running.

Every keyboard that has the key is read, including ones plugged in later (the
device list is rescanned every few seconds). Nothing grabs the devices, so the
key still reaches whatever else reads them.

Environment: RX3_TOGGLE_KEY (Linux key code, default 111 = KEY_DELETE),
RX3_TOGGLE_HOLD_S (default 5).
"""
import os
import select
import struct
import sys
import time

KEY = int(os.environ.get("RX3_TOGGLE_KEY", "111"))
HOLD_S = float(os.environ.get("RX3_TOGGLE_HOLD_S", "5"))
REQUEST = "/run/rx3-interface-toggle.request"
RESCAN_S = 3.0
COOLDOWN_S = 10.0  # after a toggle: a start takes ~20 s, a second one would undo it

EV_KEY = 1
EVENT = struct.Struct("llHHi")  # struct input_event, 64-bit timeval


def log(msg):
    print(msg, flush=True)


def keyboards():
    """/dev/input/eventN of every device whose KEY bitmap has KEY."""
    found = []
    try:
        blocks = open("/proc/bus/input/devices").read().split("\n\n")
    except OSError:
        return found
    for block in blocks:
        handlers, keybits, name = [], None, "?"
        for line in block.splitlines():
            if line.startswith("N: Name="):
                name = line[8:].strip('"')
            elif line.startswith("H: Handlers="):
                handlers = line[12:].split()
            elif line.startswith("B: KEY="):
                keybits = line[7:].split()
        if not keybits:
            continue
        # Words are printed most significant first, one unsigned long each.
        bits = 0
        for word in keybits:
            bits = (bits << 64) | int(word, 16)
        if not bits >> KEY & 1:
            continue
        for h in handlers:
            if h.startswith("event"):
                found.append(("/dev/input/" + h, name))
    return found


def request_toggle():
    with open(REQUEST, "w"):
        pass


def main():
    fds = {}  # fd -> path
    paths = {}  # path -> fd
    pressed_at = None
    fired = False
    quiet_until = 0.0
    next_scan = 0.0
    log(f"key-toggle: key {KEY}, hold {HOLD_S:g} s -> {REQUEST}")
    while True:
        now = time.monotonic()
        if now >= next_scan:
            next_scan = now + RESCAN_S
            for path, name in keyboards():
                if path in paths:
                    continue
                try:
                    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
                except OSError as e:
                    log(f"key-toggle: {path}: {e}")
                    continue
                fds[fd], paths[path] = path, fd
                log(f"key-toggle: listening on {path} ({name})")
        timeout = 0.2 if pressed_at is not None else RESCAN_S
        try:
            ready, _, _ = select.select(list(fds), [], [], timeout)
        except (OSError, ValueError):
            ready = []
        for fd in ready:
            try:
                data = os.read(fd, EVENT.size * 64)
            except BlockingIOError:
                continue
            except OSError:  # unplugged
                path = fds.pop(fd)
                paths.pop(path, None)
                os.close(fd)
                log(f"key-toggle: {path} gone")
                pressed_at = None
                continue
            for off in range(0, len(data) - EVENT.size + 1, EVENT.size):
                _, _, etype, code, value = EVENT.unpack_from(data, off)
                if etype != EV_KEY or code != KEY:
                    continue
                if value == 1:  # press; 2 = autorepeat, ignored
                    pressed_at, fired = time.monotonic(), False
                elif value == 0:
                    pressed_at = None
        now = time.monotonic()
        if pressed_at is not None and not fired and now - pressed_at >= HOLD_S:
            fired = True
            if now < quiet_until:
                log("key-toggle: held, but a toggle ran less than "
                    f"{COOLDOWN_S:g} s ago: ignored")
                continue
            quiet_until = now + COOLDOWN_S
            log(f"key-toggle: key {KEY} held {HOLD_S:g} s: toggle requested")
            try:
                request_toggle()
            except OSError as e:
                log(f"key-toggle: {REQUEST}: {e}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
