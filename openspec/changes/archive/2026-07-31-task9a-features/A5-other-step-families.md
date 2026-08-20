# A5 — Other step-1/2/3/10 families in jungle & lush_caves (lakes, geode, monster rooms, freeze_top_layer)

Recon target: MC 26.2 unobfuscated server bytecode at `tools/golden/work/server`
(`javap -p -c -constants`). Datapack JSON from
`tools/golden/work/server/data/minecraft/worldgen/`. Everything below is
bytecode/JSON-verified unless explicitly marked UNVERIFIED.

Scope: 9a does NOT implement these features. Because `setFeatureSeed(decoSeed,
perStepListIndex, step)` re-seeds the per-feature random before every placed
feature (A2/A3), a skipped feature has **zero RNG effect** on any other feature.
The only cross-feature observable is **blocks it writes** (which later features
read back, and which update the 4 final heightmaps per A4). So what matters for
9a is: (a) how often each family fires, (b) what blocks it could contribute to
the empirical 07-vs-06 diff, (c) a 9b difficulty sketch.

Step/index positions (identical in jungle and lush_caves biome JSON):

| step | index | placed_feature |
|---|---|---|
| 1 (lakes) | 0 | lake_lava_underground |
| 1 (lakes) | 1 | lake_lava_surface |
| 2 (local_modifications) | 0 | amethyst_geode |
| 3 (underground_structures) | 0 | monster_room |
| 3 (underground_structures) | 1 | monster_room_deep |
| 10 (top_layer_modification) | 0 | freeze_top_layer |

---

## (a) Placement pipelines & per-chunk fire probability

All rarity draws below verified: `RarityFilter.shouldPlace` =
`random.nextFloat() < 1.0f/chance` (fcmpg, strict `<`). `UniformInt.sample` =
`Mth.randomBetweenInclusive` = `random.nextInt(max-min+1)+min` (exactly 1
nextInt draw).

| placed_feature | modifiers (order) | attempt probability / count per chunk |
|---|---|---|
| lake_lava_underground | rarity_filter(9) → in_square → height_range uniform(abs 0 .. below_top 0) → environment_scan(down, 32, non-air && inside_world_bounds(0,-5,0)) → surface_relative_threshold(OCEAN_FLOOR_WG, max −5) → biome | 1/9 ≈ 11.1% attempts; then must find non-air ≤32 down AND end ≥5 below OCEAN_FLOOR_WG → real fire rate a few % |
| lake_lava_surface | rarity_filter(200) → in_square → heightmap(WORLD_SURFACE_WG) → biome | 1/200 = 0.5% |
| amethyst_geode | rarity_filter(24) → in_square → height_range uniform(above_bottom 6 = y −58 .. abs 30) → biome | 1/24 ≈ 4.2% attempts; feature itself then aborts unless surroundings are solid (invalid_blocks_threshold=1) |
| monster_room | count(10) → in_square → height_range uniform(abs 0 .. below_top 0) → biome | 10 attempts EVERY chunk; each place() usually fails validation (needs solid floor+ceiling and 1–5 wall openings into air) |
| monster_room_deep | count(4) → in_square → height_range uniform(above_bottom 6 = −58 .. abs −1) → biome | 4 attempts EVERY chunk |
| freeze_top_layer | biome only | fires (place() called) EVERY chunk whose origin biome lists it; places nothing in warm biomes (see (b)) |

Note for the walk: none of these pipelines' draws need C implementation in 9a —
skipping the whole (step,index) burns nothing, because the next feature re-seeds.

---

## (b) Block palettes vs the empirical 07-vs-06 diff

Blocks each family can write (bytecode-verified against place()):

- **lake_lava** (`LakeFeature`, config lake_lava.json): `minecraft:lava[level=0]`
  (fluid, cells y<4 of the 16×16×8 blob box), `minecraft:air` (cells y≥4, also
  scheduleTick + markAboveForPostProcessing), `minecraft:stone` (barrier shell,
  border cells; y≥4 border only with 50% nextInt(2) roll). Ice pass is
  water-only — never runs for lava. Box origin = origin.offset(−8,−4,−8).
