# SPDX-License-Identifier: GPL-3.0-or-later
# An .icns with iconutil (part of macOS) from tools/gen_macos_assets.py's
# output. SOURCE is either a ready <name>.iconset directory, whose PNGs are
# packed as they are (every size drawn for itself), or a 1024 px PNG master,
# which sips scales to every size. Run with:
#   cmake -DSOURCE=<iconset dir or png> -DICONSET=<dir.iconset> -DOUTPUT=<icns> -P make_icns.cmake
file(REMOVE_RECURSE "${ICONSET}")
file(MAKE_DIRECTORY "${ICONSET}")
if(IS_DIRECTORY "${SOURCE}")
  file(GLOB pngs "${SOURCE}/icon_*.png")
  if(NOT pngs)
    message(FATAL_ERROR "no icon_*.png in ${SOURCE}")
  endif()
  file(COPY ${pngs} DESTINATION "${ICONSET}")
else()
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
endif()
execute_process(COMMAND iconutil -c icns "${ICONSET}" -o "${OUTPUT}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "iconutil could not build ${OUTPUT}")
endif()
