# worldgen-parity Specification

## Purpose

비트정확 패리티의 normative 정의: 무엇이 "바닐라와 동일"인지(canonical payload
hash), 어떤 게이트가 그것을 판정하는지(2단 게이트: 비트정확 + order-replay),
golden capture 체계가 지켜야 할 불변식. 근거·수치·결정 역사는
[context.md](context.md).

## Requirements

### Requirement: Bit-exact parity within the gate scope

Generated output MUST byte-match vanilla Minecraft Java Edition 26.2 within
the two-tier gate scope defined below (ADR-002 D3, ADR-007). "Statistically
similar" terrain is a defect, not a feature. Double-precision value paths
MUST NOT be downcast to float.

#### Scenario: Full-region canonical parity

- **GIVEN** the golden capture of region r.0.0 (seed 1234567890, overworld)
- **WHEN** hyperchunk generates the same region in REPLAY mode
- **THEN** the canonical payload hash equals the pinned golden hash in
  `golden/SHA256SUMS` (canonical `a5963205…`), asserted by
  `scripts/parity_gate.sh`

#### Scenario: Approximate generation is rejected

- **WHEN** a change produces terrain that differs from vanilla in any byte
  within the gate scope, however visually similar
- **THEN** the parity gates fail and the change is rejected

### Requirement: Two-tier parity gate

The parity acceptance gate SHALL be two-tiered (ADR-007):

- **Tier 1** — stage dumps `01_structure_starts` through `06_carvers` MUST be
  byte-equal to the vanilla golden dumps unconditionally. These stages are
  pure functions of (seed, chunk pos) and order-independent.
- **Tier 2** — stages `07_features` through `11_full` MUST be byte-equal to
  the golden dumps **under order-replay**: the C implementation replays the
  decoration order recorded in the golden bundle's order manifest.

Tier 1 stages MUST NOT be weakened to order-replay; Tier 2 MUST NOT be
weakened to statistical similarity.

#### Scenario: Tier 1 stage comparison

- **GIVEN** golden stage dumps for stages 01–06
- **WHEN** the corresponding parity test (`noise_stage`, `surface_stage`,
  `carvers_stage`) runs
- **THEN** every dump compares byte-equal with zero diff, with no order input

#### Scenario: Tier 2 order-replay comparison

- **GIVEN** a golden bundle containing 07+ stage dumps and the
  `order.manifest` of the run that produced them
- **WHEN** the C implementation generates with the scheduler replaying that
  manifest
- **THEN** the 07–11 stage outputs compare byte-equal against that bundle

### Requirement: Vanilla nondeterminism is an input, not noise

Vanilla 26.2 feature decoration is order-nondeterministic run-to-run, and the
order is baked into block content. The verification system MUST treat the
recorded order as an input (ADR-007 D3): golden bundles with different
decoration orders (primary and alt) MUST both replay to byte-equality. A 07+
golden bundle is a coherent pair (dumps + order manifest) and MUST NOT be
mixed with another bundle's manifest.

#### Scenario: Two different-order bundles both pass

- **GIVEN** the primary golden bundle and an alt bundle captured with a
  different decoration order
- **WHEN** the Tier 2 suites run against each bundle with its own manifest
- **THEN** both comparisons are byte-equal

### Requirement: Canonical payload hash definition

Any "bit-exact", "canonical-identical", or hash-equality claim MUST use this
definition: sha256 over the full chunk payload bytes with save-time fields
(root `LastUpdate`, the mca header timestamp table) and sector
layout/compression framing normalized out, as computed by
`tools/golden/compare_regions.py`. Raw `.mca` bit-equality MUST NOT be
claimed or used as a gate: the container embeds the capture-time wall clock,
so raw-bit equality is impossible in principle for any implementation,
vanilla included.

#### Scenario: Canonical hash comparison

- **GIVEN** a generated r.0.0.mca and the golden capture
- **WHEN** `compare_regions.py --canonical-hash` runs on both
- **THEN** equality is judged on normalized chunk payloads only, and the
  golden side reproduces the pinned canonical hash `a5963205…`

### Requirement: RNG consumption order is a contract

LCG and Xoroshiro128++ call order MUST be preserved exactly (ADR-002 P2). Any
reordering — loop restructuring, batching, early-exit — that changes RNG
consumption order changes worldgen output and MUST be rejected by the parity
gates.

#### Scenario: Consumption-order regression

- **WHEN** a change alters the number or order of RNG draws on any generation
  path
- **THEN** at least one Tier 1 or Tier 2 parity suite fails byte-equality

### Requirement: JVM numeric semantics on value paths

Value paths that vanilla evaluates with JVM-specific semantics MUST reproduce
those semantics, not the C library's:

- Vanilla `Math.sin`/`Math.cos` are HotSpot intrinsics that can differ from
  glibc by 1 ulp; such paths MUST call `hc_jdk_sin`/`hc_jdk_cos`, never
  libm `sin`/`cos`.
- Float literals MUST be parsed from the raw literal with `strtof`; parsing
  as double and narrowing (`strtod` → `(float)`) double-rounds and MUST NOT
  be used on datapack value paths.

#### Scenario: Trig path audit

- **WHEN** a generation path ports vanilla code that calls `Math.sin` or
  `Math.cos`
- **THEN** the C port calls `hc_jdk_sin`/`hc_jdk_cos` and the parity suites
  remain byte-equal

### Requirement: Golden capture system

Golden data MUST be captured from an instrumented vanilla 26.2 server running
on JDK 25 (ADR-006 D2) via the `tools/golden/` harness. Heavy payloads (stage
dumps, region refs, feature traces, `.mca`) are local-only and gitignored;
the repository tracks their sha256 pins (`golden/SHA256SUMS`, per-bundle
`SHA256SUMS`), order manifests, and format docs. Golden regeneration is a
separate reviewed process landing as a dedicated `golden:` commit with
capture provenance; feature PRs MUST NOT touch `golden/`.

#### Scenario: Feature PR touches golden data

- **WHEN** a non-`golden:` PR modifies anything under `golden/`
- **THEN** the PR is rejected regardless of content

#### Scenario: Local golden data integrity

- **GIVEN** a machine holding the local-only golden payloads
- **WHEN** the parity suites load them
- **THEN** payloads are validated against the tracked sha256 pins before use

### Requirement: Parity claim scope

Public and in-repo parity claims MUST be scoped as "bit-identical **at the
same seed and the same decoration order**" (ADR-007). The claim MUST NOT be
stated as unconditional raw-file equality. Claim wording rules for public
materials are normative in
[benchmarks-and-viz](../benchmarks-and-viz/spec.md).

#### Scenario: Parity wording in public material

- **WHEN** a README, caption, or PR description claims byte equality
- **THEN** the claim names the replayed-order scope (or links the canonical
  hash definition) rather than implying unconditional `.mca` equality
