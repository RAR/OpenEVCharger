#!/usr/bin/env bash
# flash_nexcyber.sh — Flash an OpenEVCharger nexcyber build to the
# N32G45x via ST-Link + OpenOCD.
#
# Why this isn't just `openocd ... program FILE`:
#   The Nations N32G45x reports DEV_ID 0x511, which OpenOCD's
#   stm32f1x flash driver auto_probe rejects (not in the STM32
#   allowlist). The rippleon GD32F205 trick (DBGMCU IDCODE 0x418,
#   which DOES match STM32F1) doesn't work here because Nations
#   chose a Nations-specific dev_id.
#
#   So we drive the F1-style FPEC controller manually via the
#   companion tools/openocd-n32g45x-flash.tcl script. It:
#     1. Loads the binary into RAM at 0x20002000 via load_image
#     2. Unlocks the FLASH controller (KEYR sequence)
#     3. Page-erases the target region (2 KB pages)
#     4. Sets PG=1 in FLASH_CR, then word-writes RAM→flash
#     5. Verifies spot-checks, resets, runs
#
#   Important gotcha bench-confirmed 2026-05-11: ST-Link AHB-AP
#   sub-word writes (mwh) silently fail in flash PG mode on this
#   chip. Word writes (mww) work cleanly — the flash controller
#   accepts them as 2-halfword bursts internally. The TCL script
#   uses mww throughout.
#
# Usage:
#   tools/flash_nexcyber.sh                       # → build_nexcyber/openevcharger.bin
#   tools/flash_nexcyber.sh path/to/some.bin

set -euo pipefail

BIN="${1:-build_nexcyber/openevcharger.bin}"

if [[ ! -f "$BIN" ]]; then
    echo "flash_nexcyber: binary not found: $BIN" >&2
    exit 1
fi

# Resolve to absolute path because openocd's load_image runs in its
# own working directory.
BIN="$(realpath "$BIN")"
SCRIPT_DIR="$(dirname "$(realpath "$0")")"

NX_FLASH_BIN="$BIN" exec openocd \
    -f interface/stlink.cfg \
    -c "transport select hla_swd" \
    -f target/stm32f4x.cfg \
    -f "$SCRIPT_DIR/openocd-n32g45x-flash.tcl"
