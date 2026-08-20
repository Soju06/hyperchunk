# scheduler Context

## Purpose & scope

[spec.md](spec.md) is the normative SSOT for the dual-mode scheduler. This
document holds the decision history behind "why two modes" (ADR-008) and the
implementation facts that followed.

## Current state (as of 2026-08)

- FREE is implemented as a cell-FIFO DAG scheduler (decoration +/-2 neighbor
  dependencies plus stage barriers, P2-3); it fixes one valid total order
  that serializes conflicting pairs, and determinism is judged against the
  own-v1 hash. The vanilla-class argument (vanilla features run on a
  single-threaded executor, so order is the only axis of nondeterminism) is
  in the P2-3 notes section 1.4 ([changes/archive/](../../changes/archive/)
  phase2).
- Operational pitfalls, such as the FREE own-v1 constant re-pinning rule and
  the golden manifest being a crawl (gate-only), are also recorded in the
  P2-3 notes. The TSan gate runs under `setarch -R`.
- B-6 measurements: REPLAY 6/6 == golden `a5963205...`, FREE 6/6 == own-v1
  `2eb7485b...` (3 runs each at 20T and 32T). Rules for public numbers and
  claims are in [benchmarks-and-viz](../benchmarks-and-viz/spec.md).
- Bench numbers are always reported as FREE, parity badges always as REPLAY
  (ADR-008 P2). The normative claim rules are in benchmarks-and-viz spec.md.

## Decision history

> append-only. Existing entries are never edited. When a decision changes,
> add a new entry and mark the previous one `Superseded by`.

Placement: this document is the primary home for ADR-008. ADR-002 P4, the
origin of conflict-avoidance scheduling, is in
[generation-pipeline/context.md](../generation-pipeline/context.md);
scheduler ownership (ADR-003 D3) is in
[core-abi/context.md](../core-abi/context.md).

### ADR-008: The core scheduler operates in dual mode, bench (free order) and verification (order-replay) (Phase 1, 2026-07-28)

**Status:** Decided
**Type:** Architecture / Contract
**Resolves:** Concretizes ADR-007's gate definition into execution modes

#### Context

After ADR-007 redefined the parity gate as two tiers, the owner fixed the
execution-mode split. Owner directive (translated from the Korean original):

> "We do have to do multithreading too, so for the speed comparison we show
> the most optimized approach (slightly nondeterministic, like vanilla), and
> for TC and the consistency tests we check whether it matches by following
> the order (speed is a lower priority there)."

#### Decision (3 core decisions)

| # | Decision | Key point |
|---|---|---|
| D1 | **Bench/demo mode: free scheduling** | Conflict-free maximum parallelism (checkerboard etc.). Feature-application order is free, optimized for performance. Order nondeterminism at the same level as vanilla is allowed *(later, in P2-3, FREE also fixed a total order and gained output determinism; see Current state)* |
| D2 | **Verification mode: order-replay** | Replays the golden order manifest. Speed is a lower priority; the goal is byte-equality of stages 07-11 (ADR-007 Tier 2) |
| D3 | **Both modes share the same stage code; only the scheduler policy is swapped** | The proof holds only if the verified code is what runs in the bench. Separate per-mode implementations are prohibited |

#### Demo narrative (ties into ADR-001)

"The very code that proved bit-exact against vanilla in verification mode
runs Nx in bench mode." Because the two modes share code (D3), the speed
claim and the correctness claim become claims about a single implementation.

#### Anti-goals

- Splitting into a verification-only slow reference implementation and a
  bench-only implementation (violates D3; the proof is voided)
- Stage-dump comparison of bench-mode output (meaningless by definition
  because the order is not fixed, excluding the Tier 1 range)

#### Pitfalls

1. The scheduler policy injection point must live in the core API: the
   `hc_schedule_policy { FREE, REPLAY(manifest) }` shape. A library
   contract, not a CLI flag.
2. Bench numbers are always reported in FREE mode, parity badges always in
   REPLAY mode. Mixing them in a report is credibility suicide.
   *(normative: [benchmarks-and-viz/spec.md](../benchmarks-and-viz/spec.md))*

#### References

- ADR-007 (two-tier gate, [worldgen-parity/context.md](../worldgen-parity/context.md)),
  ADR-003 (scheduler ownership, [core-abi/context.md](../core-abi/context.md)),
  ADR-001 (demo purpose, [project.md](../../project.md))
