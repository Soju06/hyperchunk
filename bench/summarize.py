#!/usr/bin/env python3
"""P2-0 벤치 JSONL 요약 — 반복 실행의 중앙값/최소/최대와 스테이지 분해 표.

usage: summarize.py <results.jsonl> [more.jsonl ...]

여러 파일을 주면 파일(=구성)별로 표를 내고, gen_wall 중앙값 배율을
첫 파일 기준으로 붙인다 (O2/O3, 스레드 수 비교용).
"""
import json
import statistics
import sys


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    if not rows:
        sys.exit(f"no rows in {path}")
    return rows


def ms(ns):
    return ns / 1e6


def summarize(path, base_med=None):
    rows = load(path)
    n = len(rows)
    if not all(r["pass"] for r in rows):
        sys.exit(f"FAIL row present in {path} — parity broken, numbers void")
    gen = [r["gen_wall_ns"] for r in rows]
    med = statistics.median(gen)
    lo, hi = min(gen), max(gen)
    spread = (hi - lo) / med * 100
    print(f"\n== {path} ==")
    print(f"runs={n} threads={rows[0]['threads']} seed={rows[0]['seed']} "
          f"pass={n}/{n}")
    print(f"gen wall: median {ms(med):.1f} ms  min {ms(lo):.1f}  "
          f"max {ms(hi):.1f}  spread {spread:.1f}%"
          + (f"  vs-base x{base_med / med:.3f}" if base_med else ""))
    print(f"per-chunk (1024 emitted): {ms(med) / 1024:.3f} ms median")

    def med_of(getter):
        return statistics.median(getter(r) for r in rows)

    chain_wall = med_of(lambda r: r["chain_wall_ns"])
    cpu = {k: med_of(lambda r, k=k: r["chain_cpu_ns"][k])
           for k in rows[0]["chain_cpu_ns"]}
    cpu_tot = sum(cpu.values())
    print(f"chain phase wall {ms(chain_wall):.1f} ms, cpu-sum "
          f"{ms(cpu_tot):.1f} ms:")
    for k, v in sorted(cpu.items(), key=lambda kv: -kv[1]):
        print(f"  {k:12s} {ms(v):9.1f} ms  {v / cpu_tot * 100:5.1f}% of chain cpu")
    ser = {k: med_of(lambda r, k=k: r["serial_wall_ns"][k])
           for k in rows[0]["serial_wall_ns"]}
    print("serial phases (wall, median):")
    for k, v in sorted(ser.items(), key=lambda kv: -kv[1]):
        print(f"  {k:12s} {ms(v):9.1f} ms")
    # 스테이지 비중: 생성 wall 합 기준 (하네스 오버헤드 pp_verify 제외).
    # chain cpu 는 병렬이라 wall 과 직접 합산 불가 — 비중표는 wall 로.
    parts = {"chain(04+07+08)": chain_wall}
    parts.update({k: v for k, v in ser.items() if k != "pp_verify"})
    tot = sum(parts.values())
    print(f"gen wall breakdown (sum {ms(tot):.1f} ms):")
    for k, v in sorted(parts.items(), key=lambda kv: -kv[1]):
        print(f"  {k:16s} {ms(v):9.1f} ms  {v / tot * 100:5.1f}%")
    return med


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    base = None
    for path in sys.argv[1:]:
        med = summarize(path, base)
        if base is None:
            base = med


if __name__ == "__main__":
    main()
