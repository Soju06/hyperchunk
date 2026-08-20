# benchmarks-and-viz - Context

## Purpose & scope

[spec.md](spec.md) is the normative SSOT for the public numbers and claim
rules. This document records where those numbers come from (the B-6
campaign), a summary of the measurement protocol, the caveats, and the
factual record of the viz pipeline. The full original notes live in the
bench (B-1 to B-6) and viz (VIZ-1 to VIZ-5) folders under
[changes/archive/](../../changes/archive/).

## B-6 public re-measurement (2026-08-12, hc-e6): source of the numbers

- **Stage**: OCI `VM.Standard.E6.Flex`, 16 OCPU = AMD EPYC 9J45 (Zen5) SMT 32
  vCPU, 64GB, Ubuntu 24.04, OpenJDK 25.0.3 (both Java lineages
  `-Xms2G -Xmx8G`). hyperchunk at HEAD 7aaf6c7, preset `bench-o2`; on this
  stage the noise kernel uses the AVX-512 backend (runtime dispatch). Stage
  eligibility was measured in B-4 (0 steal ticks during the bench,
  instructions determinism 1.4e-5).
- **Workload**: seed 1234567890, overworld r.0.0 full coverage, 1024 chunks,
  median of 3 runs. Vanilla/C2ME measured by 0.5s precision polling
  (`forceload` + `execute if loaded` probes), hyperchunk by the internal
  `gen_wall` instrumentation (includes generation + serialize + sha256).
- **Results**: vanilla 11.9s / C2ME 3.7s / REPLAY 20T 3.155s / FREE 20T
  0.894s. 32T stated alongside: REPLAY 3.091s, FREE 0.829s (14.4x, but 20T
  is the official figure). In cps terms: 86 -> 277 -> 1,145 chunks/s
  (FREE 20T).
- **Lower-bound derivation**: competitors = min run - 0.5s poll interval -
  2-tick command latency (0.1s, a conservative assumption), hyperchunk =
  max run -> FREE vs vanilla **>=12.5x**, vs C2ME **>=3.3x**. Probe
  injection load is additionally left undeducted (its direction overstates
  the competitors, i.e. favors us).
- **REPLAY vs C2ME 1.17x is inside the error band** (lower bound 0.95x under
  the same deductions): the rationale for banning a public superiority
  claim. The correct phrasing: REPLAY reproduces the golden with
  "canonical-identical output at C2ME-class speed" (the spec's REPLAY claim
  phrasing).
- **Determinism**: vanilla 3 runs, 3 hashes (semantic diff 581/587/591
  /1024), C2ME 5 runs, 5 hashes (758-804/1024), hyperchunk REPLAY 6/6 ==
  golden `a5963205...3c24`, FREE 6/6 == own-v1 `2eb7485b...84d6` (3 runs
  each at 20T/32T). Mechanism: decoration reads the neighbor chunk's
  current blocks, so the completion order between neighbors gets baked into
  the content.
- **Measurement-window symmetry**: Java boot 6.0-7.0s excluded <->
  hyperchunk setup ~0.77s (reference load, DF compile) excluded. The raw
  jsonl preserves proc_wall_ns and setup_ns (FREE 20T proc_wall median
  1.68s).
- **Key caveats** (full list in B-6 note section 5): cloud VM (2 steal ticks
  observed); all three components of the one-sided polling error point in
  our favor (deducted in the lower bounds); the 144 pre-generated spawn
  chunks favor only the competitors; C2ME is an alpha channel build (latest
  matching 26.2); C2ME's 12 workers is the heap-derived default, closed by
  a forced 24-worker sensitivity run at 3.7s with no gain; stage-dependent
  multipliers (AVX-512/32vCPU), not to be generalized.

Number lineage: B-1 (claw, 2026-08-06): vanilla 15.8s / C2ME 6.5s / FREE
2.74s (5.76x) -> B-6 (hc-e6): the table above. The multiplier growth mixes
in stage-dependent contributions (AVX-512, 32 cores, P2-8 to P2-11
cumulative).

## Viz pipeline (VIZ-1 to VIZ-5, 2026-08-13 to 2026-08-19)

- **hyperchunk capture**: `HC_BENCH_TIMELINE` waterfall v1 marks (plus P
  records for pp, `P <m> <cx> <cz> <t0> <t1>`) -> `hcviz convert`.
  t0 = the setup_end mark, wall = proc_end - setup_end.
- **Event definition**: complete (default) = max(own chain w5, decoration
  E.t1 in the +/-1 window) = the last substantive block write. serialize
  was rejected: its p50 bunches at 98.9%, at the very end, which collapses
  the reveal (VIZ-2). `--stage1 chain` additionally emits own C.w5 as
  `t_stage1_ms`, the two-stage reveal (VIZ-3): a faint terrain tone at
  chain time (stage1.cells=1 hard invariant) -> final pixels at complete
  time.
- **Java-side capture**: Fabric loader 0.19.3 + `chunk-timeline-mod`
  (thenApply attached at ChunkStep.apply RETURN; fully inert when
  `-Dhyperchunk.timeline.file` is absent; shutdown-hook TSV flush,
  completeness gated by the `# end events=N` trailer). t_stage1_ms =
  SURFACE completion, t_done_ms = FEATURES completion. **C2ME's FULL step
  does not go through ChunkStep.apply, so FULL-based semantics are
  forbidden** (VIZ-5).
- **Clock mapping**: nano->epoch mapping via the (epochMillis, nanoTime)
  ref/flush pairs in the TSV header; `hcviz convert-instr` rejects drift
  above 100ms (measured -0.2 to -0.6ms).
- **Schema**: `tools/viz/schema/timeline.schema.json` (draft 2020-12).
  chunks[] carries `t_done_ms` plus optional `t_stage1_ms` (t_stage1 <=
  t_done invariant). meta: `synthetic` / `probe` with `probe_interval_ms` /
  `instrumented` with `stage1_event`, `done_event`, `disk_loaded_chunks`,
  `clock_drift_ms` / `time_scaled_from_wall_s`; these drive the caption
  branch (synthetic/probe/instrumented).
- **Measured results**: features completion p10/p50/p90 vanilla 17/46/87%,
  C2ME 32/58/87%: progressive reveal holds on all three panels. Pre-gen
  fallback: of the 144 boot spawn-prep chunks, the 4 chunks at features or
  beyond fall back to the first in-window event
  (meta.disk_loaded_chunks=4).

## Decision history

> append-only. Existing entries are never edited. When a decision changes,
> add a new entry and mark the previous one `Superseded by`.

The claim rules of this capability were migrated not from a single ADR but
from B-6 note section 0 (public numbers) and section 6 (GIF and public
caption recommendations) (2026-08-12; the original is B-6-3way-public.md in
the bench folder of [changes/archive/](../../changes/archive/)). The roots
of the public labeling discipline are ADR-008 P2 (bench=FREE,
parity=REPLAY; [scheduler/context.md](../scheduler/context.md)) and ADR-001
D5 (disclaimer; [project.md](../../project.md)). ADR-002 P5 (bench host
reliability) was resolved by B-4's hc-e6 eligibility measurement
([generation-pipeline/context.md](../generation-pipeline/context.md)).
