# Task 10 / R5c — `minecraft:rooted_azalea_tree` (RootSystemFeature + azalea tree: BendingTrunkPlacer, RandomSpreadFoliagePlacer) — 26.2 bytecode recon

Source of truth: `javap -p -c -constants -cp
tools/golden/libs/extracted/server-26.2.jar <fqcn>` (real unobfuscated 26.2
server). Every claim cites `method@offset`. Goal: C-ready spec, implementable
without re-reading bytecode. Reference JSON:
`reference/configured_feature/rooted_azalea_tree.json`,
`reference/placed_feature/rooted_azalea_tree.json`,
`reference/configured_feature/azalea_tree.json`.

RNG rule of this document: "draw" = one call into the shared
`FeaturePlaceContext.random` (`hc_wgr_t`). `nextInt(1)` IS a draw. All int
providers here are `constant` or `uniform`; `uniform[a,b].sample =
a + nextInt(b-a+1)` (1 draw) — matches `hc_featx_iprov_sample` HC_IP_UNIFORM.

---

## 1. Concrete config values

### 1.1 rooted_azalea_tree (type `minecraft:root_system`, RootSystemConfiguration)

Record field order (constructor `RootSystemConfiguration.<init>` putfields
@4..88; accessor names below):

| field | value |
|---|---|
| `treeFeature` | **Holder\<PlacedFeature\>** — inline `{"feature":"minecraft:azalea_tree","placement":[]}` (codec `PlacedFeature.CODEC` — RootSystemConfiguration `lambda$static$0@1-6` reads `PlacedFeature.CODEC.fieldOf("feature")`) |
| `requiredVerticalSpaceForTree` | 3 |
| `levelTestDistance` | 0 |
| `maxLevelDeviation` | 0 |
| `rootRadius` | 3 |
| `rootReplaceable` | HolderSet `#minecraft:azalea_root_replaceable` |
| `rootStateProvider` | simple `minecraft:rooted_dirt` |
| `rootPlacementAttempts` | 20 |
| `rootColumnMaxHeight` | 100 |
| `hangingRootRadius` | 3 |
| `hangingRootsVerticalSpan` | 2 |
| `hangingRootStateProvider` | simple `minecraft:hanging_roots[waterlogged=false]` |
| `hangingRootPlacementAttempts` | 20 |
| `allowedVerticalWaterForTree` | 2 |
| `allowedTreePosition` | `all_of[ any_of[ tag #air, tag #replaceable_by_trees ], tag #azalea_grows_on @(0,-1,0) ]` — BlockPredicate, **0 draws** |

The `feature` field is a **PlacedFeature holder** (NOT a raw
ConfiguredFeature): `placeDirtAndTree@107-124` does
`config.treeFeature().value()` → checkcast `PlacedFeature` → invoke
`PlacedFeature.place(level, chunkGenerator, random, pos)`.

### 1.2 Placement pipeline (rooted_azalea_tree placed feature)

`count uniform[1,2]` → `in_square` → `height_range uniform[above_bottom 0,
absolute 256]` → `environment_scan{dir=up, max_steps=12, target=solid,
allowed=tag #air}` → `random_offset{xz=0, y=-1}` → `biome`. All modifier
kinds already exist (HC_PM_COUNT/IN_SQUARE/HEIGHT_RANGE/ENV_SCAN/
RANDOM_OFFSET/BIOME). Net effect: origin = the air cell **just below a solid
ceiling** reached by scanning up ≤12 through air.

### 1.3 azalea_tree (type `minecraft:tree`, TreeConfiguration)

| field | value |
|---|---|
| `trunk_placer` | `bending_trunk_placer{base_height=4, height_rand_a=2, height_rand_b=0, min_height_for_leaves=3, bend_length=uniform[1,2]}` (codec default of `min_height_for_leaves` is 1 — BendingTrunkPlacer `lambda$static$0@8-14`; azalea sets 3) |
| `trunk_provider` | simple `minecraft:oak_log[axis=y]` |
| `foliage_placer` | `random_spread_foliage_placer{radius=3, offset=0, foliage_height=2, leaf_placement_attempts=50}` |
| `foliage_provider` | **weighted**: `azalea_leaves[distance=7,persistent=false,waterlogged=false]` weight 3, `flowering_azalea_leaves[…]` weight 1 (totalWeight 4, list order as written) |
| `below_trunk_provider` | **simple** `minecraft:rooted_dirt` (NOT rule_based — unlike the overworld oak/birch configs) |
| `minimum_size` | `two_layers_feature_size` all defaults: limit=1, lower_size=0, upper_size=1, no `min_clipped_height` |
| `ignore_vines` | false |
| `decorators` | `[]` — **zero decorator draws** |
| `force_dirt` / `dirt_provider` | do not exist in 26.2. The old setDirtAt/isDirt preamble is gone; the only below-trunk mechanism is `TrunkPlacer.placeBelowTrunkBlock` (§5 step 2). |

