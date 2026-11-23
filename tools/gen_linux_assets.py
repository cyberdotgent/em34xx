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
Run from the repository root: python3 tools/gen_linux_assets.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_macos_assets as m  # noqa: E402
import gen_win32_assets as w  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "frontend", "qt", "res", "hicolor")
SIZES = [16, 24, 32, 48, 64, 96, 128, 192, 256, 512]


def main():
    for n in SIZES:
        rows = [b"".join(bytes(c) if c else b"\x00\x00\x00\x00" for c in row) for row in w.terminal_pixels(n)]
        folder = os.path.join(OUT, "%dx%d" % (n, n), "apps")
        os.makedirs(folder, exist_ok=True)
        m.write_png(os.path.join(folder, "em34xx.png"), rows)


if __name__ == "__main__":
    main()
