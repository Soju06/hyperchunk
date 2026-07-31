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

- [x] Extend stage-dump mod to emit `order.manifest` (2026-07-31, Task 9-pre;
      granularity is per-chunk, not per-(chunk,stage) — see below and
      `.hermes/notes/task9pre-order/A0-manifest-design-decision.md`).
- [x] Regenerate golden bundle with the order manifest included
      (2026-07-31; plus a second bundle `golden/stages-alt/` with a distinct
      recorded order, ADR-007 D3).
- [x] Plan Task 9 (features) consumes the manifest for replay verification
      (2026-07-31, Task 9a: tests/parity/test_features_walk.c replays both
      bundles' grid order — 81+81 manifest seeds verified, ore/blob+magma
      block parity on both 07 dump sets; trace golden bundle under
      golden/features-trace/, see its FORMAT.md).

**Tier-2 result (Task 9b, 2026-07-31): GREEN.** The full features stage
(steps 0..10) replays BOTH bundles' recorded orders and matches BOTH
`07_features.{blocks,heightmaps}` dump sets cell-for-cell (36,864 blocks +
6 heightmaps × 256 columns × 9 grid chunks, 0 diff; 55,658 / 56,322 blocks
placed per replay). The same code reproducing two different recorded orders
is the ADR-007 D3 evidence. The vanilla p/f trace additionally matches
line-exactly (positions, npos, placed bits; steps ≤10) for the primary
bundle. Gate: tests/parity/test_features_walk.c (CI-enabled, strict).

## Order manifest (Task 9-pre, 2026-07-31)

Design + full bytecode evidence: `.hermes/notes/task9pre-order/` (A0–A6).
Probe: `tools/golden/order_probe.py` (run it on the two bundles; evidence
snapshot in A7 of the notes dir).

### Files per bundle

- `order.manifest` (tracked) — one line per features-stage chunk application
  in actual execution order:
  `seq chunkX chunkZ decorationSeedHex thread nanos`, `#` header records
  target_version / seed / dimension / dump grid / hook point / columns.
  Captured at `WorldgenRandom#setDecorationSeed(JII)J@RETURN` — the
  RNG-determining instant of a chunk's decoration — armed per-thread by
  `ChunkStatusTasks#generateFeatures` HEAD (setDecorationSeed has a second,
  SPAWN-stage caller that must not record; the arm plus a block-origin
  cross-check excludes it). Records EVERY features application in the dump
  dimension (spawn area + dependency ring, not just the grid) because
  ring decorations write into grid chunks.
- `order.snapshots` (tracked) — positions every (chunk, stage) dump in the
  features order: `stage chunkX chunkZ seqBegin seqEnd thread nanos`,
  seq counter sampled before/after the dump's file writes.
  `seqBegin == seqEnd` ⇒ the dump saw exactly the first seqBegin features
  applications; `!=` ⇒ torn snapshot (features landed mid-dump; observed
  only for async-completing stages: 09_light, 11_full).

### Granularity: per-chunk total order is minimal AND sufficient

- Within-chunk decoration is a pure function of (seed, registries, chunk
  pos, pre-features state): FeatureSorter order is deterministic, the RNG is
  re-seeded via `setFeatureSeed(decoSeed, idx, step)` before every structure
  and feature (A2). Nothing sub-chunk to record.
- 26.2 serializes ALL worldgen step bodies of a dimension on one
  `ConsecutiveExecutor` (A5) — the manifest is the actual total execution
  history even at `max.bg.threads=255`, not an imposed linearization.
- The decoration seed is a pure function of (levelSeed, cx·16, cz·16) (A3);
  the manifest's seed column is a consistency check the replay must verify,
  not extra information. Cross-bundle check: identical per chunk. ✓

### What the manifest does / does not determine

- DOES: each chunk's `07_features` dump (snapshot taken synchronously at its
  own decoration completion = own manifest event + all spill-ins with
  smaller seq), and the final features-complete block state.
