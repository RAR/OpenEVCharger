#!/usr/bin/env python3
"""led_passive_probe.py — Find LED-drive pins by grounding the LED pad
and reading which MCU pin's IDR bit drops to 0.

Process:
  1. Halt MCU.
  2. Reconfigure every pin on GPIO ports A-E to INPUT_FLOATING (CRL/CRH
     nibbles = 0x4) so nothing fights an external ground.
  3. Snapshot baseline IDR for all 5 ports — these are the "idle" reads
     (most pins float HIGH or LOW depending on board-level pull paths).
  4. For each color: prompt operator to ground the LED pad. Read IDR
     again. Diff against baseline. The pin whose bit went LOW (or HIGH)
     is the one wired to that pad.
  5. Resume MCU (firmware re-inits GPIO modes from scratch).

Usage:
    tools/led_passive_probe.py

Safe to run with M4 firmware loaded — the halt + float-all step releases
every output, so user grounding is harmless. Don't ground anything until
the script prints "GPIO floated — safe to ground pads".
"""
from __future__ import annotations

import subprocess
import sys

GPIO_BASES = {
    "A": 0x40010800,
    "B": 0x40010C00,
    "C": 0x40011000,
    "D": 0x40011400,
    "E": 0x40011800,
}
GPIO_CRL  = 0x00
GPIO_CRH  = 0x04
GPIO_IDR  = 0x08

# Nibble = 0x4 = CNF=01 (floating input) + MODE=00 (input).
# Full CRL or CRH for all-floating-input = 0x44444444.
CR_ALL_FLOAT = 0x44444444

OPENOCD_BASE = [
    "openocd",
    "-f", "interface/stlink.cfg",
    "-c", "transport select hla_swd",
    "-f", "target/stm32f4x.cfg",
]


def run_openocd(extra_cmds, halt=False, resume=False):
    args = list(OPENOCD_BASE) + ["-c", "init"]
    if halt:
        args += ["-c", "halt"]
    for c in extra_cmds:
        args += ["-c", c]
    if resume:
        args += ["-c", "resume"]
    args += ["-c", "shutdown"]
    proc = subprocess.run(args, capture_output=True, text=True, timeout=30)
    return proc.stderr + proc.stdout


def parse_word_at(text, addr):
    needle = f"{addr:#010x}:"
    for line in text.splitlines():
        s = line.strip()
        if s.startswith(needle):
            tok = s[len(needle):].split()[0]
            try:
                return int(tok, 16)
            except ValueError:
                return None
    return None


def float_all_and_snapshot():
    """Halt, float every pin on A-E, then read IDR for each port. One
    openocd invocation for the writes, one for the reads, both in the
    same halt-then-shutdown session is awkward — so we combine."""
    cmds = []
    for port, base in GPIO_BASES.items():
        cmds.append(f"mww {base + GPIO_CRL:#x} {CR_ALL_FLOAT:#x}")
        cmds.append(f"mww {base + GPIO_CRH:#x} {CR_ALL_FLOAT:#x}")
    for port, base in GPIO_BASES.items():
        cmds.append(f"mdw {base + GPIO_IDR:#x} 1")
    text = run_openocd(cmds, halt=True, resume=False)
    idrs = {}
    for port, base in GPIO_BASES.items():
        idrs[port] = parse_word_at(text, base + GPIO_IDR)
        if idrs[port] is None:
            print(f"FAILED to read IDR for port {port}", file=sys.stderr)
            print(text, file=sys.stderr)
            sys.exit(1)
    return idrs


def snapshot_idr():
    """Read IDR for ports A-E. MCU stays halted."""
    cmds = []
    for port, base in GPIO_BASES.items():
        cmds.append(f"mdw {base + GPIO_IDR:#x} 1")
    text = run_openocd(cmds, halt=False, resume=False)
    idrs = {}
    for port, base in GPIO_BASES.items():
        idrs[port] = parse_word_at(text, base + GPIO_IDR)
    return idrs


def diff_idr(before, after):
    """Return list of (pin_label, before_bit, after_bit) for bits that changed."""
    changed = []
    for port in GPIO_BASES:
        b = before[port]
        a = after[port]
        if b is None or a is None:
            continue
        delta = b ^ a
        for pin in range(16):
            if (delta >> pin) & 1:
                changed.append((
                    f"P{port}{pin}",
                    (b >> pin) & 1,
                    (a >> pin) & 1,
                ))
    return changed


def resume_mcu():
    args = list(OPENOCD_BASE) + ["-c", "init", "-c", "resume", "-c", "shutdown"]
    subprocess.run(args, capture_output=True, text=True, timeout=15)


def main():
    print("Halting MCU and floating all GPIO A-E pins...")
    baseline = float_all_and_snapshot()
    print("GPIO floated — safe to ground pads.\n")
    print("Baseline IDR:")
    for port, val in baseline.items():
        print(f"  GPIO{port}: {val:#010x}  (b{val & 0xFFFF:016b})")
    print()

    results = {}
    for color in ("YELLOW", "RED", "GREEN"):
        input(f"Ground the {color} LED pad now, then press Enter...")
        after = snapshot_idr()
        changes = diff_idr(baseline, after)
        if not changes:
            print(f"  {color}: NO change detected. Try again? (Ctrl-C to quit)")
            input("Press Enter to re-snapshot, or ground a different pad first... ")
            after = snapshot_idr()
            changes = diff_idr(baseline, after)
        for pin, b, a in changes:
            print(f"  {color}: {pin}  {b} → {a}")
        if len(changes) == 1:
            results[color] = changes[0][0]
        elif len(changes) > 1:
            # Filter for bits that went HIGH → LOW (grounding pulls a
            # high-floating pin low; we don't expect any pin to go
            # high since we're grounding).
            drops = [(p, b, a) for p, b, a in changes if b == 1 and a == 0]
            if len(drops) == 1:
                results[color] = drops[0][0]
                print(f"  → single drop: {drops[0][0]}")
            else:
                print(f"  → multiple changes, ambiguous: {[p for p, _, _ in changes]}")
                results[color] = "AMBIGUOUS: " + ",".join(p for p, _, _ in changes)
        else:
            results[color] = "NONE"
        input(f"Release {color}, then press Enter to continue...")

    resume_mcu()

    print("\n=== Results ===")
    for color, pin in results.items():
        print(f"  {color:<7} = {pin}")
    print("\nMCU resumed.")


if __name__ == "__main__":
    main()
