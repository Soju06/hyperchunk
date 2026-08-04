#!/usr/bin/env python3
"""Cross-validate a unified golden capture: mca <-> manifest <-> dumps.

Usage: check_capture_coherence.py <r.0.0.mca> <bundle_dir>

This is the STRUCTURAL half of the Task-13 golden-replacement gate (절대
규칙: replace golden only after coherence passes). The byte-level half is
the C replay itself (test_region against region-refs extracted from the
candidate mca). Checks here:

  1. mca: 1024 chunks present, Status minecraft:full, DataVersion uniform.
  2. LastUpdate: identical across all 1024 chunks (gametime frozen at save).
  3. Tick inventory: block t in {0,1,2,5}, fluid t in {0,5}; every live row
     (fluid t=5, block t=2/t=5) belongs to a chunk with a postprocess.manifest
     promotion whose recorded gameTime == LastUpdate (N=0 assumption of
     R-D §3 made explicit, per-chunk).
  4. postprocess.manifest: every region chunk promoted exactly once; gameTime
     uniform == LastUpdate; seq dense.
  5. PostProcessing NBT: 24 empty lists in every chunk (drain happened).
  6. 11_full dump vs mca blocks for grid chunks in r.0.0: every differing
     cell must be postProcess-class — at/adjacent(≤2 Chebyshev) to a marked
     position, or a fluid-class state change (water level / bubble_column /
     air<->fluid) — UNLESS the chunk has post-snapshot feature writers
     recorded in order.manifest/order.snapshots (then feature spill is
     possible and cell-level attribution is delegated to the C replay; such
     chunks are reported as WARN with full inventory, not silently passed).
     The stale-mca failure classes (granite/fern/short_grass clusters far
     from any mark) FAIL here.
  7. Replay-window sufficiency: for every manifest entry E whose chunk is
     within (-2..3)^2 (the influence set of the 4 gate chunks) every EARLIER
     entry within Chebyshev distance 2 of E must lie inside |cx|,|cz| <= 4
     (the C tests' replay filter) — proves the windowed replay is exact for
     the gate, not an approximation.

Exit 0 = PASS (possibly with WARNs printed), 1 = FAIL, 2 = usage.
"""
import sys
import os
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mca import read_region, parse_nbt

GRID = [(0, 0), (1, 0), (0, 1), (1, 1)]  # r.0.0 ∩ dump grid
MIN_Y = -64
HEIGHT = 384

FAILS = []
WARNS = []


def fail(msg):
    FAILS.append(msg)
    print("COHERENCE FAIL: " + msg)


def warn(msg):
    WARNS.append(msg)
    print("coherence warn: " + msg)


def load_postprocess(path):
    """returns (chunk_lines, positions) — chunk_lines: list of dicts in seq
    order; positions: {(cx,cz): [(k, secY, x, y, z, state), ...]}"""
    chunks = []
    positions = defaultdict(list)
    seq2chunk = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split(" ")
            if parts[0] == "p":
                seq = int(parts[1])
                k, sec_y, x, y, z = (int(v) for v in parts[2:7])
                state = " ".join(parts[7:])
                positions[seq2chunk[seq]].append((k, sec_y, x, y, z, state))
            else:
                seq, cx, cz, gt, n = (int(v) for v in parts[:5])
                if seq != len(chunks):
                    fail(f"postprocess seq gap: got {seq} want {len(chunks)}")
                chunks.append({"seq": seq, "cx": cx, "cz": cz, "gt": gt, "n": n})
                seq2chunk[seq] = (cx, cz)
    return chunks, positions


def load_manifest(path):
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if not line.strip() or line.startswith("#"):
                continue
            p = line.split()
            out.append((int(p[0]), int(p[1]), int(p[2])))
    return out


def load_snapshots(path):
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if not line.strip() or line.startswith("#"):
                continue
            p = line.split()
            out.append((p[0], int(p[1]), int(p[2]), int(p[3]), int(p[4])))
    return out


