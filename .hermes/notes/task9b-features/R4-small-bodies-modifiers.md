# R4 — Small feature bodies + new placement modifiers/providers (MC 26.2)

Source of truth: `javap -p -c -constants` (plus `-v` for BootstrapMethods) against
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server`, disassembled in this session.
Datapack facts from `tools/golden/work/server/data/minecraft/worldgen/` and
`data/minecraft/tags/`. RNG shorthand per A2 (task9a-features): nextFloat = 1 draw;
nextInt(pow2) = 1 draw; nextInt(non-pow2) = 1+ draws (rejection loop);
`Mth.randomBetweenInclusive` ALWAYS draws (even lo==hi); `Mth.nextInt` draws 0 when lo>=hi.
Labels: [VERIFIED-bytecode] / [VERIFIED-data] / [UNVERIFIED].

Companion R-notes: R3 covers BlockColumnFeature/VegetationPatch etc.; §9 here covers only what
those bodies call INTO block/provider classes for cave vines.

---

## 1. SimpleBlockFeature  [VERIFIED-bytecode]

`net/minecraft/world/level/levelgen/feature/SimpleBlockFeature.place`, config
`SimpleBlockConfiguration` (record `(BlockStateProvider toPlace, boolean scheduleTick)`;
codec: `to_place` required, `schedule_tick` optionalFieldOf default **false** — verified in
the codec lambda; a 1-arg ctor also hardcodes false).

```java
public boolean place(FeaturePlaceContext<SimpleBlockConfiguration> ctx) {
    SimpleBlockConfiguration cfg = ctx.config();
    WorldGenLevel level = ctx.level();
    BlockPos pos = ctx.origin();
    BlockState state = cfg.toPlace().getOptionalState(level, ctx.random(), pos);  // DRAW(S) here
    if (state == null) return false;                          // (providers below never return null)
    if (!state.canSurvive(level, pos)) return false;          // §1.3 — happens AFTER the draw
    if (state.getBlock() instanceof DoublePlantBlock) {
        if (!level.isEmptyBlock(pos.above())) return false;   // above must be air; NO write, still true draw burned
        DoublePlantBlock.placeAt(level, state, pos, 2);       // §1.4 — TWO setBlock flag 2
    } else if (state.getBlock() instanceof MossyCarpetBlock) {
        MossyCarpetBlock.placeAt(level, pos, level.getRandom(), 2);  // NOT our configs; note: LEVEL rng, not feature rng!
    } else {
        level.setBlock(pos, state, 2);                        // flag 2
    }
    if (cfg.scheduleTick())
        level.scheduleTick(pos, level.getBlockState(pos).getBlock(), 1);  // re-READS the block
    return true;
}
```

- `getOptionalState` (BlockStateProvider base) = plain `return getState(level, random, pos)`;
  the null path exists for other provider subclasses, not ours. [VERIFIED-bytecode]
- The provider draw happens BEFORE canSurvive — a rejected position still burns the draw.
- No config in our set (nor any vanilla simple_block we saw) sets `schedule_tick`; when true it
  schedules `(pos, blockNowAtPos, delay=1)` — worldgen ProtoChunk records block ticks; steps ≤10
  never read them back, so it is replay-inert for block bytes but present in chunk NBT.
  [VERIFIED-bytecode for the call; NBT effect UNVERIFIED]

### 1.1 The four configs  [VERIFIED-data]

`data/minecraft/worldgen/configured_feature/*.json`:

- **grass_jungle**: `weighted_state_provider`, entries in order:
  `short_grass` weight 3, `fern` weight 1.
- **flower_default**: `weighted_state_provider`: `poppy` weight 2, `dandelion` weight 1.
- **tall_grass**: `simple_state_provider`, state `tall_grass[half=lower]` (Properties given
  explicitly in JSON).
- **spore_blossom**: `simple_state_provider`, state `spore_blossom` (default state).

### 1.2 Provider draw semantics  [VERIFIED-bytecode]

- `SimpleStateProvider.getState` = return field. **0 draws.**
- `WeightedStateProvider.getState` = `weightedList.getRandomOrThrow(random)`.
  `WeightedList.getRandomOrThrow`: throw if empty selector, else
  `selector.get(random.nextInt(totalWeight))` — **exactly one nextInt(totalWeight)**.
  Selector choice (WeightedList ctor): totalWeight==0 → null (isEmpty); totalWeight **< 64** →
  `Flat` (an Object[totalWeight] filled by `Arrays.fill(entries, i, i+w, value)` in **list
  order**; `get(i)` = `entries[i]`); ≥64 → `Compact` (linear cumulative scan `i -= w; if (i<0)
  return value;` in list order). Both selectors are value-equivalent; only the array layout
  differs. Weight codec: `weight` is a required NON_NEGATIVE_INT field (no default).
  - grass_jungle: totalWeight 4 → **nextInt(4)** (pow2, 1 draw); result 0..2 = short_grass,
    3 = fern.
  - flower_default: totalWeight 3 → **nextInt(3)** (non-pow2, rejection loop); 0..1 = poppy,
    2 = dandelion.

### 1.3 canSurvive chains for the concrete blocks  [VERIFIED-bytecode + BootstrapMethods]

Block classes (resolved through Blocks.<clinit> invokedynamic bootstrap args):
SHORT_GRASS/FERN → `TallGrassBlock`; POPPY/DANDELION → `FlowerBlock`;
TALL_GRASS/LARGE_FERN → `DoublePlantBlock`; SPORE_BLOSSOM → `SporeBlossomBlock`.

- `TallGrassBlock` and `FlowerBlock` override NEITHER `canSurvive` NOR `mayPlaceOn` → base
  `VegetationBlock.canSurvive(state, level, pos)`:
  ```java
  BlockPos below = pos.below();
  return mayPlaceOn(level.getBlockState(below), level, below);
  // default mayPlaceOn = belowState.is(BlockTags.SUPPORTS_VEGETATION)
  ```
  This is a 26.2 change: the old per-block dirt/farmland checks are now ONE tag test.
- `DoublePlantBlock.canSurvive` with half=lower (our state) falls through to the same
  VegetationBlock tag test on the block below. (half=upper branch — checks below is same block
  with half=lower — is unreachable in this feature.)
- `SporeBlossomBlock.canSurvive` =
  `Block.canSupportCenter(level, pos.above(), Direction.DOWN) && !level.isWaterAt(pos)`.
  `canSupportCenter(level, p, DOWN)`: `s = getBlockState(p)`; if `s.is(#unstable_bottom_center)`
  (tag = #fence_gates) → false; else `s.isFaceSturdy(level, p, DOWN, SupportType.CENTER)`.
  `isWaterAt(pos)` = `getFluidState(pos).is(FluidTags.WATER)`; fluid tag water =
  {water, flowing_water} [VERIFIED-data]. So: ceiling face-sturdy-down-center AND not in water.
  → C needs face-sturdiness (CENTER support at least for full cubes) — for worldgen ceilings
  these are stone/moss/clay full blocks; treat isFaceSturdy(CENTER)=true for collision-full
  blocks, flag exotic ceilings [UNVERIFIED for the general shape table — same gap as §2 vines].

Tag expansion [VERIFIED-data]: `#supports_vegetation` = `#substrate_overworld` + farmland;
`#substrate_overworld` = `#dirt` + `#mud` + `#moss_blocks` + `#grass_blocks`
= {dirt, coarse_dirt, rooted_dirt} + {mud, muddy_mangrove_roots} + {moss_block,
pale_moss_block} + {grass_block, podzol, mycelium} + farmland → **11 blocks**.

### 1.4 DoublePlantBlock.placeAt  [VERIFIED-bytecode]

```java
public static void placeAt(LevelAccessor level, BlockState state, BlockPos pos, int flags) {
    BlockPos above = pos.above();
    level.setBlock(pos,   copyWaterloggedFrom(level, pos,   state.setValue(HALF, LOWER)), flags); // write #1
    level.setBlock(above, copyWaterloggedFrom(level, above, state.setValue(HALF, UPPER)), flags); // write #2
}
```
Called with flags=**2** from SimpleBlockFeature. Both halves written, lower first.
`copyWaterloggedFrom(level, p, s)` = if s has WATERLOGGED property → set it from
`level.isWaterAt(p)`, else return s unchanged. `tall_grass` has no waterlogged property → both
writes are exactly `tall_grass[half=lower]` and `tall_grass[half=upper]`. Zero draws.

Return-value summary: false iff provider gave null (never here) / canSurvive failed / double
plant with non-air above; true otherwise. Draw count is the SAME on all paths (provider draw
only).

---

## 2. VinesFeature  [VERIFIED-bytecode]

Used by two placed features (both `"feature": "minecraft:vines"`, config = NONE)
[VERIFIED-data]:

- `placed_feature/vines.json`: count **127** → in_square → height_range uniform
  abs **64 .. 100** → biome.
- `placed_feature/classic_vines_cave_feature.json`: count **256** → in_square → height_range
  uniform above_bottom **0** (= −64) .. abs **256** → biome.

```java
public boolean place(FeaturePlaceContext<NoneFeatureConfiguration> ctx) {
    WorldGenLevel level = ctx.level(); BlockPos pos = ctx.origin(); ctx.config(); // popped
    if (!level.isEmptyBlock(pos)) return false;             // getBlockState(pos).isAir()
    for (Direction dir : Direction.values()) {              // DOWN, UP, NORTH, SOUTH, WEST, EAST
        if (dir == Direction.DOWN) continue;
        if (VineBlock.isAcceptableNeighbour(level, pos.relative(dir), dir)) {
            level.setBlock(pos,
                Blocks.VINE.defaultBlockState().setValue(VineBlock.getPropertyForFace(dir), true),
                2);                                          // flag 2
            return true;                                     // FIRST acceptable face wins
        }
    }
    return false;
}
```

- **Zero RNG.** Return true iff exactly one vine block written (single face set).
- Direction enum order [VERIFIED-bytecode field order]: DOWN, UP, NORTH, SOUTH, WEST, EAST —
  probe order is therefore **UP, NORTH, SOUTH, WEST, EAST** (DOWN skipped).
- `VineBlock.getPropertyForFace(dir)` = PROPERTY_BY_DIRECTION.get(dir) → vine boolean property
  {up, north, south, west, east}; default state all false; exactly one set true.
- `VineBlock.isAcceptableNeighbour(level, neighborPos, dir)` =
  `MultifaceBlock.canAttachTo(level, dir, neighborPos, level.getBlockState(neighborPos))`:
  ```java
  return Block.isFaceFull(nState.getBlockSupportShape(level, nPos), dir.getOpposite())
      || Block.isFaceFull(nState.getCollisionShape(level, nPos),   dir.getOpposite());
  ```
  i.e. the neighbor's face TOWARD the vine (dir.getOpposite()) must be a full 16×16 face of
  either its support shape or collision shape. For the C port: full opaque cubes (stone, dirt,
  logs, leaves — note LeavesBlock support shape is the full block even though collision differs)
  pass; air/plants/fluids fail. A per-block "full face" classification is required — the same
  table §1.3 needs. [VERIFIED-bytecode for the call chain; per-block shape table is separate
  work — see open questions.]

---

## 3. BambooFeature  [VERIFIED-bytecode]

Config `ProbabilityFeatureConfiguration`: single field `probability`, codec
`floatRange(0.0f, 1.0f).fieldOf("probability")` (required, no default) [VERIFIED-bytecode].

Configs [VERIFIED-data]: `bamboo_no_podzol` = `{"type":"minecraft:bamboo","config":
{"probability":0.0}}`; `bamboo_some_podzol` probability **0.2**. Grid-relevant placed feature
`bamboo_light` (jungle step 9) = rarity_filter(4) → in_square → heightmap(MOTION_BLOCKING) →
biome → feature **bamboo_no_podzol**. (`bamboo`/`bamboo_vegetation` → bamboo_some_podzol,
bamboo_jungle-only biomes.)

Static states (from `<clinit>`, all on `Blocks.BAMBOO` = BambooStalkBlock):

| field | state |
|---|---|
| BAMBOO_TRUNK | `bamboo[age=1, leaves=none, stage=0]` |
| BAMBOO_FINAL_LARGE | `bamboo[age=1, leaves=large, stage=1]` |
| BAMBOO_TOP_LARGE | `bamboo[age=1, leaves=large, stage=0]` |
| BAMBOO_TOP_SMALL | `bamboo[age=1, leaves=small, stage=0]` |

```java
public boolean place(ctx) {
    int placed = 0;                                    // slot 2
    BlockPos origin = ctx.origin(); WorldGenLevel level = ctx.level();
    RandomSource r = ctx.random(); ProbabilityFeatureConfiguration cfg = ctx.config();
    BlockPos.MutableBlockPos m  = origin.mutable();    // trunk cursor
    BlockPos.MutableBlockPos m2 = origin.mutable();    // podzol cursor
    if (level.isEmptyBlock(m)) {
        if (Blocks.BAMBOO.defaultBlockState().canSurvive(level, m)) {   // §3.1
            int height = r.nextInt(12) + 5;            // DRAW 1: nextInt(12) non-pow2; height 5..16
            if (r.nextFloat() < cfg.probability) {     // DRAW 2: nextFloat ALWAYS drawn (fcmpg/ifge, strict <)
                int rad = r.nextInt(4) + 1;            // DRAW 3: nextInt(4) pow2 — ONLY inside the if
                for (int x = origin.getX()-rad; x <= origin.getX()+rad; x++)        // x outer
                    for (int z = origin.getZ()-rad; z <= origin.getZ()+rad; z++) {  // z inner
                        int dx = x-origin.getX(), dz = z-origin.getZ();
                        if (dx*dx + dz*dz <= rad*rad) {
                            m2.set(x, level.getHeight(Heightmap.Types.WORLD_SURFACE, x, z) - 1, z); // LIVE final map
                            if (level.getBlockState(m2).is(BlockTags.BENEATH_BAMBOO_PODZOL_REPLACEABLE))
                                level.setBlock(m2, Blocks.PODZOL.defaultBlockState(), 2);
                        }
                    }
            }
            for (int i = 0; i < height && level.isEmptyBlock(m); i++) {  // stops at first non-air
                level.setBlock(m, BAMBOO_TRUNK, 2);
                m.move(Direction.UP, 1);
            }
            if (m.getY() - origin.getY() >= 3) {       // = number of trunk blocks actually placed
                level.setBlock(m, BAMBOO_FINAL_LARGE, 2);                          // at first-free / blocking pos!
                level.setBlock(m.move(Direction.DOWN, 1), BAMBOO_TOP_LARGE, 2);    // overwrites trunk
                level.setBlock(m.move(Direction.DOWN, 1), BAMBOO_TOP_SMALL, 2);    // overwrites trunk
            }
        }
        placed++;   // iinc target of BOTH the canSurvive-false jump and the <3 jump
    }
    return placed > 0;
}
```

Order-critical facts:

1. **Draw order**: nextInt(12) height FIRST, then nextFloat ALWAYS (even for probability 0.0 —
   bamboo_no_podzol still burns 1 float per body run), then nextInt(4) only when
   `nextFloat < probability` strictly. bamboo_no_podzol: exactly 2 draws per body that reaches
   the RNG (empty + canSurvive); 0 draws when the isEmptyBlock/canSurvive gates fail (they
   precede all draws).
2. Podzol disc: x-major then z; strict Euclidean `dx²+dz² <= rad²`; target y =
   `getHeight(WORLD_SURFACE, x, z) − 1` — the **LIVE FINAL heightmap** (updated by prior
   feature writes; needs the 9b heightmap substrate). Replaces only blocks in
   `#beneath_bamboo_podzol_replaceable` = `#substrate_overworld` (10 blocks, §1.3 minus
   farmland). setBlock flag 2 (heightmap-updating path).
3. Trunk loop: up to `height` BAMBOO_TRUNK writes; stops early at first non-air. The 3 top
   writes run iff ≥3 trunk blocks were placed; the FINAL_LARGE write lands on the position
   where the loop stopped — if the loop stopped on a non-air block, that block is
   **overwritten** unconditionally. Then the two blocks below are rewritten (large, small):
   net final column from top: `...[age=1,leaves=large,stage=1] / [large,stage=0] /
   [small,stage=0] / trunk(none,0)×(n−3)`.
4. Return value = `isEmptyBlock(origin)` — the `placed++` executes even when canSurvive fails
   or the tip branch is skipped. (Quirk verified: `ifeq 386` from canSurvive jumps TO the iinc.)

### 3.1 Bamboo canSurvive  [VERIFIED-bytecode + data]

`BambooStalkBlock.canSurvive` = `level.getBlockState(pos.below()).is(BlockTags.SUPPORTS_BAMBOO)`.
`#supports_bamboo` = `#sand` + `#substrate_overworld` + {bamboo, bamboo_sapling, gravel,
suspicious_gravel}.

---

## 4. NoiseThresholdCountPlacement + Biome.BIOME_INFO_NOISE  [VERIFIED-bytecode]

`placement/NoiseThresholdCountPlacement extends RepeatingPlacement`; fields
`double noiseLevel; int belowNoise; int aboveNoise` (codec: `noise_level` DOUBLE,
`below_noise` INT, `above_noise` INT — all required).

```java
protected int count(RandomSource random, BlockPos pos) {   // random is DEAD — zero draws
    double v = Biome.BIOME_INFO_NOISE.getValue(pos.getX() / 200.0, pos.getZ() / 200.0, false);
    return v < this.noiseLevel ? this.belowNoise : this.aboveNoise;   // dcmpg ifge: strict <, NaN→above
}
```

- Zero RNG; count depends only on the (pre-modifier) position — in our pipelines it runs FIRST
  on the chunk origin `(minBlockX, −64, minBlockZ)`, so v is per-chunk constant.
- Only user in the four grid biomes: `patch_tall_grass_2` (lush_caves step 9):
  noise_threshold_count(noise_level −0.8, below 0, above 7) → rarity_filter(32) → in_square →
  heightmap(MOTION_BLOCKING) → biome → count(96) → random_offset(trapezoid ±7 xz / ±3 y) →
  block_predicate_filter(matching_block_tag #air) [VERIFIED-data, quoted in session].
  belowNoise=0 ⇒ empty stream when v < −0.8.

### 4.1 BIOME_INFO_NOISE construction — vs core/src/biome_temp.c

`Biome.<clinit>` [VERIFIED-bytecode]:

```java
TEMPERATURE_NOISE        = new PerlinSimplexNoise(new WorldgenRandom(new LegacyRandomSource(1234L)), ImmutableList.of(0));
FROZEN_TEMPERATURE_NOISE = new PerlinSimplexNoise(new WorldgenRandom(new LegacyRandomSource(3456L)), ImmutableList.of(-2, -1, 0));
BIOME_INFO_NOISE         = new PerlinSimplexNoise(new WorldgenRandom(new LegacyRandomSource(2345L)), ImmutableList.of(0));
```

**BIOME_INFO_NOISE is EXACTLY the same class/construction as TEMPERATURE_NOISE with seed 2345
instead of 1234** (single octave {0}, LegacyRandomSource-backed WorldgenRandom). The C port
already has it: `core/src/biome_temp.c` `psn_init(&g_info_noise, 2345, 0)` (it is used there by
the FROZEN temperature modifier at scales ×0.2/×0.09). `PerlinSimplexNoise.getValue(x, y,
useNoiseOffsets)` re-verified this session: per octave `value += SimplexNoise.getValue(x*f +
(useOffsets ? xo : 0), y*f + (useOffsets ? yo : 0)) * g; f /= 2; g *= 2` — with
useNoiseOffsets=**false** (both the biome-temp callers and noise_threshold_count) the offsets
add 0.0, matching `psn_value`. So noise_threshold_count needs only:
`psn_value(&g_info_noise, x / 200.0, z / 200.0)` — exposing that static from biome_temp.c (it
is currently file-static) is the only code change.

---

## 5. SurfaceWaterDepthFilter  [VERIFIED-bytecode]

```java
protected boolean shouldPlace(PlacementContext ctx, RandomSource random, BlockPos pos) {
    int of = ctx.getHeight(Heightmap.Types.OCEAN_FLOOR,  pos.getX(), pos.getZ());  // read #1
    int ws = ctx.getHeight(Heightmap.Types.WORLD_SURFACE, pos.getX(), pos.getZ()); // read #2
    return ws - of <= this.maxWaterDepth;    // if_icmpgt → false; signed int, inclusive
}
```

- Reads **OCEAN_FLOOR then WORLD_SURFACE** — the LIVE FINAL heightmap enum constants (NOT the
  *_WG ones); both are updated by feature-stage `setBlock` (flag-2/3) writes per A4, so this
  filter observes earlier features' writes (tree-after-tree coupling).
- Zero RNG. getHeight semantics = A2 §5 (`getFirstAvailable` = one above top counted block; the
  two maps count different predicates, so ws−of = water/leaf column depth above the floor).
  trees_jungle/trees_water use max_water_depth 0 (JSON, task9a census).

---

## 6. IntProviders: WeightedListInt + TrapezoidInt  [VERIFIED-bytecode]

### 6.1 `minecraft:weighted_list` — WeightedListInt

Fields: `WeightedList<IntProvider> distribution` (codec field `distribution`, non-empty);
ctor precomputes minValue/maxValue = min/max over entries' provider bounds (no draws).

```java
public int sample(RandomSource r) {
    return distribution.getRandomOrThrow(r)   // DRAW #1..: nextInt(totalWeight) — see §1.2
                       .sample(r);            // then the CHOSEN provider's own draws
}
```

Draw order: **outer weight pick first, then exactly the chosen provider's sample**. Grid user:
cave_vine layer-0 height (R3's BlockColumnFeature) — distribution `[uniform[0,19] w2,
uniform[0,2] w3, uniform[0,6] w10]`, totalWeight 15 (<64 ⇒ Flat selector):
`nextInt(15)` (non-pow2) → index 0–1 ⇒ `nextInt(20)`, 2–4 ⇒ `nextInt(3)`, 5–14 ⇒ `nextInt(7)`
(all via `Mth.randomBetweenInclusive`, always drawn). Exactly 2 nextInt calls per sample
(each rejection-capable).

### 6.2 `minecraft:trapezoid` (INT provider) — TrapezoidInt

Record `(int minInclusive, int maxInclusive, int plateau)`. Codec: **all three fields
REQUIRED** (`min`, `max`, `plateau` — unlike TrapezoidHeight where plateau is optional-0);
validated max≥min and plateau≤max−min.

```java
public int sample(RandomSource r) {
    if (plateau == 0 && maxInclusive == -minInclusive)                 // SYMMETRIC FAST PATH
        return r.nextInt(maxInclusive + 1) - r.nextInt(maxInclusive + 1);   // draw A minus draw B
    int range = maxInclusive - minInclusive;
    if (plateau == range)                                              // uniform degenerate
        return Mth.randomBetweenInclusive(r, minInclusive, maxInclusive);   // 1 draw
    int half  = (range - plateau) / 2;                                 // idiv (floor toward 0)
    int upper = range - half;
    return minInclusive
         + Mth.randomBetweenInclusive(r, 0, upper)                     // draw #1: nextInt(upper+1)
         + Mth.randomBetweenInclusive(r, 0, half);                     // draw #2: nextInt(half+1)
}
```

**The symmetric fast path does not exist in TrapezoidHeight** (A2 §3.2) — the int provider's
first branch fires for every `min == −max, plateau == 0` config. Our random_offset users
(patch_grass_jungle, flower_warm, patch_tall_grass_2, and the other patch_*/mushroom
step-9 features): xz_spread trapezoid(−7, 7, 0) ⇒ `nextInt(8) − nextInt(8)` (two pow2 draws,
1 each); y_spread trapezoid(−3, 3, 0) ⇒ `nextInt(4) − nextInt(4)`. Value = firstDraw −
secondDraw (NOT min + a + b): with identical draw streams the general-path formula gives a
different value (a+b−7 vs a−b) — port the branch structure literally.

