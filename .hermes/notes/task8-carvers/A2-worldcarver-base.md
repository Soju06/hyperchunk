# A2 — `net.minecraft.world.level.levelgen.carver.WorldCarver` base class (MC 26.2, unobfuscated server bytecode)

Source of truth: `javap -p -c -constants` (and `javap -v` for BootstrapMethods) against
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server`. All pseudocode is a 1:1 reconstruction
from bytecode. No vanilla-source guessing.

Companion notes: A1 (orchestration / `applyCarvers` — who builds the Aquifer/CarvingMask and calls
`ConfiguredWorldCarver.carve`), A3 (CaveWorldCarver — supplies the actual x/y/z/radius args and the
skip-checker lambda), A4 (CanyonWorldCarver), A5 (CarvingContext internals + `topMaterial` +
`Aquifer.computeSubstance`), A6 (CarvingMask bit layout + ProtoChunk.setBlockState flag semantics),
A7 (Mth.floor, RandomSource). This note covers the ABSTRACT BASE where block mutation happens:
`WorldCarver`, its `$CarveSkipChecker` inner interface, `CarverConfiguration`, `CarverDebugSettings`,
and the `overworld_carver_replaceables` tag.

Class files: `WorldCarver.class` + `WorldCarver$CarveSkipChecker.class` (functional interface).
No other inner classes.

**RNG: the base class makes ZERO RandomSource draws.** `carveEllipsoid`/`carveBlock`/`getCarveState`/
`canReach` are fully deterministic given their arguments. Every `nextFloat`/`nextInt` in the carver
stack lives in the subclasses (A3/A4). The only nondeterminism entry points here are the
`Aquifer.computeSubstance` call (A5, noise-driven) and `CarvingContext.topMaterial` (A5, surface rules).

---

## 1. Class shape, static fields, `static {}` order

```java
public abstract class WorldCarver<C extends CarverConfiguration> {
    // field declaration order:
    public static final WorldCarver<CaveCarverConfiguration>   CAVE;         // (registered "cave")
    public static final WorldCarver<CaveCarverConfiguration>   NETHER_CAVE;  // (registered "nether_cave")
    public static final WorldCarver<CanyonCarverConfiguration> CANYON;       // (registered "canyon")
    protected static final BlockState AIR;        // Blocks.AIR.defaultBlockState()
    protected static final BlockState CAVE_AIR;   // Blocks.CAVE_AIR.defaultBlockState()
    protected static final FluidState WATER;      // Fluids.WATER.defaultFluidState()
    protected static final FluidState LAVA;       // Fluids.LAVA.defaultFluidState()
    protected Set<Fluid> liquids;                 // instance, NOT final — see §1.2
    private final MapCodec<ConfiguredWorldCarver<C>> configuredCodec;
}
```

`static {}` putstatic order (authoritative):

1. `CAVE = register("cave", new CaveWorldCarver(CaveCarverConfiguration.CODEC))`
2. `NETHER_CAVE = register("nether_cave", new NetherWorldCarver(CaveCarverConfiguration.CODEC))`
3. `CANYON = register("canyon", new CanyonWorldCarver(CanyonCarverConfiguration.CODEC))`
4. `AIR = Blocks.AIR.defaultBlockState()`
5. `CAVE_AIR = Blocks.CAVE_AIR.defaultBlockState()`
6. `WATER = Fluids.WATER.defaultFluidState()`
7. `LAVA = Fluids.LAVA.defaultFluidState()`

`register` = `Registry.register(BuiltInRegistries.CARVER, name, carver)`. So registry IDs are
cave=0, nether_cave=1, canyon=2 (insertion order, if numeric IDs ever matter).

Note the subclass constructors run BEFORE the AIR/CAVE_AIR/WATER/LAVA constants are assigned
(harmless: constructors don't read them; the constants are only read at carve time).

### 1.1 Constructor

Descriptor: `WorldCarver(Codec<C>)`.

```java
public WorldCarver(Codec<C> configCodec) {
    this.liquids = ImmutableSet.of(Fluids.WATER);                              // (1)
    this.configuredCodec = configCodec.fieldOf("config")                       // (2)
        .xmap(this::configured, ConfiguredWorldCarver::config);
    // BootstrapMethods verified: indy#0 = this::configured, indy#1 = ConfiguredWorldCarver::config
}
```

`configured(C c)` = `new ConfiguredWorldCarver<>(this, c)`. `configuredCodec()` returns the field.
`getRange()` returns hard-coded `4` (chunk radius scanned by the orchestrator for carver starts — A1).

### 1.2 `liquids` is a DEAD field in 26.2

- Written here to `{WATER}`; `NetherWorldCarver.<init>` overwrites it with
  `ImmutableSet.of(Fluids.LAVA, Fluids.WATER)`.
- **No `getfield liquids` exists anywhere**: `grep -rla 'liquids' net/minecraft/` over the whole
  class tree matches only `WorldCarver.class` and `NetherWorldCarver.class`, and both occurrences
  are `putfield`. No `hasDisallowedLiquid`-style method survives.
- 26.2 delta vs 1.21 folklore: the aquifer path fully replaced liquid-adjacency checks; the field is
  vestigial. C impl can ignore it.

### 1.3 Abstract methods (implemented by A3/A4)

```java
public abstract boolean carve(CarvingContext, C, ChunkAccess,
    Function<BlockPos, Holder<Biome>>, RandomSource, Aquifer, ChunkPos, CarvingMask);
