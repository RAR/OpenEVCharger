#!/usr/bin/env python3
"""
find_shmem_accesses.py — find both reads and writes into the SysV-shmem
region in a debug ARM ELF whose shmat() return value is stored in a single
global pointer.

Strategy:
  1. Auto-detect the shmem-base global pointer ("MeterSMPtr" or analogous):
     look for a sequence  bl shmat@plt;  mov r2, r0;  ldr r3, [pc, #N];
     str r2, [r3]  — the address stored at that literal-pool entry IS the
     pointer global. (Override via --ptr=HEX.)
  2. Walk the disassembly forward instruction-by-instruction. Track a
     per-register "is-shmem-base?" tag using a tiny constant-propagation
     model. When we see:
        ldr  Rx, [pc, #N]   @ literal pool entry whose value == PTR
        ldr  Rx, [Rx]                  (dereference)
     we mark Rx as 'B' (shmem base, offset 0).
     For:
        add  Ry, Rx, #imm     (Rx tagged B+k)  ->  tag Ry as B+(k+imm)
        mov  Ry, Rx           (Rx tagged)      ->  Ry inherits tag
        ldr/ldrb/ldrh ..., [Rx ...]   (Rx tagged B+k)  ->  record READ
        str/strb/strh ..., [Rx ...]   (Rx tagged B+k)  ->  record WRITE
     Branch boundaries (function ends, b/bl) reset reg tags.
  3. Multi-step add chain handled: add r,r,#imm; add r,r,#imm2  → cumulative.

Usage:
    find_shmem_accesses.py BINARY [--ptr=HEX] [--csv] [--unique-only]
"""
import sys, re, subprocess, collections, argparse

ap = argparse.ArgumentParser()
ap.add_argument("binary")
ap.add_argument("--ptr", help="override shmem-base global addr (hex, no 0x)")
ap.add_argument("--csv", action="store_true", help="emit CSV instead of human-readable")
ap.add_argument("--unique-only", action="store_true", help="skip per-instruction rows; show only unique-offset summary")
ap.add_argument("--name", help="binary short-name for CSV output (default: basename)")
args = ap.parse_args()

import os
SHORT = args.name or os.path.basename(args.binary)

dis = subprocess.run(
    ["arm-linux-gnueabi-objdump", "-d", args.binary],
    capture_output=True, text=True, check=True
).stdout.splitlines()

# ---------------- Pass 1: build literal-pool map -----------------------
LITERAL = {}  # addr_int -> value_int
WORD_RE = re.compile(r'^\s*([0-9a-f]+):\s+([0-9a-f]+)\s+\.word\s+0x([0-9a-f]+)')
for line in dis:
    m = WORD_RE.match(line)
    if m:
        LITERAL[int(m.group(1), 16)] = int(m.group(3), 16)

# ---------------- Auto-detect PTR if not given --------------------------
# Pattern after shmat@plt return:
#   bl   ???? <shmat@plt>
#   (some nop / mov)
#   mov  r2, r0
#   ldr  r3, [pc, #N]   @ ADDR  → LITERAL[ADDR] is the shmem-base global addr
#   str  r2, [r3]
SHMAT_RE = re.compile(r'^\s*[0-9a-f]+:\s+\S+\s+bl\s+[0-9a-f]+\s+<shmat@plt>')
MOV_R0_RE = re.compile(r'^\s*[0-9a-f]+:\s+\S+\s+mov\s+r2,\s+r0\s*$')
LDR_PCREL = re.compile(r'^\s*[0-9a-f]+:\s+\S+\s+ldr\s+(\S+),\s+\[pc,\s+#-?\d+\]\s+@\s+([0-9a-f]+)')
STR_DEREF = re.compile(r'^\s*[0-9a-f]+:\s+\S+\s+str\s+(\S+),\s+\[(\S+)\]\s*$')

if args.ptr:
    PTR_VAL = int(args.ptr, 16)
    auto = False
