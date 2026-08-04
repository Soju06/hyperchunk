#!/usr/bin/env python3
"""Task 14: 템플릿 배치가 요구하는 추가 상태 열거 → R-blockprops3.tsv.

배경: T14 테이블 (tsv1 209 + tsv2 33+39) 은 mca-결손 + 일부 템플릿 상태만
커버한다. 배치 엔진은 그 밖에 (a) 로드시 선택-팔레트 전체 상태, (b) 프로세서
출력 (BlockAge/copper/AppendLoot), (c) 폴드/워터로깅 과도기 상태 (stairs
shape 재계산 전, waterlogged=false 원상태 등), (d) 마커/합성 상태 (barrier,
마커 체스트) 를 해석해야 한다. 지속(persist) 상태는 mca 팔레트에 있어 이미
등록됐고, 여기서 열거하는 것은 과도기·미지속 상태의 안전 초집합이다.

프로퍼티 유도: 과도기 상태의 light damp/emit/occ 는 라이트 스테이지가 절대
읽지 않으므로 (지속 상태만 라이트에 도달; 지속 상태 = 기등록) 패밀리 규칙
값으로 충분하다. 관측가능 값은 FLAGS(모션/유체/솔리드)와 collisionFull
(버킷 분류) — 패밀리 규칙으로 정확 유도. 미지 패밀리는 tsv1/2 의 같은
베이스 형제 행에서 복사, 그마저 없으면 실패 목록 출력.
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NOTES = ROOT / ".hermes/notes/task14-fullregion"


def parse_nbt(data):
    pos = [0]

    def u8():
        v = data[pos[0]]
        pos[0] += 1
        return v

    def rstr():
        n = struct.unpack_from(">H", data, pos[0])[0]
        pos[0] += 2
        s = data[pos[0]:pos[0] + n].decode()
        pos[0] += n
        return s

    def pl(t):
        if t == 1:
            return u8()
        if t == 2:
            v = struct.unpack_from(">h", data, pos[0])[0]
            pos[0] += 2
            return v
        if t == 3:
            v = struct.unpack_from(">i", data, pos[0])[0]
            pos[0] += 4
            return v
        if t == 4:
            v = struct.unpack_from(">q", data, pos[0])[0]
            pos[0] += 8
            return v
        if t == 5:
            v = struct.unpack_from(">f", data, pos[0])[0]
            pos[0] += 4
            return v
        if t == 6:
            v = struct.unpack_from(">d", data, pos[0])[0]
            pos[0] += 8
            return v
        if t == 7:
            n = pl(3)
            v = data[pos[0]:pos[0] + n]
            pos[0] += n
            return v
        if t == 8:
            return rstr()
        if t == 9:
            et = u8()
            n = pl(3)
            return [pl(et) for _ in range(n)]
        if t == 10:
            d = {}
            while True:
                et = u8()
                if et == 0:
                    break
                k = rstr()
                d[k] = pl(et)
            return d
        if t == 11:
            n = pl(3)
            return [pl(3) for _ in range(n)]
        if t == 12:
            n = pl(3)
            return [pl(4) for _ in range(n)]
        raise Exception(t)

    t = u8()
    assert t == 10
    rstr()
    return pl(10)


def canon(name, props):
    if not props:
        return name
    return name + "[" + ",".join(
        f"{k}={v}" for k, v in sorted(props.items())) + "]"


def parse_state(s):
    if "[" not in s:
        return s, {}
    base, rest = s.split("[", 1)
    props = {}
    for kv in rest.rstrip("]").split(","):
        k, v = kv.split("=")
        props[k] = v
    return base, props


def main():
    # 1. 골든 starts → 사용 템플릿 집합 (+TSP 여부)
    tsp_templates = set()   # shipwreck/warm_3/portal_3
    all_templates = set()
    for frag in (ROOT / "golden/structures").glob("c.*.starts.nbt"):
        root = parse_nbt(open(frag, "rb").read())
        for sname, tag in root.items():
            for ch in tag["Children"]:
                if "Template" in ch:
                    tsp_templates.add(ch["Template"])
                    all_templates.add(ch["Template"])
                elif "pool_element" in ch:
                    all_templates.add(ch["pool_element"]["location"])

    # 2. 팔레트 상태 수집 (shipwreck: 실측 선택 팔레트 5 — LCG(getSeed(
    #    144,58,80)).nextInt(8); 그 외 단일)
    states = set()
    tsp_states = set()
    for tn in sorted(all_templates):
        path = ROOT / "reference/structure" / (tn.split(":", 1)[1] + ".nbt")
        root = parse_nbt(open(path, "rb").read())
        pals = root.get("palettes")
        if pals:
            assert "shipwreck" in tn, tn
            pal = pals[5]
        else:
            pal = root["palette"]
        for e in pal:
            s = canon(e["Name"], e.get("Properties", {}))
            states.add(s)
            if tn in tsp_templates:
                tsp_states.add(s)
        # jigsaw final_state 도 수집 (부분 지정 → 디폴트 병합)
        for b in root["blocks"]:
            nbt = b.get("nbt")
            if nbt is None:
                continue
            nm = pal[b.get("state", 0)]["Name"]
            if nm == "minecraft:jigsaw" and "final_state" in nbt:
                fs = nbt["final_state"]
                if fs == "minecraft:structure_void":
                    continue
                base, props = parse_state(fs)
                DEF = {
                    "minecraft:waxed_copper_bulb":
                        {"lit": "false", "powered": "false"},
                    "minecraft:waxed_oxidized_copper_grate":
                        {"waterlogged": "false"},
                    "minecraft:tripwire": {
                        "attached": "false", "disarmed": "false",
                        "east": "false", "north": "false",
                        "powered": "false", "south": "false",
                        "west": "false"},
                }
                merged = dict(DEF.get(base, {}))
                merged.update(props)
                states.add(canon(base, merged))

    # 3. 프로세서/마커/합성 산출
    for f in ("north", "east", "south", "west"):
        for h in ("top", "bottom"):
            for sh in ("straight", "inner_left", "inner_right",
                       "outer_left", "outer_right"):
                for wl in ("false", "true"):
                    for base in ("minecraft:stone_brick_stairs",
                                 "minecraft:mossy_stone_brick_stairs"):
                        states.add(
                            f"{base}[facing={f},half={h},shape={sh},"
                            f"waterlogged={wl}]")
    for wl in ("false", "true"):
        for base in ("minecraft:stone_slab", "minecraft:stone_brick_slab",
                     "minecraft:mossy_stone_brick_slab"):
            states.add(f"{base}[type=bottom,waterlogged={wl}]")
    states.update([
        "minecraft:cracked_stone_bricks", "minecraft:mossy_stone_bricks",
        "minecraft:crying_obsidian", "minecraft:barrier",
        "minecraft:suspicious_sand[dusted=0]",
        "minecraft:chest[facing=north,type=single,waterlogged=false]",
        "minecraft:chest[facing=north,type=single,waterlogged=true]",
        "minecraft:waxed_oxidized_copper_bulb[lit=true,powered=false]",
        "minecraft:waxed_weathered_copper_bulb[lit=true,powered=false]",
        "minecraft:waxed_exposed_copper_bulb[lit=true,powered=false]",
    ])

    # 4. TSP 클로저 (폴드/워터로깅 과도기): stairs shape / fence 방향 /
    #    door half / chest type / waterlogged 변형
    add = set()
    for s in list(tsp_states):
        base, props = parse_state(s)
        if base.endswith("_stairs"):
            for f in ("north", "east", "south", "west"):
                for h in ("top", "bottom"):
                    for sh in ("straight", "inner_left", "inner_right",
                               "outer_left", "outer_right"):
                        for wl in ("false", "true"):
                            add.add(f"{base}[facing={f},half={h},"
                                    f"shape={sh},waterlogged={wl}]")
        elif base.endswith("_fence"):
            for e_ in ("false", "true"):
                for n_ in ("false", "true"):
                    for s_ in ("false", "true"):
                        for w_ in ("false", "true"):
                            for wl in ("false", "true"):
                                add.add(f"{base}[east={e_},north={n_},"
                                        f"south={s_},waterlogged={wl},"
                                        f"west={w_}]")
        elif base.endswith("_door") and not base.endswith("_trapdoor"):
            for half in ("lower", "upper"):
                p2 = dict(props)
                p2["half"] = half
                add.add(canon(base, p2))
        elif base.endswith("chest"):
            for ty in ("single", "left", "right"):
                for wl in ("false", "true"):
                    p2 = dict(props)
                    p2["type"] = ty
                    p2["waterlogged"] = wl
                    add.add(canon(base, p2))
        if "waterlogged" in props:
            for wl in ("false", "true"):
                p2 = dict(props)
                p2["waterlogged"] = wl
                add.add(canon(base, p2))
    states.update(add)

    # 5. 기등록 (blocks.c NAMES 전체) 차감
    src = open(ROOT / "core/src/blocks.c").read()
    start = src.index("NAMES[HC_B_COUNT] = {")
    end = src.index("/* T14-GEN END names */", start)
    registered = set(re.findall(r'"(minecraft:[^"]+)"', src[start:end]))
    missing = sorted(s for s in states if s not in registered)

    # 6. 프로퍼티 유도
    def sib_rows():
        rows = {}
        for tsv in ("R-blockprops.tsv", "R-blockprops2.tsv"):
            for line in open(NOTES / tsv):
                line = line.rstrip("\n")
                if not line or line.startswith("#") or \
                   line.startswith("state\t"):
                    continue
                parts = line.split("\t")
                b, _ = parse_state(parts[0])
                rows.setdefault(b, parts)
        return rows

    sibs = sib_rows()
    out = []
    unresolved = []
    for s in missing:
        base, props = parse_state(s)
        wl = props.get("waterlogged") == "true"
        note = "transient/family-derived (gen_t14_states3)"
        if base.endswith(("_planks", "_log", "_wood", "_bricks", "_block")) \
           and False:
            pass  # 명시 목록으로만 (아래)
        if base.endswith("_stairs"):
            row = (s, "t", "t", "f", "f", "0", "0", "none", note, "f")
        elif base.endswith("_slab") and props.get("type") != "double":
            row = (s, "t", "t", "f", "f", "0", "0", "none", note, "f")
        elif base.endswith("_fence"):
            row = (s, "t", "t", "f", "f", "0", "0", "none", note, "f")
        elif base.endswith("_trapdoor"):
            row = (s, "t", "t", "f", "f", "0", "0", "none", note, "f")
        elif base.endswith("_door"):
            row = (s, "t", "t", "f", "f", "0", "0", "none", note, "f")
        elif base.endswith("chest"):
            row = (s, "t", "t", "f", "f", "0", "0", "none", note, "f")
        elif base.endswith("_wall"):
            row = (s, "t", "t", "f", "f", "0", "0", "none", note, "f")
        elif base == "minecraft:barrier":
            # Barrier: noOcclusion 풀큐브 (collision full, fullOcclude f)
            row = (s, "t", "t", "f", "f", "0", "0", "none",
                   "barrier: noOcclusion full-collision cube", "t")
        elif base == "minecraft:tripwire":
            row = (s, "f", "f", "f", "f", "0", "0", "none",
                   "tripwire: noCollision thin", "f")
        elif base.endswith("copper_bulb"):
            lit = props.get("lit") == "true"
            em = {"minecraft:waxed_copper_bulb": 15,
                  "minecraft:waxed_exposed_copper_bulb": 12,
                  "minecraft:waxed_weathered_copper_bulb": 8,
                  "minecraft:waxed_oxidized_copper_bulb": 4}.get(base, 0)
            row = (s, "t", "t", "t", "f", str(em if lit else 0), "15",
                   "none", "copper bulb full cube; lit emission", "t")
        elif base == "minecraft:jigsaw":
            row = (s, "t", "t", "t", "f", "0", "15", "none",
                   "jigsaw full cube", "t")
        elif base in ("minecraft:cracked_stone_bricks",
                      "minecraft:mossy_stone_bricks",
                      "minecraft:crying_obsidian",
                      "minecraft:stone_bricks", "minecraft:obsidian",
                      "minecraft:netherrack"):
            em = "10" if base == "minecraft:crying_obsidian" else "0"
            row = (s, "t", "t", "t", "f", em, "15", "none",
                   "full cube", "t")
        elif base in sibs:
            p = sibs[base]
            row = (s, p[1], p[2], p[3], p[4], p[5], p[6], "none",
                   f"sibling-derived from {p[0]}", "f" if len(p) < 10
                   else p[9])
            # collisionFull: tsv1 형제는 부록에 있었음 — 보수적으로
            # fullOcclude 와 동일시 (풀큐브 형제만 t)
            row = (s, p[1], p[2], p[3], p[4], p[5], p[6], "none",
                   f"sibling-derived from {p[0]}",
                   "t" if p[3].strip().lower().startswith("t") else "f")
        else:
            unresolved.append(s)
            continue
        if wl and row[1] != "" :
            pass  # F_WLOG 는 생성기가 상태명으로 판단
        out.append(row)

    if unresolved:
        print("UNRESOLVED (need manual rows):")
        for s in unresolved:
            print("  ", s)
        sys.exit(1)

    with open(NOTES / "R-blockprops3.tsv", "w") as f:
        f.write("state\tblocksMotion\tlegacySolid\tfullOcclude\t"
                "replaceable\temission\tlightBlock\tshapeOcclusion\tnotes\t"
                "collisionFull\n")
        for row in out:
            f.write("\t".join(row) + "\n")
    print(f"{len(out)} rows -> R-blockprops3.tsv "
          f"({len(missing)} missing of {len(states)} enumerated)")


if __name__ == "__main__":
    main()