public abstract boolean isStartChunk(C, RandomSource);
```

### 1.4 `WorldCarver$CarveSkipChecker`

```java
public interface WorldCarver$CarveSkipChecker {
    boolean shouldSkip(CarvingContext context, double relX, double relY, double relZ, int y);
}
```
Exactly one abstract method, no defaults, no fields. Implemented as lambdas in A3/A4.
`relX/relY/relZ` are the ellipsoid-relative coordinates computed in §2 (already divided by radius);
`y` is the absolute block Y.

---

## 2. `carveEllipsoid` — 1:1 reconstruction

Descriptor:
```
protected boolean carveEllipsoid(CarvingContext ctx, C config, ChunkAccess chunk,
    Function<BlockPos, Holder<Biome>> biomeGetter, Aquifer aquifer,
    double x, double y, double z,            // ellipsoid center (absolute, doubles)
    double hRadius, double vRadius,          // horizontal / vertical radii (doubles)
    CarvingMask mask, WorldCarver$CarveSkipChecker skipChecker)
```
(local slots: x=6, y=8, z=10, hRadius=12, vRadius=14, mask=16, skipChecker=17.)

```java
{
    ChunkPos chunkPos = chunk.getPos();
    double midX = (double)chunkPos.getMiddleBlockX();       // (cpX<<4)+8, i2d
    double midZ = (double)chunkPos.getMiddleBlockZ();       // (cpZ<<4)+8, i2d
    double reach = 16.0 + hRadius * 2.0;                    // all double math
    if (Math.abs(x - midX) > reach || Math.abs(z - midZ) > reach) {
        return false;                                       // dcmpl → NaN compares as NOT-greater (passes)
    }
    int minBlockX = chunkPos.getMinBlockX();                // cpX << 4
    int minBlockZ = chunkPos.getMinBlockZ();                // cpZ << 4

    // bounds computed IN THIS ORDER:
    int minX0 = Math.max(Mth.floor(x - hRadius) - minBlockX - 1, 0);                    // (1)
    int maxX0 = Math.min(Mth.floor(x + hRadius) - minBlockX, 15);                       // (2)
    int minY  = Math.max(Mth.floor(y - vRadius) - 1, ctx.getMinGenY() + 1);             // (3)
    int topPad = chunk.isUpgrading() ? 0 : 7;                                           // (4)
    int maxY  = Math.min(Mth.floor(y + vRadius) + 1,
                         ctx.getMinGenY() + ctx.getGenDepth() - 1 - topPad);            // (5)
    int minZ0 = Math.max(Mth.floor(z - hRadius) - minBlockZ - 1, 0);                    // (6)
    int maxZ0 = Math.min(Mth.floor(z + hRadius) - minBlockZ, 15);                       // (7)

    boolean carved = false;
    BlockPos.MutableBlockPos pos      = new BlockPos.MutableBlockPos();  // carve target
    BlockPos.MutableBlockPos checkPos = new BlockPos.MutableBlockPos();  // below-pos scratch

    for (int x0 = minX0; x0 <= maxX0; ++x0) {                       // X outer, ascending, INCLUSIVE
        int blockX = chunkPos.getBlockX(x0);                        // (cpX<<4)+x0
        double dx = ((double)blockX + 0.5 - x) / hRadius;           // double throughout
        for (int z0 = minZ0; z0 <= maxZ0; ++z0) {                   // Z middle, ascending, INCLUSIVE
            int blockZ = chunkPos.getBlockZ(z0);
            double dz = ((double)blockZ + 0.5 - z) / hRadius;       // NOTE: divided by hRadius (same as x)
            if (!(dx * dx + dz * dz < 1.0)) continue;               // dcmpl+iflt: proceed if < 1.0 OR NaN — !(x<1.0) 형태는 NaN 극성이 뒤집힌다 (VERIFICATION.md 리뷰 정정: sum >= 1.0 continue 가 정확)
            MutableBoolean reachedSurface = new MutableBoolean(false);  // fresh PER (x0,z0) COLUMN
            for (int y0 = maxY; y0 > minY; --y0) {                  // Y inner, DESCENDING,
                                                                    // maxY INCLUSIVE, minY EXCLUSIVE
                double dy = ((double)y0 - 0.5 - y) / vRadius;       // y sampled at y0 - 0.5 (MINUS!)
                if (skipChecker.shouldSkip(ctx, dx, dy, dz, y0)) continue;
                if (mask.get(x0, y0, z0) && !isDebugEnabled(config)) continue;
                mask.set(x0, y0, z0);                               // set BEFORE carveBlock,
                                                                    // regardless of carveBlock result
                pos.set(blockX, y0, blockZ);
                carved |= carveBlock(ctx, config, chunk, biomeGetter, mask,
                                     pos, checkPos, aquifer, reachedSurface);
            }
        }
    }
    return carved;
}
```

Load-bearing details:

- **Iteration order: x ascending → z ascending → y DESCENDING** (`iinc 46, -1`), with the y loop
  condition `if_icmple` against minY, i.e. y0 runs `maxY, maxY-1, …, minY+1`. `minY` itself is never
  visited; `maxY` is.
- The x/z half-block offset is `+0.5`; the y offset is `-0.5` (`y0 - 0.5 - y`). Exact bytecode:
  x/z use `dadd` of `0.5`, y uses `dsub` of `0.5`.
- The 2-D disc pre-check `dx²+dz² < 1.0` gates the whole column; the checker sees candidate blocks
  even when `dy` would put them outside the ellipsoid — vertical clipping is entirely the
  skip-checker's job (A3 §cave-skipper does `dx²+dy²+dz² ≥ 1.0` etc.).
- `dz` divides by **hRadius**, not a separate z radius — the ellipsoid is a spheroid (circular in XZ).
- `Mth.floor` is the standard fast floor (A7). All four horizontal bounds use it on double sums
  before int subtraction; min bounds get an extra `-1`, maxX0/maxZ0 do NOT get `+1` (clamped 0..15),
  maxY gets `+1`, minY gets `-1`.
- `topPad`: `chunk.isUpgrading()` ⇔ `chunk.getBelowZeroRetrogen() != null` (ChunkAccess bytecode) —
  i.e. pre-1.18 world upgrade chunks carve all the way to the top; normal generation stops 7 below
  the generation ceiling: `maxY ≤ minGenY + genDepth - 8`.
- Mask ordering: `mask.get` is consulted AFTER the skip-checker (skipped blocks never touch the
  mask), and `mask.set` happens BEFORE `carveBlock` and is unconditional at that point — a block
  whose state is not replaceable still gets its mask bit set. Mask args are
  `(x0 ∈ [0,15], y0 = absolute block Y, z0 ∈ [0,15])`; internal y-offset handling is A6 §CarvingMask.
- The early-out `reach` test compares against the CENTER-of-chunk block coords `(cpX*16+8, cpZ*16+8)`
  as doubles; `Math.abs(double)` (JDK), threshold `16.0 + 2.0*hRadius`, strict `>` rejects.
- Return value: `ior`-accumulated "any block carved".

26.2 delta: none structurally vs the 1.21-era shape, but do not trust folklore for the bound
constants — the exact `-1/+1/15/7` pattern above is bytecode-verified.

---

## 3. `carveBlock` — 1:1 reconstruction (the grass fixup lives here)

Descriptor:
```
protected boolean carveBlock(CarvingContext ctx, C config, ChunkAccess chunk,
    Function<BlockPos, Holder<Biome>> biomeGetter, CarvingMask mask,
    BlockPos.MutableBlockPos pos, BlockPos.MutableBlockPos checkPos,
    Aquifer aquifer, MutableBoolean reachedSurface)
