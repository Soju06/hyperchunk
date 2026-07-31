# A4 — SpringFeature, DiskFeature, UnderwaterMagmaFeature (26.2 bytecode recon)

Sources: `javap -p -c -constants` against `tools/golden/work/server` class tree; datapack JSON at
`tools/golden/work/server/data/minecraft/worldgen/`. Everything below is bytecode/JSON-verified
unless explicitly marked UNVERIFIED.

Classes:
- `net/minecraft/world/level/levelgen/feature/SpringFeature.class`
- `net/minecraft/world/level/levelgen/feature/DiskFeature.class`
- `net/minecraft/world/level/levelgen/feature/UnderwaterMagmaFeature.class`
- configs in `.../feature/configurations/{Spring,Disk,UnderwaterMagma}Configuration.class`
- NOTE (26.2 rename): the rule-based provider is `stateproviders/RuleBasedStateProvider`
  (NOT `RuleBasedBlockStateProvider` as in 1.19–1.21), and it is a regular registered
  `BlockStateProvider` subtype (`minecraft:rule_based_state_provider`), used directly inside
  `DiskConfiguration.state_provider`.

## 0. Which JSONs use which feature type (26.2 datapack)

| configured_feature | type | notes |
|---|---|---|
| `disk_clay` | `minecraft:disk` | simple_state_provider(clay); target matching_blocks[dirt,clay]; half_height 1; radius uniform(2,3) |
| `disk_gravel` | `minecraft:disk` | simple_state_provider(gravel); target matching_blocks[dirt,grass_block]; half_height 2; radius uniform(2,5) |
| `disk_sand` | `minecraft:disk` | rule_based_state_provider (rule: air at offset (0,-1,0) → sandstone; fallback sand); target matching_blocks[dirt,grass_block]; half_height 2; radius uniform(2,6) |
| `disk_grass` | `minecraft:disk` | rule_based (rule: not(any_of(solid@(0,1,0), matching_fluids water@(0,1,0))) → grass_block[snowy=false]; fallback dirt); target matching_blocks[dirt,mud]; half_height 2; radius uniform(2,6) — mangrove-swamp only, listed for completeness |
| `spring_water` | `minecraft:spring_feature` | state = fluid `minecraft:water` [falling=true]; valid_blocks = [stone,granite,diorite,andesite,deepslate,tuff,calcite,dirt,snow_block,powder_snow,packed_ice] |
| `spring_lava_overworld` | `minecraft:spring_feature` | state = fluid `minecraft:lava` [falling=true]; valid_blocks = [stone,granite,diorite,andesite,deepslate,tuff,calcite,dirt] |
| `spring_lava_frozen` | `minecraft:spring_feature` | lava; valid_blocks = [snow_block,powder_snow,packed_ice] — frozen biomes only |
| `underwater_magma` | `minecraft:underwater_magma` | floor_search_range 5, placement_radius_around_floor 1, placement_probability_per_valid_position 0.5 |

`ore_clay` (lush_caves step-6 idx 26) is `type: minecraft:ore` — NOT a disk; belongs to the ores note.

`SpringConfiguration` codec (bytecode-verified defaults; the shipped JSONs omit all three):
`state` (FluidState.CODEC, required), `requires_block_below` (BOOL, **default true**),
`rock_count` (INT, **default 4**), `hole_count` (INT, **default 1**), `valid_blocks`
(homogeneousList(BLOCK), required). So all overworld springs run with
requiresBlockBelow=true, rockCount=4, holeCount=1.

`DiskConfiguration` codec: `state_provider` (BlockStateProvider.CODEC), `target`
(BlockPredicate.CODEC), `radius` (`IntProviders.codec(0, 8)`), `half_height`
(`Codec.intRange(0, 4)`). Record accessor order in ctor: stateProvider, target, radius, halfHeight.

`UnderwaterMagmaConfiguration` codec: `floor_search_range` (intRange), `placement_radius_around_floor`
(intRange), `placement_probability_per_valid_position` (floatRange). All required in the shipped JSON.

## 1. Biome membership / step / per-step list index (jungle & lush_caves)

Both `biome/jungle.json` and `biome/lush_caves.json`:

