#!/usr/bin/env python3
"""Generate a deterministic 256 KiB shmem fixture with known sentinel values at
the offsets shmem.c reads. Tests assert against these sentinels — this verifies
accessor *logic*, independent of real-hardware RE uncertainty (that is covered
by bench milestones M0/M1). Run once; the output .bin is committed.

    python3 make_shmem_fixture.py
"""
import pathlib

SIZE = 0x40000
buf = bytearray(SIZE)

# (offset, value) — must match src/shmem_offsets.h
buf[0xa00] = 0x02          # connector state
buf[0xa07] = 0x42          # fault flags (OFF_FAULT_FLAGS)
buf[0xa08] = 0x55          # heartbeat
buf[0xa0b] = 0x01          # STM32 link OK
buf[0xa10] = 0x78          # VRMS raw (120)
buf[0xa24] = 0x10          # IRMS raw (16)
buf[0xa63] = 0x09          # FW-upgrade gate (OFF_FW_UPGRADE_GATE)

# trap-alarm bitmap 0x138..0x158 (32 bytes): set byte +3 nonzero
buf[0x138 + 3] = 0x01

out = pathlib.Path(__file__).parent / "shmem_snapshot.bin"
with open(out, "wb") as f:
    f.write(buf)
print(f"wrote shmem_snapshot.bin ({SIZE} bytes)")
