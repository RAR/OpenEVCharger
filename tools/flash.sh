#!/usr/bin/env bash
# Build (if needed) and flash openbhzd.elf to the connected GD32F205 via SWD.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFG="$REPO_ROOT/tools/openocd-gd32f205.cfg"
ELF="$REPO_ROOT/build/openbhzd.elf"

# Refuse to flash without a stock backup on disk
if [[ ! -e "$REPO_ROOT/recovery/stock-mcu-V1.0.066.bin" ]]; then
    echo "ERROR: no stock backup at recovery/stock-mcu-V1.0.066.bin"
    echo "Run ./tools/stock_backup.sh first. Aborting."
    exit 1
fi

# Build if .elf is missing or older than any source
if [[ ! -e "$ELF" ]] || \
   find "$REPO_ROOT/src" -newer "$ELF" -type f | grep -q .; then
    echo "Build needed; running cmake --build build"
    cmake --build "$REPO_ROOT/build"
fi

echo "Flashing $ELF ..."
openocd -f "$CFG" \
        -c "init" \
        -c "reset halt" \
        -c "program $ELF verify reset" \
        -c "shutdown"

echo "Done. Heartbeat LED on PD4 should be blinking at 1 Hz."