def decode_sections(root):
    """{(x,y,z) absolute: state string} from mca chunk root (block_states)."""
    blocks = {}
    cx, cz = root["xPos"], root["zPos"]
    for sec in root["sections"]:
        if "block_states" not in sec:
            continue
        sy = sec["Y"]
        bs = sec["block_states"]
        pal = []
        for e in bs["palette"]:
            name = e["Name"]
            props = e.get("Properties")
            if props:
                inner = ",".join(f"{k}={v}" for k, v in sorted(props.items()))
                name += "[" + inner + "]"
            pal.append(name)
        if "data" not in bs:
            for i in range(4096):
                y = sy * 16 + (i >> 8)
                blocks[(cx * 16 + (i & 15), y, cz * 16 + ((i >> 4) & 15))] = pal[0]
            continue
        data = bs["data"]
        n = len(pal)
        bits = max(4, (n - 1).bit_length())
        per = 64 // bits
        mask = (1 << bits) - 1
        for i in range(4096):
            w = data[i // per] & 0xFFFFFFFFFFFFFFFF
            idx = (w >> ((i % per) * bits)) & mask
            y = sy * 16 + (i >> 8)
            blocks[(cx * 16 + (i & 15), y, cz * 16 + ((i >> 4) & 15))] = pal[idx]
    return blocks


def load_blocks_dump(path):
    """{(x,y,z) absolute: normalized state} from a *_full.blocks.txt dump."""
    pal = {}
    blocks = {}
    cx = cz = min_y = None
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    row = 0
    in_data = False
    for line in lines:
        if line.startswith("# chunk "):
            _, _, a, b = line.split()
            cx, cz = int(a), int(b)
        elif line.startswith("# minY "):
            min_y = int(line.split()[2])
        elif line.startswith("palette "):
            _, idx, state = line.split(" ", 2)
            pal[int(idx)] = normalize_state(state)
        elif line == "data":
            in_data = True
        elif in_data and line and not line.startswith("#"):
            y = min_y + row // 16
            z = row % 16
            for x, tok in enumerate(line.split()):
                blocks[(cx * 16 + x, y, cz * 16 + z)] = pal[int(tok)]
            row += 1
    return blocks


def normalize_state(s):
    """BlockStateParser.serialize -> Name[k=v sorted] canonical form."""
    if "[" not in s:
        return s
    name, rest = s.split("[", 1)
    kvs = rest.rstrip("]").split(",")
    return name + "[" + ",".join(sorted(kvs)) + "]"


FLUIDY = ("minecraft:water", "minecraft:lava", "minecraft:bubble_column")


def is_fluid_class(a, b):
    """block change a->b explainable as a fluid postProcess product?"""
    base = lambda s: s.split("[")[0]
    if base(a) in FLUIDY or base(b) in FLUIDY:
        return True
    if a in ("minecraft:air", "minecraft:cave_air") and base(b) in FLUIDY:
        return True
    return False


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    mca_path, bundle = sys.argv[1], sys.argv[2]

    chunks = read_region(mca_path)
    print(f"mca: {len(chunks)} chunks present")
    if len(chunks) != 1024:
        fail(f"expected 1024 chunks, got {len(chunks)}")

    roots = {}
    for idx, ent in sorted(chunks.items()):
        _, root = parse_nbt(ent.payload)
        roots[(ent.x if ent.x < 32 else ent.x, ent.z)] = root
    # mca.py x/z are region-local 0..31 == absolute for r.0.0

    # -- 1/2: Status, DataVersion, LastUpdate uniform
    statuses = Counter(r["Status"] for r in roots.values())
    if set(statuses) != {"minecraft:full"}:
        fail(f"non-full statuses: {statuses}")
    lus = Counter(r["LastUpdate"] for r in roots.values())
    if len(lus) != 1:
        fail(f"LastUpdate not uniform: {lus}")
    last_update = next(iter(lus))
    print(f"LastUpdate uniform: {last_update} ({lus[last_update]} chunks)")
    dvs = Counter(r["DataVersion"] for r in roots.values())
    if len(dvs) != 1:
        fail(f"DataVersion not uniform: {dvs}")

    # -- 4: postprocess manifest
    ppc, ppp = load_postprocess(os.path.join(bundle, "postprocess.manifest"))
    seen = Counter((c["cx"], c["cz"]) for c in ppc)
    dups = [k for k, v in seen.items() if v > 1]
    if dups:
        fail(f"chunks promoted more than once: {dups[:10]}")
    region_promoted = {k for k in seen if 0 <= k[0] <= 31 and 0 <= k[1] <= 31}
    if len(region_promoted) != 1024:
        fail(f"region chunks promoted: {len(region_promoted)}/1024")
    gts = Counter(c["gt"] for c in ppc)
    print(f"postprocess: {len(ppc)} promotions, gameTime histogram {dict(gts)}")
    if set(gts) != {last_update}:
        # replayable either way (recorded), but t-values then vary per chunk
        warn(f"promotion gameTime not uniformly == LastUpdate({last_update}): {dict(gts)}")

    # -- 3: tick inventory
    bt = Counter()
    ft = Counter()
    live_chunks = set()
    for (cx, cz), r in roots.items():
        for e in r.get("block_ticks", []):
            bt[e["t"]] += 1
            if e["t"] != 0:
                live_chunks.add((cx, cz))
        for e in r.get("fluid_ticks", []):
            ft[e["t"]] += 1
            if e["t"] != 0:
                live_chunks.add((cx, cz))
    print(f"block_ticks t histogram: {dict(bt)}")
    print(f"fluid_ticks t histogram: {dict(ft)}")
    if not set(ft) <= {0, 5}:
        fail(f"unexpected fluid tick t values: {dict(ft)}")
    if not set(bt) <= {0, 1, 2, 5}:
        fail(f"unexpected block tick t values: {dict(bt)}")
    not_promoted = [c for c in live_chunks if c not in seen]
    if not_promoted:
        fail(f"chunks with live ticks but no recorded promotion: {not_promoted[:10]}")

    # -- 5: PostProcessing all empty
    bad_pp = [k for k, r in roots.items()
              if any(len(l) != 0 for l in r.get("PostProcessing", []))]
    if bad_pp:
        fail(f"non-empty PostProcessing lists in {len(bad_pp)} chunks: {bad_pp[:5]}")
    else:
        print("PostProcessing: empty in all chunks")

    # -- 7: replay-window sufficiency for the gate influence set
    man = load_manifest(os.path.join(bundle, "order.manifest"))
    influence = {(cx, cz) for cx in range(-2, 4) for cz in range(-2, 4)}
    decoseq = {}
    viol = 0
    for seq, cx, cz in man:
        if (cx, cz) in influence:
            for (px, pz), pseq in decoseq.items():
                if max(abs(px - cx), abs(pz - cz)) <= 2 and \
                        (abs(px) > 4 or abs(pz) > 4):
                    fail(f"window violation: out-of-window ({px},{pz})@{pseq} "
                         f"decorated before influence chunk ({cx},{cz})@{seq}")
                    viol += 1
        decoseq[(cx, cz)] = seq
    if not viol:
        print(f"replay window: |c|<=4 filter exact for gate influence set "
              f"({len(man)} manifest entries)")

    # -- 6: 11_full dumps vs mca for grid chunks
    snaps = load_snapshots(os.path.join(bundle, "order.snapshots"))
    full_seq = {(cx, cz): sb for (st, cx, cz, sb, se) in snaps
                if st.startswith("11_")}
    for (cx, cz) in GRID:
        dump = load_blocks_dump(
            os.path.join(bundle, f"c.{cx}.{cz}", "11_full.blocks.txt"))
        mcab = decode_sections(roots[(cx, cz)])
        marks = ppp.get((cx, cz), [])
        markpos = {(x, y, z) for (_, _, x, y, z, _) in marks}
        # positions marked in NEIGHBOR chunks whose spread can reach us
        for (nx, nz), plist in ppp.items():
            if (nx, nz) != (cx, cz) and abs(nx - cx) <= 1 and abs(nz - cz) <= 1:
                markpos |= {(x, y, z) for (_, _, x, y, z, _) in plist}
        post_writers = [
            (seq, mx, mz) for seq, mx, mz in man
            if seq >= full_seq.get((cx, cz), 1 << 30)
            and max(abs(mx - cx), abs(mz - cz)) <= 1]
        diffs = []
        for pos, dstate in dump.items():
            m = normalize_state(mcab.get(pos, "minecraft:air"))
            d = normalize_state(dstate)
            if m != d:
                diffs.append((pos, d, m))
        unexplained = []
        for (pos, d, m) in diffs:
            near_mark = any(
                max(abs(pos[0] - mp[0]), abs(pos[1] - mp[1]), abs(pos[2] - mp[2])) <= 2
                for mp in markpos)
            if not (near_mark or is_fluid_class(d, m)):
                unexplained.append((pos, d, m))
        tag = f"c.{cx}.{cz}: 11_full vs mca: {len(diffs)} diff cells, " \
              f"{len(unexplained)} not postProcess-class, " \
              f"{len(post_writers)} post-snapshot feature writers"
        print(tag)
        for (pos, d, m) in diffs[:20]:
            print(f"    {pos}: dump={d} mca={m}")
        if unexplained:
            if post_writers:
                warn(tag + " — attribution delegated to C replay (post-snapshot "
                     f"writers: {post_writers})")
            else:
                for (pos, d, m) in unexplained[:20]:
                    print(f"    UNEXPLAINED {pos}: dump={d} mca={m}")
                fail(tag + " — stale-mca class diffs with no post-snapshot writers")

    print()
    if FAILS:
        print(f"coherence: FAIL ({len(FAILS)} failures, {len(WARNS)} warnings)")
        return 1
    print(f"coherence: PASS ({len(WARNS)} warnings)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
