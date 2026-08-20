# scheduler Specification

## Purpose

Normative SSOT for the batch scheduler: the FREE/REPLAY dual-mode contract,
stage-code sharing between the two modes, and the determinism guarantee.
Rationale, numbers, and decision history are in [context.md](context.md).

## Requirements

### Requirement: Dual-mode scheduler (FREE and REPLAY)

The core scheduler SHALL operate in exactly two modes (ADR-008):

- **FREE** (bench/demo): conflict-free maximum parallelism; the scheduler
  picks its own feature-application order, optimized for speed.
- **REPLAY** (verification): the scheduler replays a golden order manifest;
  speed is subordinate to byte-equality of stages 07-11
  ([worldgen-parity](../worldgen-parity/spec.md) Tier 2).

#### Scenario: Mode selection

- **WHEN** a consumer requests generation
- **THEN** it selects FREE or REPLAY(manifest) through the scheduling-policy
  contract, and the same region can be generated in either mode

### Requirement: Both modes share the same stage code

FREE and REPLAY MUST execute the same stage implementations; only the
scheduling policy differs (ADR-008 D3). A verification-only slow path or a
bench-only fast path MUST NOT exist: the code proven bit-exact in REPLAY
must be the code timed in FREE, or the proof is void.

#### Scenario: Stage code divergence attempt

- **WHEN** a change introduces a stage implementation used by only one mode
- **THEN** it is rejected

### Requirement: Scheduling policy is a library contract

The policy injection point MUST live in the core API
(`hc_schedule_policy { FREE, REPLAY(manifest) }` shape), not as a CLI-only
flag (ADR-008 P1). Feature-stage conflicts (cross-chunk writes within the
decoration radius) MUST be prevented structurally by the scheduler; only
conflict-free chunk sets run concurrently (ADR-002 P4, ADR-003 D3).

#### Scenario: Conflict-free batching

- **GIVEN** two chunks within each other's decoration write radius
- **WHEN** FREE mode schedules them
- **THEN** they never decorate concurrently; the scheduler serializes the
  conflicting pair

### Requirement: Deterministic output in both modes

Scheduler nondeterminism MUST NOT leak into output content:

- REPLAY output MUST be canonical-identical to the golden capture
  (pinned canonical hash `a5963205...`).
- FREE MUST fix one valid total order for conflicting work such that its
  output is deterministic across runs and thread counts, pinned as its own
  reference hash (own-v1 `2eb7485b...` in `golden/SHA256SUMS`). When FREE's
  fixed order changes by design, the own-v1 constant is re-pinned in the
  same change, with the coherence log gating described in the archive notes.

#### Scenario: Repeated FREE runs

- **WHEN** the same region is generated repeatedly in FREE mode at different
  thread counts
- **THEN** every run's canonical hash equals the pinned own-v1 hash
  (observed: 12/12 runs at 20 and 32 threads, B-6)

#### Scenario: Repeated REPLAY runs

- **WHEN** the same region is generated repeatedly in REPLAY mode
- **THEN** every run's canonical hash equals the golden canonical hash
