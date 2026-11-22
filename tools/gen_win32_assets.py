#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the em34xxw icons and toolbar bitmaps (original artwork).

Writes into src/frontend/win32/res/:
  em34xxw.ico      application icon: a terminal (16, 24, 32, 48, 64 px)
  profile.ico      .em34xx document icon (16, 32, 48 px)
  toolbar16.bmp    toolbar strip, 8 glyphs, 32-bit BGRA with alpha
  toolbar24.bmp    the same at 24 px (picked at 120-167 dpi)
  toolbar32.bmp    the same at 32 px (168 dpi and more)
  status16.bmp     status bar strip (16, 24, 32 px like the toolbar): session
  status24.bmp     orbs and TLS locks
  status32.bmp

The glyphs are drawn from simple shapes in unit coordinates with 4x4
supersampling, so every size is rendered, not scaled. Toolbar order (must
match the image indices in src/frontend/win32/win32_frontend.cpp):
  0 connect to profile, 1 new connection, 2 save, 3 disconnect, 4 settings,
  5 keymap, 6 reconnect, 7 reboot (power cycle)
Status strip order: 0 orb green, 1 orb grey, 2 orb red, 3 orb orange,
  4 lock closed, 5 lock open, 6 lock closed (dimmed), 7 lock open (dimmed)
