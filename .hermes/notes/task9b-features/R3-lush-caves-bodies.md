# R3 — Lush-caves bodies: multiface_growth, block_column, vegetation_patch (MC 26.2)

Source of truth: `javap -p -c -constants` (+ `-v` for BootstrapMethods) against
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server`; datapack facts from
`data/minecraft/worldgen/` + `data/minecraft/tags/block/` in that tree.
Disassembled by three parallel recon agents in the 9b resume session (run 2);
main session cross-checked the quirks called out below. RNG shorthand per A2/A3:
nextFloat = 1 draw; nextInt(pow2) = 1 draw; nextInt(non-pow2) = 1+ draws
(rejection loop). Labels: [VERIFIED-bytecode] / [VERIFIED-data] / [DERIVED] /
[UNVERIFIED].

Companions: R1 (flags/heightmaps), R2 (trees), R4 (small bodies; §9 there covers
the cave-vines provider draws that block_column consumes).

---

## 1. `minecraft:multiface_growth` (glow_lichen)

### 1.1 Configuration [VERIFIED-bytecode]

`MultifaceGrowthConfiguration` codec fields (defaults in parens): `block`
(required; must be a `MultifaceSpreadeableBlock` — 26.2 split; codec-validated),
`search_range` intRange(1,64) (10), `can_place_on_floor` (false),
`can_place_on_ceiling` (false), `can_place_on_wall` (false),
`chance_of_spreading` floatRange (0.5f), `can_be_placed_on` HolderSet (required).

Constructor builds `validDirections` in this exact order:
`if (ceiling) add(UP); if (floor) add(DOWN); if (wall) add HORIZONTAL` where
`Direction.Plane.HORIZONTAL` order = **NORTH, EAST, SOUTH, WEST**. glow_lichen
(ceiling=T, floor=F, wall=T) ⇒ `[UP, NORTH, EAST, SOUTH, WEST]` (5).

Helpers: `getShuffledDirections(r)` = `Util.shuffledCopy(validDirections, r)`;
`getShuffledDirectionsExcept(r, excluded)` = filter (keeps relative order) then
shuffle.

### 1.2 `Util.shuffle` [VERIFIED-bytecode]

```java
for (int i = list.size(); i > 1; --i)
    swap(i - 1, random.nextInt(i));      // draws nextInt(n), nextInt(n-1), ..., nextInt(2)
```
n−1 draws for size n. `Direction.allShuffled(r)` = shuffledCopy of
`Direction.values()` = [DOWN, UP, NORTH, SOUTH, WEST, EAST] ⇒ 5 draws
nextInt(6..2).

### 1.3 `MultifaceGrowthFeature.place` [VERIFIED-bytecode]

```java
if (!isAirOrWater(getBlockState(origin))) return false;   // 0 draws
// isAirOrWater = isAir() || is(Blocks.WATER)  — block IDENTITY (any water level)
if (!(cfg.placeBlock instanceof MultifaceSpreadeableBlock block)) return false;
List<Direction> list = cfg.getShuffledDirections(random);  // 4 draws (5,4,3,2)
if (placeGrowthIfPossible(block, level, origin, getBlockState(origin), cfg, random, list))
    return true;
