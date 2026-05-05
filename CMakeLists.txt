cmake_minimum_required(VERSION 3.20)

# The toolchain file MUST be set on the cmake command line via
# -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
project(openbhzd C ASM)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
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
    src/hal/clock.c
    src/hal/wdg.c
    src/hal/uart.c
    src/hal/gpio.c
    src/hal/adc_scan.c
    src/hal/adc_inject.c
    src/hal/cp_pwm.c
    src/hal/relay.c
    src/hal/gfci.c
    src/hal/bl0939.c
    src/hal/spi3.c
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
)

# Bench-time options. Default OFF unless explicitly passed via cmake.
if(OPENBHZD_SEMIHOSTING)
    target_compile_definitions(${TARGET} PRIVATE OPENBHZD_SEMIHOSTING=1)
endif()
if(DEFINED OPENBHZD_RELAY_ACTUATE_SELF_TEST)
    target_compile_definitions(${TARGET} PRIVATE
        OPENBHZD_RELAY_ACTUATE_SELF_TEST=${OPENBHZD_RELAY_ACTUATE_SELF_TEST})
endif()
if(DEFINED OPENBHZD_WS2812_LEDS)
    target_compile_definitions(${TARGET} PRIVATE
        OPENBHZD_WS2812_LEDS=${OPENBHZD_WS2812_LEDS})
endif()
if(DEFINED OPENBHZD_WS2812_INVERT)
    target_compile_definitions(${TARGET} PRIVATE
        OPENBHZD_WS2812_INVERT=${OPENBHZD_WS2812_INVERT})
endif()
if(DEFINED OPENBHZD_LED_PROTOCOL_UCS1903)
    target_compile_definitions(${TARGET} PRIVATE
        OPENBHZD_LED_PROTOCOL_UCS1903=${OPENBHZD_LED_PROTOCOL_UCS1903})
endif()
if(DEFINED OPENBHZD_LED_PROTOCOL_APA106)
    target_compile_definitions(${TARGET} PRIVATE
        OPENBHZD_LED_PROTOCOL_APA106=${OPENBHZD_LED_PROTOCOL_APA106})
endif()
if(DEFINED OPENBHZD_LED_PROTOCOL_SK6812_RGBW)
    target_compile_definitions(${TARGET} PRIVATE
        OPENBHZD_LED_PROTOCOL_SK6812_RGBW=${OPENBHZD_LED_PROTOCOL_SK6812_RGBW})
endif()
if(DEFINED OPENBHZD_LED_FORCE_GREEN)
    target_compile_definitions(${TARGET} PRIVATE
        OPENBHZD_LED_FORCE_GREEN=${OPENBHZD_LED_FORCE_GREEN})
endif()
# One-shot: erase crash_state ping-pong sectors at boot to clear a
# latched FAST_RESTART / CRASH_LOOP_SAFE_FAIL on the bench. Set =1 for
# a single flash cycle, then unset and re-flash.
if(DEFINED OPENBHZD_BENCH_CRASH_RESET)
    target_compile_definitions(${TARGET} PRIVATE
        OPENBHZD_BENCH_CRASH_RESET=${OPENBHZD_BENCH_CRASH_RESET})
endif()
# Replace the SDK's broken 120m_hxtal PLL chain with a clean direct
# config (HXTAL/2 * 30 = 120 MHz). Default OFF for rollback safety;
# enable with -DOPENBHZD_REAL_120M_PLL=1.
if(DEFINED OPENBHZD_REAL_120M_PLL)
    target_compile_definitions(${TARGET} PRIVATE
        OPENBHZD_REAL_120M_PLL=${OPENBHZD_REAL_120M_PLL})
endif()
# One-shot bench probe: at boot, bit-bang the BL0939 SPI link and
# log raw responses. Use to confirm SCLK/SDI/SDO wiring + that the
# chip is alive before adding register-level protocol code.
if(DEFINED OPENBHZD_BL0939_SMOKE)
    target_compile_definitions(${TARGET} PRIVATE
        OPENBHZD_BL0939_SMOKE=${OPENBHZD_BL0939_SMOKE})
endif()
# CC ladder detector (FAULT_CC_OUT_OF_RANGE). Default OFF until a
# bench characterisation pass nails down the OEM CC divider topology.
# Decoder still runs unconditionally so cc_max_amps publishes for
# diagnostic logging.
if(DEFINED OPENBHZD_CC_DETECTOR)
    target_compile_definitions(${TARGET} PRIVATE
        OPENBHZD_CC_DETECTOR=${OPENBHZD_CC_DETECTOR})
endif()
# Bench survey of FreeRTOS task stack high-water marks. Prints a single
# `stack: io=W safety=W comms=W persist=W` line every 30 s from io_task.
# Default ON for bench builds; pass -DOPENBHZD_STACK_WATCH=0 for production.
if(NOT DEFINED OPENBHZD_STACK_WATCH)
    set(OPENBHZD_STACK_WATCH 1)
endif()
target_compile_definitions(${TARGET} PRIVATE
    OPENBHZD_STACK_WATCH=${OPENBHZD_STACK_WATCH})

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
