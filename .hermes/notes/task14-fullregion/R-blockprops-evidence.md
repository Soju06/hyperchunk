# R-blockprops evidence — MC 26.2 server, bytecode/decompile-pinned

Companion to `R-blockprops.tsv` (209 missing states + 4 REVERIFY rows). Every value in the TSV
derives from the sources below. Decompiler: vineflower-1.11.1 over
`tools/golden/work/server/` class tree; outputs under `tools/golden/work/task14-blockprops-decomp/`
(state/, lighting/, blocks/, blocksreg/Blocks.java, shapes/, math/, misc*/). Bytecode reads via
`javap -c -p` where noted.

## 1. blocksMotion() / legacySolid cache

`net/minecraft/world/level/block/state/BlockBehaviour$BlockStateBase` (decomp `state/BlockBehaviour.java`):

```java
@Deprecated public boolean blocksMotion() {
   Block block = this.getBlock();
   return block != Blocks.COBWEB && block != Blocks.BAMBOO_SAPLING && this.isSolid();
}
@Deprecated public boolean isSolid() { return this.legacySolid; }   // set in initCache()

private boolean calculateSolid() {
   if (this.owner.properties.forceSolidOn) return true;
   else if (this.owner.properties.forceSolidOff) return false;
   else if (this.cache == null) return false;           // dynamicShape blocks
   else {
      VoxelShape shape = this.cache.collisionShape;     // getCollisionShape(EmptyBlockGetter, ZERO, empty ctx)
      if (shape.isEmpty()) return false;
      AABB bounds = shape.bounds();
      return bounds.getSize() >= 0.7291666666666666 ? true : bounds.getYsize() >= 1.0;
   }
}
```
`AABB.getSize()` = `(getXsize()+getYsize()+getZsize())/3.0` (javap of `net/minecraft/world/phys/AABB.getSize`,
bytecode: dadd, dadd, ldc2_w 3.0d, ddiv).

Consequences used in the TSV:
- `noCollision()` sets `hasCollision=false` AND `canOcclude=false` (Properties.noCollision) → collision
  shape empty → legacySolid=false (flowers, rails, tripwire(+hook), torches, buttons, vine, leaf_litter,
  bubble_column, candles are additionally just small shapes).
- Ladder: `forceSolidOff()` in Blocks registration → false.
- Cobweb: `forceSolidOn().noCollision()` → legacySolid=true but blocksMotion=false (explicit COBWEB
  exclusion above).
- Iron chain: `forceSolidOn()` → true.
- Oak fence has `forceSolidOn()`; jungle/spruce fences do NOT — all still true because
  CrossCollisionBlock collision shape height = 24/16=1.5 (`FenceBlock` ctor `super(4.0F,16.0F,4.0F,16.0F,24.0F,p)`;
  `CrossCollisionBlock` ctor: `collisionShapes = makeShapes(postWidth, collisionHeight, wallWidth, 0.0F, collisionHeight)`)
  → `getYsize()>=1.0` branch.
- Doors/trapdoors: panel `Block.boxZ(16.0,13.0,16.0)` → bounds 1×1×0.1875;
  `(1.0+1.0+0.1875)/3.0 == 0.7291666666666666` exactly (same IEEE double as the literal; verified in
  Python double arithmetic) → `>=` true → legacySolid=true.
- Beds: shape `column(16,3,9) ∪ legs(y 0..3)` → bounds y 0..9/16; avg `(1+0.5625+1)/3 = 0.8541666666666666` → true.
- Chest: `column(14,0,14)` → avg 0.875 → true. Decorated pot: `column(14,0,16)` → avg ≈0.9167 → true.
  Hopper: bounds x/z full, y up to 16 → avg ≥0.9167 → true.
- Flower pot / potted_*: `column(6,0,6)` → avg 0.375, ysize 0.375 → false.
- Slabs (1+0.5+1)/3≈0.833, stairs bounds full → true. Leaves, mangrove_roots, spawner/trial_spawner/vault,
  copper grate, glazed terracotta, suspicious_sand: full-cube collision → true.

## 2. fullOcclude (solidRender) and the lightBlock (getLightDampening) formula

