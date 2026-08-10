#!/usr/bin/env python3
"""B-2 워터폴 분석기 — hyperchunk-bench 의 HC_BENCH_TIMELINE 덤프(v1)를 읽어
스레드 점유 타임라인 / 유휴 분해 / DAG 크리티컬 패스 / 체인-DAG 융합
시뮬레이션을 낸다. stdlib 전용, 분석 전용 (벤치 수치에 관여하지 않음).

  python3 bench/waterfall.py <timeline.txt> [--ascii] [--fuse]

포맷 v1 (hyperchunk_bench.c wf_dump 참조):
  M <name> <ns> / C <i> <cx> <cz> <tid> <w0..w5>
  E <idx> <kind> <cx> <cz> <worker> <t0> <t1> / D <idx> <n> <cell...>
  S <idx> <worker> <t0> <t1>
"""
import sys
from collections import defaultdict

KIND = {0: "deco", 1: "l08", 2: "l09", 3: "prepare"}
CHAIN_STAGES = ["nc_init", "beard", "noise", "surface", "carvers"]


def load(path):
    marks, chain, evs, deps, ser = {}, [], [], {}, []
    nthreads = 20
    for line in open(path):
        f = line.split()
        if not f:
            continue
        if line.startswith("#"):
            for tok in f:
                if tok.startswith("threads="):
                    nthreads = int(tok.split("=")[1])
            continue
        if f[0] == "M":
            marks[f[1]] = int(f[2])
        elif f[0] == "C":
            chain.append({"i": int(f[1]), "cx": int(f[2]), "cz": int(f[3]),
                          "tid": int(f[4]), "w": [int(x) for x in f[5:11]]})
        elif f[0] == "E":
            evs.append({"idx": int(f[1]), "kind": int(f[2]), "cx": int(f[3]),
                        "cz": int(f[4]), "worker": int(f[5]),
                        "t0": int(f[6]), "t1": int(f[7])})
        elif f[0] == "D":
            deps[int(f[1])] = [int(x) for x in f[3:]]
        elif f[0] == "S":
            ser.append({"idx": int(f[1]), "worker": int(f[2]),
                        "t0": int(f[3]), "t1": int(f[4])})
    return marks, chain, evs, deps, ser, nthreads


def ms(ns):
    return ns / 1e6


def ascii_timeline(rows, t0, t1, nthreads, width=100, title=""):
    """rows: worker -> list of (start, end). 문자 = 버킷 점유율."""
    span = t1 - t0
    if span <= 0:
        return
    print(f"  -- {title} ({ms(span):.1f} ms, {width} cols x "
          f"{ms(span) / width:.2f} ms) --")
    for w in range(nthreads):
        buckets = [0.0] * width
        for (s, e) in rows.get(w, []):
            b0 = max(0.0, (s - t0) / span * width)
            b1 = min(float(width), (e - t0) / span * width)
            ib = int(b0)
            while ib < b1 and ib < width:
                seg = min(ib + 1, b1) - max(ib, b0)
                buckets[ib] += max(0.0, seg)
                ib += 1
        line = "".join(
            "#" if b > 0.90 else "+" if b > 0.5 else "." if b > 0.05 else " "
            for b in buckets)
        print(f"  w{w:02d} |{line}|")


def dag_edges(evs, deps):
    """셀 FIFO(셀별 base 순서 연속쌍) + 배리어 의존을 엣지로 재구성."""
    cell_seq = defaultdict(list)
    for e in evs:
        for c in deps[e["idx"]]:
            cell_seq[c].append(e["idx"])
    edges = defaultdict(set)  # idx -> set of predecessor idx
    for c, seq in cell_seq.items():
        for a, b in zip(seq, seq[1:]):
            edges[b].add(a)
    barriers = [e["idx"] for e in evs if e["kind"] == 3]
    return edges, barriers