```
NOTE: the `mask` parameter (slot 5) is **never loaded in the method body** — dead parameter.

```java
{
    BlockState state = chunk.getBlockState(pos);                              // (1)
    if (state.is(Blocks.GRASS_BLOCK) || state.is(Blocks.MYCELIUM)) {          // (2)
        reachedSurface.setTrue();       // set BEFORE replaceability check — sticks for the column
    }
    if (!canReplaceBlock(config, state) && !isDebugEnabled(config)) {         // (3)
        return false;                   // (reachedSurface may already be true!)
    }
    BlockState carveState = getCarveState(ctx, config, pos, aquifer);         // (4) §4
    if (carveState == null) return false;                                     // (5)

    chunk.setBlockState(pos, carveState);                                     // (6) 2-arg → flags = 3
    if (aquifer.shouldScheduleFluidUpdate() && !carveState.getFluidState().isEmpty()) {
        chunk.markPosForPostProcessing(pos);                                  // (7)
    }
    if (reachedSurface.isTrue()) {                                            // (8) grass fixup
        checkPos.setWithOffset(pos, Direction.DOWN);                          //     pos.below()
        if (chunk.getBlockState(checkPos).is(Blocks.DIRT)) {                  //     EXACTLY minecraft:dirt
            ctx.topMaterial(biomeGetter, chunk, checkPos,
                            /*hasFluid=*/ !carveState.getFluidState().isEmpty())   // (A5 §topMaterial)
               .ifPresent(top -> {                                            // lambda$carveBlock$0:
                   chunk.setBlockState(checkPos, top);                        //   2-arg → flags = 3
                   if (!top.getFluidState().isEmpty()) {
                       chunk.markPosForPostProcessing(checkPos);
                   }
               });
        }
    }
    return true;                                                              // (9)
}
```

Exact semantics, in order:

1. **Surface flag (2)**: checked against the PRE-carve state of the current block; blocks:
   `GRASS_BLOCK` or `MYCELIUM` only (both are in the replaceable tag via `#grass_blocks`, so in
   vanilla data they also get carved). The flag is set even when step (3)/(5) then returns false.
   The `MutableBoolean` is per-(x,z)-column (created in §2), so once true, EVERY subsequently carved
   block lower in the same column (y descends) runs the fixup check (8).
