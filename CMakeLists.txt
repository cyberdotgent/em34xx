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

find_package(SDL3 CONFIG REQUIRED)
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
#   tn34xx_app       config, monitor, gdb, application         -> all of the above
#   tn34xx_console   replxx monitor console                    -> app
#   tn34xx_frontend_sdl  SDL3 front end                        -> present, emu
#   tn34xx_frontends     front-end registry                    -> frontend_sdl
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
  src/bus/probe_device.cpp
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

tn34xx_library(tn34xx_app
  src/config/config.cpp
  src/config/connect_args.cpp
  src/monitor/expr.cpp
  src/monitor/monitor.cpp
  src/gdb/gdb_stub.cpp
)
target_link_libraries(tn34xx_app
  PUBLIC tn34xx_machines tn34xx_host tn34xx_bridge tn34xx_present tn34xx_net tomlplusplus::tomlplusplus)

tn34xx_library(tn34xx_console src/app/console.cpp)
target_link_libraries(tn34xx_console PUBLIC tn34xx_app PRIVATE replxx::replxx)

# --- SDL3 front end -------------------------------------------------------
tn34xx_library(tn34xx_frontend_sdl src/frontend/sdl_frontend.cpp)
target_link_libraries(tn34xx_frontend_sdl PUBLIC tn34xx_present PRIVATE SDL3::SDL3)

add_executable(tn5250-probe src/tools/tn5250_probe.cpp)
target_link_libraries(tn5250-probe PRIVATE tn34xx_tn5250 tn34xx_warnings)

# --- Executable -----------------------------------------------------------
add_executable(tn34xx src/main.cpp)
target_link_libraries(tn34xx PRIVATE tn34xx_app tn34xx_console tn34xx_frontend_sdl tn34xx_warnings)

if(TN34XX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
