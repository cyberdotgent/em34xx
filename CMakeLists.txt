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

# --- Emulator core: everything except the SDL front end and main(). --------
add_library(tn34xx_core STATIC
  src/util/log.cpp
  src/util/sha256.cpp
  src/util/strings.cpp
  src/bus/bus.cpp
  src/bus/probe_device.cpp
  src/cpu/i86.cpp
  src/cpu/i86_disasm.cpp
  src/rom/romset.cpp
  src/machine/display_profile.cpp
  src/machine/machine.cpp
  src/machine/emulator.cpp
  src/models/registry.cpp
  src/models/model_3477.cpp
  src/models/model_3486.cpp
  src/models/model_3489.cpp
  src/input/keyboard_ids.cpp
  src/input/set3_keyboard.cpp
  src/devices/i2c.cpp
  src/devices/ibm3477/display.cpp
  src/devices/ibm3477/i2c.cpp
  src/devices/ibm3477/keyboard_if.cpp
  src/devices/ibm3477/system.cpp
  src/host/host_config.cpp
  src/host/twinax_link.cpp
  src/config/config.cpp
  src/monitor/expr.cpp
  src/monitor/monitor.cpp
  src/gdb/gdb_stub.cpp
  src/net/socket.cpp
)
target_include_directories(tn34xx_core PUBLIC src)
target_link_libraries(tn34xx_core
  PUBLIC tomlplusplus::tomlplusplus
  PRIVATE tn34xx_warnings replxx::replxx Threads::Threads)
if(WIN32)
  target_link_libraries(tn34xx_core PRIVATE ws2_32)
  target_compile_definitions(tn34xx_core PUBLIC _WIN32_WINNT=0x0601 WINVER=0x0601)
endif()

# --- SDL3 front end -------------------------------------------------------
add_library(tn34xx_frontend STATIC
  src/frontend/sdl_frontend.cpp
)
target_link_libraries(tn34xx_frontend
  PUBLIC tn34xx_core
  PRIVATE tn34xx_warnings SDL3::SDL3)

# --- Executable -----------------------------------------------------------
add_executable(tn34xx src/main.cpp)
target_link_libraries(tn34xx PRIVATE tn34xx_core tn34xx_frontend tn34xx_warnings)

if(TN34XX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