2. **Replaceability (3)**: `canReplaceBlock(config, state)` = `state.is(config.replaceable)`
   (HolderSet membership; §6 tag). Debug mode bypasses the check entirely.
3. **setBlockState (6)**: 26.2 delta — `ChunkAccess.setBlockState(BlockPos, BlockState)` is a new
   2-arg convenience that delegates to `setBlockState(pos, state, 3)` (`iconst_3`; verified in
   ChunkAccess bytecode). 1.21 had `(pos, state, boolean isMoving=false)`. What flag bits 3 mean for
   ProtoChunk is A6's ground (OPEN there; ProtoChunk likely ignores flags for worldgen purposes).
4. **Post-processing (7)**: only when the aquifer says `shouldScheduleFluidUpdate()` (A5 — the
   real aquifer returns whether it placed fluid-capable substance; disabled aquifer false) AND the
   placed state actually has a fluid (water/lava). `markPosForPostProcessing` = fluid tick at
   chunk promotion (A6).
5. **Grass fixup (8)** — answers the assignment question: YES, 26.2 still does surface repair inside
   the carver, via `CarvingContext.topMaterial`. Exact conditions, all required:
   - a `GRASS_BLOCK`/`MYCELIUM` was *seen* at or above the current y in this column during this
     ellipsoid (flag), and
   - the current block was actually carved (steps 3–6 passed), and
   - the block directly BELOW the just-carved pos `is(Blocks.DIRT)` — the plain `minecraft:dirt`
     block ONLY (not coarse dirt, rooted dirt, podzol, mud…), and
   - `topMaterial` returns a non-empty Optional.
   Then the DIRT below is overwritten with the surface-rule result and fluid-post-processed if it
   carries fluid. Only `checkPos = pos.below()` is examined — there is no "checkerAbove"; the
   above-check folklore does not exist in 26.2 bytecode.
