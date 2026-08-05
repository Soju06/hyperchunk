#!/usr/bin/env python3
"""Task 14: blocks.c / hc_blocks.h T14 블록 테이블 생성기.

입력: .hermes/notes/task14-fullregion/R-blockprops.tsv (209 + REVERIFY 4),
      R-blockprops2.tsv (템플릿-전용 33 + 피처 39 + collisionFull 부록 209).
출력: 마커 사이 코드를 재생성 (idempotent):
  - hc_blocks.h: HC_B_T14_COUNT define
  - blocks.c: NAMES 추가분, FLAGS 추가분, t14_damp/emit/occ/cfull 테이블

순서: (1) mca-결손 209 (TSV 순서), (2) 템플릿-전용 33, (3) 피처 39.
bubble_column[drag=true] 는 기존 엔트리의 이름 수정으로 처리돼 생성 목록에서
제외한다 (drag_down → drag 는 별도 수동 수정).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NOTES = ROOT / ".hermes/notes/task14-fullregion"

def parse_tsv(path):
    rows = []
    for line in open(path):
        line = line.rstrip("\n")
        if not line or line.startswith("#") or line.startswith("state\t"):
            continue
        parts = line.split("\t")
        rows.append(parts)
    return rows

def tf(v):
    return v.strip().lower().startswith("t")

def occ_mask(spec):
    spec = spec.strip()
    if spec in ("none", ""):
        return 0
    # "D:f,U:3,N:f,S:3,W:7,E:7" — 면 순서 D,U,N,S,W,E 니블 (LSB=D)
    vals = {}
    for part in spec.split(","):
        k, v = part.split(":")
        vals[k.strip()] = 0xF if v.strip() == "f" else int(v.strip(), 16)
    order = ["D", "U", "N", "S", "W", "E"]
    m = 0
    for i, k in enumerate(order):
        m |= vals[k] << (4 * i)
    return m

def main():
    main_rows = parse_tsv(NOTES / "R-blockprops.tsv")
    rows2_raw = [l.rstrip("\n") for l in open(NOTES / "R-blockprops2.tsv")]

    # R-blockprops.tsv: REVERIFY 행 제외 (state 가 이미 레지스트리에 있는
    # spawner/cobweb/rail/trial_spawner 재검증행 — notes 로 식별 불가하므로
    # 상태명 dedup 로 처리: 첫 등장 우선, 이후 등장 스킵)
    cfull_appendix = {}
    sec = "main"
    rows2 = []
    for l in rows2_raw:
        if l.startswith("# appendix"):
            sec = "appendix"
            continue
        if l.startswith("# features"):
            sec = "features"
            continue
        if not l or l.startswith("#") or l.startswith("state\t"):
            continue
        parts = l.split("\t")
        if sec == "appendix":
            cfull_appendix[parts[0]] = tf(parts[1])
        else:
            rows2.append((sec, parts))

    missing = [l.strip() for l in open(NOTES / "missing_states.txt")
               if l.strip()]
    missing_set = set(missing)

    # main TSV 를 상태명으로 인덱스
    by_state = {}
    for p in main_rows:
        if p[0] not in by_state:
            by_state[p[0]] = p

    entries = []  # (state, bm, ls, fo, repl, em, lb, occ, cfull)
    def add(state, p, cfull):
        bm, ls, fo, repl = tf(p[1]), tf(p[2]), tf(p[3]), tf(p[4])
        em, lb = int(p[5]), int(p[6])
        occ = occ_mask(p[7]) if len(p) > 7 else 0
        entries.append((state, bm, ls, fo, repl, em, lb, occ, cfull))

    skipped = []
    for s in missing:
        if s == "minecraft:bubble_column[drag=true]":
            skipped.append(s)  # 기존 엔트리 이름 수정으로 커버
            continue
        p = by_state.get(s)
        if not p:
            sys.exit(f"missing TSV row for {s}")
        if s not in cfull_appendix:
            sys.exit(f"missing collisionFull appendix for {s}")
        add(s, p, cfull_appendix[s])

    added = {e[0] for e in entries}
    for sec, p in rows2:
        # rows2 는 9컬럼 + collisionFull(맨끝)
        if p[0] in added:
            continue  # mca-결손 209 와 겹치는 피처 상태 (leaf_litter 4종)
        cfull = tf(p[-1])
        add(p[0], p, cfull)
        added.add(p[0])

    # Task 14 배치 엔진 과도기 상태 (gen_t14_states3.py 산출)
    tsv3 = NOTES / "R-blockprops3.tsv"
    if tsv3.exists():
        for p in parse_tsv(tsv3):
            if p[0] in added:
                continue
            add(p[0], p, tf(p[-1]))
            added.add(p[0])

    # trial_chambers (13,35) 마진 배치 회전 산물 (StatePropsProbe 실측)
    tsv4 = NOTES / "R-blockprops4.tsv"
    if tsv4.exists():
        for p in parse_tsv(tsv4):
            if p[0] in added:
                continue
            add(p[0], p, tf(p[-1]))
            added.add(p[0])

    print(f"{len(entries)} entries ({len(skipped)} skipped: {skipped})")

    # 중복/등록 충돌 검사 — 생성 구간 이전 (수동 등록분) 만 본다
    src = open(ROOT / "core/src/blocks.c").read()
    start = src.index("NAMES[HC_B_COUNT] = {")
    end = src.index("T14-GEN BEGIN names", start)
    existing = set(re.findall(r'"(minecraft:[^"]+)"', src[start:end]))
    seen = set()
    for e in entries:
        if e[0] in existing:
            sys.exit(f"already registered: {e[0]}")
        if e[0] in seen:
            sys.exit(f"duplicate: {e[0]}")
        seen.add(e[0])

    # --- 프래그먼트 생성 ---
    names_frag = []
    flags_frag = []
    damp_frag, emit_frag, occ_frag, cfull_frag = [], [], [], []
    for (state, bm, ls, fo, repl, em, lb, occ, cfull) in entries:
        wl = "waterlogged=true" in state
        f = []
        if bm:
            f.append("F_MOTION")
        if ls:
            f.append("F_SOLID")
        if fo:
            f.append("F_FULL")
        if ("_leaves[" in state) and ("persistent" in state):
            f.append("F_LEAVES")
        if repl:
            f.append("F_REPL")
        if wl:
            f.append("F_WLOG")
        names_frag.append(f'    "{state}",')
        flags_frag.append(f'    {"|".join(f) if f else "0"},')
        damp_frag.append(str(lb))
        emit_frag.append(str(em))
        occ_frag.append(f"0x{occ:06x}" if occ else "0")
        cfull_frag.append("1" if cfull else "0")

    def pack(vals, per=12):
        out = []
        for i in range(0, len(vals), per):
            out.append("    " + ", ".join(vals[i:i + per]) + ",")
        return "\n".join(out)

    def splice(path, marker, body):
        p = ROOT / path
        text = open(p).read()
        begin = f"/* T14-GEN BEGIN {marker} */"
        endm = f"/* T14-GEN END {marker} */"
        i = text.index(begin) + len(begin)
        j = text.index(endm)
        open(p, "w").write(text[:i] + "\n" + body + "\n" + text[j:])

    splice("core/src/hc_blocks.h", "count",
           f"#define HC_B_T14_COUNT {len(entries)}")
    splice("core/src/blocks.c", "names", "\n".join(names_frag))
    splice("core/src/blocks.c", "flags", "\n".join(flags_frag))
    splice("core/src/blocks.c", "damp", pack(damp_frag))
    splice("core/src/blocks.c", "emit", pack(emit_frag))
    splice("core/src/blocks.c", "occ", pack(occ_frag, 6))
    splice("core/src/blocks.c", "cfull", pack(cfull_frag, 24))
    print("spliced OK")

if __name__ == "__main__":
    main()