`BlockStateBase.initCache()`:
```java
this.occlusionShape = this.canOcclude ? this.owner.getOcclusionShape(this.asState()) : Shapes.empty();
this.solidRender = Block.isShapeFullBlock(this.occlusionShape);
... occlusionShapesByFace[dir] = this.occlusionShape.getFaceShape(direction);   // unless empty/full
this.propagatesSkylightDown = this.owner.propagatesSkylightDown(this.asState());
this.lightDampening = this.owner.getLightDampening(this.asState());
```
TSV `fullOcclude` = solidRender = canOcclude && occlusion shape is a full cube.
Default `getOcclusionShape(state)` = `state.getShape(EmptyBlockGetter, ZERO)`;
FenceBlock overrides it (`makeShapes(4,16,2,6,15)`) — still not full → fullOcclude=false.

26.2 lightBlock formula — `BlockBehaviour.getLightDampening` (this replaces the old getLightBlock):
```java
protected int getLightDampening(BlockState state) {
   if (state.isSolidRender()) return 15;
   else return state.propagatesSkylightDown() ? 0 : 1;
}
```
Default `propagatesSkylightDown(state)` =
`!Block.isShapeFullBlock(state.getShape(EmptyBlockGetter, ZERO)) && state.getFluidState().isEmpty()`.
So ANY waterlogged (or intrinsically fluid) non-full-occluding state gets lightBlock=1 unless a block
overrides. LightEngine additionally floors at runtime: `getOpacity(state) = Math.max(1, state.getLightDampening())`.

Overrides relevant to the list (scanned the entire `block/*.class` tree for the four method names via
constant-pool grep; only these classes override for our blocks):
- `LeavesBlock.getLightDampening` → `return 1;` (mangrove_leaves via MangroveLeavesBlock →
  TintedParticleLeavesBlock → LeavesBlock) — always 1.
- `TransparentBlock.propagatesSkylightDown` → `return true;` (stained glass; copper grates via
  WaterloggedTransparentBlock) → 0.
- `CrossCollisionBlock.propagatesSkylightDown` → `return !state.getValue(WATERLOGGED);` (fences) → 0 dry / 1 waterlogged.
- `VineBlock.propagatesSkylightDown` → `return true;` → 0.
- `VegetationBlock.propagatesSkylightDown` → `return state.getFluidState().isEmpty();` (azure_bluet) → 0.
- NetherrackBlock only *calls* propagatesSkylightDown (nylium spread check), no override → 15 via solidRender.

Special values so derived: spawner=1, trial_spawner=1, vault=1, mangrove_roots=1, cobweb=1 (all:
`noOcclusion()`/noCollision + default FULL getShape → propagates=false); WebBlock/SpawnerBlock/
TrialSpawnerBlock/VaultBlock/MangroveRootsBlock/BrushableBlock/GlazedTerracottaBlock/BarrelBlock/
DispenserBlock declare NO getShape/occlusion/light overrides (decompiled, verified).
bubble_column=1 (shape `Shapes.empty()` but FluidState=water). leaf_litter=0 (flat per-state shapes,
no fluid). Waterlogged chests/slabs/stairs/fences=1; their dry counterparts 0.

## 3. lightEmission (state-dependent), from Blocks.java registrations

- Copper bulbs (`COPPER_BULB = WeatheringCopperCollection.registerBlocks(...)`):
  `.lightLevel(litBlockEmission(switch (weatherState) { UNAFFECTED->15; EXPOSED->12; WEATHERED->8; OXIDIZED->4 }))`
  with `litBlockEmission(n) = state -> state.getValue(LIT) ? n : 0`.
  `WeatheringCopperCollection.registerBlocks` applies the SAME `propertiesSupplier.apply(state)` to waxed
  and unwaxed variants (decompiled WeatheringCopperCollection.java, lines registering both `weatheringIds`
  and `waxedIds`). → waxed_copper_bulb lit=15/unlit=0; waxed_exposed lit=12; waxed_weathered lit=8;
  waxed_oxidized lit=4. `powered` has no effect on emission.
- Candles: `candleProperties(...)` uses `.lightLevel(CandleBlock.LIGHT_EMISSION)`;
  `CandleBlock.LIGHT_EMISSION = state -> state.getValue(LIT) ? 3 * state.getValue(CANDLES) : 0`.
  → candle[1..4, lit=false]=0; red_candle[3,lit=true]=9; red_candle[4,lit=true]=12.
- Trial spawner: `.lightLevel(s -> s.getValue(TrialSpawnerBlock.STATE).lightLevel())`;
  `TrialSpawnerState` enum (decomp misc/TrialSpawnerState.java): INACTIVE=0, WAITING_FOR_PLAYERS=4,
  ACTIVE=8, WAITING_FOR_REWARD_EJECTION=8, EJECTING_REWARD=8, COOLDOWN=0. → waiting_for_players → 4.
