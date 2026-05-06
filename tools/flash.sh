#!/usr/bin/env bash
# Build (if needed) and flash an OpenEVCharger image to the connected
# GD32F205 via SWD.
#
# Usage:
#   tools/flash.sh                              # default build/ tree
#   tools/flash.sh build/ota_baseline/...elf    # explicit path (.elf or .bin)
#   tools/flash.sh recovery/stock-...bin        # raw .bin restore
#
# An explicit .elf is programmed directly. An explicit .bin is
# programmed at 0x08000000 (bank0 base). With no argument, the script
# rebuilds the default `build/` tree and flashes its openevcharger.elf, the
# original behaviour.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFG="$REPO_ROOT/tools/openocd-gd32f205.cfg"

# Refuse to flash without a stock backup on disk
if [[ ! -e "$REPO_ROOT/recovery/stock-mcu-V1.0.066.bin" ]]; then
    echo "ERROR: no stock backup at recovery/stock-mcu-V1.0.066.bin"
    echo "Run ./tools/stock_backup.sh first. Aborting."
    exit 1
fi

if [[ $# -ge 1 ]]; then
    TARGET="$1"
    [[ "$TARGET" = /* ]] || TARGET="$REPO_ROOT/$TARGET"
    if [[ ! -e "$TARGET" ]]; then
        echo "ERROR: $TARGET not found"
        exit 1
    fi
    case "$TARGET" in
        *.elf)
            echo "Flashing $TARGET ..."
            openocd -f "$CFG" \
                    -c "init" \
                    -c "reset halt" \
                    -c "program $TARGET verify reset" \
                    -c "shutdown"
            ;;
        *.bin|*.hex)
            echo "Flashing raw $TARGET at 0x08000000 ..."
            openocd -f "$CFG" \
                    -c "init" \
                    -c "reset halt" \
                    -c "program $TARGET 0x08000000 verify reset" \
                    -c "shutdown"
            ;;
        *)
            echo "ERROR: unrecognised file type for $TARGET (need .elf/.bin/.hex)"
            exit 1
            ;;
    esac
else
    BUILD_DIR="$REPO_ROOT/build"
    ELF="$BUILD_DIR/openevcharger.elf"
    CACHE="$BUILD_DIR/CMakeCache.txt"

    # Detect stale CMake cache pointing at a previous source path
    # (e.g. after the OpenBHZD → OpenEVCharger rename, or any later
    # repo move). Re-configure from scratch instead of letting cmake
    # error out on a vanished source dir.
    NEED_CONFIGURE=0
    if [[ ! -e "$CACHE" ]]; then
        NEED_CONFIGURE=1
    else
        CACHED_SRC=$(awk -F= '/^CMAKE_HOME_DIRECTORY:INTERNAL=/{print $2}' "$CACHE")
        if [[ "$CACHED_SRC" != "$REPO_ROOT" ]]; then
            echo "Stale CMake cache (src=$CACHED_SRC, expected $REPO_ROOT) — wiping build/"
            rm -rf "$BUILD_DIR"
            NEED_CONFIGURE=1
        fi
    fi
    if [[ "$NEED_CONFIGURE" -eq 1 ]]; then
        echo "Configuring build/ (toolchain + REAL_120M_PLL)"
        cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
              -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/arm-none-eabi-toolchain.cmake" \
              -DOPENEVCHARGER_REAL_120M_PLL=1 \
              -G Ninja
    fi

    if [[ ! -e "$ELF" ]] || \
       find "$REPO_ROOT/src" -newer "$ELF" -type f | grep -q .; then
        echo "Build needed; running cmake --build build"
        cmake --build "$BUILD_DIR"
    fi
    echo "Flashing $ELF ..."
    openocd -f "$CFG" \
            -c "init" \
            -c "reset halt" \
            -c "program $ELF verify reset" \
            -c "shutdown"
fi

echo "Done. Heartbeat LED on PD4 should be blinking at 1 Hz."
