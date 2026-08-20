# A2 — Placement-modifier pipeline, bytecode-exact (MC 26.2, unobfuscated server)

Source of truth: `javap -p -c -constants` against
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server`. Every pseudocode block below is a 1:1
reconstruction from bytecode disassembled in this session. The only non-bytecode claim is the JDK
sequential-Stream evaluation order in §1.2, which is JDK library semantics (labeled as such).
Datapack facts come from `tools/golden/work/server/data/minecraft/worldgen/{biome,placed_feature}/`.

Companion notes: `.hermes/notes/task9pre-order/A2-applybiomedecoration.md` (the per-step walk that
calls `placeWithBiomeCheck`), `A3-worldgenrandom-seeding.md` (draw composition of the
WorldgenRandom wrapper — REQUIRED reading for the draw counts here), `A4-worldgenregion-window.md`
(3x3 window, heightmap freezing).

RNG shorthand used throughout (from A3, verified there against 26.2 bytecode + vectors):
- `nextFloat()` = 1 xoroshiro draw (`next(24) * 2^-24`).
- `nextInt(b)`, b a power of two (incl. b=1 and b=16) = exactly 1 draw (`(b * next(31)) >> 31`).
  **`nextInt(1)` still burns a draw.**
- `nextInt(b)`, b not a power of two = 1 draw + rejection loop (`b0=next(31); v=b0%b;` reject while
  `b0 - v + (b-1) < 0`); ≥1 draws, occasionally more.
- All draws are through the WorldgenRandom LCG-shaped defaults over one xoro draw per `next(bits)`.

---

## 0. Modifier census

### 0.1 Steps 1/2/3/6/8 of {jungle, lush_caves, beach, river} — the deep-dive set

Union of the biomes' `features[step]` lists (identical across the four biomes except `ore_clay`,
which only lush_caves adds in step 6):

| step | placed features | pipelines (modifier `type` sequence) |
|---|---|---|
| 1 LAKES | lake_lava_underground | rarity_filter(9), in_square, height_range(uniform abs0..below_top0), environment_scan(down,32), surface_relative_threshold_filter(OCEAN_FLOOR_WG, max=-5), biome |
| 1 LAKES | lake_lava_surface | rarity_filter(200), in_square, heightmap(WORLD_SURFACE_WG), biome |
| 2 LOCAL_MODIFICATIONS | amethyst_geode | rarity_filter(24), in_square, height_range(uniform above_bottom6..abs30), biome |
| 3 UNDERGROUND_STRUCTURES | monster_room | count(10), in_square, height_range(uniform abs0..below_top0), biome |
| 3 UNDERGROUND_STRUCTURES | monster_room_deep | count(4), in_square, height_range(uniform above_bottom6..abs-1), biome |
| 6 UNDERGROUND_ORES | 25 `ore_*` + underwater_magma + ore_clay (lush only) | count(c) **or** rarity_filter(c), in_square, height_range(uniform **or** trapezoid), [surface_relative_threshold_filter], biome |
| 6 UNDERGROUND_ORES | disk_sand | count(3), in_square, heightmap(OCEAN_FLOOR_WG), block_predicate_filter(matching_fluids water), biome |
| 6 UNDERGROUND_ORES | disk_clay, disk_gravel | in_square, heightmap(OCEAN_FLOOR_WG), block_predicate_filter(matching_fluids water), biome |
| 8 FLUID_SPRINGS | spring_water | count(25), in_square, height_range(uniform above_bottom0..abs192), biome |
| 8 FLUID_SPRINGS | spring_lava | count(20), in_square, height_range(**very_biased_to_bottom** above_bottom0..below_top8, inner=8), biome |

Non-constant IntProviders in this set: `ore_gold_lower` count = uniform[0,1];
`underwater_magma` count = uniform[44,52]. Everything else is a constant count (JSON bare int).
HeightProviders in this set: uniform, trapezoid (all plateau-less), very_biased_to_bottom
(spring_lava only). `underwater_magma` also carries surface_relative_threshold_filter
(OCEAN_FLOOR_WG, max_inclusive=-2; min defaults to INT_MIN).

Distinct modifier types to implement for 9a: **rarity_filter, count, in_square, height_range,
heightmap, environment_scan, surface_relative_threshold_filter, block_predicate_filter, biome.**

### 0.2 Step 9/10 census (handoff to 9b — one line each, NOT deep-dived here)

| placed feature | pipeline |
|---|---|
| glow_lichen | count(uniform[104,157]), height_range, in_square, surface_relative_threshold_filter, biome |
| bamboo_light (jungle) | rarity_filter, in_square, heightmap, biome |
| trees_jungle / trees_water | count, in_square, surface_water_depth_filter, heightmap, biome |
| flower_warm / flower_default / patch_bush / patch_melon / patch_pumpkin / patch_sugar_cane / brown+red_mushroom_normal | rarity_filter, in_square, heightmap, biome, count, random_offset, block_predicate_filter |
| patch_grass_jungle | count, in_square, heightmap, biome, count, random_offset, block_predicate_filter |
| patch_grass_badlands | in_square, heightmap, biome, count, random_offset, block_predicate_filter |
| patch_firefly_bush_near_water | count, in_square, heightmap, biome, block_predicate_filter, count, random_offset, block_predicate_filter |
| patch_tall_grass_2 (lush) | **noise_threshold_count**, rarity_filter, in_square, heightmap, biome, count, random_offset, block_predicate_filter |
| lush_caves_{ceiling_vegetation,clay,vegetation} / cave_vines / rooted_azalea_tree / spore_blossom | count, in_square, height_range, environment_scan, random_offset, biome |
| classic_vines_cave_feature / vines | count, in_square, height_range, biome |
| seagrass_river | in_square, heightmap, **count**, biome (count AFTER heightmap — copies share one column) |
| freeze_top_layer (step 10) | biome |

New types appearing only in 9/10: `surface_water_depth_filter` (no RNG; pass iff
`getHeight(WORLD_SURFACE,x,z) - getHeight(OCEAN_FLOOR,x,z) <= maxWaterDepth`, LIVE final maps),
`random_offset` (3 IntProvider samples in order **xzSpread→x, ySpread→y, xzSpread→z**),
`noise_threshold_count` (RepeatingPlacement; count from static `Biome.BIOME_INFO_NOISE`
PerlinSimplexNoise `getValue(x/200.0, z/200.0, false) < noiseLevel ? belowNoise : aboveNoise`;
zero RNG draws, but world-position-dependent legacy noise — 9b must port that noise).
Negative finding: `count_on_every_layer`, `fixed_placement`, `noise_based_count`, `carving_mask`
do NOT occur in any step of the four biomes.

---

## 1. `PlacedFeature` — the pipeline driver

### 1.1 Bytecode (`net/minecraft/world/level/levelgen/placement/PlacedFeature.class`)

`PlacedFeature` is a `Record(Holder<ConfiguredFeature> feature, List<PlacementModifier> placement)`.

```java
public boolean placeWithBiomeCheck(WorldGenLevel level, ChunkGenerator gen, RandomSource random, BlockPos origin) {
    return placeWithContext(new PlacementContext(level, gen, Optional.of(this)), random, origin);
}
public boolean place(...) {   // NOT used by applyBiomeDecoration; topFeature = Optional.empty()
    return placeWithContext(new PlacementContext(level, gen, Optional.empty()), random, origin);
}

