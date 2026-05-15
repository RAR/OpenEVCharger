#!/usr/bin/env python3
"""DELTADCOF bundle wrapper for the Eluminocity CH-21130 / Delta EVMU30.

Our unit's UpdateCSU() accepts /UsbFlash/DcoFImage when it carries the legacy
9-byte ASCII magic "DELTADCOF" followed by a big-endian uint32 byte-sum of the
payload. (The newer AC Mini Plus uses a different model-string trailer — we do
NOT use that format here.)

    python3 wrap_dco.py <payload-in> <DcoFImage-out>
"""
import sys

MAGIC = b"DELTADCOF"

def wrap(payload: bytes) -> bytes:
    checksum = sum(payload) & 0xFFFFFFFF
    return payload + MAGIC + checksum.to_bytes(4, "big")

def unwrap(bundle: bytes) -> bytes:
    if len(bundle) < 13 or bundle[-13:-4] != MAGIC:
        raise ValueError("not a DELTADCOF bundle (bad magic)")
    payload = bundle[:-13]
    stored = int.from_bytes(bundle[-4:], "big")
    if (sum(payload) & 0xFFFFFFFF) != stored:
        raise ValueError("DELTADCOF checksum mismatch")
    return payload

def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 1
    with open(argv[1], "rb") as f:
        payload = f.read()
    with open(argv[2], "wb") as f:
        f.write(wrap(payload))
    print(f"wrote {argv[2]} ({len(payload)} + 13 bytes)")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
