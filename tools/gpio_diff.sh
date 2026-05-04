#!/usr/bin/env bash
# GPIO snapshot + diff helper for "drive an external stimulus, see
# which MCU pin changes" investigations.
#
# Pattern: take a baseline ISTAT snapshot of all 5 GPIO ports under
# halt+poke, then ask the user to apply the stimulus, then take a
# second snapshot and report the bit-level diff with PXn labels.
#
# Targets the open question of which MCU pin observes the GFCI
# module's fault output (and the broader weld-feedback hunt). Works
# against either fw — we read ISTAT registers directly via OpenOCD
# without involving the running firmware.
#
# Usage:
#   tools/gpio_diff.sh save              # snapshot → /tmp/.openbhzd-gpio-baseline
#   tools/gpio_diff.sh diff              # snapshot now, diff vs baseline
#   tools/gpio_diff.sh watch [interval]  # poll + print diffs, default 1 s
#   tools/gpio_diff.sh dump              # one-shot full ISTAT dump

set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
CFG="$REPO/tools/openocd-gd32f205.cfg"
BASELINE="${TMPDIR:-/tmp}/.openbhzd-gpio-baseline"

GPIOA_ISTAT=0x40010808
GPIOB_ISTAT=0x40010C08
GPIOC_ISTAT=0x40011008
GPIOD_ISTAT=0x40011408
GPIOE_ISTAT=0x40011808

# Labels for noteworthy pins (from pinout.md). Used to annotate diffs;
# absence of a label is fine, the bit number alone is still useful.
declare -A LABEL=(
    [PA2]="GUN-NTC"  [PA3]="WALL-NTC"  [PA4]="CP-READBACK"
    [PA7]="CC-SENSE" [PA9]="USART0-TX(no-pad)" [PA10]="USART0-RX(no-pad)"
    [PA15]="WS2812-DIN"
    [PB0]="AC-PRESENCE?" [PB2]="BUZZER" [PB3]="W25Q-SCK"
    [PB4]="W25Q-MISO" [PB5]="W25Q-MOSI" [PB6]="W25Q-CS"
    [PB9]="U11-G0" [PB12]="FORCE-OPEN-LATCH-OUT"
    [PC0]="CT-SENSE" [PC1]="LCT-SENSE" [PC3]="BTN-LADDER"
    [PC5]="PE-ADC" [PC8]="BTN(stock-only)" [PC9]="INTERNAL-BTN"
    [PC10]="UART4-TX(LCD)" [PC11]="UART4-RX(LCD)"
    [PC12]="UART5-TX(FC41D)"
    [PD0]="FC41D-CEN" [PD1]="FC41D-WAKE" [PD2]="UART5-RX(FC41D)"
    [PD4]="HEARTBEAT-LED" [PD5]="USART1-TX(printk)"
    [PD6]="USART1-RX" [PD10]="DIP4" [PD11]="DIP3"
    [PD12]="DIP2" [PD13]="DIP1" [PD15]="U11-G1"
    [PE0]="AUX-RELAY" [PE1]="FC41D-VEN" [PE2]="GFCI-SENSE?"
    [PE3]="GFCI-CAL" [PE4]="GFCI-???" [PE12]="MAIN-CONTACTOR"
    [PE13]="CP-PWM-OUT"
)

read_istats() {
    openocd -f "$CFG" \
        -c init -c halt \
        -c "mdw $GPIOA_ISTAT 1" \
        -c "mdw $GPIOB_ISTAT 1" \
        -c "mdw $GPIOC_ISTAT 1" \
        -c "mdw $GPIOD_ISTAT 1" \
        -c "mdw $GPIOE_ISTAT 1" \
        -c resume -c shutdown 2>&1 \
        | grep -E "^0x4001(0|1)" \
        | awk '{print $1, $NF}'
}

# Convert "0x40010808 0x0000abcd" lines to "GPIOA 0xabcd" mapping
parse_istats() {
    local istats=$1
    declare -A out
    while read -r addr val; do
        case "$addr" in
            0x40010808) out[A]=$val ;;
            0x40010C08) out[B]=$val ;;
            0x40011008) out[C]=$val ;;
            0x40011408) out[D]=$val ;;
            0x40011808) out[E]=$val ;;
        esac
    done <<< "$istats"
    for k in A B C D E; do
        printf 'GPIO%s=%s\n' "$k" "${out[$k]:-???}"
    done
}

# Diff two parsed maps. Args: file1 file2. Prints e.g. "PE2: 1→0  (GFCI-SENSE?)"
diff_maps() {
    local before=$1 after=$2
    local found=0
    while IFS= read -r line; do
        local port val_before val_after
        port=$(echo "$line" | cut -d= -f1 | sed 's/GPIO//')
        val_before=$(echo "$line" | cut -d= -f2)
        val_after=$(grep "^GPIO$port=" "$after" | cut -d= -f2 || true)
        [ -z "$val_after" ] && continue
        local diff=$(( val_before ^ val_after ))
        [ $diff -eq 0 ] && continue
        for bit in $(seq 0 15); do
            local mask=$((1 << bit))
            if [ $((diff & mask)) -ne 0 ]; then
                local b_bit=$(( (val_before >> bit) & 1 ))
                local a_bit=$(( (val_after  >> bit) & 1 ))
                local pin="P${port}${bit}"
                local label="${LABEL[$pin]:-}"
                if [ -n "$label" ]; then
                    printf '  %s: %d → %d  (%s)\n' "$pin" "$b_bit" "$a_bit" "$label"
                else
                    printf '  %s: %d → %d\n' "$pin" "$b_bit" "$a_bit"
                fi
                found=1
            fi
        done
    done < "$before"
    [ $found -eq 0 ] && echo "  (no GPIO bits changed)"
}

snapshot_to() {
    local out=$1
    local raw=$(read_istats)
    parse_istats "$raw" > "$out"
}

case "${1:-help}" in
    save)
        snapshot_to "$BASELINE"
        echo "Baseline saved to $BASELINE:"
        cat "$BASELINE" | sed 's/^/  /'
        ;;

    diff)
        if [ ! -f "$BASELINE" ]; then
            echo "No baseline at $BASELINE — run \`save\` first." >&2
            exit 1
        fi
        local_now="${TMPDIR:-/tmp}/.openbhzd-gpio-now"
        snapshot_to "$local_now"
        echo "Baseline:"
        cat "$BASELINE" | sed 's/^/  /'
        echo "Now:"
        cat "$local_now" | sed 's/^/  /'
        echo "Diff:"
        diff_maps "$BASELINE" "$local_now"
        ;;

    watch)
        interval="${2:-1}"
        echo "Watching every ${interval}s — drive the stimulus and watch for diffs."
        echo "Ctrl-C to stop."
        local_prev="${TMPDIR:-/tmp}/.openbhzd-gpio-prev"
        local_now="${TMPDIR:-/tmp}/.openbhzd-gpio-now"
        snapshot_to "$local_prev"
        echo "[$(date '+%H:%M:%S')] initial:"
        cat "$local_prev" | sed 's/^/  /'
        while true; do
            sleep "$interval"
            snapshot_to "$local_now"
            if ! cmp -s "$local_prev" "$local_now"; then
                echo "[$(date '+%H:%M:%S')] change:"
                diff_maps "$local_prev" "$local_now"
                cp "$local_now" "$local_prev"
            fi
        done
        ;;

    dump)
        snapshot_to "/dev/stdout"
        ;;

    *)
        sed -n '1,/^set -euo/p' "$0" | sed 's/^# \?//'
        exit 1
        ;;
esac
