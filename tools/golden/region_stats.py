#!/usr/bin/env python3
"""Print chunk population stats of a region file.

Usage: region_stats.py r.0.0.mca
Output (machine-friendly):
    present <n>
    status <status> <count>     (one line per distinct Status)
    compression <id> <count>
Used by make_golden.sh to poll for 'all 1024 chunks reached minecraft:full'.
"""

import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mca import parse_nbt, read_region


def main() -> int:
    path = sys.argv[1]
    try:
        chunks = read_region(path)
    except FileNotFoundError:
        print("present 0")
        return 0
    statuses: Counter[str] = Counter()
    compressions: Counter[int] = Counter()
    for c in chunks.values():
        _, root = parse_nbt(c.payload)
        statuses[str(root.get("Status", "?"))] += 1
        compressions[c.compression] += 1
    print(f"present {len(chunks)}")
    for s, n in sorted(statuses.items()):
        print(f"status {s} {n}")
    for cid, n in sorted(compressions.items()):
        print(f"compression {cid} {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
