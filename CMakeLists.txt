cmake_minimum_required(VERSION 3.21)

project(tn34xx
  VERSION 0.0.0
  DESCRIPTION "Hardware emulator for IBM 34xx-family 5250 terminals"
  LANGUAGES CXX)

# C++17 is the deliberate ceiling: it is the newest standard that every
# toolchain still able to target Windows 7 (x86) implements completely.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TN34XX_BUILD_TESTS "Build unit tests and the CPU test-vector runner" OFF)
option(TN34XX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# TLS for the TN5250 client uses the host's own stack: SChannel on Windows
# (always there), the system libssl/libcrypto elsewhere (not from vcpkg).
#   AUTO (default): use it when found, else build without TLS
#   ON: fail configuration without it;  OFF: build without TLS
set(TN34XX_TLS AUTO CACHE STRING "TLS for the TN5250 client: AUTO, ON or OFF")
set_property(CACHE TN34XX_TLS PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${TN34XX_TLS}" _tn34xx_tls)
set(TN34XX_TLS_BACKEND none)
if(NOT _tn34xx_tls MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "TN34XX_TLS must be AUTO, ON or OFF (got '${TN34XX_TLS}')")
endif()
if(NOT _tn34xx_tls STREQUAL "OFF")
  if(WIN32)
    set(TN34XX_TLS_BACKEND schannel)
  else()
    find_package(OpenSSL 1.1.1)
    if(OPENSSL_FOUND)
      set(TN34XX_TLS_BACKEND openssl)
    elseif(_tn34xx_tls STREQUAL "ON")
      message(FATAL_ERROR "TN34XX_TLS=ON, but the system OpenSSL development files were not found "
                          "(install libssl-dev / openssl-devel, or configure with -DTN34XX_TLS=AUTO or OFF)")
    endif()
  endif()
endif()
if(TN34XX_TLS_BACKEND STREQUAL "openssl")
  message(STATUS "TN5250 TLS: system OpenSSL ${OPENSSL_VERSION} (${OPENSSL_SSL_LIBRARY})")
elseif(TN34XX_TLS_BACKEND STREQUAL "schannel")
  message(STATUS "TN5250 TLS: Windows SChannel (secur32, crypt32)")
else()
  message(STATUS "TN5250 TLS: disabled (TN34XX_TLS=${TN34XX_TLS}); install the OpenSSL development "
                 "files (libssl-dev / openssl-devel) and reconfigure to enable it")
endif()

# Front ends: each is its own executable, configured only where it applies.
#   TN34XX_FRONTEND_SDL  AUTO (default): build `tn34xx` when SDL3 is found
#                        ON: fail configuration without SDL3;  OFF: skip it
# The Win32 `tn34xxw` is built on WIN32 only; a Cocoa `tn34xx.app` (APPLE)
# is still a stub at the end of this file (docs/architecture.md).
set(TN34XX_FRONTEND_SDL AUTO CACHE STRING "SDL3 front end (tn34xx): AUTO, ON or OFF")
set_property(CACHE TN34XX_FRONTEND_SDL PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${TN34XX_FRONTEND_SDL}" _tn34xx_sdl)
if(NOT _tn34xx_sdl MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "TN34XX_FRONTEND_SDL must be AUTO, ON or OFF (got '${TN34XX_FRONTEND_SDL}')")
endif()
set(TN34XX_HAVE_SDL OFF)
if(_tn34xx_sdl STREQUAL "ON")
  find_package(SDL3 CONFIG REQUIRED)
  set(TN34XX_HAVE_SDL ON)
elseif(_tn34xx_sdl STREQUAL "AUTO")
  find_package(SDL3 CONFIG QUIET)
  if(SDL3_FOUND)
    set(TN34XX_HAVE_SDL ON)
  endif()
endif()
message(STATUS "SDL3 front end (tn34xx): ${TN34XX_HAVE_SDL}")

# Executable names, one variable per target so they can change in one place.
set(TN34XX_EXE_SDL tn34xx)                 # SDL3 window + developer tooling
set(TN34XX_EXE_HEADLESS tn34xx-headless)   # no window, monitor on stdin/stdout
set(TN34XX_EXE_WIN32 tn34xxw)              # native Win32 GUI (end-user), WIN32 only
set(TN34XX_EXE_COCOA tn34xx)               # future Cocoa GUI, bundled as tn34xx.app
find_package(replxx CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(tn34xx_warnings INTERFACE)
if(MSVC)
  target_compile_options(tn34xx_warnings INTERFACE /W4)
else()
  target_compile_options(tn34xx_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()
if(TN34XX_WARNINGS_AS_ERRORS)
  target_compile_options(tn34xx_warnings INTERFACE $<IF:$<BOOL:${MSVC}>,/WX,-Werror>)
endif()

# --- Libraries ---------------------------------------------------------------
# Layered; each library links only the layers below it (docs/architecture.md):
#
#   tn34xx_emu       util, cpu, bus, rom, machine (Machine, Emulator, interfaces)
#   tn34xx_net       sockets                                   -> emu
#   tn34xx_machines  devices, models, input                    -> emu
#   tn34xx_host      Twinax hosts, workstation controller      -> emu
#   tn34xx_tn5250    TN5250 client (TLS)                       -> host, net
#   tn34xx_bridge    TN5250 bridge (controller + sessions)     -> host, tn5250
#   tn34xx_present   CRT rendering, display profiles           -> emu
#   tn34xx_runtime   config, Application, HostLink             -> machines, bridge, present
#   tn34xx_devtools  monitor, console (replxx), GDB stub, probes -> runtime, net
#   tn34xx_frontend_sdl  SDL3 front end                        -> runtime
#   tn34xx_frontend_win32  Win32 front end (WIN32 only)        -> runtime
#
# End-user GUI executables link tn34xx_runtime plus their front end only;
# tn34xx (SDL) and tn34xx-headless also link tn34xx_devtools.
function(tn34xx_library name)
  add_library(${name} STATIC ${ARGN})
  target_include_directories(${name} PUBLIC src)
  target_link_libraries(${name} PRIVATE tn34xx_warnings)
endfunction()

tn34xx_library(tn34xx_emu
  src/util/log.cpp
  src/util/sha256.cpp
  src/util/fingerprint.cpp
  src/util/strings.cpp
  src/bus/bus.cpp
  src/cpu/cpu.cpp
  src/cpu/i86.cpp
  src/cpu/i86_disasm.cpp
  src/rom/romset.cpp
  src/machine/display_subtype.cpp
  src/machine/model_info.cpp
  src/machine/twinax_port.cpp
  src/machine/machine.cpp
  src/machine/emulator.cpp
)
target_link_libraries(tn34xx_emu PUBLIC Threads::Threads)
if(WIN32)
  target_compile_definitions(tn34xx_emu PUBLIC _WIN32_WINNT=0x0601 WINVER=0x0601)
endif()
if(MINGW)
  # C99 printf family (%zu, %llu) instead of msvcrt's; see util/strings.hpp.
  target_compile_definitions(tn34xx_emu PUBLIC __USE_MINGW_ANSI_STDIO=1)
endif()

tn34xx_library(tn34xx_net src/net/socket.cpp)
target_link_libraries(tn34xx_net PUBLIC tn34xx_emu)
if(WIN32)
  target_link_libraries(tn34xx_net PUBLIC ws2_32)
endif()

tn34xx_library(tn34xx_machines
  src/models/registry.cpp
  src/models/model_3477.cpp
  src/models/model_3486.cpp
  src/models/model_3489.cpp
  src/input/host_keymap.cpp
  src/input/keyboard_ids.cpp
  src/input/set3_keyboard.cpp
  src/devices/battery_ram.cpp
  src/devices/i2c.cpp
  src/devices/ibm3477/display.cpp
  src/devices/ibm3477/i2c.cpp
  src/devices/ibm3477/keyboard_if.cpp
  src/devices/ibm3477/system.cpp
  src/devices/ibm3477/twinax_engine.cpp
)
target_link_libraries(tn34xx_machines PUBLIC tn34xx_emu PRIVATE tomlplusplus::tomlplusplus)

tn34xx_library(tn34xx_host
  src/host/host_config.cpp
  src/host/twinax_link.cpp
  src/host/wsc/device_commands.cpp
  src/host/wsc/idle_host.cpp
  src/host/wsc/keyboard_map.cpp
  src/host/wsc/link_scheduler.cpp
  src/host/wsc/screen_writer.cpp
  src/host/wsc/station.cpp
  src/host/wsc/tn5250_adapter.cpp
)
target_link_libraries(tn34xx_host PUBLIC tn34xx_emu)

# --- TN5250 client: the host side of the Twinax bridge (src/tn5250) --------
tn34xx_library(tn34xx_tn5250
  src/tn5250/client.cpp
  src/tn5250/codepage.cpp
  src/tn5250/engine.cpp
  src/tn5250/record.cpp
  src/tn5250/telnet.cpp
  src/tn5250/tls_policy.cpp
  src/tn5250/tls_${TN34XX_TLS_BACKEND}.cpp
  src/tn5250/transport.cpp
)
target_link_libraries(tn34xx_tn5250 PUBLIC tn34xx_host tn34xx_net Threads::Threads)
if(TN34XX_TLS_BACKEND STREQUAL "openssl")
  target_link_libraries(tn34xx_tn5250 PRIVATE OpenSSL::SSL OpenSSL::Crypto)
elseif(TN34XX_TLS_BACKEND STREQUAL "schannel")
  target_link_libraries(tn34xx_tn5250 PRIVATE secur32 crypt32)
endif()

# --- TN5250 bridge: the workstation controller with TN5250 sessions --------
tn34xx_library(tn34xx_bridge src/host/wsc/tn5250_bridge.cpp)
target_link_libraries(tn34xx_bridge PUBLIC tn34xx_tn5250 tn34xx_host)

tn34xx_library(tn34xx_present
  src/present/crt_render.cpp
  src/present/display_profile.cpp
)
target_link_libraries(tn34xx_present PUBLIC tn34xx_emu)
# The CRT renderer works per output pixel (millions per frame): unoptimised
# it takes 200+ ms a frame and makes a Debug build unusable interactively.
# Always optimise it; it is rarely what one steps through in a debugger.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(src/present/crt_render.cpp src/present/display_profile.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>")
endif()

# --- Application runtime: what every executable links, GUIs included ------
tn34xx_library(tn34xx_runtime
  src/config/config.cpp
  src/config/connect_args.cpp
  src/config/profile.cpp
  src/config/settings.cpp
  src/config/user_dirs.cpp
  src/app/application.cpp
  src/app/host_link.cpp
)
target_link_libraries(tn34xx_runtime
  PUBLIC tn34xx_machines tn34xx_host tn34xx_bridge tn34xx_present tomlplusplus::tomlplusplus)

# --- Developer tooling: SDL and headless executables only ------------------
tn34xx_library(tn34xx_devtools
  src/devtools/console.cpp
  src/devtools/dev_tools.cpp
  src/devtools/probe_device.cpp
  src/monitor/expr.cpp
  src/monitor/monitor.cpp
  src/gdb/gdb_stub.cpp
)
target_link_libraries(tn34xx_devtools PUBLIC tn34xx_runtime tn34xx_net PRIVATE replxx::replxx)

add_executable(tn5250-probe src/tools/tn5250_probe.cpp)
target_link_libraries(tn5250-probe PRIVATE tn34xx_tn5250 tn34xx_warnings)

# --- Executables: one per front end ----------------------------------------
add_executable(${TN34XX_EXE_HEADLESS} src/main_headless.cpp)
target_link_libraries(${TN34XX_EXE_HEADLESS} PRIVATE tn34xx_devtools tn34xx_warnings)

if(TN34XX_HAVE_SDL)
  tn34xx_library(tn34xx_frontend_sdl src/frontend/sdl/sdl_frontend.cpp)
  target_link_libraries(tn34xx_frontend_sdl PUBLIC tn34xx_runtime PRIVATE SDL3::SDL3)

  add_executable(${TN34XX_EXE_SDL} src/main_sdl.cpp)
  target_link_libraries(${TN34XX_EXE_SDL} PRIVATE tn34xx_frontend_sdl tn34xx_devtools tn34xx_warnings)
endif()

# Native end-user front ends link tn34xx_runtime and their own front-end
# library only, never tn34xx_devtools (docs/architecture.md).
#
# tn34xxw: the Win32 GUI (GUI subsystem, resources from tn34xxw.rc built by
# windres; docs/frontend-win32.md).
if(WIN32)
  enable_language(RC)
  tn34xx_library(tn34xx_frontend_win32
    src/frontend/win32/win32_frontend.cpp
    src/frontend/win32/win32_dialogs.cpp
    src/frontend/win32/win32_util.cpp
  )
  target_link_libraries(tn34xx_frontend_win32 PUBLIC tn34xx_runtime
    PRIVATE comctl32 comdlg32 shell32 gdi32 user32 advapi32)
  # Unicode (W) APIs throughout; the generic macros map to them too.
  target_compile_definitions(tn34xx_frontend_win32 PUBLIC UNICODE _UNICODE)
  add_executable(${TN34XX_EXE_WIN32} WIN32 src/main_win32.cpp src/frontend/win32/tn34xxw.rc)
  target_link_libraries(${TN34XX_EXE_WIN32} PRIVATE tn34xx_frontend_win32 tn34xx_warnings)
  # The resource script includes resource.h and the files under res/.
  set_property(SOURCE src/frontend/win32/tn34xxw.rc APPEND PROPERTY OBJECT_DEPENDS
    ${PROJECT_SOURCE_DIR}/src/frontend/win32/resource.h
    ${PROJECT_SOURCE_DIR}/src/frontend/win32/tn34xxw.manifest
    ${PROJECT_SOURCE_DIR}/src/frontend/win32/res/tn34xxw.ico
    ${PROJECT_SOURCE_DIR}/src/frontend/win32/res/profile.ico
    ${PROJECT_SOURCE_DIR}/src/frontend/win32/res/toolbar16.bmp
    ${PROJECT_SOURCE_DIR}/src/frontend/win32/res/toolbar24.bmp
    ${PROJECT_SOURCE_DIR}/src/frontend/win32/res/toolbar32.bmp)
endif()

# Not written yet: a Cocoa tn34xx.app, which will look like this:
#
# if(APPLE)
#   enable_language(OBJCXX)
#   tn34xx_library(tn34xx_frontend_cocoa src/frontend/cocoa/cocoa_frontend.mm)
#   target_link_libraries(tn34xx_frontend_cocoa PUBLIC tn34xx_runtime PRIVATE "-framework Cocoa")
#   add_executable(${TN34XX_EXE_COCOA} MACOSX_BUNDLE src/main_cocoa.mm)
#   target_link_libraries(${TN34XX_EXE_COCOA} PRIVATE tn34xx_frontend_cocoa)
# endif()

if(TN34XX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