- Step **6** (UNDERGROUND_ORES): jungle → idx 25 `underwater_magma`, 26 `disk_sand`, 27 `disk_clay`,
  28 `disk_gravel`. lush_caves → idx 25 `underwater_magma`, 26 `ore_clay`, 27 `disk_sand`,
  28 `disk_clay`, 29 `disk_gravel`. (Index feeds `setFeatureSeed(decoSeed, perStepListIndex, step)` —
  see A2. Note the off-by-one between the two biomes from `ore_clay`; the per-step index is taken from
  the merged featuresPerStep indexing in A2, these JSON indices are the biome-local list positions.)
- Step **8** (FLUID_SPRINGS): idx 0 `spring_water`, idx 1 `spring_lava` in both biomes.

## 2. Placement modifier chains (types only; pipeline mechanics in the pipeline note)

- `placed_feature/disk_clay.json`, `disk_gravel.json`: `in_square` → `heightmap(OCEAN_FLOOR_WG)` →
  `block_predicate_filter{matching_fluids: minecraft:water}` → `biome`. (No count → 1 attempt.)
- `disk_sand.json`: `count(3)` → same tail as above.
- `disk_grass.json`: `count(1)` → `in_square` → `heightmap(OCEAN_FLOOR_WG)` →
  `random_offset(xz 0, y −1)` → `block_predicate_filter{matching_blocks: mud}` → `biome`.
- `spring_water.json`: `count(25)` → `in_square` → `height_range(uniform(above_bottom 0 → absolute 192))` → `biome`.
- `spring_lava.json` (→ configured `spring_lava_overworld`): `count(20)` → `in_square` →
  `height_range(very_biased_to_bottom(above_bottom 0 → below_top 8, inner 8))` → `biome`.
- `underwater_magma.json`: `count(uniform(44,52))` → `in_square` →
  `height_range(uniform(above_bottom 0 → absolute 256))` →
  `surface_relative_threshold_filter{heightmap OCEAN_FLOOR_WG, max_inclusive −2}` → `biome`.

**Negative finding:** in 26.2 the disks do **NOT** use `surface_water_depth_filter` (my 1.18-era
memory said they might). They use `block_predicate_filter` (matching_fluids water at the post-heightmap
position). `SurfaceWaterDepthFilter.class` exists but none of these placed features reference it.

### surface_relative_threshold_filter (unique to underwater_magma here — reconstructed fully)

`placement/SurfaceRelativeThresholdFilter.shouldPlace(ctx, random, pos)` — draws NO RNG:

```
long h    = (long) ctx.getHeight(heightmapType, pos.getX(), pos.getZ());
long lo   = h + (long) minInclusive;   // long arithmetic — no overflow with INT_MIN default
long hi   = h + (long) maxInclusive;
return lo <= pos.getY() && pos.getY() <= hi;
```

Codec: `heightmap` required; `min_inclusive` optional default `-2147483648` (Integer.MIN_VALUE);
`max_inclusive` optional default `2147483647`. underwater_magma → keep pos iff
`y <= OCEAN_FLOOR_WG(x,z) − 2` (min side always passes). C impl must do the sum in 64-bit.

## 3. SpringFeature.place — full reconstruction

**ZERO RNG draws.** `random` is never loaded in `place`. Everything is world-state-gated:

```
cfg = ctx.config; level = ctx.level; origin = ctx.origin;
// gate 1: block ABOVE origin must be in validBlocks
if (!level.getBlockState(origin.above()).is(cfg.validBlocks)) return false;
// gate 2 (requiresBlockBelow = true for all shipped springs):
if (cfg.requiresBlockBelow && !level.getBlockState(origin.below()).is(cfg.validBlocks)) return false;
// gate 3: origin itself must be air OR a valid block
s = level.getBlockState(origin);
if (!s.isAir() && !s.is(cfg.validBlocks)) return false;
// count rocks among the 5 neighbors, read order: west, east, north, south, below
rocks = Σ [level.getBlockState(n).is(cfg.validBlocks)]  for n in {W,E,N,S,down}
// count holes, same order: west, east, north, south, below
holes = Σ [level.isEmptyBlock(n)]                       for n in {W,E,N,S,down}
   // isEmptyBlock = getBlockState(pos).isAir()  (LevelReader default, verified)
placed = 0;
if (rocks == cfg.rockCount /*4*/ && holes == cfg.holeCount /*1*/) {
    level.setBlock(origin, cfg.state.createLegacyBlock(), 2);   // flag 2
    level.scheduleTick(origin, cfg.state.getType(), 0);         // fluid tick, delay 0
    placed++;
}
return placed > 0;
```

