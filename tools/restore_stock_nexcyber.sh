#!/usr/bin/env bash
# restore_stock_nexcyber.sh — Re-flash the original stock V1.0.066
# firmware back onto the bench unit's N32G45x.
#
# Why this isn't just `tools/flash_nexcyber.sh stock-mcu-2026-05-07.bin`:
#   The 128 KB stock dump doesn't fit in the chip's 80 KB SRAM in one
#   load_image, so we chunk into 2x 64 KB passes. Each pass:
#     - staging at 0x20002000 (load_image ~64 KB)
#     - erase pages for its target region (32 pages × 2 KB)
#     - word-program from RAM → flash
#   First pass writes 0x08000000-0x0800FFFF, second writes
#   0x08010000-0x0801FFFF (skipping the PERSIST tail that our linker
#   carves off, but stock fw was 128 KB end-to-end and the upper region
#   matches its original layout anyway).
#
# Verifies sha256 against the projectstate-recorded digest before flash.
#
# Usage:
#   tools/restore_stock_nexcyber.sh                       # default backup path
#   tools/restore_stock_nexcyber.sh /path/to/dump.bin

set -euo pipefail

DEFAULT_BACKUP="/home/rar/device-configs/esphome/testcharger/stock-mcu-2026-05-07.bin"
EXPECTED_SHA="d1d9c2e5a9d6c1e5f770390c26c302855940cdec69acdf60d356dac62d0dda00"

BIN="${1:-$DEFAULT_BACKUP}"

if [[ ! -f "$BIN" ]]; then
    echo "restore_stock_nexcyber: backup not found: $BIN" >&2
    exit 1
fi

SIZE=$(stat -c %s "$BIN")
if [[ "$SIZE" != "131072" ]]; then
    echo "restore_stock_nexcyber: expected 131072 B backup, got $SIZE" >&2
    exit 1
fi

ACTUAL_SHA=$(sha256sum "$BIN" | awk '{print $1}')
if [[ "$ACTUAL_SHA" != "$EXPECTED_SHA" ]]; then
    echo "restore_stock_nexcyber: SHA mismatch!" >&2
    echo "  expected: $EXPECTED_SHA" >&2
    echo "  actual:   $ACTUAL_SHA" >&2
    echo "  refusing to flash unknown image" >&2
    exit 1
fi
echo "restore: sha256 match ($EXPECTED_SHA)"

BIN_ABS="$(realpath "$BIN")"
SCRIPT_DIR="$(dirname "$(realpath "$0")")"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# Split into two 64 KB chunks.
dd if="$BIN_ABS" of="$TMPDIR/chunk_a.bin" bs=65536 count=1 status=none
dd if="$BIN_ABS" of="$TMPDIR/chunk_b.bin" bs=65536 skip=1 count=1 status=none
echo "restore: split into 2x 64 KB ($TMPDIR/chunk_{a,b}.bin)"

run_openocd() {
    local chunk="$1"
    local offset_hex="$2"
    local no_reset="$3"
    NX_FLASH_BIN="$chunk" \
    NX_FLASH_TARGET_OFFSET="$offset_hex" \
    NX_FLASH_NO_RESET_RUN="$no_reset" \
    openocd \
        -f interface/stlink.cfg \
        -c "transport select hla_swd" \
        -f target/stm32f4x.cfg \
        -f "$SCRIPT_DIR/openocd-n32g45x-flash.tcl"
}

echo
echo "=== Pass 1/2: write 0x08000000-0x0800FFFF ==="
run_openocd "$TMPDIR/chunk_a.bin" 0 1

echo
echo "=== Pass 2/2: write 0x08010000-0x0801FFFF ==="
run_openocd "$TMPDIR/chunk_b.bin" 0x10000 0

echo
echo "restore: done. Stock V1.0.066 firmware is now running."
echo "         Watch the LCD — it should boot into the stock UI."
