# cmake/options.cmake — build-time / bench feature flags.
#
# Included by CMakeLists.txt AFTER the production target is created, so
# every block here operates on ${TARGET}. These are transient debug/bench
# knobs, board-independent. Hardware-fact defaults (HXTAL, GFCI CAL
# timing, PE-continuity topology) live in boards/<board>/board.cmake, not
# here.

# Default OFF unless explicitly passed via cmake.
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
if(DEFINED OPENEVCHARGER_BENCH_CRASH_RESET)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_BENCH_CRASH_RESET=${OPENEVCHARGER_BENCH_CRASH_RESET})
endif()
if(DEFINED OPENEVCHARGER_REAL_120M_PLL)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_REAL_120M_PLL=${OPENEVCHARGER_REAL_120M_PLL})
endif()
if(DEFINED OPENEVCHARGER_BL0939_SMOKE)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_BL0939_SMOKE=${OPENEVCHARGER_BL0939_SMOKE})
endif()
if(DEFINED OPENEVCHARGER_CC_DETECTOR)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_CC_DETECTOR=${OPENEVCHARGER_CC_DETECTOR})
endif()
# Override of the board.cmake default (boards/*/board.cmake sets the
# hardware-fact default; this lets a bench run flip it).
if(DEFINED OPENEVCHARGER_GFCI_CAL_SELF_TEST)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_GFCI_CAL_SELF_TEST=${OPENEVCHARGER_GFCI_CAL_SELF_TEST})
endif()
if(NOT DEFINED OPENEVCHARGER_OTA_APPLY_ENABLED)
    set(OPENEVCHARGER_OTA_APPLY_ENABLED 1)
endif()
target_compile_definitions(${TARGET} PRIVATE
    OPENEVCHARGER_OTA_APPLY_ENABLED=${OPENEVCHARGER_OTA_APPLY_ENABLED})
if(DEFINED OPENEVCHARGER_OTA_TEST_MARKER)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_OTA_TEST_MARKER=${OPENEVCHARGER_OTA_TEST_MARKER})
endif()
if(NOT DEFINED OPENEVCHARGER_STACK_WATCH)
    set(OPENEVCHARGER_STACK_WATCH 1)
endif()
target_compile_definitions(${TARGET} PRIVATE
    OPENEVCHARGER_STACK_WATCH=${OPENEVCHARGER_STACK_WATCH})
