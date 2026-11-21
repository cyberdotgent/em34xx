# SPDX-License-Identifier: GPL-3.0-or-later
# Shared body of the MinGW-w64 cross toolchains (mingw-i686.cmake,
# mingw-x86_64.cmake). Those set EM34XX_MINGW_ARCH and include this file.
#
# Used twice: as vcpkg's VCPKG_CHAINLOAD_TOOLCHAIN_FILE when building the
# ports (via the overlay triplets in cmake/triplets) and, through the same
# variable, for the project itself (see the mingw-* presets).
if(_EM34XX_MINGW_TOOLCHAIN)
  return()
endif()
set(_EM34XX_MINGW_TOOLCHAIN 1)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR ${EM34XX_MINGW_ARCH})
set(_prefix ${EM34XX_MINGW_ARCH}-w64-mingw32)

# Pin the posix thread model (winpthreads): std::thread, std::mutex and
# std::condition_variable all work, and winpthreads links statically. The
# unsuffixed compiler name follows update-alternatives, which defaults to
# the win32 model on Debian/Ubuntu.
find_program(CMAKE_C_COMPILER NAMES ${_prefix}-gcc-posix ${_prefix}-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${_prefix}-g++-posix ${_prefix}-g++ REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${_prefix}-windres windres)
set(CMAKE_FIND_ROOT_PATH /usr/${_prefix})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Windows 7 is the oldest supported target. Everything (ports included) is
# compiled against the Windows 7 API so nothing newer is linked statically.
string(APPEND CMAKE_C_FLAGS_INIT " -D_WIN32_WINNT=0x0601 -DWINVER=0x0601")
string(APPEND CMAKE_CXX_FLAGS_INIT " -D_WIN32_WINNT=0x0601 -DWINVER=0x0601")

# No MinGW runtime DLLs: libgcc, libstdc++ and winpthreads are linked in.
foreach(kind EXE SHARED MODULE)
  string(APPEND CMAKE_${kind}_LINKER_FLAGS_INIT " -static -static-libgcc -static-libstdc++")
endforeach()

# Release builds ship stripped (no debug information): unused functions and
# data are dropped and symbols removed. Debug builds keep everything.
string(APPEND CMAKE_C_FLAGS_RELEASE_INIT " -ffunction-sections -fdata-sections")
string(APPEND CMAKE_CXX_FLAGS_RELEASE_INIT " -ffunction-sections -fdata-sections")
foreach(kind EXE SHARED MODULE)
  string(APPEND CMAKE_${kind}_LINKER_FLAGS_RELEASE_INIT " -s -Wl,--gc-sections")
endforeach()

# Let ctest run the cross-built tests through wine when it is installed.
find_program(EM34XX_WINE NAMES wine)
if(EM34XX_WINE)
  set(CMAKE_CROSSCOMPILING_EMULATOR ${EM34XX_WINE})
endif()
