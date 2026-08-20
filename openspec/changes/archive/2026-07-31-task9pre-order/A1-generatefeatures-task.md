# A1 — Features-stage task & step wiring: `ChunkStatusTasks.generateFeatures` + FEATURES `ChunkStep` (MC 26.2, unobfuscated server bytecode)

Source of truth: `javap -p -c -constants` (and `javap -v` for BootstrapMethods) against
`/home/ubuntu/projects/hyperchunk/tools/golden/libs/extracted/server-26.2.jar` /
`tools/golden/work/server`, all disassembled in this session. All pseudocode is a 1:1
reconstruction from bytecode. No vanilla-source guessing.

Companion notes (task9pre-order siblings): the internals of
`ChunkGenerator.applyBiomeDecoration` (decoration seed, step loop, feature iteration) are NOT in
this note — this note owns the task body, the FEATURES step definition/radii, the `ChunkStep.apply`
choke point, and the light stages.

---

## 1. Stage task: `ChunkStatusTasks.generateFeatures`

`net/minecraft/world/level/chunk/status/ChunkStatusTasks.class`:

```java
public static CompletableFuture<ChunkAccess> generateFeatures(
        WorldGenContext ctx, ChunkStep step, StaticCache2D<GenerationChunkHolder> cache, ChunkAccess chunk) {
    ServerLevel level = ctx.level();                                        // slot 4
    Heightmap.primeHeightmaps(chunk, EnumSet.of(                            // §1.1 — BEFORE region ctor
        Heightmap.Types.MOTION_BLOCKING,
        Heightmap.Types.MOTION_BLOCKING_NO_LEAVES,
        Heightmap.Types.OCEAN_FLOOR,
        Heightmap.Types.WORLD_SURFACE));                                    // EnumSet.of(E,E,E,E), this arg order
    WorldGenRegion region = new WorldGenRegion(level, cache, step, chunk);  // slot 5 — §1.2
    if (!SharedConstants.DEBUG_DISABLE_FEATURES) {                          // §1.5
        ctx.generator().applyBiomeDecoration(                               // §1.3 — THE decoration entry point
            region,                                                         // WorldGenLevel
            chunk,                                                          // ChunkAccess (target)
            level.structureManager().forWorldGenRegion(region));            // StructureManager
    }
    Blender.generateBorderTicks(region, chunk);                             // §1.4 — unconditional, after decoration
    return CompletableFuture.completedFuture(chunk);
}
```

Bytecode anchors: `primeHeightmaps` at bc 6–22 (getstatics in order MOTION_BLOCKING,
MOTION_BLOCKING_NO_LEAVES, OCEAN_FLOOR, WORLD_SURFACE → `EnumSet.of(E,E,E,E)` →
`invokestatic Heightmap.primeHeightmaps:(Lnet/.../ChunkAccess;Ljava/util/Set;)V`);
`new WorldGenRegion` at 25–34; `DEBUG_DISABLE_FEATURES ifne 65` at 39–42; decoration call
`invokevirtual #196 ChunkGenerator.applyBiomeDecoration:(Lnet/minecraft/world/level/WorldGenLevel;Lnet/minecraft/world/level/chunk/ChunkAccess;Lnet/minecraft/world/level/StructureManager;)V`
at 62; `Blender.generateBorderTicks` at 65–68; `completedFuture` at 71–75.

