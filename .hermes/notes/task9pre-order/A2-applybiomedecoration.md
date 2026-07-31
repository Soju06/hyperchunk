# A2 — Features-stage decoration body: `ChunkGenerator.applyBiomeDecoration` (MC 26.2, unobfuscated server bytecode)

Source of truth: `javap -p -c -constants` (and `javap -v` for BootstrapMethods) against
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server`. All pseudocode is a 1:1 reconstruction
from bytecode disassembled in this session. No vanilla-source guessing; where 1.21-era memory was
used as a hypothesis it was verified against 26.2 bytecode and matched.

Tooling gotcha (cost one dead-end): the shell's `grep` wrapper injects `-I` (skip binary), so
`grep -rl name work/server` silently returns nothing on `.class` files. Use `command grep -rl`.

---

## 1. Stage task: `ChunkStatusTasks.generateFeatures`

`net/minecraft/world/level/chunk/status/ChunkStatusTasks.class`:

```java
public static CompletableFuture<ChunkAccess> generateFeatures(
        WorldGenContext ctx, ChunkStep step, StaticCache2D<GenerationChunkHolder> cache, ChunkAccess chunk) {
    ServerLevel level = ctx.level();                                              // slot 4
    Heightmap.primeHeightmaps(chunk, EnumSet.of(
        Heightmap.Types.MOTION_BLOCKING, Heightmap.Types.MOTION_BLOCKING_NO_LEAVES,
        Heightmap.Types.OCEAN_FLOOR,     Heightmap.Types.WORLD_SURFACE));
    WorldGenRegion region = new WorldGenRegion(level, cache, step, chunk);        // slot 5
    if (!SharedConstants.DEBUG_DISABLE_FEATURES) {
        ctx.generator().applyBiomeDecoration(
            region,                                              // WorldGenLevel
            chunk,                                               // ChunkAccess (center)
            level.structureManager().forWorldGenRegion(region)); // StructureManager
    }
    Blender.generateBorderTicks(region, chunk);   // 1.18-blending only; no-op for fresh worlds
    return CompletableFuture.completedFuture(chunk);
}
```

- Heightmaps of the CENTER chunk are (re-)primed at task start, BEFORE decoration. Neighbor
  chunks' heightmaps are whatever their own earlier stages (and earlier feature runs writing into
  them) left behind — this is one of the two channels through which cross-chunk decoration order
  leaks into output (the other is block states themselves).
- Exactly one decoration entry point; signature:
  `ChunkGenerator.applyBiomeDecoration(Lnet/minecraft/world/level/WorldGenLevel;Lnet/minecraft/world/level/chunk/ChunkAccess;Lnet/minecraft/world/level/StructureManager;)V`.
- `WorldGenRegion.getSeed()` returns field `seed` = `level.getSeed()` (raw world seed) — verified.

## 2. `ChunkGenerator.applyBiomeDecoration` — 1:1 reconstruction

`net/minecraft/world/level/chunk/ChunkGenerator.class`, method at offset table verified in full,
including all `lambda$applyBiomeDecoration$0..5` synthetics and BootstrapMethods 5–12.

Relevant fields (set in `ChunkGenerator.<init>(BiomeSource, Function)`):
- `biomeSource : BiomeSource`
- `generationSettingsGetter : Function<Holder<Biome>, BiomeGenerationSettings>` — default ctor
  installs `holder -> ((Biome)holder.value()).getGenerationSettings()` (`lambda$new$0`).
- `featuresPerStep : Supplier<List<FeatureSorter.StepFeatureData>>` =
  `Suppliers.memoize(() -> FeatureSorter.buildFeaturesPerStep(List.copyOf(biomeSource.possibleBiomes()), b -> generationSettingsGetter.apply(b).features(), true))`
  (`lambda$new$1`/`lambda$new$2`). Memoized once per generator instance — computed at most once
  per launch (also forced eagerly by `validate()`).

```java
public void applyBiomeDecoration(WorldGenLevel level, ChunkAccess chunk, StructureManager structureManager) {
    ChunkPos chunkPos = chunk.getPos();                                            // slot 4
    if (SharedConstants.debugVoidTerrain(chunkPos)) return;                        // §2.5, debug-only

    SectionPos sectionPos = SectionPos.of(chunkPos, level.getMinSectionY());       // slot 5
    BlockPos origin = sectionPos.origin();       // (minBlockX, minSectionY<<4, minBlockZ); slot 6
    Registry<Structure> structureRegistry =
        level.registryAccess().lookupOrThrow(Registries.STRUCTURE);               // slot 7
    Map<Integer, List<Structure>> structuresByStep = structureRegistry.stream()
        .collect(Collectors.groupingBy(s -> s.step().ordinal()));                 // slot 8; lambda$0
        // groupingBy default = HashMap<Integer, ArrayList>; per-step LIST order
        // = Registry.stream() encounter order = MappedRegistry.byId order (§2.6)
    List<FeatureSorter.StepFeatureData> featuresPerStep = this.featuresPerStep.get(); // slot 9

    WorldgenRandom random = new WorldgenRandom(
        new XoroshiroRandomSource(RandomSupport.generateUniqueSeed()));           // slot 10
        // generateUniqueSeed() is nondeterministic but the state is fully
        // overwritten by setDecorationSeed below — no leak.
    long decorationSeed = random.setDecorationSeed(
        level.getSeed(), origin.getX(), origin.getZ());                           // slot 11 (§2.1)

    Set<Holder<Biome>> biomes = new ObjectArraySet<>();                           // slot 13
    ChunkPos.rangeClosed(sectionPos.chunk(), 1).forEach(pos -> {                  // lambda$1; 3x3 chunks
        ChunkAccess neighbor = level.getChunk(pos.x, pos.z);
        for (LevelChunkSection section : neighbor.getSections())
            section.getBiomes().getAll(biomes::add);           // PalettedContainerRO.getAll
    });
    biomes.retainAll(this.biomeSource.possibleBiomes());

    int stepDataCount = featuresPerStep.size();                                   // slot 14
    Registry<PlacedFeature> placedFeatureRegistry =
        level.registryAccess().lookupOrThrow(Registries.PLACED_FEATURE);          // slot 15 (crash text only)
    int maxStep = Math.max(GenerationStep.Decoration.values().length,             // 11 steps in 26.2:
                           stepDataCount);                                        // slot 16
        // RAW_GENERATION, LAKES, LOCAL_MODIFICATIONS, UNDERGROUND_STRUCTURES,
        // SURFACE_STRUCTURES, STRONGHOLDS, UNDERGROUND_ORES, UNDERGROUND_DECORATION,
        // FLUID_SPRINGS, VEGETAL_DECORATION, TOP_LAYER_MODIFICATION
    try {                                                       // exception range 187..741
        for (int step = 0; step < maxStep; ++step) {                              // slot 17
            int structCounter = 0;                              // slot 18 — RESET each step

            // ---- (a) structure starts for this step, BEFORE plain features ----
            if (structureManager.shouldGenerateStructures()) {  // = WorldOptions.generateStructures()
                for (Structure structure :
                        structuresByStep.getOrDefault(step, Collections.emptyList())) { // slot 21
                    random.setFeatureSeed(decorationSeed, structCounter, step);   // §2.1
                    Supplier<String> desc = () -> structureRegistry.getResourceKey(structure)
                        .map(ResourceKey::toString).orElseGet(structure::toString); // lambda$2; slot 22
                    level.setCurrentlyGenerating(desc);         // diagnostics only
                    try {
                        structureManager.startsForStructure(sectionPos, structure)   // List<StructureStart> (§2.4)
                            .forEach(start -> start.placeInChunk(                     // lambda$3
                                level, structureManager, this, random,
                                getWritableArea(chunk),   // BB (minBlockX, minY+1, minBlockZ)..(minBlockX+15, maxY, minBlockZ+15)
                                chunkPos));
                    } catch (Exception e) { /* CrashReport "Feature placement"/"Feature"/desc */ }
                    ++structCounter;      // increments PER STRUCTURE (registry entry),
                                          // NOT per start; all starts of one structure
                                          // share one RNG stream seeded once.
                }
            }

            // ---- (b) placed features for this step ----
            if (step < stepDataCount) {
                IntSet intSet = new IntArraySet();                                // slot 19
                for (Holder<Biome> biome : biomes) {                              // ObjectArraySet iter — order irrelevant (membership only)
                    List<HolderSet<PlacedFeature>> perStepSets =
                        this.generationSettingsGetter.apply(biome).features();    // slot 22
                    if (step >= perStepSets.size()) continue;
                    HolderSet<PlacedFeature> holderSet = perStepSets.get(step);   // slot 23
                    FeatureSorter.StepFeatureData data = featuresPerStep.get(step); // slot 24
                    holderSet.stream().map(Holder::value)                         // bootstrap 10 = Holder.value
                        .forEach(pf -> intSet.add(data.indexMapping().applyAsInt(pf))); // lambda$4
                }
                int n = intSet.size();                                            // slot 20
                int[] indices = intSet.toIntArray();                              // slot 21
                Arrays.sort(indices);                    // ascending — canonical order
                FeatureSorter.StepFeatureData data = featuresPerStep.get(step);   // slot 22
                for (int k = 0; k < n; ++k) {                                     // slot 23
                    int featureIndex = indices[k];                                // slot 24
                    PlacedFeature pf = data.features().get(featureIndex);         // slot 25
                    Supplier<String> desc = () -> placedFeatureRegistry.getResourceKey(pf)
                        .map(ResourceKey::toString).orElseGet(pf::toString);      // lambda$5; slot 26
                    random.setFeatureSeed(decorationSeed, featureIndex, step);
                        // salt = index INTO THE PER-STEP TOPO-SORTED LIST (0..len-1),
                        // NOT the loop counter k and NOT a cross-step global id.
                        // Skipped indices (features absent from this chunk's biome set)
                        // leave holes; the salt is stable per (registry, step) regardless
                        // of which biomes are present.
                    level.setCurrentlyGenerating(desc);
                    try {
                        pf.placeWithBiomeCheck(level, this, random, origin);      // §2.7
                    } catch (Exception e) { /* CrashReport "Feature placement"/"Feature"/desc */ }
                }
            }
        }
        level.setCurrentlyGenerating(null);
        if (SharedConstants.DEBUG_FEATURE_COUNT)
            FeatureCountTracker.chunkDecorated(level.getLevel());
    } catch (Exception e) {
        // CrashReport "Biome decoration" / "Generation": CenterX, CenterZ, "Decoration Seed"=decorationSeed
        throw new ReportedException(...);
    }
}
```

Interleaving summary per step: structures of that step first (registry order), then placed
features (sorted per-step-list index order). The SAME `WorldgenRandom` instance threads through
both, but it is re-seeded via `setFeatureSeed` before every structure and before every feature, so
no RNG state crosses items — EXCEPT among the multiple `StructureStart`s of one structure within
one chunk, which deliberately share one stream (§2.4 shows their order is deterministic).

### 2.1 Decoration RNG: exact seeding semantics

`WorldgenRandom` (`net/minecraft/world/level/levelgen/WorldgenRandom.class`) `extends
LegacyRandomSource`, wraps a delegate `randomSource` field; ctor calls `super(0L)` then stores the
delegate. Key overrides (all verified from bytecode):

```java
public int next(int bits) {
    ++this.count;
    if (this.randomSource instanceof LegacyRandomSource legacy) return legacy.next(bits);
    return (int)(this.randomSource.nextLong() >>> (64 - bits));
}
public synchronized void setSeed(long seed) {
    if (this.randomSource == null) return;      // super-ctor guard
    this.randomSource.setSeed(seed);
}
// nextLong() NOT overridden -> BitRandomSource default (verified):
//   long nextLong() { int hi = next(32); int lo = next(32); return ((long)hi << 32) + (long)lo; }

