# SPDX-License-Identifier: GPL-3.0-or-later
# An .icns from a 1024 px PNG master (tools/gen_macos_assets.py), with sips
# and iconutil (both part of macOS). Run with:
#   cmake -DSOURCE=<png> -DICONSET=<dir.iconset> -DOUTPUT=<icns> -P make_icns.cmake
file(REMOVE_RECURSE "${ICONSET}")
file(MAKE_DIRECTORY "${ICONSET}")
foreach(size IN ITEMS 16 32 128 256 512)
  math(EXPR double "${size} * 2")
  foreach(pixels IN ITEMS ${size} ${double})
    if(pixels EQUAL size)
      set(name "icon_${size}x${size}.png")
    else()
      set(name "icon_${size}x${size}@2x.png")
    endif()
    execute_process(COMMAND sips -z ${pixels} ${pixels} "${SOURCE}" --out "${ICONSET}/${name}"
      OUTPUT_QUIET RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "sips could not scale ${SOURCE} to ${pixels} px")
    endif()
  endforeach()
endforeach()
execute_process(COMMAND iconutil -c icns "${ICONSET}" -o "${OUTPUT}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "iconutil could not build ${OUTPUT}")
endif()
