cmake_minimum_required(VERSION 3.20)

# The toolchain file MUST be set on the cmake command line via
# -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake (rippleon, M3)
# or
# -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain-cm4f.cmake (nexcyber, M4F)
project(openevcharger C ASM)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ---------- Board selection ----------
#
# `rippleon` (default) builds for the Rippleon ROC001 / GD32F205VE bench
# unit. `nexcyber` is the in-progress port to the Tuya-based Nexcyber
# AC EVSE / Nations N32G45x — see boards/nexcyber/README.md for status
# and what's needed to make it buildable.
if(NOT DEFINED OPENEVCHARGER_BOARD)
    set(OPENEVCHARGER_BOARD "rippleon")
endif()
set(OPENEVCHARGER_BOARD "${OPENEVCHARGER_BOARD}" CACHE STRING
    "Target board: rippleon (GD32F205VE) or nexcyber (N32G45x)")
set_property(CACHE OPENEVCHARGER_BOARD PROPERTY STRINGS rippleon nexcyber)
message(STATUS "OpenEVCharger board: ${OPENEVCHARGER_BOARD}")

if(OPENEVCHARGER_BOARD STREQUAL "nexcyber")
    # Nexcyber port is bring-up-skeleton only — the Nations N32G45x
    # vendor SDK isn't vendored yet and the HAL hasn't been ported.
    # See boards/nexcyber/README.md for the porter checklist.
    message(FATAL_ERROR
        "OPENEVCHARGER_BOARD=nexcyber is not yet buildable. "
        "Skeleton only at this point; see boards/nexcyber/README.md "
        "for what's missing (vendor SDK, HAL port, startup files).")
elseif(NOT OPENEVCHARGER_BOARD STREQUAL "rippleon")
    message(FATAL_ERROR
        "Unknown OPENEVCHARGER_BOARD '${OPENEVCHARGER_BOARD}'. "
        "Valid values: rippleon, nexcyber.")
endif()

# ---------- Vendor lib paths ----------

set(VENDOR_LIB ${CMAKE_SOURCE_DIR}/third_party/GD32F20x_Firmware_Library)
set(CMSIS_CORE ${VENDOR_LIB}/cmsis/cores/gd32)
set(GD_HEADERS ${VENDOR_LIB}/cmsis/variants/gd32f20x)
set(GD_STARTUP ${VENDOR_LIB}/cmsis/startup_files)
set(STDP_INC   ${VENDOR_LIB}/spl/inc)
set(STDP_SRC   ${VENDOR_LIB}/spl/src)

set(GD_STARTUP_SRC ${GD_STARTUP}/startup_gd32f20x_cl.S)
set(GD_SYSTEM_SRC  ${GD_HEADERS}/system_gd32f20x.c)

set(STDP_SRCS
    ${STDP_SRC}/gd32f20x_gpio.c
    ${STDP_SRC}/gd32f20x_rcu.c
    ${STDP_SRC}/gd32f20x_misc.c
    ${STDP_SRC}/gd32f20x_fwdgt.c
    ${STDP_SRC}/gd32f20x_usart.c
    ${STDP_SRC}/gd32f20x_adc.c
    ${STDP_SRC}/gd32f20x_dma.c
    ${STDP_SRC}/gd32f20x_timer.c
    ${STDP_SRC}/gd32f20x_spi.c
    ${STDP_SRC}/gd32f20x_rtc.c
    ${STDP_SRC}/gd32f20x_bkp.c
    ${STDP_SRC}/gd32f20x_pmu.c
)

# ---------- FreeRTOS ----------

set(FREERTOS_DIR ${CMAKE_SOURCE_DIR}/third_party/FreeRTOS-Kernel)
set(FREERTOS_PORT_DIR ${FREERTOS_DIR}/portable/GCC/ARM_CM3)

