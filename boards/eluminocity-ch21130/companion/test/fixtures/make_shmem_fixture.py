#!/usr/bin/env python3
"""Generate a deterministic 256 KiB shmem fixture with known sentinel values
at the offsets the bridge reads. Tests assert against these sentinels —
this verifies accessor *logic* against the layout in
boards/eluminocity-ch21130/docs/06-shmem-RE-from-binaries.md, independent
of bench-physics RE uncertainty.

    python3 make_shmem_fixture.py
"""
import pathlib

SIZE = 0x40000
buf = bytearray(SIZE)

# Metering — little-endian. Scaling is empirical from the bench session of
# 2026-05-15: VRMS u16 raw/100, IRMS u16 raw/10, POWER u32 raw/1 (the
# producer divides by 1000 internally before storing — see comment in
# src/charger_state.c).
# VRMS_MEAS @ 0x0000 = 23000 -> 230.0 V
buf[0x0000] = 0xD8
buf[0x0001] = 0x59
# IRMS_MEAS @ 0x0004 = 160   -> 16.0 A
buf[0x0004] = 0xA0
buf[0x0005] = 0x00
# POWER_MEAS @ 0x000c = 3680 -> 3680.0 W (V*I, sensible for 230 V × 16 A)
buf[0x000c] = 0x60
buf[0x000d] = 0x0E
buf[0x000e] = 0x00
buf[0x000f] = 0x00

# State cluster
buf[0x0a00] = 0x02   # USER_STATE = charging
buf[0x0a01] = 0x02   # RED_LED    = flash
buf[0x0a07] = 0x03   # PRI_STATE  = 3 (Pri_Comm digested state)
buf[0x0a08] = 0x02   # PILOT_STATE = C (charging)
buf[0x0a0b] = 0x00   # STM32_FAULT = 0 (link healthy)
buf[0x0a10] = 0x32   # PILOT_DUTY  = 50 %
buf[0x0a24] = 0x1E   # RATED_AMPS  = 30 A

# Alarm bitmap @ 0x0a74, LE u32: bit 3 set (EMGSTOP)
buf[0x0a74] = 0x08
buf[0x0a75] = 0x00
buf[0x0a76] = 0x00
buf[0x0a77] = 0x00

out = pathlib.Path(__file__).parent / "shmem_snapshot.bin"
with open(out, "wb") as f:
    f.write(buf)
print(f"wrote shmem_snapshot.bin ({SIZE} bytes)")
