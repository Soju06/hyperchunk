# worldgen-parity: Context

## Purpose & scope

[spec.md](spec.md) is the normative SSOT for the parity definition and gates.
This document holds the history and rationale of those decisions and the
background of the current gate state.

## Current state (as of 2026-08)

- Full region r.0.0 matches the vanilla golden capture on the canonical hash
  (pinned in `golden/SHA256SUMS`, asserted by `scripts/parity_gate.sh`). All
  37 ctest suites green, sanitizer-clean. CI runs the 25-suite subset that
  needs only tracked data; the 12 golden-dependent suites are local-only
  (.github/CONTRIBUTING.md section Tests and golden data).
- Strict comparison (canonical normalization including scheduled ticks) is
  the code default. With the recapture-3 unified golden plus the
  `postProcessGeneration` drain, the region gate is 4/4 byte-exact
  (Task 13-close, [archive](../../changes/archive/)).
- What each stage gate covers and what it is blind to was measured with
  mutation probes and recorded in the per-task completion notes (the task7
  through task14 folders under [changes/archive/](../../changes/archive/)).

## Origin of the canonical hash

The "canonical-identical" definition (the spec's Canonical payload hash
requirement) was finalized for public documents in section 3 of the B-6
public bench notes: a normalized hash over all chunk payload bytes, excluding
save-time fields (root `LastUpdate`, the mca header timestamp table) and
sector layout/compression framing (`tools/golden/compare_regions.py`). The
golden's header timestamps are the capture-time wall clock, so raw-bit
equality is impossible in principle for any system. Golden canonical
`a5963205...3c24`, FREE own-v1 `2eb7485b...84d6` (golden/SHA256SUMS). Full
text: [benchmarks-and-viz/context.md](../benchmarks-and-viz/context.md).

## Decision history

> Append-only. Existing entries are never edited. When a decision changes,
> add a new entry and mark the previous one `Superseded by`.

Related ADR placement: D3 (bit parity mandatory), P2 (RNG consumption order),
and P3 (keep double) of ADR-002 (full text:
[generation-pipeline/context.md](../generation-pipeline/context.md)) are
reflected as requirements in this capability's spec.md. ADR-006 (full text:
same document) D2 (golden capture environment JDK 25) is also reflected into
this spec.md. Below is the full text of ADR-007, whose primary home is this
capability.

### ADR-007: Redefine the parity gate as two tiers (bit-exact + order-replay) (Phase 1, 2026-07-28)

**Status:** Decided
**Type:** Quality strategy / Contract
**Resolves:** Refines the "region sha256 equality" gate of ADR-002 D3 (principle kept, definition replaced)

#### Context

Fact established by measurement during the Task 2 golden work: **vanilla 26.2 worldgen is nondeterministic run-to-run even at the same seed on the same machine.** Evidence is recorded in `tools/golden/NOTES.md`. Summary:

- Stage dumps `01_structure_starts` through `06_carvers` are byte-identical across two independent runs
- From `07_features` onward, 4/9 chunks in the 3x3 baseline run and 16/25 chunks in the 5x5 sequential probe differ in actual content (glow_lichen/vine/ore vein placement)
- Not only the raw `.mca` mismatches; the canonical payload hash with timestamps masked out mismatches too
- Sequential forceload also failed to stabilize it: **the full-promotion order itself differed between the two probe runs**

Mechanism: up through carvers, stages are position-seeded pure functions of (seed, chunk pos), so order does not matter. Features read and write across chunk boundaries (tree/vine spillover, heightmap updates), so neighbor decoration order changes the result, and the scheduler does not fix that order.

Therefore ADR-002 D3's "sha256 equality with the vanilla region", taken as-is, is not a well-formed definition. **Vanilla does not sha256-match even itself.**

#### Decision (3 core decisions)

| # | Decision | Key point |
|---|---|---|
| D1 | **Tier 1 gate: stages 01-06 must be byte-identical to the vanilla golden dumps** | Order-independent range, so unconditionally bit-exact. noise/surface/carvers belong here |
| D2 | **Tier 2 gate: features onward are verified by order-replay** | Record the golden run's actual features execution order as an order manifest; the C implementation replays that order and must byte-match the 07-11 dumps |
| D3 | **Promote the nondeterminism to an input, not noise** | Replaying two golden runs with different orders and matching both is strong evidence of algorithmic parity |

#### Why order-replay over pinning the vanilla order?

The experiment to bend vanilla to our order (sequential probe) failed. The scheduler cannot be pinned from outside. In the other direction, **recording the order vanilla actually took and following it ourselves** is always possible, and it produces a reproducible golden bundle (dumps + order manifest). The C core owns the batch scheduler anyway (ADR-003 D3), so replaying an arbitrary order comes naturally.

#### Implications for the 3-way demo (ties to ADR-001)

The exact scope of the "bit-identical to vanilla" claim becomes "bit-identical **at the same seed + the same decoration order**". The demo copy and the README state this scope honestly. This is not a weakness but a defense: we occupy first the exact point where a technical audience would attack with "vanilla differs run to run itself, so identical to what?".

#### Anti-goals

- Patching the vanilla scheduler itself to pin the order (invasive, unmaintainable)
- Relaxing features onward to "statistically similar" (abandons the parity principle)
- Lowering the Tier 1 range to order-replay (needless weakening)

#### Pitfalls

1. **Order manifest recording is not implemented yet.** Requires extending the stage-dump mod (open item). A golden without a manifest cannot be used for Tier 2 verification. *(Since implemented: hook design finalized in Task 9-pre, see the task9pre-order note in [archive](../../changes/archive/).)*
2. **The cause of the probe's sequential-forcing failure is unresolved.** The forceload completion-wait logic failed to pin the scheduler. The Tier 2 design does not depend on this failure, but retrying it is a waste of time.
3. **spawn/full stage diffs are downstream effects of features.** Do not mistake them for a separate cause.

#### Verification

- Confirm the golden bundle includes order.manifest
- The C implementation supports a manifest replay mode and passes both golden bundles with different orders
- Tier 1: `diff -rq` across all of 01-06 with 0 diff

#### References

- ADR-002 (parity principle: [generation-pipeline/context.md](../generation-pipeline/context.md)), ADR-003 (batch scheduler ownership: [core-abi/context.md](../core-abi/context.md))
- tools/golden/NOTES.md (evidence, environment, reproduction procedure)
- tools/golden/experiments/sequential_probe.sh
