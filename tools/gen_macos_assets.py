#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the em34xx.app icons (original artwork) as 1024 px PNG masters.

Writes into src/frontend/cocoa/res/:
  em34xx.png    application icon: the em34xxw terminal glyph on the macOS
                rounded-square plate (824 px plate on a 1024 px canvas)
  profile.png   .em34xx document icon: the em34xxw document glyph

The glyphs come from tools/gen_win32_assets.py, so both front ends show the
same pictures. The build turns each PNG into an .icns with sips and iconutil
(src/frontend/cocoa/make_icns.cmake). Rendering in pure Python takes a minute.
Run from the repository root: python3 tools/gen_macos_assets.py
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_win32_assets as w  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "frontend", "cocoa", "res")


def placed(glyph, x0, y0, size):
    """The glyph's shapes moved into the square (x0, y0, size) of the unit canvas."""
    def move(shape):
        return lambda x, y: shape((x - x0) / size, (y - y0) / size)
    return [(move(shape), col) for shape, col in glyph]


def gradient(top, bottom, y0, y1):
    """A vertical gradient colour: a function of y."""
    def col(x, y):
        t = min(max((y - y0) / (y1 - y0), 0.0), 1.0)
        return tuple(int(round(a + (b - a) * t)) for a, b in zip(top, bottom))
    return col


def app_icon():
    # The plate: Apple's macOS 11 icon grid, 824/1024 with a 185/1024 corner.
    m, r = 100 / 1024, 185 / 1024
    shadow = w.rect(m, m + 0.012, 1 - m, 1 - m + 0.012, r)
    plate = w.rect(m, m, 1 - m, 1 - m, r)
    layers = [(shadow, (0, 0, 0, 70)),
              (plate, gradient((246, 243, 236, 255), (214, 207, 192, 255), m, 1 - m))]
    return layers + placed(w.g_terminal(), 0.20, 0.19, 0.60)


def document_icon():
    return w.g_document()


def render(layers, size, ss=2):
    """RGBA rows, top-down, straight alpha; colours may be functions of (x, y)."""
    rows = []
    n = ss * ss
    for j in range(size):
        row = bytearray()
        for i in range(size):
            r = g = b = a = 0.0
            for sj in range(ss):
                for si in range(ss):
                    x = (i + (si + 0.5) / ss) / size
                    y = (j + (sj + 0.5) / ss) / size
                    cr = cg = cb = ca = 0.0
                    for shape, col in layers:
                        if shape(x, y):
                            c = col(x, y) if callable(col) else col
                            sa = c[3] / 255.0
                            cr = c[0] * sa + cr * (1 - sa)
                            cg = c[1] * sa + cg * (1 - sa)
                            cb = c[2] * sa + cb * (1 - sa)
                            ca = sa + ca * (1 - sa)
                    r += cr
                    g += cg
                    b += cb
                    a += ca
            a /= n
            if a > 0:
                r, g, b = r / n / a, g / n / a, b / n / a
            row += bytes((int(round(min(r, 255))), int(round(min(g, 255))), int(round(min(b, 255))),
                          int(round(a * 255))))
        rows.append(bytes(row))
    return rows


def write_png(path, rows):
    h = len(rows)
    wd = len(rows[0]) // 4

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + row for row in rows)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", wd, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    os.makedirs(OUT, exist_ok=True)
    write_png(os.path.join(OUT, "em34xx.png"), render(app_icon(), 1024))
    write_png(os.path.join(OUT, "profile.png"), render(document_icon(), 1024))


if __name__ == "__main__":
    main()