Run from the repository root: python3 tools/gen_win32_assets.py
"""
import math
import os
import struct

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "frontend", "win32", "res")

# --- shapes: functions (x, y) -> bool in unit coordinates -------------------


def rect(x0, y0, x1, y1, r=0.0):
    def f(x, y):
        if x < x0 or x > x1 or y < y0 or y > y1:
            return False
        if r <= 0:
            return True
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return f


def circle(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def ring(cx, cy, r0, r1):
    return lambda x, y: r0 * r0 <= (x - cx) ** 2 + (y - cy) ** 2 <= r1 * r1


def line(x0, y0, x1, y1, w):
    dx, dy = x1 - x0, y1 - y0
    ll = dx * dx + dy * dy

    def f(x, y):
        t = max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / ll))
        px, py = x0 + t * dx, y0 + t * dy
        return (x - px) ** 2 + (y - py) ** 2 <= (w / 2) ** 2
    return f


def poly(points):
    def f(x, y):
        inside = False
        n = len(points)
        for i in range(n):
            ax, ay = points[i]
            bx, by = points[(i + 1) % n]
            if (ay > y) != (by > y) and x < (bx - ax) * (y - ay) / (by - ay) + ax:
                inside = not inside
        return inside
    return f


def union(*fs):
    return lambda x, y: any(f(x, y) for f in fs)


def minus(a, b):
    return lambda x, y: a(x, y) and not b(x, y)


# --- glyphs: lists of (shape, (r, g, b, a)) painted in order ----------------

DARK = (40, 44, 52, 255)
WHITE = (250, 250, 250, 255)
GREEN = (46, 160, 67, 255)
RED = (205, 45, 45, 255)
BLUE = (37, 99, 175, 255)
GREY = (150, 156, 166, 255)
LIGHT = (220, 224, 230, 255)
AMBER = (230, 160, 40, 255)


def outlined(shape_outer, shape_inner, edge, fill):
    return [(shape_outer, edge), (shape_inner, fill)]


def g_profiles():
    back = rect(0.30, 0.06, 0.94, 0.70, 0.06)
    back_in = rect(0.36, 0.12, 0.88, 0.64, 0.03)
    front = rect(0.06, 0.24, 0.72, 0.94, 0.06)
    front_in = rect(0.12, 0.30, 0.66, 0.88, 0.03)
    lines = union(rect(0.20, 0.40, 0.58, 0.47), rect(0.20, 0.55, 0.58, 0.62), rect(0.20, 0.70, 0.46, 0.77))
    return outlined(back, back_in, DARK, LIGHT) + outlined(front, front_in, DARK, WHITE) + [(lines, BLUE)]


def plug(dx=0.0, dy=0.0, color=GREEN):
    body = rect(0.30 + dx, 0.38 + dy, 0.70 + dx, 0.70 + dy, 0.06)
    prongs = union(rect(0.38 + dx, 0.10 + dy, 0.46 + dx, 0.40 + dy), rect(0.54 + dx, 0.10 + dy, 0.62 + dx, 0.40 + dy))
    cable = rect(0.45 + dx, 0.68 + dy, 0.55 + dx, 0.96 + dy)
    return [(prongs, DARK), (cable, DARK), (body, DARK), (rect(0.35 + dx, 0.43 + dy, 0.65 + dx, 0.65 + dy, 0.03), color)]


def g_connect():
    return plug()


def g_new_connection():
    badge = union(rect(0.66, 0.04, 0.78, 0.40), rect(0.54, 0.16, 0.90, 0.28))
    return plug(dx=-0.12) + [(circle(0.72, 0.22, 0.24), WHITE), (circle(0.72, 0.22, 0.20), GREEN),
                             (badge, WHITE)]


def g_reboot():
    # The power symbol: a broken ring and a bar through the gap.
    ring_shape = minus(ring(0.5, 0.54, 0.24, 0.38), poly([(0.5, 0.54), (0.26, 0.0), (0.74, 0.0)]))
    bar = rect(0.43, 0.06, 0.57, 0.50, 0.05)
    return [(ring_shape, DARK), (bar, WHITE), (rect(0.45, 0.08, 0.55, 0.48, 0.04), AMBER),
            (minus(ring(0.5, 0.54, 0.27, 0.35), poly([(0.5, 0.54), (0.22, 0.0), (0.78, 0.0)])), AMBER)]


def g_orb(color):
    r, g, b, a = color
    dark = (int(r * 0.6), int(g * 0.6), int(b * 0.6), 255)
    light = (min(255, r + 90), min(255, g + 90), min(255, b + 90), 255)
    return [(circle(0.5, 0.5, 0.36), dark), (circle(0.5, 0.5, 0.30), color),
            (circle(0.42, 0.40, 0.11), light)]


def g_lock(closed, dim=False):
    edge = (150, 156, 166, 255) if dim else DARK
    body_col = (214, 218, 224, 255) if dim else AMBER if closed else GREY
    if closed:
        shackle = minus(ring(0.5, 0.40, 0.15, 0.25), rect(0.0, 0.40, 1.0, 1.0))
        legs = union(rect(0.25, 0.40, 0.35, 0.50), rect(0.65, 0.40, 0.75, 0.50))
    else:
        shackle = minus(ring(0.66, 0.30, 0.15, 0.25), rect(0.0, 0.30, 1.0, 1.0))
        legs = rect(0.81, 0.30, 0.91, 0.42)
    body = rect(0.18, 0.46, 0.82, 0.92, 0.06)
    return [(union(shackle, legs), edge), (body, edge), (rect(0.23, 0.51, 0.77, 0.87, 0.04), body_col),
            (rect(0.46, 0.60, 0.54, 0.78, 0.02), edge)]


def g_save():
    outer = rect(0.08, 0.08, 0.92, 0.92, 0.06)
    shape = minus(outer, poly([(0.80, 0.07), (0.93, 0.07), (0.93, 0.20)]))
    return [(shape, DARK), (rect(0.14, 0.14, 0.86, 0.86, 0.03), BLUE),
            (rect(0.26, 0.14, 0.70, 0.40), LIGHT), (rect(0.56, 0.18, 0.64, 0.36), DARK),
            (rect(0.22, 0.54, 0.78, 0.86, 0.02), WHITE),
            (union(rect(0.30, 0.62, 0.70, 0.66), rect(0.30, 0.72, 0.70, 0.76)), GREY)]


def g_disconnect():
    return plug(dy=0.0, color=GREY) + [(line(0.14, 0.14, 0.86, 0.86, 0.16), WHITE),
                                       (line(0.16, 0.16, 0.84, 0.84, 0.11), RED)]


def g_settings():
    teeth = []
    for i in range(8):
        a = i * math.pi / 4
        c, s = math.cos(a), math.sin(a)
        teeth.append(line(0.5 + 0.30 * c, 0.5 + 0.30 * s, 0.5 + 0.44 * c, 0.5 + 0.44 * s, 0.16))
    gear = union(circle(0.5, 0.5, 0.33), *teeth)
    return [(gear, DARK), (circle(0.5, 0.5, 0.27), GREY), (circle(0.5, 0.5, 0.13), DARK),
            (circle(0.5, 0.5, 0.08), WHITE)]


def g_keymap():
    board = rect(0.04, 0.22, 0.96, 0.80, 0.07)
    keys = []
    for row, (y0, n, x0) in enumerate([(0.30, 6, 0.12), (0.44, 6, 0.15), (0.58, 0, 0.0)]):
        for k in range(n):
            x = x0 + k * 0.125
            keys.append(rect(x, y0, x + 0.09, y0 + 0.09, 0.015))
    space = rect(0.26, 0.60, 0.74, 0.69, 0.015)
    return [(board, DARK), (rect(0.08, 0.26, 0.92, 0.76, 0.05), GREY), (union(space, *keys), WHITE),
            (rect(0.78, 0.60, 0.88, 0.69, 0.015), AMBER)]


def g_terminal():
    bezel = rect(0.04, 0.06, 0.96, 0.76, 0.08)
    screen = rect(0.12, 0.13, 0.88, 0.66, 0.03)
    text = union(rect(0.18, 0.21, 0.62, 0.26), rect(0.18, 0.32, 0.80, 0.37), rect(0.18, 0.43, 0.52, 0.48),
                 rect(0.18, 0.54, 0.30, 0.59))
    stand = poly([(0.40, 0.74), (0.60, 0.74), (0.68, 0.90), (0.32, 0.90)])
    base = rect(0.22, 0.88, 0.78, 0.96, 0.03)
    return [(stand, DARK), (base, DARK), (bezel, DARK), (rect(0.07, 0.09, 0.93, 0.73, 0.06), (214, 206, 186, 255)),
            (screen, (12, 28, 16, 255)), (text, (80, 230, 110, 255))]


def g_document():
    page = poly([(0.16, 0.04), (0.64, 0.04), (0.86, 0.26), (0.86, 0.96), (0.16, 0.96)])
    page_in = poly([(0.21, 0.09), (0.62, 0.09), (0.81, 0.28), (0.81, 0.91), (0.21, 0.91)])
    fold = poly([(0.62, 0.06), (0.62, 0.28), (0.84, 0.28)])
    mon = [(rect(0.28, 0.40, 0.74, 0.74, 0.04), DARK), (rect(0.32, 0.44, 0.70, 0.70, 0.02), (12, 28, 16, 255)),
           (union(rect(0.36, 0.50, 0.60, 0.53), rect(0.36, 0.57, 0.66, 0.60)), (80, 230, 110, 255)),
           (rect(0.42, 0.74, 0.60, 0.82), DARK)]
    return [(page, DARK), (page_in, WHITE), (fold, GREY)] + mon


# --- rendering ---------------------------------------------------------------

def render(glyph, size, ss=4):
    """BGRA rows, top-down, straight alpha."""
    px = []
    for j in range(size):
        row = []
        for i in range(size):
            r = g = b = a = 0.0
            for sj in range(ss):
                for si in range(ss):
                    x = (i + (si + 0.5) / ss) / size
                    y = (j + (sj + 0.5) / ss) / size
                    cr = cg = cb = ca = 0.0
                    for shape, col in glyph:
                        if shape(x, y):
                            sa = col[3] / 255.0
                            cr = col[0] * sa + cr * (1 - sa)
                            cg = col[1] * sa + cg * (1 - sa)
                            cb = col[2] * sa + cb * (1 - sa)
                            ca = sa + ca * (1 - sa)
                    r += cr
                    g += cg
                    b += cb
                    a += ca
            n = ss * ss
            a /= n
            if a > 0:
                r, g, b = r / n / a, g / n / a, b / n / a
            row.append((int(round(min(b, 255))), int(round(min(g, 255))), int(round(min(r, 255))),
                        int(round(a * 255))))
        px.append(row)
    return px


def dib(rows, mask=False):
    """BITMAPINFOHEADER + 32-bit bottom-up pixels (+ AND mask for icons)."""
    h = len(rows)
    w = len(rows[0])
    data = bytearray()
    for row in reversed(rows):
        for (b, g, r, a) in row:
            data += bytes((b, g, r, a))
    and_mask = bytearray()
    if mask:
        stride = ((w + 31) // 32) * 4
        for row in reversed(rows):
            bits = bytearray(stride)
            for i, (_, _, _, a) in enumerate(row):
                if a == 0:
                    bits[i // 8] |= 0x80 >> (i % 8)
            and_mask += bits
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2 if mask else h, 1, 32, 0, len(data) + len(and_mask), 2835,
                         2835, 0, 0)
    return header + bytes(data) + bytes(and_mask)


def write_ico(path, glyph, sizes):
    images = [dib(render(glyph, s), mask=True) for s in sizes]
    out = bytearray(struct.pack("<HHH", 0, 1, len(sizes)))
    offset = 6 + 16 * len(sizes)
    for s, img in zip(sizes, images):
        out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    for img in images:
        out += img
    with open(path, "wb") as f:
        f.write(out)


def write_strip(path, glyphs, size):
    tiles = [render(g, size) for g in glyphs]
    rows = [sum((t[j] for t in tiles), []) for j in range(size)]
    body = dib(rows)
    with open(path, "wb") as f:
        f.write(struct.pack("<2sIHHI", b"BM", 14 + len(body), 0, 0, 14 + 40) + body)


def main():
    os.makedirs(OUT, exist_ok=True)
    write_ico(os.path.join(OUT, "em34xxw.ico"), g_terminal(), [16, 24, 32, 48, 64])
    write_ico(os.path.join(OUT, "profile.ico"), g_document(), [16, 32, 48])
    glyphs = [g_profiles(), g_new_connection(), g_save(), g_disconnect(), g_settings(), g_keymap(), g_connect(),
              g_reboot()]
    status = [g_orb(GREEN), g_orb(GREY), g_orb(RED), g_orb(AMBER), g_lock(True), g_lock(False),
              g_lock(True, True), g_lock(False, True)]
    for s in (16, 24, 32):
        write_strip(os.path.join(OUT, "toolbar%d.bmp" % s), glyphs, s)
        write_strip(os.path.join(OUT, "status%d.bmp" % s), status, s)


if __name__ == "__main__":
    main()
