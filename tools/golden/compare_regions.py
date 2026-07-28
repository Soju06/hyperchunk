#!/usr/bin/env python3
"""Compare two Anvil region files at chunk-payload level, or hash one
canonically. This answers the ADR-002 D3 determinism question precisely:
raw-file sha256 vs payload-level equality vs canonical (LastUpdate-masked)
equality are reported separately.

Usage:
    compare_regions.py A.mca B.mca          # detailed comparison, exit 0/1
    compare_regions.py --canonical-hash F.mca   # print canonical sha256

Comparison layers (each reported independently):
  1. raw file bytes           — what a naive sha256 gate sees
  2. header timestamp table   — wall-clock save times, expected to differ
  3. chunk payloads (NBT, decompressed) — worldgen output + save-time fields
  4. canonical payloads       — payloads with root LastUpdate masked to 0

Canonical hash = sha256 over (index || masked payload) for every present
chunk in index order. Header, sector layout and compression framing are
excluded, so it is stable under resaves that only move sectors around.
"""

import hashlib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mca import mask_last_update, nbt_diff, parse_nbt, read_region


def canonical_hash(path: str) -> str:
    chunks = read_region(path)
    h = hashlib.sha256()
    for i in sorted(chunks):
        h.update(struct.pack(">I", i))
        h.update(mask_last_update(chunks[i].payload))
    return h.hexdigest()


def compare(pa: str, pb: str) -> int:
    raw_equal = Path(pa).read_bytes() == Path(pb).read_bytes()
    print(f"raw_file_bytes_equal {raw_equal}")

    a, b = read_region(pa), read_region(pb)
    print(f"present A={len(a)} B={len(b)}")
    only_a = sorted(set(a) - set(b))
    only_b = sorted(set(b) - set(a))
    if only_a:
        print(f"chunks_only_in_A {len(only_a)} e.g. {only_a[:8]}")
    if only_b:
        print(f"chunks_only_in_B {len(only_b)} e.g. {only_b[:8]}")

    common = sorted(set(a) & set(b))
    ts_diff = [i for i in common if a[i].timestamp != b[i].timestamp]
    print(f"header_timestamps_differ {len(ts_diff)}/{len(common)}")

    payload_diff = [i for i in common if a[i].payload != b[i].payload]
    print(f"payloads_differ {len(payload_diff)}/{len(common)}")

    canon_diff = [
        i for i in payload_diff
        if mask_last_update(a[i].payload) != mask_last_update(b[i].payload)
    ]
    print(f"canonical_payloads_differ {len(canon_diff)}/{len(common)}")

    # Explain the first few payload differences via NBT structural diff.
    shown = 0
    for i in payload_diff:
        if shown >= 5:
            print(f"... {len(payload_diff) - shown} more differing chunks not shown")
            break
        _, ra = parse_nbt(a[i].payload)
        _, rb = parse_nbt(b[i].payload)
        diffs = nbt_diff(ra, rb, limit=10)
        masked = " (masked by canonicalization)" if i not in canon_diff else ""
        print(f"chunk[{i}] (x={i % 32}, z={i // 32}) NBT diff{masked}:")
        for d in diffs:
            print(f"    {d}")
        shown += 1

    print(f"canonical_sha256_A {canonical_hash(pa)}")
    print(f"canonical_sha256_B {canonical_hash(pb)}")

    if only_a or only_b or canon_diff:
        print("VERDICT: regions differ beyond save-time metadata")
        return 1
    if payload_diff or ts_diff or not raw_equal:
        print("VERDICT: worldgen-identical; differences are save-time metadata only")
        return 0
    print("VERDICT: bit-identical files")
    return 0


def main() -> int:
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "--canonical-hash":
        print(f"{canonical_hash(args[1])}  {args[1]}")
        return 0
    if len(args) == 2:
        return compare(args[0], args[1])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
