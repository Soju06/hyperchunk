"""HC_BENCH_TIMELINE (waterfall v1) → timeline.json converter.

Waterfall v1 (bench/hyperchunk_bench.c, wf_dump):
    # hyperchunk-bench waterfall v1 threads=<N> policy=<free|replay>
    M <name> <ns>                                phase mark (CLOCK_MONOTONIC ns)
    C <i> <cx> <cz> <tid> <w0..w5>               chain chunk, 6 stage boundaries
    E <idx> <kind> <cx> <cz> <worker> <t0> <t1>  DAG event (0 deco,1 l08,2 l09,3 prepare)
    D <idx> <n> <cell...>                        event touched cells
    S <idx> <worker> <t0> <t1>                   serialize chunk, cx=idx&31 cz=idx>>5

t=0 of the produced timeline is the 'setup_end' mark; wall_s spans to
'proc_end' (== gen wall + replay_load, i.e. setup/boot excluded).

Per-chunk completion event choices for the emitted 32×32 region:
    serialize (default) — chunk fully done and in final serialized form (S.t1)
    deco                — block content complete (decoration event end, E.t1)
    chain               — chain stages done: noise/surface/carvers (C.w5)
"""

from __future__ import annotations

import json
from pathlib import Path

EVENTS = ("serialize", "deco", "chain")
_EV_DECO = 0


class ConvertError(Exception):
    pass


def parse_waterfall(path):
    p = Path(path)
    marks, C, E, S = {}, [], [], []
    threads = policy = None
    with open(p) as f:
        header = f.readline().strip()
        if "waterfall v1" not in header:
            raise ConvertError(f"{p}: not a waterfall v1 file: {header!r}")
        for tok in header.split():
            if tok.startswith("threads="):
                threads = int(tok.split("=", 1)[1])
            elif tok.startswith("policy="):
                policy = tok.split("=", 1)[1]
        for line in f:
            parts = line.split()
            if not parts:
                continue
            kind = parts[0]
            if kind == "M":
                marks[parts[1]] = int(parts[2])
            elif kind == "C":
                # i cx cz tid w0..w5
                C.append(tuple(int(x) for x in parts[1:]))
            elif kind == "E":
                E.append(tuple(int(x) for x in parts[1:]))
            elif kind == "S":
                S.append(tuple(int(x) for x in parts[1:]))
            # D lines (dependency cells) are irrelevant for completion times
    for req in ("setup_end", "proc_end"):
        if req not in marks:
            raise ConvertError(f"{p}: missing mark '{req}'")
    return {"threads": threads, "policy": policy, "marks": marks, "C": C, "E": E, "S": S}


def waterfall_to_timeline(
    path,
    system: str,
    display_name: str,
    stage: str = "",
    event: str = "serialize",
    seed: int = None,
    result_json: str = None,
) -> dict:
    if event not in EVENTS:
        raise ConvertError(f"unknown event '{event}' (want {EVENTS})")
    wf = parse_waterfall(path)
    t0 = wf["marks"]["setup_end"]
    wall_s = (wf["marks"]["proc_end"] - t0) / 1e9

    recs = []
    if event == "serialize":
        if not wf["S"]:
            raise ConvertError(
                f"{path}: no S records — serialize timeline needs --policy free"
            )
        for idx, _worker, _s0, s1 in wf["S"]:
            recs.append((idx & 31, idx >> 5, s1))
    elif event == "deco":
        for _idx, kind, cx, cz, _worker, _e0, e1 in wf["E"]:
            if kind == _EV_DECO and 0 <= cx < 32 and 0 <= cz < 32:
                recs.append((cx, cz, e1))
        if not recs:
            raise ConvertError(f"{path}: no deco events — needs --policy free")
    else:  # chain
        for _i, cx, cz, _tid, *w in wf["C"]:
            if 0 <= cx < 32 and 0 <= cz < 32:
                recs.append((cx, cz, w[5]))

    chunks = [
        {"cx": cx, "cz": cz, "t_done_ms": round((t - t0) / 1e6, 3)}
        for cx, cz, t in recs
    ]
    chunks.sort(key=lambda c: c["t_done_ms"])

    meta = {
        "synthetic": False,
        "source": Path(path).name,
        "event": event,
        "threads": wf["threads"],
        "policy": wf["policy"],
        "workers": wf["threads"],
        "region": {"x0": 0, "z0": 0, "w": 32, "h": 32},
    }
    if seed is not None:
        meta["seed"] = seed
    if result_json:
        res = json.loads(Path(result_json).read_text())
        for k in ("seed", "canonical", "pass"):
            if k in res:
                meta[k] = res[k]

    return {
        "system": system,
        "display_name": display_name,
        "stage": stage,
        "wall_s": round(wall_s, 6),
        "chunks": chunks,
        "meta": meta,
    }