Key deltas vs the carvers task (`generateCarvers`, task8 A1 §1):
- Heightmaps are primed HERE (not at carvers), before the region exists.
- The whole body is SYNCHRONOUS on the calling thread — the returned future is already completed.
- No `Blender.addAroundOldChunksCarvingMaskFilter`; instead `generateBorderTicks` AFTER decoration.
- NO seed / RandomState / BiomeManager arguments are passed: `applyBiomeDecoration(WorldGenLevel,
  ChunkAccess, StructureManager)` derives everything from the region (sibling note's territory).
- NO status bump inside the task — that happens in `ChunkStep.apply`'s continuation (§3.2).

### 1.1 `Heightmap.primeHeightmaps(ChunkAccess, Set<Heightmap.Types>)` — 1:1

`net/minecraft/world/level/levelgen/Heightmap.class`:

```java
public static void primeHeightmaps(ChunkAccess chunk, Set<Heightmap.Types> types) {
    if (types.isEmpty()) return;
    int n = types.size();                                               // slot 2
    ObjectArrayList<Heightmap> list = new ObjectArrayList<>(n);         // slot 3 (fastutil)
    ObjectListIterator<Heightmap> it = list.iterator();                 // slot 4 — created on EMPTY list, reused
    int top = chunk.getHighestSectionPosition() + 16;                   // slot 5
    BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();      // slot 6
    for (int x = 0; x < 16; ++x) {                                      // slot 7, x OUTER
        for (int z = 0; z < 16; ++z) {                                  // slot 8, z INNER
            list.clear-equivalent: // NO — list is re-FILLED each column:
            for (Heightmap.Types t : types)                             // Set iteration (EnumSet → decl order)
                list.add(chunk.getOrCreateHeightmapUnprimed(t));
            for (int y = top - 1; y >= chunk.getMinY(); --y) {          // slot 9, downward scan
                pos.set(x, y, z);
                BlockState state = chunk.getBlockState(pos);            // slot 10
                if (state.is(Blocks.AIR)) continue;                     // skip air fast-path
                while (it.hasNext()) {
                    Heightmap hm = it.next();
                    if (hm.isOpaque.test(state)) {
                        hm.setHeight(x, z, y + 1);
                        it.remove();                                    // this heightmap done for column
                    }
                }
                if (list.isEmpty()) break;                              // all types resolved → next column
                it.back(n);                                             // rewind iterator for next y
            }
        }
    }
}
```

(Faithful to bytecode: the iterator is a single `ObjectListIterator` allocated once at bc 26–32 and
rewound with `back(n)` at bc 239–247; `getOrCreateHeightmapUnprimed` re-adds per column, bc 72–115.
Caveat: because `list` is never cleared between columns and `add` happens per column, entries
accumulate but `it.remove()` deletes resolved ones — net effect is the vanilla column scan. For the
C port treat it as: per column, for each type, height = 1 + highest y with `isOpaque(state)`, else
unset.) RNG-free, pure function of chunk blocks.

- `ChunkAccess.getOrCreateHeightmapUnprimed(Types)` = `heightmaps.computeIfAbsent(type,
  t -> new Heightmap(this, t))` (map is an `EnumMap`-like `Map` field; verified accessor exists,
  body not security-relevant).
- Priming target: the CENTER chunk only. Types primed = exactly the 4 FINAL_HEIGHTMAPS (§2.6).

### 1.2 `WorldGenRegion.<init>(ServerLevel, StaticCache2D, ChunkStep, ChunkAccess)` — re-verified this session

putfield order: `blockTicks`/`fluidTicks` (WorldGenTickAccess), `subTickCount = new AtomicLong()`,
`generatingStep = step`, `cache`, `center = chunk`, `level`, `seed = level.getSeed()`, `levelData`,
`random = level.getChunkSource().randomState().getOrCreateRandomFactory(WORLDGEN_REGION_RANDOM).at(center.getPos().getWorldPosition())`,
`dimensionType`, `biomeManager = new BiomeManager(this, BiomeManager.obfuscateSeed(this.seed))`,
`centerChunkX/Z = center.getPos().x()/z()`, `writeRadius = step.blockStateWriteRadius()` (= 1 for
FEATURES, §2.3).

`getChunk(int x, int z)` → `getChunk(x, z, ChunkStatus.EMPTY, true)`:

```java
int dist = center.getPos().getChessboardDistance(x, z);          // max(|dx|,|dz|)
ChunkStatus max = dist >= generatingStep.directDependencies().size()
                ? null
                : generatingStep.directDependencies().get(dist);
if (max != null) {
    GenerationChunkHolder h = cache.get(x, z);
    if (requestedStatus.isOrBefore(max)) {
        ChunkAccess c = h.getChunkIfPresentUnchecked(max);
        if (c != null) return c;
    }
}
throw crash("Requested chunk unavailable during world generation");  // CrashReport with dist/status details
```

Note: `getChunkIfPresentUnchecked(max)` returns the LIVE ProtoChunk object once it has reached at
least `max` — including any mutations applied by later stages (e.g. a neighbor already decorated).
This is the mechanism that makes cross-chunk decoration order observable (§6).

### 1.3 The decoration entry point — exact identity

`ChunkGenerator.applyBiomeDecoration(WorldGenLevel, ChunkAccess, StructureManager) : void` —
CONCRETE method on `net/minecraft/world/level/chunk/ChunkGenerator.class` (has Code attr, ~bc 534ff;
uses the memoized `featuresPerStep` field: `Supplier<List<FeatureSorter$StepFeatureData>>` built with
`Suppliers.memoize` in the ctor). Internals = sibling note. Overrides in 26.2 (grep census, §4):
ONLY `DebugLevelSource` overrides it; `NoiseBasedChunkGenerator` and `FlatLevelSource` have zero
constant-pool references to the name → both inherit `ChunkGenerator`'s implementation.

### 1.4 `Blender.generateBorderTicks(WorldGenRegion, ChunkAccess)` — fresh-world no-op

```java
public static void generateBorderTicks(WorldGenRegion region, ChunkAccess chunk) {
    if (SharedConstants.DEBUG_DISABLE_BLENDING) return;
    ChunkPos pos = chunk.getPos();
    boolean old = chunk.isOldNoiseGeneration();
    ...
    BlendingData bd = chunk.getBlendingData();
    if (bd == null) return;                    // bc 50-55 — ALWAYS taken on a fresh world
    // (only for pre-1.18 upgrade chunks: schedules border block/fluid ticks along old/new seams
    //  via generateBorderTick(chunk, pos) over Direction.Plane.HORIZONTAL neighbors — RNG-free)
}
```

For hyperchunk golden worlds (fresh, no upgraded chunks) this returns at the `bd == null` check.
No RNG anywhere in the method; it only schedules ticks (post-worldgen simulation concern).

### 1.5 `SharedConstants.DEBUG_DISABLE_FEATURES`

Non-compile-time-constant `static final boolean`, initialized in `SharedConstants.<clinit>` via
`debugFlag("DISABLE_FEATURES")`; `debugFlag` returns false unless `DEBUG_ENABLED` (dev builds) —
always FALSE on the production server. Same mechanism as `DEBUG_DISABLE_CARVERS`/`_BLENDING`.

---

## 2. FEATURES step definition: `ChunkPyramid` static init

`ChunkPyramid.<clinit>` builds `GENERATION_PYRAMID` (steps EMPTY, STRUCTURE_STARTS,
STRUCTURE_REFERENCES, BIOMES, NOISE, SURFACE, CARVERS, FEATURES, INITIALIZE_LIGHT, LIGHT, SPAWN,
FULL — invokedynamic bootstraps #13..#24 → `lambda$static$0..11`), then `LOADING_PYRAMID`
(bootstraps #25..#36 → `lambda$static$12..23`), then
`SAFETY_MARGIN_CHUNKS = 2 * (32 + GENERATION_PYRAMID.getStepTo(FULL).accumulatedDependencies().size() + 1)`
and `MAX_CHUNK_COORDINATE_VALUE`.

### 2.1 GENERATION FEATURES step: `lambda$static$7` (bootstrap #20; task via bootstrap #6)

```java
.step(ChunkStatus.FEATURES, builder -> builder
    .addRequirement(ChunkStatus.STRUCTURE_STARTS, 8)
    .addRequirement(ChunkStatus.CARVERS, 1)
    .blockStateWriteRadius(1)
    .setTask(ChunkStatusTasks::generateFeatures))   // bootstrap #6 = REF_invokeStatic generateFeatures
```

(Bytecode: `getstatic STRUCTURE_STARTS; bipush 8; addRequirement`, `getstatic CARVERS; iconst_1;
addRequirement`, `iconst_1; blockStateWriteRadius`, `indy #6:doWork; setTask`.)

`ChunkPyramid$Builder.step(status, op)`: first step → `new ChunkStep$Builder(status)` (parent null);
otherwise `new ChunkStep$Builder(status, steps.getLast())` — asserts
`status.getIndex() == parent.targetStatus.getIndex() + 1` (strict linear pyramid), then
`steps.add(op.apply(builder).build())`.

### 2.2 `ChunkStep$Builder` semantics (re-verified) and resulting radii

- 2-arg ctor: `blockStateWriteRadius = -1` (default), `task = ChunkStatusTasks::passThrough`
  (bootstrap #0 of ChunkStep$Builder → `REF_invokeStatic ChunkStatusTasks.passThrough`),
  `directDependenciesByRadius = { parent.targetStatus }` = `[CARVERS]` for FEATURES.
- `addRequirement(status, radius)`: if `radius+1 > len`: allocate `radius+1`, `Arrays.fill(new,
  status)`, then for `i in 0..min(radius+1, oldLen)-1`: `new[i] = ChunkStatus.max(old[i], status)`.
  If it does NOT grow, the merge loop runs over `0..min(radius+1, len)-1` in place.
- Applying the chain for FEATURES:
  1. init: `[CARVERS]`
  2. `addRequirement(STRUCTURE_STARTS, 8)`: grow to 9 filled with SS; merge idx 0:
     `max(CARVERS, SS) = CARVERS` → `[CARVERS, SS, SS, SS, SS, SS, SS, SS, SS]`
  3. `addRequirement(CARVERS, 1)`: no grow; merge idx 0,1 with CARVERS →
     **directDependencies = `[0]=CARVERS, [1]=CARVERS, [2..8]=STRUCTURE_STARTS`** (size 9).
- ⇒ Neighbor window during decoration of chunk C: chunks at chessboard distance ≤ 1 are readable
  at ≥ CARVERS (post-carving terrain, pre-features unless already decorated — §6); distance 2..8
  readable at ≥ STRUCTURE_STARTS only; distance ≥ 9 → crash path.
- (Contrast, same session: CARVERS step = `lambda$static$6`: `addRequirement(SS,8);
  blockStateWriteRadius(0)` → `[0]=SURFACE, [1..8]=SS`; SURFACE = `lambda$static$5`:
  `addRequirement(SS,8); addRequirement(BIOMES,1); blockStateWriteRadius(0)`.)

### 2.3 `blockStateWriteRadius(1)` — enforcement

`WorldGenRegion.writeRadius = 1` for FEATURES. `setBlock(pos, state, flags, recursionLeft)` first
calls `ensureCanWrite(pos)`:

```java
public boolean ensureCanWrite(BlockPos pos) {
    if (!isWithinWriteZone(pos)) {           // blockToSectionCoord(x/z) vs centerChunkX/Z:
        Util.logAndPauseIfInIde(...);        //   |cx - centerChunkX| <= writeRadius && |cz - ...| <= writeRadius
        return false;                        // write silently DROPPED (log only), no throw
    }
    if (center.isUpgrading())                // pre-1.18 upgrade path only
        return !center.getHeightAccessorForGeneration().isOutsideBuildHeight(pos.getY());
    return true;
}
```

26.2 delta / correction to task8 note A1 §2.1: `ensureCanWrite` has NO per-distance Y-slack formula
in 26.2 — `isWithinWriteZone` is a pure per-axis chunk-distance check against `writeRadius`; the
only Y check is the `isUpgrading()` branch (irrelevant for fresh worlds). So features may WRITE
blocks anywhere in the 3×3 chunk square centered on C, full height.

### 2.4 `accumulatedDependencies` — scheduling only (negative finding)

`build()` = `new ChunkStep(status, new ChunkDependencies(ImmutableList.copyOf(directDeps)),
new ChunkDependencies(copyOf(buildAccumulatedDependencies())), blockStateWriteRadius, task)`.
`buildAccumulatedDependencies` merges parent's accumulated deps shifted by
`getRadiusOfParent(parent.targetStatus)` (= highest index whose status `isOrAfter` it; for FEATURES
that is index 1, since `directDeps[1] = CARVERS`). Consumers (grep census): only
`ChunkGenerationTask` (`getAccumulatedRadiusOf`, `accumulatedDependencies`) — scheduling/ticket
math. `WorldGenRegion` uses `directDependencies` exclusively. Not needed for worldgen math in C.

### 2.5 LOADING_PYRAMID FEATURES: `lambda$static$19` = identity

`lambda$static$19(ChunkStep$Builder s) { return s; }` (aload_0; areturn). So the loading pyramid's
FEATURES step keeps builder defaults: NO requirements beyond `[CARVERS]`, `writeRadius = -1`, and
`task = ChunkStatusTasks::passThrough`:

```java
public static CompletableFuture<ChunkAccess> passThrough(WorldGenContext ctx, ChunkStep step,
        StaticCache2D<GenerationChunkHolder> cache, ChunkAccess chunk) {
    return CompletableFuture.completedFuture(chunk);
}
```

⇒ Chunks loaded from disk NEVER re-decorate. Decoration runs exactly once per chunk per world,
via the generation pyramid.

### 2.6 `ChunkStatus` registration (re-verified)

`FEATURES = register("features", /*parent*/ CARVERS, FINAL_HEIGHTMAPS, ChunkType.PROTOCHUNK)`;
`INITIALIZE_LIGHT = register("initialize_light", FEATURES, FINAL_HEIGHTMAPS, PROTOCHUNK)`;
`LIGHT = register("light", INITIALIZE_LIGHT, FINAL_HEIGHTMAPS, PROTOCHUNK)`.
`FINAL_HEIGHTMAPS = EnumSet.of(OCEAN_FLOOR, WORLD_SURFACE, MOTION_BLOCKING,
MOTION_BLOCKING_NO_LEAVES)` — statuses from CARVERS onward carry it as `heightmapsAfter`.
`register` stores the EnumSet on the status (`heightmapsAfter` field) and registers in
`BuiltInRegistries.CHUNK_STATUS`.

Consequence (verified in `ProtoChunk.setBlockState(BlockPos, BlockState, int)`): every block write
during FEATURES ends with

```java
EnumSet<Heightmap.Types> after = getPersistedStatus().heightmapsAfter();  // = FINAL_HEIGHTMAPS at CARVERS+
// prime any of the 4 that are missing (batch primeHeightmaps), then:
for (Heightmap.Types t : after) heightmaps.get(t).update(xRel, y, zRel, state);
```

so heightmaps stay incrementally correct DURING decoration — a feature placed early changes the
heightmap values later features see. (Also in setBlockState: if `status.isOrAfter(INITIALIZE_LIGHT)`
it pokes the light engine — during FEATURES the persisted status is CARVERS, so that branch is
dead.) `ChunkAccess.getHeight(type, x, z)` on a chunk missing that heightmap lazily
`primeHeightmaps(this, EnumSet.of(type))` (bc 65–73 after an IDE-only log) — deterministic, matters
when decoration reads a NEIGHBOR's heightmap (neighbors are not primed by `generateFeatures`,
only the center chunk is).

---

## 3. `ChunkStep.apply` — the choke point (confirmed)

### 3.1 Record + signature

`net/minecraft/world/level/chunk/status/ChunkStep.class` = final Record
`(ChunkStatus targetStatus, ChunkDependencies directDependencies, ChunkDependencies
accumulatedDependencies, int blockStateWriteRadius, ChunkStatusTask task)`.

```java
public CompletableFuture<ChunkAccess> apply(WorldGenContext ctx,
        StaticCache2D<GenerationChunkHolder> cache, ChunkAccess chunk) {
    if (chunk.getPersistedStatus().isBefore(this.targetStatus)) {
        ProfiledDuration d = JvmProfiler.INSTANCE.onChunkGenerate(chunk.getPos(),
                ctx.level().dimension(), this.targetStatus.getName());
        return this.task.doWork(ctx, this, cache, chunk)
                   .thenApply(result -> completeChunkGeneration(result, d));   // lambda$apply$0
    } else {
        return this.task.doWork(ctx, this, cache, chunk);                       // no bump, no profiler
    }
}
```

EXACT signature in 26.2:
`public java.util.concurrent.CompletableFuture<net.minecraft.world.level.chunk.ChunkAccess>
apply(net.minecraft.world.level.chunk.status.WorldGenContext,
net.minecraft.util.StaticCache2D<net.minecraft.server.level.GenerationChunkHolder>,
net.minecraft.world.level.chunk.ChunkAccess)` — matches
`tools/golden/stage-dump-mod/.../mixin/ChunkStepMixin.java` (`@Inject(method = "apply",
at = @At("RETURN"), cancellable = true)`, params `(WorldGenContext, StaticCache2D, ChunkAccess,
CallbackInfoReturnable<CompletableFuture<ChunkAccess>>)`). ✔ still valid.

### 3.2 `completeChunkGeneration(ChunkAccess, ProfiledDuration)` — the status bump

```java
private ChunkAccess completeChunkGeneration(ChunkAccess chunk, ProfiledDuration d) {
    if (chunk instanceof ProtoChunk pc && pc.getPersistedStatus().isBefore(this.targetStatus))
        pc.setPersistedStatus(this.targetStatus);
    if (d != null) d.finish(true);
    return chunk;
}
```

So after FEATURES the protochunk's persisted status becomes FEATURES inside the `thenApply`
continuation — i.e. still within the future the mixin re-wraps. The mixin's dump continuation is
appended AFTER this one (`cir.getReturnValue().thenApply(...)`), so dumps observe the post-bump,
post-decoration chunk. Since `generateFeatures` returns an already-completed future, the entire
chain (task body → status bump → mixin dump) executes synchronously on the thread that called
`apply`.

### 3.3 Sole caller chain (grep census over ALL classes referencing `ChunkStep`)

Classes whose constant pool references `chunk/status/ChunkStep`: ChunkStatusTask, ChunkPyramid,
ChunkStep$Builder, ChunkPyramid$Builder, ChunkStatusTasks, ChunkStep, WorldGenRegion, ChunkLevel,
GeneratingChunkMap, GenerationChunkHolder, ChunkMap, ChunkGenerationTask. Disassembling each for
`Method ...ChunkStep.apply:` invocations: **ONLY `net.minecraft.server.level.ChunkMap` calls
`ChunkStep.apply`** (in `ChunkMap.applyStep`). Chain:

```
ChunkGenerationTask (lambda, bc 432: invokevirtual GenerationChunkHolder.applyStep)
  → GenerationChunkHolder.applyStep(ChunkStep, GeneratingChunkMap, StaticCache2D)
      - if isStatusDisallowed(target) → UNLOADED_CHUNK_FUTURE
      - if acquireStatusBump(target)  → map.applyStep(this, step, cache).handle(lambda$applyStep$0)
        (acquireStatusBump = run-once guard per status per holder)
      - else → getOrCreateFuture(target)   // already claimed by another path
  → ChunkMap.applyStep(GenerationChunkHolder, ChunkStep, StaticCache2D)   [GeneratingChunkMap impl]
      - if step.targetStatus() == EMPTY → scheduleChunkLoad(pos)          // BYPASSES ChunkStep.apply
      - else: parent = cache.get(pos).getChunkIfPresentUnchecked(target.getParent());
              if (parent == null) throw IllegalStateException("Parent chunk missing");
              return step.apply(this.worldGenContext, cache, parent);
  → ChunkStep.apply (§3.1)
```

Notes: the EMPTY step never flows through `apply` (mixin never sees EMPTY); every other status of
BOTH pyramids flows through `apply` exactly once per (chunk, status) thanks to
`acquireStatusBump`. The existing `StageDumper.isGenerationStep` filter
(`ChunkPyramid.GENERATION_PYRAMID.getStepTo(status) == step`) correctly rejects loading-pyramid
steps by identity.

---

## 4. Call-site census / negative findings

All greps binary-aware (`grep -rla`) over `tools/golden/work/server/{net,com}`:

- `generateFeatures`: constant-pool hits ONLY in `ChunkPyramid.class` (bootstrap #6 method handle)
  and `ChunkStatusTasks.class` (declaration). No other call sites exist.
- `applyBiomeDecoration`: hits ONLY `ChunkGenerator.class` (concrete declaration),
  `ChunkStatusTasks.class` (the single call, in `generateFeatures`), `DebugLevelSource.class`
  (override for the debug world type — out of scope). `NoiseBasedChunkGenerator.class` and
  `FlatLevelSource.class`: 0 hits ⇒ no override, both inherit. ⇒ **the features task is the only
  path that decorates chunks during worldgen.**
- `primeHeightmaps` callers: `ChunkStatusTasks.generateFeatures` (the priming in §1),
  `ProtoChunk.setBlockState` (lazy batch-prime of missing `heightmapsAfter` types, §2.6),
  `ChunkAccess.getHeight` (lazy single-type prime, §2.6), `Heightmap.setRawData` (length-mismatch
  repair on load), `SerializableChunkData` (disk load). All deterministic, RNG-free.
- `generateBorderTicks` callers: only `ChunkStatusTasks.generateFeatures` (+ self in Blender).
- `GENERATION_PYRAMID` consumers: `ChunkLevel`, `ChunkGenerationTask` (+ ChunkPyramid itself).

---

## 5. `initializeLight` / `light` — consume-only (no RNG, no decoration)

`ChunkStatusTasks.initializeLight`:

```java
public static CompletableFuture<ChunkAccess> initializeLight(WorldGenContext ctx, ChunkStep step,
        StaticCache2D<GenerationChunkHolder> cache, ChunkAccess chunk) {
    ThreadedLevelLightEngine engine = ctx.lightEngine();
    chunk.initializeLightSources();                       // = skyLightSources.fillFrom(this) — geometric scan
    ((ProtoChunk) chunk).setLightEngine(engine);
    boolean lit = isLighted(chunk);                       // persistedStatus ≥ LIGHT && isLightCorrect()
    return engine.initializeLight(chunk, lit);
}
```

`ChunkStatusTasks.light`:

```java
public static CompletableFuture<ChunkAccess> light(WorldGenContext ctx, ...) {
    boolean lit = isLighted(chunk);
    return ctx.lightEngine().lightChunk(chunk, lit);
}
```

`ThreadedLevelLightEngine.initializeLight(ChunkAccess, boolean)` queues a PRE_UPDATE task
(`lambda$initializeLight$0`: per non-empty section `updateSectionStatus`; NO block writes) then
`supplyAsync(lambda$initializeLight$2: setLightEnabled(pos, retainLight); retainData(pos, false);
return chunk)`. `lightChunk(ChunkAccess, boolean)` = `setLightCorrect(false)`, queue PRE_UPDATE
(`lambda$lightChunk$0`: `propagateLightSources(pos)` unless already lit), then
`supplyAsync(lambda$lightChunk$2: setLightCorrect(true); return chunk)`. Every lambda body
disassembled: exclusively LevelLightEngine calls + flags. **No RandomSource construction, no
`setBlock`, no feature/carver/heightmap interaction. They consume the FEATURES output only.** ✔

(One nuance for dump identity, not RNG: light data itself is computed asynchronously on the light
engine thread; the 07_features dump content — blocks/biomes/heightmaps — is finalized strictly
before INITIALIZE_LIGHT runs.)

---

## 6. Implications for the order manifest (ADR-007 Tier-2)

1. **Hypothesis CONFIRMED at the plumbing level.** `generateFeatures` is a pure function of
   (world seed, registries, chunk pos, contents of the 3×3 neighborhood at call time). It takes no
   RNG and constructs none; all randomness is created inside `applyBiomeDecoration` from
   region-derived seeds (sibling note). The ONLY inputs that vary between runs of the same world
   are the block/heightmap contents of chunks at chessboard distance ≤ 1, which depend on whether
   those neighbors were themselves already decorated — i.e. exactly the per-chunk decoration ORDER.
   `writeRadius = 1` (§2.3) plus live-object reads (§1.2) are the two coupling channels; distance
   2..8 chunks are only at STRUCTURE_STARTS and expose structure-start data (order-independent —
   written once at the STRUCTURE_STARTS stage).
2. **What the manifest must record:** the global sequence of FEATURES-step executions:
   `(dimension, chunkX, chunkZ)` in execution order. Nothing else is free: within-chunk iteration
   (steps → features → placements) is registry/seed-deterministic, heightmap priming is
   content-deterministic, `generateBorderTicks` is a fresh-world no-op, and the light stages are
   RNG-irrelevant. Recording other stages' order is unnecessary for bit-exactness but cheap and
   useful as a sanity cross-check (CARVERS/SURFACE etc. are order-independent by construction —
   radius-0 writes).
3. **Where to hook:** `ChunkStep.apply` remains the single choke point (§3.3) and the existing
   `ChunkStepMixin` signature is correct for 26.2. Because `generateFeatures` is fully synchronous
   inside `apply` (§1, §3.2), appending the order-record in the same RETURN-injected continuation
   the dump uses gives the true execution order — but ONLY per thread. Vanilla runs `apply` for
   different chunks on multiple worker threads; the manifest writer therefore needs a
   process-global monotonic counter (e.g. `AtomicLong.getAndIncrement()` inside the continuation)
   or a synchronized appender, so concurrent FEATURES completions serialize into one total order.
   Caveat to document in the replayer: two chunks decorated concurrently in the golden run are
   provably non-adjacent (scheduler guarantees dependencies; write windows never overlap
   mid-flight), so ANY interleaving-consistent total order of the recorded sequence reproduces the
   same bits — the recorded order is one valid linearization, which is all the C replay needs.
4. **Record at entry or exit?** Exit (RETURN-continuation) is safe because the body is synchronous:
   exit order on a given thread == entry order. If Mojang ever makes a task genuinely async,
   entry-side recording (`@At("HEAD")` before `doWork`) would be the robust choice; note it as a
   version-bump check.
5. **Filters already correct:** loading-pyramid FEATURES is `passThrough` (§2.5) and
   `StageDumper.isGenerationStep` rejects it by step identity; EMPTY never reaches `apply` (§3.3);
   `acquireStatusBump` guarantees at-most-once per chunk — no dedup needed in the manifest format,
   but a duplicate-pos assertion is a free integrity check.
6. **Replay-side requirements extracted from this slice:** before decorating chunk C the C engine
   must (a) prime C's 4 FINAL_HEIGHTMAPS from current blocks (§1.1), (b) keep all 4 heightmaps
   incrementally updated on every block write in the 3×3 window (§2.6 — including writes into
   neighbors), (c) enforce the write window as silent-drop, not crash (§2.3), (d) serve neighbor
   reads from live post-whatever-stage-they-reached state per the manifest order.

---

## 7. OPEN items

- OPEN: `primeHeightmaps`'s odd iterator reuse (`back(n)` without clearing the list between
  columns) — bytecode faithful reading above; recommend a differential probe (Java vs C heightmap
  dump on one chunk) before trusting my "net effect" simplification for columns where
  `getOrCreateHeightmapUnprimed` returns an ALREADY-primed map on later columns (list re-add per
  column with existing heights — `setHeight` overwrites unconditionally, so idempotent, but verify).
- OPEN: `WorldGenRegion.getHeight`/`getBlockState` paths used from inside `applyBiomeDecoration`
  route through `getChunk(x,z)` = status EMPTY request → allowed by any non-null directDeps entry;
  double-check sibling note covers which accessor features actually use for neighbor heightmaps.
- OPEN (cross-note correction): task8 `A1-applycarvers-orchestration.md` §2.1 claims an
  `ensureCanWrite` Y-slack of `16*(dist-writeRadius)`; the 26.2 bytecode disassembled this session
  has no such term (§2.3). Moot for carvers (writeRadius 0, center-only), but the task8 note should
  be amended.
