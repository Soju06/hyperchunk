# A1 — Carver-stage orchestration: `ChunkStatusTasks.generateCarvers` → `NoiseBasedChunkGenerator.applyCarvers` (MC 26.2, unobfuscated server bytecode)

Source of truth: `javap -p -c -constants` (and `javap -v` for BootstrapMethods) against
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server`. All pseudocode is a 1:1 reconstruction
from bytecode. No vanilla-source guessing.

Companion notes: A2 (WorldCarver base: `isStartChunk` impls, `carveEllipsoid`, CarveSkipChecker),
A3 (CaveWorldCarver), A4 (CanyonWorldCarver), A5 (CarvingContext + aquifer),
A6 (CarvingMask + ProtoChunk + heightmaps), A7 (WorldgenRandom + Mth + value providers).
This note owns the stage plumbing, the 17×17 source-chunk loop, and the per-(carver,chunk) RNG seeding.

---

## 1. Stage task: `ChunkStatusTasks.generateCarvers`

`net/minecraft/world/level/chunk/status/ChunkStatusTasks.class`:

```java
public static CompletableFuture<ChunkAccess> generateCarvers(
        WorldGenContext ctx, ChunkStep step, StaticCache2D<GenerationChunkHolder> cache, ChunkAccess chunk) {
    ServerLevel level = ctx.level();                                        // slot 4
    WorldGenRegion region = new WorldGenRegion(level, cache, step, chunk);  // slot 5
    if (chunk instanceof ProtoChunk protoChunk) {
        Blender.addAroundOldChunksCarvingMaskFilter(region, protoChunk);    // §1.2
    }
    ctx.generator().applyCarvers(
        region,                                                    // WorldGenRegion
        level.getSeed(),                                           // long — the raw world seed
        level.getChunkSource().randomState(),                      // RandomState (A5)
        level.getBiomeManager(),                                   // BiomeManager (§5.2: zoom seed = obfuscateSeed(worldSeed))
        level.structureManager().forWorldGenRegion(region),        // StructureManager
        chunk);                                                    // target ChunkAccess
    return CompletableFuture.completedFuture(chunk);
}
```

`WorldGenContext` is a record `(ServerLevel level, ChunkGenerator generator,
StructureTemplateManager structureManager, ThreadedLevelLightEngine lightEngine,
Executor mainThreadExecutor, LevelChunk$UnsavedListener unsavedListener)`. Note the
StructureManager passed to `applyCarvers` is `ServerLevel.structureManager()` (a different object
from the context's StructureTemplateManager), wrapped `forWorldGenRegion(region)`.

- No RNG is constructed here; no per-stage `WorldgenRandom` argument exists anymore
  (26.2 delta vs old `applyCarvers(..., GenerationStep.Carving)` shape — see §6).
- The StructureManager ends up ONLY inside the NoiseChunk-creation lambda (beardifier); in normal
  generation the NoiseChunk is already cached from the NOISE stage, so it is dormant (§7.1).

### 1.1 `WorldGenRegion.<init>(ServerLevel, StaticCache2D, ChunkStep, ChunkAccess)` — relevant fields

putfield order (carver-relevant subset): `generatingStep = step`, `cache`, `center = chunk`,
`level`, `seed = level.getSeed()`, `levelData`,
`random = level.getChunkSource().randomState().getOrCreateRandomFactory(WORLDGEN_REGION_RANDOM).at(center.getPos().getWorldPosition())`
(26.2 delta: positional region random — NOT used by carving), `dimensionType`,
`biomeManager = new BiomeManager(this, BiomeManager.obfuscateSeed(this.seed))`,
`centerChunkX/Z`, `writeRadius = step.blockStateWriteRadius()`.

`WorldGenRegion.getChunk(int x, int z)` → `getChunk(x, z, ChunkStatus.EMPTY)` →
`getChunk(x, z, status, /*require*/ true)`:

```java
int dist = center.getPos().getChessboardDistance(x, z);   // max(|dx|,|dz|)
ChunkStatus max = dist >= generatingStep.directDependencies().size()
                ? null
                : generatingStep.directDependencies().get(dist);
