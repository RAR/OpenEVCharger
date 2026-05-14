    # Nexcyber (Nations N32G45x / Cortex-M4F).
    #
    # Defines TWO targets:
    #   openevcharger                  — production firmware (src/main.c + full
    #                                    src/hal/n32g45x/). Wired in Task 10;
    #                                    until then this file defines only the
    #                                    bench harness so the board still builds.
    #   openevcharger-nexcyber-bringup — the M0-M4 bring-up harness, the image
    #                                    actually flashed during bench work.

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

    # ---------- Bench-harness target ----------

    set(BRINGUP_SRCS
        boards/nexcyber-zbu011k/bench/bringup_main.c
        src/hal/n32g45x/clock.c
        src/hal/n32g45x/uart.c
        src/hal/n32g45x/gpio.c
        src/hal/n32g45x/adc_scan.c
        src/hal/n32g45x/cp_pwm.c
        src/hal/n32g45x/spi2.c
        src/hal/n32g45x/bl0939.c
        src/hal/n32g45x/nextion.c
        src/hal/n32g45x/relay.c
        src/hal/n32g45x/gfci.c
        src/hal/n32g45x/led_ring.c
        src/core/j1772.c
    )

    add_executable(openevcharger-nexcyber-bringup
        ${BRINGUP_SRCS}
        ${FREERTOS_SRCS}
        ${NX_STARTUP_SRC}
        ${NX_SYSTEM_SRC}
        ${NX_SPL_SRCS}
    )

    target_include_directories(openevcharger-nexcyber-bringup PRIVATE
        src/hal/n32g45x          # board-specific HAL headers win over src/hal/*.h
        boards/nexcyber-zbu011k  # pin_map.h
        src
        ${NX_CORE} ${NX_VARIANT} ${NX_SPL_INC}
        ${FREERTOS_DIR}/include ${FREERTOS_PORT_DIR}
    )

    target_compile_definitions(openevcharger-nexcyber-bringup PRIVATE
        N32G45X=1
        HSE_VALUE=8000000
        USE_STDPERIPH_DRIVER=1
    )

    target_link_options(openevcharger-nexcyber-bringup PRIVATE
        -T${CMAKE_SOURCE_DIR}/boards/nexcyber-zbu011k/n32g45x.ld
        -Wl,-Map=openevcharger-nexcyber-bringup.map,--cref
    )

    set_target_properties(openevcharger-nexcyber-bringup PROPERTIES SUFFIX ".elf")

    add_custom_command(TARGET openevcharger-nexcyber-bringup POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:openevcharger-nexcyber-bringup> openevcharger-nexcyber-bringup.bin
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:openevcharger-nexcyber-bringup>
        BYPRODUCTS openevcharger-nexcyber-bringup.bin
    )

    # ---------- Production target placeholder ----------
    # Wired in Task 10. For now alias it to the bench harness so CMakeLists.txt's
    # shared tail (which expects ${TARGET}) has something to attach to, and the
    # board still produces a build.

    add_executable(${TARGET} ${BRINGUP_SRCS}
        ${FREERTOS_SRCS} ${NX_STARTUP_SRC} ${NX_SYSTEM_SRC} ${NX_SPL_SRCS})

    target_include_directories(${TARGET} PRIVATE
        src/hal/n32g45x boards/nexcyber-zbu011k src
        ${NX_CORE} ${NX_VARIANT} ${NX_SPL_INC}
        ${FREERTOS_DIR}/include ${FREERTOS_PORT_DIR})

    target_compile_definitions(${TARGET} PRIVATE
        N32G45X=1 HSE_VALUE=8000000 USE_STDPERIPH_DRIVER=1)

    set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/boards/nexcyber-zbu011k/n32g45x.ld)
