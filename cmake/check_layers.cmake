# SPDX-License-Identifier: GPL-3.0-or-later
# Enforces the library layering (docs/architecture.md): every #include "..."
# in src/<dir> must name a directory that <dir>'s library may depend on.
# Run with: cmake -DSOURCE_DIR=<repo> -P cmake/check_layers.cmake

set(emu util bus cpu rom machine)
set(allowed_util ${emu})
set(allowed_bus ${emu})
set(allowed_cpu ${emu})
set(allowed_rom ${emu})
set(allowed_machine ${emu})
set(allowed_net ${emu} net)
set(allowed_input ${emu} input)
set(allowed_devices ${emu} input devices)
set(allowed_models ${emu} input devices models)
# The workstation controller drives a tn5250::DisplaySession, a header-only
# interface (tn5250/session_api.hpp); tn34xx_host does not link the client.
set(allowed_host ${emu} host tn5250/session_api.hpp)
set(allowed_tn5250 ${emu} net host tn5250)
set(allowed_bridge ${emu} host tn5250)
set(allowed_present ${emu} present)
# The runtime: everything an end-user GUI build links. No monitor, no GDB.
set(app_layer ${emu} input devices models host tn5250 present config app)
set(allowed_config ${app_layer})
set(allowed_app ${app_layer} frontend/frontend.hpp)
# Front ends see the runtime (Application, HostLink, everything it links:
# models, input, the TN5250 client) but never the tooling.
set(allowed_frontend ${app_layer} frontend)
# Developer tooling (tn34xx_devtools): may use everything but front ends.
set(devtools_layer ${app_layer} net monitor gdb devtools)
set(allowed_monitor ${devtools_layer})
set(allowed_gdb ${devtools_layer})
set(allowed_devtools ${devtools_layer})
set(allowed_tools ${emu} net host tn5250)

file(GLOB_RECURSE sources RELATIVE "${SOURCE_DIR}/src" "${SOURCE_DIR}/src/*.cpp" "${SOURCE_DIR}/src/*.hpp")
set(violations "")
foreach(f IN LISTS sources)
  if(NOT f MATCHES "/")
    continue()   # executables' main files at the top of src/
  endif()
  string(REGEX MATCH "^[^/]+" dir "${f}")
  if(f MATCHES "^host/wsc/tn5250_bridge\\.")
    set(dir bridge)   # tn34xx_bridge: the controller plus the TN5250 client
  endif()
  if(NOT DEFINED allowed_${dir})
    string(APPEND violations "  ${f}: directory '${dir}' has no layer in cmake/check_layers.cmake\n")
    continue()
  endif()
  file(STRINGS "${SOURCE_DIR}/src/${f}" lines REGEX "^[ \t]*#[ \t]*include[ \t]+\"")
  foreach(line IN LISTS lines)
    string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]+\"([^\"]+)\".*" "\\1" inc "${line}")
    if(NOT inc MATCHES "/")
      continue()   # same-directory include
    endif()
    string(REGEX MATCH "^[^/]+" target "${inc}")
    list(FIND allowed_${dir} "${target}" i_dir)
    list(FIND allowed_${dir} "${inc}" i_file)
    if(i_dir EQUAL -1 AND i_file EQUAL -1)
      string(APPEND violations "  src/${f}: includes \"${inc}\" (layer '${dir}' may not depend on '${target}')\n")
    endif()
  endforeach()
endforeach()
if(violations)
  message(FATAL_ERROR "Layering violations:\n${violations}")
endif()
message(STATUS "Layering OK (${CMAKE_CURRENT_LIST_FILE})")
