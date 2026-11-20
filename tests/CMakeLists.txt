find_package(doctest CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

add_executable(tn34xx_unit_tests
  unit_main.cpp
  test_cpu.cpp
  test_bus.cpp
  test_monitor_expr.cpp
  test_twinax.cpp
  test_util.cpp
  test_keyboard.cpp
  test_host_keymap.cpp
  test_display.cpp
  test_display3477.cpp
  test_3477.cpp
  test_twinax_engine.cpp
  test_wsc.cpp
  test_connect.cpp
  fake_station.cpp
)
target_link_libraries(tn34xx_unit_tests PRIVATE tn34xx_core doctest::doctest)
target_compile_definitions(tn34xx_unit_tests PRIVATE TN34XX_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME unit COMMAND tn34xx_unit_tests)

# SingleStepTests runner. Fetch the vectors with tools/fetch-test-vectors.sh.
add_executable(tn34xx_cpu_vectors cpu_vectors.cpp)
target_link_libraries(tn34xx_cpu_vectors PRIVATE tn34xx_core nlohmann_json::nlohmann_json ZLIB::ZLIB)

set(TN34XX_TESTDATA "${PROJECT_SOURCE_DIR}/testdata" CACHE PATH "Directory holding fetched test vectors")
if(EXISTS "${TN34XX_TESTDATA}/8088/metadata.json")
  add_test(NAME cpu_vectors_8088 COMMAND tn34xx_cpu_vectors "${TN34XX_TESTDATA}/8088")
  set_tests_properties(cpu_vectors_8088 PROPERTIES LABELS "slow;vectors" TIMEOUT 1800)
else()
  message(STATUS "8088 test vectors not found in ${TN34XX_TESTDATA}; run tools/fetch-test-vectors.sh")
endif()

# ROM smoke tests run only when the (never committed) ROM images are present.
if(EXISTS "${PROJECT_SOURCE_DIR}/roms/95f4171.bin" AND EXISTS "${PROJECT_SOURCE_DIR}/roms/95f4172.bin")
  add_test(NAME rom_3477_smoke
           COMMAND tn34xx --model 3477 --rom-dir "${PROJECT_SOURCE_DIR}/roms" --batch
                   --rc "${CMAKE_CURRENT_SOURCE_DIR}/rc/3477-smoke.rc")
  set_tests_properties(rom_3477_smoke PROPERTIES LABELS "rom")
endif()

# TN5250 client (src/tn5250): negotiation, data stream, field editing.
add_executable(tn34xx_tn5250_tests
  unit_main.cpp
  test_tn5250_negotiation.cpp
  test_tn5250_datastream.cpp
  test_tn5250_editing.cpp
  test_tn5250_tls.cpp
  test_bridge.cpp
  fake_station.cpp
)
target_link_libraries(tn34xx_tn5250_tests PRIVATE tn34xx_bridge tn34xx_tn5250 tn34xx_warnings doctest::doctest)
if(TN34XX_TLS_BACKEND STREQUAL "openssl")
  # The TLS test runs its own in-process OpenSSL server.
  target_compile_definitions(tn34xx_tn5250_tests PRIVATE TN34XX_TLS_OPENSSL=1)
  target_link_libraries(tn34xx_tn5250_tests PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
add_test(NAME tn5250 COMMAND tn34xx_tn5250_tests)
