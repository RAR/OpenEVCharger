#!/usr/bin/env python3
"""pc11_red_test.py — Test whether PC11 (safety heartbeat) doubles as
the red LED driver.

Stock firmware pulsed red. Our M4 firmware pulses PC11 at ~5 Hz as a
safety heartbeat. If PC11 → buffer → red LED, then with M4 running
red should be flickering at 5 Hz. The operator reports red is "always
on" — which is consistent with either:
  (a) PC11 is the red driver but the buffer FET is shorted (LED stays
      on regardless of gate)
  (b) PC11 doesn't drive red

This test distinguishes the two:
  Phase 1: halt MCU. PC11 stops pulsing — frozen at whichever level
           it was last at.
  Phase 2: force PC11 LOW for 5 seconds. If buffer is working, red
           goes OFF. If buffer is shorted, red stays ON.
  Phase 3: force PC11 HIGH for 5 seconds. (Same observation.)
  Phase 4: resume firmware — heartbeat resumes.

Usage:
    tools/pc11_red_test.py
"""
from __future__ import annotations

import subprocess
import time

OPENOCD_BASE = [
    "openocd",
    "-f", "interface/stlink.cfg",
    "-c", "transport select hla_swd",
    "-f", "target/stm32f4x.cfg",
]

GPIOC_BSRR = 0x40011010
GPIOC_BRR  = 0x40011014


def run(cmds, halt=False, resume=False):
    args = list(OPENOCD_BASE) + ["-c", "init"]
    if halt:
        args += ["-c", "halt"]
    for c in cmds:
        args += ["-c", c]
    if resume:
        args += ["-c", "resume"]
    args += ["-c", "shutdown"]
    subprocess.run(args, capture_output=True, text=True, timeout=15)


def main():
    print("Halting MCU — PC11 heartbeat stops pulsing now.")
    run([], halt=True)
    input("Look at red LED. Did it stop pulsing or change brightness? "
          "[Enter when done observing]")

    print("\nDriving PC11 LOW (buffer gate off — LED should go dark if PC11"
          " drives red and buffer is healthy).")
    run([f"mww {GPIOC_BRR:#x} {1 << 11:#x}"])
    input("Look at red LED. Is it OFF or still ON? [Enter when done]")

    print("\nDriving PC11 HIGH (buffer gate on — LED should light if PC11"
          " drives red).")
    run([f"mww {GPIOC_BSRR:#x} {1 << 11:#x}"])
    input("Look at red LED. Is it ON or OFF? [Enter when done]")

    print("\nResuming MCU.")
    args = list(OPENOCD_BASE) + ["-c", "init", "-c", "resume", "-c", "shutdown"]
    subprocess.run(args, capture_output=True, text=True, timeout=10)
    print("Done. Reasoning guide:")
    print()
    print("  red OFF on LOW, ON on HIGH    → PC11 drives red, buffer healthy.")
    print("                                    The 'always on' was the heartbeat")
    print("                                    pulsing too fast to see as flicker.")
    print("  red ON during LOW and HIGH    → buffer FET shorted; PC11 might or")
    print("                                    might not be the driver — can't tell.")
    print("                                    Damage from earlier session likely.")
    print("  red OFF during LOW and HIGH   → PC11 doesn't drive red.")


if __name__ == "__main__":
    main()
