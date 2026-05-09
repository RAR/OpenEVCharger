# arm-none-eabi cross-compile toolchain for Cortex-M4F (Nations N32G45x).
#
# Selected when -DOPENEVCHARGER_BOARD=nexcyber. Mirrors the M3 toolchain
# (cmake/arm-none-eabi-toolchain.cmake) — same warnings, same nano/nosys
# specs — but enables the FPU and selects cortex-m4 target flags.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(TOOLCHAIN_PREFIX arm-none-eabi-)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_OBJCOPY ${TOOLCHAIN_PREFIX}objcopy CACHE INTERNAL "objcopy")
set(CMAKE_OBJDUMP ${TOOLCHAIN_PREFIX}objdump CACHE INTERNAL "objdump")
set(CMAKE_SIZE ${TOOLCHAIN_PREFIX}size CACHE INTERNAL "size")

# Cortex-M4F: single-precision FPU (fpv4-sp-d16), hard ABI.
set(CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard")

set(CMAKE_C_FLAGS_INIT
    "${CPU_FLAGS} -ffunction-sections -fdata-sections -fno-common -fmessage-length=0 \
    -Wall -Wextra -Wshadow -Wundef -Werror=implicit-function-declaration \
    -Werror=incompatible-pointer-types -Werror=return-type \
    -fstack-usage")

set(CMAKE_CXX_FLAGS_INIT
    "${CMAKE_C_FLAGS_INIT} -fno-exceptions -fno-rtti -fno-use-cxa-atexit")

set(CMAKE_ASM_FLAGS_INIT "${CPU_FLAGS} -x assembler-with-cpp")

set(CMAKE_EXE_LINKER_FLAGS_INIT
    "${CPU_FLAGS} -Wl,--gc-sections -Wl,--print-memory-usage -nostartfiles \
    -specs=nano.specs -specs=nosys.specs")

set(CMAKE_C_FLAGS_DEBUG_INIT "-Og -g3 -DDEBUG")
set(CMAKE_C_FLAGS_RELWITHDEBINFO_INIT "-Os -g3")
set(CMAKE_C_FLAGS_RELEASE_INIT "-Os -g")