- DOES NOT alone: `08..11` dumps — those are snapshots at async-completing
  moments; which later spill-ins they contain is the timing that
  `order.snapshots` records (per dump: the features-order prefix it saw).
  A light/spawn/full-stage replay gate (Task 10+) must consume BOTH files,
  or gate on the features-converged final state instead of mid-snapshots.

### Bundles (both: 9-chunk grid around (0,0), seed 1234567890, 315 dump files)

- `golden/stages/seed1234567890` — recorded with `max.bg.threads=1`
  (all applications on Worker-Main-1), 81 applications, 0 torn snapshots.
- `golden/stages-alt/seed1234567890` — recorded with
  `HYPERCHUNK_BG_THREADS=4` (Worker-Main-1..4), 81 applications, 5 torn
  snapshots (flagged in order.snapshots), own `stages-alt/SHA256SUMS`.
- Orders diverge from seq 0; 8/9 grid chunks differ in `07_features`.
  Empirical nugget: with one bg worker the grid neighborhood order is
  STICKY (two 1-thread runs + the old bundle share identical grid
  distance-2 prefixes ⇒ identical 07 dumps) because the grid sits inside
  the boot-time spawn area whose dependency cascade pins the first wave;
  the 4-worker recording is what makes the two bundles a real Tier-2 pair.
- Stages 01–06: byte-identical across ALL of (old pre-manifest bundle,
  new 1-thread bundle, 4-thread alt bundle) — re-validates ADR-007 D1
  under both scheduler configs. (The regeneration added 18 header-only
  `01/02 *.heightmaps.txt` files absent from the old bundle: the old bundle
  predated the committed dumper, which always writes the file; header-only,
  no worldgen content.)
- A bundle is a coherent (dumps + order.manifest + order.snapshots) pair —
  REGENERATION REPLACES IT WHOLESALE; 07+ dumps can never be reproduced
  without replaying the recorded order. Manifests/snapshots are tracked in
  git; raw dumps stay local with hashes in SHA256SUMS.

### Sufficiency probe results (order_probe.py, 2026-07-31)

- Decoration seeds: identical per chunk across bundles (order-free). ✓
- Every grid chunk whose 07 dump differs between bundles has a differing
  distance-2 decoration-order prefix — zero unexplained diffs. ✓
- Concrete spill evidence: c.-1.0 — neighbor (-1,1) decorated before it in
  the alt bundle only; alt's 07 dump shows that neighbor's ore-vein spills
  (deepslate_redstone_ore / deepslate_iron_ore / tuff) along the z=15
  border facing (-1,1); absent in primary. 2191 differing blocks, 434 on
  borders. ✓
- 07 snapshots: clean (seqBegin==seqEnd) and exactly at own manifest
  event + 1 in both bundles. ✓
- No vanilla out-of-window access warnings in either run's log (the A4
  detector strings) — the 3×3 write-window argument held empirically. ✓

### What the C replay (Task 9) must consume

1. Replay features applications in manifest seq order (single-threaded
   replay is the actual recorded order).
2. Before decorating a chunk: recompute the decoration seed per A3
   (Xoroshiro128++ delegate; WorldgenRandom.nextLong burns two xoro draws)
   and assert equality with the manifest's seed column.
3. Snapshot each grid chunk's blocks/heightmaps at ITS OWN decoration
   completion (not after all decorations) and compare to `07_features.*` —
   spill-ins with smaller seq included, larger seq excluded.
4. Do this for BOTH bundles; matching both distinct orders bit-exactly is
   the Tier-2 evidence (ADR-007 D3).

## Environment

- MC 26.2 (`TARGET_VERSION`), server sha1 823e2250d24b3ddac457a60c92a6a941943fcd6a
- Fabric loader 0.19.3, installer 1.1.2, JDK 25.0.3 (Ubuntu 24.04)
- Overworld y-range confirmed: -64..319 (384 sections of data unchanged from 1.21)
- Gamerules pinned by harness: random_tick_speed=0, spawn_mobs=false,
  advance_weather=false, advance_time=false (26.2 snake_case names)