---

## 2. RootSystemFeature.place — full algorithm

### 2.1 `place(ctx)` (place@0-85)

```
1. if (!level.getBlockState(origin).isAir()) return false        // place@10-24, 0 draws
2. mutable = origin.mutable()                                     // place@46-51
3. ok = placeDirtAndTree(level, chunkGen, cfg, random, mutable, origin)  // place@53-66
4. if (ok) placeRoots(level, cfg, random, origin, mutable)        // place@69-81
5. return true                                                    // place@84 — ALWAYS true once origin was air
```

The initial air check is at the **origin** (post-pipeline pos). The trace
`placed` flag for this feature = "origin was air", independent of whether a
tree was actually placed.

### 2.2 `placeDirtAndTree` (placeDirtAndTree@0-155)

```
for (i = 0; i < rootColumnMaxHeight /*100*/; i++):                 // @3-9, @148-151
    mutable.move(UP)                                               // @12-17  (first tested pos = origin.y+1 = the ceiling block)
    if (level.getHeight(WORLD_SURFACE, mutable) < mutable.getY())  // @21-37
        return false                                               // @40-41 — hard abort
    if (allowedTreePosition.test(level, mutable)                   // @42-54, 0 draws
        && spaceForTree(level, cfg, mutable)):                     // @57-64, 0 draws
        below = mutable.below()                                    // @67-72
        if (level.getFluidState(below).is(FluidTags.LAVA)          // @74-88
            || !level.getBlockState(below).isSolid())              // @91-102
            return false                                           // @105-106 — hard abort (NOT continue)
        if (((PlacedFeature)cfg.treeFeature().value())
                .place(level, chunkGen, random, mutable)):         // @107-124 — tree draws happen here
            placeDirt(origin, origin.getY() + i, level, cfg, random) // @130-145
            return true                                            // @146-147
        // tree returned false → fall through, continue scanning up
return false                                                       // @154-155
```

Key semantics:
- Heightmap: `LevelReader.getHeight(Types,BlockPos)` default =
  `getHeight(type, pos.getX(), pos.getZ())` (LevelReader.getHeight@0-15);
  WorldGenRegion returns first-available (top blocking y + 1) — exactly
  `hc_feat_height(rg, HC_HM_WORLD_SURFACE, x, z)` (live FINAL map,
  predicate !isAir). Abort when that height < candidate y.
- A candidate failing predicate/spaceForTree just continues; **lava/non-solid
  below and heightmap are hard aborts**; a failing nested tree `place()`
  continues the upward scan (its draws are consumed and must be replayed).
- At success iteration i, the tree origin is `origin.y + i + 1`.

### 2.3 `spaceForTree` (spaceForTree@0-159) — 0 draws

```
m = pos.mutable()
for (k = 1; k <= requiredVerticalSpaceForTree /*3*/; k++):   // @5-14, @50-53
    m.move(UP)                                               // @17-21  (reads pos+1 .. pos+3)
    if (!isAllowedTreeSpace(getBlockState(m), k, allowedVerticalWaterForTree)) return false  // @34-45
// levelTestDistance = 0 → @56 ifle 158 skips the 4-direction level test entirely
return true
```

`isAllowedTreeSpace(state, k, waterAllowed)` (isAllowedTreeSpace@0-36):
`state.isAir()` → true; else `(k+1 <= waterAllowed) && state.getFluidState()
.is(FluidTags.WATER)`. With waterAllowed=2: water passes only at k=1 (the
first block above the tree pos).

### 2.4 `placeDirt` (placeDirt@0-58) + `placeRootedDirt` (placeRootedDirt@0-129)

