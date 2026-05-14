cmake_minimum_required(VERSION 3.20)

# The toolchain file MUST be set on the cmake command line:
#   -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake       (rippleon-roc001, Cortex-M3)
#   -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain-cm4f.cmake  (nexcyber-zbu011k, Cortex-M4F)
project(openevcharger C ASM)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ---------- Board selection ----------
if(NOT DEFINED OPENEVCHARGER_BOARD)
    set(OPENEVCHARGER_BOARD "rippleon-roc001")
endif()
set(OPENEVCHARGER_BOARD "${OPENEVCHARGER_BOARD}" CACHE STRING
    "Target board (a dir under boards/)")
set_property(CACHE OPENEVCHARGER_BOARD PROPERTY STRINGS
    rippleon-roc001 nexcyber-zbu011k)
message(STATUS "OpenEVCharger board: ${OPENEVCHARGER_BOARD}")
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/boards/${OPENEVCHARGER_BOARD}/board.cmake")
    message(FATAL_ERROR
        "Unknown OPENEVCHARGER_BOARD '${OPENEVCHARGER_BOARD}' — no "
        "boards/${OPENEVCHARGER_BOARD}/board.cmake")
endif()

set(TARGET ${PROJECT_NAME})

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

# ---------- Board target ----------
# board.cmake creates ${TARGET} (the production firmware) and sets
# LINKER_SCRIPT. The nexcyber board.cmake additionally defines the
# bench-harness target.
include(${CMAKE_SOURCE_DIR}/boards/${OPENEVCHARGER_BOARD}/board.cmake)

# ---------- Shared: build-info defines, options, link, post-build ----------

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