if (max != null && requestedStatus.isOrBefore(max)) {
    ChunkAccess c = cache.get(x, z).getChunkIfPresentUnchecked(max);
    if (c != null) return c;
}
// else: crash path ("Requested chunk unavailable during world generation")
```

So during CARVERS the region hands out neighbor protochunks from the StaticCache2D; the allowed
radius comes from `directDependencies` (§2.1).

### 1.2 `Blender.addAroundOldChunksCarvingMaskFilter(WorldGenLevel, ProtoChunk)` — note only

Guarded by `SharedConstants.DEBUG_DISABLE_BLENDING`. Iterates `Direction8.values()` neighbors via
`region.getChunk(x, z)` collecting per-direction BlendingData into an ImmutableMap; only installs a
mask filter when old (pre-1.18-blending) chunks are adjacent. For a fresh hyperchunk world there is
no BlendingData, so this is a no-op — not deep-dived here (mask internals are A6 §CarvingMask).

---

## 2. Pyramid entry: dependency radii for CARVERS

`ChunkPyramid.GENERATION_PYRAMID` (static{} of `ChunkPyramid.class`) builds steps in order
EMPTY, STRUCTURE_STARTS, STRUCTURE_REFERENCES, BIOMES, NOISE, SURFACE, CARVERS, FEATURES,
INITIALIZE_LIGHT, LIGHT, SPAWN, FULL. The CARVERS step (lambda$static$6, indy bootstrap #19):

```java
.step(ChunkStatus.CARVERS, builder -> builder
    .addRequirement(ChunkStatus.STRUCTURE_STARTS, 8)
    .blockStateWriteRadius(0)
    .setTask(ChunkStatusTasks::generateCarvers))          // bootstrap #7 → generateCarvers
```

(For contrast: SURFACE = `addRequirement(STRUCTURE_STARTS,8).addRequirement(BIOMES,1)`,
FEATURES = `addRequirement(STRUCTURE_STARTS,8).addRequirement(CARVERS,1).blockStateWriteRadius(1)`.)

### 2.1 `ChunkStep$Builder` semantics (exact)

- Builder for a non-first step is created with `parent = previous step`; it initializes
  `directDependenciesByRadius = { parent.targetStatus }` (length 1: radius 0 → SURFACE).
- `addRequirement(status, radius)`: grows the array to `radius+1`, `Arrays.fill(new, status)`, then
  for indices `0..min(radius+1, oldLen)-1` keeps `ChunkStatus.max(old[i], status)`.
- Net **directDependencies for CARVERS** (index = chessboard radius):
  `[0]=SURFACE, [1..8]=STRUCTURE_STARTS` (size 9 ⇒ neighbors available up to radius 8).
- `blockStateWriteRadius(0)`: `WorldGenRegion.writeRadius = 0` — carving may only WRITE blocks in
  the center chunk (also `ensureCanWrite` allows Y-slack of `16*(dist-writeRadius)`, moot at dist 0).
- `accumulatedDependencies` merges parent's accumulated deps shifted by
  `getRadiusOfParent(SURFACE)` = highest index whose status `isOrAfter(SURFACE)` = 0 — only used
  for scheduling/`SAFETY_MARGIN_CHUNKS`, not for worldgen math.

So: when carving chunk C, chunks at chessboard distance 1..8 exist at ≥ STRUCTURE_STARTS only
(no noise/surface data needed from them — the biome for a neighbor source chunk is recomputed from
the biome source, §5.1, and cached on that protochunk).

### 2.2 `ChunkStatus` registration (static{} order)

`CARVERS = register("carvers", SURFACE, FINAL_HEIGHTMAPS, ChunkType.PROTOCHUNK)` — parent SURFACE.
26.2 delta: there is NO `LIQUID_CARVERS` status and no second carving pass anywhere in the pyramid.
FINAL_HEIGHTMAPS = EnumSet.of(OCEAN_FLOOR, WORLD_SURFACE, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES)
(heightmap priming itself happens in `generateFeatures`, not in the carver task — A6).

---

## 3. `ChunkGenerator.applyCarvers` — ABSTRACT in 26.2

`net/minecraft/world/level/chunk/ChunkGenerator.class`:

```java
public abstract void applyCarvers(WorldGenRegion, long seed, RandomState, BiomeManager,
                                  StructureManager, ChunkAccess);