- Since rockCount+holeCount = 5 = number of checked neighbors, the shipped springs fire iff
  **exactly 4 of {W,E,N,S,down} are valid blocks and exactly 1 is air** (blocks that are neither —
  e.g. water, ores, gravel — make both counts miss).
- Block written: `cfg.state.createLegacyBlock()`. Chain verified: `FluidState.createLegacyBlock` →
  `Fluid.createLegacyBlock(state)`; `WaterFluid` → `Blocks.WATER.defaultBlockState().setValue(LEVEL,
  getLegacyLevel(state))`, `LavaFluid` → same with `Blocks.LAVA`. `FlowingFluid.getLegacyLevel`:
  `isSource() ? 0 : 8 - min(amount,8) + (falling ? 8 : 0)`. `minecraft:water`/`minecraft:lava` are the
  Source fluids (`WaterFluid$Source.isSource(state)` returns constant true), so despite
  `falling=true` in the JSON the block written is plain **`water[level=0]` / `lava[level=0]`**.
- `scheduleTick(pos, Fluid, 0)` records a pending **fluid tick** (ProtoChunk `fluid_ticks` NBT) —
  NOT visible in the 07 blocks/heightmaps dumps. Flagged, not deep-dived.
- All 8 distinct positions read are within ±1 of origin — always inside the 3×3 WorldGenRegion window.

## 4. DiskFeature.place — full reconstruction

**Exactly ONE RNG draw, always** (before any world reads): the radius sample. State providers used by
the shipped disks draw nothing (verified below).

```
cfg = ctx.config; origin = ctx.origin; level = ctx.level; random = ctx.random;
placed = false;
y0   = origin.getY();
top  = y0 + cfg.halfHeight();          // scan start (inclusive)
bot  = y0 - cfg.halfHeight() - 1;      // scan end (exclusive: loop while y > bot)
r    = cfg.radius().sample(random);    // DRAW #1: UniformInt.sample
        // UniformInt.sample = Mth.randomBetweenInclusive(random, min, max)
        //                   = random.nextInt(max - min + 1) + min   (verified in Mth)
        //   disk_clay: nextInt(2)+2; disk_gravel: nextInt(4)+2; disk_sand/grass: nextInt(5)+2
mutable = new MutableBlockPos();
for (pos in BlockPos.betweenClosed(origin.offset(-r,0,-r), origin.offset(r,0,r))) {
    dx = pos.x - origin.x; dz = pos.z - origin.z;
    if (dx*dx + dz*dz > r*r) continue;                 // circle test, ints
    placed |= placeColumn(cfg, level, random, top, bot, mutable.set(pos));
}
return placed;
```

**Iteration order of `betweenClosed`** (verified in `BlockPos$4.computeNext`): linear index
`i = 0..width*height*depth-1` with `x = minX + i % width`, `y = minY + (i/width) % height`,
`z = minZ + (i/width)/height`. Here ySpan = 1 → **x fastest (west→east), z outer (north→south)**.

```
placeColumn(cfg, level, random, top, bot, mutable):
    placedAny = false; placedLast = false;
    for (y = top; y > bot; y--) {                       // top-down, halfHeight*2+2 rows
        mutable.setY(y);
        if (cfg.target().test(level, mutable)) {
            state = cfg.stateProvider().getOptionalState(level, random, mutable);
            if (state != null) {
                level.setBlock(mutable, state, 2);      // flag 2
                if (!placedLast) markAboveForPostProcessing(level, mutable);
                placedAny = true; placedLast = true;
            }
            // state == null → placedLast UNCHANGED (not reset)
        } else placedLast = false;
    }
    return placedAny;
```

`Feature.markAboveForPostProcessing(level, pos)` (verified): up to 2 steps upward from pos; each step:
`move(UP)`; if `getBlockState(cursor).isAir()` return; else
`getChunk(cursor).markPosForPostProcessing(cursor)`. No RNG, no block writes (post-processing set is
chunk metadata / `PostProcessing` NBT, not in the block dump).

### State providers used by disks (all RNG-free)