6. **Biome-data-driven flow-through**: `topMaterial(biomeGetter, chunk, checkPos, hasFluid)` runs the
   dimension SURFACE RULE at that position (CarvingContext holds `SurfaceRules$RuleSource` — A5).
   So any biome whose surface rule emits a custom block (e.g. a sulfur-surface rule for
   `sulfur_caves`) CAN place that block through this path — but only ever on top of an exact
   `minecraft:dirt` block under a carved grass/mycelium column. There is no other biome-driven
   material hook in the base carver.

---

## 4. `getCarveState` + debug state mapping

```java
private BlockState getCarveState(CarvingContext ctx, C config, BlockPos pos, Aquifer aquifer) {
    if (pos.getY() <= config.lavaLevel.resolveY(ctx)) {          // if_icmpgt inverted: y <= level
        return LAVA.createLegacyBlock();                          // lava block state (level fluid → block)
    }
    BlockState substance = aquifer.computeSubstance(
        new DensityFunction.SinglePointContext(pos.getX(), pos.getY(), pos.getZ()),
        0.0 /* dconst_0 — density argument is literally 0.0 */);  // (A5 §computeSubstance)
    if (substance == null) {
        return isDebugEnabled(config) ? config.debugSettings.getBarrierState() : null;
    }
    return isDebugEnabled(config) ? getDebugState(config, substance) : substance;
}
```

- LAVA floor: `y <= lavaLevel.resolveY(context)` (inclusive). `lavaLevel` is a `VerticalAnchor`
  from carver JSON (vanilla overworld carvers: `{"absolute": 8}` → y ≤ 8 becomes lava,
  bypassing the aquifer entirely). `resolveY` uses `CarvingContext extends WorldGenerationContext`.
- Everything else (air vs CAVE water vs preserved barrier) is the aquifer's decision at density 0.0.
  A `null` from the aquifer means "do not carve" (block preserved) — surfaces as `carveBlock`
  returning false.
- The base class never places `WATER`/`AIR`/`CAVE_AIR` constants itself in this path — despite the
  static fields existing, the actual carve substance comes exclusively from `LAVA.createLegacyBlock()`
  or `aquifer.computeSubstance`. (The AIR/CAVE_AIR/WATER statics are read by subclasses / the
  aquifer, A3/A5.)

