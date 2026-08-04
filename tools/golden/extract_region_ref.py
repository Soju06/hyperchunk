#!/usr/bin/env python3
"""Extract per-chunk canonical reference payloads from the golden region.

Writes golden/region-ref/c.<x>.<z>.nbt (decompressed chunk NBT payload with
the root LastUpdate value masked to 0 — the canonical form hashed by
compare_regions.py) for the r.0.0 grid chunks that the Task-12 C region
gate byte-compares against. Files are local-only (gitignored, like stage
dumps); their hashes live in golden/SHA256SUMS. Re-run after fetching the
golden .mca:

    python3 tools/golden/extract_region_ref.py

Prints the SHA256SUMS lines it wrote/verified.
"""

import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mca import mask_last_update, read_region

ROOT = Path(__file__).resolve().parents[2]
REGION = ROOT / "golden" / "seed1234567890_r.0.0.mca"
OUT = ROOT / "golden" / "region-ref"

# Task 12 shipped the replayable 3x3-grid subset; Task 14 (full-region
# gate) extracts every present chunk. The 4 original files are byte-stable
# under this extension (same masking), so existing SHA256SUMS lines hold.
def main() -> int:
    chunks = read_region(str(REGION))
    OUT.mkdir(parents=True, exist_ok=True)
    for idx in sorted(chunks):
        e = chunks[idx]
        payload = mask_last_update(e.payload)
        path = OUT / f"c.{e.x}.{e.z}.nbt"
        path.write_bytes(payload)
        sha = hashlib.sha256(payload).hexdigest()
        print(f"{sha}  region-ref/{path.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
