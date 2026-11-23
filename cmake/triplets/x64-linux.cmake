# SPDX-License-Identifier: GPL-3.0-or-later
# vcpkg's stock x64-linux, with SDL3's libdecor support switched back on.
# The sdl3 port passes -DSDL_WAYLAND_LIBDECOR=OFF; without libdecor an SDL
# window has no title bar, border or controls on a Wayland compositor that
# draws no server-side decorations (GNOME's Mutter, observed on Fedora 45).
# Options given here are appended after the port's own, so this one wins.
# SDL loads libdecor at run time and needs its headers at build time
# (libdecor-devel / libdecor-0-dev); if they are missing SDL is configured
# without it and CMakeLists.txt warns. Overlays the stock triplet of the same
# name (VCPKG_OVERLAY_TRIPLETS in CMakePresets.json).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

if(PORT STREQUAL "sdl3")
  set(VCPKG_CMAKE_CONFIGURE_OPTIONS "-DSDL_WAYLAND_LIBDECOR=ON")
endif()