```java
private static BlockState getDebugState(CarverConfiguration config, BlockState state) {
    if (state.is(Blocks.AIR))   return config.debugSettings.getAirState();
    if (state.is(Blocks.WATER)) {
        BlockState w = config.debugSettings.getWaterState();
        return w.hasProperty(BlockStateProperties.WATERLOGGED)
             ? w.setValue(BlockStateProperties.WATERLOGGED, true) : w;
    }
    if (state.is(Blocks.LAVA))  return config.debugSettings.getLavaState();
    return state;
}

private static boolean isDebugEnabled(CarverConfiguration config) {
    return SharedConstants.DEBUG_CARVERS || config.debugSettings.isDebugMode();
}
```

`SharedConstants.DEBUG_CARVERS` is NOT a compile-time false: it's `debugFlag("CARVERS")`, gated on
`DEBUG_ENABLED` which reads a JVM system property (`booleanProperty(prefixDebugFlagName("ENABLED"))`).
Production servers: false. C impl: treat `isDebugEnabled` as `config.debugSettings.debug_mode`
(vanilla data: always default false ⇒ constant false).

---

## 5. `canReach` — chunk-relative distance cull (used by A3/A4 tunnel loops)

```java
protected static boolean canReach(ChunkPos chunkPos, double x, double z,
                                  int branchIndex, int branchCount, float width) {
    double midX = (double)chunkPos.getMiddleBlockX();
    double midZ = (double)chunkPos.getMiddleBlockZ();
    double dx = x - midX;                       // double
    double dz = z - midZ;                       // double
    double remaining = (double)(branchCount - branchIndex);   // int sub, then i2d
    double r = (double)(width + 2.0F + 16.0F);  // *** FLOAT adds: (width + 2.0f) + 16.0f, THEN f2d ***
    return dx * dx + dz * dz - remaining * remaining <= r * r; // dcmpg; NaN → false
}
```

Float-typing trap: `width + 2.0F + 16.0F` is computed in 32-bit float (`fadd`, `fadd`, `f2d`) —
NOT `(double)width + 18.0`. For odd `width` values the rounding differs; reproduce with
`(double)((float)((width + 2.0f) + 16.0f))`. Comparison is `<=` via `dcmpg; ifgt` (NaN rejects).

---

## 6. `CarverConfiguration` (+ superclass) — fields and codec

Plain class (NOT a record), `extends ProbabilityFeatureConfiguration implements FeatureConfiguration`.

```java
public class CarverConfiguration extends ProbabilityFeatureConfiguration {
    public static final MapCodec<CarverConfiguration> CODEC;
    // instance fields, declaration/putfield order after super(probability):
    public final HeightProvider y;                    // (1)
    public final FloatProvider yScale;                // (2)
    public final VerticalAnchor lavaLevel;            // (3)
    public final CarverDebugSettings debugSettings;   // (4)
    public final HolderSet<Block> replaceable;        // (5)
}
// superclass: public final float probability;  codec field "probability", Codec.floatRange(0.0f, 1.0f)
```

CODEC (RecordCodecBuilder.mapCodec, group order = JSON keys):
1. `Codec.floatRange(0.0F, 1.0F).fieldOf("probability")` — REQUIRED
2. `HeightProvider.CODEC.fieldOf("y")` — REQUIRED
3. `FloatProviders.CODEC.fieldOf("yScale")` — REQUIRED (26.2 delta: static `FloatProviders.CODEC`
   registry-dispatch codec class, not an instance field on `FloatProvider`; key spelling is
   literally `"yScale"`, camelCase)
4. `VerticalAnchor.CODEC.fieldOf("lava_level")` — REQUIRED
5. `CarverDebugSettings.CODEC.optionalFieldOf("debug_settings", CarverDebugSettings.DEFAULT)`
6. `RegistryCodecs.homogeneousList(Registries.BLOCK).fieldOf("replaceable")` — REQUIRED; accepts
   `"#tag"` or inline list; vanilla data uses `"#minecraft:overworld_carver_replaceables"` (§8)

