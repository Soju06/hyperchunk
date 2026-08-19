"""HC_BENCH_TIMELINE (waterfall v1) → timeline.json converter.

Waterfall v1 (bench/hyperchunk_bench.c, wf_dump):
    # hyperchunk-bench waterfall v1 threads=<N> policy=<free|replay>
    M <name> <ns>                                phase mark (CLOCK_MONOTONIC ns)
    C <i> <cx> <cz> <tid> <w0..w5>               chain chunk, 6 stage boundaries
    E <idx> <kind> <cx> <cz> <worker> <t0> <t1>  DAG event (0 deco,1 l08,2 l09,3 prepare)
    D <idx> <n> <cell...>                        event touched cells
    S <idx> <worker> <t0> <t1>                   serialize chunk, cx=idx&31 cz=idx>>5
    P <m> <cx> <cz> <t0> <t1>                    pp drain span (VIZ-2; absent in
                                                 pre-VIZ-2 captures)

t=0 of the produced timeline is the 'setup_end' mark; wall_s spans to
'proc_end' (== gen wall + replay_load, i.e. setup/boot excluded).

Per-chunk completion event choices for the emitted 32×32 region:
    complete (default)  — last substantive block write: max(own C.w5, deco
                          E.t1 over the ±1 write window).  Deco write radius
                          is ±1 (features.c ensureCanWrite; structure pieces
                          route through the same setters); light events and
                          serialize never write blocks.  Excluded: the pp
                          drain's sparse updateFromNeighbourShapes fixes
                          (vine prune / fence shape — no heightmap effect,
                          invisible at tile scale; fluids are scheduled, not
                          placed) — those land before the 'pp1' mark.
    strict              — bit-final content: max(complete, pp P.t1 over the
                          ±1 write window).  Requires P records (VIZ-2+
                          captures).  The pp drain is a row-major serial
                          sweep covering every target chunk ±1, so this
                          collapses most chunks into the pp window — audit
                          use, not for reveal visuals.
    serialize           — chunk fully done and in final serialized form (S.t1)
    deco                — own decoration event end (E.t1); ignores neighbor
                          deco writes into this chunk (debug/comparison)
    chain               — chain stages done: noise/surface/carvers (C.w5)

Two-stage reveal (VIZ-3): stage1="chain" additionally emits t_stage1_ms =
own C.w5 per chunk — the instant the chunk's terrain shape is final while
decoration is still pending.  Always <= t_done_ms (every event above is at
or after the chunk's own chain end).  Timelines without the field render
single-stage; see schema/timeline.schema.json.
"""

from __future__ import annotations

import json
from pathlib import Path

EVENTS = ("complete", "strict", "serialize", "deco", "chain")
_EV_DECO = 0

PROBE_NAME = "forceload+if-loaded full-status"


class ConvertError(Exception):
    pass


