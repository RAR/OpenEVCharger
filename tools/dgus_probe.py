#!/usr/bin/env python3
"""dgus_probe.py — Talk to a DWIN T5/T5L DGUS-II LCD directly via a
USB-UART adapter. Sweeps baud rates and CRC modes to identify how the
LCD is configured (per its SD-card CFG byte 0x05 bit 7).

Strategy
--------
1. For each candidate baud, send the **Version Read** frame at VP=0x000F
   (cmd 0x83). The LCD's response is deterministic — `5A A5 06 83 00 0F
   01 <gui_ver> <os_ver>` — so a clean RX = identified config.
2. Try both no-CRC and CRC variants per baud (the LCD silently drops
   mismatching frames; only the correct (baud, crc-mode) pair replies).
3. If the LCD is wedged before responding, send the **System_Reset**
   frame (write 55AA 5AA5 to VP=0x0004) which hard-resets the LCD CPU
   regardless of CRC mode (the first 4 data bytes trigger reset before
   any CRC check matters).
4. As a final fallback, send a visible **backlight blink** sequence at
   every baud — operator-eye confirmation.

Wiring
------
  USB-UART TX  → LCD RX2 (pin 5 on 10-pin FFC, or pin 4 on 8-pin)
  USB-UART RX  ← LCD TX2 (pin 4 on 10-pin FFC, or pin 5 on 8-pin)
  USB-UART GND ↔ LCD GND
  5V 1A bench supply → LCD VCC (USB-UART can't source 220 mA)

Usage
-----
  tools/dgus_probe.py                       # full sweep (~12 bauds, ~20 s)
  tools/dgus_probe.py --port /dev/ttyACM1   # custom port
  tools/dgus_probe.py --reset               # only send universal reset
  tools/dgus_probe.py --blink 115200        # only visible blink at one baud
  tools/dgus_probe.py --probe 115200        # version-read at one baud
"""
from __future__ import annotations

import argparse
import sys
import time

import serial

# ---------- DGUS-II frame helpers ----------

def crc16_modbus(data: bytes) -> int:
    c = 0xFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c


def frame(payload: bytes, with_crc: bool = False) -> bytes:
    """Build a DGUS-II frame: 5A A5 LEN payload [crc_lo crc_hi]."""
    if with_crc:
        c = crc16_modbus(payload)
        return bytes([0x5A, 0xA5, len(payload) + 2]) + payload + bytes([c & 0xFF, (c >> 8) & 0xFF])
    return bytes([0x5A, 0xA5, len(payload)]) + payload


# Pre-canned frames
VERSION_READ      = bytes([0x83, 0x00, 0x0F, 0x01])
SYSTEM_RESET      = bytes([0x82, 0x00, 0x04, 0x55, 0xAA, 0x5A, 0xA5])
BACKLIGHT_OFF     = bytes([0x82, 0x00, 0x82, 0x00])
BACKLIGHT_ON      = bytes([0x82, 0x00, 0x82, 0x64])
PAGE_0            = bytes([0x82, 0x00, 0x84, 0x5A, 0x01, 0x00, 0x00])

BAUDS = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600]


# ---------- probe routines ----------

def open_port(port: str, baud: int):
    return serial.Serial(port=port, baudrate=baud, bytesize=8, parity='N',
                         stopbits=1, timeout=0.4, write_timeout=2.0)


def drain(s: serial.Serial, t: float = 0.4) -> bytes:
    """Read everything available within t seconds."""
    end = time.time() + t
    buf = bytearray()
    while time.time() < end:
        n = s.in_waiting
        if n:
            buf += s.read(n)
            end = time.time() + 0.10  # extend on activity
        else:
            time.sleep(0.01)
    return bytes(buf)


def looks_like_dgus_reply(buf: bytes) -> bool:
    """Heuristic: starts with 5A A5, then length byte plausible, then 83 (read reply)."""
    if len(buf) < 4:
        return False
    if buf[0] != 0x5A or buf[1] != 0xA5:
        return False
    return buf[3] in (0x83, 0x82)