else:
    PTR_VAL = None
    auto = True
    in_shmat_chain = False
    chain_lines_seen = 0
    pending = None  # (ldr_reg, ldr_target_addr) waiting to confirm
    for line in dis:
        if SHMAT_RE.match(line):
            in_shmat_chain = True
            chain_lines_seen = 0
            pending = None
            continue
        if not in_shmat_chain:
            continue
        chain_lines_seen += 1
        if chain_lines_seen > 12:
            in_shmat_chain = False
            continue
        if pending is None:
            m = LDR_PCREL.match(line)
            if m:
                lit_addr = int(m.group(2), 16)
                if lit_addr in LITERAL:
                    pending = (m.group(1), LITERAL[lit_addr])
            continue
        # have pending (Rn, value-of-LITERAL[addr]) — confirm STR uses Rn as base
        m = STR_DEREF.match(line)
        if m and m.group(2) == pending[0]:
            PTR_VAL = pending[1]
            break
        pending = None
        # try the next ldr in this window
        m = LDR_PCREL.match(line)
        if m:
            lit_addr = int(m.group(2), 16)
            if lit_addr in LITERAL:
                pending = (m.group(1), LITERAL[lit_addr])
    if PTR_VAL is None:
        sys.exit(f"{SHORT}: could not auto-detect shmat pointer global (binary may not call shmat directly)")

# ---------------- Pass 2: walk instructions, track register tags -------
FUNC_RE = re.compile(r'^([0-9a-f]+)\s+<([^>]+)>:')
LDR_PCREL_TAG = re.compile(
    r'^\s*([0-9a-f]+):\s+\S+\s+ldr\s+(r\d+|sl|fp|ip),\s+\[pc,\s+#(-?\d+)\].*@\s*([0-9a-f]+)'
)
LDR_DEREF_RE = re.compile(
    r'^\s*([0-9a-f]+):\s+\S+\s+ldr\s+(r\d+|sl|fp|ip),\s+\[(r\d+|sl|fp|ip)\]\s*$'
)
ADD_IMM_RE = re.compile(
    r'^\s*([0-9a-f]+):\s+\S+\s+add\s+(r\d+|sl|fp|ip),\s+(r\d+|sl|fp|ip),\s+#(\d+)'
)
MOV_REG_RE = re.compile(
    r'^\s*([0-9a-f]+):\s+\S+\s+mov\s+(r\d+|sl|fp|ip),\s+(r\d+|sl|fp|ip)\s*$'
)
ACC_RE = re.compile(
    r'^\s*([0-9a-f]+):\s+\S+\s+(strb|strh|str|ldrb|ldrh|ldr)\s+(\S+),\s+\[(r\d+|sl|fp|ip)(?:,\s+#(-?\d+))?\]'
)
BRANCH_RE = re.compile(
    r'^\s*([0-9a-f]+):\s+\S+\s+(bl?|bx|bne|beq|bgt|blt|bge|ble|bhi|blo|bcs|bcc|bmi|bpl|bvs|bvc)\b'
)

SIZE = {"str": 4, "strb": 1, "strh": 2,
        "ldr": 4, "ldrb": 1, "ldrh": 2}

# Output rows: (insn_addr, fn, op, size_bytes, offset_int, src_or_dst)
HITS = []
cur_fn = "?"
reg_tag = {}   # reg_name -> ('B', offset_int)

def reset():
    reg_tag.clear()

