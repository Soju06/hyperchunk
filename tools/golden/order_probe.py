#!/usr/bin/env python3
"""Manifest sufficiency probe (Task 9-pre §5, ADR-007 Tier 2).

Checks that two golden bundles (dumps + order.manifest each) are coherent
with the claim "per-chunk decoration order is the only free variable of the
features stage":

  1. per-chunk decoration seeds match across bundles (order-independent,
     recon A3) while the ORDER differs (that difference is the Tier-2 input);
  2. every grid chunk whose 07_features dump differs between the bundles has
     a differing decoration-order prefix (chunks within Chebyshev distance
     <= 2 decorated before it) — i.e. no dump difference without an order
     explanation;
  3. for one adjacent pair whose relative order flipped, report concrete
     differing blocks near the shared border (feature spill-over evidence);
  4. order.snapshots coherence: every 07 dump is clean (seqBegin == seqEnd)
     and sits immediately after its own manifest event; per-chunk snapshot
     positions are monotonic across stages; torn snapshots (features landed
     mid-dump) occur only for async-completing stages.

This is a consistency check, not the proof — the proof is the C replay
(Task 9) matching both bundles bit-exactly.

Usage: order_probe.py <bundleA> <bundleB>   (dirs containing order.manifest
and c.<x>.<z>/07_features.blocks.txt)
"""

import sys
from pathlib import Path

GRID = [(x, z) for x in range(-1, 2) for z in range(-1, 2)]


def read_manifest(bundle: Path):
    """-> (header dict, [(seq, cx, cz, seedhex, thread)] in file order)"""
    header, rows = {}, []
    for line in (bundle / "order.manifest").read_text().splitlines():
        if line.startswith("#"):
            parts = line[1:].strip().split(None, 1)
            if len(parts) == 2:
                header[parts[0]] = parts[1]
            continue
        seq, cx, cz, seed, thread, _nanos = line.split()
        rows.append((int(seq), int(cx), int(cz), seed, thread))
    assert [r[0] for r in rows] == list(range(len(rows))), "seq not dense"
    return header, rows


def read_blocks(path: Path):
    """-> {(x, y, z): blockstate} palette-resolved."""
    palette, grid = [], {}
    miny = None
    data = False
    y = z = 0
    for line in path.read_text().splitlines():
        if line.startswith("# minY"):
            miny = int(line.split()[2])
        elif line.startswith("palette "):
            _, idx, state = line.split(" ", 2)
            assert int(idx) == len(palette)
            palette.append(state)
        elif line == "data":
            data = True
        elif data:
            for x, idx in enumerate(line.split()):
                grid[(x, miny + y, z)] = palette[int(idx)]
            z += 1
            if z == 16:
                z = 0
                y += 1
    return grid


def read_snapshots(bundle: Path):
    """-> [(stage, cx, cz, seq_begin, seq_end, thread)] in dump order."""
    rows = []
    for line in (bundle / "order.snapshots").read_text().splitlines():
        if line.startswith("#"):
            continue
        stage, cx, cz, b, e, thread = line.split()[:6]
        rows.append((stage, int(cx), int(cz), int(b), int(e), thread))
    return rows


def check_snapshots(name, bundle, manifest_rows):
    snaps = read_snapshots(bundle)
    seq_of = {(cx, cz): q for q, cx, cz, _s, _t in manifest_rows}
    torn_by_stage = {}
    ok = True
    per_chunk_last = {}
    for stage, cx, cz, b, e, _t in snaps:
        if b != e:
            torn_by_stage[stage] = torn_by_stage.get(stage, 0) + 1
            if stage.startswith(("07_",)):
                print(f"    FAIL {name}: torn 07 snapshot c.{cx}.{cz}")
                ok = False
        if stage.startswith("07_") and b != seq_of[(cx, cz)] + 1:
            print(f"    FAIL {name}: 07 snapshot of c.{cx}.{cz} at seq {b}, "
                  f"manifest event is {seq_of[(cx, cz)]}")
            ok = False
        key = (cx, cz)
        if key in per_chunk_last and b < per_chunk_last[key]:
            print(f"    FAIL {name}: non-monotonic snapshot seq for c.{cx}.{cz} at {stage}")
            ok = False
        per_chunk_last[key] = b
    print(f"    {name}: {len(snaps)} snapshots, 07 invariant "
          f"{'holds' if ok else 'VIOLATED'}, torn by stage: {torn_by_stage or 'none'}")
    return ok


def prefix(rows, cx, cz):
    """Decoration-order prefix of (cx, cz): chunks within Chebyshev distance
    2 decorated before it, in order. Distance 2 covers reads-of-neighbors'
    -writes transitivity (recon A4: write radius 1, feature reads radius 1)."""
    out = []
    for _seq, x, z, _seed, _t in rows:
        if (x, z) == (cx, cz):
            return tuple(out)
        if max(abs(x - cx), abs(z - cz)) <= 2:
            out.append((x, z))
    raise AssertionError(f"chunk ({cx},{cz}) not in manifest")