- `SimpleStateProvider.getState(level, random, pos)` → returns the constant field. **No RNG.**
- `RuleBasedStateProvider` (26.2): `getOptionalState(level, random, pos)`: iterate `rules` in JSON
  order; first rule whose `ifTrue` BlockPredicate tests true at pos → return
  `rule.then().getState(level, random, pos)`; if none match → `fallback` present ?
  `fallback.getState(...)` : **null** (`fallback` is `optionalFieldOf` — nullable in 26.2!).
  Shipped disks always have a fallback → never null. Its `getState` = `getOptionalState`, and if that
  is null falls back to `level.getBlockState(pos)` (echo current block) — unreachable for shipped data.
  All `then`/`fallback` providers in disk_sand/disk_clay/disk_gravel/disk_grass are
  simple_state_provider ⇒ **the whole disk column loop consumes zero RNG**.
- Negative finding: none of the disk JSONs use `dual_noise`, `noise_provider`, `weighted_state_provider`
  etc. (those exist in `stateproviders/` but are unreferenced by these features).

### BlockPredicates referenced (target + rule predicates), bytecode-verified

All state-testing predicates extend `StateTestingPredicate`: final
`test(level, pos) = test(level.getBlockState(pos.offset(this.offset)))` — `offset` optional
Vec3i field, default ZERO, range-checked ±16 (`Vec3i.offsetCodec(16)`).

- `matching_blocks` (`MatchingBlocksPredicate`): `state.is(HolderSet<Block> blocks)`.
- `matching_fluids` (`MatchingFluidsPredicate`): `state.getFluidState().is(HolderSet<Fluid> fluids)`.
  With `fluids: minecraft:water` this matches **source water only** (flowing water has fluid type
  `minecraft:flowing_water`, a different registry entry). Waterlogged blocks DO match (their
  getFluidState is water[source]).
- `solid` (`SolidPredicate`): `state.isSolid()`.
- `not` (`NotPredicate`): negation of inner `predicate` (no offset of its own).
- `any_of` (`AnyOfPredicate`): short-circuit OR over `predicates` in JSON order.
- Negative finding: no `would_survive`, no `replaceable`, no `matching_block_tag` in these features.

## 5. UnderwaterMagmaFeature.place — full reconstruction

```
level = ctx.level; origin = ctx.origin; cfg = ctx.config; random = ctx.random;
floorY = getFloorY(level, origin, cfg);              // pure world reads, NO RNG
if (floorY.isEmpty()) return false;                  // ← bail with 0 draws
floorPos = origin.atY(floorY.get());
v   = (r, r, r) with r = cfg.placementRadiusAroundFloor (=1)
box = BoundingBox.fromCorners(floorPos - v, floorPos + v);   // fromCorners normalizes min/max
placedCount = BlockPos.betweenClosedStream(box)              // same x-fastest, then y, then z order
      .filter(pos -> random.nextFloat() < cfg.placementProbabilityPerValidPosition)  // DRAW per pos
      .filter(pos -> isValidPlacement(level, pos))
      .mapToInt(pos -> { level.setBlock(pos, Blocks.MAGMA_BLOCK.defaultBlockState(), 2); return 1; })
      .sum();
return placedCount > 0;
```

**RNG:** if a floor is found, **exactly (2r+1)³ = 27 `random.nextFloat()` draws for the shipped
config**, one per box position, in iteration order, UNCONDITIONALLY (the probability filter is first
in the stream; validity never gates a draw). Comparison is `nextFloat() < prob` (strict, `fcmpg`).
The stream is sequential and lazy: for each position in order, draw → maybe validity-check → maybe
setBlock; so world reads/writes interleave with draws, but the draw count/order is fixed.
`betweenClosedStream(BoundingBox)` → `betweenClosedStream(min..., max...)` → same `BlockPos$4`
iterator as disks: **x fastest, then y, then z**.

### getFloorY (no RNG)

`Column.scan(level, origin, cfg.floorSearchRange /*=5*/, colPred, tipPred)` with
`colPred = state.is(Blocks.WATER)` and `tipPred = !state.is(Blocks.WATER)` (block identity, NOT fluid
state — waterlogged blocks are not "water" here). Then `.map(Column::getFloor).orElseGet(OptionalInt::empty)`.

