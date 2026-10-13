#!/usr/bin/env python3
"""Split legacy RX3STM2 stems into the rx3-toolkit's RX3STM1 role files.

The legacy KEY/STEMS core (RX3-RK3399-DDJ400, overlay key-stems-0cb402e0) reads one
RX3STM2 file per track: a 64-byte header, then the DRUMS payload, then the VOCAL
payload, same frame count each (its tools/make-sidecar-v2.py). The toolkit's core
accepts RX3STM1 role files (`<base>.rx3stem` = vocals, `<base>.rx3drums` = drums,
INST rebuilt on the deck from the original mix) or an RX3PKG2 package -- not
RX3STM2. Both formats share the header struct, so this is a split, not a
re-separation: each payload is copied unchanged under an RX3STM1 header.

Writes into a separate directory: the legacy files stay as they are, for the
legacy core. Each output is written to `.partial` and renamed, as the toolkit does.
"""
import argparse
import os
import pathlib
import struct
import sys

HEADER = struct.Struct("<8sIIIIQ32s")
FRAME = {2: 4}          # format id 2 = s16 interleaved stereo (gain 1.0, reserved 0)


def split(src, out_dir):
    with open(src, "rb") as f:
        raw = f.read(HEADER.size)
        magic, rate, ch, fmt, hsize, frames, reserved = HEADER.unpack(raw)
        if magic != b"RX3STM2\0":
            return "skip (not RX3STM2: %r)" % magic
        if rate != 44100 or ch != 2 or hsize != 64 or fmt not in FRAME or any(reserved):
            return "skip (rate %d ch %d fmt %d hsize %d)" % (rate, ch, fmt, hsize)
        part = frames * FRAME[fmt]
        if os.path.getsize(src) != HEADER.size + 2 * part:
            return "skip (size %d, expected %d)" % (os.path.getsize(src), HEADER.size + 2 * part)
        head = HEADER.pack(b"RX3STM1\0", rate, ch, fmt, HEADER.size, frames, b"\0" * 32)
        base = src.name[:-len(".rx3stem")]
        # DRUMS first in the legacy file, VOCAL second.
        for suffix in (".rx3drums", ".rx3stem"):
            dst = out_dir / (base + suffix)
            tmp = out_dir / (base + suffix + ".partial")
            with open(tmp, "wb") as o:
                o.write(head)
                left = part
                while left:
                    chunk = f.read(min(left, 8 << 20))
                    if not chunk:
                        raise IOError("short read in %s" % src)
                    o.write(chunk)
                    left -= len(chunk)
            if os.path.getsize(tmp) != HEADER.size + part:
                raise IOError("bad size for %s" % tmp)
            os.replace(tmp, dst)
    return "ok (%d frames, %.1f s)" % (frames, frames / 44100.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src", type=pathlib.Path, help="directory of legacy RX3STM2 .rx3stem files")
    ap.add_argument("dst", type=pathlib.Path, help="output directory (created)")
    a = ap.parse_args()
    a.dst.mkdir(parents=True, exist_ok=True)
    bad = 0
    for src in sorted(a.src.glob("*.rx3stem")):
        if src.name.startswith("._"):
            continue                # macOS metadata, not a stem
        r = split(src, a.dst)
        bad += not r.startswith("ok")
        print("%-60s %s" % (src.name, r))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