```

26.2 delta: in 1.21 this was a concrete method on ChunkGenerator taking a trailing
`GenerationStep.Carving` arg. In 26.2 it is abstract, has NO carving-step argument, and the only
overworld implementation is `NoiseBasedChunkGenerator.applyCarvers` (§4). (FlatLevelSource /
DebugLevelSource stubs are out of scope for hyperchunk.)

`ChunkGenerator` fields (declaration order): `biomeSource`, `featuresPerStep` (memoized, features
stage), `generationSettingsGetter`. `ChunkGenerator(BiomeSource)` delegates to the 2-arg ctor with
`generationSettingsGetter = biome -> biome.value().getGenerationSettings()`;
`getBiomeGenerationSettings(Holder<Biome>)` just applies that function.
`NoiseBasedChunkGenerator.<init>(BiomeSource, Holder<NoiseGeneratorSettings>)` uses the 1-arg super,
so the default getter applies.

---

## 4. `NoiseBasedChunkGenerator.applyCarvers` — full 1:1 reconstruction

Local-slot map (from bytecode): 0=this, 1=region, 2/3=seed(long), 4=randomState, 5=biomeManager,
6=structureManager, 7=chunk(TARGET/center), 8=zoomedBiomeManager, 9=worldgenRandom, 10=`int i=8`
(stored, then unused — loops use literal `bipush 8`), 11=centerPos, 12=noiseChunk, 13=aquifer,
14=carvingContext, 15=carvingMask, 16=offsetX, 17=offsetZ, 18=sourcePos, 19=sourceChunk,
20=biomeGenSettings, 21=carvers(Iterable), 22=carverIndex, 23=iterator, 24=holder, 25=configuredCarver.

```java
public void applyCarvers(WorldGenRegion region, long seed, RandomState randomState,
                         BiomeManager biomeManager, StructureManager structureManager,
                         ChunkAccess chunk) {
    if (SharedConstants.DEBUG_DISABLE_CARVERS) return;

    BiomeManager zoomedBiomeManager = biomeManager.withDifferentSource(              // (8)
        (x, y, z) -> this.biomeSource.getNoiseBiome(x, y, z, randomState.sampler())); // lambda$applyCarvers$0
    WorldgenRandom random = new WorldgenRandom(                                       // (9)
        new LegacyRandomSource(RandomSupport.generateUniqueSeed()));                  // seed irrelevant: always re-seeded (§4.1)
    int i = 8;                                                                        // (10) stored, unused below
    ChunkPos centerPos = chunk.getPos();                                              // (11)
    NoiseChunk noiseChunk = chunk.getOrCreateNoiseChunk(                              // (12) cached from NOISE stage (§7.1)
        c -> this.createNoiseChunk(c, structureManager, Blender.of(region), randomState)); // lambda$applyCarvers$1
    Aquifer aquifer = noiseChunk.aquifer();                                           // (13) — A5
    CarvingContext carvingContext = new CarvingContext(                               // (14) — A5
        this, region.registryAccess(), chunk.getHeightAccessorForGeneration(),
        noiseChunk, randomState, this.settings.value().surfaceRule());
    CarvingMask carvingMask = ((ProtoChunk) chunk).getOrCreateCarvingMask();          // (15) — NO step arg (§6)

    for (int offsetX = -8; offsetX <= 8; ++offsetX) {                                 // (16) x OUTER, ascending
        for (int offsetZ = -8; offsetZ <= 8; ++offsetZ) {                             // (17) z INNER, ascending
            ChunkPos sourcePos = new ChunkPos(centerPos.x() + offsetX,                // (18)
                                              centerPos.z() + offsetZ);
            ChunkAccess sourceChunk = region.getChunk(sourcePos.x(), sourcePos.z());  // (19)
            BiomeGenerationSettings biomeGenSettings = sourceChunk.carverBiome(       // (20) cached on sourceChunk (§5.1)
                () -> this.getBiomeGenerationSettings(
                          this.biomeSource.getNoiseBiome(                             // lambda$applyCarvers$2
                              QuartPos.fromBlock(sourcePos.getMinBlockX()),           //   = sourcePos.x << 2   (>>2 of x<<4)
                              0,                                                      //   Y quart = 0 (constant)
                              QuartPos.fromBlock(sourcePos.getMinBlockZ()),
                              randomState.sampler())));
            Iterable<Holder<ConfiguredWorldCarver<?>>> carvers = biomeGenSettings.getCarvers(); // flat (§5.3)
            int carverIndex = 0;                                                      // (22)
            for (Iterator it = carvers.iterator(); it.hasNext(); ) {                  // plain Iterator (26.2 delta:
                ConfiguredWorldCarver<?> carver =                                     //   NOT a ListIterator/nextIndex)
                    (ConfiguredWorldCarver) ((Holder) it.next()).value();             // (25)
                random.setLargeFeatureSeed(seed + (long) carverIndex,                 // *** RESEED — §4.1 ***
                                           sourcePos.x(), sourcePos.z());
                if (carver.isStartChunk(random)) {                                    // RNG draw(s): A2 (carver-specific)
                    carver.carve(carvingContext,
                                 chunk,                                               // TARGET chunk (slot 7), not sourceChunk
                                 zoomedBiomeManager::getBiome,                        // Function<BlockPos,Holder<Biome>> (§5.2)
                                 random, aquifer,
                                 sourcePos,                                           // source ("start") chunk pos
                                 carvingMask);                                        // return value popped/ignored
                }
                ++carverIndex;                                                        // iinc 22,1 — ALWAYS, even if
            }                                                                         //   isStartChunk was false
        }
    }
}
```

Bytecode anchors: loop headers `125: bipush -8 … 131: bipush 8 if_icmpgt 336` (x) and
`136: bipush -8 … 142: bipush 8 if_icmpgt 330` (z) — both `-8..8` INCLUSIVE ⇒ 17×17 = 289 source
chunks. Reseed at `258..275`: `lload_2; iload 22; i2l; ladd` = `seed + (long)carverIndex`, then
`sourcePos.x(); sourcePos.z(); invokevirtual setLargeFeatureSeed:(JII)V`. Loop iterates raw ints;
a fresh `ChunkPos` object is allocated per source chunk (values, not iteration order over a list).
`carve`'s boolean result is `pop`ped.

### 4.1 RNG stream — construction and seeding (exact)

- ONE `WorldgenRandom(new LegacyRandomSource(RandomSupport.generateUniqueSeed()))` instance for the
  whole stage call; the same instance is REUSED across all 289 source chunks × all carvers.
  `RandomSupport.generateUniqueSeed()` = `SEED_UNIQUIFIER.updateAndGet(x -> x*181783497276652981L) ^ System.nanoTime()`
  — deliberately nondeterministic, and provably irrelevant: `setLargeFeatureSeed` runs before ANY draw.
- `WorldgenRandom(RandomSource)` ctor: `super(0L)` (LegacyRandomSource with seed 0) then stores the
  delegate in `randomSource`; all draws forward to the delegate (A7).
- Re-seeded EXACTLY ONCE per (source chunk, carver-list index) pair, i.e. `289 * len(carvers)` times
  per target chunk, immediately before that carver's `isStartChunk`:

```java
// WorldgenRandom.setLargeFeatureSeed(long seed, int chunkX, int chunkZ)  — verified bytecode:
public void setLargeFeatureSeed(long seed, int x, int z) {
    this.setSeed(seed);              // NO |1 here (contrast setDecorationSeed, which does l|1)
    long a = this.nextLong();
    long b = this.nextLong();
    this.setSeed((long)x * a ^ (long)z * b ^ seed);
}
```

  Called with `seed = worldSeed + (long)carverIndex`, `x = sourcePos.x`, `z = sourcePos.z`
  (arg order: J, then chunkX, then chunkZ). The per-carver index enters through the SEED term (added
  to the world seed before the two nextLong draws), not through a salt after mixing.
- `carverIndex` counts list positions 0,1,2,… over `getCarvers()` iteration order and increments
  whether or not `isStartChunk` passed and whether or not `carve` did anything. (Same numeric result
  as 1.21's `ListIterator.nextIndex()`-before-`next()` pattern, but 26.2 compiles a plain
  `Iterator` + manual counter.)
- Draw order after each reseed: `isStartChunk` draws first (carver-specific, e.g. one `nextFloat()`
  vs `probability` — A2 §isStartChunk), then, only if it returned true, all of `carve`'s draws
  (A3/A4). Nothing else touches `random` in this method.

---

## 5. Biome resolution — TWO different resolvers

### 5.1 Carver LIST selection (`carverBiome`) — raw biome source, source-chunk origin, y=0

`ChunkAccess.carverBiome(Supplier<BiomeGenerationSettings>)` is a lazy per-chunk cache
(`carverBiomeSettings` field, null-checked, NOT synchronized):

```java
public BiomeGenerationSettings carverBiome(Supplier<BiomeGenerationSettings> s) {
    if (this.carverBiomeSettings == null) this.carverBiomeSettings = s.get();
    return this.carverBiomeSettings;
}
```

The supplier (lambda$applyCarvers$2) computes — no BiomeManager zoom, no fiddle:

```java
getBiomeGenerationSettings(                                  // = holder.value().getGenerationSettings()
    biomeSource.getNoiseBiome(QuartPos.fromBlock(sourcePos.getMinBlockX()),  // (16*x)>>2 = x<<2
                              0,                                             // quart Y = 0 → block y ∈ [0,4)
                              QuartPos.fromBlock(sourcePos.getMinBlockZ()),  // z<<2
                              randomState.sampler()))
