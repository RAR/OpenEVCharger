#!/usr/bin/env python3
"""ntc_peek.py — snapshot all candidate ADC pins for NTC identification.

Halts the N32G45x momentarily via ST-Link, reads s_adc_buf (ADC1
continuous DMA, 4 ranks) and adc2_diag_buf (ADC2 on-demand scan, 7
ranks), then resumes. Prints a labeled table so we can spot which
channel drops to ~0 when an NTC line is shorted to ground.

Symbol addresses are hard-coded for the current build_nexcyber elf;
re-run `arm-none-eabi-nm build_nexcyber/openevcharger.elf | grep -E
'adc2_diag_buf|s_adc_buf'` and update if you rebuild and the linker
shuffles things.

Usage:
    tools/ntc_peek.py                # one shot
    tools/ntc_peek.py baseline       # save as baseline
    tools/ntc_peek.py diff           # compare to baseline + flag deltas

ADC reference is VrefInt-corrected from rank 3 of ADC1.
"""
from __future__ import annotations

import json
import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

ADC1_ADDR = 0x20000028  # 4 × u16
ADC2_ADDR = 0x20000018  # 7 × u16

ADC1_LABELS = [
    ("PA6",  3,  "ADC1 ch3  (rippleon CT1 slot, nexcyber TBD)"),
    ("PC0",  6,  "NTC_GUN_A (high-Z, ~3.3V open)"),
    ("PC1",  7,  "NTC_GUN_B (divided,  ~2.2V open)"),
    ("VRI", 18, "VrefInt (~1.20 V)"),
]

ADC2_LABELS = [
    ("PA4",  1,  "CP secondary (bench-locked)"),
    ("PA5",  2,  "TBD"),
    ("PB1",  3,  "TBD"),
    ("PA7",  4,  "TBD"),
    ("PC4",  5,  "GFCI sense (bench-locked)"),
    ("PC5", 12, "NTC_BOARD (onboard PCB)"),
    ("PB2", 13, "CP_RAW (bench-locked)"),
]


def run_openocd(commands):
    args = [
        "openocd",
        "-f", "interface/stlink.cfg",
        "-c", "transport select hla_swd",
        "-f", "target/stm32f4x.cfg",
        "-c", "init",
        "-c", "halt",
    ]
    for c in commands:
        args += ["-c", c]
    args += ["-c", "resume", "-c", "shutdown"]
    out = subprocess.run(args, capture_output=True, text=True)
    # openocd writes "Info :" lines to stderr; mdw output appears there too
    return out.stderr + out.stdout


def parse_mdw(text, addr, count):
    """Pull `count` consecutive words starting at `addr` from mdw output.

    OpenOCD prints lines like:
        0x20000028: 0ff6033c 05c00a8d
        0x20000018: 021c0364 03220743 080f0865 0000088b
    Note: data words are 8-hex-digit tokens WITHOUT the 0x prefix.
    """
    import re
    addr_marker = f"{addr:#010x}:"
    words = []
    in_block = False
    for line in text.splitlines():
        s = line.strip()
        if s.startswith(addr_marker):
            in_block = True
            data_part = s[len(addr_marker):]
        elif in_block:
            # Continuation line: another address marker means a new block.
            m = re.match(r"(0x[0-9a-fA-F]+):", s)
            if m:
                # New block — only continue if it's the next expected addr.
                if int(m.group(1), 16) != addr + len(words) * 4:
                    break
                data_part = s[len(m.group(0)):]
            else:
                break
        else:
            continue
        for tok in data_part.split():
            try:
                words.append(int(tok, 16))
            except ValueError:
                pass
            if len(words) >= count:
                return words[:count]
    return words[:count]


def words_to_halfwords(words, n):
    """Little-endian: low halfword first."""
    h = []
    for w in words:
        h.append(w & 0xFFFF)
        h.append((w >> 16) & 0xFFFF)
    return h[:n]