def critical_path(evs, edges, barriers):
    """실측 duration 가중 최장 경로 (배리어 = 세그먼트 분할).
    배리어는 앞 전부-완료 + 뒤 전부-차단이라, 구조적 CP = 세그먼트별
    최장 체인의 합 + 배리어 자신."""
    dur = {e["idx"]: e["t1"] - e["t0"] for e in evs}
    n = max(dur) + 1
    seg_of = [0] * n
    s = 0
    bset = set(barriers)
    for i in range(n):
        if i in bset:
            s += 1
            seg_of[i] = -1  # 배리어 자신
        else:
            seg_of[i] = s
    cp = {}  # idx -> (len, path_tail)
    order = sorted(dur)  # base 순서 = 위상 순서 (엣지는 항상 앞→뒤)
    best_per_seg = defaultdict(lambda: (0, None))
    for i in order:
        if i in bset:
            continue
        base = 0
        arg = None
        for p in edges.get(i, ()):  # 셀 FIFO 선행자
            if p in bset:
                continue
            if seg_of[p] != seg_of[i]:
                continue  # 세그먼트 밖은 배리어 합산에서 처리
            if cp[p][0] > base:
                base, arg = cp[p][0], p
        cp[i] = (base + dur[i], arg)
        if cp[i][0] > best_per_seg[seg_of[i]][0]:
            best_per_seg[seg_of[i]] = (cp[i][0], i)
    total = sum(v[0] for v in best_per_seg.values()) + \
        sum(dur[b] for b in barriers)
    return total, best_per_seg, cp, dur, seg_of


def list_schedule(tasks, preds, nworkers, priority):
    """리스트 스케줄링 (greedy, ready 중 priority 최소 우선). tasks: id->dur.
    preds: id -> iterable of id. 태스크는 완료 인스턴트에만 ready 로 풀리므로
    배정은 t=0 과 완료 시점에서만 일어난다. 반환: makespan, finish dict."""
    import heapq
    succ = defaultdict(list)
    indeg = {t: 0 for t in tasks}
    for t, ps in preds.items():
        for p in ps:
            succ[p].append(t)
            indeg[t] += 1
    ready = [(priority[t], t) for t in tasks if indeg[t] == 0]
    heapq.heapify(ready)
    running = []  # (finish_time, task)
    finish = {}
    free = nworkers
    now = 0.0
    done = 0
    while done < len(tasks):
        while ready and free > 0:
            _, t = heapq.heappop(ready)
            heapq.heappush(running, (now + tasks[t], t))
            free -= 1
        now, t = heapq.heappop(running)
        batch = [t]
        while running and running[0][0] <= now:
            batch.append(heapq.heappop(running)[1])
        for t in batch:
            finish[t] = now
            free += 1
            done += 1
            for s in succ[t]:
                indeg[s] -= 1
                if indeg[s] == 0:
                    heapq.heappush(ready, (priority[s], s))
    return max(finish.values()), finish


