# R4 — Per-blockstate light property tables for the Task-10 palette (26.2 bytecode)

Lane B4. Source of truth: `javap -p -c -constants` (and `-v` for BootstrapMethods) on
`tools/golden/libs/extracted/server-26.2.jar`. Everything below is derived from bytecode;
no decompiled source was copied.

---

## 1. How 26.2 computes the four per-state values (pipeline)

26.2 **renamed `getLightBlock` → `getLightDampening`** and made it a *derived* value, not a
Properties field. There is no per-block `lightBlock` constant anymore.

`BlockBehaviour$BlockStateBase` constructor (javap of
`net.minecraft.world.level.block.state.BlockBehaviour$BlockStateBase`):

* `lightEmission` — bc #23–38: `getfield Properties.lightEmission:ToIntFunction` (#31 at 26),
  `ToIntFunction.applyAsInt(asState())` (#41 at 33), `putfield lightEmission:I` (#47 at 38).
  Default function is `Properties.lambda$new$1` = `iconst_0` (constant 0), installed in
  `Properties.<init>` via InvokeDynamic bsm#1.
* `useShapeForLightOcclusion` — bc 41–50: `invokevirtual Block.useShapeForLightOcclusion(state)`
  (#50 at 47), `putfield` #54. Default (`BlockBehaviour.useShapeForLightOcclusion`) = `iconst_0`.
* `canOcclude` — copied from `Properties.canOcclude` (getfield #100 at 131, putfield #103).
  `Properties.<init>` sets `canOcclude=true` (iconst_1, putfield #61 at bc 68–70).
  `Properties.noOcclusion()` → `canOcclude=false`. **`Properties.noCollision()` sets BOTH
  `hasCollision=false` AND `canOcclude=false`** (putfield #17 then #61). `forceSolidOn/Off`,
  `liquid()`, `replaceable()` do NOT touch canOcclude.

`BlockStateBase.initCache()` (order matters; all putfields cited):

1. `occlusionShape` = `canOcclude ? Block.getOcclusionShape(state) : Shapes.empty()`
   (ifeq at 74 → Shapes.empty #215 at 94; else invokevirtual #211 at 88; putfield #221 at 97).
   Default `BlockBehaviour.getOcclusionShape(state)` = `state.getShape(EmptyBlockGetter.INSTANCE,
   BlockPos.ZERO)` (invokevirtual BlockState.getShape #399).
2. `solidRender` = `Block.isShapeFullBlock(occlusionShape)` (invokestatic #224 at 105,
   putfield #228 at 108). (`isShapeFullBlock` goes through `SHAPE_FULL_BLOCK_CACHE`
   LoadingCache in `Block`.)
3. `occlusionShapesByFace`: if occlusionShape empty → shared `EMPTY_OCCLUSION_SHAPES`
   (bc 121–125); if solidRender → shared `FULL_BLOCK_OCCLUSION_SHAPES` (bc 138–142); else
   per-direction `occlusionShape.getFaceShape(dir)` (invokevirtual VoxelShape.getFaceShape
   #251 at 193).
4. `propagatesSkylightDown` = `Block.propagatesSkylightDown(state)` (invokevirtual #255 at 215,
   putfield #258 at 218). Default (`BlockBehaviour.propagatesSkylightDown`):
   `!Block.isShapeFullBlock(state.getShape(EMPTY,ZERO)) && state.getFluidState().isEmpty()`
   (getShape #399, isShapeFullBlock #433 ifne→false, FluidState.isEmpty #453).
   NOTE: uses **getShape** (collision-ish outline), not occlusionShape, so it is independent
   of canOcclude.
5. `lightDampening` = `Block.getLightDampening(state)` (invokevirtual #260 at 233, putfield
   #264 at 236). Default (`BlockBehaviour.getLightDampening`, bc 0–22):

   ```
   dampening(state) = state.isSolidRender() ? 15
                    : state.propagatesSkylightDown() ? 0 : 1
   ```
   (`isSolidRender()` returns the cached `solidRender` field.)

So the whole per-state light tuple in C is:
`(emission, dampening, propagatesSkylightDown, useShapeForLightOcclusion, canOcclude, occlusionShapeFaces)`.

## 2. What the light engine actually consumes (why the columns matter)

From `net.minecraft.world.level.lighting.LightEngine`:

* `getOpacity(state)` = `Math.max(1, state.getLightDampening())` (iconst_1; invokevirtual
  BlockState.getLightDampening #54; invokestatic Math.max #125).
* `isEmptyShape(state)` = `!(state.canOcclude() && state.useShapeForLightOcclusion())`
  (invokevirtual canOcclude #88, useShapeForLightOcclusion #63). **A full solid stone block is
  an "empty shape" for the light engine** — full blocks stop light via dampening=15, never via
  shape occlusion.
* static `getOcclusionShape(state, dir)` = `isEmptyShape ? Shapes.empty()
  : state.getFaceOcclusionShape(dir)` (#84).
* `shapeOccludes(s1, s2, dir)` = `Shapes.faceShapeOccludes(occ(s1,dir), occ(s2,dir.opposite))`
  (invokestatic #140).
* static `getLightDampeningInto(s1, s2, dir, base)`: if both isEmptyShape → base; else
  `Shapes.mergedFaceOccludes(shape1, shape2, dir)` on the FULL 3-D `getOcclusionShape()`s
  (invokestatic #80) → returns `16` (bipush at 69) when occluded, else base.
* `hasDifferentLightProperties(a, b)` compares exactly (dampening, emission, USO) — the
  identity triple for "does a block change require relight".
* `ChunkSkyLightSources.isEdgeOccluded(upper, lower)`:
  `if (lower.getLightDampening() != 0) return true;` (invokevirtual #158) then
  `Shapes.faceShapeOccludes(LightEngine.getOcclusionShape(upper,DOWN),
  LightEngine.getOcclusionShape(lower,UP))` (#163/#172). I.e. the sky-source column scan keys
  off **dampening==0**, NOT propagatesSkylightDown. PSD only matters as an *input* to the
  dampening default at state-init time; the engine itself never calls it.

Consequence for C: per state you need `emission`, `dampening`, and (only when
`canOcclude && useShapeForLightOcclusion`) per-face occlusion shapes. In this entire palette +
ring-feature additions, **exactly one state has a non-empty light shape: `minecraft:snow[layers=1]`**.

## 3. Jar-wide override scan (all of `net/minecraft/world/level/block`)

Classes *declaring* each method (full scan of extracted .class files, confirmed via javap):

* `getLightDampening`: **LeavesBlock** (`iconst_1` — unconditional 1), TintedGlassBlock (n/a).
* `propagatesSkylightDown`: **LiquidBlock** (`iconst_0`), **VineBlock** (`iconst_1`),
  **BambooStalkBlock** (`iconst_1`), **VegetationBlock**
  (`state.getFluidState().isEmpty()` — #55/#59), **GlowLichenBlock** (same, #46/#52),
  HangingMossBlock (`iconst_1`), MossyCarpetBlock (`iconst_1`; this is *pale* moss carpet, NOT
  `minecraft:moss_carpet` which is plain `CarpetBlock`), plus TransparentBlock, ShulkerBoxBlock,
  PipeBlock, CrossCollisionBlock, WallBlock, LightBlock, BarrierBlock, TintedGlassBlock (none in
  our set).
* `useShapeForLightOcclusion`: **SnowLayerBlock** (`iconst_1`), plus Slab/Stair/DirtPath/
  Farmland/Lectern/EnchantingTable/DaylightDetector/EndPortalFrame/SculkSensor/SculkShrieker/
  Piston(Base,Head)/Stonecutter/Shelf (none in our set).
* `getOcclusionShape`: Lectern/FenceGate/Fence/SculkShrieker only (none in our set) — everything
  in our set uses the default `getShape(EMPTY, ZERO)`.

Class assignments for the palette (from `Blocks.<clinit>` BootstrapMethods, javap -v):
AirBlock (air/cave_air, `getShape`=Shapes.empty()), GrassBlock, RotatedPillarBlock (logs,
deepslate), TintedParticleLeavesBlock (oak/jungle leaves, bsm#53/#56 → lambda$static$26/29),
UntintedParticleLeavesBlock (azalea/flowering azalea leaves, bsm#62/#63 → lambda$static$35/36)
— both extend LeavesBlock; TallGrassBlock (short_grass/fern), DoublePlantBlock (tall_grass),
FlowerBlock (dandelion/poppy), SeagrassBlock/TallSeagrassBlock, BushBlock (bush),
FireflyBushBlock, SugarCaneBlock, AzaleaBlock, SporeBlossomBlock, HangingRootsBlock,
CaveVinesBlock/CaveVinesPlantBlock (→GrowingPlant*→Block), CocoaBlock, BigDripleafBlock/
BigDripleafStemBlock (→HorizontalDirectionalBlock→Block), SmallDripleafBlock (→DoublePlantBlock
→VegetationBlock), AmethystClusterBlock (cluster + all 3 buds; buds use
`Properties.ofLegacyCopy(AMETHYST_CLUSTER)` then re-set sound+lightLevel), AmethystBlock,
BuddingAmethystBlock, ChestBlock, SpawnerBlock (→BaseEntityBlock→Block, **no getShape
override → full cube**), CarpetBlock (moss_carpet), SnowLayerBlock, DropExperienceBlock (all
ores incl. copper/deepslate), RedStoneOreBlock, MagmaBlock, SandBlock/ColoredFallingBlock,
BonemealableFeaturePlacerBlock (moss_block), RootedDirtBlock, LiquidBlock, VineBlock,
GlowLichenBlock (→MultifaceSpreadeableBlock→MultifaceBlock), BambooStalkBlock, MelonBlock does
not exist anymore — **melon registers as plain `Block`** (`register(BlockItemId, Properties)`),
full cube.

Hierarchy notes that decide PSD:
* SporeBlossomBlock, HangingRootsBlock, SugarCaneBlock, CaveVines* extend `Block` directly →
  default PSD path (their `getShape` overrides are non-full → PSD = fluidEmpty in effect).
* DoublePlantBlock has **no getShape override** (default full cube!) but that never leaks:
  PSD comes from the VegetationBlock override (fluid check only) and dampening's solidRender
  uses `occlusionShape`, which is empty because tall_grass et al. are `noCollision()`
  (canOcclude=false).

## 4. Emission sources (Blocks.<clinit> landmarks)

* Default: `Properties.lambda$new$1` = 0.
* `LAVA`: InvokeDynamic bsm#39 → `Blocks.lambda$static$13` = `bipush 15` (constant, all levels).
* `MAGMA_BLOCK`: bsm#409 → `lambda$static$271` = `iconst_3`.
* `REDSTONE_ORE` / `DEEPSLATE_REDSTONE_ORE`: `bipush 9; invokestatic litBlockEmission` (clinit
  bc 10006/10008); `litBlockEmission` → `lambda$litBlockEmission$0` =
  `state.getValue(BlockStateProperties.LIT) ? level : 0`. Palette has `lit=false` → **0**
  (lit=true → 9).
* `CAVE_VINES` / `CAVE_VINES_PLANT`: `bipush 14; invokestatic CaveVines.emission` (clinit bc
  29752/29754). `CaveVines.lambda$emission$0` = `state.getValue(BERRIES) ? 14 : 0`.
* `GLOW_LICHEN`: `bipush 7; invokestatic GlowLichenBlock.emission` (clinit bc 12803/12805).
  `GlowLichenBlock.lambda$emission$0` = `MultifaceBlock.hasAnyFace(state) ? 7 : 0`;
  `hasAnyFace` loops all 6 direction properties. Every glow_lichen state in the palette has
  ≥1 face=true → **7** (waterlogging does not affect emission).
* `AMETHYST_CLUSTER`/`LARGE`/`MEDIUM`/`SMALL_AMETHYST_BUD`: bsm#532/534/536/538 →
  `lambda$static$345/347/349/351` = `iconst_5 / iconst_4 / iconst_2 / iconst_1`
  (constants; independent of facing/waterlogged).
* `FIREFLY_BUSH`: bsm#628 → `lambda$static$410` = `iconst_2`.
* Everything else in the set (incl. **SPAWNER** and **CHEST**): no `lightLevel(...)` call in
  their registration segment → emission 0.

## 5. C-ready table

Columns: `EM` = lightEmission; `LD` = getLightDampening (the 26.2 "light block"/opacity input;
engine uses `max(1, LD)` per step); `PSD` = propagatesSkylightDown (input to LD only);
`USO` = useShapeForLightOcclusion; `LSHAPE` = occlusion shape as seen by the light engine
(after the `canOcclude && USO` gate of `LightEngine.isEmptyShape`).

Derivation reminders baked into the rows:
* full-cube canOcclude blocks → solidRender → LD 15, PSD false.
* leaves → LeavesBlock override LD=1 (PSD false because default getShape is the full cube).
* `noCollision()`/`noOcclusion()` plants → canOcclude false → solidRender false →
  LD = PSD ? 0 : 1, PSD per override/default.
* waterlogged=true (or intrinsic water: seagrass, tall_seagrass, water, lava) → fluid non-empty
  → PSD false → LD 1 (for non-solid, non-leaves).

| state | EM | LD | PSD | USO | LSHAPE |
|---|---|---|---|---|---|
| `minecraft:air` | 0 | 0 | T | F | EMPTY |
| `minecraft:andesite` | 0 | 15 | F | F | EMPTY |
| `minecraft:azalea` | 0 | 0 | T | F | EMPTY |
| `minecraft:azalea_leaves[distance=1,persistent=false,waterlogged=false]` | 0 | 1 | F | F | EMPTY |
| `minecraft:azalea_leaves[distance=2,persistent=false,waterlogged=false]` | 0 | 1 | F | F | EMPTY |
| `minecraft:azalea_leaves[distance=3,persistent=false,waterlogged=false]` | 0 | 1 | F | F | EMPTY |
| `minecraft:azalea_leaves[distance=4,persistent=false,waterlogged=false]` | 0 | 1 | F | F | EMPTY |
| `minecraft:azalea_leaves[distance=5,persistent=false,waterlogged=false]` | 0 | 1 | F | F | EMPTY |
| `minecraft:bamboo[age=1,leaves=large,stage=0]` | 0 | 0 | T | F | EMPTY |
| `minecraft:bamboo[age=1,leaves=large,stage=1]` | 0 | 0 | T | F | EMPTY |
| `minecraft:bamboo[age=1,leaves=none,stage=0]` | 0 | 0 | T | F | EMPTY |
| `minecraft:bamboo[age=1,leaves=small,stage=0]` | 0 | 0 | T | F | EMPTY |
| `minecraft:bedrock` | 0 | 15 | F | F | EMPTY |
| `minecraft:big_dripleaf[facing=east,tilt=none,waterlogged=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:big_dripleaf[facing=east,tilt=none,waterlogged=true]` | 0 | 1 | F | F | EMPTY |
| `minecraft:big_dripleaf[facing=north,tilt=none,waterlogged=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:big_dripleaf[facing=north,tilt=none,waterlogged=true]` | 0 | 1 | F | F | EMPTY |
| `minecraft:big_dripleaf[facing=south,tilt=none,waterlogged=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:big_dripleaf[facing=south,tilt=none,waterlogged=true]` | 0 | 1 | F | F | EMPTY |
| `minecraft:big_dripleaf[facing=west,tilt=none,waterlogged=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:big_dripleaf_stem[facing=east,waterlogged=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:big_dripleaf_stem[facing=east,waterlogged=true]` | 0 | 1 | F | F | EMPTY |
| `minecraft:big_dripleaf_stem[facing=north,waterlogged=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:big_dripleaf_stem[facing=north,waterlogged=true]` | 0 | 1 | F | F | EMPTY |
| `minecraft:big_dripleaf_stem[facing=south,waterlogged=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:big_dripleaf_stem[facing=south,waterlogged=true]` | 0 | 1 | F | F | EMPTY |
| `minecraft:big_dripleaf_stem[facing=west,waterlogged=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:big_dripleaf_stem[facing=west,waterlogged=true]` | 0 | 1 | F | F | EMPTY |
| `minecraft:cave_air` | 0 | 0 | T | F | EMPTY |
| `minecraft:cave_vines[age=23,berries=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:cave_vines[age=23,berries=true]` | 14 | 0 | T | F | EMPTY |
| `minecraft:cave_vines[age=24,berries=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:cave_vines[age=24,berries=true]` | 14 | 0 | T | F | EMPTY |
| `minecraft:cave_vines[age=25,berries=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:cave_vines[age=25,berries=true]` | 14 | 0 | T | F | EMPTY |
| `minecraft:cave_vines_plant[berries=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:cave_vines_plant[berries=true]` | 14 | 0 | T | F | EMPTY |
| `minecraft:chest[facing=east,type=single,waterlogged=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:clay` | 0 | 15 | F | F | EMPTY |
| `minecraft:coal_ore` | 0 | 15 | F | F | EMPTY |
| `minecraft:cobblestone` | 0 | 15 | F | F | EMPTY |
| `minecraft:cocoa[age=0,facing=east]` | 0 | 0 | T | F | EMPTY |
| `minecraft:cocoa[age=0,facing=north]` | 0 | 0 | T | F | EMPTY |
| `minecraft:cocoa[age=0,facing=west]` | 0 | 0 | T | F | EMPTY |
| `minecraft:cocoa[age=1,facing=east]` | 0 | 0 | T | F | EMPTY |
| `minecraft:cocoa[age=1,facing=north]` | 0 | 0 | T | F | EMPTY |
| `minecraft:cocoa[age=1,facing=south]` | 0 | 0 | T | F | EMPTY |
| `minecraft:cocoa[age=1,facing=west]` | 0 | 0 | T | F | EMPTY |
| `minecraft:cocoa[age=2,facing=east]` | 0 | 0 | T | F | EMPTY |
| `minecraft:cocoa[age=2,facing=north]` | 0 | 0 | T | F | EMPTY |
| `minecraft:cocoa[age=2,facing=south]` | 0 | 0 | T | F | EMPTY |
| `minecraft:cocoa[age=2,facing=west]` | 0 | 0 | T | F | EMPTY |
| `minecraft:copper_ore` | 0 | 15 | F | F | EMPTY |
| `minecraft:dandelion` | 0 | 0 | T | F | EMPTY |
| `minecraft:deepslate[axis=y]` | 0 | 15 | F | F | EMPTY |
| `minecraft:deepslate_coal_ore` | 0 | 15 | F | F | EMPTY |
| `minecraft:deepslate_copper_ore` | 0 | 15 | F | F | EMPTY |
| `minecraft:deepslate_diamond_ore` | 0 | 15 | F | F | EMPTY |
| `minecraft:deepslate_gold_ore` | 0 | 15 | F | F | EMPTY |
| `minecraft:deepslate_iron_ore` | 0 | 15 | F | F | EMPTY |
| `minecraft:deepslate_lapis_ore` | 0 | 15 | F | F | EMPTY |
| `minecraft:deepslate_redstone_ore[lit=false]` | 0 | 15 | F | F | EMPTY |
| `minecraft:diamond_ore` | 0 | 15 | F | F | EMPTY |
| `minecraft:diorite` | 0 | 15 | F | F | EMPTY |
| `minecraft:dirt` | 0 | 15 | F | F | EMPTY |
| `minecraft:fern` | 0 | 0 | T | F | EMPTY |
| `minecraft:flowering_azalea` | 0 | 0 | T | F | EMPTY |
| `minecraft:flowering_azalea_leaves[distance=1,persistent=false,waterlogged=false]` | 0 | 1 | F | F | EMPTY |
| `minecraft:flowering_azalea_leaves[distance=2,persistent=false,waterlogged=false]` | 0 | 1 | F | F | EMPTY |
| `minecraft:flowering_azalea_leaves[distance=3,persistent=false,waterlogged=false]` | 0 | 1 | F | F | EMPTY |
| `minecraft:flowering_azalea_leaves[distance=4,persistent=false,waterlogged=false]` | 0 | 1 | F | F | EMPTY |
| `minecraft:glow_lichen[down=false,east=false,north=false,south=false,up=false,waterlogged=false,west=true]` | 7 | 0 | T | F | EMPTY |
| `minecraft:glow_lichen[down=false,east=false,north=false,south=false,up=false,waterlogged=true,west=true]` | 7 | 1 | F | F | EMPTY |
| `minecraft:glow_lichen[down=false,east=false,north=false,south=false,up=true,waterlogged=false,west=false]` | 7 | 0 | T | F | EMPTY |
| `minecraft:glow_lichen[down=false,east=false,north=false,south=false,up=true,waterlogged=true,west=false]` | 7 | 1 | F | F | EMPTY |
| `minecraft:glow_lichen[down=false,east=false,north=false,south=true,up=false,waterlogged=false,west=false]` | 7 | 0 | T | F | EMPTY |
| `minecraft:glow_lichen[down=false,east=false,north=false,south=true,up=false,waterlogged=true,west=false]` | 7 | 1 | F | F | EMPTY |
| `minecraft:glow_lichen[down=false,east=false,north=false,south=true,up=false,waterlogged=true,west=true]` | 7 | 1 | F | F | EMPTY |
| `minecraft:glow_lichen[down=false,east=false,north=true,south=false,up=false,waterlogged=false,west=false]` | 7 | 0 | T | F | EMPTY |
| `minecraft:glow_lichen[down=false,east=false,north=true,south=false,up=false,waterlogged=true,west=false]` | 7 | 1 | F | F | EMPTY |
| `minecraft:glow_lichen[down=false,east=true,north=false,south=false,up=false,waterlogged=false,west=false]` | 7 | 0 | T | F | EMPTY |
| `minecraft:glow_lichen[down=false,east=true,north=false,south=false,up=true,waterlogged=false,west=false]` | 7 | 0 | T | F | EMPTY |
| `minecraft:glow_lichen[down=false,east=true,north=false,south=false,up=true,waterlogged=true,west=false]` | 7 | 1 | F | F | EMPTY |
| `minecraft:glow_lichen[down=true,east=false,north=false,south=false,up=false,waterlogged=false,west=false]` | 7 | 0 | T | F | EMPTY |
| `minecraft:glow_lichen[down=true,east=false,north=false,south=false,up=false,waterlogged=false,west=true]` | 7 | 0 | T | F | EMPTY |
| `minecraft:glow_lichen[down=true,east=false,north=false,south=false,up=false,waterlogged=true,west=false]` | 7 | 1 | F | F | EMPTY |
| `minecraft:glow_lichen[down=true,east=false,north=false,south=true,up=false,waterlogged=false,west=false]` | 7 | 0 | T | F | EMPTY |
| `minecraft:glow_lichen[down=true,east=true,north=false,south=false,up=false,waterlogged=false,west=false]` | 7 | 0 | T | F | EMPTY |
| `minecraft:gold_ore` | 0 | 15 | F | F | EMPTY |
| `minecraft:granite` | 0 | 15 | F | F | EMPTY |
| `minecraft:grass_block[snowy=false]` | 0 | 15 | F | F | EMPTY |
| `minecraft:gravel` | 0 | 15 | F | F | EMPTY |
| `minecraft:hanging_roots[waterlogged=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:iron_ore` | 0 | 15 | F | F | EMPTY |
| `minecraft:jungle_leaves[distance=1..6,persistent=false,waterlogged=false]` (6 states) | 0 | 1 | F | F | EMPTY |
| `minecraft:jungle_log[axis=x|y|z]` (3 states) | 0 | 15 | F | F | EMPTY |
| `minecraft:lapis_ore` | 0 | 15 | F | F | EMPTY |
| `minecraft:lava[level=0]` (and all levels 1..15) | 15 | 1 | F | F | EMPTY |
| `minecraft:magma_block` | 3 | 15 | F | F | EMPTY |
| `minecraft:melon` | 0 | 15 | F | F | EMPTY |
| `minecraft:moss_block` | 0 | 15 | F | F | EMPTY |
| `minecraft:moss_carpet` | 0 | 0 | T | F | EMPTY |
| `minecraft:mossy_cobblestone` | 0 | 15 | F | F | EMPTY |
| `minecraft:oak_leaves[distance=1..6,persistent=false,waterlogged=false]` (6 states) | 0 | 1 | F | F | EMPTY |
| `minecraft:oak_log[axis=x|y|z]` (3 states) | 0 | 15 | F | F | EMPTY |
| `minecraft:poppy` | 0 | 0 | T | F | EMPTY |
| `minecraft:redstone_ore[lit=false]` | 0 | 15 | F | F | EMPTY |
| `minecraft:rooted_dirt` | 0 | 15 | F | F | EMPTY |
| `minecraft:sand` | 0 | 15 | F | F | EMPTY |
| `minecraft:sandstone` | 0 | 15 | F | F | EMPTY |
| `minecraft:short_grass` | 0 | 0 | T | F | EMPTY |
| `minecraft:small_dripleaf[facing=east,half=lower,waterlogged=true]` | 0 | 1 | F | F | EMPTY |
| `minecraft:small_dripleaf[facing=east,half=upper,waterlogged=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:small_dripleaf[facing=north,half=lower,waterlogged=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:small_dripleaf[facing=north,half=lower,waterlogged=true]` | 0 | 1 | F | F | EMPTY |
| `minecraft:small_dripleaf[facing=north,half=upper,waterlogged=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:small_dripleaf[facing=south,half=lower,waterlogged=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:small_dripleaf[facing=south,half=lower,waterlogged=true]` | 0 | 1 | F | F | EMPTY |
| `minecraft:small_dripleaf[facing=south,half=upper,waterlogged=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:small_dripleaf[facing=west,half=lower,waterlogged=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:small_dripleaf[facing=west,half=lower,waterlogged=true]` | 0 | 1 | F | F | EMPTY |
| `minecraft:small_dripleaf[facing=west,half=upper,waterlogged=false]` | 0 | 0 | T | F | EMPTY |
| `minecraft:spawner` | 0 | 1 | F | F | EMPTY |
| `minecraft:spore_blossom` | 0 | 0 | T | F | EMPTY |
| `minecraft:stone` | 0 | 15 | F | F | EMPTY |
| `minecraft:tall_grass[half=lower]` | 0 | 0 | T | F | EMPTY |
| `minecraft:tall_grass[half=upper]` | 0 | 0 | T | F | EMPTY |
| `minecraft:tuff` | 0 | 15 | F | F | EMPTY |
| `minecraft:vine[...]` (all 5 palette face combos) | 0 | 0 | T | F | EMPTY |
| `minecraft:water[level=0]` (and all levels 1..15, incl. falling 8..15) | 0 | 1 | F | F | EMPTY |

Ring-feature additions:

| state | EM | LD | PSD | USO | LSHAPE |
|---|---|---|---|---|---|
| `minecraft:budding_amethyst` | 0 | 15 | F | F | EMPTY |
| `minecraft:amethyst_block` | 0 | 15 | F | F | EMPTY |
| `minecraft:calcite` | 0 | 15 | F | F | EMPTY |
| `minecraft:smooth_basalt` | 0 | 15 | F | F | EMPTY |
| `minecraft:amethyst_cluster[facing=*,waterlogged=false]` (6 facings) | 5 | 0 | T | F | EMPTY |
| `minecraft:amethyst_cluster[facing=*,waterlogged=true]` (6 facings) | 5 | 1 | F | F | EMPTY |
| `minecraft:large_amethyst_bud[facing=*,waterlogged=false]` | 4 | 0 | T | F | EMPTY |
| `minecraft:large_amethyst_bud[facing=*,waterlogged=true]` | 4 | 1 | F | F | EMPTY |
| `minecraft:medium_amethyst_bud[facing=*,waterlogged=false]` | 2 | 0 | T | F | EMPTY |
| `minecraft:medium_amethyst_bud[facing=*,waterlogged=true]` | 2 | 1 | F | F | EMPTY |
| `minecraft:small_amethyst_bud[facing=*,waterlogged=false]` | 1 | 0 | T | F | EMPTY |
| `minecraft:small_amethyst_bud[facing=*,waterlogged=true]` | 1 | 1 | F | F | EMPTY |
| `minecraft:seagrass` | 0 | 1 | F | F | EMPTY |
| `minecraft:tall_seagrass[half=lower]` | 0 | 1 | F | F | EMPTY |
| `minecraft:tall_seagrass[half=upper]` | 0 | 1 | F | F | EMPTY |
| `minecraft:sugar_cane[age=*]` | 0 | 0 | T | F | EMPTY |
| `minecraft:bush` | 0 | 0 | T | F | EMPTY |
| `minecraft:firefly_bush` | 2 | 0 | T | F | EMPTY |
| `minecraft:snow[layers=1]` | 0 | 0 | T | **T** | **SNOW1** |

Seagrass/tall_seagrass: both `getFluidState()` overrides return `Fluids.WATER.getSource(false)`
unconditionally (SeagrassBlock/TallSeagrassBlock bytecode) → fluid never empty → PSD false →
LD 1 via the VegetationBlock PSD override.

## 6. Distinct occlusion shapes (light-effective)

Only two matter to the light engine for this entire set:

* `EMPTY` — every state except snow. Includes all full solid cubes (USO=false →
  `LightEngine.isEmptyShape` short-circuits) and non-occluding shapes. In C:
  `shapeOccludes(a,b,dir)` is `false` whenever neither endpoint is snow[layers=1..7];
  `getLightDampeningInto` never adds the shape-16 term.
* `SNOW1` — `minecraft:snow[layers=1]`: canOcclude=true (registration has `forceSolidOff()`
  but NO `noOcclusion`/`noCollision`), USO=true (SnowLayerBlock override `iconst_1`).
  `occlusionShape = getShape = SHAPES[1]`. `SnowLayerBlock.<clinit>`:
  `SHAPES = Block.boxes(8, i -> Block.column(16.0, 0.0, i*2))`
  (bipush 8; InvokeDynamic bsm#1 → `lambda$static$0`: `Block.column(16.0d, 0.0d, i*2)`), and
  `Block.column(sx, minY, maxY)` = `box(8-sx/2, minY, 8-sx/2, 8+sx/2, maxY, 8+sx/2)`
  → SHAPES[layers] = `box(0, 0, 0, 16, 2*layers, 16)` (1/16-units). For layers=1
  (y ∈ [0, 2/16)):
  - `getFaceOcclusionShape(DOWN)` = full 16×16 face (shape flush with y=0) → occludes any face.
  - `getFaceOcclusionShape(UP)` = empty (shape does not reach y=16).
  - `NORTH/SOUTH/EAST/WEST` = 16-wide × 2-tall strip at the bottom of the face (non-empty,
    NOT face-filling → `faceShapeOccludes(strip, empty)` = false).
  Snow generally: layers 2..7 analogous (bottom slab height 2·layers); layers=8 → full cube →
  `solidRender=true` → LD 15 and FULL_BLOCK_OCCLUSION_SHAPES.

Shapes that exist in `occlusionShapesByFace` caches but are **invisible to lighting** (USO=false):
chest `Block.column(14,0,14)` = box(1,0,1,15,14,15) (ChestBlock.<clinit> bc 29–39), moss_carpet
`Block.column(16,0,1)` = box(0,0,0,16,1,16) (CarpetBlock.<clinit> bc 11–19), big_dripleaf pad.
Do NOT implement these for light.

## 7. Direct answers

* **Does `minecraft:spawner` emit light in 26.2?** NO. Registration segment (Blocks clinit bc
  6721–6765): `of().mapColor(STONE).instrument(BASEDRUM).requiresCorrectToolForDrops()
  .strength(5.0f).sound(SPAWNER).noOcclusion()` — no `lightLevel` call. Older versions' lightLevel
  is gone. Its light tuple: EM 0, LD 1 (no getShape override anywhere in
  SpawnerBlock/BaseEntityBlock → default full-cube getShape → PSD false; canOcclude=false via
  noOcclusion → solidRender false → LD = 1).
* **Does `minecraft:chest` have nontrivial occlusion?** For lighting, NO. canOcclude=true and
  its occlusionShape is the non-full box(1,0,1,15,14,15), but USO=false, so
  `LightEngine.isEmptyShape(chest)=true`. Light-wise chest is EM 0, LD 0 (PSD true: shape not
  full, waterlogged=false).
* **Does `minecraft:melon`?** Trivial: plain `Block` (registered via
  `register(BlockItemId, Properties)`, no factory), full cube, canOcclude=true → LD 15.

## 8. Consistency with empirical anchors

* Anchor (1) (08 `light_block` all zeros): the palette **does** contain emitters inside the grid
  chunks (glow_lichen EM 7, cave_vines[berries=true] EM 14, potentially magma). Zero block light
  at 08 therefore means stage 08 does not run block-light propagation — emission from this table
  is consumed by stage 09 (`propagateLightSources`). No contradiction with the tables; it pins
  *when* the EM column is applied, not *what* it is.
* Anchor (2) (08 `light_sky` rows all-0 or all-f, f-region from y=112 uniformly, independent of
  the 79..92 surface): consistent — nothing in this table forces per-column variation at 08;
  the uniform section-aligned boundary is a property of SkyLightSectionStorage/initialize
  semantics (lane B1/B2), not of the per-state values. Note that the *only* per-state inputs the
  sky path can see are LD (via `isEdgeOccluded`'s `getLightDampening() != 0`) and the SNOW1
  face shapes; PSD is never read by the engine at runtime.
* Anchors (3)/(4) (order-independence of 08, order-dependence of 09): orthogonal to this lane.

## 9. Items not fully pinned / caveats

* Occlusion face geometry was derived from `VoxelShape.getFaceShape` semantics (slice of the
  shape at the face plane); the exact `Shapes.faceShapeOccludes` / `mergedFaceOccludes`
  bit-logic is lane B2/B3 territory. For this palette only `(full-face, X)` and
  `(strip/empty, X)` cases occur, where full-face → occludes, everything else → not.
* `SHAPES[0]` for snow (zero-height box from `column(16,0,0)`) is unreachable (layers ∈ 1..8).
* Water/lava level>0 states were included generically (LiquidBlock has no state-dependent light
  behavior; the lava emission lambda `lambda$static$13` ignores the state). If springs write
  `falling` water it is the same LiquidBlock (level 8..15) → same row.
* `minecraft:glow_lichen` with all six faces false (not in palette, placeable only transiently)
  would be EM 0 per `hasAnyFace`.
* Everything else in the table is pinned directly from bytecode; no rows are guesses.
