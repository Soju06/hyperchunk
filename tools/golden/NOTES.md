# Golden generation notes — determinism findings (2026-07-28)

## TL;DR

Vanilla 26.2 worldgen is **not run-to-run deterministic from the `features`
stage onward**, even for the same seed on the same machine, and even under
attempted sequential chunk forcing. Stages `01_structure_starts` through
`06_carvers` are bit-identical across runs. This changes the parity gate
design (see ADR-007).

## Evidence

All artifacts referenced below are reproducible; probe harness lives in
`tools/golden/experiments/sequential_probe.sh`.

1. **Default runs (make_stage_dumps.sh, 3x3 grid), run A vs committed golden:**
   - `01..06` stage dumps: identical.
   - `07_features` onward: differ in 4/9 chunks (real content differences:
     glow_lichen/vine/ore placements, not formatting).
   - raw `r.0.0.mca` sha256: differs. canonical payload (LastUpdate masked,
     `tools/golden/compare_regions.py --canonical-hash`): still differs.

2. **Sequential probe (5x5 grid, one forceload at a time, two runs):**
   - `01..06`: identical across runs (0 diffs).
   - `07_features`+: 19–40 files differ per stage, 16/25 chunks affected.
   - Crucially, the **full-promotion order itself differed between the two
     probe runs** (`grep 'dumped c\..*/full' server.log`), i.e. issuing
     forceloads one at a time and waiting for the target chunk's `full` dump
     does NOT pin the scheduler to a stable total order. The experiment is
     therefore *inconclusive about* "strict order ⇒ determinism", but
     *conclusive that* vanilla's scheduler does not provide a stable order
     even under coarse sequential driving.

## Root cause (mechanism)

- Stages up to `carvers` are per-chunk pure functions of (seed, chunk pos):
  position-seeded RNG, no cross-chunk writes. Hence bit-stable.
- `features` (decoration) both **reads and writes across chunk borders**
  (trees/vines spill into neighbors; heightmap updates affect later
  placements). Which neighbor decorated first therefore changes the blocks a
  later chunk sees, and light/spawn/full inherit those differences.
- The chunk system schedules stage promotions on worker threads driven by
  dependency resolution; the resulting features-application order is not a
  deterministic function of the seed.

## Consequence for the parity gate (ADR-002 D3)

"sha256(our region) == sha256(vanilla region)" is not well-defined: vanilla
does not equal itself across runs. The gate is redefined in **ADR-007** as:

- **Tier 1 (bit-exact, order-free):** stages `01..06` must match vanilla
  golden dumps byte-for-byte. These are order-independent.
- **Tier 2 (order-replay):** the stage-dump mod must additionally record the
  actual features-stage execution order of the golden run (order manifest).
  The C implementation replays that recorded order and must then match the
  `07_features`..`11_full` dumps byte-for-byte. Nondeterminism becomes an
  *input*, not noise. Matching two independent golden runs (different
  recorded orders) is strong evidence of algorithmic parity.

## Open items

- [ ] Extend stage-dump mod to emit `order.manifest` (sequence of
      (chunk, stage) applications during the golden run).
- [ ] Regenerate golden bundle with the order manifest included.
- [ ] Plan Task 9 (features) consumes the manifest for replay verification.

## Environment

- MC 26.2 (`TARGET_VERSION`), server sha1 823e2250d24b3ddac457a60c92a6a941943fcd6a
- Fabric loader 0.19.3, installer 1.1.2, JDK 25.0.3 (Ubuntu 24.04)
- Overworld y-range confirmed: -64..319 (384 sections of data unchanged from 1.21)
- Gamerules pinned by harness: random_tick_speed=0, spawn_mobs=false,
  advance_weather=false, advance_time=false (26.2 snake_case names)