for (Direction dir : list) {                               // shuffled order
    List<Direction> list2 = cfg.getShuffledDirectionsExcept(random, dir.getOpposite());
    // dir==UP: opposite DOWN not in validDirections → size 5 → 4 draws;
    // dir horizontal: opposite removed → size 4 → 3 draws. ALWAYS drawn.
    for (int i = 0; i < cfg.searchRange; ++i) {            // 20
        mutable.setWithOffset(origin, dir);                // *** BASE = origin — STATIONARY CURSOR ***
        BlockState s = getBlockState(mutable);
        if (!isAirOrWater(s) && !s.is(cfg.placeBlock)) break;
        if (placeGrowthIfPossible(block, level, mutable, s, cfg, random, list2))
            return true;
    }
}
return false;
```

The `searchRange` walk never advances (`setWithOffset(origin, dir)` every
iteration — verified `aload_3` base). Failed iterations draw nothing and write
nothing ⇒ semantically one evaluation of `origin+dir` per direction.

### 1.4 `placeGrowthIfPossible` [VERIFIED-bytecode]

```java
for (Direction dir : directions) {                        // the passed shuffled list
    BlockState neighbor = getBlockState(pos.relative(dir));
    if (neighbor.is(cfg.canBePlacedOn)) {
        BlockState placed = block.getStateForPlacement(stateAtPos, level, pos, dir);
        if (placed == null) return false;                 // ABORTS — does NOT try remaining dirs
        level.setBlock(pos, placed, 3);                   // flag 3
        level.getChunk(pos).markPosForPostProcessing(pos);
        if (random.nextFloat() < cfg.chanceOfSpreading)   // draw only on success, AFTER setBlock
            block.getSpreader().spreadFromFaceTowardRandomDirection(placed, level, pos, dir, random, true);
        return true;
    }
}
return false;
```

### 1.5 `MultifaceBlock.getStateForPlacement(current, level, pos, dir)` [VERIFIED-bytecode]

- `isValidStateForPlacement`: `isFaceSupported(dir)` (always true for lichen);
  `current.is(this) && hasFace(current, dir)` → null; else
  `canAttachTo(level, dir, pos+dir, state(pos+dir))` =
  `isFaceFull(supportShape) || isFaceFull(collisionShape)` of the neighbor's
  face toward the lichen — full cubes pass, **leaves pass via collision**.
- base = `current` if lichen (face merge); else `default[waterlogged=true]` if
  `current.getFluidState().isSourceOfType(WATER)`; else default. Then
  `setValue(faceProp(dir), true)`.
- Face property map: NORTH→north, EAST→east, SOUTH→south, WEST→west, UP→up,
  DOWN→down. `hasFace` = `getValueOrElse(prop, false)`.

### 1.6 Spreader — IS in the feature path [VERIFIED-bytecode]

On `nextFloat() < 0.5f`: `spreadFromFaceTowardRandomDirection(placed, level,
pos, face=dir, random, mark=true)`:

```java
Direction.allShuffled(random)              // 5 draws nextInt(6..2), ALWAYS
  .stream().map(sd -> spreadFromFaceTowardDirection(...)).filter(present).findFirst()
  // lazy — evaluates shuffled spreadDirs in order, stops at first success
```

`getSpreadFromFaceTowardDirection(state, pos, face, spreadDir)`:
- `spreadDir.getAxis() == face.getAxis()` → skip.
- `!hasFace(state, face) || hasFace(state, spreadDir)` → skip
  (isOtherBlockValidAsSource = false for DefaultSpreaderConfig).
- For type in [SAME_POSITION, SAME_PLANE, WRAP_AROUND]:
  - SAME_POSITION: sp = (pos, spreadDir)
  - SAME_PLANE: sp = (pos+spreadDir, face)
  - WRAP_AROUND: sp = (pos+spreadDir+face, spreadDir.getOpposite())
  - `canSpreadInto`: s = state(sp.pos); `stateCanBeReplaced` =
    `s.isAir() || s.is(lichen) || (s.is(WATER) && fluid isSource)`; AND
    `isValidStateForPlacement(level, s, sp.pos, sp.face)`. First passing type
    wins.
- `spreadToFace`: placed2 = getStateForPlacement(s, sp.pos, sp.face) (non-null
  here); mark; `setBlock(sp.pos, placed2, 2)` — **flag 2**.

**The spread path has NO can_be_placed_on check** — lichen spreads onto any
attachable face (incl. dirt/clay/leaves), unlike the primary placement.
Spread consumes no RNG beyond the 5 shuffle draws.

### 1.7 Draw ledger per body invocation (glow_lichen)

1. origin not air/water → 0 draws, return false.
2. shuffle: nextInt(5),4,3,2.
3. origin placement success → nextFloat [+ nextInt(6),5,4,3,2 iff < 0.5] → true.
4. else per direction (shuffled): 4 draws (UP) / 3 draws (horizontal); then
   success at origin+dir → nextFloat [+5 spread draws] → true.

### 1.8 Data [VERIFIED-data]

configured_feature/glow_lichen.json: block=glow_lichen, search_range=20,
ceiling=true, wall=true (floor default false), chance_of_spreading default 0.5,
can_be_placed_on = inline list {stone, andesite, diorite, granite,
dripstone_block, calcite, tuff, deepslate, sulfur, cinnabar} (26.2 adds
sulfur/cinnabar). Placed pipeline: count uniform[104,157] → height_range
uniform(above_bottom 0..abs 256) → in_square → surface_relative_threshold
(OCEAN_FLOOR_WG, max −13) → biome.

---

## 2. `minecraft:block_column` (cave_vine, cave_vine_in_moss, dripleaf columns)

### 2.1 `BlockColumnFeature.place` [VERIFIED-bytecode]

```java
int[] heights = new int[layers.size()];
int total = 0;
for (k = 0..n-1) { heights[k] = layers.get(k).height().sample(random); total += heights[k]; }
if (total == 0) return false;                       // the ONLY false path

