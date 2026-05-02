#!/usr/bin/env bash
# Restore the stock GD32F205 firmware from recovery/stock-mcu-V1.0.066.bin.
# Use this if a new flash bricks the unit or you want to revert to factory.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFG="$REPO_ROOT/tools/openocd-gd32f205.cfg"
BIN="$REPO_ROOT/recovery/stock-mcu-V1.0.066.bin"

if [[ ! -e "$BIN" ]]; then
    echo "ERROR: $BIN not found. Cannot restore stock."
    exit 1
fi

SIZE=$(stat -c '%s' "$BIN")
if [[ "$SIZE" -ne 524288 ]]; then
    echo "ERROR: $BIN is $SIZE bytes (expected 524288). Refusing."
    exit 1
fi

echo "Restoring stock firmware from $BIN ..."
openocd -f "$CFG" \
        -c "init" \
        -c "reset halt" \
        -c "flash write_image erase $BIN 0x08000000 bin" \
        -c "verify_image $BIN 0x08000000 bin" \
        -c "reset run" \
        -c "shutdown"

echo "Stock V1.0.066 restored."
