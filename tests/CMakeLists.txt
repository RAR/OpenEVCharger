cmake_minimum_required(VERSION 3.20)

# Standalone host-build for unit tests. Run with:
#
#   cmake -S tests -B build/host
#   cmake --build build/host
#   ctest --test-dir build/host --output-on-failure
#
# Spec § 11 calls this out as a CI gate for any PR touching core/, proto/,
# or persist/. Keep this project deliberately separate from the main
# CMakeLists.txt so `cmake -S .` (firmware build) still requires the
# arm-none-eabi toolchain file and isn't tempted to silently fall back to
# the host compiler.
#
# Layout (94 cases as of 2026-05-03):
#   openevcharger_pure         — pure-C modules built straight from src/.
#                           No FreeRTOS / SPL / hardware headers.
#   openevcharger_w25q_mock    — host-side simulated NOR flash backing
#                           hal/w25q.h + tiny printk()/uart_* stubs.
#   openevcharger_tests        — test binary; one suite_*() per module.
#
# Suites:
#   crc16, crc32, j1772, fault, tlv          — pure logic
#   pingpong, boot_config                    — persist/ on w25q mock
#   over_temp, gfci_policy                   — core/ pure detectors

project(openevcharger_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# Same warning level as a sane firmware build. -Werror keeps drift-free.
add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wno-sign-compare)

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# Modules under test — build straight from src/ (no copies, no shims).
# Pure logic only. Anything pulling in the GD32 SPL or FreeRTOS headers
# would not belong here.
add_library(openevcharger_pure STATIC
    ${SRC_DIR}/core/j1772.c
    ${SRC_DIR}/core/fault.c
    ${SRC_DIR}/core/over_temp.c
    ${SRC_DIR}/core/gfci_policy.c
    ${SRC_DIR}/core/system_time.c
    ${SRC_DIR}/persist/crc16.c
    ${SRC_DIR}/persist/crc.c
    ${SRC_DIR}/persist/pingpong.c
    ${SRC_DIR}/persist/boot_config.c
    ${SRC_DIR}/persist/ota_stage.c
    ${SRC_DIR}/proto/tlv.c
)
target_include_directories(openevcharger_pure PUBLIC
    ${SRC_DIR}/core
    ${SRC_DIR}/persist
    ${SRC_DIR}/proto
    ${SRC_DIR}/hal
    ${SRC_DIR}/drivers
)

# Host-side mock of hal/w25q.h + tiny stubs for printk / uart_*. Linked
# into tests that exercise persist layers (pingpong, boot_config).
# Keeps the firmware build untouched — the real w25q.c / uart.c only
# ship in the arm-none-eabi target.
add_library(openevcharger_w25q_mock STATIC
    mocks/w25q_mock.c
    mocks/host_stubs.c
)
target_include_directories(openevcharger_w25q_mock PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SRC_DIR}/hal
    ${SRC_DIR}/drivers
)

add_executable(openevcharger_tests
    test_main.c
    test_crc16.c
    test_crc32.c
    test_j1772.c
    test_fault.c
    test_tlv.c
    test_pingpong.c
    test_boot_config.c
    test_over_temp.c
    test_gfci_policy.c
    test_ota_stage.c
    test_system_time.c
)
target_link_libraries(openevcharger_tests PRIVATE openevcharger_pure openevcharger_w25q_mock)
target_include_directories(openevcharger_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

enable_testing()
add_test(NAME openevcharger_tests COMMAND openevcharger_tests)
