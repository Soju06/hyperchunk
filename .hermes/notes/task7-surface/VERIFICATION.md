# Task 7 (05_surface) — verification record, 2026-07-30

Final state: 433e869. Gates: 16/16 ctest, 9/9 chunks 0 diff (55,779 golden
lines, blocks+heightmaps, chained from OUR 04_noise state), check_no_fma
PASS (14,641 insns), ASan+UBSan full-suite PASS, tree clean.

## Adversarial review (4 lenses vs A1-A6, javap tiebreak)

- buildSurface/extensions, Context/conditions, compile, biome+temp: **0
  confirmed algorithmic deviations**. The biome/temp reviewer ran a live
  differential harness against the real 26.2 server classes: zoom (6,000
  random + 2,916 edge positions, 2 seeds) and temperature (cold biomes,
  high y, NONE+FROZEN) bit-identical; obfuscateSeed 7 seeds identical.
- Fixed from review (commit 433e869): exact JDK Math.round bit algorithm in
  getBand (floor(v+0.5) diverges at exactly v=0.49999999999999994 — A1 §3
  annotated), 1-byte stack OOB in block_state_id (unreachable w/ shipped
  data), fail-loud on biome #tags, empty sequence accepted as vanilla no-op.
- Verified design deviation (value-neutral, documented in surface.c header):
  per-node lazy condition memos skipped; only steep is memoized (context
  singleton) because heightmaps mutate mid-stage. Reviewer could construct
  no counterexample.

## Mutation probes (committed state, sequential, main tree + build-probe)

| probe | unit | stage | verdict |
|---|---|---|---|
| P1 depth 2.75→2.76 | FAIL | FAIL | covered |
| P2 vgrad nextFloat→nextDouble | PASS | PASS | blind — same first nextLong, Δ<2⁻²⁴, ~25K rolls insufficient |
| P3 stone_depth 1→2 | PASS | FAIL | covered |
| P4 water ≥→> | PASS | PASS | blind — no exposed water surface in 9 chunks |
| P5 minSurfaceLevel −8→−7 | PASS | PASS | blind — boundary never decisive |
| P6 steep 4→3 | PASS | PASS | blind — no steep-gated biome |
| P7 2d sampler y=0→blockY | PASS | PASS | blind — noise_threshold all in non-jungle branches |
| P8 cold 0.15f→0.14f | PASS | PASS | blind — jungle never near threshold |
| P11 bands nextInt(5)→(4) | FAIL | PASS | unit covers bands; stage blind (no badlands) |
| P12 getBand ×4→×5 | PASS | PASS | blind — bandlands unreached, offset not golden-dumped |
| P14 h2→h1 | build-error (unused var) | — | structurally blind: h1==h2 without badlands |

Golden-blind branches rest on the note-review + live-harness evidence above.
Closing them for real needs a badlands/frozen-ocean/snowy-seed golden set
(see memory note surface-stage-gate-coverage).

## 26.2 deltas confirmed in the implementation

- noise_threshold `is_3d` (new): only sulfur_cave_gradient uses it; 3d
  sampler keyed on lastUpdateY, samples (x, blockY, z).
- Noises.SULFUR_CAVE_GRADIENT new; NO sulfur logic in SurfaceSystem itself —
  all in the rule/data layer.
- coldEnoughToSnow(BlockPos, int seaLevel) 2-arg; threshold seaLevel+17
  (== 80 only at default sea level).
- buildSurface tail param Set<Holder<Biome>> (possibleBiomes) — pure
  constant-folding optimization, safely skipped.
- updateY is the 4-int form; setBlockState 2-arg → int flags=3.
- Everything else structurally identical to 1.21 folklore (A1 §13).

## What Task 8 (carvers) inherits

- hc_surface_t + topMaterial: carvers call SurfaceSystem.topMaterial (A1
  §11 — reconstructed, NOT yet implemented in C; it builds a one-off
  context with possibleBiomes=null, stoneDepth 1/1). rule_apply/cond_test
  are reusable as-is.
- hc_biome_view_t is the biome getter carvers need (BiomeManager zoom).
- col_set/hc_hm_update_both handle non-opaque replacement (air carving
  lowers WORLD_SURFACE_WG via the rescan branch — written for this).
- markPosForPostProcessing is still a no-op — revisit for fluid carving.
- 06_carvers golden includes 26.2 sulfur-cave carving (FORMAT.md).
