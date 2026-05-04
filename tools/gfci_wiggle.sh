#!/usr/bin/env bash
# GFCI sense-pin (PE2) wiggle test against stock fw V1.0.066.
#
# Goal: validate the static-analysis finding from docs/re-stock-safety.md
# that PE2 is the polled GFCI fault sense input, active-high.
#
# Method: bias PE2 weakly via the MCU's internal ~40 kΩ pull-up /
# pull-down (NOT a push-pull drive — output PP would fight the GFCI
# module's output stage and risk damaging it). Then watch the stock
# firmware's fault-byte at RAM 0x200026a0 over a full state-machine
# cycle (~5 s). The agent decoded bit 1 of that byte = "PE2 high during
# steady-state" = "GFCI tripped while idle".
#
# Expected outcomes per docs/re-stock-safety.md § Q2 Confidence:
#
#   Active-high confirmed (FLIP THE GFCI GATE):
#     baseline_pe2  = 0   baseline_fault[0x200026a0] bit1 = 0
#     pull-up_pe2   = 1   pullup_fault                bit1 = 1
#     pull-down_pe2 = 0   pulldown_fault              bit1 = 0
#
#   Polarity inverted (idle-high = fault asserted):
#     baseline_pe2  = 1   baseline_fault              bit1 = 1
#     pull-down able to drag PE2 low and fault bit clears
#
#   Wrong pin (pulls move PE2 but fault byte unchanged):
#     PE2 toggles HIGH/LOW with pulls, but [0x200026a0] bit1 stays
#     constant → state machine isn't sampling this pin, look
#     elsewhere.
#
#   GFCI module pushing PP (pulls don't move PE2):
#     baseline_pe2 == pullup_pe2 == pulldown_pe2 — internal weak pull
#     can't overcome the module's output stage. Need physical
#     trace-cut + series resistor to probe further.
#
# REQUIRES: stock fw V1.0.066 flashed on the bench unit. Running under
# OpenBHZD this script is harmless (PE2 is not currently configured)
# but the fault byte at 0x200026a0 has no meaning under our fw.
#
# Usage:
#   tools/gfci_wiggle.sh test           # automated 3-phase wiggle
#   tools/gfci_wiggle.sh read           # one-shot status read
#   tools/gfci_wiggle.sh pull-up        # apply pull-up + resume
#   tools/gfci_wiggle.sh pull-down      # apply pull-down + resume
#   tools/gfci_wiggle.sh restore        # PE2 → input floating
#
# Caveat: the test halts the MCU briefly at each phase boundary. Any
# in-flight ADC scan / TLV traffic gets paused. Re-flash stock fw
# afterwards if it didn't auto-recover, but typically OpenOCD's
# `resume` puts it back into normal operation.
#
# GPIOE register layout (GD32F205 = STM32F1 family):
#   CTL0  @ 0x40011800   (CRL — pins 0..7, 4 bits each)
#   ISTAT @ 0x40011808   (IDR — input data, read-only)
#   OCTL  @ 0x4001180C   (ODR — output data; for input-PUPD selects high/low pull)
#   BOP   @ 0x40011810   (BSRR set bits)
#   BC    @ 0x40011814   (BRR clear bits)
#
# PE2 occupies bits 8..11 of CTL0:
#   0x4 = input floating          (default after reset)
#   0x8 = input pull-up/-down     (pull direction selected by ODR bit 2)
#   0x2 = output PP @ 2 MHz       (DO NOT USE — fights GFCI module)

set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
CFG="$REPO/tools/openocd-gd32f205.cfg"

GPIOE_CTL0=0x40011800
GPIOE_ISTAT=0x40011808
GPIOE_OCTL=0x4001180C
GPIOE_BOP=0x40011810
GPIOE_BC=0x40011814
FAULT_BYTE=0x200026a0

# mmw mask helpers — set bits 8,11 = 0, bit 10 = 1 → input PUPD;
# set bit 8 = 0, bit 10 = 0 → input floating; etc. We always
# clear the full 4-bit PE2 slot then set the desired pattern.
PE2_SLOT_CLR=0x00000F00
PE2_INPUT_FLOAT_SET=0x00000400   # CNF=01, MODE=00
PE2_INPUT_PUPD_SET=0x00000800    # CNF=10, MODE=00
PE2_HIGH_BIT=0x00000004          # ODR bit 2 / BOP bit 2 / BC bit 2