- Vault: `.lightLevel(s -> s.getValue(VaultBlock.STATE).lightLevel())`; `VaultState` enum:
  INACTIVE=HALF_LIT, ACTIVE/UNLOCKING/EJECTING=LIT; `LightLevel { HALF_LIT(6), LIT(12) }`.
  → vault[...vault_state=inactive]=6 for all facings and both ominous values (ominous not consulted).
- crying_obsidian `.lightLevel(s->10)`; wall_torch `.lightLevel(s->14)`; spawner registration has NO
  `.lightLevel` call → emission 0 (REVERIFY confirmed).
- bubble_column: no lightLevel → 0.

## 4. useShapeForLightOcclusion states + face slice masks

Only `StairBlock.useShapeForLightOcclusion → true` and
`SlabBlock.useShapeForLightOcclusion → state.getValue(TYPE) != SlabType.DOUBLE` override the false default
among the listed blocks (full-tree constant-pool scan; other overriders — DaylightDetector, DirtPath,
EnchantingTable, EndPortalFrame, Farmland, Lectern, SculkSensor/Shrieker, Shelf, SnowLayer, Stonecutter,
pistons — are not in the list). All listed slab/stair states are canOcclude=true (copied from full-cube
base via ofLegacyCopy/ofFullCopy; no noOcclusion), so LightEngine actually consumes their shapes.

Face slices: `initCache` stores `occlusionShape.getFaceShape(direction)` per direction;
`VoxelShape.calculateFace(dir)` slices the shape at the face plane (index found at 1e-7 / 0.9999999) and
returns `Shapes.empty()` if nothing touches the face, `Shapes.block()` if the slice is the full 1×1
(isCubeLike after slicing), else the slice.

Shape construction (decomp blocks/StairBlock.java, SlabBlock.java):
```
SLAB: SHAPE_BOTTOM = Block.column(16,0,8); SHAPE_TOP = Block.column(16,8,16)
STAIR: SHAPE_OUTER    = column(16,0,8) ∪ box(0,8,0, 8,16,8)          // bottom slab + top NW quadrant
       SHAPE_STRAIGHT = OUTER ∪ rotate(OUTER, BLOCK_ROT_Y_90)        // + top NE  → top north half
       SHAPE_INNER    = STRAIGHT ∪ rotate(STRAIGHT, BLOCK_ROT_Y_90)  // top all but SW quadrant
       maps = Shapes.rotateHorizontal(shape [, INVERT_Y for half=top])
       getShape: STRAIGHT/OUTER_LEFT/INNER_RIGHT -> map[facing];
                 INNER_LEFT -> map[facing.getCounterClockWise()]; OUTER_RIGHT -> map[facing.getClockWise()]
```
Rotation semantics pinned from `Shapes.rotate` + `OctahedralGroup` (decomp math/OctahedralGroup.java):
new coordinate along axis A = old coordinate along axis `perm(A)`, mirrored about center 0.5 iff
`inverts(A)`. `BLOCK_ROT_Y_90 = ROT_90_Y_NEG = (P321, invertX)` → (x,y,z)→(1−z, y, x), i.e. maps a
north-authored shape to the EAST-facing variant (checked against `OctahedralGroup.rotate(Direction)`:
NORTH→EAST). `BLOCK_ROT_Y_180 = (P123, invX+invZ)`; `BLOCK_ROT_Y_270 = (P321, invertZ)`;
`INVERT_Y = (P123, invertY)`. `rotateHorizontal(north, initial)` keys:
NORTH=initial, EAST=Y_90∘initial, SOUTH=Y_180∘initial, WEST=Y_270∘initial (compose = right-hand-first).
Sanity: facing=north straight bottom stair = bottom slab + full-height north half — matches vanilla.

Mask convention in the TSV (world-axis based, quadrant = fully covered by the face slice; all slab/stair
slices are aligned to the 0.5 grid so quadrants are all-or-nothing):
- nibble bit index = 2*A + B, hex digit per face `D:x,U:x,N:x,S:x,W:x,E:x`
- N/S/E/W faces: A=0 lower half (y<0.5), A=1 upper half; B: on N/S faces 0=west half /1=east half,
  on W/E faces 0=north half /1=south half.