`CaveCarverConfiguration` / `CanyonCarverConfiguration` extend this (A3/A4 ground).

---

## 7. `CarverDebugSettings` — fields, DEFAULT, codec quirk

Plain class. Field/putfield order: `debugMode:boolean`, `airState`, `waterState`, `lavaState`,
`barrierState` (all `BlockState`). Private 5-arg ctor; three static `of(...)` overloads
(`of(bool,4 states)`, `of(4 states)` → debugMode=false, `of(bool, air)` → rest copied from DEFAULT).

`static {}`:
```java
DEFAULT = new CarverDebugSettings(false,
    Blocks.ACACIA_BUTTON.defaultBlockState(),                      // airState
    Blocks.CANDLE.defaultBlockState(),                             // waterState
    (Block)Blocks.STAINED_GLASS.orange()).defaultBlockState(),     // lavaState  (26.2: ColorCollection.orange(), cf. A1-task7 §1)
    Blocks.GLASS.defaultBlockState());                             // barrierState
CODEC = RecordCodecBuilder.create(...);
```

Codec fields — NOTE THE COPY-PASTE QUIRK, bytecode-verified: **all four block-state defaults call
`DEFAULT.getAirState()`** (offsets 28/52/76/100 each invoke `getAirState`):
1. `Codec.BOOL.optionalFieldOf("debug_mode", false)`
2. `BlockState.CODEC.optionalFieldOf("air_state", DEFAULT.getAirState())`
3. `BlockState.CODEC.optionalFieldOf("water_state", DEFAULT.getAirState())`   // sic — acacia_button
4. `BlockState.CODEC.optionalFieldOf("lava_state", DEFAULT.getAirState())`    // sic
5. `BlockState.CODEC.optionalFieldOf("barrier_state", DEFAULT.getAirState())` // sic

Irrelevant for parity (debug only, and vanilla worldgen JSON never sets `debug_settings`), but
recorded since it is what the bytecode says.

---

## 8. `data/minecraft/tags/block/overworld_carver_replaceables.json` — exact content

Raw values (`replace` absent ⇒ false):
```json
{ "values": [
    "#minecraft:base_stone_overworld", "#minecraft:substrate_overworld", "#minecraft:sand",
    "#minecraft:terracotta", "#minecraft:iron_ores", "#minecraft:copper_ores", "#minecraft:snow",
    "minecraft:water", "minecraft:gravel", "minecraft:suspicious_gravel",
    "minecraft:sandstone", "minecraft:red_sandstone", "minecraft:calcite", "minecraft:packed_ice",
    "minecraft:raw_iron_block", "minecraft:raw_copper_block",
    "minecraft:cinnabar", "minecraft:sulfur", "minecraft:potent_sulfur"
] }
```

Child tags expanded (all cited from the same tree, `data/minecraft/tags/block/*.json`):
- `#base_stone_overworld`: stone, granite, diorite, andesite, tuff, deepslate
- `#substrate_overworld` (26.2 delta — new indirection tag): `#dirt` (dirt, coarse_dirt, rooted_dirt),
  `#mud` (mud, muddy_mangrove_roots), `#moss_blocks` (moss_block, pale_moss_block),
  `#grass_blocks` (grass_block, podzol, mycelium)
- `#sand`: sand, red_sand, suspicious_sand
- `#terracotta`: terracotta + all 16 colored terracottas (white, orange, magenta, light_blue,
  yellow, lime, pink, gray, light_gray, cyan, purple, blue, brown, green, red, black)
- `#iron_ores`: iron_ore, deepslate_iron_ore
- `#copper_ores`: copper_ore, deepslate_copper_ore
- `#snow`: snow, snow_block, powder_snow

