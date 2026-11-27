# SPDX-License-Identifier: GPL-3.0-or-later
find_package(doctest CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

# Emulator core, devices and the workstation controller: no monitor, no
# configuration, no front end.
add_executable(em34xx_unit_tests
  unit_main.cpp
  test_cpu.cpp
  test_bus.cpp
  test_i80186.cpp
  test_emulator.cpp
  test_keyboard.cpp
  test_host_keymap.cpp
  test_keyboard_e2e.cpp
  test_display3477.cpp
  test_3477.cpp
  test_3486.cpp
  test_3483.cpp
  test_twinax_engine.cpp
  test_wsc.cpp
  test_session_cycle.cpp
  fake_station.cpp
)
target_link_libraries(em34xx_unit_tests PRIVATE em34xx_machines em34xx_host em34xx_warnings doctest::doctest)
target_compile_definitions(em34xx_unit_tests PRIVATE EM34XX_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME unit COMMAND em34xx_unit_tests)

# Runtime and developer tooling: configuration, monitor, probes, presentation.
add_executable(em34xx_app_tests
  unit_main.cpp
  test_monitor_expr.cpp
  test_twinax.cpp
  test_util.cpp
  test_burn_in.cpp
  test_display.cpp
  test_connect.cpp
  test_cut_controller.cpp
  test_probe.cpp
  test_gdb_stub.cpp
  test_user_data.cpp
  test_application.cpp
)
target_link_libraries(em34xx_app_tests PRIVATE em34xx_devtools em34xx_warnings doctest::doctest)
target_compile_definitions(em34xx_app_tests PRIVATE EM34XX_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# GUI link check: a minimal application linked against the runtime only.
add_executable(em34xx_runtime_only runtime_only_main.cpp)
target_link_libraries(em34xx_runtime_only PRIVATE em34xx_runtime em34xx_warnings)
add_test(NAME runtime_only COMMAND em34xx_runtime_only)
add_test(NAME app COMMAND em34xx_app_tests)

# SingleStepTests runner. Fetch the vectors with tools/fetch-test-vectors.sh.
add_executable(em34xx_cpu_vectors cpu_vectors.cpp)
target_link_libraries(em34xx_cpu_vectors PRIVATE em34xx_emu nlohmann_json::nlohmann_json ZLIB::ZLIB)

set(EM34XX_TESTDATA "${PROJECT_SOURCE_DIR}/testdata" CACHE PATH "Directory holding fetched test vectors")
if(EXISTS "${EM34XX_TESTDATA}/8088/metadata.json")
  add_test(NAME cpu_vectors_8088 COMMAND em34xx_cpu_vectors "${EM34XX_TESTDATA}/8088")
  set_tests_properties(cpu_vectors_8088 PROPERTIES LABELS "slow;vectors" TIMEOUT 1800)
else()
  message(STATUS "8088 test vectors not found in ${EM34XX_TESTDATA}; run tools/fetch-test-vectors.sh")
endif()

# ROM smoke tests run only when the (never committed) ROM images are present.
if(EXISTS "${PROJECT_SOURCE_DIR}/roms/95f4171.bin" AND EXISTS "${PROJECT_SOURCE_DIR}/roms/95f4172.bin")
  add_test(NAME rom_3477_smoke
           COMMAND em34xx-headless --model 3477 --rom-dir "${PROJECT_SOURCE_DIR}/roms" --batch
                   --rc "${CMAKE_CURRENT_SOURCE_DIR}/rc/3477-smoke.rc")
  set_tests_properties(rom_3477_smoke PROPERTIES LABELS "rom")
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/roms/65f2995.bin" AND EXISTS "${PROJECT_SOURCE_DIR}/roms/65f2996.bin")
  add_test(NAME rom_3477_fc_smoke
           COMMAND em34xx-headless --model 3477-FC --rom-dir "${PROJECT_SOURCE_DIR}/roms" --batch
                   --rc "${CMAKE_CURRENT_SOURCE_DIR}/rc/3477-fc-smoke.rc")
  set_tests_properties(rom_3477_fc_smoke PROPERTIES LABELS "rom")
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/roms/06g5525.bin")
  # 700M instructions: about 20 s in a release build, minutes in debug.
  add_test(NAME rom_3483_smoke
           COMMAND em34xx-headless --model 3483 --rom-dir "${PROJECT_SOURCE_DIR}/roms" --batch
                   --rc "${CMAKE_CURRENT_SOURCE_DIR}/rc/3483-smoke.rc")
  set_tests_properties(rom_3483_smoke PROPERTIES LABELS "rom;slow" TIMEOUT 900)
  # Two runs sharing one battery-backed RAM: save a setup, then boot with it.
  set(nvram_3483 "${CMAKE_CURRENT_BINARY_DIR}/3483-saved.nvram")
  add_test(NAME rom_3483_nvram_clean COMMAND ${CMAKE_COMMAND} -E rm -f "${nvram_3483}")
  add_test(NAME rom_3483_save_setup
           COMMAND em34xx-headless --model 3483 --rom-dir "${PROJECT_SOURCE_DIR}/roms" --batch
                   --nvram "${nvram_3483}" --rc "${CMAKE_CURRENT_SOURCE_DIR}/rc/3483-save-setup.rc")
  add_test(NAME rom_3483_boot_saved
           COMMAND em34xx-headless --model 3483 --rom-dir "${PROJECT_SOURCE_DIR}/roms" --batch
                   --nvram "${nvram_3483}" --rc "${CMAKE_CURRENT_SOURCE_DIR}/rc/3483-boot-saved.rc")
  set_tests_properties(rom_3483_nvram_clean PROPERTIES LABELS "rom;slow" FIXTURES_SETUP nvram3483clean)
  set_tests_properties(rom_3483_save_setup PROPERTIES LABELS "rom;slow" TIMEOUT 900
                       FIXTURES_REQUIRED nvram3483clean FIXTURES_SETUP nvram3483)
  set_tests_properties(rom_3483_boot_saved PROPERTIES LABELS "rom;slow" TIMEOUT 900 FIXTURES_REQUIRED nvram3483)
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/roms/09f4604.bin" AND EXISTS "${PROJECT_SOURCE_DIR}/roms/09f4605.bin")
  # 1.2G instructions: about a minute in a release build.
  add_test(NAME rom_3472_smoke
           COMMAND em34xx-headless --model 3472 --rom-dir "${PROJECT_SOURCE_DIR}/roms" --batch
                   --rc "${CMAKE_CURRENT_SOURCE_DIR}/rc/3472-smoke.rc")
  set_tests_properties(rom_3472_smoke PROPERTIES LABELS "rom;slow" TIMEOUT 900)

  # A first boot leaves the setup in battery-backed RAM; the next boots past it.
  set(nvram_3472 "${CMAKE_CURRENT_BINARY_DIR}/3472-saved.nvram")
  add_test(NAME rom_3472_nvram_clean COMMAND ${CMAKE_COMMAND} -E rm -f "${nvram_3472}")
  add_test(NAME rom_3472_save_setup
           COMMAND em34xx-headless --model 3472 --rom-dir "${PROJECT_SOURCE_DIR}/roms" --batch
                   --nvram "${nvram_3472}" --rc "${CMAKE_CURRENT_SOURCE_DIR}/rc/3472-save-setup.rc")
  add_test(NAME rom_3472_boot_saved
           COMMAND em34xx-headless --model 3472 --rom-dir "${PROJECT_SOURCE_DIR}/roms" --batch
                   --nvram "${nvram_3472}" --rc "${CMAKE_CURRENT_SOURCE_DIR}/rc/3472-boot-saved.rc")
  set_tests_properties(rom_3472_nvram_clean PROPERTIES LABELS "rom;slow" FIXTURES_SETUP nvram3472clean)
  set_tests_properties(rom_3472_save_setup PROPERTIES LABELS "rom;slow" TIMEOUT 900
                       FIXTURES_REQUIRED nvram3472clean FIXTURES_SETUP nvram3472)
  set_tests_properties(rom_3472_boot_saved PROPERTIES LABELS "rom;slow" TIMEOUT 900 FIXTURES_REQUIRED nvram3472)
endif()

# TN3270 (src/tn3270): the 3270 data stream.
add_executable(em34xx_tn3270_tests
  unit_main.cpp
  test_tn3270_datastream.cpp
  test_tn3270_client.cpp
)
target_link_libraries(em34xx_tn3270_tests PRIVATE em34xx_tn3270 em34xx_tn5250 em34xx_host em34xx_warnings doctest::doctest)
add_test(NAME tn3270 COMMAND em34xx_tn3270_tests)

# TN5250 client (src/tn5250): negotiation, data stream, field editing.
add_executable(em34xx_tn5250_tests
  unit_main.cpp
  test_tn5250_negotiation.cpp
  test_tn5250_datastream.cpp
  test_tn5250_editing.cpp
  test_tn5250_tls.cpp
  test_bridge.cpp
  fake_station.cpp
)
target_link_libraries(em34xx_tn5250_tests PRIVATE em34xx_bridge em34xx_runtime em34xx_warnings doctest::doctest)
if(EM34XX_TLS_BACKEND STREQUAL "openssl")
  # The TLS test runs its own in-process OpenSSL server.
  target_compile_definitions(em34xx_tn5250_tests PRIVATE EM34XX_TLS_OPENSSL=1)
  target_link_libraries(em34xx_tn5250_tests PRIVATE OpenSSL::SSL OpenSSL::Crypto)
elseif(EM34XX_TLS_BACKEND STREQUAL "apple")
  target_compile_definitions(em34xx_tn5250_tests PRIVATE EM34XX_TLS_APPLE=1)
endif()
add_test(NAME tn5250 COMMAND em34xx_tn5250_tests)

# Library layering (docs/architecture.md): includes must follow the
# dependency direction of the CMake libraries.
add_test(NAME layering COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
         -P ${PROJECT_SOURCE_DIR}/cmake/check_layers.cmake)
