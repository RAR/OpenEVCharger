#!/usr/bin/env python3
"""led_walk.py — Walk every unattributed pin on ports A/B/C, drive each
HIGH one at a time, and ask the operator whether any LED on the ring
turned on. The pin that triggers a color is the LED drive line.

Topology (confirmed 2026-05-11 pm): LED ring runs off 12V, no driver
chip on the ring board (just LEDs + current-limit resistors). The
main PCB has buffer transistors (NPN base or N-MOSFET gate) between
each MCU pin and the LED cathodes. MCU HIGH → buffer ON → LED
cathode to GND → LED lights. So we drive HIGH, not LOW.

(Operator grounding the Y/R/G pads at the connector lights LEDs
because grounding bypasses the buffer; that's why the passive-IDR
probe couldn't see anything from the MCU side.)

Skips pins that are already attributed (CP PWM, contactor coils,
relay sense, etc.) or risky to toggle (SWD pins). The candidate set
is documented inline.

For each pin:
  1. Halt MCU.
  2. Configure pin as OUT_PP @ 2 MHz.
  3. Drive HIGH via BSRR.
  4. Prompt: "Did any LED color appear? [color / Enter for no / 'q' to quit]"
  5. Restore to input-floating, continue.
At end: resume firmware, print summary of color → pin.

Usage:
    tools/led_walk.py
"""
from __future__ import annotations

import subprocess
import sys

GPIO_BASES = {"A": 0x40010800, "B": 0x40010C00, "C": 0x40011000}
GPIO_CRL  = 0x00
GPIO_CRH  = 0x04
GPIO_BSRR = 0x10
GPIO_BRR  = 0x14

# Already-attributed pins to skip. Driving these may cause noise events
# (relay clicks, buzzer, etc.) but isn't physically dangerous on the
# powered-off bench; we skip just to keep the walk focused.
SKIP_PINS = {
    # Contactor — clicks but safe with mains off
    "PA0", "PA1",
    # CP PWM, UART TX, touch buttons, SWD
    "PA8", "PA9", "PA11", "PA12", "PA13", "PA14",
    # ADC inputs (currently AIN, driving is invalid)
    "PA4", "PA5", "PA6", "PA7", "PB0", "PB1", "PB2", "PC0", "PC1", "PC4", "PC5",
    # Mains current sense (digital input)
    "PC3", "PC6", "PC7",
    # STOP / heartbeat / BL0939 SPI
    "PC11", "PC13", "PB12", "PB13", "PB14", "PB15",
    # NOTE: PA15, PB8, PB9, PC8, PC10 were tested with LOW drive (wrong
    # polarity) earlier — they are still strong candidates with HIGH drive.
    # Not in skip list.
}

OPENOCD_BASE = [
    "openocd",
    "-f", "interface/stlink.cfg",
    "-c", "transport select hla_swd",
    "-f", "target/stm32f4x.cfg",
]


def run_openocd(cmds, halt=True):
    args = list(OPENOCD_BASE) + ["-c", "init"]
    if halt:
        args += ["-c", "halt"]
    for c in cmds:
        args += ["-c", c]
    args += ["-c", "shutdown"]
    return subprocess.run(args, capture_output=True, text=True, timeout=20)


def drive_pin(port, pin, level):
    """Configure pin OUT_PP @ 2 MHz, drive HIGH (level=1) or LOW (level=0)."""
    base = GPIO_BASES[port]
    cr_addr = base + (GPIO_CRL if pin < 8 else GPIO_CRH)
    nibble_pos = (pin if pin < 8 else pin - 8) * 4

    # Read CR, set our nibble to 0x2 (OUT_PP @ 2MHz, general purpose).
    text = run_openocd([f"mdw {cr_addr:#x} 1"]).stderr
    needle = f"{cr_addr:#010x}:"
    cur = None
    for line in text.splitlines():
        s = line.strip()
        if s.startswith(needle):
            cur = int(s[len(needle):].split()[0], 16)
            break
    if cur is None:
        print(f"  ERROR: could not read CR at {cr_addr:#x}")
        return False
    new_cr = (cur & ~(0xF << nibble_pos)) | (0x2 << nibble_pos)
    if level == 0:
        sr_addr = base + GPIO_BRR
    else:
        sr_addr = base + GPIO_BSRR
    run_openocd([
        f"mww {cr_addr:#x} {new_cr:#x}",
        f"mww {sr_addr:#x} {1 << pin:#x}",
    ])
    return True


def restore_pin_input(port, pin):
    """Set pin back to INPUT FLOATING (CR nibble = 0x4) so it doesn't fight."""
    base = GPIO_BASES[port]
    cr_addr = base + (GPIO_CRL if pin < 8 else GPIO_CRH)
    nibble_pos = (pin if pin < 8 else pin - 8) * 4
    text = run_openocd([f"mdw {cr_addr:#x} 1"]).stderr
    needle = f"{cr_addr:#010x}:"
    cur = None
    for line in text.splitlines():
        s = line.strip()
        if s.startswith(needle):
            cur = int(s[len(needle):].split()[0], 16)
            break
    if cur is None:
        return
    new_cr = (cur & ~(0xF << nibble_pos)) | (0x4 << nibble_pos)
    run_openocd([f"mww {cr_addr:#x} {new_cr:#x}"])


def resume_mcu():
    args = list(OPENOCD_BASE) + ["-c", "init", "-c", "resume", "-c", "shutdown"]
    subprocess.run(args, capture_output=True, text=True, timeout=15)


def main():
    print("LED walk — driving each unattributed pin HIGH in turn.")
    print("(NPN/MOSFET buffer topology: HIGH = LED on, LOW = LED off.)")
    print("Press Enter if no LED color appears. Type the color name if it does.")
    print("Type 'q' to quit. Type 's' to skip and continue.\n")

    results = []
    for port in ("A", "B", "C"):
        for pin in range(16):
            label = f"P{port}{pin}"
            if label in SKIP_PINS:
                continue
            print(f"--- {label} ---")
            if not drive_pin(port, pin, 1):
                continue
            ans = input(f"{label} HIGH. Color? [Enter=none/q=quit/s=skip] > ").strip()
            restore_pin_input(port, pin)
            if ans == "q":
                break
            if ans and ans != "s":
                results.append((label, ans))
                print(f"  → recorded {label} = {ans}")
        else:
            continue
        break

    resume_mcu()

    print("\n=== Results ===")
    if not results:
        print("  No LED-driving pin found in candidate set.")
    else:
        for label, color in results:
            print(f"  {label} = {color}")


if __name__ == "__main__":
    main()