def probe_tsv_to_timeline(
    path,
    system: str,
    display_name: str,
    wall_s: float,
    poll_s: float,
    stage: str = "",
    seed: int = None,
    workers: int = None,
) -> dict:
    """chunks-*.tsv (viz_run.sh per-chunk probe capture) → timeline dict.

    Input rows: TREL_s \\t CX \\t CZ — TREL is the driver clock at the poll
    cycle where the chunk's FULL-status promotion was first confirmed
    (t0 = forceload).  One-sided overestimate <= POLL_S + command tick
    alignment (~50ms) + console/log round trip; see
    tools/viz/capture/vanilla-c2me-probe.md.

    wall_s is the run's t1 (first poll with all 1024 confirmed) — the same
    one-sided probe overshoot applies.  These are single-series timelines
    (no t_stage1_ms: FULL promotion is the only externally observable
    completion instant; sub-full progress needs a mod hook).
    meta.synthetic is deliberately ABSENT (measured data); the renderer's
    probe caption keys on meta.probe_interval_ms instead.
    """
    p = Path(path)
    recs = []
    with open(p) as f:
        for ln, line in enumerate(f, 1):
            parts = line.split()
            if not parts:
                continue
            if len(parts) != 3:
                raise ConvertError(f"{p}:{ln}: want 'TREL\\tCX\\tCZ', got {line!r}")
            trel, cx, cz = float(parts[0]), int(parts[1]), int(parts[2])
            recs.append((cx, cz, trel))
    seen = {(cx, cz) for cx, cz, _ in recs}
    if len(seen) != len(recs):
        raise ConvertError(f"{p}: duplicate chunk confirmations")
    want = {(x, z) for x in range(32) for z in range(32)}
    if seen != want:
        missing = sorted(want - seen)[:5]
        extra = sorted(seen - want)[:5]
        raise ConvertError(
            f"{p}: not exactly r.0.0 coverage ({len(seen)}/1024; "
            f"missing {missing}, extra {extra})"
        )
    late = [(cx, cz, t) for cx, cz, t in recs if t > wall_s]
    if late:
        raise ConvertError(f"{p}: t_done past wall_s {wall_s}: {late[:3]}")

    chunks = [
        {"cx": cx, "cz": cz, "t_done_ms": round(t * 1000.0, 3)}
        for cx, cz, t in recs
    ]
    chunks.sort(key=lambda c: c["t_done_ms"])

    meta = {
        "source": p.name,
        "probe": PROBE_NAME,
        "probe_interval_ms": round(poll_s * 1000.0, 3),
        "region": {"x0": 0, "z0": 0, "w": 32, "h": 32},
    }
    if seed is not None:
        meta["seed"] = seed
    if workers is not None:
        meta["workers"] = workers

    return {
        "system": system,
        "display_name": display_name,
        "stage": stage,
        "wall_s": wall_s,
        "chunks": chunks,
        "meta": meta,
    }


INSTRUMENTED_NAME = "fabric-loader+chunk-timeline-mod"
INSTR_STAGE1_STAGES = ("noise", "surface")
INSTR_DONE_STAGES = ("features",)