MutableBlockPos writePos = origin.mutable();
MutableBlockPos scanPos  = writePos.mutable().move(direction);   // origin+dir BEFORE loop

for (int l = 0; l < total; ++l) {                   // NO RNG in the scan
    if (!allowedPlacement.test(level, scanPos)) { truncate(heights, total, l, prioritizeTip); break; }
    scanPos.move(direction);
}
for (layerIdx = 0..n-1) {
    int h = heights[layerIdx]; if (h == 0) continue;
    for (m = 0..h-1) {
        level.setBlock(writePos, layer.state().getState(level, random, writePos), 2); // draw PER BLOCK
        writePos.move(direction);
    }
}
return true;                                        // even if truncated to 0 blocks
```

- Scan is offset by one: tests `origin+dir*1 .. origin+dir*total`; writes go to
  `origin+dir*0 .. origin+dir*(total'-1)`. The origin is never predicate-tested;
  the last passing scan position is tested but not written.
- No isOutsideBuildHeight anywhere: OOB reads = VOID_AIR (matches `#air` tag ⇒
  the scan passes through the void boundary); OOB writes silently no-op. The
  provider draw happens before setBlock regardless.

### 2.2 `truncate(heights, total, l, prioritizeTip)` [VERIFIED-bytecode]

```java
int excess = total - l;
int step  = prioritizeTip ? 1 : -1;
int start = prioritizeTip ? 0 : heights.length - 1;
int end   = prioritizeTip ? heights.length : -1;
for (idx = start; idx != end && excess > 0; idx += step) {
    int reduce = Math.min(heights[idx], excess);
    excess -= reduce; heights[idx] -= reduce;
}
```
prioritize_tip=true removes from layer 0 forward (tip layer sacrificed last).

### 2.3 Config + draw ledger [VERIFIED-bytecode + data]

`BlockColumnConfiguration(layers[](height IntProvider NON_NEGATIVE, provider),
direction, allowed_placement, prioritize_tip)` — all fields required, no
defaults.

- cave_vine (R4 §9 has provider details): allowed=#air, dir=down, tip=true.
  Draws: nextInt(15)→{nextInt(20)|nextInt(3)|nextInt(7)}; then per body block
  nextInt(5); tip block nextInt(5)+nextInt(3) (age 23..25 →
  cave_vines[age,berries]).
- cave_vine_in_moss: identical except layer-0 height = weighted_list
  [uniform[0,3] w5, uniform[1,7] w1] ⇒ nextInt(6)→{nextInt(4)|nextInt(7)+1}.
- dripleaf big-dripleaf columns (4, one per facing E/W/S/N in file order):
  allowed = any_of[#air, matching_blocks water] (short-circuit OR, that order);
  dir=up; layer0 = big_dripleaf_stem[facing] height weighted_list
  [uniform[0,4] w2, const 0 w1] ⇒ nextInt(3)→{nextInt(5)|0 draws}; layer1 =
  big_dripleaf[facing,tilt=none] height 1 (0 draws). Simple providers ⇒ all
  setBlocks draw-free.

---

## 3. `minecraft:vegetation_patch` / `waterlogged_vegetation_patch`

### 3.1 Shape [VERIFIED-bytecode]

`VegetationPatchConfiguration` — 10 required codec fields (no defaults):
replaceable HolderSet, ground_state, vegetation_feature Holder<PlacedFeature>,
surface (floor→dir=DOWN / ceiling→dir=UP), depth IntProvider(1..128),
extra_bottom_block_chance, vertical_range intRange(1,256), vegetation_chance,
xz_radius IntProvider, extra_edge_column_chance.

`place`:
```java
int xR = xzRadius.sample(random) + 1;    // draw #1 (X first)
int zR = xzRadius.sample(random) + 1;    // draw #2
Set<BlockPos> set = placeGroundPatch(...);   // virtual → waterlogged override
distributeVegetation(...);
return !set.isEmpty();
```

`placeGroundPatch` — x OUTER asc, z INNER asc, −r..r inclusive:
- corner (xEdge && zEdge): skipped, 0 draws.
- edge-not-corner: skip w/o draw iff chance == 0; else 1 nextFloat, kept iff
  `f <= extraEdgeColumnChance` (**INCLUSIVE**: source `!(f > c)`).
- `mutable = origin + (x, 0, z)`;
  scan 1: `while (isAir(mutable) && k < verticalRange) mutable.move(dir)`;
  scan 2: `while (!isAir(mutable) && k < verticalRange) mutable.move(dir2)`.
- `mutable2 = mutable + dir`; if `isAir(mutable) && state(mutable2)
  .isFaceSturdy(dir2)` (FULL — full cubes only, leaves fail):
  - `depth = depth.sample(random)` (ConstantInt 0 draws; uniform 1 draw)
    `+ (extraBottomBlockChance > 0 && nextFloat() < ebc ? 1 : 0)`
    (draw iff chance > 0; **STRICT <**; depth draw FIRST).
  - `topPos = mutable2.immutable()` captured BEFORE placeGround mutates it.
  - `if (placeGround(..., mutable2, depth)) set.add(topPos)`.

`placeGround(level, cfg, replaceable, random, pos, maxDist)`:
```java
for (i = 0; i < maxDist; i++) {
    BlockState st = groundState.getState(level, random, pos);  // simple → 0 draws
    BlockState cur = getBlockState(pos);
    if (st.is(cur.getBlock())) continue;         // same block: NO write, NO move, i++ (quirk!)
    if (!replaceable.test(cur)) return i != 0;   // abort: true iff any progress
    level.setBlock(pos, st, 2);
    pos.move(dir);
}
return true;
```

`distributeVegetation`: for pos in **HashSet iteration order** (Vec3i.hashCode
= (y + z*31)*31 + x, JDK HashMap semantics — same emulation as R2 §12): iff
chance > 0: 1 nextFloat per element; iff `f < vegetationChance` (STRICT) →
`placeVegetation` (return ignored in base class).

`placeVegetation(pos)` = `vegetationFeature.value().place(level, gen, random,
pos.relative(dir2))` — the **non-biome PlacedFeature.place** path (empty
placement lists in all our configs), same RandomSource. `Feature.place` first
checks `level.ensureCanWrite(pos)` — false → return false with **0 draws**
(XZ-only ±1-chunk window; unreachable for grid configs but port the guard).

### 3.2 `WaterloggedVegetationPatchFeature` [VERIFIED-bytecode]

Overrides `placeGroundPatch` + `placeVegetation` only:

```java
Set ground = super.placeGroundPatch(...);
Set water = new HashSet<>();
for (pos : ground)                       // ground-set iteration order
    if (!isExposed(level, ground, pos, m)) water.add(pos);
for (pos : water)                        // water-set iteration order
    setBlock(pos, WATER.defaultBlockState(), 2);   // water[level=0]
return water;                            // feeds distributeVegetation AND place() return
```

`isExposed` = any of NORTH, EAST, SOUTH, WEST, DOWN (short-circuit, that
order; UP never checked) `isExposedDirection` =
`!state(pos+d).isFaceSturdy(level, pos+d, d.getOpposite())`. World-state only
(same-patch clay counts). No isWaterAt in 26.2. 0 draws.

`placeVegetation(pos /*water pos*/)`:
```java
if (super.placeVegetation(level, cfg, gen, random, pos.below())) {  // runs AT pos (below→+UP)
    BlockState s = getBlockState(pos);
    if (s.hasProperty(WATERLOGGED) && !s.getValue(WATERLOGGED))
        setBlock(pos, s.setValue(WATERLOGGED, true), 2);
    return true;
}
return false;
```

Pool = 1-deep: clay fills downward from floor-top, then each kept floor-top
cell is overwritten with water; a patch whose every column is exposed returns
**false** (empty water set).

### 3.3 Configs and pipelines [VERIFIED-data]

| cf | surface | depth | ebc | eec | veg chance | vr | vegetation |
|---|---|---|---|---|---|---|---|
| moss_patch (lush_caves_vegetation, count 125) | floor | 1 | 0.0 (no draw) | 0.3 | 0.8 | 5 | moss_vegetation |
| moss_patch_ceiling (lush_caves_ceiling_vegetation, count 125) | ceiling | uniform[1,2] | 0.0 | 0.3 | 0.08 | 5 | cave_vine_in_moss |
| clay_with_dripleaves (lush_caves_clay true-arm) | floor | 3 | 0.8 | 0.7 | 0.05 | 2 | dripleaf |
| clay_pool_with_dripleaves (false-arm, WATERLOGGED) | floor | 3 | 0.8 | 0.7 | 0.1 | 5 | dripleaf |

All xz_radius = uniform[4,7] (nextInt(4)+4, then +1 ⇒ 5..8). lush_caves_clay =
random_boolean_selector (nextBoolean = next(1)!=0; true→clay_with_dripleaves).
All three lush placed pipelines: count {125,125,62} → in_square → height_range
uniform(above_bottom0..abs256) → environment_scan(down/up, 12, target=solid,
allowed=#air) → random_offset(0, ±1) → biome.

Tags: `#moss_replaceable` = #base_stone_overworld + #cave_vines + #dirt + #mud
+ #moss_blocks + #grass_blocks (18 blocks); `#lush_ground_replaceable` = that +
clay + gravel + sand (21).

`moss_vegetation` (simple_block weighted, total 96 → nextInt(96)):
flowering_azalea 4, azalea 7, moss_carpet 25, short_grass 50,
tall_grass[lower] 10.

`dripleaf` = simple_random_selector (nextInt(5)) over [simple_block
small_dripleaf (weighted 4 facings E,W,N,S w1 each → nextInt(4)), 4×
big-dripleaf block_column (§2.3)].

---

## 4. Port notes (C mapping)

1. Direction encoding for these bodies: MC ordinals DOWN,UP,N,S,W,E with
   opposite/axis tables; glow_lichen facemask bits (hc_blocks layout) = down0,
   east1, north2, south3, up4, west5.
2. `isAirOrWater` = `hc_block_is_air(s) || s == HC_B_WATER` (block identity —
   waterlogged states are NOT the water block).
3. canAttachTo (mface primary neighbor + spread validity) = full cube OR
   leaves (`can_attach_to`); isFaceSturdy (vpatch surface + isExposed) = full
   cube only (`hc_block_is_full_cube`).
4. vpatch/wvpatch sets = jset (R2 §12 HashSet emulation, shared from
   features_tree.c).
5. `Feature.place`'s ensureCanWrite guard: implement at the body-invocation
   leaf (0 draws on reject) — unreachable for grid configs, fail-soft exactly
   like vanilla.
6. All these bodies' writes go through hc_feat_set_block (flags 2/3 both
   update the 4 FINAL maps in the ProtoChunk path).
