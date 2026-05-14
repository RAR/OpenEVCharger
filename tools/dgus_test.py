#!/usr/bin/env python3
"""dgus_test.py — Send a DGUS backlight frame to USART2 at various baud
rates, with raw bytes poked directly into USART2_DAT via SWD.

DGUS-II protocol:
  Frame = 5A A5 <len> 82 <vp_hi> <vp_lo> <data...>
  VP 0x0082 = backlight level (1 byte, 0..0x64)
  VP 0x0084 = page change (write 5A 01 <pp_hi> <pp_lo>)

Backlight off:  5A A5 04 82 00 82 00      (length=4 covers cmd..data)
Backlight 100:  5A A5 04 82 00 82 64
Page change N:  5A A5 07 82 00 84 5A 01 00 <N>

Per baud:
  1. Halt MCU.
  2. Poke USART2_BRR to candidate.
  3. Resume.
  4. mww each byte of "dim=0 frame" into USART2_DAT — openocd is so much
     slower than UART that we don't need to wait for TXDE.
  5. Sleep 1.5s.
  6. Same for "dim=100 frame".
  7. Sleep 1.5s.

Watch the LCD. If the backlight blinks during a particular baud window,
that's the rate.
"""
from __future__ import annotations

import subprocess
import time

USART2_BRR = 0x40004408
USART2_DAT = 0x40004404

BACKLIGHT_OFF = [0x5A, 0xA5, 0x04, 0x82, 0x00, 0x82, 0x00]
BACKLIGHT_ON  = [0x5A, 0xA5, 0x04, 0x82, 0x00, 0x82, 0x64]

CANDIDATES = [
    # (label, BRR)
    ("9600   @ 60MHz", 0x186A),
    ("115200 @ 60MHz", 0x020A),
    ("9600   @ 36MHz (SPL)", 0x0EA6),
    ("115200 @ 36MHz", 0x0138),
    ("38400  @ 60MHz", 0x061B),
    ("57600  @ 60MHz", 0x0411),
    ("230400 @ 60MHz", 0x0105),
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


def send_frame(bytes_):
    """Blast each byte into USART2_DAT. Single openocd session to
    amortize startup latency."""
    cmds = [f"mww {USART2_DAT:#x} {b:#x}" for b in bytes_]
    run(cmds)


def main():
    print("DGUS backlight test — sweeping bauds at USART2.")
    print("Watch LCD: backlight should go OFF for 1.5s, then ON for 1.5s.\n")
    for label, brr in CANDIDATES:
        print(f"=== BRR={brr:#06x}  ({label}) ===", flush=True)
        run([f"mww {USART2_BRR:#x} {brr:#x}"], halt=True, resume=True)
        send_frame(BACKLIGHT_OFF)
        time.sleep(1.5)
        send_frame(BACKLIGHT_ON)
        time.sleep(1.5)
    print("\nSweep done. If any rate caused the backlight to blink,")
    print("that's the right baud for the LCD.")


if __name__ == "__main__":
    main()