for line in dis:
    fm = FUNC_RE.match(line)
    if fm:
        cur_fn = fm.group(2)
        reset()
        continue
    if not line.strip():
        reset()
        continue

    # ldr Rx, [pc, #N]
    m = LDR_PCREL_TAG.match(line)
    if m:
        rx = m.group(2); lit_addr = int(m.group(4), 16)
        val = LITERAL.get(lit_addr)
        if val == PTR_VAL:
            reg_tag[rx] = ('P', 0)
        else:
            reg_tag.pop(rx, None)
        continue

    # ldr Rx, [Ry] — promotes P → B
    m = LDR_DEREF_RE.match(line)
    if m:
        rx, ry = m.group(2), m.group(3)
        t = reg_tag.get(ry)
        if t and t[0] == 'P':
            reg_tag[rx] = ('B', 0)
        else:
            reg_tag.pop(rx, None)
        continue

    # add Rx, Ry, #imm — propagate offset
    m = ADD_IMM_RE.match(line)
    if m:
        rx, ry, imm = m.group(2), m.group(3), int(m.group(4))
        t = reg_tag.get(ry)
        if t and t[0] == 'B':
            reg_tag[rx] = ('B', t[1] + imm)
        elif rx == ry and t and t[0] == 'B':
            # add r3, r3, #imm — adjust in place (handled above too)
            reg_tag[rx] = ('B', t[1] + imm)
        else:
            reg_tag.pop(rx, None)
        continue

    # mov Rx, Ry — propagate tag
    m = MOV_REG_RE.match(line)
    if m:
        rx, ry = m.group(2), m.group(3)
        if ry in reg_tag:
            reg_tag[rx] = reg_tag[ry]
        else:
            reg_tag.pop(rx, None)
        continue

    # ldr/ldrb/ldrh/str/strb/strh
    m = ACC_RE.match(line)
    if m:
        ia, op, src, rb, imm = m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)
        t = reg_tag.get(rb)
        if t and t[0] == 'B':
            off = t[1] + (int(imm) if imm else 0)
            HITS.append((int(ia, 16), cur_fn, op, SIZE[op], off, src))
        continue

    # any branch invalidates regs (conservative)
    m = BRANCH_RE.match(line)
    if m:
        reset()
        continue

# ---------------- Output ----------------------------------------------
WR = [h for h in HITS if h[2].startswith("str")]
RD = [h for h in HITS if h[2].startswith("ldr")]

if args.csv:
    # CSV format: binary,kind,off,size,fn,insn
    for h in HITS:
        ia, fn, op, sz, off, src = h
        kind = "W" if op.startswith("str") else "R"
        print(f"{SHORT},{kind},0x{off:06x},{sz},{fn},{ia:#x}")
    sys.exit(0)

print(f"# {SHORT}: {len(HITS)} shmem accesses via *0x{PTR_VAL:x}"
      f"  ({len(WR)} writes, {len(RD)} reads){'  [auto-detected ptr]' if auto else ''}")

if not args.unique_only:
    print()
    print("WRITES (str/strb/strh)")
    print(f"{'insn':>8s}  {'fn':30s}  {'op':5s}  {'size':>4s}  {'off':>8s}  src")
    print("-" * 80)
    for ia, fn, op, sz, off, src in sorted(WR, key=lambda r: (r[4], r[0])):
        print(f"  {ia:06x}  {fn[:30]:30s}  {op:5s}  {sz:>4d}  0x{off:06x}  {src}")
    print()
    print("READS (ldr/ldrb/ldrh)")
    print(f"{'insn':>8s}  {'fn':30s}  {'op':5s}  {'size':>4s}  {'off':>8s}  dst")
    print("-" * 80)
    for ia, fn, op, sz, off, src in sorted(RD, key=lambda r: (r[4], r[0])):
        print(f"  {ia:06x}  {fn[:30]:30s}  {op:5s}  {sz:>4d}  0x{off:06x}  {src}")

# Unique-offsets summary
def summary(label, rows):
    by_off = collections.defaultdict(set)
    for _, _, _, sz, off, _ in rows:
        by_off[off].add(sz)
    print(f"\n{label} UNIQUE OFFSETS ({len(by_off)}):")
    for off in sorted(by_off):
        szs = ",".join(str(s) for s in sorted(by_off[off]))
        print(f"  0x{off:06x}  {szs}B")

summary("WRITE", WR)
summary("READ", RD)
