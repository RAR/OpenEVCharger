#!/usr/bin/env python3
"""baud_sweep.py — Poke USART2_BRR through common Nextion baud
candidates (across both PCLK1=36 MHz and PCLK1=60 MHz assumptions)
and retry the dims=10/dims=100 backlight test at each rate.

USART2_BRR = 0x40004408 on N32G45x.

Bench cmd mailbox g_bench_cmd at 0x2000000c:
  5 = nextion_send_cmd("dims=10")
  6 = nextion_send_cmd("dims=100")

Procedure per candidate:
  1. Halt MCU.
  2. mww USART2_BRR <candidate>
  3. Resume.
  4. Poke g_bench_cmd = 5 (dim).
  5. Sleep 1.8s — operator watches LCD.
  6. Poke g_bench_cmd = 6 (restore).
  7. Sleep 1.8s.

Operator stops the script (Ctrl-C) when they see the backlight dim
and reports the most recent BRR value printed.

Usage:
    tools/baud_sweep.py
"""
from __future__ import annotations

import subprocess
import time

USART2_BRR = 0x40004408
BENCH_CMD  = 0x2000000c

CANDIDATES = [
    # (label, BRR_value)
    ("9600 @ 36MHz   (SPL default)",      0x0EA6),
    ("9600 @ 60MHz",                       0x186A),
    ("9600 @ 30MHz",                       0x0C35),
    ("9600 @ 72MHz",                       0x1D4C),
    ("38400 @ 36MHz",                      0x03AA),
    ("38400 @ 60MHz",                      0x061B),
    ("57600 @ 36MHz",                      0x0271),
    ("57600 @ 60MHz",                      0x0411),
    ("115200 @ 36MHz",                     0x0138),
    ("115200 @ 60MHz",                     0x020A),
    ("115200 @ 72MHz",                     0x0271),
]

OPENOCD_BASE = [
    "openocd",
    "-f", "interface/stlink.cfg",
    "-c", "transport select hla_swd",
    "-f", "target/stm32f4x.cfg",
]


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
    print("Baud sweep — watch the LCD. When you see the backlight dim,")
    print("note which BRR value was just printed and Ctrl-C.\n")
    for label, brr in CANDIDATES:
        print(f"=== BRR={brr:#06x}  ({label}) ===", flush=True)
        # Halt, override BRR, resume.
        run([f"mww {USART2_BRR:#x} {brr:#x}"], halt=True, resume=True)
        # Trigger dims=10.
        run([f"mww {BENCH_CMD:#x} 5"], halt=True, resume=True)
        time.sleep(1.8)
        # Trigger dims=100.
        run([f"mww {BENCH_CMD:#x} 6"], halt=True, resume=True)
        time.sleep(1.8)
    print("\nSweep complete. If you saw a dim, the BRR value listed just")
    print("before is the right baud rate.")


if __name__ == "__main__":
    main()