def main():
    path = sys.argv[1]
    do_ascii = "--ascii" in sys.argv
    do_fuse = "--fuse" in sys.argv
    marks, chain, evs, deps, ser, nthreads = load(path)
    t0 = marks["proc0"]

    print("== phase marks (ms from proc0) ==")
    for k in ["setup_end", "chain_end", "dag0", "dag1", "pp0", "pp1",
              "lfinal0", "lfinal1", "ser0", "ser1", "sha0", "sha1",
              "proc_end"]:
        if k in marks:
            print(f"  {k:10s} {ms(marks[k] - t0):9.1f}")

    # ---- 체인 ----
    c0 = min(c["w"][0] for c in chain)
    c1 = max(c["w"][5] for c in chain)
    print("\n== chain phase ==")
    print(f"  span {ms(c1 - c0):.1f} ms (first start -> last end); "
          f"chain_end mark {ms(marks['chain_end'] - c0):.1f}")
    per_thread = defaultdict(list)
    stage_sum = defaultdict(int)
    for c in chain:
        per_thread[c["tid"]].append((c["w"][0], c["w"][5]))
        for s in range(5):
            stage_sum[CHAIN_STAGES[s]] += c["w"][s + 1] - c["w"][s]
    busy = {w: sum(e - s for s, e in v) for w, v in per_thread.items()}
    total_busy = sum(busy.values())
    span = c1 - c0
    print(f"  busy-sum {ms(total_busy):.1f} ms / budget {nthreads}x"
          f"{ms(span):.1f} = {ms(span) * nthreads:.1f} ms "
          f"(가동률 {100 * total_busy / (span * nthreads):.1f}%)")
    ends = sorted(max(e for _, e in v) for v in per_thread.values())
    starts = sorted(min(s for s, _ in v) for v in per_thread.values())
    print(f"  ramp: 스레드 시작 skew {ms(starts[-1] - starts[0]):.2f} ms; "
          f"tail: 첫 스레드 종료 -> 마지막 {ms(ends[-1] - ends[0]):.1f} ms")
    tail_idle = sum(ends[-1] - e for e in ends)
    print(f"  tail idle (join 대기 합) {ms(tail_idle):.1f} ms "
          f"= 예산의 {100 * tail_idle / (span * nthreads):.1f}%")
    print("  stage wall-sum: " + " ".join(
        f"{k}={ms(stage_sum[k]):.0f}" for k in CHAIN_STAGES))
    durs = sorted((c["w"][5] - c["w"][0]) for c in chain)
    print(f"  chunk dur p50={ms(durs[len(durs) // 2]):.2f} "
          f"p90={ms(durs[int(len(durs) * .9)]):.2f} "
          f"p99={ms(durs[int(len(durs) * .99)]):.2f} "
          f"max={ms(durs[-1]):.2f} ms")
    if do_ascii:
        ascii_timeline(per_thread, c0, c1, nthreads, title="chain")

    # ---- DAG ----
    if evs:
        print("\n== DAG phase ==")
        d0, d1 = marks["dag0"], marks["dag1"]
        span = d1 - d0
        per_w = defaultdict(list)
        kind_sum = defaultdict(int)
        kind_n = defaultdict(int)
        for e in evs:
            per_w[e["worker"]].append((e["t0"], e["t1"]))
            kind_sum[KIND[e["kind"]]] += e["t1"] - e["t0"]
            kind_n[KIND[e["kind"]]] += 1
        busy = sum(e - s for v in per_w.values() for s, e in v)
        print(f"  wall {ms(span):.1f} ms; busy-sum {ms(busy):.1f} / "
              f"{ms(span) * nthreads:.1f} "
              f"(가동률 {100 * busy / (span * nthreads):.1f}%)")
        print("  event wall-sum: " + " ".join(
            f"{k}={ms(kind_sum[k]):.0f}(n={kind_n[k]})"
            for k in ["deco", "l08", "l09", "prepare"] if k in kind_sum))
        edges, barriers = dag_edges(evs, deps)
        cp_total, best_seg, cp, dur, seg_of = critical_path(
            evs, edges, barriers)
        print(f"  구조적 크리티컬 패스 (실측 dur, 워커 무한 가정): "
              f"{ms(cp_total):.1f} ms = wall 의 "
              f"{100 * cp_total / span:.0f}%")
        segs = sorted(best_seg.items())
        top = sorted(segs, key=lambda kv: -kv[1][0])[:5]
        print("  세그먼트별 최장 체인 top5: " + " ".join(
            f"s{k}={ms(v[0]):.0f}ms" for k, v in top))
        # 배리어 스톨: 배리어 직전 완료-희소 구간
        bar_spans = [(e["t0"], e["t1"]) for e in evs if e["kind"] == 3]
        bar_wall = sum(e - s for s, e in bar_spans)
        print(f"  prepare(배리어) {len(bar_spans)}회, 자신 wall-sum "
              f"{ms(bar_wall):.1f} ms (단독 실행 = {nthreads}-폭 정지)")
        if do_ascii:
            ascii_timeline(per_w, d0, d1, nthreads, title="DAG")

    # ---- serialize ----
    if ser and ser[0]["t1"] > 0:
        print("\n== serialize phase ==")
        s0m, s1m = marks["ser0"], marks["ser1"]
        span = s1m - s0m
        per_w = defaultdict(list)
        for s in ser:
            per_w[s["worker"]].append((s["t0"], s["t1"]))
        busy = sum(e - s for v in per_w.values() for s, e in v)
        first = min(s["t0"] for s in ser)
        last = max(s["t1"] for s in ser)
        print(f"  wall {ms(span):.1f} ms; 워커 구간 {ms(last - first):.1f}; "
              f"선두 오버헤드(alloc+spawn) {ms(first - s0m):.1f}; "
              f"꼬리(join+concat) {ms(s1m - last):.1f}")
        print(f"  busy-sum {ms(busy):.1f} / {ms(span) * nthreads:.1f} "
              f"(가동률 {100 * busy / (span * nthreads):.1f}%)")
        if do_ascii:
            ascii_timeline(per_w, s0m, s1m, nthreads, title="serialize")

    # ---- 전체 예산 ----
    print("\n== gen-window 예산 ==")
    g0, g1 = marks["setup_end"], marks["sha1"]
    gspan = g1 - g0
    all_busy = sum(c["w"][5] - c["w"][0] for c in chain)
    all_busy += sum(e["t1"] - e["t0"] for e in evs)
    all_busy += sum(s["t1"] - s["t0"] for s in ser if s["t1"] > 0)
    # 단일 스레드 구간 (pp, light_final, sha)
    for a, b in [("pp0", "pp1"), ("lfinal0", "lfinal1"), ("sha0", "sha1")]:
        if a in marks and b in marks:
            all_busy += marks[b] - marks[a]
    print(f"  window(setup_end->sha1) {ms(gspan):.1f} ms; 예산 "
          f"{nthreads}x = {ms(gspan) * nthreads:.0f} ms; 계측된 busy-sum "
          f"{ms(all_busy):.0f} ms => 전체 가동률 "
          f"{100 * all_busy / (gspan * nthreads):.1f}%")

    # ---- 융합 시뮬 ----
    if do_fuse and evs:
        print("\n== 체인-DAG 융합 시뮬 (실측 dur 재스케줄; 모델 추정치) ==")
        edges, barriers = dag_edges(evs, deps)
        bset = set(barriers)
        tasks, preds, prio = {}, {}, {}
        for c in chain:
            tid = ("c", c["i"])
            tasks[tid] = (c["w"][5] - c["w"][0]) / 1e6
        NE = len(evs)
        for e in evs:
            i = e["idx"]
            tasks[("e", i)] = (e["t1"] - e["t0"]) / 1e6
            ps = set()
            for p in edges.get(i, ()):
                ps.add(("e", p))
            # 체인 의존: 접촉 셀 중 청크-격자 셀 (< WORLD_CHUNKS)
            if i not in bset:
                for cell in deps[i]:
                    if cell < len(chain):
                        ps.add(("c", cell))
            preds[("e", i)] = ps
        # 배리어 의존: 앞 전 DAG 이벤트 -> 배리어 -> 뒤 전 DAG 이벤트.
        # (체인 이벤트는 라이트 월드 밖 — 배리어 비대상.) 엣지 폭발을
        # 피하려고 배리어는 직전 배리어와 사이 이벤트만 잇는다 (전이 폐포).
        prev_b = None
        for b in barriers:
            ps = set()
            lo = prev_b + 1 if prev_b is not None else 0
            for j in range(lo, b):
                ps.add(("e", j))
            if prev_b is not None:
                ps.add(("e", prev_b))
            preds[("e", b)] |= ps
            for j in range(b + 1, NE):
                if j in bset:
                    break
                preds[("e", j)].add(("e", b))
            prev_b = b
        # 우선순위 2안: (a) 현행 체인 순서(행우선) 우선, (b) 수요 순서
        for name, chain_prio in [
            ("naive(행우선 체인 우선)",
             lambda i: i),
            ("demand(첫 소비 deco 순)", None),
        ]:
            if chain_prio is None:
                first_use = {}
                for e in evs:
                    i = e["idx"]
                    if i in bset:
                        continue
                    for cell in deps[i]:
                        if cell < len(chain) and cell not in first_use:
                            first_use[cell] = i
                for t in tasks:
                    if t[0] == "c":
                        prio[t] = first_use.get(t[1], 10 ** 9)
                    else:
                        prio[t] = t[1]
            else:
                for t in tasks:
                    prio[t] = (0, chain_prio(t[1])) if t[0] == "c" \
                        else (1, t[1])
            mk, _ = list_schedule(tasks, preds, nthreads, prio)
            print(f"  {name}: 융합 makespan {mk:.1f} ms "
                  f"(현행 chain+dag = "
                  f"{ms(marks['chain_end'] - marks['setup_end']) + ms(marks['dag1'] - marks['dag0']):.1f} ms)")


if __name__ == "__main__":
    main()
