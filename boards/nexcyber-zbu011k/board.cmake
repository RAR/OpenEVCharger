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
        ${CMAKE_SOURCE_DIR}/boards/nexcyber-zbu011k
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

    set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/boards/nexcyber-zbu011k/n32g45x.ld)
