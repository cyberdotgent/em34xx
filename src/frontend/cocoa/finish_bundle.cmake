# SPDX-License-Identifier: GPL-3.0-or-later
# After linking em34xx.app: check that the executable has every requested
# architecture (a universal build must not ship one slice), then sign the
# bundle ad hoc so macOS runs it from the build folder. Release signing
# with a Developer ID happens later (tools/macos-release.sh).
#   cmake -DAPP=<bundle> -DEXE=<executable> -DARCHS=<a,b> -P finish_bundle.cmake
if(ARCHS)
  execute_process(COMMAND lipo -archs "${EXE}" OUTPUT_VARIABLE have
    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "lipo cannot read ${EXE}")
  endif()
  string(REPLACE " " ";" have "${have}")
  string(REPLACE "," ";" want "${ARCHS}")
  foreach(arch IN LISTS want)
    if(NOT arch IN_LIST have)
      message(FATAL_ERROR "${EXE} lacks ${arch} (has: ${have})")
    endif()
  endforeach()
endif()
execute_process(COMMAND codesign --force --sign - --timestamp=none "${APP}"
  RESULT_VARIABLE result ERROR_VARIABLE err)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "ad hoc signing of ${APP} failed: ${err}")
endif()
