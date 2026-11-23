#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the em34xxw icons and toolbar bitmaps (original artwork).

Writes into src/frontend/win32/res/:
  em34xxw.ico      application icon: a 3477 as Windows 9x pixel art, drawn
                   per size (16, 24, 32, 48, 64 px), and the 64 px
                   drawing four times over at 256 px for Explorer's large views
                   (stored uncompressed like the others, which windres is sure
                   to take; a PNG entry would be smaller)
  profile.ico      .em34xx document icon (16, 32, 48 px)
  toolbar16.bmp    toolbar strip, 8 glyphs, 32-bit BGRA with alpha
  toolbar24.bmp    the same at 24 px (picked at 120-167 dpi)
  toolbar32.bmp    the same at 32 px (168 dpi and more)
  status16.bmp     status bar strip (16, 24, 32 px like the toolbar): session
  status24.bmp     orbs and TLS locks
  status32.bmp

The glyphs are drawn from simple shapes in unit coordinates with 4x4
supersampling, so every size is rendered, not scaled; the application icon
is pixel art instead (terminal_pixels). Toolbar order (must
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


# --- the application icon: pixel art -----------------------------------------
#
# A 3477 behind its keyboard, in the manner of a Windows 9x icon: one
# drawing per size on the pixel grid, a black outline, a short fixed palette,
# light from the top left and no anti-aliasing. Original artwork after
# photographs of the terminal: beige cabinet, broad taupe bezel, the control
# strip under the tube (grille, two blue keys, the power switch and its light) and the
# 122-key keyboard in front, which hides the stand. The bezel block stands proud of the cabinet: the rear housing behind it
# is lower, and the chassis with the control strip runs under both to the back. The badge is a blank plate; no maker's mark is drawn.

PX_K = (0, 0, 0, 255)          # outline
PX_W = (255, 251, 240, 255)    # highlight (the Windows palette's cream)
PX_B = (216, 208, 184, 255)    # cabinet beige
PX_S = (168, 160, 136, 255)    # cabinet in shadow
PX_T = (140, 132, 112, 255)    # bezel taupe
PX_D = (96, 90, 76, 255)       # bezel in shadow
PX_G = (12, 36, 22, 255)       # the tube, dark
PX_Y = (64, 240, 96, 255)      # phosphor, bright: green-screen green
PX_O = (28, 128, 56, 255)      # phosphor, dim
PX_U = (40, 88, 184, 255)      # the blue keys
PX_N = (0, 0, 128, 255)        # badge

# Per size: front face, the side (depth of the bezel block, depth of the rear
# housing, how far the rear housing's top steps down), cabinet rim, bezel widths (side,
# top, bottom), control strip and the details, all in pixels.
# kbd is the keyboard: its box, how far the terminal moves up for it (kbd_dy)
# and to the right, so that the keyboard's left end stands clear (term_dx),
# the shear of its top (kbd_slope: rows to a pixel, 0 for none), the rows of
# its front face and so of its right side too (kbd_face), the rows of keys, the bank of function keys above
# the main block (first row, last row, first column, last column) and the
# blocks of keys as column ranges; or kbd_caps, every key by itself. The key
# columns are those of the near edge, before the shear.
# Text rows are (row, first column, last column, colour) inside the screen:
# a host menu (title, numbered options, rules, function keys, status row).
def _keycaps_64():
    """The 64 px keyboard key by key, before it is sheared: wells (the dark
    ground of each block of keys) and caps (x0, x1, row, colour), one pixel a
    key at a pitch of two. White keys and the grey ones of a 122-key keyboard,
    the space bar, and the cursor keys as a cross."""
    white, grey = PX_W, PX_S
    rows = [50, 52, 54, 56, 58]
    wells = [(2, 49, 6, 59), (8, 49, 36, 59), (46, 49, 55, 59),
             # The cursor block: its two full rows, then only the cross.
             (38, 49, 44, 53), (40, 53, 42, 59), (38, 55, 44, 57)]
    caps = []
    # The function keys: 2 rows of 12 with no gaps, three groups of four that
    # differ in colour only (grey, white, grey), dark ground all round.
    wells.append((10, 43, 34, 47))
    for k in range(12):
        x = 11 + 2 * k
        caps += [(x, x, y, white if k // 4 == 1 else grey) for y in (44, 46)]
    for y in rows:
        caps += [(x, x, y, grey) for x in (3, 5)]       # the ten keys at the left
        # The numeric pad: its top row and its right column are grey keys.
        caps += [(x, x, y, grey if y == 50 or x == 53 else white) for x in (47, 49, 51, 53)]
        for x in range(9, 36, 2):
            if y == 58 and x not in (9, 13, 31, 35):
                continue                                # the space bar and the gaps
            is_grey = x in (9, 35) or y == 58       # one column of grey keys at each end
            caps.append((x, x, y, grey if is_grey else white))
    # The bottom row: key, a gap one key wide, key, the space bar, key, gap, key.
    # The gaps are the keyboard's own colour, open to its near margin.
    caps.append((15, 29, 58, white))
    # The pad's tall key: the last two keys of its right column joined. Its
    # lower end is a column to the right here, so that after the shear the key
    # is one upright bar; the well is a column wider on the right to keep dark
    # ground all round it.
    caps += [(53, 53, 57, grey), (53, 53, 58, PX_D), (54, 54, 58, grey)]
    caps += [(x, x, y, PX_B) for x in (11, 33) for y in (58, 59)]
    for y, xs in ((50, (39, 41, 43)), (52, (39, 41, 43)), (54, (41,)), (56, (39, 41, 43)), (58, (41,))):
        caps += [(x, x, y, grey) for x in xs]       # the cursor block is all grey keys
    # One column to the left: the far edge reaches a column less far right
    # now that the keyboard is a row deeper.
    wells = [(x0 - 1, y0, x1 - 1, y1) for x0, y0, x1, y1 in wells]
    caps = [(x0 - 1, x1 - 1, y, col) for x0, x1, y, col in caps]
    return wells, caps


TERMINAL_LAYOUT = {
    # 16 px is drawn square on: no side, and a one-pixel bevelled rim for the
    # whole of the cabinet front, so the screen is as large as it can be.
    16: dict(front=(1, 0, 14, 10), side=(0, 0, 0), rim=1, bezel=(0, 0, 0), strip=(3, 10, 12, 12),
             round=False, badge=None,
             text=[(1, 1, 5, PX_Y), (3, 1, 7, PX_Y), (5, 1, 3, PX_Y)],
             grille=[], keys=[(5, 5), (7, 7)], switch=(10, 10), strip_rows=(11, 11),
             kbd=(0, 13, 15, 15), kbd_dy=0, kbd_slope=0, kbd_face=0, kbd_keys=(14, 14), kbd_f=None,
             kbd_groups=[(1, 2), (4, 9), (11, 11), (13, 14)]),
    24: dict(front=(1, 1, 18, 16), side=(2, 2, 2), rim=1, bezel=(1, 1, 2), strip=(4, 16, 18, 19),
             round=False, badge=None,
             text=[(1, 1, 2, PX_Y), (1, 5, 7, PX_Y), (3, 2, 2, PX_Y), (3, 4, 8, PX_Y), (5, 1, 10, PX_O),
                   (7, 1, 3, PX_Y), (7, 5, 7, PX_Y)],
             grille=[6], keys=[(8, 8), (10, 10)], switch=(12, 13), led=(15, 15, 17), strip_rows=(17, 17),
             kbd=(0, 20, 23, 23), kbd_dy=0, kbd_slope=1, kbd_face=0, kbd_keys=(21, 22), kbd_f=None,
             kbd_groups=[(2, 3), (5, 12), (14, 15), (17, 19)]),
    32: dict(front=(2, 1, 24, 21), side=(3, 3, 3), rim=1, bezel=(2, 2, 3), strip=(5, 21, 24, 25),
             round=True, badge=(5, 3, 7),
             text=[(1, 1, 3, PX_Y), (1, 6, 9, PX_Y), (3, 3, 3, PX_Y), (3, 5, 10, PX_Y), (5, 3, 3, PX_Y),
                   (5, 5, 8, PX_Y), (7, 1, 13, PX_O), (9, 1, 3, PX_Y), (9, 5, 8, PX_Y), (10, 12, 13, PX_O)],
             grille=[7, 9], keys=[(11, 12), (14, 15)], switch=(17, 19), led=(21, 21, 23), strip_rows=(22, 23),
             kbd=(0, 26, 31, 31), kbd_dy=0, kbd_slope=1, kbd_face=1, kbd_keys=(27, 29), kbd_f=None,
             kbd_groups=[(2, 3), (5, 17), (19, 21), (23, 26)]),
    48: dict(front=(3, 2, 36, 32), side=(4, 5, 5), rim=2, bezel=(3, 3, 4), strip=(8, 32, 36, 38),
             round=True, badge=(8, 6, 12),
             text=[(1, 1, 4, PX_Y), (1, 9, 14, PX_Y), (3, 1, 9, PX_Y), (5, 3, 3, PX_Y), (5, 5, 12, PX_Y),
                   (7, 3, 3, PX_Y), (7, 5, 9, PX_Y), (9, 3, 3, PX_Y), (9, 5, 11, PX_Y), (9, 14, 18, PX_Y),
                   (11, 1, 20, PX_O), (13, 1, 4, PX_Y), (13, 6, 10, PX_Y), (13, 12, 16, PX_Y), (15, 1, 20, PX_O),
                   (16, 1, 1, PX_Y), (16, 18, 20, PX_O)],
             grille=[10, 12, 14], keys=[(17, 19), (21, 23)], switch=(25, 28), led=(30, 30, 35), strip_rows=(34, 36),
             kbd=(0, 37, 47, 47), kbd_dy=2, kbd_slope=2, kbd_face=2, kbd_keys=(41, 44), kbd_f=(38, 39, 7, 26),
             kbd_groups=[(2, 4), (7, 26), (29, 33), (36, 41)]),
    # 64 px has room for the keyboard key by key (kbd_caps); the larger sizes
    # are enlargements of this drawing. The keyboard is the full width and the
    # terminal smaller behind it, its left side over the first function key.
    64: dict(front=(14, 0, 50, 34), side=(5, 6, 6), rim=2, bezel=(3, 4, 4), strip=(19, 34, 50, 40),
             round=True, badge=(19, 4, 24),
             text=[(1, 1, 5, PX_Y), (1, 10, 16, PX_Y), (3, 1, 11, PX_Y), (5, 4, 4, PX_Y), (5, 6, 14, PX_Y),
                   (7, 4, 4, PX_Y), (7, 6, 10, PX_Y), (9, 4, 4, PX_Y), (9, 6, 13, PX_Y), (9, 16, 21, PX_Y),
                   (12, 1, 3, PX_Y), (12, 5, 23, PX_O), (15, 1, 5, PX_Y), (15, 7, 12, PX_Y), (15, 14, 19, PX_Y),
                   (17, 1, 23, PX_O), (19, 1, 1, PX_Y), (19, 20, 23, PX_O)],
             grille=[21, 23, 25, 27], keys=[(30, 33), (35, 38)], switch=(40, 44), led=(46, 47, 37),
             strip_rows=(36, 38),
             kbd=(0, 41, 63, 63), kbd_dy=0, kbd_slope=3, kbd_face=2, kbd_caps=_keycaps_64()),
}


def terminal_pixels(n):
    """The terminal at n x n: rows of colours, None where transparent."""
    if n not in TERMINAL_LAYOUT:
        return enlarged_terminal(n)
    lay = TERMINAL_LAYOUT[n]
    g = [[None] * n for _ in range(n)]

    def box(x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                g[y][x] = c

    def frame(x0, y0, x1, y1, c):
        box(x0, y0, x1, y0, c)
        box(x0, y1, x1, y1, c)
        box(x0, y0, x0, y1, c)
        box(x1, y0, x1, y1, c)

    # The control strip under the tube.
    x0, y0, x1, y1 = lay["strip"]
    box(x0, y0, x1, y1, PX_K)
    box(x0 + 1, y0 + 1, x1 - 1, y1 - 1, PX_B)
    r0, r1 = lay["strip_rows"]
    if y1 - 1 > r1:
        box(x0 + 1, y1 - 1, x1 - 1, y1 - 1, PX_S)
    if r0 > y0 + 1:
        box(x0 + 1, y0 + 1, x1 - 1, r0 - 1, PX_S)      # in the shadow of the bezel block
    for x in lay["grille"]:
        box(x, r0, x, r1, PX_D)
    for k0, k1 in lay["keys"]:
        box(k0, r0, k1, r1, PX_U)
    k0, k1 = lay["switch"]
    box(k0, r0, k1, r1, PX_W)
    if "led" in lay:
        # The power light beside the switch, lit (first column, last column, row).
        l0, l1, ly = lay["led"]
        box(l0, ly, l1, ly, PX_Y)

    # The side, falling away to the right. The bezel is a block that stands
    # proud at the front; behind it the rear housing is lower, and under both
    # the chassis that carries the control strip runs through to the back.
    fx0, fy0, fx1, fy1 = lay["front"]
    d1, d2, step = lay["side"]
    chassis = lay["strip"][3]
    for i in range(1, d1 + d2 + 2 if d1 else 1):
        inset = i // 2      # the edges leave from the front's own corners
        x = fx1 + i
        # The chassis' lower edge climbs steeply from the strip's corner, then
        # runs back a little below the underside of the bezel block.
        under = fy1 - inset
        bottom = max(under + 2, chassis - 2 * i)
        if i <= d1:
            top = fy0 + inset
            box(x, top, x, under, PX_B if i == 1 and d1 > 1 else PX_S)
            g[under][x] = PX_K
        else:
            top = fy0 + inset + step
            box(x, top, x, under, PX_S)
            if i == d1 + 1:
                # The back edge of the bezel block: a line down its whole height
                # where there is a column to spare, else only the step.
                box(x, fy0 + inset, x, under if d2 > 1 else top, PX_K)
        box(x, under + 1, x, bottom, PX_S)
        # Under that edge the base of the chassis is set back: a darker band
        # from the strip's corner to the back, darkest right under the edge.
        lower = max(chassis - i // 2, bottom + 2) if n >= 24 else bottom
        if lower > bottom:
            box(x, bottom + 1, x, lower, PX_T)
            g[bottom + 1][x] = PX_D
            g[lower][x] = PX_K
        if i == d1 + d2 + 1:
            box(x, top, x, lower, PX_K)
        g[top][x] = g[bottom][x] = PX_K

    # The front: cabinet rim, bezel, tube.
    box(fx0, fy0, fx1, fy1, PX_K)
    x0, y0, x1, y1 = fx0 + 1, fy0 + 1, fx1 - 1, fy1 - 1
    rim = lay["rim"]
    if rim:
        box(x0, y0, x1, y1, PX_B)
        box(x0, y1, x1, y1, PX_S)
        box(x1, y0, x1, y1, PX_S)
        box(x0, y0, x1 - 1, y0, PX_W)
        box(x0, y0, x0, y1 - 1, PX_W)
        x0, y0, x1, y1 = x0 + rim, y0 + rim, x1 - rim, y1 - rim
    box(x0, y0, x1, y1, PX_T)
    bs, bt, bb = lay["bezel"]
    sx0, sy0, sx1, sy1 = x0 + bs, y0 + bt, x1 - bs, y1 - bb
    if bs >= 2:
        # The bezel slopes in to the glass: dark where it faces away from the light.
        frame(sx0 - 1, sy0 - 1, sx1 + 1, sy1 + 1, PX_S)
        box(sx0 - 1, sy0 - 1, sx1 + 1, sy0 - 1, PX_D)
        box(sx0 - 1, sy0 - 1, sx0 - 1, sy1, PX_D)
    box(sx0, sy0, sx1, sy1, PX_G)
    if lay["round"]:
        g[sy0][sx0] = g[sy0][sx1] = g[sy1][sx0] = PX_D
        g[sy1][sx1] = PX_S
    for row, c0, c1, col in lay["text"]:
        box(sx0 + c0, sy0 + row, sx0 + c1, sy0 + row, col)
    if lay["badge"]:
        bx0, by, bx1 = lay["badge"]
        box(bx0, by, bx1, by, PX_K)
        box(bx0 + 1, by, bx0 + 1 + (bx1 - bx0 > 2), by, PX_N)
    # The keyboard, a 122-key one lying on the desk in front: ten keys at the
    # left, the main block with the bank of function keys above it, the cursor
    # keys, the numeric pad. Its top is drawn in the terminal's own
    # projection, the depth running off to the right: each row further back
    # sits further right, over a front face that stands upright. The terminal
    # moves up where the keyboard needs the room.
    dy, dx = lay["kbd_dy"], lay.get("term_dx", 0)
    g = [[None] * dx + row[:n - dx] for row in g[dy:]] + [[None] * n for _ in range(dy)]
    x0, y0, x1, y1 = lay["kbd"]
    slope, face = lay["kbd_slope"], lay["kbd_face"]
    near = y1 - 1 - face                    # the top's nearest row

    def shift(y):
        return (near - y) // slope if slope and y < near else 0

    right = x1 - shift(y0) - (1 if face else 0)     # a column for the side's own edge
    flat = [[None] * n for _ in range(n)]   # the keyboard before the shear

    def kbox(bx0, by0, bx1, by1, c):
        for y in range(by0, by1 + 1):
            flat[y][bx0:bx1 + 1] = [c] * (bx1 - bx0 + 1)

    kbox(x0, y0, right, y1, PX_K)
    kbox(x0 + 1, y0 + 1, right - 1, near, PX_B)
    if face:
        kbox(x0 + 1, near + 1, right - 1, y1 - 1, PX_S)
    if "kbd_caps" in lay:
        wells, caps = lay["kbd_caps"]
        for wx0, wy0, wx1, wy1 in wells:
            kbox(wx0, wy0, wx1, wy1, PX_D)
        for cx0, cx1, cy, col in caps:
            kbox(cx0, cy, cx1, cy, col)
    else:
        k0, k1 = lay["kbd_keys"]
        blocks = [(k0, k1, c0, c1) for c0, c1 in lay["kbd_groups"]]
        if lay["kbd_f"]:
            blocks.append(lay["kbd_f"])
        for r0, r1, c0, c1 in blocks:
            for y in range(r0, r1 + 1):
                for x in range(c0, c1 + 1):
                    # A checker in the sheared picture, not before it.
                    flat[y][x] = PX_W if (x + shift(y) + y) % 2 == 0 else PX_T
    for y in range(y0, y1 + 1):
        for x in range(x0, right + 1):
            if flat[y][x]:
                g[y][x + shift(y)] = flat[y][x]
    # The right side has the front face's depth and its colour: the top's
    # right edge drawn again that many rows lower, and the band from the one
    # to the other filled, with no line between the top and the side.
    # Its outline keeps the top edge's even steps all the way down past the
    # front face, and the far corner is squared off.
    if face:
        g[y0][right + shift(y0) + 1] = PX_K
        for y in range(y0 + 1, y1):
            edge = right + shift(min(y, near))
            low = right + shift(max(y - face, y0))
            for x in range(edge, low + 1):
                g[y][x] = PX_S
            g[y][low + 1] = PX_K
    return g


def enlarged_terminal(n):
    """A size with no drawing of its own: the largest drawing that divides it,
    every pixel a k x k square, never a fractional scale (96 is the 48 px
    drawing; 128, 192, 256 and 512 are the 64 px one)."""
    base = max(s for s in TERMINAL_LAYOUT if n % s == 0)
    k = n // base
    return [[c for c in row for _ in range(k)] for row in terminal_pixels(base) for _ in range(k)]


def pixel_glyph(grid):
    """A pixel drawing as glyph layers, one per colour, for the shape renderers."""
    n = len(grid)

    def layer(col):
        return lambda x, y: 0 <= x < 1 and 0 <= y < 1 and grid[int(y * n)][int(x * n)] == col
    return [(layer(col), col) for col in sorted({c for row in grid for c in row if c})]


def g_terminal(size=48):
    return pixel_glyph(terminal_pixels(size))


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
    """glyph: layers, or a function of the size for art drawn per size."""
    images = [dib(render(glyph(s) if callable(glyph) else glyph, s), mask=True) for s in sizes]
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
    write_ico(os.path.join(OUT, "em34xxw.ico"), g_terminal, [16, 24, 32, 48, 64, 256])
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
