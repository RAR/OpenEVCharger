#!/usr/bin/env bash
# Dump the GD32F205VC main flash (256 KB) to recovery/stock-mcu-V1.0.066.bin
# Run BEFORE flashing any new firmware. Always runs read-only.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$REPO_ROOT/recovery/stock-mcu-V1.0.066.bin"
CFG="$REPO_ROOT/tools/openocd-gd32f205.cfg"

mkdir -p "$REPO_ROOT/recovery"

if [[ -e "$OUT" ]]; then
    echo "Backup already exists at $OUT"
    echo "Refusing to overwrite. Move/rename it if you really want a fresh dump."
    exit 1
fi

echo "Dumping 512 KB main flash to $OUT ..."
openocd -f "$CFG" \
        -c "init" \
        -c "reset halt" \
        -c "dump_image $OUT 0x08000000 0x80000" \
        -c "reset run" \
        -c "shutdown"

SIZE=$(stat -c '%s' "$OUT")
if [[ "$SIZE" -ne 524288 ]]; then
    echo "ERROR: dump size $SIZE != 524288 expected. Backup is suspect; investigate before proceeding."
    exit 2
fi

echo "OK: $OUT ($SIZE bytes)"
sha256sum "$OUT"
