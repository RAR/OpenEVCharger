#!/usr/bin/env python3
"""pc14_red_test.py — Verify whether PC14 drives the red LED.

PC14 is the LSE oscillator input on STM32F1 / N32G45x. To use it as a
general-purpose GPIO, three things must be true first:
  1. RCC_APB1ENR.PWREN = 1     (PWR peripheral clocked)
  2. PWR_CR.DBP = 1             (backup-domain write-protect off)
  3. RCC_BDCR.LSEON = 0         (LSE disabled — frees PC14/PC15)

Without those, CRL/CRH writes to the PC14 nibble are silently
ignored. The led_walk run that "tested" PC14 most likely did
nothing — explaining why the operator couldn't tell whether the
red LED changed state.

This script does the unlock sequence, then toggles PC14 HIGH / LOW
twice with the operator pressing Enter between phases. If the red
LED tracks PC14's state, it's the driver. If red stays on the whole
time, red is hardwired (probably wired straight to 12V via a
current-limit resistor as a power-on indicator).

Register addresses (Nations N32G45x = STM32F1 compat):
  RCC_APB1ENR  = 0x4002101C
  RCC_BDCR     = 0x40021020
  PWR_CR       = 0x40007000
  GPIOC_CRH    = 0x40011004
  GPIOC_BSRR   = 0x40011010
  GPIOC_BRR    = 0x40011014

Usage:
    tools/pc14_red_test.py
"""
from __future__ import annotations

import subprocess

OPENOCD_BASE = [
    "openocd",
    "-f", "interface/stlink.cfg",
    "-c", "transport select hla_swd",
    "-f", "target/stm32f4x.cfg",
]

RCC_APB1ENR = 0x4002101C
RCC_BDCR    = 0x40021020
PWR_CR      = 0x40007000
GPIOC_CRH   = 0x40011004
GPIOC_BSRR  = 0x40011010
GPIOC_BRR   = 0x40011014


def run(cmds, halt=False, resume=False):
    args = list(OPENOCD_BASE) + ["-c", "init"]
    if halt:
        args += ["-c", "halt"]
    for c in cmds:
        args += ["-c", c]
    if resume:
        args += ["-c", "resume"]
    args += ["-c", "shutdown"]
    return subprocess.run(args, capture_output=True, text=True, timeout=20).stderr


def parse_word(text, addr):
    needle = f"{addr:#010x}:"
    for line in text.splitlines():
        s = line.strip()
        if s.startswith(needle):
            return int(s[len(needle):].split()[0], 16)
    return None


def unlock_pc14():
    """Enable PWR clock, disable backup write-protect, disable LSE."""
    # Read current values so we can preserve other bits.
    text = run([
        f"mdw {RCC_APB1ENR:#x} 1",
        f"mdw {PWR_CR:#x} 1",
        f"mdw {RCC_BDCR:#x} 1",
        f"mdw {GPIOC_CRH:#x} 1",
    ], halt=True)
    apb1 = parse_word(text, RCC_APB1ENR) or 0
    pwr  = parse_word(text, PWR_CR) or 0
    bdcr = parse_word(text, RCC_BDCR) or 0
    crh  = parse_word(text, GPIOC_CRH) or 0

    new_apb1 = apb1 | (1 << 28)       # PWREN
    new_pwr  = pwr | (1 << 8)         # DBP
    new_bdcr = bdcr & ~(1 << 0)       # LSEON = 0
    # PC14 = bits 24..27 of CRH. Set to OUT_PP @ 2MHz = 0x2.
    new_crh  = (crh & ~(0xF << 24)) | (0x2 << 24)

    print(f"RCC_APB1ENR: {apb1:#010x} → {new_apb1:#010x}  (PWREN)")
    print(f"PWR_CR:      {pwr:#010x} → {new_pwr:#010x}  (DBP)")
    print(f"RCC_BDCR:    {bdcr:#010x} → {new_bdcr:#010x}  (LSEON=0)")
    print(f"GPIOC_CRH:   {crh:#010x} → {new_crh:#010x}  (PC14=OUT_PP)")
    run([
        f"mww {RCC_APB1ENR:#x} {new_apb1:#x}",
        f"mww {PWR_CR:#x} {new_pwr:#x}",
        f"mww {RCC_BDCR:#x} {new_bdcr:#x}",
        f"mww {GPIOC_CRH:#x} {new_crh:#x}",
    ])


def drive_high():
    run([f"mww {GPIOC_BSRR:#x} {1 << 14:#x}"])


def drive_low():
    run([f"mww {GPIOC_BRR:#x} {1 << 14:#x}"])


def resume_mcu():
    args = list(OPENOCD_BASE) + ["-c", "init", "-c", "resume", "-c", "shutdown"]
    subprocess.run(args, capture_output=True, text=True, timeout=10)


def main():
    print("PC14 red-LED test — unlocking backup-domain to free PC14 as GPIO.")
    unlock_pc14()
    print()
    print("Phase 1: driving PC14 HIGH (should turn red LED ON if PC14 drives it).")
    drive_high()
    input("Look at the red LED. Press Enter when ready...")
    print()
    print("Phase 2: driving PC14 LOW (should turn red LED OFF if PC14 drives it).")
    drive_low()
    input("Look at the red LED. Press Enter when ready...")
    print()
    print("Phase 3: driving PC14 HIGH again.")
    drive_high()
    input("Look at the red LED. Press Enter when ready...")
    print()
    resume_mcu()
    print("MCU resumed.")
    print()
    print("Report which of these you observed:")
    print("  (a) Red tracked PC14: ON during HIGH phases, OFF during LOW phase")
    print("  (b) Red was always ON, regardless of phase")
    print("  (c) Red was always OFF, regardless of phase")
    print("  (d) Something else")


if __name__ == "__main__":
    main()
