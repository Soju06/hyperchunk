# benchmarks-and-viz Specification

## Purpose

공개 벤치 수치와 클레임 규칙, 데모/viz 캡처의 normative SSOT. 여기 있는 숫자와
문구는 **클레임 펜스**다: 요약·재서술 과정에서 숫자·문구를 바꾸는 것 자체가
스펙 위반이다. 측정 프로토콜·캐벳 전량·결정 역사는 [context.md](context.md).

## Requirements

### Requirement: Canonical public benchmark numbers

The published 3-way numbers are the B-6 campaign medians (region r.0.0, 1024
chunks, seed 1234567890, one machine — hc-e6: OCI `VM.Standard.E6.Flex`,
AMD EPYC 9J45 Zen5, 32 vCPU, OpenJDK 25.0.3):

| System | Wall | vs vanilla |
|---|---|---|
| vanilla 26.2 dedicated server | **11.9 s** | 1.00x |
| Fabric 0.19.3 + C2ME 0.4.2-alpha.0.35 | **3.7 s** | 3.22x |
| hyperchunk REPLAY, 20 threads | **3.155 s** | 3.77x |
| hyperchunk FREE, 20 threads | **0.894 s** | **13.3x** (4.14x vs C2ME) |

Public materials MUST quote these numbers unchanged. Any re-measurement that
changes them lands as its own reviewed bench note before public materials
are updated.

#### Scenario: Summarizing the benchmark

- **WHEN** any document, caption, or PR summarizes the public benchmark
- **THEN** the numbers 11.9 s / 3.7 s / 3.155 s / 0.894 s / 13.3x / 4.14x
  appear verbatim or are omitted — never rounded, rescaled, or "improved"

### Requirement: Stage attribution — multipliers are not portable

Every public multiplier claim MUST name its stage (machine): 13.3x/4.14x are
properties of hc-e6 (Zen5, AVX-512 dispatch active, 32 vCPU) and MUST NOT be
generalized to arbitrary hardware (B-6 caveat 13).

#### Scenario: Multiplier without a stage

- **WHEN** a public sentence states a multiplier with no machine attribution
- **THEN** it is corrected to name the stage or removed

### Requirement: Lower bounds accompany headline multipliers

Public headline multipliers MUST carry the measurement-error-deducted lower
bounds as a footnote: **≥12.5x** vs vanilla, **≥3.3x** vs C2ME.

#### Scenario: Headline caption

- **WHEN** the 13.3x (or 4.14x) figure appears in a caption or README
- **THEN** the ≥12.5x / ≥3.3x lower bounds are stated alongside

### Requirement: REPLAY claim phrasing

REPLAY vs C2ME (1.17x) is inside the measurement-error band (lower bound
0.95x). A superiority claim over C2ME for REPLAY MUST NOT be made. The
sanctioned phrasing is: REPLAY delivers **"canonical-identical output at
C2ME-class speed"**. Any "bit-exact"-style wording MUST carry the canonical
hash definition ([worldgen-parity](../worldgen-parity/spec.md)) as a
footnote.

#### Scenario: REPLAY caption review

- **WHEN** public material describes REPLAY relative to C2ME
- **THEN** it uses the C2ME-class phrasing and never "faster than C2ME"

### Requirement: Mode labeling of performance and parity claims

Bench numbers are always FREE mode; parity claims are always REPLAY mode
(ADR-008 P2). PR performance claims MUST label stage, machine, and mode;
mixing modes in a report is a rejection.

#### Scenario: Perf claim in a PR

- **WHEN** a PR claims a performance change
- **THEN** the numbers carry stage + machine + mode labels (e.g.
  `noise, hc-e6, FREE`)

### Requirement: Determinism narrative numbers

The public determinism comparison MUST use the observed minima, never
higher: same seed, two runs — vanilla differs in **581+/1024** chunks, C2ME
in **758+/1024**, hyperchunk in **0**, across all 12 runs at both 20 and 32
threads (REPLAY 6/6 == golden canonical `a5963205…`, FREE 6/6 == own-v1
`2eb7485b…`).

#### Scenario: Determinism caption

- **WHEN** the determinism result is published
- **THEN** the 581+/758+/0 figures and the 12/12-run scope are stated
  without exaggeration

### Requirement: Legal disclaimer on public materials

Distribution README and video/demo descriptions MUST include: "NOT AN
OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR
MICROSOFT." The project name MUST NOT use "Minecraft" as a dominant element
(ADR-001 D5).

#### Scenario: New public artifact

- **WHEN** a release, video, or demo page is published
- **THEN** the disclaimer is present

### Requirement: Demo renders measured timing, not synthetic animation

Every panel of the 3-way race demo MUST replay measured per-chunk timing
from the same machine: hyperchunk from its own instrumentation
(`HC_BENCH_TIMELINE`), vanilla and C2ME from the Fabric `chunk-timeline-mod`
server-side instrumentation. Timeline JSON MUST record its provenance
(`meta.synthetic` / probe / instrumented fields) so a synthetic panel cannot
pass as measured. The two-stage reveal maps `t_stage1_ms` (chain/SURFACE) to
the faint-terrain pass and `t_done_ms` (complete/FEATURES) to final pixels,
with the `t_stage1 ≤ t_done` invariant enforced at conversion.

#### Scenario: Timeline provenance check

- **GIVEN** a timeline JSON fed to `hcviz render`
- **WHEN** its `meta` block is inspected
- **THEN** the capture method (instrumented/probe/synthetic) is recorded and
  the demo caption reflects it

#### Scenario: hyperchunk panel fidelity

- **WHEN** the hyperchunk panel is regenerated from instrumentation
- **THEN** its final frame is pixel-identical to the still render of the
  same region (observed max|diff| = 0)