Fully flattened set (45 blocks): stone, granite, diorite, andesite, tuff, deepslate, dirt,
coarse_dirt, rooted_dirt, mud, muddy_mangrove_roots, moss_block, pale_moss_block, grass_block,
podzol, mycelium, sand, red_sand, suspicious_sand, terracotta, {16 colored terracottas}, iron_ore,
deepslate_iron_ore, copper_ore, deepslate_copper_ore, snow, snow_block, powder_snow, water, gravel,
suspicious_gravel, sandstone, red_sandstone, calcite, packed_ice, raw_iron_block, raw_copper_block,
cinnabar, sulfur, potent_sulfur.

26.2 delta: `cinnabar`, `sulfur`, `potent_sulfur` are new carver-replaceable blocks (new 26.x
blocks), and `suspicious_gravel` + the `#substrate_overworld` grouping are newer than 1.21 folklore.
`grass_block`/`mycelium` being replaceable is what lets the §3 fixup flag actually fire.

Load path: `config.replaceable` is deserialized per configured carver from
`data/minecraft/worldgen/configured_carver/{cave,cave_extra_underground,canyon}.json`, each of which
has `"replaceable": "#minecraft:overworld_carver_replaceables"` (contents of those JSONs = A3/A4
ground; the tag is shared by all three).

---

## 9. Surface-repair summary for the C port (assignment item 4)

- Only repair mechanism: §3 step (8). Trigger block set: {grass_block, mycelium} (pre-carve state of
  a carved-column block). Repair target: the single block directly below a carved block, iff it is
  exactly `minecraft:dirt`. Replacement: `SurfaceRules` result via `CarvingContext.topMaterial`
  with `useWaterHeight = carvedStateHasFluid` (A5 owns the rule-evaluation internals).
- Because the replacement comes from the biome-parameterized surface rule source, ANY biome's
  data-driven top material (including a hypothetical sulfur surface for `minecraft:sulfur_caves`)
  flows through this path with zero carver-code involvement. The carver itself hard-codes only
  GRASS_BLOCK / MYCELIUM / DIRT.
- Y-descending iteration means the fixup runs top-down within a column and can fire multiple times
  (each carved block below the first grass re-checks its own below-neighbor).

---

## 10. sulfur scan (assignment item)

- `strings` scan of `WorldCarver.class`, `WorldCarver$CarveSkipChecker.class`,
  `CarverConfiguration.class`, `CarverDebugSettings.class` for "sulfur" (case-insensitive): **no hits**.
  No carver code special-cases sulfur.
- BUT data-side: `overworld_carver_replaceables` tag includes `minecraft:sulfur` and
  `minecraft:potent_sulfur` (and `minecraft:cinnabar`) — sulfur blocks ARE carvable by the standard
  overworld carvers (§8), and sulfur-biome surface material can be re-placed by the dirt fixup
  (§9) purely via surface-rule data.

---

## 11. OPEN items

- OPEN: exact meaning of flags `3` in `ChunkAccess.setBlockState(pos, state, 3)` for
  ProtoChunk/LevelChunk during carving (A6 to confirm ProtoChunk ignores or interprets them; parity
  likely unaffected because ProtoChunk section writes are flag-independent — verify in A6).
- OPEN: `CarvingMask.get/set(x0, y0abs, z0)` internal y normalization (A6).
- OPEN: `VerticalAnchor.resolveY` variants (absolute/aboveBottom/belowTop) — bytecode not read here;
  vanilla carver JSONs use `absolute` (A1/A7 or config-extraction task to pin down).
- OPEN: `Aquifer.computeSubstance(ctx, 0.0)` semantics with density exactly 0.0 (A5).
- OPEN: `FloatProviders.CODEC` dispatch table (registry `float_provider_type`) — needed only if
  non-uniform providers appear in carver JSON (A7 value-providers ground).
- Presumed (not proven by exhaustive reflection audit): `liquids` field is dead — no getfield in the
  entire class tree (§1.2).
