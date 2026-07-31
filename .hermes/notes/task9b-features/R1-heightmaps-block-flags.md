# R1 — Heightmap machinery + per-blockstate flags for the features palette (MC 26.2)

Source of truth: `javap -p -c -constants` (plus `-v` for lambda bootstrap targets) against
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server`, disassembled in this session.
Datapack facts from `tools/golden/work/server/data/minecraft/tags/block/*.json`.
Labels: [VERIFIED-bytecode], [VERIFIED-data], [DERIVED] (arithmetic over verified constants),
[UNVERIFIED].

Companions: task9a A2 (pipeline), A4 (WG-region window / frozen WG maps), A7 (handoff).

---

## 1. Heightmap machinery

### 1.1 `Heightmap$Types` — enum order, ids, usage, isOpaque predicates [VERIFIED-bytecode]

Enum/ordinal order (also the `$VALUES`/`values()` order and the EnumSet iteration order):

| ord | name | id | Usage | isOpaque predicate |
|---|---|---|---|---|
| 0 | WORLD_SURFACE_WG | 0 | WORLDGEN | `NOT_AIR` = `!state.isAir()` |
| 1 | WORLD_SURFACE | 1 | CLIENT | `NOT_AIR` |
| 2 | OCEAN_FLOOR_WG | 2 | WORLDGEN | `MATERIAL_MOTION_BLOCKING` = `state.blocksMotion()` (method ref, Heightmap bootstrap #1) |
| 3 | OCEAN_FLOOR | 3 | LIVE_WORLD | `MATERIAL_MOTION_BLOCKING` |
| 4 | MOTION_BLOCKING | 4 | CLIENT | `state.blocksMotion() \|\| !state.getFluidState().isEmpty()` (Types.lambda$static$0) |
| 5 | MOTION_BLOCKING_NO_LEAVES | 5 | CLIENT | `(state.blocksMotion() \|\| !state.getFluidState().isEmpty()) && !(state.getBlock() instanceof LeavesBlock)` (Types.lambda$static$1) |

- `NOT_AIR` and `MATERIAL_MOTION_BLOCKING` are static fields on `Heightmap` (its `<clinit>`);
  `NOT_AIR` is `Heightmap.lambda$static$0` (`!isAir()`), `MATERIAL_MOTION_BLOCKING` is a method
  reference to `BlockStateBase.blocksMotion()` (checked via `javap -v` BootstrapMethods).
- The NO_LEAVES exclusion is a Java `instanceof LeavesBlock` on `state.getBlock()` — NOT a tag.
  `TintedParticleLeavesBlock extends LeavesBlock`, so oak/jungle leaves are excluded (§3).
- `keepAfterWorldgen()` = `usage != WORLDGEN`; `sendToClient()` = `usage == CLIENT`.

### 1.2 `Heightmap` object — storage encoding [VERIFIED-bytecode]

```java
Heightmap(ChunkAccess chunk, Types type) {
    this.isOpaque = type.isOpaque();
    this.chunk = chunk;
    int bits = Mth.ceillog2(chunk.getHeight() + 1);      // overworld: ceillog2(385) = 9
    this.data = new SimpleBitStorage(bits, 256);         // 256 columns
}
private static int getIndex(int x, int z) { return x + z * 16; }        // x + z*16!
private int  getFirstAvailable(int idx)   { return data.get(idx) + chunk.getMinY(); }
public  int  getFirstAvailable(int x,int z){ return getFirstAvailable(getIndex(x, z)); }
public  int  getHighestTaken(int x, int z){ return getFirstAvailable(getIndex(x, z)) - 1; }
private void setHeight(int x, int z, int v){ data.set(getIndex(x, z), v - chunk.getMinY()); }
```
- Stored raw value = `y_firstFree − minY` (i.e. **highest counted block + 1 − minY**); raw 0 =
  empty column; `getFirstAvailable` = y of the first non-counted block above the top counted
  block (matches 9a §5 convention: `ChunkAccess.getHeight(type,x,z)` = `getFirstAvailable − 1`,
  and `WorldGenRegion.getHeight` re-adds 1).
- Index is `x + 16*z` (x minor, z major) — matters for raw-dump comparisons.

### 1.3 `Heightmap.primeHeightmaps(chunk, Set<Types>)` — exact algorithm [VERIFIED-bytecode]

```java
public static void primeHeightmaps(ChunkAccess chunk, Set<Types> types) {
    if (types.isEmpty()) return;
    int n = types.size();
    ObjectArrayList<Heightmap> list = new ObjectArrayList<>(n);
    ObjectListIterator<Heightmap> it = list.iterator();          // ONE iterator, created on empty list
    int topY = chunk.getHighestSectionPosition() + 16;           // see below
    BlockPos.MutableBlockPos m = new BlockPos.MutableBlockPos();
    for (int x = 0; x < 16; ++x) {                               // x outer
        for (int z = 0; z < 16; ++z) {                           // z inner
            for (Types t : types)                                // Set iteration order (EnumSet = ordinal)
                list.add(chunk.getOrCreateHeightmapUnprimed(t)); // appends; NO clear between columns
            for (int y = topY - 1; y >= chunk.getMinY(); --y) {  // top-down
                m.set(x, y, z);
                BlockState state = chunk.getBlockState(m);
                if (state.is(Blocks.AIR)) continue;              // block IDENTITY test (cave_air NOT skipped)
                while (it.hasNext()) {
                    Heightmap hm = it.next();
                    if (hm.isOpaque.test(state)) { hm.setHeight(x, z, y + 1); it.remove(); }
                }
                if (list.isEmpty()) break;                       // column satisfied for all types
                it.back(n);                                      // rewind iterator by (at most) n
            }
        }
    }
}
```
- Scan start: `topY − 1` where `topY = getHighestSectionPosition() + 16`.
  `ChunkAccess.getHighestSectionPosition()` [VERIFIED-bytecode] =
  `getHighestFilledSectionIndex()` (scan `sections[]` from top for first `!hasOnlyAir()`),
  `== -1 ? getMinY() : SectionPos.sectionToBlockCoord(getSectionYFromSectionIndex(idx))`
  (i.e. bottom block-y of the highest non-empty section). So the y loop starts at the TOP of the
  first non-empty section + 15 (one section of headroom above the highest filled section).
  All-empty chunk: topY = minY + 16.
- Per column the satisfied heightmaps are REMOVED from the list; the loop breaks early when all
  n are set. Next column re-adds all n (fresh `getOrCreateHeightmapUnprimed` — same instances,
  `computeIfAbsent`).
- **Carry-over quirk** (faithful-port hazard): the list is never cleared. If a column bottoms out
  (`y < minY`) with some heightmaps unsatisfied (their stored value stays raw 0 = minY), those
  entries STAY in the list; the next column appends n more (duplicates of the same Heightmap
  objects). `it.back(n)` then rewinds at most n slots, so leftover entries beyond the last n can
  be skipped or double-processed with the CURRENT column's (x,z). For post-noise overworld chunks
  every column has bedrock (NOT_AIR and blocksMotion both fire), so all 4 types satisfy per
  column and the quirk is unreachable — but a byte-exact C port should either replicate the exact
  list/iterator semantics or assert the always-satisfied precondition. [DERIVED from the verified
  bytecode control flow]
- `state.is(Blocks.AIR)` is `TypedInstance.is(T)` = `typeHolder().value() == block` — reference
  equality against the `air` block only [VERIFIED-bytecode]. cave_air/void_air fall through to the
  predicate tests (where `isAir()` is true ⇒ NOT_AIR false, `blocksMotion()` false ⇒ no set).

### 1.4 `Heightmap.update(x, y, z, state)` [VERIFIED-bytecode]

Called with x,z chunk-local (0..15) and y GLOBAL (see §1.7 caller).

```java
public boolean update(int x, int y, int z, BlockState state) {
    int first = getFirstAvailable(x, z);                    // y of first free block
    if (y <= first - 2) return false;                       // strictly below the surface: no-op
    if (this.isOpaque.test(state)) {
        if (y >= first) { setHeight(x, z, y + 1); return true; }   // raises the map
        return false;                                       // y == first-1 and opaque: unchanged
    }
    if (first - 1 == y) {                                   // top counted block became non-opaque: rescan down
        BlockPos.MutableBlockPos m = new BlockPos.MutableBlockPos();
        for (int yy = y - 1; yy >= chunk.getMinY(); --yy) {
            m.set(x, yy, z);
            if (this.isOpaque.test(chunk.getBlockState(m))) {
                setHeight(x, z, yy + 1); return true;       // first opaque below wins
            }
        }
        setHeight(x, z, chunk.getMinY()); return true;      // stores raw 0
    }
    return false;
}
```
- Note the exact predicate re-evaluation against the chunk's CURRENT blocks during the downward
  rescan — feature writes lower in the same column already made are visible.
- The `y <= first−2` early-out means placing an opaque block strictly under the surface never
  touches the map, and REMOVING (making non-opaque) a block strictly under the surface never
  touches it either.

### 1.5 `ChunkStatusTasks.generateFeatures` — priming before decoration [VERIFIED-bytecode]

```java
ServerLevel level = ctx.level();
Heightmap.primeHeightmaps(chunk, EnumSet.of(Types.MOTION_BLOCKING, Types.MOTION_BLOCKING_NO_LEAVES,
                                            Types.OCEAN_FLOOR,     Types.WORLD_SURFACE));
WorldGenRegion region = new WorldGenRegion(level, cache, step, chunk);   // AFTER priming
if (!SharedConstants.DEBUG_FEATURES) // (field DEBUG_DISABLE_FEATURES, false in prod)
    ctx.generator().applyBiomeDecoration(region, chunk, level.structureManager().forWorldGenRegion(region));
Blender.generateBorderTicks(region, chunk);
return CompletableFuture.completedFuture(chunk);
```
- The `EnumSet.of(...)` argument order is MB, MB_NO_LEAVES, OF, WS, but EnumSet ITERATES in
  ordinal order: **WORLD_SURFACE(1), OCEAN_FLOOR(3), MOTION_BLOCKING(4),
  MOTION_BLOCKING_NO_LEAVES(5)** — this is the per-column `types` iteration order inside
  primeHeightmaps (only affects internal list order; per-type results are independent).
  [VERIFIED-bytecode for the call; EnumSet ordinal iteration is JDK semantics, labeled as such]
- Priming happens ONCE per chunk at the start of its features task, from the post-carvers
  blocks, BEFORE any decoration write of that chunk. Neighbor chunks in the 3×3 already at
  FEATURES-or-later primed theirs during their own task.

### 1.6 `ChunkAccess.getHeight(type, x, z)` — lazy single-type prime on read [VERIFIED-bytecode]

```java
public int getHeight(Types type, int x, int z) {
    Heightmap hm = heightmaps.get(type);
    if (hm == null) {
        if (IS_RUNNING_IN_IDE && this instanceof LevelChunk) LOGGER.error("Unprimed heightmap: {} {} {}", ...);
        Heightmap.primeHeightmaps(this, EnumSet.of(type));      // lazy full-chunk prime of THAT type
        hm = heightmaps.get(type);
    }
    return hm.getFirstAvailable(x & 15, z & 15) - 1;
}
```
- During decoration the 4 FINAL maps exist for the center chunk (§1.5). For a NEIGHBOR chunk in
  the 3×3 that has not decorated yet (status < features), a FINAL-map read (step-9 `heightmap`
  placement / surface_water_depth_filter reaching into a neighbor column) triggers this lazy
  prime on that neighbor's current (pre-features) blocks — and the map then lives on and is
  updated by that neighbor's later writes. Order-sensitive; the C region must reproduce
  "prime-on-first-touch from current blocks".

### 1.7 Who updates the maps during features — `ProtoChunk.setBlockState` [VERIFIED-bytecode]

`ProtoChunk.setBlockState(pos, state, flags...)` tail (after the section write; light block only
when `status.isOrAfter(INITIALIZE_LIGHT)` — dead during features):

```java
EnumSet<Types> after = getPersistedStatus().heightmapsAfter();
EnumSet<Types> toPrime = null;
for (Types t : after)                       // EnumSet: ordinal order
    if (heightmaps.get(t) == null) { if (toPrime == null) toPrime = EnumSet.noneOf(Types.class); toPrime.add(t); }
if (toPrime != null) Heightmap.primeHeightmaps(this, toPrime);
for (Types t : after)
    heightmaps.get(t).update(x & 15, y, z & 15, state);   // x,z sectionRelative; y GLOBAL
return old;
```
Early-outs before any of this: `isOutsideBuildHeight(y)` ⇒ return `VOID_AIR` default state
(no write, no map update); `section.hasOnlyAir() && state.is(Blocks.AIR)` ⇒ return state.

`ChunkStatus.heightmapsAfter` per status [VERIFIED-bytecode, ChunkStatus `<clinit>`]:
- `WORLDGEN_HEIGHTMAPS = EnumSet.of(OCEAN_FLOOR_WG, WORLD_SURFACE_WG)` for: empty,
  structure_starts, structure_references, biomes, noise, surface.
- `FINAL_HEIGHTMAPS = EnumSet.of(OCEAN_FLOOR, WORLD_SURFACE, MOTION_BLOCKING,
  MOTION_BLOCKING_NO_LEAVES)` for: **carvers**, features, initialize_light, light, spawn, full.

`ChunkStep.apply` calls `setPersistedStatus(targetStatus)` only in `completeChunkGeneration`
AFTER the task body finishes [VERIFIED-bytecode]. So DURING the features task
`getPersistedStatus() == CARVERS`, whose set is already FINAL_HEIGHTMAPS ⇒ **every
ProtoChunk.setBlockState during decoration updates exactly the 4 FINAL maps** (never the WG
maps — consistent with A4's "WG maps frozen during features"). Conversely, during the carvers
task persisted = SURFACE ⇒ carver writes update the 2 WG maps.
- The `toPrime` branch inside setBlockState is normally dead during features for the center
  chunk (maps exist from §1.5) but is live for cross-chunk writes into a not-yet-decorated
  neighbor: the neighbor's 4 FINAL maps get primed at the moment of the FIRST spill write into
  it, from its pre-features blocks, then updated incrementally by that and subsequent writes.
  This is the mechanism behind the 9a "moss spill" drift.
- setBlock flags do NOT gate heightmap updates in ProtoChunk (its setBlockState ignores flags
  for maps; flag semantics only matter in LevelChunk). OreFeature's BulkSectionAccess writes
  bypass setBlockState entirely (writes `LevelChunkSection.setBlockState` directly) ⇒ no map
  updates — matches 9a's ore implementation.

---

## 2. BlockStateBase flag machinery (26.2)

### 2.1 Per-state fields (BlockStateBase ctor / initCache) [VERIFIED-bytecode]

From `Properties` at state construction: `isAir`, `liquid`, `ignitedByLava`, `pushReaction`,
`replaceable`, `canOcclude`, `requiresCorrectToolForDrops`, `destroySpeed`, `instrument`,
`spawnTerrainParticles`, `lightEmission` (per-state function), `offsetFunction`, predicates.
`fluidState` initialized to `Fluids.EMPTY.defaultFluidState()` in the ctor and REPLACED in
`initCache()` with `block.getFluidState(state)` (virtual — per-block waterlogged overrides).
`initCache()` also computes: `cache = block.hasDynamicShape() ? null : new Cache(state)`,
`legacySolid = calculateSolid()`, `occlusionShape = canOcclude ? block.getOcclusionShape(state)
: Shapes.empty()`, `solidRender = Block.isShapeFullBlock(occlusionShape)`,
occlusionShapesByFace, propagatesSkylightDown, lightDampening.

### 2.2 `blocksMotion()` / `isSolid()` / `calculateSolid()` [VERIFIED-bytecode]

```java
public boolean blocksMotion() {
    Block b = getBlock();
    return b != Blocks.COBWEB && b != Blocks.BAMBOO_SAPLING && isSolid();
}
public boolean isSolid() { return legacySolid; }         // per-STATE cached field

private boolean calculateSolid() {
    if (block.properties.forceSolidOn)  return true;     // checked FIRST (beats dynamicShape)
    if (block.properties.forceSolidOff) return false;
    if (cache == null) return false;                     // dynamicShape blocks w/o force flags
    VoxelShape cs = cache.collisionShape;                // getCollisionShape(EmptyBlockGetter, ZERO, empty ctx)
    if (cs.isEmpty()) return false;
    AABB bb = cs.bounds();
    if (bb.getSize() >= 0.7291666666666666) return true; // getSize = (xsize+ysize+zsize)/3.0 [VERIFIED]
    return bb.getYsize() >= 1.0;
}
```
- `AABB.getSize()` = `(getXsize()+getYsize()+getZsize())/3.0` (doubles) [VERIFIED-bytecode].
- Note `blocksMotion` special-cases BAMBOO_SAPLING but NOT BAMBOO (the stalk): bamboo stalk
  blocksMotion = true (forceSolidOn).
- `BlockBehaviour.getCollisionShape` default: `hasCollision ? state.getShape(bg,pos) : empty`;
  `getShape` default = `Shapes.block()` (full cube) [VERIFIED-bytecode]. `Properties.noCollision()`
  sets `hasCollision=false` AND `canOcclude=false` [VERIFIED-bytecode].

### 2.3 `canBeReplaced()` [VERIFIED-bytecode]

`state.canBeReplaced()` = the per-state `replaceable` field = `Properties.replaceable()` flag,
nothing else. (The `canBeReplaced(BlockPlaceContext)`/`(Fluid)` overloads add item/fluid logic —
`(Fluid)` = `canBeReplaced() || !isSolid()` — and blocks override the context overload (Vine:
also true if same-block with a vacant face; Multiface: item==this && hasAnyVacantFace) — those
overloads are player-placement paths; worldgen block predicates (`BlockPredicate.replaceable`)
use the no-arg field version.)

### 2.4 `getFluidState()` [VERIFIED-bytecode]

Per-state cached at initCache from `Block.getFluidState(state)`:
- `Block` default: `Fluids.EMPTY.defaultFluidState()` (empty).
- SimpleWaterlogged overrides (LeavesBlock, MultifaceBlock, BigDripleaf(+Stem via same pattern),
  SmallDripleafBlock, ...): `state.getValue(WATERLOGGED) ? Fluids.WATER.getSource(false) :
  super.getFluidState(state)` — waterlogged=true states report the water SOURCE fluid (nonempty,
  counts for MOTION_BLOCKING and for `matching_fluids water`).
- `LiquidBlock`: `stateCache.get(Math.min(state.getValue(LEVEL), 8))` — nonempty for every LEVEL;
  LEVEL=0 default = source.

### 2.5 Face sturdiness / support helpers [VERIFIED-bytecode]

```java
// BlockStateBase
isFaceSturdy(bg, pos, dir)              = isFaceSturdy(bg, pos, dir, SupportType.FULL);
isFaceSturdy(bg, pos, dir, supportType) = cache != null ? cache.isFaceSturdy(dir, supportType)
                                        : supportType.isSupporting(asState(), bg, pos, dir);
// Cache precomputes faceSturdy[dir.ordinal()*3 + support.ordinal()] over
//   EmptyBlockGetter.INSTANCE / BlockPos.ZERO, for all 6 dirs x {FULL, CENTER, RIGID}.
// Cache.collisionShape = block.getCollisionShape(state, EmptyBlockGetter, ZERO, CollisionContext.empty())
// Cache.isCollisionShapeFullBlock = Block.isShapeFullBlock(state.getCollisionShape(EBG, ZERO))
// Cache.largeCollisionShape = any axis with min<0 or max>1.

// SupportType (enum order FULL=0, CENTER=1, RIGID=2):
FULL.isSupporting(s,bg,p,d)   = Block.isFaceFull(s.getBlockSupportShape(bg,p), d);
CENTER.isSupporting(s,bg,p,d) = !Shapes.joinIsNotEmpty(s.getBlockSupportShape(bg,p).getFaceShape(d),
                                   CENTER_SUPPORT_SHAPE, BooleanOp.ONLY_SECOND);
   // CENTER_SUPPORT_SHAPE = Block.column(2.0, 0.0, 10.0)  → 2x2 px column, y 0..10, centered
RIGID.isSupporting(s,bg,p,d)  = !Shapes.joinIsNotEmpty(s.getBlockSupportShape(bg,p).getFaceShape(d),
                                   RIGID_SUPPORT_SHAPE, BooleanOp.ONLY_SECOND);
   // RIGID_SUPPORT_SHAPE = Shapes.join(Shapes.block(), Block.column(12.0, 0.0, 16.0), ONLY_FIRST)
   //                     → full cube minus the centered 12x12 column = the 2-px rim
// Block.column(sizeXZ, minY, maxY) = column(sizeXZ, sizeXZ, minY, maxY) = box centered at 8,8 in XZ.

// Block statics:
canSupportRigidBlock(bg, pos)      = bg.getBlockState(pos).isFaceSturdy(bg, pos, Direction.UP, SupportType.RIGID);
canSupportCenter(level, pos, dir)  = { s = level.getBlockState(pos);
                                       if (dir == DOWN && s.is(BlockTags.UNSTABLE_BOTTOM_CENTER)) return false;
                                       return s.isFaceSturdy(level, pos, dir, SupportType.CENTER); }
isFaceFull(shape, dir)             = isShapeFullBlock(shape.getFaceShape(dir));
isShapeFullBlock(shape)            = SHAPE_FULL_BLOCK_CACHE.getUnchecked(shape);   // == Shapes.block() equality test
```
- `getBlockSupportShape` default = `getCollisionShape(state,bg,pos, CollisionContext.empty())`;
  **LeavesBlock overrides it to `Shapes.empty()`** ⇒ leaves are NEVER sturdy for any support
  type/face even though they block motion. [VERIFIED-bytecode]
- `UNSTABLE_BOTTOM_CENTER` = `#minecraft:fence_gates` [VERIFIED-data] — irrelevant to our palette.
- For any full-cube collision block: FULL/CENTER/RIGID all true on all 6 faces. [DERIVED]

### 2.6 Misc ordering facts [VERIFIED-bytecode]

- `Direction.values()` = DOWN, UP, NORTH, SOUTH, WEST, EAST (ordinals 0..5).
- `BlockStateBase.DIRECTIONS` / `Cache.DIRECTIONS` = `Direction.values()`.
- `state.is(Block)` = reference equality of `getBlock()`; `state.is(TagKey)` via holder tags.

---

## 3. Per-block flags — the features palette

Legend: MB-pred = counts for OCEAN_FLOOR(_WG)/MOTION_BLOCKING (i.e. `blocksMotion()`);
NL-excl = additionally excluded from MOTION_BLOCKING_NO_LEAVES (instanceof LeavesBlock);
fluid = `getFluidState()` nonempty states. Properties chains and block classes
[VERIFIED-bytecode, Blocks.<clinit> + block class ctors]; solid verdicts [DERIVED] from the
verified shapes/flags via §2.2 unless marked otherwise.

| block | class | isAir | replaceable | blocksMotion/isSolid | fluid nonempty | full-cube collision | notes (chain flags) |
|---|---|---|---|---|---|---|---|
| air | AirBlock | **true** | **true** | false (noCollision) | – | no (empty) | `replaceable.noCollision.noLootTable.air` |
| stone | Block | false | false | **true** | – | yes | requiresCorrectToolForDrops |
| dirt | Block | false | false | **true** | – | yes | |
| coarse_dirt | Block | false | false | **true** | – | yes | |
| podzol | SnowyBlock | false | false | **true** | – | yes | SNOWY=false |
| grass_block | GrassBlock (⊂SpreadingSnowyBlock⊂SnowyBlock) | false | false | **true** | – | yes | randomTicks; SNOWY=false |
| clay | Block | false | false | **true** | – | yes | |
| gravel | ColoredFallingBlock | false | false | **true** | – | yes | ColorRGBA(-8356741) |
| water | LiquidBlock(Fluids.WATER) | false | **true** | false | **all 16 states** | no (empty; getShape=empty) | `replaceable.noCollision.liquid`; LEVEL default 0 |
| lava | LiquidBlock(Fluids.LAVA) | false | **true** | false | **all states** | no | + randomTicks, lightLevel 15 |
| oak_log / jungle_log | RotatedPillarBlock | false | false | **true** | – | yes | logProperties: ignitedByLava; AXIS=Y |
| oak_leaves / jungle_leaves | TintedParticleLeavesBlock(0.01f) ⊂ LeavesBlock | false | false | **true** (full cube; noOcclusion does not affect solid) | waterlogged=true states | yes (collision full; support shape EMPTY §2.5) | leavesProperties: strength 0.2, randomTicks, noOcclusion, ignitedByLava, DESTROY; **NL-excl**; DISTANCE=7, PERSISTENT=false, WATERLOGGED=false |
| vine | VineBlock | false | **true** | false (noCollision) | – | no | randomTicks, ignitedByLava, DESTROY; UP=N=E=S=W=false (no DOWN prop) |
| glow_lichen | GlowLichenBlock ⊂ MultifaceSpreadeableBlock ⊂ MultifaceBlock | false | **true** | false (noCollision) | waterlogged=true states | no | lightLevel=f(state) via GlowLichenBlock.emission; default: WATERLOGGED=false, all 6 face bools false (trySetValue loop) |
| cave_vines | CaveVinesBlock ⊂ GrowingPlantHeadBlock | false | false | false (noCollision) | – | no | randomTicks; growthDirection=DOWN, tipShape=SHAPE, scheduleFluidTicks=false, growPerTickProbability=0.1D; AGE=0, BERRIES=false |
| cave_vines_plant | CaveVinesPlantBlock ⊂ GrowingPlantBodyBlock | false | false | false (noCollision) | – | no | BERRIES=false (no AGE) |
| bamboo | BambooStalkBlock | false | false | **true** (**forceSolidOn**, wins over dynamicShape) | – | no (collision = column(3px)+offset) | dynamicShape + offsetType XZ + DESTROY; AGE=0, LEAVES=NONE, STAGE=0; NOT the blocksMotion exception (that is bamboo_sapling) |
| cocoa | CocoaBlock ⊂ HorizontalDirectionalBlock | false | false | false (shapes per AGE/FACING all ≤ col(8px,h9) ⇒ avg<0.72917, ysize<1) | – | no | randomTicks, noOcclusion, DESTROY; FACING=NORTH, AGE=0 |
| short_grass / fern | TallGrassBlock ⊂ VegetationBlock | false | **true** | false (noCollision) | – | no | instabreak, offset XYZ, ignitedByLava, DESTROY |
| tall_grass | DoublePlantBlock ⊂ VegetationBlock | false | **true** | false (noCollision) | – | no | HALF=LOWER default |
| poppy / dandelion | FlowerBlock ⊂ VegetationBlock | false | false | false (noCollision) | – | no | **not replaceable**; offset XZ; poppy=NIGHT_VISION 5.0f, dandelion=SATURATION 0.35f |
| moss_block | BonemealableFeaturePlacerBlock(CaveFeatures.MOSS_PATCH_BONEMEAL) | false | false | **true** | – | yes | pushReaction DESTROY (but full cube ⇒ solid); sturdy all faces |
| moss_carpet | CarpetBlock | false | false | false (shape=column(16,0,1): avg=(1+1/16+1)/3=0.6875<0.72917, ysize 1/16<1) | – | no | DESTROY |
| azalea / flowering_azalea | AzaleaBlock ⊂ VegetationBlock | false | false | false (**forceSolidOff**) | – | no | noOcclusion; shape = or(column(16,8..16), column(4?,0..8)) — irrelevant to solid |
| spore_blossom | SporeBlossomBlock | false | false | false (noCollision) | – | no | instabreak, DESTROY; SHAPE=column(12,13..16) |
| big_dripleaf | BigDripleafBlock ⊂ HorizontalDirectionalBlock | false | false | false (**forceSolidOff**; HAS collision: SHAPE_LEAF per TILT ≈ column(16,11..15)/…/empty for FULL tilt) | waterlogged=true | no | WATERLOGGED=false, FACING=NORTH, TILT=NONE |
| big_dripleaf_stem | BigDripleafStemBlock | false | false | false (noCollision) | waterlogged=true | no | WATERLOGGED=false, FACING=NORTH |
| small_dripleaf | SmallDripleafBlock ⊂ DoublePlantBlock | false | false | false (noCollision) | waterlogged=true | no | instabreak, offset XYZ, DESTROY; HALF=LOWER, WATERLOGGED=false, FACING=NORTH |

Heightmap consequences [DERIVED from table + §1.1]:
- WORLD_SURFACE counts EVERYTHING except isAir (air/cave_air/void_air).
- OCEAN_FLOOR / MOTION_BLOCKING during features move when placing: logs, leaves, moss_block,
  bamboo stalk, dirt, clay, gravel, azalea? NO (forceSolidOff, and no fluid) — azalea,
  flowering_azalea, moss_carpet, cocoa, big_dripleaf(+stem), all small plants, vine, lichen,
  spore_blossom, cave_vines do NOT move OF/MB unless waterlogged (fluid counts for MB only,
  NOT for OCEAN_FLOOR which is blocksMotion-only).
- MOTION_BLOCKING_NO_LEAVES = MOTION_BLOCKING minus `instanceof LeavesBlock` (only the two
  leaves in our palette) — waterlogged LEAVES count for MB but NOT for MB_NO_LEAVES (the
  instanceof test runs after the fluid test).

### 3.1 Water/lava fluid detail

`LiquidBlock.getFluidState` = `stateCache.get(min(LEVEL,8))` [VERIFIED-bytecode]; the cache list
is built from the FlowingFluid's states (source for 0, flowing for 1..8). Every water/lava state
has nonempty fluid ⇒ counts for MOTION_BLOCKING(_NO_LEAVES) but never for OCEAN_FLOOR(_WG).

---

## 4. Support/ground tags [VERIFIED-data]

- `#supports_vegetation` = `#substrate_overworld` + farmland;
  `#substrate_overworld` = `#dirt` + `#mud` + `#moss_blocks` + `#grass_blocks`;
  `#dirt` = {dirt, coarse_dirt, rooted_dirt}; `#mud` = {mud, muddy_mangrove_roots};
  `#moss_blocks` = {moss_block, pale_moss_block}; `#grass_blocks` = {grass_block, podzol, mycelium}.
  ⇒ in-world ground that supports vegetation: dirt, coarse_dirt, rooted_dirt, moss_block,
  grass_block, podzol, mycelium, mud(+roots), farmland. **NOT stone, NOT clay, NOT gravel.**
- `#supports_azalea` = `#supports_vegetation` + clay.
- `#supports_bamboo` = `#sand` + `#substrate_overworld` + bamboo + bamboo_sapling + gravel + suspicious_gravel.
- `#supports_cocoa` = `#jungle_logs` = {jungle_log, jungle_wood, stripped_jungle_log, stripped_jungle_wood}.
- `#supports_big_dripleaf` = `#supports_small_dripleaf` + dirt + grass_block + podzol + coarse_dirt
  + mycelium + rooted_dirt + moss_block + mud + muddy_mangrove_roots + farmland;
  `#supports_small_dripleaf` = {clay, moss_block}.
- `#unstable_bottom_center` = `#fence_gates`.

## 5. canSurvive chains touched by 9b bodies (bytecode-verified summaries)

- `VegetationBlock.canSurvive(s, level, pos)` = `mayPlaceOn(level.getBlockState(pos.below()), level, pos.below())`;
  default `mayPlaceOn` = `below.is(#supports_vegetation)`. Applies to short_grass/fern
  (TallGrassBlock), tall_grass lower half, flowers (FlowerBlock — no mayPlaceOn override).
- `AzaleaBlock.mayPlaceOn` = `below.is(#supports_azalea)` (clay allowed).
- `DoublePlantBlock.canSurvive`: UPPER half ⇒ `below.is(this) && below.HALF == LOWER`; LOWER ⇒
  `VegetationBlock.canSurvive` (i.e. mayPlaceOn below).
- `SmallDripleafBlock.canSurvive`: UPPER ⇒ DoublePlant rule; LOWER ⇒ below-state via overridden
  `mayPlaceOn` = `below.is(#supports_small_dripleaf) || (level.getFluidState(pos.above()).isSourceOfType(WATER) && below.is(#supports_vegetation))`.
- `BigDripleafBlock.canSurvive`: below `is(this) || is(BIG_DRIPLEAF_STEM) || is(#supports_big_dripleaf)`.
- `BambooStalkBlock.canSurvive` = `below.is(#supports_bamboo)`.
- `CocoaBlock.canSurvive` = `state(pos.relative(FACING)).is(#supports_cocoa)` (log BEHIND the
  facing direction — cocoa FACING points AT the log).
- `MultifaceBlock.canSurvive` (glow_lichen): for each of the 6 directions in `Direction.values()`
  order, if `hasFace(state,d)`: require `canAttachTo(level, pos, d)` (any failing face ⇒ false);
  true iff at least one face present and all present faces attachable.
- `VineBlock.canSurvive` = `hasFaces(getUpdatedState(state, level, pos))` (face-pruning then ≥1
  face — full getUpdatedState recon belongs to the vine body note).
- `GrowingPlantBlock.canSurvive` (cave_vines, growthDirection=DOWN): attach pos = `pos.above()`;
  `if (!canAttachTo(aboveState)) false; else aboveState.is(headBlock) || aboveState.is(bodyBlock)
  || aboveState.isFaceSturdy(level, abovePos, DOWN)` (FULL support, DOWN face). Default
  `canAttachTo` = true (no override in CaveVines classes checked? — head override NOT found;
  see open questions).
- `CarpetBlock.canSurvive` (moss_carpet) = `!level.isEmptyBlock(pos.below())` (any non-air).

Reminder: leaves' support shape is EMPTY (§2.5) ⇒ `isFaceSturdy` false ⇒ cave vines cannot hang
from leaves via the sturdy branch; big-dripleaf/vegetation checks are tag/identity-based and
unaffected by sturdiness.

## 6. C implementation notes

1. Encode per-STATE flags: {isAir, blocksMotion, fluidNonEmpty, isLeaves, replaceable,
   supportShapeKind}. The 4 heightmap predicates then collapse to bit tests. Waterlogged
   variants of leaves/lichen/dripleafs flip fluidNonEmpty only.
2. Heightmap storage: raw = firstFree − minY, index x+16*z, 9 bits. Prime = §1.3 double loop
   (x outer, z inner, y from `highestNonEmptySection.top + 15` down, `is(AIR)` identity skip);
   assert the "list empties every column" precondition instead of porting the carry-over quirk.
3. Every feature write through the region (ProtoChunk.setBlockState path) must run §1.4 update
   against the 4 FINAL maps of the OWNING chunk — including spills into neighbors, where the
   first spill primes that neighbor's FINAL maps from its current blocks first (§1.7). WG maps
   are never updated during features.
4. `getHeight` reads of FINAL types on non-decorated neighbors lazy-prime (§1.6) — same code
   path as the spill priming; implement once.
5. isFaceSturdy for our palette only ever needs: full-cube ⇒ true; leaves ⇒ false; everything
   else in the palette that features query (plants etc.) ⇒ per-shape, but no 9b body queries
   sturdiness of non-full non-leaf palette blocks except cave-vines-on-X (X full cube or leaves
   in practice) — keep a per-state 3x6 bitset anyway, computed from §2.5 exact shapes.
6. blocksMotion special cases: cobweb/bamboo_sapling (neither in our worlds' palette, but keep
   the guard), forceSolidOn (bamboo), forceSolidOff (azalea, flowering_azalea, big_dripleaf).

## 7. Open questions / residual risks

- primeHeightmaps carry-over quirk (§1.3): unreachable for post-noise overworld chunks; if a
  future gate covers superflat/void-ish columns where OCEAN_FLOOR never satisfies, the exact
  fastutil `ObjectArrayList.iterator().back(n)` semantics must be ported. Flagged, not ported.
- Azalea trunk column width read as `column(4?, 0..8)` — first arg constant was clipped in
  extraction; irrelevant to all verdicts here (forceSolidOff), re-extract if azalea shape is
  ever needed for sturdiness (it isn't: azalea support shape = collision shape, only queried by
  FULL/RIGID/CENTER which all fail on the 16x8 top slab? — actually unqueried by 9b bodies).
- Cocoa per-age exact boxes: mechanism verified (per-age `column(4+2*age?, 7−2*age, 12).move(0,0,(age−5)/16)`
  + rotateHorizontal), verdict (never solid) is robust to the clipped first arg; re-extract if a
  cocoa collision/sturdiness read ever appears in a body.
- `CaveVinesBlock`/`CaveVinesPlantBlock` `canAttachTo` overrides were not found in the scanned
  method set; default (always true) assumed [UNVERIFIED — check before implementing cave_vines
  placement; the vanilla 1.20 source has no override, but verify 26.2].
- GrowingPlantHeadBlock 5-arg ctor's `0.1D` growPerTickProbability and `scheduleFluidTicks=false`
  for cave_vines verified; the BODY block ctor args (CaveVinesPlantBlock → GrowingPlantBodyBlock)
  not individually dumped (shape constant + scheduleFluidTicks) — worldgen-irrelevant except
  shape (unused by placement predicates).
- `Blocks.register(BlockItemId, ...)` id plumbing was not audited (assumed no property effects).
- EnumSet iteration order = ordinal order is JDK library semantics (labeled, not bytecode).
