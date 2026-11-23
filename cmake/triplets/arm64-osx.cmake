# SPDX-License-Identifier: GPL-3.0-or-later
# vcpkg's stock arm64-osx, built for the oldest macOS em34xx supports (11.0,
# CMAKE_OSX_DEPLOYMENT_TARGET) rather than the build machine's: otherwise
# every link warns that the ports were built for a newer macOS. Overlays the
# stock triplet of the same name (VCPKG_OVERLAY_TRIPLETS in CMakePresets.json).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 11.0)