set(FREERTOS_SRCS
    ${FREERTOS_DIR}/tasks.c
    ${FREERTOS_DIR}/queue.c
    ${FREERTOS_DIR}/list.c
    ${FREERTOS_DIR}/timers.c
    ${FREERTOS_DIR}/event_groups.c
    ${FREERTOS_DIR}/stream_buffer.c
    ${FREERTOS_PORT_DIR}/port.c
    ${FREERTOS_DIR}/portable/MemMang/heap_4.c
)

# ---------- Application sources ----------

set(APP_SRCS
    src/main.c
    src/core/fault.c
    src/core/j1772.c
    src/core/over_temp.c
    src/core/rfid.c
    src/core/system_state.c
    src/core/system_time.c
    src/hal/clock.c
    src/hal/wdg.c
    src/hal/uart.c
    src/hal/gpio.c
    src/hal/adc_scan.c
    src/hal/adc_inject.c
    src/hal/cp_pwm.c
    src/hal/relay.c
    src/hal/gfci.c
    src/hal/rtc.c
    src/hal/bl0939.c
    src/hal/spi3.c
    src/hal/flash.c
    src/hal/uart5.c
    src/hal/rfid.c
    src/hal/ws2812.c
    src/hal/w25q.c
    src/persist/crc.c
    src/persist/boot_count.c
    src/persist/pingpong.c
    src/persist/boot_config.c
    src/persist/calibration.c
    src/persist/crc16.c
    src/persist/event_log.c
    src/persist/session_log.c
    src/persist/crash_state.c
    src/persist/rfid_authlist.c
    src/persist/ota_stage.c
    src/proto/tlv.c
    src/ui/buttons.c
    src/ui/buzzer.c
    src/ui/led_patterns.c
    src/tasks/safety_task.c
    src/tasks/io_task.c
    src/tasks/comms_task.c
    src/tasks/fc41d_flash_helper.c
    src/tasks/persist_task.c
    src/diag/stack_watch.c
)

# ---------- Build info ----------
#
# Derive OPENEVCHARGER_VERSION + OPENEVCHARGER_GIT_SHA from git so the
# MCU's build_info string ("version|sha", reported via CMD_GET_BUILD_INFO
# / EVT_BUILD_INFO and surfaced as the FC41D's "MCU Build" sensor)
# matches the source it was actually built from. The defaults in
# src/proto/build_info.h ("0.1.0-dev" / "unset") apply if git isn't
# available.
#
# CMake's execute_process is configure-time only; the configure_file
# calls below copy .git/HEAD and the active branch ref into the build
# directory so any commit on the current branch (or branch checkout)
# triggers a CMake reconfigure on the next build. Detached-HEAD case
# is handled implicitly by the .git/HEAD copy alone.
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE _OEVC_GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE _OEVC_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(EXISTS "${CMAKE_SOURCE_DIR}/.git/HEAD")
    configure_file(
        "${CMAKE_SOURCE_DIR}/.git/HEAD"
        "${CMAKE_BINARY_DIR}/_git_head_dep" COPYONLY)
    file(STRINGS "${CMAKE_SOURCE_DIR}/.git/HEAD" _OEVC_GIT_HEAD_LINE)
    if(_OEVC_GIT_HEAD_LINE MATCHES "^ref: (.+)$")
        set(_OEVC_GIT_REF_FILE "${CMAKE_SOURCE_DIR}/.git/${CMAKE_MATCH_1}")
        if(EXISTS "${_OEVC_GIT_REF_FILE}")
            configure_file("${_OEVC_GIT_REF_FILE}"
                "${CMAKE_BINARY_DIR}/_git_ref_dep" COPYONLY)
        endif()
    endif()
endif()

# ---------- Target ----------

set(TARGET ${PROJECT_NAME})

add_executable(${TARGET}
    ${APP_SRCS}
    ${FREERTOS_SRCS}
    ${GD_STARTUP_SRC}
    ${GD_SYSTEM_SRC}
    ${STDP_SRCS}
)

target_include_directories(${TARGET} PRIVATE
    src
    src/core
    ${CMSIS_CORE}
    ${GD_HEADERS}
    ${STDP_INC}
    ${FREERTOS_DIR}/include
    ${FREERTOS_PORT_DIR}
)