def main(a_dir: str, b_dir: str) -> int:
    a, b = Path(a_dir), Path(b_dir)
    ha, ra = read_manifest(a)
    hb, rb = read_manifest(b)
    print(f"A: {a} — {len(ra)} applications, threads {sorted({r[4] for r in ra})}")
    print(f"B: {b} — {len(rb)} applications, threads {sorted({r[4] for r in rb})}")
    for k in ("target_version", "seed", "dimension"):
        assert ha[k] == hb[k], f"header mismatch {k}: {ha[k]} vs {hb[k]}"

    # 1. seeds are order-free; order is not
    seeds_a = {(cx, cz): s for _q, cx, cz, s, _t in ra}
    seeds_b = {(cx, cz): s for _q, cx, cz, s, _t in rb}
    common = sorted(set(seeds_a) & set(seeds_b))
    bad = [c for c in common if seeds_a[c] != seeds_b[c]]
    print(f"\n[1] decoration seeds: {len(common)} common chunks, {len(bad)} seed mismatches")
    if bad:
        for c in bad[:5]:
            print(f"    MISMATCH {c}: {seeds_a[c]} vs {seeds_b[c]}")
        return 1
    order_a = [(cx, cz) for _q, cx, cz, _s, _t in ra]
    order_b = [(cx, cz) for _q, cx, cz, _s, _t in rb]
    if order_a == order_b:
        print("    ORDERS ARE IDENTICAL — bundles do not exercise Tier-2 replay; rerun one")
        return 2
    div = next(i for i, (x, y) in enumerate(zip(order_a, order_b)) if x != y)
    print(f"    orders differ (first divergence at seq {div}: A={order_a[div]} B={order_b[div]})")

    # 1b. snapshot log coherence
    print("\n[1b] order.snapshots coherence")
    if not (check_snapshots("A", a, ra) and check_snapshots("B", b, rb)):
        return 1

    # 2. dump difference <=> order-prefix difference, per grid chunk
    print("\n[2] grid chunks: 07_features dump vs distance-2 order prefix")
    unexplained = 0
    flipped_pairs = []
    for cx, cz in GRID:
        fa = a / f"c.{cx}.{cz}" / "07_features.blocks.txt"
        fb = b / f"c.{cx}.{cz}" / "07_features.blocks.txt"
        dump_same = fa.read_bytes() == fb.read_bytes()
        pa, pb = prefix(ra, cx, cz), prefix(rb, cx, cz)
        pfx_same = pa == pb
        tag = "OK"
        if not dump_same and pfx_same:
            tag = "UNEXPLAINED DIFF"       # difference without an order cause
            unexplained += 1
        elif dump_same and not pfx_same:
            tag = "ok (order diff, same outcome)"  # possible: prefix chunks' spills may not reach
        print(f"    c.{cx}.{cz}: dump {'same' if dump_same else 'DIFF'}, "
              f"prefix {'same' if pfx_same else 'DIFF'} -> {tag}")
        if not dump_same and not pfx_same:
            flipped_pairs.append((cx, cz))
    if unexplained:
        print(f"    FAIL: {unexplained} chunks differ without an order explanation")
        return 1

    # 3. concrete border evidence for one differing chunk
    print("\n[3] concrete spill-over evidence")
    if not flipped_pairs:
        print("    no differing grid chunk — nothing to show (orders may differ only far away)")
        return 0
    for cx, cz in flipped_pairs[:1]:
        ga = read_blocks(a / f"c.{cx}.{cz}" / "07_features.blocks.txt")
        gb = read_blocks(b / f"c.{cx}.{cz}" / "07_features.blocks.txt")
        diffs = [(p, ga[p], gb[p]) for p in ga if ga[p] != gb[p]]
        border = [d for d in diffs if d[0][0] in (0, 15) or d[0][2] in (0, 15)]
        print(f"    c.{cx}.{cz}: {len(diffs)} differing blocks, {len(border)} on chunk borders")
        pos_a = next(i for i, c in enumerate(order_a) if c == (cx, cz))
        pos_b = next(i for i, c in enumerate(order_b) if c == (cx, cz))
        before_a = {c for c in order_a[:pos_a] if max(abs(c[0]-cx), abs(c[1]-cz)) == 1}
        before_b = {c for c in order_b[:pos_b] if max(abs(c[0]-cx), abs(c[1]-cz)) == 1}
        print(f"    neighbors decorated before it in A: {sorted(before_a)}")
        print(f"    neighbors decorated before it in B: {sorted(before_b)}")
        print(f"    flipped neighbors: {sorted(before_a ^ before_b)}")
        for (x, y, z), va, vb in sorted(diffs, key=lambda d: (d[0][1], d[0][2], d[0][0]))[:8]:
            print(f"      local ({x:2},{y:4},{z:2})  world ({cx*16+x:4},{y:4},{cz*16+z:4})  A={va}  B={vb}")
    print("\nPROBE PASSED: bundles coherent with per-chunk-order-only variation")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
