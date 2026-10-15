#!/usr/bin/env python3
"""bmp2png.py — convert a BMP (as written by fbshim.so's screenshot_tick(),
24-bit, top-down or bottom-up) to PNG, using only the stdlib (no Pillow/
ImageMagick on this board). One file in, one file out.

    python3 bmp2png.py in.bmp out.png
"""
import struct
import sys
import zlib


def bmp_to_rgb_rows(data):
    if data[0:2] != b"BM":
        raise ValueError("not a BMP")
    dataoff = struct.unpack_from("<I", data, 10)[0]
    hdrsize = struct.unpack_from("<I", data, 14)[0]
    if hdrsize < 40:
        raise ValueError("unsupported BMP header size %d" % hdrsize)
    width = struct.unpack_from("<i", data, 18)[0]
    height = struct.unpack_from("<i", data, 22)[0]
    bitcount = struct.unpack_from("<H", data, 28)[0]
    if bitcount != 24:
        raise ValueError("unsupported bitcount %d (only 24-bit)" % bitcount)
    top_down = height < 0
    height = abs(height)
    rowbytes = (width * 3 + 3) & ~3
    rows = []
    for y in range(height):
        off = dataoff + y * rowbytes
        row = data[off:off + width * 3]
        rgb = bytearray(width * 3)
        for x in range(width):
            b, g, r = row[x * 3], row[x * 3 + 1], row[x * 3 + 2]
            rgb[x * 3:x * 3 + 3] = bytes((r, g, b))
        rows.append(bytes(rgb))
    if not top_down:
        rows.reverse()
    return width, height, rows


def write_png(path, width, height, rows):
    def chunk(tag, payload):
        c = tag + payload
        return struct.pack(">I", len(payload)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    raw = bytearray()
    for row in rows:
        raw.append(0)  # filter: none
        raw.extend(row)
    idat = zlib.compress(bytes(raw), 9)
    with open(path, "wb") as f:
        f.write(sig)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    width, height, rows = bmp_to_rgb_rows(data)
    write_png(sys.argv[2], width, height, rows)
    print("%s: %dx%d -> %s" % (sys.argv[1], width, height, sys.argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