- **amethyst_geode** (`GeodeFeature`): `air` (filling + crack), `amethyst_block`
  (inner layer), `budding_amethyst` (alternate inner, p=0.083), `calcite`
  (middle), `smooth_basalt` (outer), plus `small/medium/large_amethyst_bud` and
  `amethyst_cluster` (facing=any of 6, waterlogged possible) on budding blocks.
  All writes via safeSetBlock gated on `!state.is(#features_cannot_replace)`.
- **monster_room** (`MonsterRoomFeature`): `air` (room interior + soft-floor
  carve), `cobblestone` / `mossy_cobblestone` (walls/floor), `chest` (0–2, with
  facing via StructurePiece.reorient), `spawner` (always at origin on success).
- **freeze_top_layer** (`SnowAndFreezeFeature`): `snow` (layer), `ice`, and
  SNOWY=true property rewrite on the block under placed snow. **In
  jungle/lush_caves it can never place anything** — verified gate below.

### freeze_top_layer temperature gate (bytecode-verified)

`SnowAndFreezeFeature.place`: 16×16 column loop; per column
`h = level.getHeight(MOTION_BLOCKING, x, z)` (a FINAL heightmap, live-updated
during features per A4); `pos1=(x,h,z)`, `pos2=pos1.below()`;
`biome = level.getBiome(pos1).value()` (per-column 3D biome lookup, not the
chunk-origin biome); then:

1. `biome.shouldFreeze(level, pos2, false)` → ICE at pos2
2. `biome.shouldSnow(level, pos1)` → SNOW at pos1; then if state at pos2
   `hasProperty(SNOWY)` → setBlock pos2 with SNOWY=true

Gate order (both short-circuit on temperature FIRST, before any light/world
query):

- `shouldFreeze(level,pos,false)`: `if (warmEnoughToRain(pos, seaLevel)) return
  false;` where `warmEnoughToRain = getTemperature(pos, seaLevel) >= 0.15f`
  (fcmpl iflt). Then requires block-light < 10, water source in a LiquidBlock;
  the `mustBeAtEdge` water-neighbour check is skipped (arg=false here).
- `shouldSnow(level,pos)`: `getPrecipitationAt(pos, seaLevel) == SNOW` where
  `getPrecipitationAt = hasPrecipitation() ? (coldEnoughToSnow ? SNOW : RAIN) :
  NONE` and `coldEnoughToSnow = !warmEnoughToRain` — i.e. temperature < 0.15f.
  Then build-height, block-light < 10, air-or-snow at pos, and
  `SNOW.defaultBlockState().canSurvive(level,pos)`.
- `getTemperature(pos, seaLevel)` → per-thread 1024-entry LRU cache →
  `getHeightAdjustedTemperature`: `t = temperatureModifier.modifyTemperature(pos,
  baseTemperature)`; let `y0 = seaLevel + 17`; if `pos.y > y0`:
  `t -= (TEMPERATURE_NOISE.getValue(x/8f, z/8f, false)*8 + y − y0) * 0.05f /
  40.0f` (i.e. ×0.00125). TEMPERATURE_NOISE is the static PerlinSimplexNoise
  (seed 1234, octave {0}) already implemented in `core/src/biome_temp.c`.

Numeric consequence: jungle base temp 0.95, lush_caves 0.5 (biome JSON). Max
possible reduction within build height (y=320, sea 63, noise ≥ −1):
(−8 + 320 − 80) × 0.00125 ≈ 0.29 → 0.95→0.66, 0.5→0.21, both ≥ 0.15f. So
**neither biome can ever pass coldEnoughToSnow / !warmEnoughToRain at any y in
the world** — freeze_top_layer contributes ZERO blocks to any jungle/lush chunk.
(If a cold biome column intrudes into the 16×16 area from 3D biome blending, the
per-column `getBiome` would use that biome — in an all-warm golden region this
cannot happen.)

Also: `place()` unconditionally `return true` (iconst_1) — the trace golden will
record placed=true for freeze_top_layer in every chunk even though nothing was
written, and it burns **zero RNG draws** (no `random` use anywhere in the
method).

---

## (c) 9b handoff — place() sketches, exact draw sequences, difficulty