def instr_tsv_to_timeline(
    path,
    system: str,
    display_name: str,
    wall_s: float,
    t0_epoch: float,
    stage: str = "",
    seed: int = None,
    workers: int = None,
    stage1_stage: str = "surface",
    done_stage: str = "features",
) -> dict:
    """chunk-timeline mod TSV (viz5_run.sh instrumented capture) → timeline.

    Input (tools/viz/capture/chunk-timeline-mod): header pairs
    '# ref epoch_ms=<ms> nano=<ns>' / '# flush epoch_ms=<ms> nano=<ns>',
    then event rows 'kind stageIndex stageName cx cz nano' where kind 'g' is
    a GENERATION_PYRAMID step completion and 'l' a LOADING_PYRAMID one.

    Event nanos map onto the epoch clock via the ref pair; t_rel =
    epoch(evt) - t0_epoch (runner forceload t0, `date +%s.%N`).  Only events
    with t_rel >= 0 count: earlier ones are boot spawn-prep generation (the
    ~144 r.0.0 chunks pre-generated on disk before t0, B-6 §3 census) —
    content made before the timed window opened.

    Per chunk, both series take the FIRST in-window completion of their
    stage from EITHER pyramid:
      t_stage1_ms — `stage1_stage` ('g' = freshly generated terrain, 'l' =
                    the instant a boot-saved chunk's terrain became
                    available on load)
      t_done_ms   — `done_stage`, same rule
    A chunk loaded from disk at final status therefore collapses to
    t_stage1 == t_done within microseconds (appears at once) — honest: its
    content was decided during boot, outside the window.  meta records the
    count as disk_loaded_chunks (done event came from the loading pyramid).

    wall_s is the run's probe t1 (the poll loop is unchanged in
    viz5_run.sh), so the timeline shares VIZ-4's wall semantics.
    meta.probe_interval_ms is deliberately ABSENT — the caption keys
    'instrumented chunk timing' on meta.instrumented instead.
    """
    if stage1_stage not in INSTR_STAGE1_STAGES:
        raise ConvertError(
            f"unknown stage1 stage '{stage1_stage}' (want {INSTR_STAGE1_STAGES})")
    if done_stage not in INSTR_DONE_STAGES:
        raise ConvertError(
            f"unknown done stage '{done_stage}' (want {INSTR_DONE_STAGES})")
    p = Path(path)
    refs = {}
    s1_first, done_first = {}, {}
    with open(p) as f:
        for ln, line in enumerate(f, 1):
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "#":
                if len(parts) >= 4 and parts[1] in ("ref", "flush") \
                        and parts[2].startswith("epoch_ms=") \
                        and parts[3].startswith("nano="):
                    refs[parts[1]] = (
                        int(parts[2].split("=", 1)[1]),
                        int(parts[3].split("=", 1)[1]),
                    )
                continue
            if len(parts) != 6:
                raise ConvertError(
                    f"{p}:{ln}: want 'kind stageIndex stageName cx cz nano', "
                    f"got {line!r}")
            kind, _idx, name, cx, cz, nano = (
                parts[0], parts[1], parts[2],
                int(parts[3]), int(parts[4]), int(parts[5]),
            )
            if name == stage1_stage:
                target = s1_first
            elif name == done_stage:
                target = done_first
            else:
                continue
            if "ref" not in refs:
                raise ConvertError(f"{p}:{ln}: event before '# ref' header")
            ref_ms, ref_nano = refs["ref"]
            t_rel = ref_ms / 1000.0 + (nano - ref_nano) / 1e9 - t0_epoch
            if t_rel < 0:
                continue  # pre-t0: boot spawn-prep
            key = (cx, cz)
            if key not in target or t_rel < target[key][0]:
                target[key] = (t_rel, kind)
    if "ref" not in refs or "flush" not in refs:
        raise ConvertError(f"{p}: missing '# ref'/'# flush' clock headers")
    ref_ms, ref_nano = refs["ref"]
    fl_ms, fl_nano = refs["flush"]
    drift_ms = fl_ms - (ref_ms + (fl_nano - ref_nano) / 1e6)
    if abs(drift_ms) > 100.0:
        raise ConvertError(
            f"{p}: epoch/nanoTime drift {drift_ms:.1f}ms over the run — "
            "clock mapping unreliable")

    want = {(x, z) for x in range(32) for z in range(32)}
    for label, got in (("stage1", s1_first.keys()), ("done", done_first.keys())):
        have = want & set(got)
        if have != want:
            missing = sorted(want - have)[:5]
            raise ConvertError(
                f"{p}: {label} series not full r.0.0 coverage "
                f"({len(have)}/1024; missing {missing})")

    chunks = []
    loaded = 0
    for (cx, cz) in sorted(want):
        s1, s1_kind = s1_first[(cx, cz)]
        t, t_kind = done_first[(cx, cz)]
        if s1 > t:
            raise ConvertError(
                f"{p}: {stage1_stage} after {done_stage} for ({cx},{cz}) — "
                "stage-1 must not exceed t_done")
        if t > wall_s:
            raise ConvertError(
                f"{p}: t_done past wall_s {wall_s} for ({cx},{cz}): {t:.3f}")
        if t_kind == "l":
            loaded += 1
        chunks.append({
            "cx": cx, "cz": cz,
            "t_done_ms": round(t * 1000.0, 3),
            "t_stage1_ms": round(s1 * 1000.0, 3),
        })
    chunks.sort(key=lambda c: c["t_done_ms"])

    meta = {
        "source": p.name,
        "instrumented": INSTRUMENTED_NAME,
        "stage1_event": stage1_stage,
        "done_event": done_stage,
        "disk_loaded_chunks": loaded,
        "clock_drift_ms": round(drift_ms, 3),
        "region": {"x0": 0, "z0": 0, "w": 32, "h": 32},
    }
    if seed is not None:
        meta["seed"] = seed
    if workers is not None:
        meta["workers"] = workers

    return {
        "system": system,
        "display_name": display_name,
        "stage": stage,
        "wall_s": wall_s,
        "chunks": chunks,
        "meta": meta,
    }