`Column.scan` (verified):
```
if (!isStateAtPosition(origin, colPred)) return empty;      // origin must BE water block
ceil  = scanDirection(range, colPred, tipPred, cursor, y0, UP);    // runs first (reads only)
floor = scanDirection(range, colPred, tipPred, cursor, y0, DOWN);
return Optional.of(Column.create(floor, ceil));
```
`scanDirection`: `cursor.setY(y0); i = 1; while (i < range && isStateAtPosition(cursor, colPred)) { cursor.move(dir); i++; }`
then `return isStateAtPosition(cursor, tipPred) ? OptionalInt.of(cursor.getY()) : empty`.
⇒ walks ≤ range−1 = 4 steps through water; the cell it stops on must be non-water to yield a value.
So floor = y of the first non-water **block** at origin.y−1 … origin.y−4 (or empty if water continues
through y0−4). `Column.create(floor, ceil)`: both present → `Range(floor, ceil)` (getFloor =
of(floor)); floor only → `above(floor)` = Ray(floor, up=true), getFloor = of(floor); ceil only →
`below(ceil)` = Ray(ceil, up=false), **getFloor = empty** → feature bails; neither → Line, getFloor
empty. `isStateAtPosition` (WorldGenRegion) = `predicate.test(getBlockState(pos))`, plain read.

### isValidPlacement (no RNG)

```
s = level.getBlockState(pos);
if (isWaterOrAir(s)                                   // s.is(Blocks.WATER) || s.isAir()
    || isVisibleFromOutside(level, pos.below(), UP))  return false;
for (dir in Direction.Plane.HORIZONTAL)               // iteration order: SOUTH,WEST,NORTH,EAST? see below
    if (isVisibleFromOutside(level, pos.relative(dir), dir.getOpposite())) return false;
return true;

isVisibleFromOutside(level, neighborPos, faceTowardCandidate):
    shape = level.getBlockState(neighborPos).getFaceOcclusionShape(face);
    return shape == Shapes.empty() || !Block.isShapeFullBlock(shape);
```
i.e. the candidate must be a non-water, non-air block; the block **below** it must have a full
occluding UP face, and each of the 4 horizontal neighbors must have a full occluding face toward the
candidate. Only the top face may be exposed (that is where the water sits). Order of the horizontal
iteration only affects which read short-circuits — **no RNG impact**. UNVERIFIED detail: the enum
order inside `Direction$Plane.HORIZONTAL` (irrelevant for parity of draws/writes since all four must
pass; short-circuit only changes read counts).

## 6. Write-path summary

| feature | setBlock flags | block(s) written | ticks / metadata |
|---|---|---|---|
| spring | 2 | `water[level=0]` or `lava[level=0]` at origin (single block) | `scheduleTick(origin, fluid, 0)` → fluid_ticks NBT, not in block dump |
| disk | 2 | provider state per matched column cell | `markPosForPostProcessing` for ≤2 non-air blocks above each column's first placement run — chunk NBT only |
| underwater_magma | 2 | `magma_block` (default state) per surviving position | none |

All three use flag **2** (UPDATE_CLIENTS; no UPDATE_NEIGHBORS bit, no flag-16 suppress bit) — for
ProtoChunk-era placement flags don't alter the stored blockstate, and `ProtoChunk.setBlock` updates
the 4 FINAL heightmaps (see A4-worldgenregion-window). Negative finding: none of these features use
flag 19 or any other flag value; `iconst_2` at every setBlock site.

## 7. Verdict: can these fire in a jungle / lush_caves grid with aquifer caves?

- **disk_clay / disk_gravel / disk_sand (step 6):** position = in_square, then snapped to
  OCEAN_FLOOR_WG surface, then REQUIRES **source water** at that surface pos (block_predicate_filter),
  then biome-at-pos must match. In a jungle grid: fires only in river/pond columns (surface water).
  In lush_caves: the OCEAN_FLOOR_WG position is at the world surface where the biome is normally the
  surface biome, not lush_caves → the `biome` modifier kills it in practice; aquifer cave water does
  NOT help because OCEAN_FLOOR_WG ignores caves. Even when position passes, per-column target
  (dirt/grass_block/clay under the water floor) must match for any write; radius draw (1 nextInt)
  happens regardless once `place` is reached.
