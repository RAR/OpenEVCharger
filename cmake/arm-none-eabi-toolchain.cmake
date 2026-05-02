# arm-none-eabi cross-compile toolchain for GD32F205VE (Cortex-M3).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Toolchain binaries
set(TOOLCHAIN_PREFIX arm-none-eabi-)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_OBJCOPY ${TOOLCHAIN_PREFIX}objcopy CACHE INTERNAL "objcopy")
set(CMAKE_OBJDUMP ${TOOLCHAIN_PREFIX}objdump CACHE INTERNAL "objdump")
set(CMAKE_SIZE ${TOOLCHAIN_PREFIX}size CACHE INTERNAL "size")

# Cortex-M3 target flags (no FPU)
set(CPU_FLAGS "-mcpu=cortex-m3 -mthumb -mfloat-abi=soft")

set(CMAKE_C_FLAGS_INIT
    "${CPU_FLAGS} -ffunction-sections -fdata-sections -fno-common -fmessage-length=0 \
    -Wall -Wextra -Wshadow -Wundef -Werror=implicit-function-declaration \
    -Werror=incompatible-pointer-types -Werror=return-type \
    -fstack-usage -specs=nano.specs -specs=nosys.specs")

set(CMAKE_CXX_FLAGS_INIT
    "${CMAKE_C_FLAGS_INIT} -fno-exceptions -fno-rtti -fno-use-cxa-atexit")

set(CMAKE_ASM_FLAGS_INIT "${CPU_FLAGS} -x assembler-with-cpp")

set(CMAKE_EXE_LINKER_FLAGS_INIT
    "${CPU_FLAGS} -Wl,--gc-sections -Wl,--print-memory-usage -nostartfiles \
    -specs=nano.specs -specs=nosys.specs")

# Optimisation per build type
set(CMAKE_C_FLAGS_DEBUG_INIT "-Og -g3 -DDEBUG")
set(CMAKE_C_FLAGS_RELWITHDEBINFO_INIT "-Os -g3")
set(CMAKE_C_FLAGS_RELEASE_INIT "-Os -g")
