#!/usr/bin/env bash
# Halt the GD32F205 over SWD and print where it's stuck. Use after a
# crash / hang / apply-path stall to locate the offending instruction.
#
# Reads:
#   PC     — program counter (instruction faulting / spinning)
#   SP     — stack pointer
#   LR     — link register (return address — useful for spin loops)
#   CFSR   — Configurable Fault Status Register (which fault, why)
#   HFSR   — Hard Fault Status Register
#   MMFAR  — MemManage fault address
#   BFAR   — BusFault address
#   DHCSR  — Debug Halt Control / Status (so we can tell debug-attach
#            from a "real" halt)
#   .ramfunc range — _sramfunc..._eramfunc, so you can compare PC against it.

set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFG="$REPO_ROOT/tools/openocd-gd32f205.cfg"
MAP="$REPO_ROOT/build/ota_baseline/openevcharger.map"
[[ -e "$MAP" ]] || MAP="$REPO_ROOT/build/openevcharger.map"

echo "=== running-image .ramfunc range (from $MAP) ==="
grep -E "^\s*0x[0-9a-f]+\s+_(s|e)ramfunc" "$MAP" || echo "(no .ramfunc syms in map)"
echo

openocd -f "$CFG" \
        -c "init" \
        -c "halt" \
        -c "echo {=== core regs ===}" \
        -c "reg pc" \
        -c "reg sp" \
        -c "reg lr" \
        -c "reg xPSR" \
        -c "echo {=== fault registers ===}" \
        -c "echo {CFSR  (0xE000ED28):}" \
        -c "mdw 0xE000ED28" \
        -c "echo {HFSR  (0xE000ED2C):}" \
        -c "mdw 0xE000ED2C" \
        -c "echo {MMFAR (0xE000ED34):}" \
        -c "mdw 0xE000ED34" \
        -c "echo {BFAR  (0xE000ED38):}" \
        -c "mdw 0xE000ED38" \
        -c "echo {DHCSR (0xE000EDF0):}" \
        -c "mdw 0xE000EDF0" \
        -c "echo {=== FMC bank0 status ===}" \
        -c "echo {FMC_STAT0 (0x4002200C):}" \
        -c "mdw 0x4002200C" \
        -c "echo {FMC_CTL0  (0x40022010):}" \
        -c "mdw 0x40022010" \
        -c "echo {FMC_ADDR0 (0x40022014):}" \
        -c "mdw 0x40022014" \
        -c "echo {=== first 32 words of bank0 ===}" \
        -c "mdw 0x08000000 32" \
        -c "shutdown"