```
placeDirt(pos=origin, bound=origin.y+i):
    x = pos.getX(); z = pos.getZ(); m = pos.mutable()
    for (y = pos.getY(); y < bound; y++)                       // @24-27 — i column levels; i==0 → none
        placeRootedDirt(level, cfg, random, x, z, m.set(x,y,z))  // @30-49
```
Note the column top is `origin.y+i-1` = (tree origin y) − 2; the cell
directly below the tree is instead written by the tree's own
placeBelowTrunkBlock.

```
placeRootedDirt(level, cfg, random, x, z, m):    // m starts at (x,y,z)
    r = rootRadius /*3*/                                        // @0-4
    pred(state) = state.is(rootReplaceable)                     // lambda$placeRootedDirt$0@0-8 (HolderSet #azalea_root_replaceable)
    for (a = 0; a < rootPlacementAttempts /*20*/; a++):         // @17-23
        m.setWithOffset(m /*current value*/,
            nextInt(r) - nextInt(r),   // draw 1, draw 2  @31-46
            0,
            nextInt(r) - nextInt(r))   // draw 3, draw 4  @49-64
        if (pred(getBlockState(m)))                             // @69-84
            setBlock(m, rootStateProvider.getState(...)         // simple rooted_dirt, 0 draws
                     , flags=2)                                 // @87-107
        m.setX(x); m.setZ(z)                                    // @108-122 (y untouched — dy was 0)
```
Exactly **4 draws per attempt, 80 per column level, unconditional**; the
write is conditional, the draws are not. Offsets are always relative to the
column cell (x,y,z) because x/z are restored and dy=0.

### 2.5 `placeRoots` (hanging roots) (placeRoots@0-158)

```
hr = hangingRootRadius /*3*/; span = hangingRootsVerticalSpan /*2*/  // @0-10
for (a = 0; a < hangingRootPlacementAttempts /*20*/; a++):           // @15-21
    m.setWithOffset(origin,                       // base = ORIGIN (aload_3), not previous m
        nextInt(hr)  - nextInt(hr),   // draws 1,2   @28-43
        nextInt(span)- nextInt(span), // draws 3,4   @44-60
        nextInt(hr)  - nextInt(hr))   // draws 5,6   @61-77
    if (level.isEmptyBlock(m)):                                     // @82-90  (= getBlockState(m).isAir())
        st = hangingRootStateProvider.getState(...)                 // simple hanging_roots[wl=false], 0 draws  @93-104
        if (st.canSurvive(level, m)                                 // @106-114
            && getBlockState(m.above()).isFaceSturdy(level, m, DOWN)) // @117-137
            setBlock(m, st, flags=2)                                // @140-151
```
**6 draws per attempt (120 total), unconditional.**
`HangingRootsBlock.canSurvive@0-27` = `getBlockState(pos.above())
.isFaceSturdy(level, pos.above(), DOWN)` — i.e. the explicit isFaceSturdy
check at @117-137 is the same test evaluated twice (only the ignored pos arg
differs). In C: one `hc_featx_face_sturdy_full(above_state, DOWN)` test.
Since placement requires isAir at m, waterlogged stays false.

Order recap: **placeDirtAndTree (tree draws + dirt draws) fully precedes
placeRoots**, and placeRoots runs only if a tree was placed.

---

## 3. Nested PlacedFeature invocation

`PlacedFeature.place@0-19` → `placeWithContext` with
`PlacementContext(level, generator, Optional.empty())` (no biome check).
placement list `[]` → the position stream is just `[pos]`
(placeWithContext@0-60) → `ConfiguredFeature.place(level, generator, random,
pos)` per position (lambda$placeWithContext$1@0-15), OR-ing results into a
MutableBoolean (@101-106 returns isTrue). `ConfiguredFeature.place@0-16` →
`Feature.place(config, level, generator, random, pos)` → TreeFeature shell.
**Same RandomSource instance throughout; tree origin = the mutable scan
position at that iteration.**

C mapping: this is exactly `hc_featx_run_nested(e, cfg->tree_pf, x, y, z)`
(features.c:1120) on a nested `hc_pfeat_t*` compiled by `compile_placed_ref`
(features_compile.c:756 — already accepts the inline
`{"feature": "<name>", "placement": []}` shape used here); return value =
`placed_any` = the tree body result.

---

## 4. TreeFeature shell — azalea goes through the EXISTING code

`hc_featx_tree_place` (core/src/features_tree.c:1055) already implements the
shell verbatim; re-verified against 26.2 `TreeFeature.doPlace@0-265`:

