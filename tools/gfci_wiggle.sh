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
# OpenEVCharger this script is harmless (PE2 is not currently configured)
# but the fault byte at 0x200026a0 has no meaning under our fw.
#
# Two complementary tests:
#
#   cal-test:  Drive PE3 in both directions and read PE2 around each
#              step. Mirrors the prior PE3-destination wiggle that
#              confirmed PE3 → GFCI CAL line. Works under EITHER
#              stock fw or OpenEVCharger (we drive GPIOs via OpenOCD
#              directly). AC must be live so the module is powered.
#              This is the SHORTER, MORE DECISIVE test — start here.
#
#   pull-test: Bias PE2 with the MCU's internal pull-up / pull-down,
#              then watch the stock fw's fault byte at 0x200026a0.
#              Indirect — works only under stock fw and only confirms
#              that stock fw's GFCI state machine is actually running.
#
# Usage:
#   tools/gfci_wiggle.sh cal-test       # PE3 wiggle (1 s hold) + PE2 sample (any fw)
#   tools/gfci_wiggle.sh seq-test       # full stock CAL sequence + PE2 sample (any fw)
#   tools/gfci_wiggle.sh pull-test      # PE2 pull bias + fault byte (stock fw)
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

# CAL drive: wiggle PE3 in both directions and read PE2 around each
# step. Mirrors the prior PE3 destination wiggle that confirmed PE3 →
# GFCI CAL line (pinout.md note: MCU low → CAL = 5 V at GFCI side,
# i.e. CAL asserted; MCU high → CAL = 0 V, idle). If PE2 is truly
# the GFCI fault-output sense, asserting CAL should make the module
# self-test trip → fault output asserts → PE2 changes state.
#
# Works under EITHER stock fw or OpenEVCharger: we manipulate the GPIOs
# directly via OpenOCD halt+poke, so the running fw doesn't need to
# participate. AC must be live so the GFCI module is powered.
#
# Caveat: under stock fw, PE3 is being driven by the firmware's GFCI
# state machine on a ~5 s cycle. Our halt+poke wins the race for the
# instant we set it, but on resume the firmware will overwrite our
# value within milliseconds. We sample PE2 BEFORE resume so the
# read is contemporaneous with our driven PE3.
PE3_HIGH_BIT=0x00000008
# PE3 occupies bits 12..15 of CTL0
PE3_SLOT_CLR=0x0000F000
PE3_OUTPUT_PP_2MHZ_SET=0x00002000   # CNF=00 (out PP), MODE=10 (2 MHz)

# Drive PE3 to a level (0 or 1) under halt, sample PE2 IDR while
# still halted, then resume. Returns the read value.
drive_pe3_and_sample() {
    local level=$1   # "high" or "low"
    local set_cmd
    if [ "$level" = "high" ]; then
        set_cmd="mww $GPIOE_BOP $PE3_HIGH_BIT"
    else
        set_cmd="mww $GPIOE_BC $PE3_HIGH_BIT"
    fi
    # Hold time matches stock fw's CAL pulse window (~800 ms). 100 ms
    # was too short — the GFCI module's internal integrator doesn't
    # commit a fault output transition that fast. OpenOCD telnet sleep
    # is in ms.
    run_ocd \
        -c init -c halt \
        -c "mmw $GPIOE_CTL0 $PE3_OUTPUT_PP_2MHZ_SET $PE3_SLOT_CLR" \
        -c "$set_cmd" \
        -c "sleep 1000" \
        -c "mdw $GPIOE_ISTAT 1" \
        -c resume -c shutdown 2>&1 | grep "0x40011808" | head -1
}

# Run the full stock GFCI state-machine sequence and sample PE2 at
# the post-CAL settle point (case 4 of the agent's decode at flash
# 0x080128CE). Sequence:
#   step 0: PE3=0, PE4=0   1100 ms idle
#   step 1: PE3=1 (CAL on per agent's decode)  800 ms
#   step 2: PE3=0, PE4=0   550 ms (post-CAL settle)
#   step 3: PE4=1          200 ms
#   step 4: SAMPLE PE2 — should read HIGH if active-high & sense OK
#   step 5: PE4=0
#   restore PE3 / PE4 to inputs
#
# Note: agent decoded "PE3=1 = CAL on" but pinout.md says MCU low →
# CAL active. If agent's polarity is wrong, swap step 1's drive.
PE4_HIGH_BIT=0x00000010
PE4_SLOT_CLR=0x000F0000
PE4_OUTPUT_PP_2MHZ_SET=0x00020000