private boolean placeWithContext(PlacementContext ctx, RandomSource random, BlockPos origin) {
    Stream<BlockPos> stream = Stream.of(origin);                       // slot 4
    for (PlacementModifier mod : this.placement)                       // JSON "placement" list order
        stream = stream.flatMap(pos -> mod.getPositions(ctx, random, pos));   // lambda$placeWithContext$0
    ConfiguredFeature cf = this.feature.value();                       // slot 5
    MutableBoolean placedAny = new MutableBoolean();                   // slot 6
    stream.forEach(pos -> {                                            // lambda$placeWithContext$1
        if (cf.place(ctx.getLevel(), ctx.generator(), random, pos))
            placedAny.setTrue();                                       // (+ FeatureCountTracker if DEBUG_FEATURE_COUNT)
    });
    return placedAny.isTrue();
}
```

Verified: it is plain sequential `Stream.of(x).flatMap(...)...flatMap(...).forEach(...)`; no
`.parallel()`, no sorting, no short-circuiting terminal anywhere in the method.

### 1.2 Effective evaluation order → C loop nest (JDK semantics, labeled)

JDK library semantics (not MC bytecode): a sequential `flatMap` stage, upon receiving one upstream
element, applies the mapper — **this is the moment `getPositions` runs and its RNG draws burn** —
and then pushes every element of the returned inner stream through the entire downstream
(`result.sequential().forEach(downstreamSink)`) before accepting the next upstream element.
`forEach` is non-short-circuiting, so nothing is skipped. Net effect is exact depth-first
processing. The C equivalence for a pipeline `[m1, m2, ..., mk]`:

```c
// process(i, pos): run modifier i on pos, recurse for each produced position
void process(int i, BlockPos pos) {
    if (i == k) { configured_feature_place(level, gen, rng, pos); return; }   // same rng object!
    /* ALL of modifier i's RNG draws happen HERE, once per incoming pos: */
    positions = m[i].getPositions(ctx, rng, pos);
    for (p in positions) process(i + 1, p);   // for RepeatingPlacement: n copies of pos, no per-copy draws
}
process(0, origin);
```

Consequences that the replay must honor:
- Copy #0 of a `count` runs through *all* downstream modifiers *and the feature body itself*
  (which draws from the SAME WorldgenRandom) before copy #1 sees its first draw. Feature-internal
  draw counts therefore shift every subsequent sibling's positions.
- A filter rejecting a position skips all downstream draws for that position only; siblings
  continue unaffected.
- Every modifier in our set draws eagerly inside `getPositions`; the only lazily-pulled stream
  (RepeatingPlacement's `IntStream.range(0,n).mapToObj(i -> pos)`) contains no draws, so
  "eager vs lazy" collapses: global interleaving == the loop nest above, with modifier i's draws
  at loop-entry of level i.

### 1.3 Origin position

From task9pre A2: `applyBiomeDecoration` calls `placeWithBiomeCheck(region, generator, random,
origin)` with `origin = SectionPos.of(chunkPos, level.getMinSectionY()).origin()` =
`(minBlockX, -64, minBlockZ)` for the overworld, after `random.setFeatureSeed(decoSeed, idx, step)`.

---

## 2. Modifier deep dives (steps 1/2/3/6/8 set)

### 2.1 Bases

`PlacementModifier` — abstract `getPositions(PlacementContext, RandomSource, BlockPos): Stream<BlockPos>`.

`RepeatingPlacement.getPositions` (superclass of count / noise_threshold_count):
```java
int n = this.count(random, pos);                       // draws NOW, once per incoming pos
return IntStream.range(0, n).mapToObj(i -> pos);       // n aliases of the SAME BlockPos, no draws
```
n = 0 ⇒ empty stream (possible: ore_gold_lower uniform[0,1]).

`PlacementFilter.getPositions` (superclass of rarity/biome/block_predicate/surface_relative/surface_water_depth):
```java
return this.shouldPlace(ctx, random, pos) ? Stream.of(pos) : Stream.of(new BlockPos[0]);
```
Eager; empty stream on reject.

### 2.2 `minecraft:count` — CountPlacement

```java
protected int count(RandomSource random, BlockPos pos) { return this.count.sample(random); }
```
- ConstantInt (JSON bare int): `sample` returns field, **0 draws**.
- UniformInt: `sample = Mth.randomBetweenInclusive(random, min, max)` =
  `random.nextInt(max - min + 1) + min` — **always exactly one nextInt call, even when min==max**
  (`nextInt(1)` = 1 draw). ore_gold_lower: `nextInt(2)` (pow2, 1 draw). underwater_magma:
  `nextInt(9)` (non-pow2, rejection possible).
- Draw timing: once, when the origin arrives at this stage (before any copy flows downstream).

### 2.3 `minecraft:rarity_filter` — RarityFilter

```java
protected boolean shouldPlace(ctx, random, pos) {
    return random.nextFloat() < 1.0F / (float)this.chance;    // fdiv in float32; strict <
}
```
Exactly **1 nextFloat draw, always** (drawn before the comparison; no early-out). The threshold is
the float32 quotient `1.0f / (float)chance` (e.g. chance=9 → 0.11111111f = 0x3DE38E39
round-to-nearest); C must compute it in `float`, not double-then-truncate.

### 2.4 `minecraft:in_square` — InSquarePlacement

```java
public Stream<BlockPos> getPositions(ctx, random, pos) {
    int x = random.nextInt(16) + pos.getX();     // FIRST draw: X
    int z = random.nextInt(16) + pos.getZ();     // SECOND draw: Z
    return Stream.of(new BlockPos(x, pos.getY(), z));
}
```
2 draws (`nextInt(16)` pow2 → 1 each), **X before Z**, y passes through unchanged.

### 2.5 `minecraft:height_range` — HeightRangePlacement

```java
public Stream<BlockPos> getPositions(ctx, random, pos) {
    return Stream.of(pos.atY(this.height.sample(random, ctx)));   // ctx IS the WorldGenerationContext
}
```
Never empty; x/z unchanged. All RNG is inside the HeightProvider (§3.2).

### 2.6 `minecraft:heightmap` — HeightmapPlacement  (NO RNG)

```java
public Stream<BlockPos> getPositions(ctx, random, pos) {
    int x = pos.getX(), z = pos.getZ();
    int y = ctx.getHeight(this.heightmap, x, z);        // see §5 for exact value
    return y > ctx.getMinY() ? Stream.of(new BlockPos(x, y, z)) : Stream.empty();
}
```
- `ctx.getMinY()` here is `PlacementContext.getMinY()` = `level.getMinY()` = **-64** (the
  LevelHeightAccessor value, NOT the clamped `getMinGenY`; both are -64 in vanilla overworld).
- `ctx.getHeight` = `WorldGenRegion.getHeight` = `chunk.getHeight(type, x&15, z&15) + 1` =
  `heightmap.getFirstAvailable(x&15, z&15)` = **y of the lowest free block above the highest
  counted block** (`data.get(idx) + minY`). Empty column ⇒ getFirstAvailable = -64 ⇒ `-64 > -64`
  false ⇒ **empty stream**.
- Uses the frozen WG maps in our step-1/6 pipelines (WORLD_SURFACE_WG / OCEAN_FLOOR_WG — per A4
  these are NOT updated by feature writes), but the value still depends on which neighbor chunk
  the in_square draw landed in ⇒ world-state-dependent, order-sensitive across the 3x3 (each
  chunk's WG maps reflect its own pre-features state).

### 2.7 `minecraft:surface_relative_threshold_filter` — SurfaceRelativeThresholdFilter  (NO RNG)

```java
protected boolean shouldPlace(ctx, random, pos) {
    long h = (long)ctx.getHeight(this.heightmap, pos.getX(), pos.getZ());
    long lo = h + (long)this.minInclusive;      // min default INT_MIN (codec), so lo ≈ -2^31 range
    long hi = h + (long)this.maxInclusive;
    return lo <= (long)pos.getY() && (long)pos.getY() <= hi;    // all in 64-bit, no overflow
}
```
Codec defaults: min_inclusive = -2147483648, max_inclusive = 2147483647 (both features in our set
only set max). lake_lava_underground: OCEAN_FLOOR_WG, y ≤ h−5. underwater_magma: OCEAN_FLOOR_WG,
y ≤ h−2. The long-widening means INT_MIN default cannot underflow. Same getHeight semantics as
§2.6 (first-free-block, +1 convention).

### 2.8 `minecraft:block_predicate_filter` — BlockPredicateFilter  (NO RNG)

```java
protected boolean shouldPlace(ctx, random, pos) {
    return this.predicate.test(ctx.getLevel(), pos);
}
```
Predicates in our set (§6): `matching_fluids [minecraft:water]` on the disks — passes iff
`getBlockState(pos + offset(0,0,0)).getFluidState().is(waterHolderSet)` (water OR flowing_water?
No — HolderSet is exactly the listed fluid `minecraft:water`; flowing_water is a different fluid
and does NOT match; note water[level=0] source blocks and waterlogged blocks report fluid
`water`). Reads world state through the region ⇒ order-sensitive.

### 2.9 `minecraft:environment_scan` — EnvironmentScanPlacement  (NO RNG)

Fields: `directionOfSearch` (VERTICAL_CODEC: up/down only), `targetCondition`,
`allowedSearchCondition` (codec default `BlockPredicate.alwaysTrue()` — our JSON omits it),
`maxSteps` (codec intRange(1,32)).

```java
public Stream<BlockPos> getPositions(ctx, random, pos) {
    BlockPos.MutableBlockPos m = pos.mutable();
    WorldGenLevel level = ctx.getLevel();
    if (!allowedSearchCondition.test(level, m)) return Stream.empty();
    for (int i = 0; i < maxSteps; ++i) {
        if (targetCondition.test(level, m)) return Stream.of(m);       // tests BEFORE moving
        m.move(directionOfSearch);
        if (level.isOutsideBuildHeight(m.getY())) return Stream.empty();  // y < -64 || y > 319
        if (!allowedSearchCondition.test(level, m)) break;             // falls to the final test!
    }
    return targetCondition.test(level, m) ? Stream.of(m) : Stream.empty();  // one LAST test
}
```
- Loop shape: test at current pos → move → bounds check → allowed check; after `maxSteps`
  successful iterations (or an allowed-condition break) the target is tested **one final time** at
  the moved position — so up to `maxSteps` moves and `maxSteps+1` target tests total.
- With default allowedSearchCondition=true the `break` path is dead; bounds exit returns empty
  WITHOUT the final test.
- Returns the mutable pos itself (fine: consumed immediately downstream).
- lake_lava_underground config: down, 32 steps, target =
  `all_of[ not(matching_block_tag #minecraft:air), inside_world_bounds(offset=(0,-5,0)) ]`
  ⇒ first block within 32 below the height_range y that is non-air (tag air = {air, void_air,
  cave_air}) and whose y−5 ≥ −64 (i.e. y ≥ −59; upper bound y ≤ 319 trivially true).

### 2.10 `minecraft:biome` — BiomeFilter  (NO RNG) — see §4.

---

## 3. Providers

### 3.1 IntProviders in play

- `ConstantInt.sample(r)` → field, 0 draws. (JSON bare int deserializes to ConstantInt.)
- `UniformInt.sample(r)` → `Mth.randomBetweenInclusive(r, min, max)`:
  ```java
  public static int randomBetweenInclusive(RandomSource r, int lo, int hi) {
      return r.nextInt(hi - lo + 1) + lo;        // ALWAYS draws; lo==hi still nextInt(1)=1 draw
  }
  ```
- Contrast `Mth.nextInt(RandomSource r, int lo, int hi)` (used by VeryBiasedToBottomHeight):
  ```java
  if (lo >= hi) return lo;                       // ZERO draws on degenerate range
  return r.nextInt(hi - lo + 1) + lo;
  ```
  The two helpers differ in the degenerate case — do not merge them in C.
- Negative finding: no `biased_to_bottom`, `weighted_list`, `clamped*` IntProviders occur in the
  step-1/2/3/6/8 pipelines (bamboo config etc. may use others inside feature configs — out of
  scope for placement).

### 3.2 HeightProviders

All receive `(RandomSource random, WorldGenerationContext ctx)`; anchors resolve first, in the
order **min then max**, with zero draws.

`UniformHeight.sample`:
```java
int lo = minInclusive.resolveY(ctx), hi = maxInclusive.resolveY(ctx);
if (lo > hi) { warn-once; return lo; }                      // 0 draws (never fires in our configs)
return Mth.randomBetweenInclusive(random, lo, hi);          // exactly 1 nextInt(hi-lo+1)
```

`TrapezoidHeight.sample` (plateau: codec-optional, our JSONs omit it → **plateau = 0**; verified
`triangle()` factory passes 0):
```java
int lo = minInclusive.resolveY(ctx), hi = maxInclusive.resolveY(ctx);
if (lo > hi) { warn; return lo; }
int range = hi - lo;
if (plateau >= range) return Mth.randomBetweenInclusive(random, lo, hi);   // 1 draw
int halfLower = (range - plateau) / 2;                       // Java int division
int upper = range - halfLower;
return lo + Mth.randomBetweenInclusive(random, 0, upper)     // draw #1: nextInt(upper+1)
          + Mth.randomBetweenInclusive(random, 0, halfLower);// draw #2: nextInt(halfLower+1)
```
Exactly **2 nextInt calls** (each with its own rejection loop) in this fixed order; the sum of the
two uniforms produces the triangle. Example ore_iron_upper (80..384): range=304, halfLower=152,
upper=152 → `80 + nextInt(153) + nextInt(153)`. Odd range example ore_copper (−16..112):
range=128, halfLower=64, upper=64 → `−16 + nextInt(65) + nextInt(65)` (max 112 reachable? −16+64+64
=112 yes). NOTE the asymmetry when `range − plateau` is odd: halfLower = floor, upper gets the
extra — bound order matters.

`VeryBiasedToBottomHeight.sample` (spring_lava only; inner=8):
```java
int lo = minInclusive.resolveY(ctx), hi = maxInclusive.resolveY(ctx);
if (hi - lo - inner + 1 <= 0) { warn; return lo; }           // 0 draws
int y1 = Mth.nextInt(random, lo + inner, hi);                // 0 or 1 nextInt (see §3.1)
int y2 = Mth.nextInt(random, lo, y1 - 1);
return  Mth.nextInt(random, lo, y2 - 1 + inner);
```
Up to **3 nextInt calls in this order**, each skipped (0 draws) iff its lo ≥ hi. For spring_lava
overworld (lo=−64, hi=311, inner=8): y1 = nextInt(304)−56 (always draws), y2 = nextInt(y1+64)−64
(y1≥−56 ⇒ always draws), y3 = nextInt(y2+72)−64 (y2≥−64 ⇒ always draws) ⇒ always exactly 3
nextInt calls, bounds 304 (non-pow2, rejection loop), then data-dependent bounds.

Negative finding: `ConstantHeight`, `BiasedToBottomHeight`, `WeightedListHeight` unused in the
deep-dive set.

### 3.3 VerticalAnchor resolution (all 0 draws)

```java
Absolute.resolveY(ctx)    = y;
AboveBottom.resolveY(ctx) = ctx.getMinGenY() + offset;
BelowTop.resolveY(ctx)    = ctx.getGenDepth() - 1 + ctx.getMinGenY() - offset;
```

`WorldGenerationContext` (superclass of PlacementContext), ctor `(ChunkGenerator gen,
LevelHeightAccessor accessor)`:
```java
minY   = Math.max(accessor.getMinY(),  gen.getMinY());      // getMinGenY()
height = Math.min(accessor.getHeight(), gen.getGenDepth()); // getGenDepth()
```
`NoiseBasedChunkGenerator.getMinY/getGenDepth` = `settings.noiseSettings().minY()/height()` =
−64/384 (overworld); accessor (WorldGenRegion → dimension type) = −64/384. So for vanilla
overworld: `getMinGenY() = −64`, `getGenDepth() = 384`, above_bottom(o) = −64+o,
below_top(o) = 319−o. (The max/min clamping only matters for datapack dimension mismatches.)

---

## 4. BiomeFilter — exact semantics

```java
protected boolean shouldPlace(PlacementContext ctx, RandomSource random, BlockPos pos) {
    PlacedFeature top = ctx.topFeature().orElseThrow(
        () -> new IllegalStateException("Tried to biome check an unregistered feature, ..."));
    Holder<Biome> biome = ctx.getLevel().getBiome(pos);           // MODIFIED pos, at its CURRENT y
    return ctx.generator().getBiomeGenerationSettings(biome).hasFeature(top);
}
```

1. **Which position/y**: the position as modified by everything upstream in the pipeline — for the
   deep-dive set that is post in_square (x,z) and post height_range/heightmap/environment_scan (y).
   The biome sample is fully 3-D at that exact block.
2. **Biome sampling chain** (all verified):
   `WorldGenLevel.getBiome(pos)` (LevelReader default) → `getBiomeManager().getBiome(pos)`.
   `WorldGenRegion.getBiomeManager()` returns the region's own
   `new BiomeManager(this /*NoiseBiomeSource*/, BiomeManager.obfuscateSeed(seed))` built in the
   ctor; `obfuscateSeed = Hashing.sha256().hashLong(worldSeed).asLong()` (first 8 bytes of the
   SHA-256 of the little-endian long, read little-endian). `BiomeManager.getBiome` is the standard
   fiddled-vertex zoom: works on `(x−2, y−2, z−2)`, candidate quart corners `q>>2` and `+1`, picks
   the corner minimizing `getFiddledDistance(biomeZoomSeed, cx, cy, cz, fx, fy, fz)` over the 8
   corners (i iterating 0..7, first-strictly-smaller wins), then
   `noiseBiomeSource.getNoiseBiome(qx, qy, qz)`.
   For WorldGenRegion the noise-biome source resolves via the LevelReader default
   `getNoiseBiome(qx,qy,qz)` → `getChunk(QuartPos.toSection(qx), toSection(qz), ChunkStatus.BIOMES,
   false).getNoiseBiome(qx,qy,qz)` — i.e. **the stored per-chunk biome palette** (quart coords
   clamped to the chunk's sections by ChunkAccess.getNoiseBiome), NOT a fresh climate sample. The
   zoom can push the lookup into a neighboring chunk of the 3x3; all needed chunks are ≥BIOMES in
   the region so the null → `getUncachedNoiseBiome` fallback is unreachable during decoration.
3. **Membership test**: `ChunkGenerator.getBiomeGenerationSettings(holder)` =
   `generationSettingsGetter.apply(holder)` = `holder.value().getGenerationSettings()`.
   `BiomeGenerationSettings.hasFeature(pf)` = memoized
   `features.stream().flatMap(HolderSet::stream).map(Holder::value).collect(toSet()).contains(pf)`
   — a Set of PlacedFeature **values** (record equality; registry-interned so effectively
   identity) across ALL steps of that biome.
4. **Neighbor-biome-only features**: `applyBiomeDecoration` iterates the union of features of all
   biomes in the 3x3 (task9pre A2). A feature contributed only by a neighbor biome still runs its
   pipeline (burning rarity/count/in_square/height draws!) but `biome` returns false whenever the
   final position's own biome does not list the feature ⇒ filtered, feature body never runs.
   **The upstream draws are burned regardless** — the C replay must execute pipelines for
   union-features even when the center chunk's biomes would all reject them.
5. `topFeature` empty (plain `place()` path, unused in decoration) ⇒ IllegalStateException.

---

## 5. PlacementContext — full inventory

`class PlacementContext extends WorldGenerationContext`, fields
`level: WorldGenLevel`, `generator: ChunkGenerator`, `topFeature: Optional<PlacedFeature>`;
ctor passes `(generator, level)` to super (§3.3 clamping).

| method | body (verified) |
|---|---|
| `getHeight(Heightmap.Types, x, z)` | `level.getHeight(type, x, z)` |
| `getBlockState(pos)` | `level.getBlockState(pos)` |
| `getMinY()` | `level.getMinY()` (= −64; **overrides** nothing in super — super has no getMinY; distinct from inherited `getMinGenY()`) |
| `getCarvingMask(chunkPos)` | `((ProtoChunk)level.getChunk(x,z)).getOrCreateCarvingMask()` |
| `getLevel() / generator() / topFeature()` | field getters |
| inherited `getMinGenY() / getGenDepth()` | clamped −64 / 384 (§3.3) |

`WorldGenRegion.getHeight(type, x, z)` (the only implementation reached):
```java
int cx = SectionPos.blockToSectionCoord(x), cz = ...z;
warnIfReadOutsideWriteZone(cx, cz);                     // LOG ONLY — no throw, no behavior change
return getChunk(cx, cz).getHeight(type, x & 15, z & 15) + 1;
```
`ChunkAccess.getHeight(type, lx, lz)` = `heightmaps.get(type).getFirstAvailable(lx&15, lz&15) − 1`
(with a lazy `primeHeightmaps(EnumSet.of(type))` fallback if the map is missing — for WG types on
proto chunks the maps exist from noise/surface, so this is dead in practice);
`Heightmap.getFirstAvailable(lx, lz)` = `data.get(index) + chunk.getMinY()`.
⇒ **`ctx.getHeight` returns `getFirstAvailable` — the y of the first non-counted block above the
top counted block; the −1/+1 pair cancels.** For an all-air column: `minY` (−64).

Heightmap types touched by 9a pipelines: WORLD_SURFACE_WG, OCEAN_FLOOR_WG (frozen during features
per A4 — reads see the post-surface pre-features snapshot of whichever chunk owns the column).
Step-9's surface_water_depth_filter instead reads WORLD_SURFACE / OCEAN_FLOOR (LIVE, updated by
`ProtoChunk.setBlockState` during features — cross-feature coupling for 9b).

---

## 6. Block predicates used (all RNG-free)

Dispatch: `BlockPredicate.test(WorldGenLevel, BlockPos)` (bridged through the raw
`test(Object,Object)` BiPredicate).

- `StateTestingPredicate` base (matching_*): `test(level, pos)` =
  `test(level.getBlockState(pos.offset(this.offset)))`; `offset` codec-optional default (0,0,0)
  (all our JSONs omit it).
- `matching_fluids`: `state.getFluidState().is(fluidsHolderSet)`.
- `matching_block_tag`: `state.is(tagKey)`; tag `#minecraft:air` = {air, void_air, cave_air}
  (verified `data/minecraft/tags/block/air.json`).
- `not`: negation. `all_of` (CombiningPredicate): short-circuit AND in list order.
- `inside_world_bounds`: `level.isInsideBuildHeight(pos.offset(offset))` =
  `minY ≤ y ≤ maxY` with `maxY = getMinY() + getHeight() − 1` = 319. (`isOutsideBuildHeight` used
  by environment_scan is the exact complement: `y < minY || y > maxY`.)

---

## 7. Negative findings / order-sensitivity flags

1. **No RNG**: biome, block_predicate_filter, surface_relative_threshold_filter, environment_scan,
   heightmap, surface_water_depth_filter (9b) draw ZERO random numbers — verified no
   RandomSource call in any of their bytecode. Their `random` parameter is dead.
2. **World-state-dependent (order-sensitive) modifiers**: heightmap,
   surface_relative_threshold_filter (heightmap reads), environment_scan +
   block_predicate_filter (block reads), biome (stored biome palette reads — immutable during
   features, so only cross-stage-sensitive, not cross-feature). heightmap/threshold reads of *_WG
   maps are frozen per chunk but differ per neighbor chunk's generation state; block reads see
   every prior write in the 3x3 window, including writes made earlier in the SAME chunk's
   decoration walk and by previously-decorated neighbor chunks.
3. **Draw-order hazards**: RarityFilter draws even though it usually rejects (1 float ALWAYS);
   UniformInt with min==max still draws (`nextInt(1)`); `Mth.nextInt` degenerate range does NOT
   draw; UniformHeight/TrapezoidHeight empty-range (min>max) return min with 0 draws.
4. `placeWithContext`'s return value aggregates MutableBoolean; `applyBiomeDecoration` ignores it
   (no control-flow effect).
5. No modifier caches anything between calls; all state is per-call. The memoized
   `BiomeGenerationSettings.featureSet` is config-static.
6. `count_on_every_layer` / `random_offset` / `noise_threshold_count` etc. never appear before
   step 9 in the four target biomes (§0.2 handles the census for 9b).

---

## 8. Implications for the C implementation

1. **Pipeline executor**: implement `placed_feature_run(pf, ctx, rng, origin)` as the literal
   depth-first nest of §1.2 — a recursive or explicit-stack walk where each modifier's draws
   happen exactly at node entry, and the configured-feature body runs at each leaf with the same
   rng. Do NOT batch positions per stage (breadth-first) — that reorders draws whenever any
   modifier or feature body draws.
2. **Modifier VM**: only 9 opcodes needed for 9a (§0.1). Encode pipelines from the datapack JSON
   at build/config time; per-feature configs above enumerate every parameter combination we must
   support (uniform/trapezoid/very_biased_to_bottom heights; constant/uniform counts; anchors
   absolute/above_bottom/below_top).
3. **RNG contract**: route every draw through the WorldgenRandom wrapper implementation from A3
   (`wg_next_int`, `wg_next_float`). Two Mth helpers with different degenerate-range behavior
   (§3.1). RarityFilter threshold computed as `1.0f / (float)chance` in binary32.
4. **getHeight convention**: `ctx_get_height(type,x,z)` must return heightmap `getFirstAvailable`
   (stored raw bits + minY) — i.e. one ABOVE the top counted block; heightmap placement then uses
   that y directly (no further +/-1) and rejects `y <= -64`.
5. **BiomeFilter**: reuse the fiddled-zoom (sha256-obfuscated seed) biome lookup against the
   STORED per-chunk quart biome palettes of the 3x3, at the final modified position; membership =
   "does the sampled biome's own feature list (any step) contain this placed feature". Union
   features from neighbor biomes must still burn their upstream draws before being rejected here.
6. **Environment scan**: implement the exact loop of §2.9 including the final post-loop target
   test and the `isOutsideBuildHeight` early empty (no final test on that path); air = the 3-block
   tag, and `inside_world_bounds` checks the OFFSET position (y−5 ≥ −64).
7. **Long-widened threshold filter** (§2.7): compute in int64; min defaults to INT32_MIN.
8. **Per-position aliasing**: RepeatingPlacement emits the same coordinates n times; C can just
   loop n times over the identical (x,y,z) — no copy semantics matter since positions are values.
9. **Order-sensitive reads** (§7.2) mean feature replay is only correct inside the full scheduler
   order from task9pre A5 — unit-testing a single pipeline requires freezing the 3x3 world state
   (blocks + final & WG heightmaps + biome palettes) as fixture input.

Open items deliberately left to 9b: surface_water_depth_filter live-map coupling,
random_offset provider census, noise_threshold_count's `Biome.BIOME_INFO_NOISE` seeding
(static PerlinSimplexNoise — its construction seed needs its own recon), and the
step-9 configured-feature bodies (`random_selector`, `simple_block`, tree RNG, etc.).

---

## Adversarial verification (independent re-derivation, 2026-07-31)

Second agent re-disassembled every class cited above from the 26.2 tree (`javap -p -c
-constants`) and re-checked the datapack JSON programmatically. Original text above left
intact. Results:

**CONFIRMED (bytecode re-derived, byte-for-byte agreement):** §1.1 pipeline driver
(slots 4/5/6, flatMap-per-modifier in JSON order, forEach + MutableBoolean,
DEBUG_FEATURE_COUNT branch, `place()` = Optional.empty vs `placeWithBiomeCheck` =
Optional.of(this)); §2.1 RepeatingPlacement / PlacementFilter shapes; §2.2 count draw
semantics (ConstantInt 0 draws, UniformInt always 1 nextInt via `Mth.randomBetweenInclusive`);
§2.3 RarityFilter (`nextFloat` drawn unconditionally, `fdiv` in binary32, `fcmpg/ifge` =
strict `<`; 1/9f = 0x3DE38E39 re-computed); §2.4 in_square (nextInt(16) X then Z); §2.5;
§2.6 heightmap (pass iff `y > level.getMinY()`, `if_icmple` → empty; WorldGenRegion +1 /
ChunkAccess −1 cancel to `getFirstAvailable` = `data.get(idx) + minY`); §2.7 (i2l/ladd/lcmp
64-bit widening, codec defaults −2147483648/2147483647); §2.8; §2.9 environment_scan exact
loop (initial allowed test → empty; target-test-before-move; `isOutsideBuildHeight` exit
WITHOUT final test; allowed-fail break falls to the one post-loop target test; codec:
VERTICAL_CODEC, allowed default alwaysTrue, max_steps intRange(1,32)); §3.1 both Mth
helpers incl. the degenerate-range asymmetry (`Mth.nextInt`: `if_icmplt` guard, lo≥hi ⇒ 0
draws); §3.2 UniformHeight (min-then-max resolve, lo>hi warn-once + return lo with 0 draws)
and TrapezoidHeight (plateau≥range ⇒ 1 draw; else `lo + rBI(0, range−halfLower) +
rBI(0, halfLower)`, halfLower=(range−plateau)/2 idiv, upper-bound draw FIRST; `of(min,max)`
factory passes iconst_0 plateau) and VeryBiasedToBottomHeight structure (guard
`hi−lo−inner+1 ≤ 0` ⇒ lo, then 3 ordered `Mth.nextInt`); §3.3 anchors + WorldGenerationContext
Math.max/Math.min ctor + NoiseBasedChunkGenerator minY/height from noiseSettings; §4 biome
filter chain (topFeature orElseThrow, `new BiomeManager(this, obfuscateSeed(seed))` in the
WorldGenRegion ctor — aload_0 confirmed as the NoiseBiomeSource arg —, sha256 hashLong asLong,
fiddled zoom on (x−2,y−2,z−2) with i=0..7 dcmpl first-strictly-smaller, LevelReader default
getNoiseBiome → getChunk(BIOMES,false) stored palette, ChunkAccess.getNoiseBiome
Mth.clamp of the y-quart, hasFeature = memoized Set of HolderSet::stream → Holder::value);
§5 full table incl. `warnIfReadOutsideWriteZone` ending in `Util.logAndPauseIfInIde` +
return (log-only in production); §6 predicates (StateTestingPredicate tests
`getBlockState(pos.offset(offset))`, offset default Vec3i.ZERO; all_of iterator
short-circuit AND; not; inside_world_bounds → isInsideBuildHeight, maxY = minY+height−1;
air tag = {air, void_air, cave_air}); §7.1 no-RNG census (zero RandomSource call sites in
all six modifiers' bytecode); §0.1/§0.2 datapack census re-derived programmatically from the
four biome JSONs (26-ore family = 25 common + lush-only ore_clay; rarity_filter only on
granite/diorite/andesite_upper + ore_diamond_large; non-constant counts exactly
ore_gold_lower uniform[0,1] and underwater_magma uniform[44,52]; no plateau key in any
trapezoid; negative finding on count_on_every_layer/fixed_placement/noise_based_count/
carving_mask confirmed); §0.2 handoff details (random_offset sample order xzSpread→x,
ySpread→y, xzSpread→z; surface_water_depth_filter reads OCEAN_FLOOR then WORLD_SURFACE,
passes iff WS−OF ≤ maxWaterDepth, no RNG; noise_threshold_count strict `<` via dcmpg,
NaN/equal ⇒ aboveNoise).

**REFUTED — 1 item (worked example only, structure unaffected):**

- §3.2 VeryBiasedToBottomHeight, spring_lava worked example: the note says
  "y1 = nextInt(304)−56 … bounds 304 (non-pow2, rejection loop)". WRONG ARITHMETIC.
  With lo=−64, hi=below_top(8)=311, inner=8: y1 = `Mth.nextInt(r, −56, 311)` =
  `nextInt(311−(−56)+1) − 56` = **`nextInt(368) − 56`** (range of y1 = [−56, 311], not
  [−56, 247]). 368 is likewise non-pow2 (rejection loop possible). The follow-on
  expressions are correct as written: y2 = `nextInt(y1+64)−64`, y3 = `nextInt(y2+72)−64`,
  and the claim "always exactly 3 nextInt calls" holds (y1≥−56 ⇒ y2 draws; y2≥−64 ⇒ y3
  draws). C replay using 304 would desync the draw stream on every spring_lava attempt.

**Caveat (not refuted, flagged):** the "first 8 bytes … read little-endian" gloss on
`obfuscateSeed` is Guava library semantics (HashCode.asLong), not bytecode; the bytecode
confirms only the sha256().hashLong(seed).asLong() call chain. Cross-check against a
known seed vector when implementing.