1. `height = trunkPlacer.getTreeHeight(random)` (doPlace@0-9;
   TrunkPlacer.getTreeHeight@0-30 = `baseHeight + nextInt(heightRandA+1) +
   nextInt(heightRandB+1)`) → azalea: `4 + nextInt(3) + nextInt(1)` —
   **2 draws** (nextInt(1) consumes), height ∈ [4,6].
2. `foliageHeight = foliagePlacer.foliageHeight(random, height, cfg)`
   (doPlace@11-24) — RandomSpreadFoliagePlacer.foliageHeight@0-10 =
   `this.foliageHeight.sample` = constant 2 → **0 draws**.
3. `radius = foliagePlacer.foliageRadius(random, height-foliageHeight)`
   (doPlace@26-44) — FoliagePlacer.foliageRadius@0-10 = `radius.sample` =
   constant 3 → **0 draws**.
4. rootPos = origin (no root_placer) (doPlace@46-68); y-bounds check
   (doPlace@70-130): `origin.y >= minY+1 && origin.y + height + 1 <= maxY+1`.
5. `minClippedHeight` empty (two_layers default) (doPlace@131-139);
   `maxFree = getMaxFreeTreeHeight(...)` (doPlace@141-152; `isFree` = air ∪
   #replaceable_by_trees ∪ #logs, plus vine veto since ignore_vines=false);
   clip test doPlace@154-180 (`ts_min_clipped = -1`).
6. `attachments = trunkPlacer.placeTrunk(level, trunkSetter, random,
   maxFree, rootPos, cfg)` (doPlace@220-238) — §5.
7. `attachments.forEach(a -> foliagePlacer.createFoliage(level,
   foliageSetter, random, cfg, maxFree, a, foliageHeight, radius))`
   (doPlace@240-259, lambda$doPlace$1@0-19) — ArrayList order. The public
   `FoliagePlacer.createFoliage@0-22` samples `offset.sample(random)` first
   (constant 0 → **0 draws**) then calls the protected overload — §6.
8. `place()` finalize unchanged: decorators=[] → `run_decorators` no-op (0
   draws); `updateLeaves` distance BFS; `updateShapeAtEdge` boundary pass.
   Return true iff logs∪leaves nonempty.

**Only the two placers + config plumbing are new. No decorator draws, no
root-placer draws, no ignore_vines special-casing beyond the existing
getMaxFreeTreeHeight vine check.**

---

## 5. BendingTrunkPlacer.placeTrunk (placeTrunk@0-227)

Args: (level, setter, random, freeTreeHeight /*=maxFree*/, pos, cfg).

```
1. dir = Direction.Plane.HORIZONTAL.getRandomDirection(random)     // @0-7 — 1 DRAW
   = faces[nextInt(4)] (Util.getRandom(T[],rand)@0-10 =
     arr[random.nextInt(arr.length)]);
   faces = [NORTH, EAST, SOUTH, WEST] (Direction$Plane.static{}@8-34)
   → index maps 1:1 onto features_tree.c HORIZ[4] = N(0,-1) E(1,0) S(0,1) W(-1,0).
2. h = freeTreeHeight - 1                                          // @9-13
3. m = pos.mutable(); below = m.below()  /* = pos.below(), computed before any move */  // @15-27
   placeBelowTrunkBlock(level, setter, random, below, cfg)         // @29-36
   — TrunkPlacer.placeBelowTrunkBlock@0-27: st =
     belowTrunkProvider.getOptionalState(...) (BlockStateProvider
     .getOptionalState@0-7 = getState, never null for simple) →
     setter.accept(below, rooted_dirt). UNCONDITIONAL write into the trunk
     (logs) set — no validTreePos, no isDirt/rule check. 0 draws.
4. list = []                                                       // @39-42
5. for (i = 0; i <= h; i++):                                       // @44-51, @140-143 — freeTreeHeight iterations
     if (i + 1 >= h + random.nextInt(2)) m.move(dir)               // @54-78 — nextInt(2) DRAWN EVERY ITERATION
     if (TreeFeature.validTreePos(level, m))                       // @79-85
         placeLog(level, setter, random, m, cfg)                   // @88-99 (see below; 0 draws)
     if (i >= minHeightForLeaves /*3*/)                            // @100-106
         list.add(FoliageAttachment(m.immutable(), 0, false))      // @109-130 — radiusOffset 0, doubleTrunk false
     m.move(UP)                                                    // @131-139
6. bendCount = bendLength.sample(random)                           // @146-156 — uniform[1,2] = 1 + nextInt(2): 1 DRAW
7. for (j = 0; j <= bendCount; j++):                               // @158-165, @219-222 — bendCount+1 iterations
     if (TreeFeature.validTreePos(level, m)) placeLog(...)         // @168-188
     list.add(FoliageAttachment(m.immutable(), 0, false))          // @189-210 — UNCONDITIONAL (even if log skipped)
     m.move(dir)                                                   // @211-218
8. return list                                                     // @225-227
```

Geometry: the vertical column starts AT `pos` (i=0 places at pos, then moves
up); the horizontal drift condition `i+1 >= h + nextInt(2)` can shift the
column mid-loop (each shift is permanent — mutable state). After the loop the
cursor sits one above the last column cell; the bend then extends
horizontally in `dir`, adding an attachment at every bend cell.

`placeLog` (TrunkPlacer.placeLog@0-14 → 6-arg placeLog@0-43): re-checks
`validTreePos` (harmless double-check, 0 draws) then
`setter.accept(pos, trunkProvider.getState(level,random,pos))` with
`Function.identity()` — simple `oak_log[axis=y]`, **0 draws, no axis
rotation** for this placer. `validTreePos` = air ∪ #replaceable_by_trees
(TreeFeature.lambda$validTreePos$0@0-22) — existing `valid_tree_pos`.

Draw ledger for placeTrunk: `1 (dir) + freeTreeHeight × 1 (nextInt(2)) +
1 (bend sample)`.

Attachment count: vertical loop contributes `max(0, h − 3 + 1)` =
freeTreeHeight − 3 attachments (i ∈ [3, h]); bend loop contributes
bendCount + 1. All with radiusOffset=0, doubleTrunk=false. List order =
bottom-up column cells, then bend cells outward.

---

## 6. RandomSpreadFoliagePlacer.createFoliage + tryPlaceLeaf

### 6.1 protected createFoliage (createFoliage@0-102)

Slots: var5=maxFreeTreeHeight, var6=attachment, var7=foliageHeight(2),
var8=radius(3), var9=offset(0, **unused by this placer**).

```
base = attachment.pos()                                            // @0-5
for (a = 0; a < leafPlacementAttempts /*50*/; a++):                // @14-23, @96-99
    m.setWithOffset(base,
        nextInt(radius)        - nextInt(radius),        // draws 1,2  @30-46  (x, bound 3 → dx ∈ [-2,2])
        nextInt(foliageHeight) - nextInt(foliageHeight), // draws 3,4  @47-63  (y, bound 2 → dy ∈ [-1,1])
        nextInt(radius)        - nextInt(radius))        // draws 5,6  @64-80  (z)
    tryPlaceLeaf(level, foliageSetter, random, cfg, m)             // @85-95
```

**6 draws per attempt, 300 per attachment, unconditional** — offsets are from
the fixed attachment pos each attempt (not cumulative). NOTE: this is
`nextInt(r) − nextInt(r)` (triangular), NOT `nextInt(2r+1) − r`.
`shouldSkipLocation` is never invoked (and returns false anyway,
RandomSpreadFoliagePlacer.shouldSkipLocation@0-1). radiusOffset/doubleTrunk
are ignored.

### 6.2 FoliagePlacer.tryPlaceLeaf (tryPlaceLeaf@0-95)

```
1. if (isStateAtPosition(pos, st -> st.getValueOrElse(PERSISTENT,false)))    // @0-17 — persistent leaf occupies → false
       return false          // our palette: all leaves persistent=false → gate never fires
2. if (!TreeFeature.validTreePos(level, pos)) return false                   // @20-30
3. st = cfg.foliageProvider.getState(level, random, pos)                     // @31-42 — WEIGHTED: 1 DRAW (§6.3), CONDITIONAL on gates 1-2
4. if (st.hasProperty(WATERLOGGED))                                          // @44-52 — leaves: yes
       st = st.setValue(WATERLOGGED, isFluidAtPosition(pos,
                fs -> fs.isSourceOfType(WATER)))                             // @55-82 — 0 draws (lambda$tryPlaceLeaf$1@0-7)
5. foliageSetter.set(pos, st); return true                                   // @84-94
```

**The weighted foliage draw happens ONLY when the leaf is actually placed**
(after the persistent + validTreePos gates). In C: move the provider sample
into `try_place_leaf` after the `valid_tree_pos` check, then apply the wl
`+7` variant exactly as today.

### 6.3 WeightedStateProvider / WeightedList draw semantics

`WeightedStateProvider.getState@0-11` → `WeightedList.getRandomOrThrow@17-38`:
`roll = random.nextInt(totalWeight)` (**1 draw**, totalWeight=4) →
`selector.get(roll)`. totalWeight 4 < FLAT_THRESHOLD 64 → `Flat` selector
(WeightedList.<init>@40-65): an array of totalWeight slots filled per entry
in **list order** (Flat.<init>@12-73, Arrays.fill), `get(i)=entries[i]`
(Flat.get@0-6). Equivalent to the Compact cumulative walk (Compact.get@8-44).
→ roll 0..2 = `azalea_leaves[d7,p=false,wl=false]`, roll 3 =
`flowering_azalea_leaves[d7,p=false,wl=false]`.
`hc_featx_sprov_sample` HC_SP_WEIGHTED (features.c:272) already implements
exactly this (nextInt(total), subtract weights in list order).

---

## 7. C integration map

### 7.1 New feature body: `HC_CF_ROOT_SYSTEM`

- `hc_features.h`: new enum member + config struct:
  ```c
  typedef struct {
      hc_pfeat_t *tree;            /* compile_placed_ref(cfg."feature") — inline placed, placement [] */
      hc_bpred_t  allowed_tree_position;
      int32_t     required_vertical_space;   /* 3 */
      int32_t     allowed_vertical_water;    /* 2 */
      /* level_test_distance / max_level_deviation are 0 in data — compile
         asserts 0 (the @56-158 branch is dead) or implements the 4-dir test */
      int32_t     root_radius, root_attempts, root_column_max_height;
      uint64_t    root_replaceable[(HC_B_COUNT + 63) / 64];  /* tag_expand("#minecraft:azalea_root_replaceable") */
      hc_sprov_t  root_state;      /* simple rooted_dirt */
      int32_t     hanging_radius, hanging_span, hanging_attempts;
      hc_sprov_t  hanging_state;   /* simple hanging_roots[wl=false] */
  } hc_rootsys_cfg_t;
  ```
- `features_compile.c` `cf_compile`: handle `minecraft:root_system`. The
  `root_replaceable` JSON value is the string `"#minecraft:azalea_root_replaceable"`
  → `tag_expand` directly.
- Body (features_tree.c or features.c): dispatch from the `cf_place` switch
  (features.c ~line 688, alongside HC_CF_TREE at features.c:804). Tree call =
  `hc_featx_run_nested` (§3). Heightmap check =
  `hc_feat_height(e->rg, HC_HM_WORLD_SURFACE, x, z)` (live FINAL, features.c:62
  predicate !isAir). Helpers: `hc_block_is_solid` (isSolid), lava =
  `s == HC_B_LAVA` (palette has only source lava — palette-union.txt:105),
  `hc_block_is_air` for isEmptyBlock, `hc_featx_face_sturdy_full(above, 0 /*DOWN*/)`
  for the hanging-root support test. All writes flag 2 = `hc_feat_set_block`.

### 7.2 Tag data gap (blocking)

`reference/tags/block/azalea_root_replaceable.json` **does not exist** —
tag_expand will fail. Add it verbatim from the jar
(`data/minecraft/tags/block/azalea_root_replaceable.json`):
```json
{ "values": [ "#minecraft:base_stone_overworld", "#minecraft:substrate_overworld",
  "#minecraft:terracotta", "minecraft:red_sand", "minecraft:clay", "minecraft:gravel",
  "minecraft:sand", "minecraft:snow_block", "minecraft:powder_snow" ] }
```
All referenced sub-tags already exist in reference/tags/block/
(base_stone_overworld, substrate_overworld, terracotta, sand).
`azalea_grows_on.json` and `supports_azalea.json` are already present.

### 7.3 Tree machinery deltas (features_tree.c / hc_features.h / features_compile.c)

1. **Trunk kind** `HC_TRUNK_BENDING`: cfg fields `min_height_for_leaves`
   (int, codec default 1) and `bend_length` (`hc_iprov_t`). Implementation is
   §5 — reuses `place_below_trunk` semantics BUT for a **simple** provider the
   write is unconditional (below_not_mask ≡ accept-all; see 4 below),
   `place_log`, `valid_tree_pos`, `attach_t`, and the existing `HORIZ` table
   (`d = hc_wgr_next_int(rng, 4)` indexes it directly).
2. **Foliage kind** `HC_FOL_RANDOM_SPREAD`: cfg field
   `leaf_placement_attempts` (int). `create_foliage` gets a branch that does
   NOT use `place_leaves_row`: 50 × 6 draws + `try_place_leaf` per §6.1. The
   `fol_offset` sample at create_foliage entry stays (constant 0 → 0 draws);
   the sampled value is unused by this kind.
3. **Weighted foliage provider**: `hc_tree_cfg_t.foliage_state` (uint16) must
   become / be augmented by an `hc_sprov_t foliage` (compile currently FAILs
   on non-simple, features_compile.c:1219). Sample it inside
   `try_place_leaf` after `valid_tree_pos` (conditional draw, §6.2), then
   apply the existing wl `+7` adjustment. Compile-time leaf-range validation
   (features_compile.c:1221) must accept each weighted entry and the azalea
   ranges.
4. **below_trunk simple**: compile currently requires the rule_based shape
   (features_compile.c:1224-1259). Accept `simple_state_provider` too —
   semantics: `getOptionalState = getState` (never null,
   BlockStateProvider.getOptionalState@0-7) → **unconditional** t_set into
   logs (below_not_mask all-zero gives exactly this with the existing
   `place_below_trunk`).
5. **random_spread placer JSON fields**: `foliage_height` +
   `leaf_placement_attempts` (there is NO `height` field for this type —
   the current compile's unconditional `height` requirement at
   features_compile.c:1202-1204 must be per-kind).
6. **updateLeaves must learn azalea leaves**: `optional_distance_at`
   (features_tree.c:665), `leaf_with_distance` (features_tree.c:674), and the
   non-leaf die-guard in `update_leaves` (features_tree.c:735) currently only
   recognize `[HC_B_OAK_LEAVES_BASE, +28)`. Azalea leaves live at
   `[HC_B_AZALEA_LEAVES_BASE, +28)` (azalea 14 = 7 distance × wl, then
   flowering +14; hc_blocks.h:131-132, blocks.c:365-371) — extend all three
   to a two-range "leaf family" check with per-family base:
   `distance = (s - fam) % 7 + 1`, rewrite keeps `((s - fam) / 7) * 7`
   (species+wl). `hc_block_is_leaves` / `#leaves` tag already include azalea
   (reference/tags/block/leaves.json:10-11).
7. Blocks already in palette: `HC_B_ROOTED_DIRT`, `HC_B_HANGING_ROOTS`(+WL),
   azalea/flowering leaves, oak_log (hc_blocks.h:119-132).

### 7.4 Draw ledger (one rooted_azalea_tree placement, per pipeline position)

| phase | draws |
|---|---|
| pipeline | count uniform[1,2]: 1; in_square: 2; height_range uniform: 1; env_scan/random_offset/biome: 0 |
| place() origin air check | 0 (return false if not air) |
| per column candidate that fails predicate/spaceForTree | 0 |
| per failed nested tree attempt | 2 (getTreeHeight) + 0 (foliageHeight, radius, offset const) — aborts at y-bounds/maxFree; if trunk ran: + trunk/foliage draws below |
| successful tree: trunk | 1 (dir nextInt(4)) + freeTreeHeight × nextInt(2) + 1 (bend uniform[1,2]) |
| successful tree: foliage | per attachment: 50 × 6 = 300 nextInt draws + 1 × nextInt(4) per actually-placed leaf |
| decorators / updateLeaves / updateShapeAtEdge | 0 |
| placeDirt | i column levels × 20 × 4 = 80·i nextInt(3) draws |
| placeRoots | 20 × 6 = 120 draws (nextInt(3)×2, nextInt(2)×2, nextInt(3)×2 per attempt) |

Order: pipeline → origin air check → upward scan {tree attempt draws} →
placeDirt → placeRoots.

### 7.5 Verification hook

Golden 07 both-bundle gate on a lush_caves-bearing region;
`HC_TREE_DEBUG` / `HC_TREE_DEBUG_CELL` stream-realign workflow from 9b works
unchanged (the azalea tree flows through the same shell). Failed-tree-attempt
draw replay (§2.2) is the most likely desync point — a probe comparing the
number of column iterations before success against vanilla is cheap
insurance.