Per random_offset position (A2 order xzSpread→x, ySpread→y, xzSpread→z): 6 nextInt draws in
order x(a,b), y(a,b), z(a,b). cave_vines' random_offset uses bare ints (`xz_spread: 0,
y_spread: -1` ⇒ ConstantInt, 0 draws).

Note: `TrapezoidInt.triangle(n)` factory = `of(-n, n, 0)` — always the fast path.

---

## 7. Block predicates (new in 9b set)  [VERIFIED-bytecode]

All RNG-free; dispatch through raw `test(Object,Object)` bridges as in A2 §6.

- **any_of** (`AnyOfPredicate extends CombiningPredicate`): iterator over `predicates` in JSON
  list order, `if (p.test(level,pos)) return true;` — **short-circuit OR, first true wins**;
  empty list → false.
- **would_survive** (`WouldSurvivePredicate`): fields `(Vec3i offset, BlockState state)`;
  `test = state.canSurvive(level, pos.offset(offset))`. Codec: `offset` optionalFieldOf default
  `Vec3i.ZERO` (Vec3i.offsetCodec(16): each component in [−16,16]); `state` required. The
  canSurvive dispatches to the state's block exactly as §1.3 (e.g. random_patch of
  pumpkin/melon uses would_survive with the crop state — resolve each concrete block's
  override when its feature is ported).
- **matching_blocks** (`MatchingBlocksPredicate extends StateTestingPredicate`): base test =
  `test(level.getBlockState(pos.offset(offset)))` (offset optional, default ZERO, A2 §6);
  subclass test = `state.is(blocksHolderSet)` — direct-list HolderSet of blocks
  (`blocks` field), block identity (any state of the block matches).
- **true** (`TrueBlockPredicate`): singleton INSTANCE, `test` returns 1. [trivial]
- **has_sturdy_face** (`HasSturdyFacePredicate` — appears in cave_vines env-scan target,
  documented for R3): `p = pos.offset(offset); return getBlockState(p).isFaceSturdy(level, p,
  direction)` — 3-arg isFaceSturdy = SupportType.FULL. Same shape-table dependency as §2.

---

## 8. SnowAndFreezeFeature (freeze_top_layer) — 26.2 re-verification vs A5

Full place() re-disassembled this session. **A5's reconstruction is CONFIRMED on every point**:
16×16 loop `for dx 0..15 (x outer) { for dz 0..15 (z inner) }`; per column
`y = level.getHeight(MOTION_BLOCKING, x, z)` (LIVE final map); `top=(x,y,z)`,
`below=top.below()`; `biome = level.getBiome(top).value()` (per-column fiddled-zoom 3-D lookup
at the heightmap y); then in order:

1. `if (biome.shouldFreeze(level, below, false)) setBlock(below, ICE.defaultState, 2);`
2. `if (biome.shouldSnow(level, top)) { setBlock(top, SNOW.defaultState, 2);
   s = getBlockState(below); if (s.hasProperty(SNOWY)) setBlock(below, s.setValue(SNOWY,true), 2); }`

**ZERO RNG** (no RandomSource call site in place / shouldFreeze / shouldSnow /
getPrecipitationAt / warmEnoughToRain paths — only the temperature-noise statics, which are
their own RNG-free evaluations). Return value: **always true** (iconst_1), regardless of
placements — A5 did not state this; it matters only for the (ignored) placeWithContext
aggregate.

Additions/precisions beyond A5 (not discrepancies):

- `shouldFreeze(level, pos, false)` full gate order: `warmEnoughToRain(pos, seaLevel)` → false;
  then `isInsideBuildHeight(pos.getY())` (A5 omitted this check for shouldFreeze; it lists it
  only for shouldSnow) && `getBrightness(LightLayer.BLOCK, pos) < 10` &&
  `getFluidState(pos).is(Fluids.WATER)` (the FLUID object, source-only water — flowing water's
  fluid is FLOWING_WATER and fails) && `getBlockState(pos).getBlock() instanceof LiquidBlock`;
  mustBeAtEdge=false skips the 4-neighbour water check → true.
- `shouldSnow(level, pos)` confirmed: precipitation==SNOW (temperature < 0.15f) →
  isInsideBuildHeight && block-light < 10 && (state.isAir() || state.is(Blocks.SNOW)) &&
  `SNOW.defaultBlockState().canSurvive(level, pos)`.
- Both freeze and snow are evaluated for EVERY column (no early column skip) — the ice check
  runs even where snow later fails, and both can fire in one column.
- In the current grid (jungle/lush/beach/river) temperature ≥0.15 everywhere ⇒ inert, as A5
  proved; the walk item stays a zero-draw f-line.
- Open (only matters for cold regions): `getBrightness(BLOCK, pos)` through WorldGenRegion's
  light engine during decoration — expected 0 (no block light sources counted mid-gen), not
  yet probe-verified.

---

## 9. Cave-vines block helpers (coordination with R3)  [VERIFIED-bytecode + data]

**Negative finding**: 26.2 has NO `CaveVines.getBodyBlock/getHeadBlock` helpers. The
`CaveVines` interface contains only `use` (player harvest), `hasGlowBerries`, `emission`, and
the SHAPE/BERRIES statics. `BlockColumnFeature` for `cave_vine.json` gets every state straight
from the JSON providers — no block-class delegation at all. What R3's body actually calls:

- Body blocks (layer 0): `weighted_state_provider` — `cave_vines_plant[berries=false]` w4,
  `[berries=true]` w1; totalWeight 5 (<64, Flat) ⇒ per sampled block **nextInt(5)** (non-pow2):
  0–3 ⇒ berries=false, 4 ⇒ berries=true.
- Head block (layer 1, height ConstantInt 1): `randomized_int_state_provider`
  (`RandomizedIntStateProvider.getState`):
  ```java
  BlockState s = source.getState(level, random, pos);          // DRAW #1: nextInt(5) as above,
                                                               // on cave_vines[age=0,berries=?]
  if (property == null || !s.hasProperty(property))            // lazy: findProperty(s, "age"),
      { property = findProperty(s, propertyName);              // cached in the provider field
        if (property == null) return s; }                      // ← would SKIP draw #2 entirely
  return s.setValue(property, values.sample(random));          // DRAW #2: uniform[23,25]
                                                               //   = rBI → nextInt(3)
  ```
  For cave_vines the `age` IntegerProperty exists ⇒ both draws always: **nextInt(5) then
  nextInt(3)**, head state `cave_vines[age=23..25, berries=?]`. (The property-missing skip is
  a datapack-only hazard; note it in the compile-time validator.)
- State-space facts for the block table: `cave_vines` (CaveVinesBlock) = AGE 0..25 × BERRIES;
  `cave_vines_plant` (CaveVinesPlantBlock) = BERRIES only. BERRIES =
  BlockStateProperties.BERRIES.
- The cave_vines PIPELINE (placed_feature/cave_vines.json, quoted in session): count(188) →
  in_square → height_range uniform(above_bottom 0 .. abs 256) → environment_scan(up, 12,
  target has_sturdy_face(down), allowed #air) → random_offset(0, −1) → biome — env-scan with a
  NON-default allowed condition, so A2 §2.9's "break→final test" path is LIVE here (allowed
  fails on non-air BEFORE target retest; the final post-loop target test then runs at that
  non-air position, where has_sturdy_face(down) can pass — that is exactly how the scan stops
  ON the ceiling block; random_offset then steps one down).

---

## 10. Draw-count quick reference (per body invocation)

| body | draws (in order) |
|---|---|
| simple_block (weighted provider) | 1 × nextInt(totalWeight) — always, even if canSurvive then fails |
| simple_block (simple provider) | 0 |
| vines | 0 |
| bamboo (gates passed) | nextInt(12); nextFloat; [nextInt(4) iff float < prob] |
| bamboo (isEmpty/canSurvive gate failed) | 0 |
| noise_threshold_count | 0 |
| surface_water_depth_filter | 0 |
| weighted_list int.sample | nextInt(totalWeight) + chosen provider's draws |
| trapezoid int (min=−max, plateau 0) | nextInt(max+1), nextInt(max+1) → a−b |
| freeze_top_layer | 0 |
| randomized_int_state_provider | source draws, then values.sample (skipped iff property missing) |

Flag census: every write in §§1–3, 8 is `setBlock(..., 2)` (the DoublePlantBlock/podzol/bamboo
tips included); flag 2 = the heightmap-updating ProtoChunk path (A4) — these ARE visible to
later MOTION_BLOCKING/WORLD_SURFACE/OCEAN_FLOOR reads (§3 podzol disc, §5, §8).

## 11. Open questions / risks

1. **Face-sturdiness table** (blocks §2 vines-attach, §1.3 spore_blossom ceiling, §7
   has_sturdy_face): `getBlockSupportShape`/`isFaceSturdy(CENTER|FULL)` need a per-block
   classification (full cube vs not; LeavesBlock overrides support shape to full). Not
   reconstructed here — needs its own recon or a conservative "occlusion/full-cube" bit reused
   from the 9a underwater_magma occlusion work, verified against golden diffs.
2. **WorldGenRegion block-light during decoration** (§8): assumed 0; inert for the current
   grid; verify by probe before porting a snowy region.
3. `schedule_tick` / `scheduleTick` NBT side effects (§1): replay-inert for block bytes; if a
   future gate compares full chunk NBT the ProtoChunk tick list must be modeled.
4. MossyCarpetBlock.placeAt draws from `level.getRandom()` (the ServerLevel/region RNG, NOT the
   feature WorldgenRandom) — not reachable from the four target biomes' configs; if pale-garden
   support is ever added this needs its own seeding recon.
5. WeightedList Compact selector (totalWeight ≥ 64) is value-equivalent to Flat but untested by
   any grid config (all our lists total < 64); implement the single cumulative scan and it
   covers both.
