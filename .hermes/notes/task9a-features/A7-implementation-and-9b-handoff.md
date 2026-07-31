# A7 — Task 9a implementation record + 9b handoff

Written at 9a completion (2026-07-31). Companions: A1–A6 (recon, this dir),
golden/features-trace/FORMAT.md (trace golden), tests/parity/
test_features_walk.c (the 3-part gate).

## 1. What 9a shipped

- `core/src/features_rng.c` — WorldgenRandom-over-Xoroshiro (A3 semantics:
  top-bits next(bits), 2-draw nextLong, BitRandomSource nextInt, deco/feature
  seeding; both Mth int helpers with their different degenerate-range draws).
  Unit gate: tests/unit/test_features_rng.c (A3 §6 vectors).
- `core/src/jdk_trig.c` — HotSpot x86-64 Math.sin/cos intrinsic stubs
  (Intel LIBM) transcribed to C. glibc sin/cos differs from the stub by 1 ulp
  on ~0.5% of the ore-angle domain (21/4099 golden vectors) — a real parity
  break the port closes. Gate: golden/rng/jdk_sincos.txt (this machine's JDK
  25 output) + a 65281-vector dense set at port time, 0 mismatches.
- `core/src/features_compile.c` — reference JSON → registry: FeatureSorter
  table (features_order-26.2.txt), per-biome per-step membership bitsets
  (biome_features-26.2.json; nether/end biomes zeroed — can't appear in
  overworld chunks), pipelines+bodies for ALL step 0..8 features (10 modifier
  opcodes, 7 predicate kinds, uniform/trapezoid/very_biased_to_bottom heights,
  const/uniform int providers). Grid-unreachable exotics (live FINAL heightmap
  reads, clamped_normal) compile to die-if-executed markers.
- `core/src/features.c` — region (3×3 write window, y-OOB=AIR reads, WG
  heightmap reads), depth-first pipeline executor (A2 §1.2 loop nest),
  bodies: ORE (full A3 semantics incl. discard draws, BitSet visited,
  BulkSectionAccess-equivalent raw writes with NO heightmap updates),
  SPRING (zero-RNG world gates), UNDERWATER_MAGMA (floor scan + 27 draws +
  full-face occlusion), MONSTER_ROOM validation phase (2 draws; dies loudly
  if validation ever passes — success path is 9b).
- `core/src/gen_features_stage.c` — the per-chunk decoration walk: deco seed,
  3×3 stored-quart biome union, per-step sorted index iteration,
  setFeatureSeed accounting. Steps 9/10 skipped (walk_max_step=8) — RNG-safe
  because every item reseeds (A5).
- Trace golden harness (make_feature_trace.sh + FeatureTrace mod hooks) and
  the bundle under golden/features-trace/ — see FORMAT.md. Grid stickiness
  proven per run (171/171 committed dump hashes reproduced).

## 2. Gate results (test_features_walk, both bundles)

- (a) decoration seeds: 81/81 primary + 81/81 alt manifest lines recomputed
  and matched (not just the 9 grid chunks).
- (b) trace vs vanilla (primary bundle, steps ≤8): 37 f-lines + ~320 p-lines
  per chunk; positions, npos, ids all exact. ONE placed-bit drift grid-wide
  (see §3). Structure placements in grid traces: 0 (9a assumption verified).
- (c) blocks vs golden 07 (BOTH orders): primary 42,579 / alt 42,843 blocks
  placed; ZERO hard mismatches. The same code replaying two different
  recorded orders matches both bundles' 07 dumps on every cell not
  overwritten by step-9/10 — early Tier-2 evidence (ADR-007 D3) for the
  ore/blob+magma+spring surface.

### Residual categories (positions where golden 07 ≠ our replay, all
### attributable to steps 9/10 — the exact 9b work list)

| category (g7 value) | primary | alt |
|---|---|---|
| block outside our table (moss, vines, logs, leaves, dripleaf, azalea, …) | 12,199 | 12,599 |
| clay (lush_caves_clay patches / pools) | 1,145 | 1,145 |
| water[level=0] (vegetation-patch pool digging) | 62 | 62 |
| dirt (tree below_trunk forcing) | 125 | 126 |

Heightmap comparison is NOT part of the 9a gate: 07 heightmap dumps carry the
4 FINAL maps, which step-9 writes (trees) reshape and which our walk does not
yet maintain (nothing in steps ≤8 reads them; ores bypass them by design).

## 3. The one placed-bit drift — root cause (fully diagnosed)