def parse_waterfall(path):
    p = Path(path)
    marks, C, E, S, P = {}, [], [], [], []
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
            elif kind == "P":
                P.append(tuple(int(x) for x in parts[1:]))
            # D lines (dependency cells) are irrelevant for completion times
    for req in ("setup_end", "proc_end"):
        if req not in marks:
            raise ConvertError(f"{p}: missing mark '{req}'")
    return {"threads": threads, "policy": policy, "marks": marks,
            "C": C, "E": E, "S": S, "P": P}


def _complete_times(wf, path, strict):
    """Per target chunk: last (possible) substantive block write.

    t_done(cx,cz) = max(own chain w5, deco E.t1 of every event with
    center in the ±1 write window[, pp P.t1 same window when strict]).
    Write windows are source-enforced at ±1 (features.c ensureCanWrite /
    ore bulk clip; postprocess.c pp_set_block dies outside drain ±1);
    l08/l09/prepare and serialize never mutate block states.  Event-end
    conservatism: E.t1 includes the event's trailing light flush and a
    neighbor event may not actually write into this chunk — t_done is
    the earliest PROVEN-final instant, not the last observed write.
    """
    w5 = {}
    for _i, cx, cz, _tid, *w in wf["C"]:
        w5[(cx, cz)] = w[5]
    deco = {}
    for _idx, kind, cx, cz, _worker, _e0, e1 in wf["E"]:
        if kind == _EV_DECO:
            deco[(cx, cz)] = max(deco.get((cx, cz), 0), e1)
    if not deco:
        raise ConvertError(f"{path}: no deco events — needs --policy free")
    pp = {}
    if strict:
        for _m, cx, cz, _p0, p1 in wf["P"]:
            pp[(cx, cz)] = max(pp.get((cx, cz), 0), p1)
        if not pp:
            raise ConvertError(
                f"{path}: no P records (pre-VIZ-2 capture) — "
                "'strict' needs per-drain pp spans; use 'complete'"
            )
    recs = []
    for cz in range(32):
        for cx in range(32):
            t = w5[(cx, cz)]
            for dz in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    k = (cx + dx, cz + dz)
                    t = max(t, deco.get(k, 0), pp.get(k, 0))
            recs.append((cx, cz, t))
    return recs


STAGE1_EVENTS = ("chain",)


def waterfall_to_timeline(
    path,
    system: str,
    display_name: str,
    stage: str = "",
    event: str = "complete",
    stage1: str = None,
    seed: int = None,
    result_json: str = None,
) -> dict:
    if event not in EVENTS:
        raise ConvertError(f"unknown event '{event}' (want {EVENTS})")
    if stage1 is not None and stage1 not in STAGE1_EVENTS:
        raise ConvertError(f"unknown stage1 event '{stage1}' (want {STAGE1_EVENTS})")
    wf = parse_waterfall(path)
    t0 = wf["marks"]["setup_end"]
    wall_s = (wf["marks"]["proc_end"] - t0) / 1e9

    recs = []
    if event in ("complete", "strict"):
        recs = _complete_times(wf, path, strict=(event == "strict"))
    elif event == "serialize":
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

    s1_times = None
    if stage1 == "chain":
        s1_times = {}
        for _i, cx, cz, _tid, *w in wf["C"]:
            s1_times[(cx, cz)] = w[5]

    chunks = []
    for cx, cz, t in recs:
        c = {"cx": cx, "cz": cz, "t_done_ms": round((t - t0) / 1e6, 3)}
        if s1_times is not None:
            s1 = s1_times.get((cx, cz))
            if s1 is None:
                raise ConvertError(
                    f"{path}: no chain record for target chunk ({cx},{cz}) — "
                    "cannot emit stage-1 series"
                )
            if s1 > t:
                raise ConvertError(
                    f"{path}: chain w5 after '{event}' time for ({cx},{cz}) — "
                    "stage-1 must not exceed t_done"
                )
            c["t_stage1_ms"] = round((s1 - t0) / 1e6, 3)
        chunks.append(c)
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
    if event in ("complete", "strict"):
        meta["pp_records"] = bool(wf["P"])
    if stage1 is not None:
        meta["stage1_event"] = stage1
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
