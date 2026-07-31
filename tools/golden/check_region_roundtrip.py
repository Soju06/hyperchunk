#!/usr/bin/env python3
"""Round-trip guard for the Task-12 region gate (ctest region_out_roundtrip).

Validates the C region writer's container framing with the same reader
that defines the canonical-payload gate (tools/golden/mca.py):
  1. OUR .mca parses (offset table, length = compressed+1, zlib type 2,
     stored-block DEFLATE decompresses cleanly);
  2. every chunk's decompressed payload is byte-identical to the payload
     the serializer emitted (<ours>.c.<x>.<z>.ours.nbt written by
     test_region) — the container is lossless;
  3. chunk (0,0) — the order-sticky strict chunk — matches the GOLDEN
     region's canonical fragment through the full container path.
Byte parity for the other chunks is bounded by check_region_residuals.py
(stale-mca envelope; see that script's docstring).

    check_region_roundtrip.py OURS.mca GOLDEN.mca
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mca import mask_last_update, read_region

CHUNKS = [(0, 0), (1, 0), (0, 1), (1, 1)]


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    ours_path = sys.argv[1]
    ours = read_region(ours_path)
    golden = read_region(sys.argv[2])
    bad = 0
    for cx, cz in CHUNKS:
        idx = (cx & 31) + (cz & 31) * 32
        if idx not in ours:
            print(f"c.{cx}.{cz}: MISSING from ours")
            bad += 1
            continue
        e = ours[idx]
        if e.compression != 2:
            print(f"c.{cx}.{cz}: compression id {e.compression} != 2")
            bad += 1
        want = Path(f"{ours_path}.c.{cx}.{cz}.ours.nbt").read_bytes()
        if e.payload != want:
            print(f"c.{cx}.{cz}: container round-trip NOT lossless "
                  f"({len(e.payload)}B vs {len(want)}B)")
            bad += 1
        else:
            print(f"c.{cx}.{cz}: container round-trip lossless "
                  f"({len(e.payload)} bytes)")
    a = mask_last_update(ours[0].payload)
    b = mask_last_update(golden[0].payload)
    if a != b:
        off = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y),
                   min(len(a), len(b)))
        print(f"c.0.0: canonical fragment MISMATCH vs golden (first diff "
              f"@{off})")
        bad += 1
    else:
        print(f"c.0.0: canonical fragment == golden ({len(a)} bytes)")
    if bad:
        print(f"roundtrip: FAIL ({bad})")
        return 1
    print("roundtrip: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