public long setDecorationSeed(long levelSeed, int minBlockX, int minBlockZ) {
    this.setSeed(levelSeed);
    long i = this.nextLong() | 1L;                                  // slot 5
    long j = this.nextLong() | 1L;                                  // slot 7
    long k = ((long)minBlockX * i + (long)minBlockZ * j) ^ levelSeed;   // slot 9
    this.setSeed(k);
    return k;
}
public void setFeatureSeed(long decorationSeed, int index, int step) {
    long l = decorationSeed + (long)index + (long)(10000 * step);
    this.setSeed(l);
}
```

Backing impl here is `XoroshiroRandomSource` (verified in §2 pseudocode), so:
- `setSeed(s)` → `randomNumberGenerator = new Xoroshiro128PlusPlus(RandomSupport.upgradeSeedTo128bit(s))`
  and `gaussianSource.reset()`.
- Each `next(32)` = `(int)(xoroshiro.nextLong() >>> 32)`; one `WorldgenRandom.nextLong()` therefore
  consumes TWO Xoroshiro outputs: `((long)(int)(x1 >>> 32) << 32) + (long)(int)(x2 >>> 32)`.
- `setDecorationSeed` call chain: fresh 128-bit state from `levelSeed` → 4 Xoroshiro outputs (2 per
  `nextLong`) → mix with block coords → reseed. Args at the call site:
  `setDecorationSeed(level.getSeed(), chunkX*16, chunkZ*16)` (origin of the chunk's bottom section;
  Y never enters).
- This exactly mirrors the carver seeding note (task8 A7) — same `WorldgenRandom`-over-Xoroshiro
  composition, different salt formula.
- Also note `NoiseBasedChunkGenerator.applyCarvers` and this method construct DIFFERENT
  WorldgenRandom instances; nothing is shared across stages.

### 2.2 Biome set used to filter features

The 3×3 chunk neighborhood centered on the decorated chunk (`ChunkPos.rangeClosed(center, 1)`,
which iterates x-outer/z-inner ascending — verified but irrelevant): for each neighbor chunk, every
section's biome `PalettedContainerRO` contributes all palette entries via `getAll`. Collected into
an insertion-ordered `ObjectArraySet`, then `retainAll(biomeSource.possibleBiomes())`.

Determinism: the set's iteration order feeds ONLY membership tests (`IntSet.add` of per-step
indices, then `Arrays.sort`), so even if palette enumeration order varied, output order would not.
Biomes were written by the BIOMES/NOISE stages of those chunks (earlier statuses, complete before
features by ChunkStep dependencies), NOT modified by decoration — so this input does not depend on
features-stage chunk order.

`BiomeSource.possibleBiomes()` = `Suppliers.memoize(() -> collectPossibleBiomes().distinct().collect(ImmutableSet.toImmutableSet()))`
(verified in `BiomeSource.class` `lambda$new$0`); for `MultiNoiseBiomeSource`,
`collectPossibleBiomes()` = `parameters().values().stream().map(Pair::getSecond)` — the
climate-parameter list order from the worldgen JSON/registry. Deterministic per registries.

### 2.3 `getWritableArea(ChunkAccess)` (structure placement clamp)

```java
private static BoundingBox getWritableArea(ChunkAccess chunk) {
    ChunkPos pos = chunk.getPos();
    LevelHeightAccessor h = chunk.getHeightAccessorForGeneration();
    return new BoundingBox(pos.getMinBlockX(), h.getMinY() + 1, pos.getMinBlockZ(),
                           pos.getMinBlockX() + 15, h.getMaxY(), pos.getMinBlockZ() + 15);
}
```
Structure starts write only into the decorated chunk's own column footprint (clamped per chunk) —
structure-block writes do NOT cross chunk borders from this call.

### 2.4 Structure start list order (`StructureManager.startsForStructure(SectionPos, Structure)`)

Verified in `net/minecraft/world/level/StructureManager.class`:

```java
public List<StructureStart> startsForStructure(SectionPos pos, Structure structure) {
    LongSet refs = level.getChunk(pos.x(), pos.z(), ChunkStatus.STRUCTURE_REFERENCES)
                        .getReferencesForStructure(structure);       // this chunk's ref set
    ImmutableList.Builder<StructureStart> b = ImmutableList.builder();
    fillStartsForStructure(structure, refs, b::add);
    return b.build();
}
public void fillStartsForStructure(Structure structure, LongSet refs, Consumer<StructureStart> out) {
    for (long packed : refs) {          // LongSet.iterator() order
        SectionPos sp = SectionPos.of(ChunkPos.unpack(packed), level.getMinSectionY());
        StructureStart start = getStartForStructure(sp, structure,
            level.getChunk(sp.x(), sp.z(), ChunkStatus.STRUCTURE_STARTS));
        if (start != null && start.isValid()) out.accept(start);
    }
}
```

- `getReferencesForStructure` → `structuresRefences` (sic) `HashMap<Structure, LongSet>`
  `getOrDefault(structure, EMPTY_REFERENCE_SET)`; values created as `LongOpenHashSet`
  (`ChunkAccess.lambda$addReferenceForStructure$0`, verified).
- Insertion order of that `LongOpenHashSet` comes from `ChunkGenerator.createReferences`
  (STRUCTURE_REFERENCES stage of the SAME chunk): fixed double loop `x = cx-8 .. cx+8` (outer),
  `z = cz-8 .. cz+8` (inner), adding `ChunkPos.pack(x, z)` when a valid start's BB intersects the
  chunk footprint. fastutil `LongOpenHashSet` has no per-run hash randomization → identical
  insertion sequence ⇒ identical iteration order. Each neighbor contributes ≤1 ref per structure
  (one start per structure per chunk), so per-structure insertion order = the fixed x/z scan order,
  independent of the (identity-hashed, run-varying) iteration order of the neighbor's
  `getAllStarts()` HashMap.
- ⇒ the starts LIST order — the only place where multiple placements share one RNG stream — is a
  pure function of state established at the STRUCTURE_STARTS/REFERENCES stages. Bit-identical
  01–06 stage dumps imply bit-identical starts lists.

### 2.5 Early exits / debug switches

- `SharedConstants.debugVoidTerrain(ChunkPos)` — returns false unless
  `DEBUG_ONLY_GENERATE_HALF_THE_WORLD` (static final, dev-only) — production no-op.
- `SharedConstants.DEBUG_DISABLE_FEATURES` guards the whole call in `generateFeatures`;
  `DEBUG_FEATURE_COUNT` gates only the tracker. Both static finals, false in release.
- `level.setCurrentlyGenerating(Supplier<String>)` — diagnostics for crash reports only; no
  worldgen effect.
- `DebugLevelSource.applyBiomeDecoration` overrides the whole method (block-grid debug world);
  irrelevant for `NoiseBasedChunkGenerator` worlds (no override there — grep of
  `NoiseBasedChunkGenerator.class` shows no `applyBiomeDecoration` constant).

### 2.6 Registry order underpinning `structuresByStep`

`Registry.stream()` (default method, verified) = `StreamSupport.stream(spliterator(), false)` over
`MappedRegistry.iterator()` = `byId` ObjectList iteration = registration order (datapack registry
load order). `Collectors.groupingBy` keyed by `Integer` step ordinal (value-hashed HashMap —
deterministic), downstream ArrayList preserves encounter order. ⇒ per-step structure order is a
pure function of registry contents.

### 2.7 `PlacedFeature.placeWithBiomeCheck` (for completeness)

```java
public boolean placeWithBiomeCheck(WorldGenLevel level, ChunkGenerator gen, RandomSource random, BlockPos origin) {
    return placeWithContext(new PlacementContext(level, gen, Optional.of(this)), random, origin);
}
private boolean placeWithContext(PlacementContext ctx, RandomSource random, BlockPos origin) {
    Stream<BlockPos> positions = Stream.of(origin);
    for (PlacementModifier pm : this.placement)
        positions = positions.flatMap(pos -> pm.getPositions(ctx, random, pos));
    ConfiguredFeature<?,?> cf = this.feature.value();
    MutableBoolean placed = new MutableBoolean();
    positions.forEach(pos -> { if (cf.place(ctx.getLevel(), ctx.generator(), random, pos)) placed.setTrue(); });
    return placed.isTrue();
}
```
Sequential stream, single `random` threaded through modifiers then the feature — deterministic
given RNG state + world state at call time. The `Optional.of(this)` is what BiomeFilter uses for
the per-position biome check. (Feature internals are out of A2 scope.)

## 3. `FeatureSorter.buildFeaturesPerStep` — is the per-step list order a pure function of registries?

`net/minecraft/world/level/biome/FeatureSorter.class`, full disassembly + BootstrapMethods:

```java
public static <T> List<StepFeatureData> buildFeaturesPerStep(
        List<T> biomes, Function<T, List<HolderSet<PlacedFeature>>> getter, boolean topLevel) {
    Object2IntMap<PlacedFeature> firstSeenIndex = new Object2IntOpenHashMap<>();  // slot 3
    MutableInt nextIndex = new MutableInt(0);                                     // slot 4
    Comparator<FeatureData> cmp =                                                 // slot 5
        Comparator.comparingInt(FeatureData::step)              // bootstrap 0
                  .thenComparingInt(FeatureData::featureIndex); // bootstrap 1
    Map<FeatureData, Set<FeatureData>> graph = new TreeMap<>(cmp);                // slot 6
    int maxSteps = 0;                                                             // slot 7

    for (T biome : biomes) {
        List<FeatureData> flat = Lists.newArrayList();                            // slot 10
        List<HolderSet<PlacedFeature>> perStep = getter.apply(biome);             // slot 11
        maxSteps = Math.max(maxSteps, perStep.size());
        for (int step = 0; step < perStep.size(); ++step)
            for (Holder<PlacedFeature> h : perStep.get(step)) {                   // HolderSet order = JSON list order
                PlacedFeature pf = h.value();
                flat.add(new FeatureData(
                    firstSeenIndex.computeIfAbsent(pf, k -> nextIndex.getAndIncrement()), // lambda$0
                    step, pf));
            }
        for (int i = 0; i < flat.size(); ++i) {
            Set<FeatureData> succ = graph.computeIfAbsent(flat.get(i),
                k -> new TreeSet<>(cmp));                                          // lambda$1
            if (i < flat.size() - 1) succ.add(flat.get(i + 1));   // edge: element -> its successor within this biome
        }
    }

    Set<FeatureData> finished   = new TreeSet<>(cmp);                             // slot 8
    Set<FeatureData> inProgress = new TreeSet<>(cmp);                             // slot 9
    List<FeatureData> postOrder = Lists.newArrayList();                           // slot 10
    for (FeatureData node : graph.keySet()) {                     // TreeMap order = (step, featureIndex)
        if (!inProgress.isEmpty()) throw new IllegalStateException("...DFS bork...");
        if (finished.contains(node)) continue;
        if (Graph.depthFirstSearch(graph, finished, inProgress, postOrder::add, node)) {  // true = cycle
            if (!topLevel) throw new IllegalStateException("Feature order cycle found");
            // culprit-minimization loop (diagnostic only): repeatedly remove biomes whose
            // removal does NOT break the cycle (re-adding those whose removal does), until
            // fixpoint, then throw ISE("Feature order cycle found, involved sources: " + list).
        }
    }
    Collections.reverse(postOrder);                               // reverse post-order = topo order
    ImmutableList.Builder<StepFeatureData> out = ImmutableList.builder();
    for (int step = 0; step < maxSteps; ++step) {
        List<PlacedFeature> list = postOrder.stream()
            .filter(fd -> fd.step() == step)                      // lambda$2, bootstrap 6
            .map(FeatureData::feature)                            // bootstrap 7
            .collect(Collectors.toList());
        out.add(new StepFeatureData(list));                       // canonical ctor adds
                                                                  // indexMapping = Util.createIndexIdentityLookup(list)
    }
    return out.build();
}
```

`FeatureData` = local record `(int featureIndex, int step, PlacedFeature feature)`;
`featureIndex` = global first-encounter counter (used ONLY as comparator tiebreak here — the salt
in §2 is the per-step list position, not this value).
`StepFeatureData.indexMapping` = `Util.createIndexIdentityLookup(features)`: `<8` elements →
`ReferenceImmutableList::indexOf` (reference equality); `>=8` → `Reference2IntOpenHashMap`
(defaultReturnValue −1) keyed by reference. Registry holders hand out the same `PlacedFeature`
instances everywhere, so reference lookups are consistent.

**Determinism verdict for §3:** every ordered container is either explicitly comparator-ordered
(`TreeMap`/`TreeSet` on `(step, featureIndex)`), insertion-ordered (`ArrayList`, `ImmutableList`,
`ImmutableSet`), or value-hashed on stable keys (`Object2IntOpenHashMap` keyed by `PlacedFeature`
records — value semantics; fastutil, no hash randomization). No identity-hash-ordered iteration
anywhere on this path. Inputs: `List.copyOf(possibleBiomes())` (ImmutableSet insertion order =
biome-source parameter order, §2.2) and each biome's `features()` list-of-HolderSets (registry
JSON order). ⇒ `featuresPerStep`, including each per-step list order and index mapping, is a pure
function of registry contents (+ biome source config). Identical across runs and across JVMs for
the same jar + datapacks. This underpins the "feature-index needs no recording" claim.

## 4. Call-site census (full class tree, `command grep -rl`)

`applyBiomeDecoration` — 3 hits total, nothing else:

| class | method | classification |
|---|---|---|
| `net/minecraft/world/level/chunk/ChunkGenerator.class` | declaration + self constants | — |
| `net/minecraft/world/level/chunk/status/ChunkStatusTasks.class` | `generateFeatures` (only call) | worldgen FEATURES stage — the one that matters |
| `net/minecraft/world/level/levelgen/DebugLevelSource.class` | override (debug grid world) | not worldgen-relevant for normal worlds |

`setDecorationSeed` — 3 hits total, nothing else:

| class | method | classification |
|---|---|---|
| `net/minecraft/world/level/levelgen/WorldgenRandom.class` | declaration | — |
| `net/minecraft/world/level/chunk/ChunkGenerator.class` | `applyBiomeDecoration` | FEATURES stage (Xoroshiro-backed) |
| `net/minecraft/world/level/levelgen/NoiseBasedChunkGenerator.class` | `spawnOriginalMobs` | SPAWN stage (post-features; `new WorldgenRandom(new LegacyRandomSource(uniqueSeed))`, seeded with `region.getCenter()` min-block coords, feeds `NaturalSpawner`) — outside 07_features dump scope but same salt scheme; note the LEGACY backing there vs Xoroshiro here |

Negative findings, stated explicitly:
- No other call sites of `applyBiomeDecoration` or `setDecorationSeed` anywhere under the extracted
  tree (searched `.` = net/, com/, assets/, data/ — only the 3+3 classes above matched).
- `NoiseBasedChunkGenerator` does NOT override `applyBiomeDecoration` (no such constant in its
  pool); the `ChunkGenerator` body above is exactly what runs for normal worlds.
- No `generateFeatures`-adjacent alternate decoration path exists in `ChunkStatusTasks` (the only
  producer of the FEATURES status; `ChunkPyramid`/`ChunkStep` wiring is sibling-slice territory).

## 5. Implications for the order manifest (ADR-007 Tier-2 gate)

Verdict: **hypothesis CONFIRMED — per-chunk decoration order is the only free variable; the
manifest needs only the ordered list of chunk positions entering `applyBiomeDecoration`.**
`(chunk, step, feature-index)` granularity is unnecessary.

Justification chain (each link bytecode-verified above):
1. Decoration seed: pure function of `(worldSeed, chunkX*16, chunkZ*16)` (§2.1). The only
   nondeterministic input (`generateUniqueSeed`) is overwritten before first use.
2. Iteration skeleton: steps `0..max(11, stepDataCount)`; per step, structures (registry `byId`
   order, §2.6) then features (ascending per-step-list index, §2/§3). Both orders are pure
   functions of registry contents. RNG is re-seeded (`setFeatureSeed`) before every item, so item
   N's variable RNG consumption cannot perturb item N+1.
3. The single shared-stream case (multiple starts of one structure) has a list order fixed by
   pre-features stage data (§2.4); Tier-1 parity of stages 01–06 pins it.
4. The biome filter set only gates membership; its iteration order is provably output-neutral
   (`IntSet` + `Arrays.sort`, §2.2), and its contents come from pre-features biome data.
5. What is NOT fixed by (seed, registries, chunk pos): the world state a feature reads/writes —
   neighbor blocks and neighbor heightmaps mutate as OTHER chunks decorate (features may place
   across borders via placement-modifier offsets; `primeHeightmaps` runs per chunk at its own task
   start, §1). That is exactly the cross-chunk ordering dependence the manifest exists to replay.

Manifest design consequences:
- Record: monotonically appended `(chunkX, chunkZ)` at `applyBiomeDecoration` entry (equivalently
  `ChunkStatusTasks.generateFeatures` after the debug-features guard). Appending must be
  thread-safe: the chunk system may run feature tasks on worker threads; whether two features
  tasks can be in flight simultaneously (they'd have to be far enough apart that their read/write
  regions are disjoint, in which case they commute and any recorded serialization replays
  bit-exactly) is scheduler behavior NOT verified in this slice — treat "append order = a valid
  serialization" as safe either way, since overlapping regions cannot legally interleave writes.
- Worth recording per entry as a cheap cross-check (optional): the 64-bit `decorationSeed` returned
  by `setDecorationSeed` — C side recomputes it from (seed, chunk pos) and any mismatch localizes a
  Xoroshiro/seed-plumbing bug to a chunk before diffing blocks.
- Do NOT record step/feature indices — they are derivable; recording them would only be useful as a
  debug trace, and §2/§3 show they cannot vary for fixed registries. If the C port ever supports
  datapack mutation, revalidate §3's inputs (possibleBiomes order, HolderSet JSON order) rather
  than adding manifest granularity.
- Replay requirement on the C side: features for chunk C must run with the 3×3 (biomes) /
  cross-border write neighborhood in exactly the state the manifest order implies — i.e. replay
  strictly serially in manifest order; also honor `shouldGenerateStructures()` (a `WorldOptions`
  flag, part of run config, must match the golden run).