run_ocd() {
    openocd -f "$CFG" "$@" 2>&1
}

# Extract bit 2 of a hex word. Helper for parsing mdw output like
# "0x40011808: 0000abcd"
hex_bit2() {
    local v=${1#0x}
    printf '%d\n' $(( 0x$v >> 2 & 1 ))
}

# Extract bit 1 of byte byte at [0x200026a0]. mdb prints "0x200026a0: ab"
hex_bit1_byte() {
    local v=${1#0x}
    printf '%d\n' $(( 0x$v >> 1 & 1 ))
}

read_phase() {
    local label=$1
    local out
    out=$(run_ocd \
        -c init -c halt \
        -c "mdw $GPIOE_ISTAT 1" \
        -c "mdb $FAULT_BYTE 1" \
        -c resume -c shutdown 2>&1 | grep -E "0x40011808|0x200026a0" || true)
    local idr_line fault_line
    idr_line=$(echo "$out" | grep "0x40011808" | head -1)
    fault_line=$(echo "$out" | grep "0x200026a0" | head -1)
    local idr_val fault_val
    idr_val=$(echo "$idr_line" | awk '{print $NF}')
    fault_val=$(echo "$fault_line" | awk '{print $NF}')
    local pe2 fault_b1
    pe2=$(hex_bit2 "$idr_val")
    fault_b1=$(hex_bit1_byte "$fault_val")
    printf '%-12s PE2=%s   fault[0x200026a0]=%s   bit1=%s\n' \
        "$label" "$pe2" "$fault_val" "$fault_b1"
}

apply_pull_up() {
    run_ocd \
        -c init -c halt \
        -c "mmw $GPIOE_CTL0 $PE2_INPUT_PUPD_SET $PE2_SLOT_CLR" \
        -c "mmw $GPIOE_OCTL $PE2_HIGH_BIT 0" \
        -c resume -c shutdown >/dev/null
}

apply_pull_down() {
    run_ocd \
        -c init -c halt \
        -c "mmw $GPIOE_CTL0 $PE2_INPUT_PUPD_SET $PE2_SLOT_CLR" \
        -c "mmw $GPIOE_OCTL 0 $PE2_HIGH_BIT" \
        -c resume -c shutdown >/dev/null
}

restore_float() {
    run_ocd \
        -c init -c halt \
        -c "mmw $GPIOE_CTL0 $PE2_INPUT_FLOAT_SET $PE2_SLOT_CLR" \
        -c resume -c shutdown >/dev/null
}

case "${1:-help}" in
    test)
        echo "GFCI PE2 wiggle test — stock fw V1.0.066 expected"
        echo "Phase 1: baseline (PE2 input floating, no bias)"
        restore_float
        sleep 6
        read_phase baseline

        echo "Phase 2: PE2 input pull-up (~40k to 3.3 V)"
        apply_pull_up
        sleep 6
        read_phase pull-up

        echo "Phase 3: PE2 input pull-down (~40k to GND)"
        apply_pull_down
        sleep 6
        read_phase pull-down

        echo "Phase 4: restore (PE2 input floating)"
        restore_float
        read_phase restored

        echo
        echo "Interpretation guide:"
        echo "  active-high (gate flip-OK): bit1 = 0 -> 1 -> 0  across phases 1/2/3"
        echo "  inverted polarity:          bit1 = 1 -> 1 -> 0"
        echo "  wrong pin / state-machine dormant: bit1 unchanged across phases"
        echo "  module pushing PP:          PE2 unchanged across phases"
        ;;

    pull-up)   apply_pull_up;   read_phase pull-up   ;;
    pull-down) apply_pull_down; read_phase pull-down ;;
    restore)   restore_float;   read_phase restored  ;;
    read)      read_phase read  ;;

    *)
        sed -n '1,/^set -euo/p' "$0" | sed 's/^# \?//'
        exit 1
        ;;
esac