```

- It is the SOURCE chunk's origin (min block corner, i.e. quart (4x, 0, 4z)), not the target's,
  and not centered (+0, no `CHUNK_CENTER_QUART` offset). `QuartPos.fromBlock` = `>>2` (arithmetic).
- Cached on the source ProtoChunk, so each chunk's carver list is computed once even though it is
  consulted by up to 289 neighboring carve passes.

### 5.2 Per-block biome inside `carve` — zoomed BiomeManager over the same raw source

`biomeManager.withDifferentSource(lambda$applyCarvers$0)` clones the level BiomeManager keeping its
`biomeZoomSeed` but replacing the noise-biome source with
`(x,y,z) -> biomeSource.getNoiseBiome(x, y, z, randomState.sampler())`. The `Function<BlockPos,
Holder<Biome>>` passed to `carve` is a method ref to `BiomeManager.getBiome(BlockPos)` on that clone
(bootstrap #9; `Objects.requireNonNull` null-guard emitted before binding) — i.e. the full
fiddled-vector 4×4 zoom (getFiddledDistance over 8 quart corners) applies to carver liquid checks etc.
- Zoom seed provenance: `MinecraftServer.createLevels` computes
  `BiomeManager.obfuscateSeed(worldOptions.seed())` = `Hashing.sha256().hashLong(seed).asLong()`
  once and passes it down; `Level.<init>` stores `new BiomeManager(levelAsSource, zoomSeed)`;
  `ServerLevel.getBiomeManager()` returns it; `withDifferentSource` copies `biomeZoomSeed` verbatim.
  (Zoom internals: covered by task-7 biome-zoom notes / A7; not re-derived here.)

### 5.3 `BiomeGenerationSettings.getCarvers()` — flat in 26.2

```java
public Iterable<Holder<ConfiguredWorldCarver<?>>> getCarvers() { return this.carvers; } // HolderSet field
```

26.2 delta: the field is a single `HolderSet<ConfiguredWorldCarver<?>>` — the per-`GenerationStep.Carving`
`Map` of 1.21 is GONE. Matches biome JSON: e.g.
`data/minecraft/worldgen/biome/sulfur_caves.json` has flat
`"carvers": ["minecraft:cave", "minecraft:cave_extra_underground", "minecraft:canyon"]`
(this list order = `carverIndex` 0,1,2). Carver configs live in
`data/minecraft/worldgen/configured_carver/{cave,cave_extra_underground,canyon,nether_cave}.json`.
Iteration order of a HolderSet.Named is registry-load list order (the JSON array order).

---

## 6. Carving mask & GenerationStep — 26.2 delta

- `net/minecraft/world/level/levelgen/GenerationStep.class` is a plain empty class whose ONLY inner
  enum is `GenerationStep$Decoration`. **There is no `GenerationStep$Carving` (AIR/LIQUID) anymore.**
- Consequently `ProtoChunk` has a single `carvingMask` field with
  `getCarvingMask()/getOrCreateCarvingMask()/setCarvingMask(CarvingMask)` — no step key
  (bit-set internals: A6).
- One carving pass total per chunk; no separate liquid-carver pass, no per-step mask map.

---

## 7. NoiseChunk / Aquifer / CarvingContext hookup (note only — A5 deep-dives)

### 7.1 `ChunkAccess.getOrCreateNoiseChunk(Function<ChunkAccess,NoiseChunk>)`

```java
if (this.noiseChunk == null) this.noiseChunk = fn.apply(this);
return this.noiseChunk;
```

During normal generation the NOISE stage (`fillFromNoise` → `doFill`) already populated
`noiseChunk` on the ProtoChunk, so at CARVERS the lambda
(`createNoiseChunk(chunk, structureManager, Blender.of(region), randomState)`) does NOT run and the
carvers see the SAME NoiseChunk/Aquifer instance (with the aquifer's accumulated internal RNG-free
positional state) that shaped the terrain. `noiseChunk.aquifer()` is the aquifer passed to every
`carve` call. `NoiseBasedChunkGenerator.globalFluidPicker` = memoized
`createFluidPicker(settings)`: FluidStatus(-54, lava) below `min(-54, seaLevel)`, else
FluidStatus(seaLevel, defaultFluid) (bytecode `iload 5; bipush -54; iload_1; Math.min; if_icmpge`) — A5.

### 7.2 `CarvingContext.<init>` args (in order)

`(NoiseBasedChunkGenerator this, region.registryAccess(), chunk.getHeightAccessorForGeneration(),
noiseChunk, randomState, settings.value().surfaceRule())` — the surface rule is used by
`CarvingContext.topMaterial` for post-carve floor fixup (A5).

---

## 8. `ConfiguredWorldCarver` — exact delegation

Record `(WorldCarver<WC> worldCarver, WC config)`; codecs `DIRECT_CODEC`/`CODEC`/`LIST_CODEC`.

```java
public boolean isStartChunk(RandomSource random) {
    return this.worldCarver.isStartChunk(this.config, random);      // WorldCarver.isStartChunk is abstract (A2)
}