def peek():
    text = run_openocd([
        f"mdw {ADC1_ADDR:#x} 2",   # 2 words = 4 halfwords
        f"mdw {ADC2_ADDR:#x} 4",   # 4 words = 8 halfwords (use first 7)
    ])
    w1 = parse_mdw(text, ADC1_ADDR, 2)
    w2 = parse_mdw(text, ADC2_ADDR, 4)
    if len(w1) < 2 or len(w2) < 4:
        print("openocd output:\n" + text, file=sys.stderr)
        raise RuntimeError("Failed to parse mdw output")
    adc1 = words_to_halfwords(w1, 4)
    adc2 = words_to_halfwords(w2, 7)
    return adc1, adc2


def raw_to_mv(raw, vri_raw):
    """ADC raw → mV using VrefInt. VrefInt nominal = 1200 mV."""
    if vri_raw == 0:
        return None
    return (raw * 1200) // vri_raw


def print_table(adc1, adc2):
    vri = adc1[3]
    print(f"VrefInt = {vri} raw ({raw_to_mv(vri, vri) if vri else '?'} mV nominal 1200)")
    print()
    print(f"{'PIN':<5} {'CH':>3}  {'RAW':>5}  {'mV':>6}  NOTE")
    print("-" * 70)
    for (pin, ch, note), raw in zip(ADC1_LABELS, adc1):
        mv = raw_to_mv(raw, vri) if vri else None
        mv_s = f"{mv:>6}" if mv is not None else "    ??"
        print(f"{pin:<5} {ch:>3}  {raw:>5}  {mv_s}  {note}")
    print()
    for (pin, ch, note), raw in zip(ADC2_LABELS, adc2):
        mv = raw_to_mv(raw, vri) if vri else None
        mv_s = f"{mv:>6}" if mv is not None else "    ??"
        print(f"{pin:<5} {ch:>3}  {raw:>5}  {mv_s}  {note}")


def save_baseline(adc1, adc2):
    path = REPO_ROOT / "tools" / ".ntc_peek_baseline.json"
    path.write_text(json.dumps({"adc1": adc1, "adc2": adc2}))
    print(f"Baseline saved → {path}")


def load_baseline():
    path = REPO_ROOT / "tools" / ".ntc_peek_baseline.json"
    if not path.exists():
        return None
    d = json.loads(path.read_text())
    return d["adc1"], d["adc2"]


def print_diff(adc1, adc2, base):
    b1, b2 = base
    vri = adc1[3]
    print(f"VrefInt = {adc1[3]} raw (baseline {b1[3]})")
    print()
    print(f"{'PIN':<5} {'CH':>3}  {'BASE':>5} {'NOW':>5}  {'Δraw':>6}  {'Δmv':>6}  NOTE")
    print("-" * 80)
    for labels, cur, ref in [(ADC1_LABELS, adc1, b1), (ADC2_LABELS, adc2, b2)]:
        for (pin, ch, note), c, b in zip(labels, cur, ref):
            d_raw = c - b
            d_mv = raw_to_mv(c, vri) - raw_to_mv(b, b1[3]) if vri and b1[3] else 0
            flag = "  ← BIG DROP" if d_raw < -200 else ("  ← BIG RISE" if d_raw > 200 else "")
            print(f"{pin:<5} {ch:>3}  {b:>5} {c:>5}  {d_raw:>+6}  {d_mv:>+6}  {note}{flag}")
        print()


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "show"
    adc1, adc2 = peek()
    if mode == "baseline":
        print_table(adc1, adc2)
        save_baseline(adc1, adc2)
    elif mode == "diff":
        base = load_baseline()
        if base is None:
            print("No baseline saved. Run `ntc_peek.py baseline` first.")
            sys.exit(1)
        print_diff(adc1, adc2, base)
    else:
        print_table(adc1, adc2)


if __name__ == "__main__":
    main()