### LakeFeature (lake_lava_underground / lake_lava_surface) — difficulty M

Draw sequence on the per-feature WorldgenRandom (`random` = context.random()):

1. Pre-draw guard: `if (origin.y <= level.getMinY()+4) return false` — BEFORE
   any draw.
2. `pos = origin.offset(-8,-4,-8)`; `boolean[2048] mask` (index
   `(x*16+z)*8+y`).
3. `n = random.nextInt(4) + 4` blobs. Per blob exactly 6 × `random.nextDouble()`:
   `a=d*6+3, b=d*4+2, c=d*6+3`, center `cx=d*(16−a−2)+1+a/2`,
   `cy=d*(8−b−4)+2+b/2`, `cz=d*(16−c−2)+1+c/2`; then a draw-free
   x∈[1,14]×z∈[1,14]×y∈[1,6] ellipsoid rasterize into mask
   (((x−cx)/(a/2))²+((y−cy)/(b/2))²+((z−cz)/(c/2))² < 1).
4. `fluidState = config.fluid.getState(...)` — SimpleStateProvider.getState
   returns the field, **0 draws** (verified).
5. Validation loop 16×16×8 (0 draws, world reads): for each cell that is
   border-of-mask (self clear, any of 6 neighbours set): read
   `getBlockState(pos.offset(x,y,z))`; abort `return false` if (y≥4 && liquid)
   or (y<4 && !isSolid && state != fluidState) or
   `!config.canPlaceFeature.test` (= BlockPredicate `true` for lava lakes).
   NOTE: aborts happen AFTER the 1+6n draws — draw count before abort is
   world-independent.
