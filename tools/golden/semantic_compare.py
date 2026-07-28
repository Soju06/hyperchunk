#!/usr/bin/env python3
"""Semantic (content-level) comparison of two Anvil region files.

The byte/canonical comparison in compare_regions.py cannot distinguish
"different world content" from "same content, different serialization":
vanilla builds section palettes in first-write order and cross-chunk feature
writes interleave nondeterministically, so palette order — and with it every
packed block_states.data long — varies run to run even when every block is
identical. This tool decodes the serialization away and compares content:

  - sections[].block_states  -> per-block state strings (y,z,x order)
  - sections[].biomes        -> per-quart biome ids
  - Heightmaps.*             -> 256 decoded 9-bit values
  - block_ticks/fluid_ticks  -> sorted by (x,y,z,type), i.e. order-insensitive
  - PostProcessing           -> per-section sorted short lists
  - LastUpdate               -> dropped (save-time game tick)

Usage:
    semantic_compare.py A.mca B.mca        # exit 0 iff content-identical
    semantic_compare.py --hash F.mca       # semantic sha256 of one region
"""

import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mca import nbt_diff, parse_nbt, read_region


def unpack(data: list[int], bits: int, count: int) -> list[int]:
    """Unpack MC 1.16+ packed long arrays (entries never span longs)."""
    per_long = 64 // bits
    mask = (1 << bits) - 1
    out = []
    for v in data:
        v &= (1 << 64) - 1  # stored as signed long
        for _ in range(per_long):
            out.append(v & mask)
            v >>= bits
            if len(out) == count:
                return out
    return out


def state_str(entry) -> str:
    name = entry["Name"]
    props = entry.get("Properties")
    if not props:
        return name
    inner = ",".join(f"{k}={props[k]}" for k in sorted(props))
    return f"{name}[{inner}]"


def decode_paletted(container, count: int, min_bits: int, to_str) -> list[str]:
    palette = [to_str(e) for e in container["palette"]]
    data = container.get("data")
    if not data:
        return [palette[0]] * count
    bits = max(min_bits, (len(palette) - 1).bit_length())
    return [palette[i] for i in unpack(data, bits, count)]


def tick_key(t) -> tuple:
    return (t.get("x", 0), t.get("y", 0), t.get("z", 0), t.get("i", ""), t.get("t", 0), t.get("p", 0))


def semantic_form(root: dict) -> dict:
    out = dict(root)
    out.pop("LastUpdate", None)
    sections = []
    for s in root.get("sections", []):
        s2 = dict(s)
        if "block_states" in s2:
            s2["block_states"] = decode_paletted(s2["block_states"], 4096, 4, state_str)
        if "biomes" in s2:
            s2["biomes"] = decode_paletted(s2["biomes"], 64, 1, str)
        sections.append(s2)
    out["sections"] = sections
    if "Heightmaps" in out:
        out["Heightmaps"] = {
            k: unpack(v, 9, 256) for k, v in sorted(out["Heightmaps"].items())
        }
    for key in ("block_ticks", "fluid_ticks"):
        if key in out:
            out[key] = sorted(out[key], key=tick_key)
    if "PostProcessing" in out:
        out["PostProcessing"] = [sorted(sec) for sec in out["PostProcessing"]]
    return out


def _jsonable(x):
    if isinstance(x, bytes):
        return list(x)
    if isinstance(x, dict):
        return {k: _jsonable(v) for k, v in sorted(x.items())}
    if isinstance(x, list):
        return [_jsonable(v) for v in x]
    return x


def semantic_hash(path: str) -> str:
    chunks = read_region(path)
    h = hashlib.sha256()
    for i in sorted(chunks):
        _, root = parse_nbt(chunks[i].payload)
        blob = json.dumps(_jsonable(semantic_form(root)), separators=(",", ":"), sort_keys=True)
        h.update(str(i).encode())
        h.update(blob.encode())
    return h.hexdigest()


def compare(pa: str, pb: str) -> int:
    a, b = read_region(pa), read_region(pb)
    common = sorted(set(a) & set(b))
    only = sorted(set(a) ^ set(b))
    if only:
        print(f"chunks_not_in_both {len(only)} e.g. {only[:8]}")
    differing = []
    for i in common:
        _, ra = parse_nbt(a[i].payload)
        _, rb = parse_nbt(b[i].payload)
        sa, sb = semantic_form(ra), semantic_form(rb)
        if sa != sb:
            differing.append((i, nbt_diff(sa, sb, limit=6)))
    print(f"semantic_content_differs {len(differing)}/{len(common)}")
    for i, diffs in differing[:8]:
        print(f"chunk[{i}] (x={i % 32}, z={i // 32}):")
        for d in diffs:
            print(f"    {d[:200]}")
    if len(differing) > 8:
        print(f"... {len(differing) - 8} more differing chunks not shown")
    if differing or only:
        print("VERDICT: world content differs")
        return 1
    print("VERDICT: world content is identical (serialization differences only)")
    return 0


def main() -> int:
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "--hash":
        print(f"{semantic_hash(args[1])}  {args[1]}")
        return 0
    if len(args) == 2:
        return compare(args[0], args[1])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
