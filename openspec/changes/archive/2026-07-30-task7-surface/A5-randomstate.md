# A5 — RandomState + wiring into the surface stage (MC 26.2, unobfuscated server bytecode)

Source of truth: `javap -p -c -constants` against `/home/ubuntu/projects/hyperchunk/tools/golden/work/server`.
All pseudocode below is a 1:1 reconstruction from bytecode; nothing is taken from vanilla-source memory.

Note on naming: 26.2 uses `net.minecraft.resources.Identifier` (the class formerly known as
`ResourceLocation`). `Identifier.DEFAULT_NAMESPACE = "minecraft"` (constant pool), and
`Identifier.toString()` is `namespace + ":" + path` (StringConcatFactory recipe `:`,
bootstrap #2, used by `toString()` via indy #101). So **every `fromHashOf` below hashes the full
`"minecraft:<path>"` string**.

---

## 1. `net.minecraft.world.level.levelgen.RandomState`

### 1.1 Fields (declaration order)

```java
private final PositionalRandomFactory random;
private final HolderGetter<NormalNoise.NoiseParameters> noises;
private final NoiseRouter router;
private final Climate.Sampler sampler;
private final SurfaceSystem surfaceSystem;          // <-- RandomState OWNS the SurfaceSystem in 26.2
private final PositionalRandomFactory aquiferRandom;
private final PositionalRandomFactory oreRandom;
private final Map<ResourceKey<NormalNoise.NoiseParameters>, NormalNoise> noiseIntances;   // sic — typo is in the bytecode
private final Map<Identifier, PositionalRandomFactory> positionalRandoms;
```

### 1.2 Static factories

```java
public static RandomState create(HolderGetter.Provider provider, ResourceKey<NoiseGeneratorSettings> settingsKey, long seed) {
    return create(
        (NoiseGeneratorSettings) provider.lookupOrThrow(Registries.NOISE_SETTINGS).getOrThrow(settingsKey).value(),
        provider.lookupOrThrow(Registries.NOISE),
        seed);
}

public static RandomState create(NoiseGeneratorSettings settings, HolderGetter<NormalNoise.NoiseParameters> noises, long seed) {
    return new RandomState(settings, noises, seed);
}
```

### 1.3 Private constructor — EXACT creation / fork order

Bytecode field-write order (putfield order is authoritative):

```java
private RandomState(NoiseGeneratorSettings settings, HolderGetter<NormalNoise.NoiseParameters> noises, long seed) {
    // (1) root positional factory
    this.random = settings.getRandomSource()          // WorldgenRandom.Algorithm enum
                          .newInstance(seed)          // LEGACY -> new LegacyRandomSource(seed)
                                                      // XOROSHIRO -> new XoroshiroRandomSource(seed)
                                                      //   (confirmed from Algorithm BootstrapMethods:
                                                      //    REF_newInvokeSpecial LegacyRandomSource.<init>(J) /
                                                      //    XoroshiroRandomSource.<init>(J))
                          .forkPositional();          // *** RNG: forkPositional() on the fresh source ***

    // (2)
    this.noises = noises;

    // (3) *** RNG: random.fromHashOf("minecraft:aquifer").forkPositional() ***
    this.aquiferRandom = this.random.fromHashOf(Identifier.withDefaultNamespace("aquifer")).forkPositional();

    // (4) *** RNG: random.fromHashOf("minecraft:ore").forkPositional() ***
    this.oreRandom = this.random.fromHashOf(Identifier.withDefaultNamespace("ore")).forkPositional();

    // (5)(6) caches — ConcurrentHashMap (thread-safe dedup)
    this.noiseIntances = new ConcurrentHashMap<>();
    this.positionalRandoms = new ConcurrentHashMap<>();

    // (7) SurfaceSystem is built HERE, BEFORE router mapAll.
    //     ctor descriptor: SurfaceSystem.<init>(RandomState, BlockState, int, PositionalRandomFactory)
    //     args: (this, settings.defaultBlock(), settings.seaLevel(), this.random)
    //     i.e. SurfaceSystem gets the ROOT positional factory `this.random`,
    //     NOT a "surface"-forked one. (Any per-noise forks happen inside SurfaceSystem
    //     via the RandomState reference / this.random — other cluster's scope.)
    this.surfaceSystem = new SurfaceSystem(this, settings.defaultBlock(), settings.seaLevel(), this.random);

    boolean useLegacyInit = settings.useLegacyRandomSource();   // local var 5, read AFTER surfaceSystem write

    // (8) router: mapAll with the noise-wiring visitor (this populates noiseIntances via getOrCreateNoise)
    this.router = settings.noiseRouter().mapAll(new NoiseWiringHelper(this, seed, useLegacyInit));   // inner class $1NoiseWiringHelper

    // (9) climate sampler: a second, "unwrapping" visitor (inner class $1)
    DensityFunction.Visitor visitor = new RandomState$1(this);
    this.sampler = new Climate.Sampler(
        this.router.temperature().mapAll(visitor),
        this.router.vegetation().mapAll(visitor),
        this.router.continents().mapAll(visitor),
        this.router.erosion().mapAll(visitor),
        this.router.depth().mapAll(visitor),
        this.router.ridges().mapAll(visitor),
        settings.spawnTarget());
}
```

Order summary (matters for any stateful RandomSource but NOT for positional hashing, which is stateless):
`random` → `noises` → `aquiferRandom` → `oreRandom` → maps → **`surfaceSystem`** → `useLegacyRandomSource()` read → `router` (NoiseWiringHelper mapAll) → `sampler`.

Call-signature note (not deep-dived here): for XOROSHIRO, `RandomSource.forkPositional()` on the freshly
seeded source is the only stateful consumption in step (1); `fromHashOf(...)` and `forkPositional()` on a
`fromHashOf` result operate on the string-hashed source.

### 1.4 `getOrCreateNoise` — dedup by ResourceKey, seed via fromHashOf(full key string)

```java
public NormalNoise getOrCreateNoise(ResourceKey<NormalNoise.NoiseParameters> key) {
    return this.noiseIntances.computeIfAbsent(key, k -> Noises.instantiate(this.noises, this.random, key));
    // lambda$getOrCreateNoise$0 captures the OUTER `key` (aload_1 = captured key), ignores the map-key param.
}
```

→ `Noises.instantiate` (see §3.2): `NormalNoise.create(this.random.fromHashOf(key.identifier()), params)`.
`fromHashOf(Identifier)` is a **default interface method** on `PositionalRandomFactory`:

```java
default RandomSource fromHashOf(Identifier id) { return fromHashOf(id.toString()); }   // "minecraft:<path>"
```

**Confirmed: dedup is per-ResourceKey in a ConcurrentHashMap; the hash string includes the `minecraft:` prefix.**

### 1.5 `getOrCreateRandomFactory` — dedup by Identifier

```java
public PositionalRandomFactory getOrCreateRandomFactory(Identifier id) {
    return this.positionalRandoms.computeIfAbsent(id,
        i -> this.random.fromHashOf(id).forkPositional());   // *** RNG: fromHashOf("minecraft:<path>") then forkPositional ***
    // lambda$getOrCreateRandomFactory$0 likewise uses the captured outer `id`.
}
```

### 1.6 Trivial accessors

```java
public NoiseRouter router()                  { return this.router; }
public Climate.Sampler sampler()             { return this.sampler; }
public SurfaceSystem surfaceSystem()         { return this.surfaceSystem; }   // YES — owned + exposed
public PositionalRandomFactory aquiferRandom() { return this.aquiferRandom; }
public PositionalRandomFactory oreRandom()     { return this.oreRandom; }
```

No CODEC anywhere in RandomState (checked constant pool — none).

---

## 2. Inner classes

### 2.1 `RandomState$1NoiseWiringHelper` (`implements DensityFunction.Visitor`)

Real ctor descriptor (from the invokespecial in RandomState ctor): `(RandomState this$0, long seed, boolean useLegacyInit)`.
State: `Map<DensityFunction, DensityFunction> wrapped = new HashMap<>()` (per-visitor memoization by identity/equals of the DF).

```java
private RandomSource newLegacyInstance(long salt) {
    return new LegacyRandomSource(this.seed + salt);          // *** RNG: new LegacyRandomSource(seed + salt) ***
}

public DensityFunction.NoiseHolder visitNoise(DensityFunction.NoiseHolder holder) {
    Holder<NormalNoise.NoiseParameters> data = holder.noiseData();
    if (data.is(Noises.TEMPERATURE_NETHER)) {                 // key "minecraft:nether/temperature"
        // *** RNG: NormalNoise.createLegacyNetherBiome(new LegacyRandomSource(seed + 0L), params) ***
        NormalNoise n = NormalNoise.createLegacyNetherBiome(newLegacyInstance(0L),
                            (NormalNoise.NoiseParameters) data.value());
        return new DensityFunction.NoiseHolder(data, n);
    }
    if (data.is(Noises.VEGETATION_NETHER)) {                  // key "minecraft:nether/vegetation"
        // *** RNG: NormalNoise.createLegacyNetherBiome(new LegacyRandomSource(seed + 1L), params) ***
        NormalNoise n = NormalNoise.createLegacyNetherBiome(newLegacyInstance(1L),
                            (NormalNoise.NoiseParameters) data.value());
        return new DensityFunction.NoiseHolder(data, n);
    }
    // default path: dedup'd instantiate keyed by the holder's ResourceKey
    NormalNoise n = this.this$0.getOrCreateNoise((ResourceKey) data.unwrapKey().orElseThrow());
    return new DensityFunction.NoiseHolder(data, n);
}

private DensityFunction wrapNew(DensityFunction df) {
    if (df instanceof BlendedNoise blended) {
        RandomSource rs = this.useLegacyInit
            ? newLegacyInstance(0L)                                            // *** RNG: LegacyRandomSource(seed + 0) ***
            : this$0.random.fromHashOf(Identifier.withDefaultNamespace("terrain"));  // *** RNG: fromHashOf("minecraft:terrain") ***
        return blended.withNewRandom(rs);
    }
    if (df instanceof DensityFunctions.EndIslandDensityFunction) {
        return new DensityFunctions.EndIslandDensityFunction(this.seed);       // *** RNG: seeded with raw world seed ***
    }
    return df;
}

public DensityFunction apply(DensityFunction df) {
    return this.wrapped.computeIfAbsent(df, this::wrapNew);
}
```

(javap prints the ctor header as no-arg — that is the usual javap artifact for a method-local class;
the code body clearly stores `lload_2 -> val$seed`, `iload 4 -> val$useLegacyInit`, `aload_1 -> this$0`. Confidence: high.)

### 2.2 `RandomState$1` — the sampler's "unwrap" visitor

Same memoizing `apply` (HashMap + `computeIfAbsent(df, this::wrapNew)`), with:

```java
private DensityFunction wrapNew(DensityFunction df) {
    if (df instanceof DensityFunctions.HolderHolder hh) return (DensityFunction) hh.function().value();
    if (df instanceof DensityFunctions.Marker m)        return m.wrapped();
    return df;
}
```

No `visitNoise` override → interface default is used (identity on NoiseHolder; not shown in this class).
It touches no RNG.

---

## 3. `net.minecraft.world.level.levelgen.Noises`

### 3.1 Key construction

```java
private static ResourceKey<NormalNoise.NoiseParameters> createKey(String path) {
    return ResourceKey.create(Registries.NOISE, Identifier.withDefaultNamespace(path));  // namespace "minecraft"
}
```

### 3.2 `instantiate` — the single place a datapack noise becomes a NormalNoise

```java
public static NormalNoise instantiate(HolderGetter<NormalNoise.NoiseParameters> getter,
                                      PositionalRandomFactory random,
                                      ResourceKey<NormalNoise.NoiseParameters> key) {
    Holder.Reference<NormalNoise.NoiseParameters> holder = getter.getOrThrow(key);
    // *** RNG: random.fromHashOf(key.identifier())  ==  fromHashOf("minecraft:<path>") ***
    return NormalNoise.create(random.fromHashOf(key.identifier()),
                              (NormalNoise.NoiseParameters) holder.value());
}
```

### 3.3 ALL static ResourceKey constants (static{} order; field → registry key string, all `minecraft:`-namespaced)

| # | Field | Key string (path; full key = `minecraft:` + path) |
|---|-------|-----------------------------------------------------|
| 1 | TEMPERATURE | `temperature` |
| 2 | VEGETATION | `vegetation` |
| 3 | CONTINENTALNESS | `continentalness` |
| 4 | EROSION | `erosion` |
| 5 | TEMPERATURE_LARGE | `temperature_large` |
| 6 | VEGETATION_LARGE | `vegetation_large` |
| 7 | CONTINENTALNESS_LARGE | `continentalness_large` |
| 8 | EROSION_LARGE | `erosion_large` |
| 9 | RIDGE | `ridge` |
| 10 | SHIFT | **`offset`** (field name ≠ key!) |
| 11 | TEMPERATURE_NETHER | `nether/temperature` |
| 12 | VEGETATION_NETHER | `nether/vegetation` |
| 13 | AQUIFER_BARRIER | `aquifer_barrier` |
| 14 | AQUIFER_FLUID_LEVEL_FLOODEDNESS | `aquifer_fluid_level_floodedness` |
| 15 | AQUIFER_LAVA | `aquifer_lava` |
| 16 | AQUIFER_FLUID_LEVEL_SPREAD | `aquifer_fluid_level_spread` |
| 17 | PILLAR | `pillar` |
| 18 | PILLAR_RARENESS | `pillar_rareness` |
| 19 | PILLAR_THICKNESS | `pillar_thickness` |
| 20 | SPAGHETTI_2D | `spaghetti_2d` |
| 21 | SPAGHETTI_2D_ELEVATION | `spaghetti_2d_elevation` |
| 22 | SPAGHETTI_2D_MODULATOR | `spaghetti_2d_modulator` |
| 23 | SPAGHETTI_2D_THICKNESS | `spaghetti_2d_thickness` |
| 24 | SPAGHETTI_3D_1 | `spaghetti_3d_1` |
| 25 | SPAGHETTI_3D_2 | `spaghetti_3d_2` |
| 26 | SPAGHETTI_3D_RARITY | `spaghetti_3d_rarity` |
| 27 | SPAGHETTI_3D_THICKNESS | `spaghetti_3d_thickness` |
| 28 | SPAGHETTI_ROUGHNESS | `spaghetti_roughness` |
| 29 | SPAGHETTI_ROUGHNESS_MODULATOR | `spaghetti_roughness_modulator` |
| 30 | CAVE_ENTRANCE | `cave_entrance` |
| 31 | CAVE_LAYER | `cave_layer` |
| 32 | CAVE_CHEESE | `cave_cheese` |
| 33 | ORE_VEININESS | `ore_veininess` |
| 34 | ORE_VEIN_A | `ore_vein_a` |
| 35 | ORE_VEIN_B | `ore_vein_b` |
| 36 | ORE_GAP | `ore_gap` |
| 37 | NOODLE | `noodle` |
| 38 | NOODLE_THICKNESS | `noodle_thickness` |
| 39 | NOODLE_RIDGE_A | `noodle_ridge_a` |
| 40 | NOODLE_RIDGE_B | `noodle_ridge_b` |
| 41 | JAGGED | `jagged` |
| 42 | **SURFACE** | `surface` |
| 43 | **SURFACE_SECONDARY** | `surface_secondary` |
| 44 | **CLAY_BANDS_OFFSET** | `clay_bands_offset` |
| 45 | **BADLANDS_PILLAR** | `badlands_pillar` |
| 46 | **BADLANDS_PILLAR_ROOF** | `badlands_pillar_roof` |
| 47 | **BADLANDS_SURFACE** | `badlands_surface` |
| 48 | **ICEBERG_PILLAR** | `iceberg_pillar` |
| 49 | **ICEBERG_PILLAR_ROOF** | `iceberg_pillar_roof` |
| 50 | **ICEBERG_SURFACE** | `iceberg_surface` |
| 51 | **SULFUR_CAVE_GRADIENT** | `sulfur_cave_gradient` — **NEW in 26.2 (sulfur caves)** |
| 52 | SWAMP | **`surface_swamp`** (field name ≠ key!) |
| 53 | CALCITE | `calcite` |
| 54 | GRAVEL | `gravel` |
| 55 | **POWDER_SNOW** | `powder_snow` |
| 56 | **PACKED_ICE** | `packed_ice` |
| 57 | ICE | `ice` |
| 58 | SOUL_SAND_LAYER | `soul_sand_layer` |
| 59 | GRAVEL_LAYER | `gravel_layer` |
| 60 | PATCH | `patch` |
| 61 | NETHERRACK | `netherrack` |
| 62 | NETHER_WART | `nether_wart` |
| 63 | NETHER_STATE_SELECTOR | `nether_state_selector` |

So e.g. the surface noise RandomSource is `random.fromHashOf("minecraft:surface")`,
clay bands offset is `random.fromHashOf("minecraft:clay_bands_offset")`, etc.

---

## 4. `NoiseBasedChunkGenerator` — buildSurface overloads only

### 4.1 Public 4-arg overload (called from ChunkStatusTasks)

```java
public void buildSurface(WorldGenRegion region, StructureManager structureManager, RandomState randomState, ChunkAccess chunk) {
    if (SharedConstants.debugVoidTerrain(chunk.getPos()) || SharedConstants.DEBUG_DISABLE_SURFACE) return;
    WorldGenerationContext ctx = new WorldGenerationContext(this, region);   // (ChunkGenerator, LevelHeightAccessor)
    Set<Holder<Biome>> possibleBiomes = collectPossibleBiomes(region, 1);    // radius literal iconst_1
    this.buildSurface(chunk, ctx, randomState, structureManager,
                      region.getBiomeManager(),
                      Blender.of(region),
                      possibleBiomes);
}
```

`collectPossibleBiomes` (NEW mechanism, see §9):

```java
private static Set<Holder<Biome>> collectPossibleBiomes(WorldGenRegion region, int radius) {
    Set<Holder<Biome>> set = new it.unimi.dsi.fastutil.objects.ReferenceOpenHashSet<>();  // identity-hashed!
    ChunkPos center = region.getCenter();
    for (int cz = center.z() - radius; cz <= center.z() + radius; cz++) {        // z OUTER, <= (inclusive)
        for (int cx = center.x() - radius; cx <= center.x() + radius; cx++) {    // x INNER, <= (inclusive)
            region.getChunk(cx, cz).collectBiomesInPalette(set);                 // palette scan, no per-block iteration
        }
    }
    return set;
}
```

So the `Set<Holder<Biome>>` = union of **biome palettes** of the 3×3 chunk neighborhood around the
center chunk (z-major iteration; order irrelevant for a set, palette order matters only if you
replicate the ReferenceOpenHashSet iteration — treat it as an unordered membership set).

### 4.2 7-arg overload

```java
public void buildSurface(ChunkAccess chunk, WorldGenerationContext ctx, RandomState randomState,
                         StructureManager structureManager, BiomeManager biomeManager,
                         Blender blender, Set<Holder<Biome>> possibleBiomes) {
    NoiseChunk noiseChunk = chunk.getOrCreateNoiseChunk(
        c -> this.createNoiseChunk(c, structureManager, blender, randomState));   // lambda$buildSurface$0
    NoiseGeneratorSettings settings = (NoiseGeneratorSettings) this.settings.value();
    randomState.surfaceSystem().buildSurface(
        randomState,                        // 1
        biomeManager,                       // 2
        settings.useLegacyRandomSource(),   // 3  <- THE boolean; from NoiseGeneratorSettings JSON "legacy_random_source"
        ctx,                                // 4
        chunk,                              // 5
        noiseChunk,                         // 6
        settings.surfaceRule(),             // 7
        possibleBiomes);                    // 8
}
```

`SurfaceSystem.buildSurface` descriptor:
`(RandomState, BiomeManager, Z, WorldGenerationContext, ChunkAccess, NoiseChunk, SurfaceRules$RuleSource, Set)V`.

`createNoiseChunk` (call-signature context only; NoiseChunk internals are another cluster):

```java
private NoiseChunk createNoiseChunk(ChunkAccess chunk, StructureManager sm, Blender blender, RandomState rs) {
    return NoiseChunk.forChunk(chunk, rs,
        Beardifier.forStructuresInChunk(sm, chunk.getPos()),
        (NoiseGeneratorSettings) this.settings.value(),
        (Aquifer.FluidPicker) this.globalFluidPicker.get(),
        blender);
}
```

Since the noise-fill stage runs first and also calls `getOrCreateNoiseChunk`, at surface time the
NoiseChunk is normally **already cached on the ChunkAccess** and the lambda never runs (same
NoiseChunk instance as fill — interpolators/slices state is shared).

---

## 5. `ChunkStatusTasks.generateSurface`

```java
public static CompletableFuture<ChunkAccess> generateSurface(WorldGenContext ctx, ChunkStep step,
        StaticCache2D<GenerationChunkHolder> cache, ChunkAccess chunk) {
    ServerLevel level = ctx.level();
    WorldGenRegion region = new WorldGenRegion(level, cache, step, chunk);
    ctx.generator().buildSurface(region,
        level.structureManager().forWorldGenRegion(region),
        level.getChunkSource().randomState(),          // the singleton RandomState from ServerChunkCache
        chunk);
    return CompletableFuture.completedFuture(chunk);
}
```

Same WorldGenRegion shape as the noise step (`generateNoise` also builds
`new WorldGenRegion(level, cache, step, chunk)` and uses `structureManager().forWorldGenRegion(region)`).

---

## 6. `WorldGenRegion` — constructor (relevant part) + biome plumbing

### 6.1 Constructor `(ServerLevel, StaticCache2D<GenerationChunkHolder>, ChunkStep, ChunkAccess center)`

Exact assignment order:

```java
this.blockTicks   = new WorldGenTickAccess<>(pos -> this.getChunk(pos).getBlockTicks());
this.fluidTicks   = new WorldGenTickAccess<>(pos -> this.getChunk(pos).getFluidTicks());
this.subTickCount = new AtomicLong();
this.generatingStep = step;
this.cache  = cache;
this.center = center;
this.level  = level;
this.seed   = level.getSeed();
this.levelData = level.getLevelData();

// *** RNG: region-local random ***
this.random = level.getChunkSource().randomState()
                   .getOrCreateRandomFactory(WORLDGEN_REGION_RANDOM)   // "minecraft:worldgen_region_random"
                   .at(this.center.getPos().getWorldPosition());       // PositionalRandomFactory.at(BlockPos)
                   // default at(BlockPos) -> at(pos.getX(), pos.getY(), pos.getZ())
                   // i.e. at(chunkX*16, 0, chunkZ*16)  [ChunkPos.getWorldPosition — signature only]

this.dimensionType = level.dimensionType();

// *** BiomeManager: source = this WorldGenRegion itself; seed = sha256(worldSeed) ***
this.biomeManager = new BiomeManager(this, BiomeManager.obfuscateSeed(this.seed));

ChunkPos pos = center.getPos();
this.centerChunkX = pos.x();
this.centerChunkZ = pos.z();
this.writeRadius = step.blockStateWriteRadius();
```

Static init: `WORLDGEN_REGION_RANDOM = Identifier.withDefaultNamespace("worldgen_region_random")`.

`BiomeManager.obfuscateSeed` (verified, it is tiny):

```java
public static long obfuscateSeed(long seed) {
    return Hashing.sha256().hashLong(seed).asLong();   // Guava sha256 of little-endian long, first 8 bytes LE
}
```

### 6.2 `getBiomeManager` / biome lookups

```java
public BiomeManager getBiomeManager() { return this.biomeManager; }

public Holder<Biome> getUncachedNoiseBiome(int qx, int qy, int qz) {
    return this.level.getUncachedNoiseBiome(qx, qy, qz);
}
```

`getNoiseBiome(int,int,int)` is **not overridden** in WorldGenRegion — it inherits the
`LevelReader` default (WorldGenRegion is the `BiomeManager.NoiseBiomeSource`):

```java
// LevelReader default
default Holder<Biome> getNoiseBiome(int qx, int qy, int qz) {
    ChunkAccess chunk = this.getChunk(QuartPos.toSection(qx), QuartPos.toSection(qz),  // qx >> 2, qz >> 2
                                      ChunkStatus.BIOMES, /*require=*/false);
    return chunk != null ? chunk.getNoiseBiome(qx, qy, qz)
                         : this.getUncachedNoiseBiome(qx, qy, qz);
}
```

So during the surface stage, `BiomeManager.getBiome(pos)` → zoomed quart coords → WorldGenRegion
(LevelReader default) → the neighborhood chunk's stored biome palette (`ChunkAccess.getNoiseBiome`),
falling back to the biome source only if the chunk is missing/below BIOMES status.

---

## 7. `ChunkAccess`

### 7.1 `getOrCreateNoiseChunk`

```java
public NoiseChunk getOrCreateNoiseChunk(Function<ChunkAccess, NoiseChunk> factory) {
    if (this.noiseChunk == null) {
        this.noiseChunk = factory.apply(this);
    }
    return this.noiseChunk;    // field re-read after the write (plain field, not volatile)
}
```

Field: `protected NoiseChunk noiseChunk;` — one NoiseChunk cached per chunk for its whole gen lifetime.

### 7.2 `getNoiseBiome(int qx, int qy, int qz)` — exact clamping

```java
public Holder<Biome> getNoiseBiome(int qx, int qy, int qz) {
    try {
        int minQuartY = QuartPos.fromBlock(this.getMinY());          // getMinY() >> 2 (arithmetic shift)
        int maxQuartY = minQuartY + QuartPos.fromBlock(this.getHeight()) - 1;   // + (height >> 2) - 1
        int clampedQY = Mth.clamp(qy, minQuartY, maxQuartY);          // Math.min(Math.max(qy, min), max)
        int sectionIndex = this.getSectionIndex(QuartPos.toBlock(clampedQY));   // (clampedQY << 2) block y
        // LevelHeightAccessor defaults: getSectionIndex(y) = SectionPos.blockToSectionCoord(y) - getMinSectionY()
        return this.sections[sectionIndex].getNoiseBiome(qx & 3, clampedQY & 3, qz & 3);
    } catch (Throwable t) {
        throw new ReportedException(CrashReport.forThrowable(t, "Getting biome"));  // "Biome being got"/"Location"
    }
}
```

Key facts:
- **Only Y is clamped** (to `[minY>>2, (minY>>2)+(height>>2)-1]` in quart space). X/Z are just masked `& 3`.
- `QuartPos.fromBlock = >> 2` (ishr — floor for negatives), `toBlock = << 2`, `toSection = >> 2`,
  `fromSection = << 2`, `quartLocal = & 3`; constants `BITS=2, SIZE=4, MASK=3`.
- Section chosen from the **clamped** quart Y, so out-of-range Y hits the bottom/top section, cell (y&3 of clamped).

### 7.3 `collectBiomesInPalette` (used by collectPossibleBiomes)

```java
public void collectBiomesInPalette(Set<Holder<Biome>> out) {
    for (LevelChunkSection section : this.sections) {
        section.getBiomes().forEachInPalette(out::add);   // PalettedContainerRO — palette entries only
    }
}
```

### 7.4 `fillBiomesFromNoise` (context — where the palette that surface reads comes from)

```java
public void fillBiomesFromNoise(BiomeResolver resolver, Climate.Sampler sampler) {
    ChunkPos pos = this.getPos();
    int qx = QuartPos.fromBlock(pos.getMinBlockX());
    int qz = QuartPos.fromBlock(pos.getMinBlockZ());
    LevelHeightAccessor h = this.getHeightAccessorForGeneration();
    for (int sy = h.getMinSectionY(); sy <= h.getMaxSectionY(); sy++) {
        LevelChunkSection section = this.getSection(this.getSectionIndexFromSectionY(sy));
        section.fillBiomesFromNoise(resolver, sampler, qx, QuartPos.fromSection(sy), qz);
    }
}
```

---

## 8. `LevelChunkSection.getNoiseBiome`

```java
public Holder<Biome> getNoiseBiome(int x, int y, int z) {     // x,y,z already masked to 0..3 by caller
    return (Holder<Biome>) this.biomes.get(x, y, z);          // PalettedContainerRO.get(III)
}
```

---

## 9. Direct answers to the cluster questions

1. **Does RandomState own the SurfaceSystem in 26.2?** Yes. `private final SurfaceSystem surfaceSystem`,
   built in the ctor step (7), exposed via `surfaceSystem()`. `NoiseBasedChunkGenerator.buildSurface`
   fetches it with `randomState.surfaceSystem()`.
2. **Which PositionalRandomFactory does SurfaceSystem get?** The **root** `this.random` =
   `settings.getRandomSource().newInstance(seed).forkPositional()` — no intermediate `fromHashOf` fork
   at construction (unlike aquifer/ore which get `fromHashOf("minecraft:aquifer"/"minecraft:ore").forkPositional()`).
   Ctor: `new SurfaceSystem(this /*RandomState*/, settings.defaultBlock(), settings.seaLevel(), this.random)`.
3. **getOrCreateNoise dedup + seeding:** dedups by `ResourceKey` in a `ConcurrentHashMap`; creates via
   `NormalNoise.create(random.fromHashOf(key.identifier()), params)`; `fromHashOf(Identifier)` →
   `fromHashOf(id.toString())` and `Identifier.toString()` = `namespace + ":" + path` — **the hashed string
   includes the `minecraft:` prefix** (e.g. `"minecraft:surface"`, `"minecraft:clay_bands_offset"`).
4. **useLegacyRandomSource:** `NoiseGeneratorSettings.useLegacyRandomSource()` (record component; the
   overworld preset is XOROSHIRO/false — value comes from the settings JSON, not hardcoded here). It is
   passed (a) into NoiseWiringHelper (BlendedNoise legacy init + implicit via Algorithm) and (b) as the
   boolean 3rd argument of `SurfaceSystem.buildSurface`.
5. **Where the `Set<Holder<Biome>>` comes from:** `collectPossibleBiomes(region, 1)` — union of section
   biome-palettes over the 3×3 chunk neighborhood, in a `ReferenceOpenHashSet` (identity semantics on
   `Holder` references).
6. **Where NoiseChunk comes from:** `chunk.getOrCreateNoiseChunk(...)` — cached field on ChunkAccess,
   normally created earlier during the noise-fill step via the identical
   `createNoiseChunk(chunk, structureManager, blender, randomState)` → `NoiseChunk.forChunk(...)`.

## 10. NEW / changed vs 1.21 (bytecode-observed, not memory-guessed where marked)

- **`Noises.SULFUR_CAVE_GRADIENT` = `minecraft:sulfur_cave_gradient` — new key (sulfur caves).**
- `SurfaceSystem.buildSurface` signature now ends with `Set<Holder<Biome>>` (possible-biomes set collected
  from the 3×3 palette union) — in 1.21 this parameter slot was a `Registry<Biome>`; `collectPossibleBiomes`
  + `ChunkAccess.collectBiomesInPalette` are the new plumbing.
- `ResourceLocation` renamed to `Identifier` (`withDefaultNamespace`, `identifier()` accessor on ResourceKey).
- `WorldGenRegion.random` is derived from
  `RandomState.getOrCreateRandomFactory("minecraft:worldgen_region_random").at(chunkOrigin)` (present in
  bytecode; I did not verify 1.21 for comparison — flagged, not claimed as new).
- Everything else in this cluster (fork order, aquifer/ore/terrain hash strings, legacy nether biome
  salts 0/1, obfuscateSeed=sha256) matches the 1.21-era structure.

## 11. Ambiguities / confidence notes

- `RandomState$1NoiseWiringHelper` ctor: javap header shows `()` but the body stores 3 captured values;
  descriptor from the caller is `(RandomState;JZ)V`. Interpretation: javac-synthesized local-class capture.
  Confidence: high.
- `lambda$getOrCreateNoise$0(ResourceKey captured, ResourceKey mapKey)` uses `aload_1` (captured) — both are
  the same object at runtime since the map key IS the captured key. Confidence: high.
- `ChunkPos.getWorldPosition()` and `XoroshiroRandomSource.forkPositional()` internals were not deep-dived
  (out of cluster); only call signatures are asserted here.
- `getMaxSectionY` inclusive bound in `fillBiomesFromNoise` (`if_icmpgt` on `sy <= max`) implies
  `getMaxSectionY()` is the last section index, consistent with `getSectionIndexFromSectionY = sy - minSectionY`.
