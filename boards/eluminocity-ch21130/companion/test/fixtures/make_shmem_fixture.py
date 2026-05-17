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

# --- Stock chip→shmem triple (Pri_Comm input; not human V/I/P) ----------
# Existence only so test_shmem.c can round-trip the LE helpers against
# the OFF_STOCK_* offsets. Values are arbitrary but kept the same as
# the older fixture so test_shmem expectations don't need to change.
# OFF_STOCK_VRMS_CHIP @ 0x0000 = 23000
buf[0x0000] = 0xD8; buf[0x0001] = 0x59
# OFF_STOCK_VRMS_DECI @ 0x0004 = 160
buf[0x0004] = 0xA0; buf[0x0005] = 0x00
# OFF_STOCK_POWER_CHIP @ 0x000c = 3680
buf[0x000c] = 0x60; buf[0x000d] = 0x0E; buf[0x000e] = 0x00; buf[0x000f] = 0x00

# --- Bridge-cooked V/I/P/E (what web reads). u32 LE fixed-point. -------
# Meter personality writes these. Seeded here so charger_state's
# fixture-path verifies the end-to-end read.
# OFF_BRIDGE_VOLTAGE_CV @ 0x0500 = 23000 cV -> 230.00 V
buf[0x0500] = 0xD8; buf[0x0501] = 0x59; buf[0x0502] = 0x00; buf[0x0503] = 0x00
# OFF_BRIDGE_CURRENT_MA @ 0x0504 = 16000 mA -> 16.000 A
buf[0x0504] = 0x80; buf[0x0505] = 0x3E; buf[0x0506] = 0x00; buf[0x0507] = 0x00
# OFF_BRIDGE_POWER_W    @ 0x0508 = 3680 W
buf[0x0508] = 0x60; buf[0x0509] = 0x0E; buf[0x050a] = 0x00; buf[0x050b] = 0x00
# OFF_BRIDGE_ENERGY_WH  @ 0x050c = 12345 Wh
buf[0x050c] = 0x39; buf[0x050d] = 0x30; buf[0x050e] = 0x00; buf[0x050f] = 0x00

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