def probe_baud(port: str, baud: int, verbose: bool = True) -> dict | None:
    """Try version read at this baud, both with and without CRC. Returns
    config dict on success, None on miss."""
    for crc_mode, label in [(False, 'no-CRC'), (True, 'CRC')]:
        try:
            s = open_port(port, baud)
        except (OSError, serial.SerialException) as e:
            print(f'  baud={baud:6d} {label:6s}  port error: {e}')
            return None
        try:
            s.reset_input_buffer()
            s.write(frame(VERSION_READ, with_crc=crc_mode))
            s.flush()
            resp = drain(s, 0.4)
            if resp:
                if looks_like_dgus_reply(resp):
                    print(f'  baud={baud:6d} {label:6s}  ✅ REPLY: {resp.hex()}')
                    return {'baud': baud, 'crc': crc_mode, 'reply': resp}
                else:
                    print(f'  baud={baud:6d} {label:6s}  garbage RX: {resp.hex()}')
            elif verbose:
                print(f'  baud={baud:6d} {label:6s}  silent')
        finally:
            s.close()
    return None


def universal_reset(port: str, baud: int):
    """Send System_Reset both with and without CRC. Works in either LCD mode."""
    print(f'\nSystem_Reset at baud={baud} (both CRC modes)...')
    for crc_mode, label in [(False, 'no-CRC'), (True, 'CRC')]:
        try:
            s = open_port(port, baud)
            s.write(frame(SYSTEM_RESET, with_crc=crc_mode))
            s.flush()
            time.sleep(0.05)
            s.close()
            print(f'  sent {label:6s} reset')
        except Exception as e:
            print(f'  {label} reset error: {e}')
    print('  → LCD CPU should be rebooting if alive; backlight may flash.')


def visible_blink(port: str, baud: int):
    """Backlight off → 600 ms → on, both CRC modes."""
    print(f'\nBacklight blink at baud={baud}...')
    for crc_mode, label in [(False, 'no-CRC'), (True, 'CRC')]:
        try:
            s = open_port(port, baud)
            print(f'  {label:6s} off')
            s.write(frame(BACKLIGHT_OFF, with_crc=crc_mode))
            s.flush()
            time.sleep(0.7)
            print(f'  {label:6s} on')
            s.write(frame(BACKLIGHT_ON, with_crc=crc_mode))
            s.flush()
            time.sleep(0.7)
            s.close()
        except Exception as e:
            print(f'  blink {label} error: {e}')


# ---------- main ----------

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--port', default='/dev/ttyACM1')
    p.add_argument('--reset', action='store_true', help='Send universal System_Reset and exit')
    p.add_argument('--blink', type=int, metavar='BAUD', help='Visible backlight blink at one baud')
    p.add_argument('--probe', type=int, metavar='BAUD', help='Version-read at one baud (returns reply if alive)')
    p.add_argument('--bauds', help='Comma-separated baud list to sweep (default: standard DGUS rates)')
    args = p.parse_args()

    bauds = BAUDS if not args.bauds else [int(b) for b in args.bauds.split(',')]

    if args.reset:
        for b in bauds:
            universal_reset(args.port, b)
        return

    if args.blink is not None:
        visible_blink(args.port, args.blink)
        return

    if args.probe is not None:
        hit = probe_baud(args.port, args.probe)
        if hit:
            print(f'\n→ LCD config: baud={hit["baud"]}, CRC={"on" if hit["crc"] else "off"}')
        else:
            print('\n→ no reply at this baud')
        return

    # Default: full sweep
    print(f'DGUS probe sweep on {args.port} — version-read at each baud + CRC mode.')
    print('(LCD must be powered at 5V and TX/RX wired to USB-UART)\n')
    hits = []
    for b in bauds:
        hit = probe_baud(args.port, b)
        if hit:
            hits.append(hit)
    print()
    if hits:
        print('=== ALIVE ===')
        for h in hits:
            print(f'  baud={h["baud"]}  CRC={"on" if h["crc"] else "off"}  reply={h["reply"].hex()}')
    else:
        print('=== SILENT at all baud/CRC combinations ===')
        print('Next steps:')
        print('  1. Verify LCD VCC = 5V, GND common, TX/RX not swapped')
        print('  2. Try `--reset` to force LCD CPU reboot, then re-run sweep')
        print('  3. Try `--blink 115200` and look for any visible backlight reaction')


if __name__ == '__main__':
    main()
