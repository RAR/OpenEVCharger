#!/usr/bin/env python3
"""uart_wiggle.py — Slow-wiggle each candidate UART_TX MCU pin so an
operator with a multimeter on the LCD RX pad can see which one is
wired.

Walks each candidate pin in turn:
  1. Halt MCU.
  2. Configure pin OUT_PP @ 2 MHz.
  3. Toggle HIGH/LOW at 1 Hz for `--seconds` (default 6) — slow enough
     for a multimeter's auto-range to track.
  4. Restore pin to INPUT_FLOATING.
  5. Prompt operator: "Did the multimeter on LCD-RX track this pin?"
At the end: resume MCU, print summary.

Candidates: all default UART TX pins on N32G45x + the USART3 remap
positions (PC10, PD8). PD8 is unlikely but cheap to include.

Usage:
    tools/uart_wiggle.py            # default 6 sec per pin
    tools/uart_wiggle.py 4          # 4 sec per pin (faster sweep)
"""
from __future__ import annotations

import subprocess
import sys
import time

CANDIDATES = [
    # First pass — already-tested (RX pad confirmed PA2). Re-test on RX1.
    ("PA2",  "A",  2,  "USART2_TX (already confirmed on RX pad)"),
    ("PA9",  "A",  9,  "USART1_TX default — log UART suspect"),
    ("PA10", "A", 10,  "USART1_RX default — but could be alt"),
    ("PB6",  "B",  6,  "USART1_TX remap / TIM4_CH1 / UART7 candidate"),
    ("PB7",  "B",  7,  "USART1_RX remap / TIM4_CH2 / UART7 candidate"),
    ("PB10", "B", 10,  "USART3_TX no remap"),
    ("PB11", "B", 11,  "USART3_RX no remap"),
    ("PC10", "C", 10,  "USART3 partial-remap TX / UART4_TX / blue LED pad"),
    ("PC11", "C", 11,  "USART3 partial-remap RX / UART4_RX / safety HB"),
    ("PC12", "C", 12,  "UART5_TX default / green LED pad"),
    ("PD2",  "D",  2,  "UART5_RX default"),
    ("PD5",  "D",  5,  "USART2 remap TX"),
    ("PD6",  "D",  6,  "USART2 remap RX"),
    ("PD8",  "D",  8,  "USART3 full-remap TX"),
    ("PD9",  "D",  9,  "USART3 full-remap RX"),
    ("PE0",  "E",  0,  "UART7 candidate (Nations)"),
    ("PE1",  "E",  1,  "UART7 candidate (Nations)"),
    ("PE10", "E", 10,  "UART7 candidate (Nations)"),
    ("PE11", "E", 11,  "UART7 candidate (Nations)"),
]

GPIO_BASES = {"A": 0x40010800, "B": 0x40010C00, "C": 0x40011000,
              "D": 0x40011400, "E": 0x40011800}
GPIO_CRL  = 0x00
GPIO_CRH  = 0x04
GPIO_BSRR = 0x10
GPIO_BRR  = 0x14

OPENOCD_BASE = [
    "openocd",
    "-f", "interface/stlink.cfg",
    "-c", "transport select hla_swd",
    "-f", "target/stm32f4x.cfg",
]


def run_openocd(cmds, halt=False):
    args = list(OPENOCD_BASE) + ["-c", "init"]
    if halt:
        args += ["-c", "halt"]
    for c in cmds:
        args += ["-c", c]
    args += ["-c", "shutdown"]
    return subprocess.run(args, capture_output=True, text=True, timeout=20)


def parse_word(text, addr):
    needle = f"{addr:#010x}:"
    for line in text.splitlines():
        s = line.strip()
        if s.startswith(needle):
            return int(s[len(needle):].split()[0], 16)
    return None


def config_out_pp(port, pin):
    base = GPIO_BASES[port]
    cr_addr = base + (GPIO_CRL if pin < 8 else GPIO_CRH)
    nibble_pos = (pin if pin < 8 else pin - 8) * 4
    text = run_openocd([f"mdw {cr_addr:#x} 1"], halt=True).stderr
    cur = parse_word(text, cr_addr) or 0
    new_cr = (cur & ~(0xF << nibble_pos)) | (0x2 << nibble_pos)
    run_openocd([f"mww {cr_addr:#x} {new_cr:#x}"])


def restore_input(port, pin):
    base = GPIO_BASES[port]
    cr_addr = base + (GPIO_CRL if pin < 8 else GPIO_CRH)
    nibble_pos = (pin if pin < 8 else pin - 8) * 4
    text = run_openocd([f"mdw {cr_addr:#x} 1"]).stderr
    cur = parse_word(text, cr_addr) or 0
    new_cr = (cur & ~(0xF << nibble_pos)) | (0x4 << nibble_pos)
    run_openocd([f"mww {cr_addr:#x} {new_cr:#x}"])


def wiggle(port, pin, seconds):
    base = GPIO_BASES[port]
    end = time.time() + seconds
    state = 1
    while time.time() < end:
        sr_addr = base + (GPIO_BSRR if state else GPIO_BRR)
        run_openocd([f"mww {sr_addr:#x} {1 << pin:#x}"])
        state ^= 1
        time.sleep(0.5)


def resume_mcu():
    args = list(OPENOCD_BASE) + ["-c", "init", "-c", "resume", "-c", "shutdown"]
    subprocess.run(args, capture_output=True, text=True, timeout=10)


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 6.0
    print(f"UART wiggle — {seconds:.1f}s @ 1 Hz per pin.")
    print("Hook a multimeter (DC volts, auto-range) to the LCD RX pad.")
    print("Each wiggle should swing 0 ↔ 3.3 V at 1 Hz on the connected pin.")
    print("Type 'yes' if you see it, anything else for no.\n")

    matches = []
    for label, port, pin, note in CANDIDATES:
        print(f"--- {label}  ({note}) ---")
        input("Press Enter to start the wiggle...")
        config_out_pp(port, pin)
        wiggle(port, pin, seconds)
        restore_input(port, pin)
        ans = input(f"Did the meter on LCD-RX track {label}? [yes/no] > ").strip().lower()
        if ans.startswith("y"):
            matches.append((label, note))
            print(f"  → recorded {label}")

    resume_mcu()
    print("\n=== Results ===")
    if not matches:
        print("  None of the candidates wiggle the LCD RX pad.")
        print("  LCD might be on another UART TX pin not in this list, or")
        print("  the meter probe wasn't on the right pad.")
    else:
        for label, note in matches:
            print(f"  {label}  ({note})")


if __name__ == "__main__":
    main()
