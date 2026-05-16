#!/usr/bin/env python3
"""Produce a per-offset producer/consumer matrix from CSV emitted by
find_shmem_accesses.py --csv."""
import csv, collections, sys

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/delta-matrix/matrix.csv"

writers = collections.defaultdict(set)
readers = collections.defaultdict(set)
sizes   = collections.defaultdict(set)

with open(path) as f:
    rdr = csv.DictReader(f)
    for row in rdr:
        off = int(row["off"], 16)
        b = row["binary"]
        sz = int(row["size"])
        sizes[off].add(sz)
        if row["kind"] == "W":
            writers[off].add(b)
        else:
            readers[off].add(b)

all_offs = sorted(set(writers) | set(readers))
print(f"# {len(all_offs)} unique shmem offsets across {len(set(b for s in writers.values() for b in s) | set(b for s in readers.values() for b in s))} binaries")
print()
print(f"{'offset':>8s}  {'B':>5s}  {'W':>5s} writers                                  {'R':>5s} readers")
print("-" * 110)
for off in all_offs:
    sz = ",".join(str(s) for s in sorted(sizes[off]))
    w = sorted(writers.get(off, set()))
    r = sorted(readers.get(off, set()))
    w_str = ",".join(w)
    r_str = ",".join(r)
    print(f"  0x{off:06x}  {sz:>5s}  {len(w):>5d} {w_str[:40]:40s}  {len(r):>5d} {r_str[:40]:40s}")
