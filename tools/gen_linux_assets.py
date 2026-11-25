#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the em34xx icon for Linux and the BSDs (original artwork).

Writes into src/frontend/qt/res/hicolor/<n>x<n>/apps/em34xx.png, the layout
of the freedesktop.org icon theme the files are installed into:
  16, 24, 32, 48, 64 the em34xxw application icon's own drawings
  96, 128, 192,      whole-number enlargements of them (enlarged_terminal in
  256, 512           tools/gen_win32_assets.py), so every edge stays hard

em34xx-qt embeds the sizes up to 256 px as its window icon (CMakeLists.txt);
em34xx.desktop names the installed ones.

Also writes em34xxw's toolbar and status bar glyphs, one PNG per glyph and
size, into src/frontend/qt/res/glyphs/<name>-<n>.png (16, 24, 32 and 48 px),
and the profile document icon into hicolor/<n>x<n>/mimetypes/
application-x-em34xx-profile.png (16, 32, 48 and 256 px).
Run from the repository root: python3 tools/gen_linux_assets.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_macos_assets as m  # noqa: E402
import gen_win32_assets as w  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "frontend", "qt", "res", "hicolor")
SIZES = [16, 24, 32, 48, 64, 96, 128, 192, 256, 512]


GLYPHS = OUT.replace("hicolor", "glyphs")
GLYPH_SIZES = [16, 24, 32, 48]
DOCUMENT_SIZES = [16, 32, 48, 256]


def rgba_rows(bgra):
    """gen_win32_assets.render() gives BGRA tuples; PNG rows are RGBA bytes."""
    return [b"".join(bytes((r, g, b, a)) for (b, g, r, a) in row) for row in bgra]


def glyphs():
    return [
        ("profiles", w.g_profiles()), ("new-connection", w.g_new_connection()), ("save", w.g_save()),
        ("disconnect", w.g_disconnect()), ("settings", w.g_settings()), ("keymap", w.g_keymap()),
        ("reconnect", w.g_connect()), ("reboot", w.g_reboot()),
        ("orb-green", w.g_orb(w.GREEN)), ("orb-grey", w.g_orb(w.GREY)), ("orb-red", w.g_orb(w.RED)),
        ("orb-orange", w.g_orb(w.AMBER)), ("lock-closed", w.g_lock(True)), ("lock-open", w.g_lock(False)),
        ("lock-closed-dim", w.g_lock(True, True)), ("lock-open-dim", w.g_lock(False, True)),
    ]


def main():
    os.makedirs(GLYPHS, exist_ok=True)
    for name, glyph in glyphs():
        for n in GLYPH_SIZES:
            m.write_png(os.path.join(GLYPHS, "%s-%d.png" % (name, n)), rgba_rows(w.render(glyph, n)))
    for n in DOCUMENT_SIZES:
        folder = os.path.join(OUT, "%dx%d" % (n, n), "mimetypes")
        os.makedirs(folder, exist_ok=True)
        m.write_png(os.path.join(folder, "application-x-em34xx-profile.png"), rgba_rows(w.render(w.g_document(), n)))
    for n in SIZES:
        rows = [b"".join(bytes(c) if c else b"\x00\x00\x00\x00" for c in row) for row in w.terminal_pixels(n)]
        folder = os.path.join(OUT, "%dx%d" % (n, n), "apps")
        os.makedirs(folder, exist_ok=True)
        m.write_png(os.path.join(folder, "em34xx.png"), rows)


if __name__ == "__main__":
    main()
