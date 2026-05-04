#!/usr/bin/env bash
# Drive PB9 and/or PD15 (the two MCU pins traced to U11 / BL0939) at
# 1 Hz so you can probe each BL0939 pin with a multimeter or scope to
# see which destination toggles. Mirrors the PE3-destination wiggle
# pattern that confirmed PE3 → GFCI CAL line on 2026-05-02.
#
# Both pins are already configured as output PP idle LOW by
# gpio_init_all() (under the legacy PIN_U11_G0/G1 macros), so we just
# toggle BSRR/BRR via OpenOCD halt+poke. Our firmware doesn't drive
# them at runtime, so there's no fight to be had.
#
# Reference (BL0939 SOP16L pinout, per the datasheet):
#   pin  9: ZX        (zero-cross output)
#   pin 10: I_leak    (RCD/leak alarm output → confirmed = MCU PE2)
#   pin 11: CF        (energy pulse output)
#   pin 12: SEL       (UART/SPI mode select; pulldown → UART when float)
#   pin 13: SCLK      (SPI clock; NC in UART mode)
#   pin 14: RX/SDI    (UART RX / SPI DIN ← MCU TX)
#   pin 15: TX/SDO    (UART TX / SPI DOUT → MCU RX, ext pull-up)
#
# UART comms = 4800 bps; SPI = up to 900 kHz. With only two traces
# from MCU to U11, UART is the most likely OEM choice (SEL hardwired
# to GND; just TX + RX needed). PB9 and PD15 are most likely
# bit-banged UART TX and RX.
#
# Usage:
#   tools/u11_wiggle.sh pb9 [period_ms]   # toggle PB9, default 1000
#   tools/u11_wiggle.sh pd15 [period_ms]
#   tools/u11_wiggle.sh both              # alternate halves: PB9 hi/PD15 lo, swap
#   tools/u11_wiggle.sh stop              # drive both LOW, exit
#
# Probe procedure:
#   1. Run `pb9` in this terminal.
#   2. Touch multimeter probe to BL0939 pins 12..15 (SEL, SCLK,
#      RX/SDI, TX/SDO) one at a time. The pin showing a 0 V ↔ 3.3 V
#      square wave at 1 Hz is what PB9 connects to.
#   3. Ctrl-C, then `tools/u11_wiggle.sh stop` to clean up.
#   4. Repeat with `pd15`.

set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
CFG="$REPO/tools/openocd-gd32f205.cfg"

GPIOB_BOP=0x40010C10   # BSRR — set bits
GPIOB_BC=0x40010C14    # BRR  — clear bits
GPIOD_BOP=0x40011410
GPIOD_BC=0x40011414

PB9_MASK=0x00000200    # bit 9
PD15_MASK=0x00008000   # bit 15

write_word() {
    local addr=$1 val=$2
    openocd -f "$CFG" \
        -c init -c halt \
        -c "mww $addr $val" \
        -c resume -c shutdown >/dev/null 2>&1
}

set_pb9_hi()   { write_word $GPIOB_BOP $PB9_MASK; }
set_pb9_lo()   { write_word $GPIOB_BC  $PB9_MASK; }
set_pd15_hi()  { write_word $GPIOD_BOP $PD15_MASK; }
set_pd15_lo()  { write_word $GPIOD_BC  $PD15_MASK; }

cleanup() {
    echo
    echo "Driving PB9, PD15 LOW (idle)…"
    set_pb9_lo
    set_pd15_lo
    exit 0
}

wiggle_one() {
    local hi=$1 lo=$2 name=$3 period_ms=$4
    local half_s
    half_s=$(awk "BEGIN { printf \"%.3f\", $period_ms / 2000 }")
    echo "Wiggling $name at $((1000 / period_ms * 1000)) mHz (period ${period_ms} ms). Ctrl-C to stop."
    trap cleanup INT
    while true; do
        $hi
        sleep "$half_s"
        $lo
        sleep "$half_s"
    done
}

wiggle_both() {
    local period_ms=${1:-1000}
    local half_s
    half_s=$(awk "BEGIN { printf \"%.3f\", $period_ms / 2000 }")
    echo "Wiggling PB9 and PD15 in anti-phase, period ${period_ms} ms. Ctrl-C to stop."
    trap cleanup INT
    while true; do
        set_pb9_hi
        set_pd15_lo
        sleep "$half_s"
        set_pb9_lo
        set_pd15_hi
        sleep "$half_s"
    done
}

case "${1:-help}" in
    pb9)   wiggle_one set_pb9_hi  set_pb9_lo  PB9  "${2:-1000}" ;;
    pd15)  wiggle_one set_pd15_hi set_pd15_lo PD15 "${2:-1000}" ;;
    both)  wiggle_both "${2:-1000}" ;;
    stop)  set_pb9_lo; set_pd15_lo; echo "PB9, PD15 → LOW" ;;
    *)
        sed -n '1,/^set -euo/p' "$0" | sed 's/^# \?//'
        exit 1
        ;;
esac