public boolean carve(CarvingContext ctx, ChunkAccess chunk, Function<BlockPos,Holder<Biome>> biomeFn,
                     RandomSource random, Aquifer aquifer, ChunkPos sourcePos, CarvingMask mask) {
    if (SharedConstants.debugVoidTerrain(chunk.getPos())) return false;   // pos check on TARGET chunk; debug-only
    return this.worldCarver.carve(ctx, this.config, chunk, biomeFn, random, aquifer, sourcePos, mask);
}
```

No extra RNG draws, no reordering — pure pass-through (the `debugVoidTerrain` guard draws nothing).

---

## 9. Re-seed / reuse summary (assignment item 5)

Per TARGET chunk: 1 `WorldgenRandom` instance. Per SOURCE chunk (289 of them): the instance is
re-seeded once per entry of that source chunk's biome `carvers` list — `setLargeFeatureSeed(worldSeed
+ carverIndex, srcX, srcZ)` — regardless of whether the carver runs. Between reseeds the stream is
consumed by `isStartChunk` and (conditionally) `carve`; leftover state is discarded by the next
reseed. Two carvers at the same list index in different source chunks share NO stream (chunk coords
enter the mix); the same source chunk's carvers differ by the `+index` term only. Vanilla overworld
biomes have `len(carvers) ∈ {0..3}`.

---

## 10. Datapack citations

- `data/minecraft/worldgen/configured_carver/cave.json`, `cave_extra_underground.json`,
  `canyon.json`, `nether_cave.json` — loaded via `ConfiguredWorldCarver.DIRECT_CODEC`
  (registry `worldgen/configured_carver`), referenced from each biome JSON's flat `"carvers"` array.
- `data/minecraft/worldgen/biome/*.json` — `"carvers"` array order defines `carverIndex`.

---

## 11. sulfur findings

`grep -ril sulfur` over `chunk/status/*`, `ChunkGenerator.class`, `NoiseBasedChunkGenerator.class`,
and `levelgen/carver/*`: **no hits** in any assigned class. Only datapack hit:
`data/minecraft/worldgen/biome/sulfur_caves.json`, whose `"carvers"` list is the standard
`["minecraft:cave", "minecraft:cave_extra_underground", "minecraft:canyon"]` — no sulfur-specific
carver exists. As expected.

---

## 12. OPEN items

- OPEN: local slot 10 (`int i = 8`) is stored and never read; the loops use literal `bipush 8`.
  Source form ambiguous (dead local vs. compiler artifact) — zero behavioral impact.
- OPEN: `Blender.addAroundOldChunksCarvingMaskFilter` full mask-filter math not traced (fresh-world
  no-op; only matters when carving next to pre-blending chunks). Flag if golden chunks ever include
  upgraded regions.
- OPEN: `ChunkAccess.carverBiome` / `getOrCreateNoiseChunk` lazy caches are unsynchronized; potential
  cross-thread visibility in vanilla is irrelevant for a deterministic single-threaded C port, noted
  for completeness.
- OPEN (neighbor ground): exact draw counts of `isStartChunk`/`carve` per carver type — A2/A3/A4;
  aquifer state sharing between noise fill and carving — A5.