`c.1.1` step 6 idx 13 (ore_iron_small) position (26,1,18): our body placed
iron_ore at cell (26,0,18) (cur=andesite from c.1.1's own idx-7 blob);
vanilla placed nothing. Ore-candidate ground truth (OreFeatureMixin o-lines,
debug run): vanilla saw **moss_block** at that cell and 40+ others — an
earlier-decorated NEIGHBOR's step-9 lush_caves vegetation spilled moss into
c.1.1 before it decorated. Our walk_max_step=8 world cannot contain those
spills. Pipeline draws are state-independent for ores, so positions never
drift — only the body's rule-test outcome can. The gate therefore requires
positions exact and caps placed-bit drifts at 2 (measured: 1), each printed.
Implementing step 9 in 9b makes the spills real and drops the cap to 0.

The drifted write landed in an already-snapshotted neighbor / hidden under
golden moss, hence zero effect on gate (c) — the trace ladder caught what the
block gate could not, exactly as designed.

## 4. Empirical facts 9b should not re-derive

- Sticky 1-thread order held again for BOTH trace runs at radius=2 with
  extra I/O (grid prefix + all grid 01..07 dumps byte-identical). The trace
  harness re-proves it every run; if it ever breaks, traces refuse to land.
- Stored-quart biome truth: the surface-golden `quart_biomes` probe differs
  from the stored chunk palettes at exactly 1/24,576 ring quarts
  ((8,-5,9): probe=jungle, stored=lush_caves). Decoration reads STORED
  palettes (union and BiomeFilter both); the walk test builds its view from
  03_biomes dumps and keeps the probe as a strict cross-check for grid
  chunks only. Palette-vs-stored union (A1 open question) remains open but
  is provably moot for steps ≤8 (unions invariant).
- Beach/river biomes enter 5 grid chunks' unions but change nothing at
  steps ≤8 (their contributions are subsets — A6 §6).
- underwater_magma: 161 pipeline survivors, 2 clusters placed (13 blocks,
  one straddling the c.1.-1/c.1.0 border) — reproduced exactly.
- springs: 405 pipeline survivors, 0 placements (rocks/holes gates), 0 draws.
- monster_room(_deep): 126 body entries, all validation failures (2 draws
  each); lakes/geode/disks: 0 body entries grid-wide.

## 5. 9b work list (features to green the full 07 gate)

Ordered by expected leverage (residual volume × difficulty):

1. **FINAL heightmap maintenance** (substrate, not a feature): prime 4 maps
   at decoration start from blocks + incremental update on flag-2/3 writes
   (A4 §4). Read by trees (live OCEAN_FLOOR/MOTION_BLOCKING), heightmap
   placement at step 9, surface_water_depth_filter. Also needed for the 07
   heightmaps gate.
2. **Lush-caves vegetation family** (~most of the moss/clay/water/dirt +
   azalea/dripleaf/spore residuals): vegetation_patch/waterlogged_
   vegetation_patch (clay_pool_with_dripleaves, lush_caves_clay/vegetation),
   moss_patch, cave_vines (block_column + weighted providers), spore_blossom
   (simple_block), rooted_azalea_tree (root_system → inline azalea tree),
   glow_lichen (multiface_growth), classic_vines_cave_feature. Difficulty M
   each; state providers (weighted_state_provider draws!) and dual noise
   providers need recon.
3. **Trees + jungle vegetation** (the bulk of the not-in-table palette):
   trees_jungle (random_selector → checked tree placed features), tree
   feature machinery (trunk/foliage placers, RNG-heavy), bamboo, cocoa
   attachment, patch_* (random_patch + simple_block), flowers, mushrooms,
   melon/pumpkin, sugar cane, firefly bush, vines. Difficulty L (largest
   single surface; trunk placers draw heavily and read/write heightmaps).
4. **Step-9 modifier/provider gaps**: surface_water_depth_filter (live maps),
   noise_threshold_count (Biome.BIOME_INFO_NOISE — static PerlinSimplexNoise,
   needs its own recon), random_offset already done, weighted_list int/state
   providers, count_extra? (census per A2 §0.2).
5. **freeze_top_layer** (step 10): provably inert in this grid (temperature
   gate, A5) — implement as the zero-draw walk item + trace f-line only.
6. **Rarely-firing step 1/2/3 bodies** (lake M, geode L incl. legacy
   NormalNoise + joml invsqrt, monster_room success path S/M, fossils?):
   needed only when a golden region actually fires them — current grid
   doesn't. Keep die-if-reached markers until then; A5 has the sketches.
7. **Ring-chunk decoration + full Tier-2**: replay ALL 81 manifest entries
   (ring chunks decorate after the grid in both bundles, so grid 07 gates
   don't need them — but 08+ gates and other regions will). Needs ring 06
   inputs at distance 3-4 (extend the trace harness radius) and biome data
   beyond the current 5×5 view (a 7×7 view or C-side multi-noise biomes).
8. **Palette-vs-stored biome union probe** (A1 open question): a one-line
   mixin probe on PalettedContainer.getAll vs stored values would close it;
   only matters once step-9 membership differences could bite (plains-default
   palette ghosts would add step-9 features to every union).

Bodies/opcodes that stay die-if-reached until a region needs them:
clamped_normal int provider, live-FINAL heightmap placement for
desert_well/forest_rock/ice_patch/ice_spike, structure placement (no starts
in this grid — verified), ScatteredOreFeature (nether ancient debris; NOTE
its writes use the heightmap-updating setBlock path unlike OreFeature).

## 6. Debug tooling added for 9b

- `-Dhyperchunk.dump.oretrace=true` (with trace mode): per-canPlaceOre
  o-lines `o step idx x y z state result` — vanilla candidate/state ground
  truth. Not part of the canonical bundle format (flag off in harnesses).
  This is what pinned the moss-spill root cause in minutes; use it first
  when a placed-bit or block diff appears.
