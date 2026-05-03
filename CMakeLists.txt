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
    src/core/system_state.c
    src/hal/wdg.c
    src/hal/uart.c
    src/hal/gpio.c
    src/hal/adc_scan.c
    src/hal/adc_inject.c
    src/hal/cp_pwm.c
    src/hal/relay.c
    src/hal/spi3.c
    src/hal/uart5.c
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
    src/proto/tlv.c
    src/ui/buttons.c
    src/tasks/safety_task.c
    src/tasks/io_task.c
    src/tasks/comms_task.c
    src/tasks/persist_task.c
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