6. Fill loop 16×16×8 (0 draws): masked cells passing
   `canReplaceWithAirOrFluid` (= NOT #features_cannot_replace): y≥4 → setBlock
   AIR + `scheduleTick(pos, AIR-block, 0)` + markAboveForPostProcessing; y<4 →
   setBlock lava.
7. Barrier pass: `barrier = config.barrier.getState(...)` (stone, 0 draws);
   skipped entirely if barrier isAir. For every border-of-mask cell: if local
   y≥4 → `random.nextInt(2)`, skip cell on 0 (draw count depends ONLY on the
   mask, not on the world); then (both y<4 and surviving y≥4) if world state
   isSolid && `canReplaceWithBarrier` (= NOT #lava_pool_stone_cannot_replace) →
   setBlock stone.
8. Water-ice pass only if fluid is #water — never for lava. `return true`.

Needs: 2 block-tag predicates, isSolid/liquid on our palette, heightmap-updating
setBlock (exists per A4). RNG usage is plain wrapper nextInt/nextDouble. M.

### GeodeFeature (amethyst_geode) — difficulty L

Feature-random draw sequence (in order):

1. `pointCount = distributionPoints.sample(random)` — default UniformInt(3,4)
   (JSON omits it; codec default verified) → 1 nextInt(2) draw.
2. Builds a **separate** `WorldgenRandom(new LegacyRandomSource(level.getSeed()))`
   and `NormalNoise.create(that, -4, [1.0])` — consumes NOTHING from the feature
   random. `NormalNoise.create(RandomSource,int,double...)` →
   `create(rand, params)` → ctor flag **true** → `PerlinNoise.create` (new
   factory) twice; `WorldgenRandom.forkPositional` delegates to the wrapped
   LegacyRandomSource. Same world seed ⇒ same noise for every geode; C can
   precompute once per world. (Exact PerlinNoise.create/fromHashOf("octave_-4")
   internals for the legacy positional factory: UNVERIFIED here, walk in 9b;
   `hc_lcg` + legacy octave init already exist in core.)
3. `random.nextDouble()` — crack size term:
   `1/sqrt(baseCrackSize(2.0, codec default) + nextDouble()/2 + (pointCount>3 ? d0 : 0))`
   where `d0 = pointCount / outerWallDistance.maxInclusive()(=6)`.
4. `random.nextFloat() < 0.95` (generate_crack_chance) → hasCrack.
5. Per point i < pointCount: 3 × `outerWallDistance.sample(random)` (uniform
   4..6 → nextInt(3)+4 each); world read `getBlockState(origin.offset(dx,dy,dz))`;
   if air or in #geode_invalid_blocks → `invalidCount++`, and if
   invalidCount > 1 (invalid_blocks_threshold) → **return false mid-loop**
   (draws burned so far are world-dependent); else 1 × `pointOffset.sample`
   (default UniformInt(1,2) → 1 draw) appended with the point.
6. If hasCrack: `random.nextInt(4)` selects 1 of 4 crack-point layouts (3
   fixed offsets each, no further draws).
7. Main loop `BlockPos.betweenClosed(origin+(-16,-16,-16), origin+(16,16,16))`
   = 33³ = 35937 cells, iteration order x-major then y then z (betweenClosed).
   Per cell: `noise.getValue(x,y,z) * 0.05` (noise_multiplier default) + sum
   over points of `invSqrt(distSqr(point)+pointOffset)` (and for crack points
   `invSqrt(distSqr+2)`); `Mth.invSqrt(double)` = **org.joml.Math.invsqrt** —
   verify its exact semantics (1.0/sqrt vs Newton fast-inverse) in 9b
   (UNVERIFIED which; joml default is 1.0/java.lang.Math.sqrt).
   Layer thresholds (layers {} → codec defaults filling 1.7, inner 2.2, middle
   3.2, outer 4.2; each as `1/sqrt(L + d0-adjustment)`). Per-cell actions:
   - crack cells (hasCrack && crackValue ≥ crackThreshold && value < fillingThr):
     safeSetBlock AIR + per-6-direction fluid check → scheduleTick (world reads).
   - value ≥ filling: safeSetBlock filling (air; SimpleStateProvider 0 draws).
   - value ≥ innerLayer: `random.nextFloat() < 0.083` → budding_amethyst else
     amethyst_block (**1 draw per inner-shell cell**, count independent of
     world); then since placements_require_layer0_alternate defaults **true**
     (codec verified) the second roll `random.nextFloat() < 0.35`
     (use_potential_placements_chance default) only happens for cells that
     chose budding — collects pos into potential-placements list.
   - value ≥ middleLayer: calcite; value ≥ outerLayer: smooth_basalt.
8. Per collected pos: `Util.getRandom(innerPlacements(4), random)` →
   `nextInt(4)` (1 draw), then scan 6 directions in Direction.values() order:
   first neighbour with `BuddingAmethystBlock.canClusterGrowAtState` gets the
   bud (facing=direction, waterlogged=fluidState.isSource) and breaks.
9. `return true` always (after step 5 passes).

Needs: legacy-seeded NormalNoise, joml invsqrt, geode tags, budding-growth
predicate, 35937-cell loop. Draw count large but mostly world-independent;
the world-dependent early exit is confined to step 5. **L**.

### MonsterRoomFeature (monster_room / monster_room_deep) — difficulty M (S/M for block-grid-only)

Draw sequence:

1. `xr = random.nextInt(2)+2`, `zr = random.nextInt(2)+2` (2 draws). Room box
   x∈[−xr−1, xr+1], y∈[−1,4], z∈[−zr−1, zr+1] around origin.
2. Validation loop (0 draws, world reads): y==−1 or y==4 cell not solid →
   `return false` immediately; counts "openings" = boundary cells at y==0 with
   `isEmptyBlock(pos) && isEmptyBlock(pos.above())`. After loop: openings <1 or
   >5 → `return false`. (So the common no-cave case costs exactly 2 draws.)
3. Carve loop x asc, y 3→−1 desc, z asc: boundary cells (x/y/z at box edge):
   if `pos.y >= minY && !below.isSolid` → setBlock AIR (no draw); else if
   state isSolid && !chest: **y==−1 → `random.nextInt(4)`; ≠0 →
   mossy_cobblestone else cobblestone** (draw count is WORLD-DEPENDENT — one
   per solid non-chest floor-boundary cell); y≠−1 → cobblestone (no draw).
   Interior cells → AIR unless chest/spawner (no draw).
4. Chest loop: 2 iterations × up to 3 tries; per try
   `random.nextInt(xr*2+1)` and `random.nextInt(zr*2+1)` (2 draws), pos must be
   `isEmptyBlock` with EXACTLY 1 solid horizontal neighbour; on success:
   safeSetBlock chest (facing via `StructurePiece.reorient`, world reads) then
   `RandomizableContainer.setBlockEntityLootTable` → **`random.nextLong()`**
   (verified; loot seed, block-entity data only) and break to next iteration.
5. Spawner: safeSetBlock SPAWNER at origin (BEFORE any further draw), then
   `getBlockEntity`; if SpawnerBlockEntity: `randomEntityId` →
   `Util.getRandom(MOBS[4], random)` → `nextInt(4)` (skeleton, zombie, zombie,
   spider); `setEntityId` → `getOrCreateNextSpawnData` →
   `WeightedList.getRandom` on default-empty spawnPotentials → returns
   Optional.empty **without drawing** (verified null-selector early return).
6. `return true`.

Block-grid-only shortcut: everything after the last grid write (loot nextLong,
spawner entity id) affects only block-entity NBT, and the RNG is re-seeded for
the next feature — so a grid-parity C impl may skip draws 4b/5 entirely AFTER
emitting the chest/spawner blocks. Chest facing (reorient) does affect the grid
(chest blockstate). **M** full, **S/M** grid-only.

### SnowAndFreezeFeature (freeze_top_layer) — difficulty S

Zero RNG draws. Per 16×16 column: MOTION_BLOCKING height read, per-column
`getBiome`, temperature gate (simplex already in `core/src/biome_temp.c`),
`shouldFreeze` water checks (block+fluid state reads), `shouldSnow` +
`SnowLayerBlock.canSurvive` + SNOWY property rewrite. Block-light gate: during
decoration the light engine hasn't run (light stage is after features), so
`getBrightness(BLOCK,·)` is 0 < 10 — delegation chain verified
(BlockAndLightGetter → ServerLevel light engine); the runtime-0 claim is
UNVERIFIED but irrelevant for warm biomes (temperature short-circuits first).
`return true` unconditionally. **S** — but pointless before a cold-biome golden
region exists.

---

## Implications for the C implementation (9a)

1. **Skipping is RNG-safe.** All four families are skippable in 9a with zero
   effect on other features' draws (per-feature re-seed, A2/A3). No placement
   modifier of theirs needs porting for the walk.
2. **Skipping is NOT grid-safe when they fire.** lake_lava_underground (~1/9
   attempts), amethyst_geode (~1/24), monster_room (14 validation attempts per
   chunk, occasional success near caves) DO write blocks in ordinary chunks;
   any chunk where vanilla fired one will diverge in the empirical 07-vs-06
   diff and can cascade into later features via world reads and the live
   final-heightmap updates (A4). The trace golden's per-(step,index)
   placed=true/false flags identify those chunks — the 9a gate should treat
   placed=true on any of {1/0, 1/1, 2/0, 3/0, 3/1} as "chunk not golden-covered
   for grid comparison", not as failure.
3. **Empirical diff attribution palette**: lava/stone/air (lakes),
   air/amethyst_block/budding_amethyst/calcite/smooth_basalt/buds/cluster
   (geode), cobblestone/mossy_cobblestone/air/chest/spawner (monster room).
   Any of these in the 07-vs-06 diff is expected leakage from skipped families,
   not an ores/blobs bug.
4. **freeze_top_layer is fully inert in jungle/lush_caves**: temperature gate
   (base 0.95 / 0.5 vs < 0.15f, max altitude reduction ≈ 0.29 at y=320) makes
   snow/ice impossible; zero draws; place() returns true. The trace will show
   step 10 index 0 placed=true with no grid effect — the C walk can hard-skip
   it and still match the grid exactly; only the trace-manifest entry needs to
   come from vanilla's recording.
5. **9b order of attack** (cheapest observable-parity per effort):
   SnowAndFreeze (S, needs cold-biome region) → MonsterRoom grid-only (S/M) →
   Lake (M) → Geode (L, legacy NormalNoise + joml invsqrt are the two
   new primitives; both must be bytecode-walked in 9b: PerlinNoise.create
   new-factory path over LegacyRandomSource.forkPositional, and
   org.joml.Math.invsqrt exact double semantics).