target_compile_definitions(${TARGET} PRIVATE
    GD32F20X_CL=1
    HXTAL_VALUE=8000000
    # F3 bench-validated 2026-05-06 (iter 3, 500 ms pulse + 1000 ms
    # recover). Bench unit chip is slow (~370 ms latch latency post
    # CAL release, ~270 ms hold, clean self-release). On-boot self-
    # test now runs the polarity-agnostic CAL pulse via gfci_self_test
    # and routes a failure to FAULT_GFCI_SELF_TEST. See src/hal/gfci.c
    # for timing rationale.
    OPENEVCHARGER_GFCI_CAL_SELF_TEST=1
    # F10: PE continuity sense topology characterisation pending real
    # install data. Bench saw PC5 raw≤1 with no live mains regardless of
    # PE state. F5 (2026-05-07 garage) found raw>400 with mains live and
    # EV plugged — detector tripped FAULT_PE_CONTINUITY before the EV
    # started drawing. Set to 0 below for the F5 session; safety_task.c
    # still prints PE raw every 5 s so the install-side baseline is
    # captured. Flip to 1 once the right threshold + polarity are pinned.
    OPENEVCHARGER_PE_CONTINUITY_DETECTOR=0
)

# Wire the git-derived version + SHA into the C define namespace from
# src/proto/build_info.h. Skipped (defaults retained) if either git
# command failed (no git, shallow clone, etc.).
if(_OEVC_GIT_VERSION)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_VERSION="${_OEVC_GIT_VERSION}")
endif()
if(_OEVC_GIT_SHA)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_GIT_SHA="${_OEVC_GIT_SHA}")
endif()

# Bench-time options. Default OFF unless explicitly passed via cmake.
if(OPENEVCHARGER_SEMIHOSTING)
    target_compile_definitions(${TARGET} PRIVATE OPENEVCHARGER_SEMIHOSTING=1)