- D/U faces: A=0 north half (z<0.5), A=1 south half; B=0 west /1=east.
- 0x0=empty face, 0xF=full face. Examples: slab bottom = D:f,U:0,N:3,S:3,W:3,E:3;
  stair facing=north half=bottom straight = D:f,U:3,N:f,S:3,W:7,E:7.
Masks in the TSV were generated by a script that reproduces exactly the constructions above
(Fraction arithmetic, half-alignment asserted).

## 5. LightEngine shapeOccludes — merge rule

`net/minecraft/world/level/lighting/LightEngine` (decomp lighting/LightEngine.java):
```java
protected static boolean isEmptyShape(BlockState state) {
   return !state.canOcclude() || !state.useShapeForLightOcclusion();
}
public static VoxelShape getOcclusionShape(BlockState state, Direction direction) {
   return isEmptyShape(state) ? Shapes.empty() : state.getFaceOcclusionShape(direction);
}
protected boolean shapeOccludes(BlockState fromState, BlockState toState, Direction direction) {
   VoxelShape fromShape = getOcclusionShape(fromState, direction);
   VoxelShape toShape = getOcclusionShape(toState, direction.getOpposite());
   return Shapes.faceShapeOccludes(fromShape, toShape);
}
```
`Shapes.faceShapeOccludes(shape, occluder)`:
true if either arg is the `block()` singleton; false if both empty; otherwise
`!joinIsNotEmpty(block(), joinUnoptimized(shape, occluder, OR), ONLY_FIRST)` — i.e. **light is blocked
iff the union of the two facing slices covers the entire full-block face** (full cube minus union empty).
So yes: two mating half-slices (e.g. bottom slab UP face empty vs anything) merge by union; a bottom
slab against an upper slab across a vertical face (3|c) unions to f → occludes.
Note the direction asymmetry only in which state contributes which face (from: `direction`,
to: `direction.getOpposite()`).
Only slab/stair(-like) states ever contribute non-empty slices here; every other state in the list is
`isEmptyShape` for the light engine (either !canOcclude or !useShapeForLightOcclusion), including fences
despite their non-empty occlusionShape. Full opaque cubes are handled before shapes via
lightDampening=15. Related: `hasDifferentLightProperties` compares lightDampening, lightEmission and
useShapeForLightOcclusion; `getLightDampeningInto` (mergedFaceOccludes on whole occlusion shapes) is the
directional variant used by skylight sources.

## 6. FluidState

- Standard waterlogged pattern everywhere in the list (`getFluidState → WATERLOGGED ? Fluids.WATER.getSource(false) : super`):
  SlabBlock, StairBlock, CrossCollisionBlock(fences), ChestBlock, ChainBlock, LadderBlock, BaseRailBlock,
  CandleBlock, DecoratedPotBlock, TrapDoorBlock, LeavesBlock, MangroveRootsBlock.
- Quirk (bytecode-verified via javap): `WaterloggedTransparentBlock.getFluidState` (copper grates) uses
  `iconst_1` → `Fluids.WATER.getSource(true)` i.e. the *falling* source variant when waterlogged.
  Irrelevant for the listed grate states (waterlogged=false) but worth pinning for emulation.
- Always-fluid block in the list: `bubble_column` — `BubbleColumnBlock.getFluidState → Fluids.WATER.getSource(false)`
  unconditionally; also `replaceable()`, `noCollision()`, `liquid()` in registration; getShape = Shapes.empty().
- No kelp/seagrass in the list.

## 7. canBeReplaced (cache flag)

`BlockStateBase.canBeReplaced()` returns the `properties.replaceable` flag. In the list only
BUBBLE_COLUMN, LEAF_LITTER and VINE registrations call `.replaceable()` → true; all other 209-list
blocks false. (LeafLitterBlock additionally overrides the contextual
`canBeReplaced(state, BlockPlaceContext)` for segment stacking — does not affect the cached flag.)

## 8. LeavesBlock membership

Only `mangrove_leaves` in the list: `MangroveLeavesBlock extends TintedParticleLeavesBlock extends LeavesBlock`.

## 9. REVERIFY items

- `minecraft:spawner`: registration `Properties.of()...strength(5.0F).sound(SoundType.SPAWNER).noOcclusion()`
  — NO lightLevel → emission 0; SpawnerBlock (extends BaseEntityBlock) has no shape/light overrides →
  full default shape + noOcclusion → lightBlock 1; full collision → legacySolid/blocksMotion true.
- `minecraft:cobweb`: forceSolidOn + noCollision; legacySolid true, blocksMotion FALSE (hardcoded
  exclusion in blocksMotion), lightBlock 1 (full outline shape), fullOcclude false (noCollision cleared canOcclude).
