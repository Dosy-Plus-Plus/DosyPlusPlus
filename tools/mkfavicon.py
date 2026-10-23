#!/usr/bin/env python3
"""Draw the site icon, from the same letterforms the client draws.

The page had no icon, so every visitor's browser asked for /favicon.ico
and got a 404. This writes one rather than hand-drawing a PNG nobody can
edit: the glyphs come from src/ui/art.c's LOGO, so the tab and the splash
screen are the same wordmark.

Sixteen pixels will not hold "DOSy++", so the icon is the D and one plus,
on the dark green of a CP437 screen. 32x32 gets both pluses.

No PIL: PNG is a zlib stream with a four-byte length and a CRC per chunk,
and an ICO is a header plus whole PNG files. Writing them here keeps the
build dependency-free, which is the same reason tools/makemod.py writes a
MOD by hand.

    tools/mkfavicon.py            -> web/favicon.ico, web/favicon.png
"""
import os
import struct
import sys
import zlib

# From src/ui/art.c LOGO: the D is columns 0-3, and the plus is the
# five-wide cross at columns 20-24. '#' is a lit pixel.
D = ["###.",
     "#..#",
     "#..#",
     "#..#",
     "###."]
PLUS = ["..#..",
        "..#..",
        "#####",
        "..#..",
        "..#.."]

BG     = (0x0b, 0x0b, 0x0b)   # the page's own near-black
FG     = (0x2e, 0xa0, 0x43)   # and its green
BORDER = (0x14, 0x4a, 0x1f)


def blit(px, art, x0, y0, scale, colour, w, h):
    for y, row in enumerate(art):
        for x, c in enumerate(row):
            if c != '#':
                continue
            for dy in range(scale):
                for dx in range(scale):
                    px_x, px_y = x0 + x * scale + dx, y0 + y * scale + dy
                    if 0 <= px_x < w and 0 <= px_y < h:
                        px[px_y][px_x] = colour


def icon(size):
    px = [[BG for _ in range(size)] for _ in range(size)]

    for i in range(size):                      # a one-pixel frame
        px[0][i] = px[size - 1][i] = BORDER
        px[i][0] = px[i][size - 1] = BORDER

    # One plus, not two. Stacked pluses share a continuous vertical bar at
    # this size and read as a single tall glyph with two crossbars, which
    # is worse than saying "D+" and leaving the rest to the title.
    if size <= 16:
        blit(px, D, 2, 3, 2, FG, size, size)            # 8x10
        blit(px, PLUS, 10, 5, 1, FG, size, size)        # 5x5
    else:
        blit(px, D, 3, 6, 4, FG, size, size)            # 16x20
        blit(px, PLUS, 20, 11, 2, FG, size, size)       # 10x10
    return px


def png(px):
    h, w = len(px), len(px[0])
    raw = b"".join(b"\x00" + b"".join(bytes(p) for p in row) for row in px)

    def chunk(tag, data):
        c = tag + data
        return (struct.pack(">I", len(data)) + c
                + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def ico(images):
    """An ICO whose entries are whole PNG files, which every browser since
    IE11 accepts and which keeps this script to one encoder."""
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    blobs = []
    for size, blob in images:
        out += struct.pack("<BBBBHHII",
                           0 if size >= 256 else size,
                           0 if size >= 256 else size,
                           0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
        blobs.append(blob)
    return out + b"".join(blobs)


def main(argv):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    web = os.path.join(root, "web")
    os.makedirs(web, exist_ok=True)

    p16, p32 = png(icon(16)), png(icon(32))
    with open(os.path.join(web, "favicon.ico"), "wb") as f:
        f.write(ico([(16, p16), (32, p32)]))
    with open(os.path.join(web, "favicon.png"), "wb") as f:
        f.write(p32)

    for name in ("favicon.ico", "favicon.png"):
        path = os.path.join(web, name)
        print("  %-14s %4d bytes" % (name, os.path.getsize(path)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
