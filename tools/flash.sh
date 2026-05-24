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
        *.elf|*.bin|*.hex)
            echo "Flashing $TARGET ..."
            # OpenOCD's high-level `program` command silently fails to
            # write flash sector 0 (the vector table) on GD32F205 — the
            # write algorithm gets confused and leaves garbage there
            # while still reporting "Verified OK". Bench-confirmed
            # 2026-05-24: after `program`, 0x08000000-0x0800003F reads
            # back as the address-byte pattern (00 01 02 03 ...) and
            # the chip hard-faults on reset.
            #
            # Workaround: manual unlock + mass erase via direct FLASH
            # controller writes, then `flash write_image` (lower-level
            # path that doesn't have the bug). Tested on rippleon ROC001.
            openocd -f "$CFG" \
                    -c "init" \
                    -c "reset halt" \
                    -c "mww 0x40022004 0x45670123" \
                    -c "mww 0x40022004 0xCDEF89AB" \
                    -c "mww 0x40022010 0x00000004" \
                    -c "mww 0x40022010 0x00000044" \
                    -c "sleep 2000" \
                    -c "reset halt" \
                    -c "flash write_image $TARGET" \
                    -c "verify_image $TARGET" \
                    -c "reset run" \
                    -c "shutdown"
            ;;
        *)
            echo "ERROR: unrecognised file type for $TARGET (need .elf/.bin/.hex)"
            exit 1
            ;;
    esac
    cat <<'POSTFLASH'

==========================================================================
  Heartbeat LED on PD4 should be blinking at 1 Hz.

  NOTE: After SWD flash, the external GFCI/CCID chip is latched in
  fail-safe state (MCU was halted for >6 s during flash, so the chip's
  watchdog tripped). The boot self-test will FAIL until you power-cycle
  the unit. After power cycle, the chip POSTs cleanly and the boot
  self-test PASSes normally.

  Skip the power cycle only if you're sure the chip wasn't ever in
  fail-safe (e.g., you flashed and the unit was already in READY with
  the refresh task running before the SWD halt — rare).
==========================================================================
POSTFLASH
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
    # See big comment above the explicit-path branch — `program` skips
    # sector 0 silently on GD32F205.
    openocd -f "$CFG" \
            -c "init" \
            -c "reset halt" \
            -c "mww 0x40022004 0x45670123" \
            -c "mww 0x40022004 0xCDEF89AB" \
            -c "mww 0x40022010 0x00000004" \
            -c "mww 0x40022010 0x00000044" \
            -c "sleep 2000" \
            -c "reset halt" \
            -c "flash write_image $ELF" \
            -c "verify_image $ELF" \
            -c "reset run" \
            -c "shutdown"
fi

cat <<'POSTFLASH'

==========================================================================
  Heartbeat LED on PD4 should be blinking at 1 Hz.

  NOTE: After SWD flash, the external GFCI/CCID chip is latched in
  fail-safe state (MCU was halted for >6 s during flash, so the chip's
  watchdog tripped). The boot self-test will FAIL until you power-cycle
  the unit. After power cycle, the chip POSTs cleanly and the boot
  self-test PASSes normally.
==========================================================================
POSTFLASH
