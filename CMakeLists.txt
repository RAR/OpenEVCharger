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
# unit. `nexcyber` builds the M0 bring-up image for the Nexcyber AC EVSE
# (Zopoise ZBU011K-C00X PCBA / Nations N32G45x) — see
# boards/nexcyber/README.md for the port status.
if(NOT DEFINED OPENEVCHARGER_BOARD)
    set(OPENEVCHARGER_BOARD "rippleon")
endif()
set(OPENEVCHARGER_BOARD "${OPENEVCHARGER_BOARD}" CACHE STRING
    "Target board: rippleon (GD32F205VE) or nexcyber (N32G45x)")
set_property(CACHE OPENEVCHARGER_BOARD PROPERTY STRINGS rippleon nexcyber)
message(STATUS "OpenEVCharger board: ${OPENEVCHARGER_BOARD}")

if(NOT OPENEVCHARGER_BOARD STREQUAL "rippleon"
   AND NOT OPENEVCHARGER_BOARD STREQUAL "nexcyber")
    message(FATAL_ERROR
        "Unknown OPENEVCHARGER_BOARD '${OPENEVCHARGER_BOARD}'. "
        "Valid values: rippleon, nexcyber.")
endif()

# ---------- Build info (board-independent) ----------
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

set(TARGET ${PROJECT_NAME})

# ============================================================================
# Rippleon (GD32F205VE / Cortex-M3) — production target.
# ============================================================================
if(OPENEVCHARGER_BOARD STREQUAL "rippleon")

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

    # ---------- Target ----------

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

    set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/linker/gd32f205vc.ld)

# ============================================================================
# Nexcyber (Nations N32G45x / Cortex-M4F) — M0 bring-up only.
# ============================================================================
#
# Builds a minimal "is the chip alive" image: clock + log UART up,
# heartbeat printk in main(). No FreeRTOS, no safety core, no
# persistence. Enough to confirm `cmake -DOPENEVCHARGER_BOARD=nexcyber`
# produces a flashable .elf and that the toolchain + linker script +
# Nations SPL all mesh.
#
# Subsequent milestones (M1 = FreeRTOS scaffold, M2 = GPIO HAL, …)
# pull in more of src/ as the GD32-specific HAL is ported.
elseif(OPENEVCHARGER_BOARD STREQUAL "nexcyber")

    # ---------- Vendor lib paths (Nations SDK) ----------

    set(NX_VENDOR  ${CMAKE_SOURCE_DIR}/third_party/N32G45x_Firmware_Library)
    set(NX_CORE    ${NX_VENDOR}/cmsis/core)
    set(NX_VARIANT ${NX_VENDOR}/cmsis/variants/n32g45x)
    set(NX_STARTUP ${NX_VENDOR}/cmsis/startup_files)
    set(NX_SPL_INC ${NX_VENDOR}/spl/inc)
    set(NX_SPL_SRC ${NX_VENDOR}/spl/src)

    set(NX_STARTUP_SRC ${NX_STARTUP}/startup_n32g45x_gcc.S)
    set(NX_SYSTEM_SRC  ${NX_VARIANT}/system_n32g45x.c)

    # Only the SPL modules we actually call from the M0 image. Pulling
    # the full SPL would compile fine but inflates flash unnecessarily;
    # extend this list as M1+ HAL modules come online.
    set(NX_SPL_SRCS
        ${NX_SPL_SRC}/misc.c
        ${NX_SPL_SRC}/n32g45x_gpio.c
        ${NX_SPL_SRC}/n32g45x_rcc.c
        ${NX_SPL_SRC}/n32g45x_usart.c
        # Pre-staged for M3 — picked up here so the SPL link surface is
        # complete before the M3 ADC / CP-PWM / BL0939-SPI drivers
        # arrive in boards/nexcyber/hal/. They're cold dead-code under
        # --gc-sections until something references them, so zero
        # runtime overhead until M3.
        ${NX_SPL_SRC}/n32g45x_adc.c
        ${NX_SPL_SRC}/n32g45x_tim.c
        ${NX_SPL_SRC}/n32g45x_spi.c
        ${NX_SPL_SRC}/n32g45x_dma.c
    )

    # ---------- FreeRTOS (M1) ----------
    #
    # Cortex-M4F port. FPU support is auto-handled inside the port via
    # `__ARM_FP` which the toolchain (-mfpu=fpv4-sp-d16 -mfloat-abi=hard)
    # defines — no extra FreeRTOSConfig.h flag needed.
    #
    # The shared `src/FreeRTOSConfig.h` is core-independent (configPRIO_BITS=4
    # matches both GD32F2 M3 and N32G45x M4F NVIC layouts; the SVC/PendSV/
    # SysTick handler aliasing maps to standard Cortex-M vector names which
    # both vendor startup files use).

    set(FREERTOS_DIR ${CMAKE_SOURCE_DIR}/third_party/FreeRTOS-Kernel)
    set(FREERTOS_PORT_DIR ${FREERTOS_DIR}/portable/GCC/ARM_CM4F)

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

    # ---------- Application sources (M0 + M1 + M2) ----------

    set(APP_SRCS
        boards/nexcyber/main.c
        boards/nexcyber/hal/clock.c
        boards/nexcyber/hal/uart.c
        boards/nexcyber/hal/gpio.c
        boards/nexcyber/hal/adc_scan.c
        boards/nexcyber/hal/cp_pwm.c
        boards/nexcyber/hal/spi2.c
        boards/nexcyber/hal/bl0939.c
        boards/nexcyber/hal/nextion.c
        boards/nexcyber/hal/relay.c
        boards/nexcyber/hal/gfci.c
        boards/nexcyber/hal/led_ring.c
        # M4 — board-independent core layers pulled in from the
        # rippleon-side shared sources.
        src/core/j1772.c
    )

    # ---------- Target ----------

    add_executable(${TARGET}
        ${APP_SRCS}
        ${FREERTOS_SRCS}
        ${NX_STARTUP_SRC}
        ${NX_SYSTEM_SRC}
        ${NX_SPL_SRCS}
    )

    # boards/nexcyber MUST come before src on the include path. The
    # board-specific HAL ports live at boards/nexcyber/hal/*.h and
    # SHADOW the rippleon ones at src/hal/*.h with the same names
    # (adc_scan.h, gpio.h, uart.h, clock.h, etc.). With -I src first,
    # GCC's #include "hal/foo.h" search resolves to the rippleon
    # version, silently using the wrong constants / function
    # signatures. Board-specific shadows must win.
    target_include_directories(${TARGET} PRIVATE
        boards/nexcyber
        src
        ${NX_CORE}
        ${NX_VARIANT}
        ${NX_SPL_INC}
        ${FREERTOS_DIR}/include
        ${FREERTOS_PORT_DIR}
    )

    target_compile_definitions(${TARGET} PRIVATE
        # Nations SPL gates its register access through this guard
        # (analogous to GD32F20X_CL on the rippleon target).
        N32G45X=1
        # HSE_VALUE defaults to 8 MHz in n32g45x.h if not pre-defined,
        # but we set it explicitly so future board revisions can pin a
        # different crystal here without touching the SDK header.
        HSE_VALUE=8000000
        # Match upstream Nations example projects.
        USE_STDPERIPH_DRIVER=1
    )

    set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/boards/nexcyber/n32g457.ld)

endif()

# ============================================================================
# Shared: build-info defines, link options, post-build.
# ============================================================================

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

# Build-time / bench feature flags (board-independent).
include(${CMAKE_SOURCE_DIR}/cmake/options.cmake)

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
