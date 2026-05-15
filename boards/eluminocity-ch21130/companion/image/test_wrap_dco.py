#!/usr/bin/env python3
"""Tests for wrap_dco.py — the DELTADCOF bundle wrapper."""
import wrap_dco

def test_wrap_appends_magic_and_sum():
    payload = bytes([0x01, 0x02, 0x03, 0xFE])
    out = wrap_dco.wrap(payload)
    # payload, then 9-byte ASCII magic, then BE-u32 byte-sum
    assert out[:4] == payload
    assert out[4:13] == b"DELTADCOF"
    expected_sum = sum(payload) & 0xFFFFFFFF
    assert int.from_bytes(out[13:17], "big") == expected_sum
    assert len(out) == len(payload) + 13

def test_roundtrip_unwrap():
    payload = bytes(range(256)) * 3
    out = wrap_dco.wrap(payload)
    assert wrap_dco.unwrap(out) == payload

def test_unwrap_rejects_bad_magic():
    try:
        wrap_dco.unwrap(b"x" * 20)
    except ValueError:
        return
    assert False, "expected ValueError on bad magic"

if __name__ == "__main__":
    test_wrap_appends_magic_and_sum()
    test_roundtrip_unwrap()
    test_unwrap_rejects_bad_magic()
    print("test_wrap_dco: 3/3 passed")
