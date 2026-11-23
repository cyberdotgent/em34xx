# SPDX-License-Identifier: GPL-3.0-or-later
# Static universal (arm64 + x86_64) build of the vcpkg ports for macOS 11+,
# for the universal em34xx.app (macos-release preset). Each port is built
# for both architectures at once (-arch arm64 -arch x86_64); ports whose
# build system cannot do that fail here, not in the app.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES "arm64;x86_64")
set(VCPKG_OSX_DEPLOYMENT_TARGET 11.0)

# toml++ builds with Meson, which cannot compile for two architectures at
# once ("cannot use 'cpp-output' output with multiple -arch options"). It is
# a header-only library: the port is built for one architecture, and a
# universal build of em34xx uses its headers only (TOML_HEADER_ONLY=1, see
# CMakeLists.txt), never its compiled library.
if(PORT STREQUAL "tomlplusplus")
  set(VCPKG_OSX_ARCHITECTURES arm64)
endif()