- **underwater_magma (step 6):** needs y ≤ OCEAN_FLOOR_WG−2 (filter), then origin block must be
  literally `minecraft:water` with a non-water block ≤4 below. Aquifer-flooded caves DO satisfy this
  (underground water bodies), as do oceans/rivers columns below the floor filter. Uniform count 44–52
  attempts/chunk → in an aquifer-cave grid this WILL fire; each success burns exactly 27 nextFloat.
  Attempts whose origin is not water (vast majority) burn ZERO feature-RNG draws.
- **spring_water (step 8):** 25 attempts, y uniform in [−64, 192]. Needs an exposed pocket: origin
  air-or-valid, valid block above AND below, exactly 4/5 rock + 1/5 air neighbors. Carver+aquifer
  caves produce many such wall configurations → fires regularly in both biomes (biome check is at the
  cave position, so lush_caves qualifies underground). Zero RNG inside place.
- **spring_lava (step 8):** 20 attempts, very_biased_to_bottom [−64, top−8] → concentrated near
  bottom; deepslate is in valid_blocks so deep caves qualify; same neighbor rule. Fires, mostly deep.
  Zero RNG inside place.

## 8. Implications for the C implementation

1. **RNG budget per `place` call** (wrapper methods on the per-feature WorldgenRandom, see A3):
   - spring: 0 draws — pure predicate; cheap to replay, but the 8 neighbor reads MUST see the
     region state as of that walk position (earlier step-8 features in the same chunk can matter).
   - disk: exactly 1 × `nextInt(bound)` (radius) BEFORE any world read; then 0 draws in the column
     loop for shipped data. Implement `getOptionalState` generically anyway (rule_based may draw if
     a datapack uses a drawing `then` provider).
   - underwater_magma: 0 draws if origin isn't water / no floor within 4 below; else exactly
     `(2r+1)^3` = 27 × `nextFloat()` in x-then-y-then-z order over the normalized box, drawn before
     each position's validity check.
2. **Iteration orders to hard-code:** `BlockPos.betweenClosed`/`betweenClosedStream`: index-decoded
   x-fastest, then y, then z (`x = i%w; y=(i/w)%h; z=i/w/h`). Disk ring: single y plane, so
   x west→east inner, z north→south outer, with the integer circle test `dx²+dz² ≤ r²`.
3. **Disk column loop details that bite:** scan is top-down from `y0+hh` down to `y0−hh−1`
   inclusive (2hh+2 cells); `placedLast` persists across a null-state cell (only a target-test
   failure resets it) — this controls duplicate `markAboveForPostProcessing` calls (metadata only,
   but replicate for NBT parity later).
4. **Predicate semantics:** matching_fluids water = source water only; underwater_magma's water test
   is `state.is(Blocks.WATER)` (block, so waterlogged ≠ water, flowing-water block IS `water[level>0]`
   → `is(Blocks.WATER)` true — note the asymmetry with matching_fluids).
5. **Occlusion query:** underwater_magma validity needs `getFaceOcclusionShape(face)` +
   `isShapeFullBlock`. For worldgen-era blocks this reduces to "neighbor is a full opaque cube" for
   stone-like blocks; C side needs a per-blockstate boolean table (full-face occlusion per direction)
   rather than the general voxel-shape machinery. Deferred: build that table from the block registry.
6. **Spring block output:** water/lava `[level=0]` (source), NOT a falling/flowing state — the JSON's
   `falling=true` is invisible in the block dump. The scheduled fluid tick (delay 0) is NBT-only;
   the 07 dumps won't show it, so spring parity = 1 block + the predicate gates.
7. **surface_relative_threshold_filter:** compute `h + min/max` in int64 (defaults are INT32_MIN/MAX).
8. **Codec defaults to bake into the loader:** spring `requires_block_below=true, rock_count=4,
   hole_count=1`; StateTestingPredicate `offset=(0,0,0)`; rule_based `fallback` optional (nullable).
9. **Heightmap coupling:** disk/underwater_magma placement uses OCEAN_FLOOR_WG (frozen pre-features,
   per A4-window note) but their world reads/writes update FINAL heightmaps — order within the step
   walk matters for later features, not for these filters.

Open question for the empirical agent: confirm `Direction$Plane.HORIZONTAL` iteration order
(SOUTH,WEST,NORTH,EAST vs data-order) is truly irrelevant here — it is for draws/writes, but if a
future feature branches on first-failing-direction with RNG in between, it will matter.
