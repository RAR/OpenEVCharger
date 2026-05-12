#!/usr/bin/env python3
"""dgus_listen.py — Passively listen on a USB-UART for spontaneous DGUS
traffic. If the LCD has touch-panel auto-upload enabled in its CFG
(common factory default), every screen touch generates a
`5A A5 06 83 <vp_hi> <vp_lo> 01 <key_hi> <key_lo>` frame.

This is purely a wire-validation test — useful when full sweeps return
silence and you want to know whether the RX path works at all.

Usage:
  tools/dgus_listen.py                 # 5s @ 115200 on /dev/ttyACM1
  tools/dgus_listen.py --baud 9600
  tools/dgus_listen.py --port /dev/ttyUSB0 --seconds 10
"""
import argparse
import sys
import time

import serial

p = argparse.ArgumentParser()
p.add_argument('--port', default='/dev/ttyACM0')
p.add_argument('--baud', type=int, default=115200)
p.add_argument('--seconds', type=float, default=5.0)
args = p.parse_args()

print(f'Listening on {args.port} @ {args.baud} for {args.seconds:.1f}s...')
print('Touch the LCD screen — if touch auto-upload is enabled, you\'ll see frames.\n')

try:
    s = serial.Serial(args.port, args.baud, timeout=0.1)
except Exception as e:
    print(f'open failed: {e}')
    sys.exit(1)

end = time.time() + args.seconds
total = bytearray()
while time.time() < end:
    chunk = s.read(256)
    if chunk:
        total += chunk
        print(f't={args.seconds - (end - time.time()):.2f}s  +{len(chunk)}B: {chunk.hex()}')

print(f'\n=== {len(total)} bytes total ===')
if total:
    print(f'hex: {total.hex()}')
    # Look for DGUS frame headers
    n = total.count(b'\x5A\xA5')
    print(f'DGUS frame headers (5A A5) found: {n}')
else:
    print('Silent. Either:')
    print('  - LCD TX line not reaching USB-UART RX (wiring)')
    print('  - LCD not powered (backlight check?)')
    print('  - LCD baud != probe baud (try another --baud)')
    print('  - touch auto-upload disabled in CFG (try sweep instead)')