endif()
if(DEFINED OPENEVCHARGER_WS2812_LEDS)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_WS2812_LEDS=${OPENEVCHARGER_WS2812_LEDS})
endif()
if(DEFINED OPENEVCHARGER_WS2812_INVERT)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_WS2812_INVERT=${OPENEVCHARGER_WS2812_INVERT})
endif()
if(DEFINED OPENEVCHARGER_LED_PROTOCOL_UCS1903)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_LED_PROTOCOL_UCS1903=${OPENEVCHARGER_LED_PROTOCOL_UCS1903})
endif()
if(DEFINED OPENEVCHARGER_LED_PROTOCOL_APA106)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_LED_PROTOCOL_APA106=${OPENEVCHARGER_LED_PROTOCOL_APA106})
endif()
if(DEFINED OPENEVCHARGER_LED_PROTOCOL_SK6812_RGBW)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_LED_PROTOCOL_SK6812_RGBW=${OPENEVCHARGER_LED_PROTOCOL_SK6812_RGBW})
endif()
if(DEFINED OPENEVCHARGER_LED_FORCE_GREEN)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_LED_FORCE_GREEN=${OPENEVCHARGER_LED_FORCE_GREEN})
endif()
# One-shot: erase crash_state ping-pong sectors at boot to clear a
# latched FAST_RESTART / CRASH_LOOP_SAFE_FAIL on the bench. Set =1 for
# a single flash cycle, then unset and re-flash.
if(DEFINED OPENEVCHARGER_BENCH_CRASH_RESET)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_BENCH_CRASH_RESET=${OPENEVCHARGER_BENCH_CRASH_RESET})
endif()
# Replace the SDK's broken 120m_hxtal PLL chain with a clean direct
# config (HXTAL/2 * 30 = 120 MHz). Default OFF for rollback safety;
# enable with -DOPENEVCHARGER_REAL_120M_PLL=1.
if(DEFINED OPENEVCHARGER_REAL_120M_PLL)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_REAL_120M_PLL=${OPENEVCHARGER_REAL_120M_PLL})
endif()
# One-shot bench probe: at boot, bit-bang the BL0939 SPI link and
# log raw responses. Use to confirm SCLK/SDI/SDO wiring + that the
# chip is alive before adding register-level protocol code.
if(DEFINED OPENEVCHARGER_BL0939_SMOKE)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_BL0939_SMOKE=${OPENEVCHARGER_BL0939_SMOKE})
endif()
# CC ladder detector (FAULT_CC_OUT_OF_RANGE). Default OFF until a
# bench characterisation pass nails down the OEM CC divider topology.
# Decoder still runs unconditionally so cc_max_amps publishes for
# diagnostic logging.
if(DEFINED OPENEVCHARGER_CC_DETECTOR)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_CC_DETECTOR=${OPENEVCHARGER_CC_DETECTOR})
endif()
# Boot CAL-pulse-and-EXTI self-test for the GFCI module
# (FAULT_GFCI_SELF_TEST). Default OFF — the PE3 polarity is
# contradicted in pin_map.h and the CAL-→-sense path hasn't been
# bench-validated on this PCB. Set =1 once a bench run with PE3 +
# PE2 scope confirms the path.
if(DEFINED OPENEVCHARGER_GFCI_CAL_SELF_TEST)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_GFCI_CAL_SELF_TEST=${OPENEVCHARGER_GFCI_CAL_SELF_TEST})
endif()
# E9: pre-scheduler stage→flash OTA apply path. Default ON as of
# 2026-05-05 — bench-validated end-to-end: layout-divergent payload
# round-tripped (commit 5b97f93), RAM-side reset confirmed, FAIL
# banner verified. Pass -DOPENEVCHARGER_OTA_APPLY_ENABLED=0 to suppress
# the apply (e.g. for a recovery image that shouldn't auto-overwrite
# itself) — boot_config.pending_ota_flag is then logged and cleared
# without touching internal flash.
if(NOT DEFINED OPENEVCHARGER_OTA_APPLY_ENABLED)
    set(OPENEVCHARGER_OTA_APPLY_ENABLED 1)
endif()
target_compile_definitions(${TARGET} PRIVATE
    OPENEVCHARGER_OTA_APPLY_ENABLED=${OPENEVCHARGER_OTA_APPLY_ENABLED})
# Phase-B bench marker: when set, main() prints a distinct banner at
# boot. Use it to tell pre- and post-OTA images apart on the bench
# console with an otherwise-identical source tree.
if(DEFINED OPENEVCHARGER_OTA_TEST_MARKER)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_OTA_TEST_MARKER=${OPENEVCHARGER_OTA_TEST_MARKER})
endif()
# Bench survey of FreeRTOS task stack high-water marks. Prints a single
# `stack: io=W safety=W comms=W persist=W` line every 30 s from io_task.
# Default ON for bench builds; pass -DOPENEVCHARGER_STACK_WATCH=0 for production.
if(NOT DEFINED OPENEVCHARGER_STACK_WATCH)
    set(OPENEVCHARGER_STACK_WATCH 1)
endif()
target_compile_definitions(${TARGET} PRIVATE
    OPENEVCHARGER_STACK_WATCH=${OPENEVCHARGER_STACK_WATCH})

set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/linker/gd32f205vc.ld)
target_link_options(${TARGET} PRIVATE
    -T${LINKER_SCRIPT}
    -Wl,-Map=${TARGET}.map,--cref
)
set_target_properties(${TARGET} PROPERTIES
    SUFFIX ".elf"
    LINK_DEPENDS ${LINKER_SCRIPT}
)

add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${TARGET}> ${TARGET}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${TARGET}> ${TARGET}.hex
    COMMAND ${CMAKE_SIZE}   $<TARGET_FILE:${TARGET}>
    BYPRODUCTS ${TARGET}.bin ${TARGET}.hex
    COMMENT "Generating ${TARGET}.bin/.hex and printing size"
)
