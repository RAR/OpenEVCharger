#!/usr/bin/env python3
"""dgus_brr_sweep.py — Sweep USART2 BRR values via firmware bench cmd 18
(which sends DGUS dim=0 then dim=100 back-to-back from a tight loop, so
no openocd inter-byte gaps).

Mailbox layout (after the g_bench_arg addition):
  g_bench_arg = 0x2000000C  — BRR value
  g_bench_cmd = 0x20000010  — set to 18 to trigger
"""
from __future__ import annotations

import subprocess
import time

G_BENCH_ARG = 0x2000000c
G_BENCH_CMD = 0x20000010

# (label, BRR value)
CANDIDATES = [
    # 115200 baud at various assumed PCLK1
    ("115200 @ 30MHz",  0x0104),
    ("115200 @ 36MHz",  0x0138),
    ("115200 @ 45MHz",  0x0186),
    ("115200 @ 54MHz",  0x01D5),
    ("115200 @ 60MHz",  0x020A),
    ("115200 @ 64MHz",  0x022B),
    ("115200 @ 72MHz",  0x0271),
    ("115200 @ 80MHz",  0x02B6),
    # 9600 baud
    ("9600   @ 36MHz",  0x0EA6),
    ("9600   @ 60MHz",  0x186A),
    # 38400 baud
    ("38400  @ 36MHz",  0x03AA),
    ("38400  @ 60MHz",  0x061B),
    # 57600 baud
    ("57600  @ 36MHz",  0x0271),
    ("57600  @ 60MHz",  0x0411),
    # 230400 baud
    ("230400 @ 36MHz",  0x009C),
    ("230400 @ 60MHz",  0x0105),
    # 19200
    ("19200  @ 36MHz",  0x0753),
    # 460800
    ("460800 @ 36MHz",  0x004E),
    ("460800 @ 60MHz",  0x0083),
    # 921600
    ("921600 @ 60MHz",  0x0041),
]

OPENOCD = [
    "openocd",
    "-f", "interface/stlink.cfg",
    "-c", "transport select hla_swd",
    "-f", "target/stm32f4x.cfg",
]


def trigger(brr):
    args = list(OPENOCD) + [
        "-c", "init",
        "-c", "halt",
        "-c", f"mww {G_BENCH_ARG:#x} {brr:#x}",
        "-c", f"mww {G_BENCH_CMD:#x} 18",
        "-c", "resume",
        "-c", "shutdown",
    ]
    subprocess.run(args, capture_output=True, text=True, timeout=15)


def main():
    print("BRR sweep via firmware tight-loop. Watch the LCD for backlight blink.")
    print("Each candidate: dim=0 → 1.5s wait → dim=100. ~4 sec per candidate.\n")
    for label, brr in CANDIDATES:
        print(f"=== BRR={brr:#06x}  ({label}) ===", flush=True)
        trigger(brr)
        # Firmware runs for ~3.5s (dim=0 + 1500ms + dim=100). Add slack.
        time.sleep(4)


if __name__ == "__main__":
    main()
