#!/usr/bin/env python3
"""Extract an out-of-region structure start for the Task-14 beardifier.

The r.0.0 full-region gate needs the piece list (BBs, projections,
ground_level_delta, junctions) of trial_chambers start c.13.35, whose start
chunk lives in r.0.1. The coherent source is the SAME capture run that
produced the golden r.0.0 (tools/golden/work/unified-run — its world
r.0.0.mca hash equals golden/seed1234567890_r.0.0.mca), so the neighbor
region file is golden data from the recorded run, not a re-generation.

Usage:
  extract_neighbor_start.py <region.mca> <cx> <cz>

Writes golden/structures/c.<cx>.<cz>.starts.nbt (verbatim byte span,
same wrapping as extract_structures.py) and prints its sha256. Refuses
to run if the sibling r.0.0.mca of the source world does not match the
golden region hash (coherence guard).
"""

import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_structures import Scanner
from mca import read_region

ROOT = Path(__file__).resolve().parents[2]
GOLDEN_R00 = ROOT / "golden" / "seed1234567890_r.0.0.mca"
OUT = ROOT / "golden" / "structures"


def main() -> int:
    region = Path(sys.argv[1])
    cx, cz = int(sys.argv[2]), int(sys.argv[3])

    sibling = region.parent / "r.0.0.mca"
    if not sibling.exists():
        print(f"coherence guard: {sibling} missing", file=sys.stderr)
        return 1
    if hashlib.sha256(sibling.read_bytes()).hexdigest() != hashlib.sha256(
        GOLDEN_R00.read_bytes()
    ).hexdigest():
        print("coherence guard: source world r.0.0.mca != golden region",
              file=sys.stderr)
        return 1

    chunks = read_region(str(region))
    idx = (cz & 31) * 32 + (cx & 31)
    if idx not in chunks:
        print(f"chunk c.{cx}.{cz} not present in {region}", file=sys.stderr)
        return 1
    e = chunks[idx]
    # mca.py reports region-local coords (== absolute only in r.0.0)
    assert e.x == (cx & 31) and e.z == (cz & 31), (e.x, e.z)
    tag, a, b = Scanner(e.payload).find_span(["structures", "starts"])
    assert tag == 10
    frag = b"\x0a\x00\x00" + e.payload[a:b]
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / f"c.{cx}.{cz}.starts.nbt"
    path.write_bytes(frag)
    print(f"{hashlib.sha256(frag).hexdigest()}  structures/{path.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