drive_full_sequence() {
    local pe3_active=$1   # "high" or "low"
    local set_pe3 clr_pe3
    if [ "$pe3_active" = "high" ]; then
        set_pe3="mww $GPIOE_BOP $PE3_HIGH_BIT"
        clr_pe3="mww $GPIOE_BC $PE3_HIGH_BIT"
    else
        set_pe3="mww $GPIOE_BC $PE3_HIGH_BIT"
        clr_pe3="mww $GPIOE_BOP $PE3_HIGH_BIT"
    fi
    run_ocd \
        -c init -c halt \
        -c "mmw $GPIOE_CTL0 $PE3_OUTPUT_PP_2MHZ_SET $PE3_SLOT_CLR" \
        -c "mmw $GPIOE_CTL0 $PE4_OUTPUT_PP_2MHZ_SET $PE4_SLOT_CLR" \
        -c "$clr_pe3" \
        -c "mww $GPIOE_BC $PE4_HIGH_BIT" \
        -c "sleep 1100" \
        -c "$set_pe3" \
        -c "sleep 800" \
        -c "$clr_pe3" \
        -c "sleep 550" \
        -c "mww $GPIOE_BOP $PE4_HIGH_BIT" \
        -c "sleep 200" \
        -c "mdw $GPIOE_ISTAT 1" \
        -c "mww $GPIOE_BC $PE4_HIGH_BIT" \
        -c "mmw $GPIOE_CTL0 $PE2_INPUT_FLOAT_SET $PE3_SLOT_CLR" \
        -c "mmw $GPIOE_CTL0 $PE2_INPUT_FLOAT_SET $PE4_SLOT_CLR" \
        -c resume -c shutdown 2>&1 | grep "0x40011808" | head -1
}

case "${1:-help}" in
    seq-test)
        echo "GFCI sequenced wiggle: full stock-fw CAL state machine, sample at case 4"
        echo "REQUIREMENT: AC must be live so the GFCI module is powered."
        echo

        echo "Phase 1: baseline (no driver)"
        restore_float
        read_phase baseline

        echo "Phase 2: full sequence with PE3 ACTIVE = LOW (per pinout.md polarity)"
        out=$(drive_full_sequence low)
        idr=$(echo "$out" | awk '{print $NF}')
        pe2=$(hex_bit2 "$idr")
        printf '%-12s PE2=%s   (GPIOE.IDR=%s)\n' "seq-low" "$pe2" "$idr"

        echo "Phase 3: full sequence with PE3 ACTIVE = HIGH (per agent's polarity)"
        out=$(drive_full_sequence high)
        idr=$(echo "$out" | awk '{print $NF}')
        pe2=$(hex_bit2 "$idr")
        printf '%-12s PE2=%s   (GPIOE.IDR=%s)\n' "seq-high" "$pe2" "$idr"

        echo
        echo "If EITHER seq-low or seq-high shows PE2 differing from baseline,"
        echo "PE2 is the fault sense and the diff direction nails the polarity."
        ;;

    cal-test)
        echo "GFCI cal wiggle: drive PE3, observe PE2 (works under any fw)"
        echo "REQUIREMENT: AC must be live so the GFCI module is powered."
        echo

        echo "Phase 1: baseline (no driver)"
        restore_float
        read_phase baseline

        echo "Phase 2: PE3 driven LOW (= CAL asserted at GFCI side)"
        out=$(drive_pe3_and_sample low)
        idr=$(echo "$out" | awk '{print $NF}')
        pe2=$(hex_bit2 "$idr")
        printf '%-12s PE2=%s   (GPIOE.IDR=%s)\n' "cal-on" "$pe2" "$idr"

        echo "Phase 3: PE3 driven HIGH (= CAL idle at GFCI side)"
        out=$(drive_pe3_and_sample high)
        idr=$(echo "$out" | awk '{print $NF}')
        pe2=$(hex_bit2 "$idr")
        printf '%-12s PE2=%s   (GPIOE.IDR=%s)\n' "cal-off" "$pe2" "$idr"

        echo
        echo "Interpretation:"
        echo "  PE2 changes between cal-on / cal-off → CONFIRMED PE2 = GFCI fault sense"
        echo "  PE2 unchanged → either wrong pin, GFCI not powered, or module already faulted"
        echo "  PE2 = 1 in cal-on, 0 in cal-off → active-high at MCU (matches agent's decode)"
        echo "  PE2 = 0 in cal-on, 1 in cal-off → active-low (polarity inverted from agent)"
        ;;

    pull-test)
        echo "GFCI PE2 pull-bias test — REQUIRES stock fw V1.0.066 (uses fault byte)"
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
