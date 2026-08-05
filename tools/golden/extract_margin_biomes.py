#!/usr/bin/env python3
"""Extract margin-chunk reference payloads for the Task-14 full-region gate.

The 41x41 replay world (chunks -5..35) surrounds r.0.0 with a margin ring
whose biomes previously came from the pure-sampled wide band golden. The
band diverges from the recorded chunk biomes on Climate RTree lastResult
tie-resolution (measured 22 quarts inside r.0.0), and margin decoration
runs feature selection off those biomes — spills/edge ticks/light from
margin chunks then contaminate r.0.0. The recorded truth for the margin
lives in the SAME capture run's neighbor region files
(tools/golden/work/unified-run/.../r.{-1..1}.{-1..1}.mca — coherence
guarded by the r.0.0 hash).

Writes golden/region-ref-margin/c.<x>.<z>.nbt (LastUpdate-masked payload,
same form as region-ref) for every chunk in [-5..35]^2 outside r.0.0 that
exists in the neighbor regions. Local-only (gitignored), like region-ref.
"""

import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mca import mask_last_update, read_region

ROOT = Path(__file__).resolve().parents[2]
GOLDEN_R00 = ROOT / "golden" / "seed1234567890_r.0.0.mca"
WORLD = (ROOT / "tools" / "golden" / "work" / "unified-run" / "world" /
         "dimensions" / "minecraft" / "overworld" / "region")
OUT = ROOT / "golden" / "region-ref-margin"

C0, C1 = -5, 35


def main() -> int:
    if hashlib.sha256((WORLD / "r.0.0.mca").read_bytes()).hexdigest() != \
       hashlib.sha256(GOLDEN_R00.read_bytes()).hexdigest():
        print("coherence guard: unified-run r.0.0.mca != golden region",
              file=sys.stderr)
        return 1
    OUT.mkdir(parents=True, exist_ok=True)
    regions = {}
    n = 0
    missing = []
    for cz in range(C0, C1 + 1):
        for cx in range(C0, C1 + 1):
            if 0 <= cx < 32 and 0 <= cz < 32:
                continue
            rx, rz = cx >> 5, cz >> 5
            key = (rx, rz)
            if key not in regions:
                regions[key] = read_region(str(WORLD / f"r.{rx}.{rz}.mca"))
            idx = (cz & 31) * 32 + (cx & 31)
            if idx not in regions[key]:
                missing.append((cx, cz))
                continue
            payload = mask_last_update(regions[key][idx].payload)
            (OUT / f"c.{cx}.{cz}.nbt").write_bytes(payload)
            n += 1
    print(f"extracted {n} margin payloads to {OUT}")
    if missing:
        print(f"missing from capture ({len(missing)}): {missing}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