- `minecraft:rail`: noCollision → legacySolid/blocksMotion false; flat shape (BaseRailBlock), lightBlock 0;
  fluid empty at waterlogged=false.
- `minecraft:trial_spawner[waiting_for_players]` emission = 4; `minecraft:vault[...inactive]` emission = 6
  (see §3; vault rows carry it in the main 209 set — all 8 facing×ominous inactive states are 6).

## 10. Registration-arg provenance for the full-cube rows

`blocksreg/Blocks.java` (vineflower of Blocks.class, 5147 lines; extracted statements in
`blocksreg/wanted_regs.txt`): chiseled_tuff/chiseled_tuff_bricks/polished_tuff/tuff_bricks =
ofLegacyCopy(TUFF); cobbled_deepslate = ofLegacyCopy(DEEPSLATE); stone-family/planks/logs/concrete/
glazed terracotta/copper family as quoted there. CUT/CHISELED/oxidized copper cubes =
`Properties.ofFullCopy(COPPER_BLOCK...)` → plain `Block` full cubes. `ofLegacyCopy`/`ofFullCopy` copy
canOcclude/forceSolidOn/forceSolidOff, so none of these acquire noOcclusion. GLAZED_TERRACOTTA collection
adds pushReaction(PUSH_ONLY) only. STAINED_GLASS collection: `noOcclusion()` (+never predicates).
BED collection: `noOcclusion()` etc. DYED_CANDLE collection: `candleProperties(color)`.

## 11. R-blockprops2.tsv addendum (33 states + features + collisionFull)

`collisionFull` column = `BlockStateBase.Cache.isCollisionShapeFullBlock`:
```java
this.isCollisionShapeFullBlock = Block.isShapeFullBlock(state.getCollisionShape(EmptyBlockGetter.INSTANCE, BlockPos.ZERO));
```
(`isCollisionShapeFullBlock()` returns the cached value when cache != null; none of the listed blocks are
dynamicShape.) `Block.isShapeFullBlock(shape)` = SHAPE_FULL_BLOCK_CACHE — shape must equal the full unit
cube, not merely have full bounds. Hence stairs/fences/hopper/decorated_pot/chest/beds/doors/trapdoors are
`f` despite legacySolid=true; full-collision non-occluding blocks (leaves, mangrove_roots, spawner,
trial_spawner, vault, stained glass, copper grate, cobweb=empty→f exception noted) follow their collision
shape: leaves/roots/spawner-likes/glass/grate = t, cobweb = f (noCollision → empty), web outline full is
outline-only.

New blocks pinned for R-blockprops2:
- JIGSAW / STRUCTURE_BLOCK (Blocks.java): `Properties.of().mapColor(COLOR_LIGHT_GRAY).requiresCorrectToolForDrops().strength(-1.0F, 3600000.0F).noLootTable()`
  — no noOcclusion, and `javap -p` of JigsawBlock.class / StructureBlock.class shows NO
  getShape/getCollisionShape/getOcclusionShape/propagatesSkylightDown/getLightDampening/useShapeForLightOcclusion
  declarations → plain full opaque cubes: blocksMotion/legacySolid/fullOcclude/collisionFull = t, lightBlock 15,
  emission 0. `orientation`/`mode` have no effect on any cached property.
- BIRCH_LOG: `RotatedPillarBlock`, logProperties(...) → full cube (same as spruce/mangrove logs).
- BIRCH_LEAVES: `p -> new TintedParticleLeavesBlock(0.01F, p)`, `leavesProperties(SoundType.GRASS)`
  → LeavesBlock subclass: lightBlock=1 (override), noOcclusion → fullOcclude=f, collision full → collisionFull=t,
  waterlogged=true rows carry FLUID=water (LeavesBlock.getFluidState standard pattern).
- OXEYE_DAISY / CORNFLOWER / ORANGE_TULIP / RED_TULIP / PINK_TULIP / WHITE_TULIP: FlowerBlock with
  `noCollision().instabreak()...offsetType(XZ).pushReaction(DESTROY)` — identical value profile to azure_bluet.
- BedBlock/HopperBlock/DecoratedPotBlock declare only `getShape` (javap) → collision = outline shape → collisionFull f.

The appendix at the bottom of R-blockprops2.tsv lists collisionFull for all 209 R-blockprops.tsv states
(145 f / 64 t), derived from the same per-block classification.
