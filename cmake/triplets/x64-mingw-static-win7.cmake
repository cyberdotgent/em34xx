# SPDX-License-Identifier: GPL-3.0-or-later
# Static MinGW-w64 build of the vcpkg ports for x86_64 Windows 7+, using the
# project's toolchain (posix threads, static runtime, _WIN32_WINNT=0x0601).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME MinGW)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/mingw-x86_64.cmake")
set(VCPKG_ENV_PASSTHROUGH PATH)
