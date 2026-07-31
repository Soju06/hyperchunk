# A4 — `WorldGenRegion`: the cross-chunk write window (MC 26.2, unobfuscated server bytecode)

Source of truth: `javap -p -c -constants` (and `javap -v` for BootstrapMethods) against
`/home/ubuntu/projects/hyperchunk/tools/golden/libs/extracted/server-26.2.jar` /
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server`. Classes disassembled in this session:
`net/minecraft/server/level/WorldGenRegion.class`,
`net/minecraft/world/level/chunk/status/ChunkStep.class` (+ `$Builder`),
`net/minecraft/world/level/chunk/status/ChunkPyramid.class`,
`net/minecraft/world/level/chunk/status/ChunkDependencies.class`,
`net/minecraft/world/level/chunk/status/ChunkStatus.class`,
`net/minecraft/world/level/chunk/status/ChunkStatusTasks.class`,
`net/minecraft/util/StaticCache2D.class`,
`net/minecraft/world/ticks/WorldGenTickAccess.class`, `ProtoChunkTicks.class`, `SavedTick.class` (+ `$1`),
`net/minecraft/world/level/chunk/ProtoChunk.class`, `ChunkAccess.class`,
`net/minecraft/world/level/levelgen/Heightmap.class`,
`net/minecraft/world/level/LevelReader.class`, `LevelAccessor.class`, `WorldGenLevel.class`,
`net/minecraft/server/level/GenerationChunkHolder.class`, `ChunkGenerationTask.class`,
`net/minecraft/world/level/levelgen/blending/Blender.class`,
`net/minecraft/world/level/block/state/BlockBehaviour$BlockStateBase.class`.
All pseudocode is a 1:1 reconstruction from bytecode. No vanilla-source guessing.

---

## 1. Construction and bounds

### 1.1 Fields

```java
public class WorldGenRegion implements WorldGenLevel {
    private static final Logger LOGGER;
    private final StaticCache2D<GenerationChunkHolder> cache;   // shared with the whole ChunkGenerationTask
    private final ChunkAccess center;
    private final ServerLevel level;
    private final long seed;                                    // = level.getSeed()
    private final LevelData levelData;
    private final RandomSource random;                          // see §5
    private final DimensionType dimensionType;
    private final WorldGenTickAccess<Block> blockTicks;         // pos -> getChunk(pos).getBlockTicks()
    private final WorldGenTickAccess<Fluid> fluidTicks;         // pos -> getChunk(pos).getFluidTicks()
    private final BiomeManager biomeManager;                    // new BiomeManager(this, obfuscateSeed(seed))
    private final ChunkStep generatingStep;
    private @Nullable Supplier<String> currentlyGenerating;     // set externally (ChunkGenerator/WorldGenLevel)
    private final AtomicLong subTickCount;                      // starts at 0, per-region
    private static final Identifier WORLDGEN_REGION_RANDOM;     // "minecraft:worldgen_region_random"
    private final int centerChunkX, centerChunkZ;
    private final int writeRadius;                              // = generatingStep.blockStateWriteRadius()
}
```

### 1.2 Constructor (exact)

```java
public WorldGenRegion(ServerLevel level, StaticCache2D<GenerationChunkHolder> cache,
                      ChunkStep generatingStep, ChunkAccess center) {
    this.blockTicks = new WorldGenTickAccess<>(pos -> this.getChunk(pos).getBlockTicks());
    this.fluidTicks = new WorldGenTickAccess<>(pos -> this.getChunk(pos).getFluidTicks());
    this.subTickCount = new AtomicLong();
    this.generatingStep = generatingStep;
    this.cache = cache;
    this.center = center;
    this.level = level;
    this.seed = level.getSeed();
    this.levelData = level.getLevelData();
    this.random = level.getChunkSource().randomState()
                       .getOrCreateRandomFactory(WORLDGEN_REGION_RANDOM)   // "worldgen_region_random"
                       .at(this.center.getPos().getWorldPosition());       // (chunkX*16, 0, chunkZ*16)
    this.dimensionType = level.dimensionType();
    this.biomeManager = new BiomeManager(this, BiomeManager.obfuscateSeed(this.seed));
    ChunkPos pos = center.getPos();
    this.centerChunkX = pos.x();
    this.centerChunkZ = pos.z();
    this.writeRadius = generatingStep.blockStateWriteRadius();
}
```

### 1.3 Who constructs it — negative finding

`grep -rla 'server/level/WorldGenRegion'` over the class tree finds constant-pool refs in:
`NoiseBasedChunkGenerator`, `StructureManager`, `DebugLevelSource`, `FlatLevelSource`,
`BlendingData`, `Blender`, `ChunkGenerator`, `ChunkStatusTasks`, `WorldGenRegion` itself.
Only **`ChunkStatusTasks`** contains `new WorldGenRegion` (`invokespecial <init>`), at exactly 7 sites:
`generateStructureReferences`, `generateBiomes`, `generateNoise`, `generateSurface`,
`generateCarvers`, `generateFeatures`, `generateSpawn`. All others only *receive* one.
So one region per (chunk, step-task) execution; the FEATURES region for chunk X is created inside
`ChunkStatusTasks.generateFeatures` and is 1:1 with X's features-stage execution.

### 1.4 The FEATURES `ChunkStep` (from `ChunkPyramid.<clinit>` lambda `lambda$static$7`)

```java
// GENERATION_PYRAMID, FEATURES step:
builder(FEATURES, stepToCarvers)          // seeds directDependenciesByRadius = [CARVERS]  (parent at r=0)
    .addRequirement(STRUCTURE_STARTS, 8)  // grow to 9, fill new slots with SS, r0=max(CARVERS,SS)=CARVERS
    .addRequirement(CARVERS, 1)           // r0=max=CARVERS, r1=max(SS,CARVERS)=CARVERS
    .blockStateWriteRadius(1)
    .setTask(ChunkStatusTasks::generateFeatures);
```

`ChunkStep$Builder.addRequirement` semantics (bytecode-verified): array is per-radius
`ChunkStatus[]`, index = chessboard radius; growing fills new tail slots with the added status;
overlapping slots take `ChunkStatus.max`. Result for FEATURES:

```
directDependencies (index = chessboard distance from center):
  [0]=CARVERS [1]=CARVERS [2..8]=STRUCTURE_STARTS          size() = 9
blockStateWriteRadius = 1
```

For comparison (same clinit): CARVERS step = `SS@8, writeRadius 0`; NOISE/SURFACE =
`SS@8 + BIOMES@1, writeRadius 0`; INITIALIZE_LIGHT (lambda$static$9) = `INITIALIZE_LIGHT... n/a` —
requires nothing extra; LIGHT step requires `INITIALIZE_LIGHT@1`.

**Answer to "center ± how many chunks":** for a FEATURES-step region the *writable* window is
**exactly the 3×3 (center ± 1 chunk, chessboard)**; the *readable* window is center ± 8, but with
guaranteed status CARVERS only at distance ≤ 1 and STRUCTURE_STARTS at 2..8 (see §2.2, §3).

### 1.5 `StaticCache2D` (the holder cache handed to the region)

```java
public static <T> StaticCache2D<T> create(int centerX, int centerZ, int radius, Initializer<T> init) {
    // minX = centerX - radius, minZ = centerZ - radius, size = 2*radius + 1 (square)
    // cache = Object[size*size], EAGERLY filled: cache[(x-minX)*sizeZ + (z-minZ)] = init.get(x, z)
}
public T get(int x, int z) { if (!contains(x, z)) throw new IllegalArgumentException(...); return cache[getIndex(x, z)]; }
```

Created in `ChunkGenerationTask.create(GeneratingChunkMap, ChunkStatus targetStatus, ChunkPos)`:
`radius = GENERATION_PYRAMID.getStepTo(targetStatus).getAccumulatedRadiusOf(EMPTY)` — i.e. the cache
covers the whole generation task's accumulated footprint, not just this step. The region's own bounds
enforcement is *not* the cache; it is `generatingStep.directDependencies` + `writeRadius`.

---

## 2. `getChunk` — read gating

### 2.1 Entry points

```java
// WorldGenRegion:
public ChunkAccess getChunk(int x, int z) { return this.getChunk(x, z, ChunkStatus.EMPTY); } // -> LevelReader default -> 4-arg, bool=true
// LevelReader defaults: getChunk(BlockPos) -> getChunk(x>>4, z>>4);  getChunk(x,z,status) -> getChunk(x,z,status,true)
```

### 2.2 The 4-arg override (exact reconstruction)

```java
public ChunkAccess getChunk(int x, int z, ChunkStatus requestedStatus, boolean requireChunk) {
    int distance = this.center.getPos().getChessboardDistance(x, z);
    ChunkStatus maxStatus = distance >= this.generatingStep.directDependencies().size()
            ? null
            : this.generatingStep.directDependencies().get(distance);
    GenerationChunkHolder holder = null;
    if (maxStatus != null) {
        holder = this.cache.get(x, z);
        if (requestedStatus.isOrBefore(maxStatus)) {
            ChunkAccess chunk = holder.getChunkIfPresentUnchecked(maxStatus);
            if (chunk != null) return chunk;
        }
    }
    throw new ReportedException(CrashReport.forThrowable(
        new IllegalStateException("Requested chunk unavailable during world generation"), ...));
}
```

Bytecode facts worth flagging:

- **The `requireChunk` boolean is dead**: no `iload 4` anywhere in the method. Any miss crashes,
  even for `getChunk(..., status, false)` callers (e.g. `LevelReader.getNoiseBiome` passes `false`).
- The chunk returned is `holder.getChunkIfPresentUnchecked(maxStatus)` =
  `futures[maxStatus.getIndex()].getNow(NOT_DONE_YET).orElse(null)` — the future registered for that
  status, but it resolves to the **same live `ProtoChunk` instance** the chunk has at all proto
  statuses, so its *current* content can be arbitrarily further along than `maxStatus`
  (order/scheduling-dependent — see §7).
- FEATURES consequences: `getChunk` succeeds for `EMPTY`-status requests at any distance ≤ 8;
  requests for `BIOMES`-or-later crash at distance ≥ 2 (`BIOMES.isOrBefore(STRUCTURE_STARTS)` is
  false). So **biome reads are hard-bounded to the 3×3** while raw block/height reads are not.

### 2.3 `hasChunk`

```java
public boolean hasChunk(int x, int z) {
    return center.getPos().getChessboardDistance(x, z) < generatingStep.directDependencies().size(); // < 9
}
```

---

## 3. Write path — `ensureCanWrite` + `setBlock`

### 3.1 Write-zone predicate

```java
private boolean isWithinWriteZone(int chunkX, int chunkZ) {
    return Math.abs(this.centerChunkX - chunkX) <= this.writeRadius     // 1 for FEATURES
        && Math.abs(this.centerChunkZ - chunkZ) <= this.writeRadius;
}

public boolean ensureCanWrite(BlockPos pos) {                            // overrides WorldGenLevel default (return true)
    if (!isWithinWriteZone(pos)) {
        Util.logAndPauseIfInIde("Detected setBlock in a far chunk [" + cx + ", " + cz + "], pos: " + pos
            + ", status: " + generatingStep.targetStatus()
            + (currentlyGenerating == null ? "" : ", currently generating: " + currentlyGenerating.get()));
        return false;                                                    // SOFT fail — log + refuse, no throw in prod
    }
    if (this.center.isUpgrading()) {                                     // below-zero retrogen only; false on fresh worlds
        LevelHeightAccessor h = this.center.getHeightAccessorForGeneration();
        return !h.isOutsideBuildHeight(pos.getY());
    }
    return true;
}
```

(String recipes recovered from BootstrapMethods `makeConcatWithConstants`; usable as a golden-run
log detector, see §7.3.)

### 3.2 `setBlock(BlockPos, BlockState, int flags, int recursionLeft)` (exact)

```java
public boolean setBlock(BlockPos pos, BlockState state, int flags, int recursionLeft) {
    if (!this.ensureCanWrite(pos)) return false;
    ChunkAccess chunk = this.getChunk(pos);                    // owner chunk — may be a NEIGHBOR ProtoChunk
    BlockState old = chunk.setBlockState(pos, state, flags);   // §4: ChunkAccess/ProtoChunk.setBlockState(BlockPos,BlockState,int)
    if (old != null) this.level.updatePOIOnBlockStateChange(pos, old, state);
    if (state.hasBlockEntity()) {
        if (chunk.getPersistedStatus().getChunkType() == ChunkType.LEVELCHUNK) {
            BlockEntity be = ((EntityBlock)state.getBlock()).newBlockEntity(pos, state);
            if (be != null) chunk.setBlockEntity(be); else chunk.removeBlockEntity(pos);
        } else {
            CompoundTag tag = new CompoundTag();               // ProtoChunk path (worldgen): DUMMY pending BE
            tag.putInt("x", pos.getX()); tag.putInt("y", pos.getY()); tag.putInt("z", pos.getZ());
            tag.putString("id", "DUMMY");
            chunk.setBlockEntityNbt(tag);                      // -> pendingBlockEntities map of OWNER chunk
        }
    } else if (old != null && old.hasBlockEntity()) {
        chunk.removeBlockEntity(pos);
    }
    if ((flags & 16) == 0) {                                   // Block.UPDATE_KNOWN_SHAPE suppresses
        BlockPos pp = state.getPostProcessPos(this, pos);      // BlockStateBase -> BlockBehaviour$PostProcess (26.2: returns pos-or-null, replaces 1.21 hasPostProcess boolean)
        if (pp != null) this.markPosForPostProcessing(pp);     // -> getChunk(pp).markPosForPostProcessing(pp): OWNER chunk's ShortList
    }
    return true;
}
```

**This is the entire mechanism by which decorating chunk A mutates chunk B**: `getChunk(pos)`
resolves the *owning* chunk of the block position; `chunk.setBlockState` writes into **B's own
`ProtoChunk` sections**, B's heightmaps (§4), B's pending-BE map, and B's post-processing lists.
`removeBlock`/`destroyBlock` delegate to `setBlock` with AIR. `addFreshEntity(entity)` routes
`getChunk(entityChunk).addEntity(entity)` — note it does **not** call `ensureCanWrite` (only
`EndSpikeFeature` among `levelgen/feature/*` references `addFreshEntity`; irrelevant to overworld).

---

## 4. `ProtoChunk.setBlockState` — what a write updates in the stored chunk

```java
public BlockState setBlockState(BlockPos pos, BlockState state, int flags) {
    int x = pos.getX(), y = pos.getY(), z = pos.getZ();
    if (this.isOutsideBuildHeight(y)) return Blocks.VOID_AIR.defaultBlockState();
    LevelChunkSection section = this.getSection(this.getSectionIndex(y));
    boolean wasOnlyAir = section.hasOnlyAir();
    if (wasOnlyAir && state.is(Blocks.AIR)) return state;                 // early-out, no heightmap touch
    int rx = SectionPos.sectionRelative(x), ry = ..., rz = ...;
    BlockState old = section.setBlockState(rx, ry, rz, state);

    if (this.status.isOrAfter(ChunkStatus.INITIALIZE_LIGHT)) {            // FALSE during features (status == CARVERS)
        /* lightEngine.updateSectionStatus / skyLightSources.update / lightEngine.checkBlock — skipped */
    }

    EnumSet<Heightmap.Types> after = this.getPersistedStatus().heightmapsAfter();
    EnumSet<Heightmap.Types> missing = null;
    for (Heightmap.Types t : after)
        if (this.heightmaps.get(t) == null) { if (missing == null) missing = EnumSet.noneOf(...); missing.add(t); }
    if (missing != null) Heightmap.primeHeightmaps(this, missing);        // lazy full prime of absent maps
    for (Heightmap.Types t : after)
        this.heightmaps.get(t).update(rx, y, rz, state);                  // incremental update of ALL maps in the set
    return old;
}
```

### 4.1 Which heightmaps (from `ChunkStatus.<clinit>`, 26.2)

```java
WORLDGEN_HEIGHTMAPS = EnumSet.of(OCEAN_FLOOR_WG, WORLD_SURFACE_WG);
FINAL_HEIGHTMAPS    = EnumSet.of(OCEAN_FLOOR, WORLD_SURFACE, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES);
// heightmapsAfter per status:
EMPTY..SURFACE                       -> WORLDGEN_HEIGHTMAPS
CARVERS, FEATURES, INITIALIZE_LIGHT,
LIGHT, SPAWN, FULL                   -> FINAL_HEIGHTMAPS
```

During the whole features stage every chunk being written (center *and* 3×3 neighbors) has
`persistedStatus == CARVERS` — `ChunkStep.apply` only bumps the status in
`completeChunkGeneration` (a `thenApply` *after* the task). Therefore:

- Every decoration write updates the owner chunk's **4 FINAL heightmaps** (lazily priming them on
  first touch if absent).
- The **`*_WG` maps are frozen during features** (last updated by the carve step, whose chunk
  status was SURFACE → WORLDGEN_HEIGHTMAPS). Negative finding: no code path updates `*_WG` from a
  features-stage write.

### 4.2 `Heightmap.primeHeightmaps` / `update` semantics (bytecode-verified)

- `primeHeightmaps(chunk, types)`: for each of the 256 columns, scans from
  `chunk.getHighestSectionPosition()+16-1` down to `minY`; skips pure-AIR states without testing;
  first state passing each map's `isOpaque` sets that map's column to `y+1` and removes it from the
  scan list. Columns with no hit are **left untouched** (fresh maps default to 0 ⇒ minY).
  It *recomputes* the requested types from current blocks (it does not skip already-primed maps —
  `getOrCreateHeightmapUnprimed` returns the existing map object, whose columns are overwritten as
  hits are found).
- `update(x, y, z, state)`: `first = getFirstAvailable(x,z)`; no-op if `y <= first - 2`;
  if opaque and `y >= first` → set `y+1`; if `y == first-1` and now non-opaque → scan down to next
  opaque (or minY). ⇒ per-column result after any write sequence equals a recompute from the final
  blocks; the heightmap is a pure function of the block content, not the write order.
- `ChunkStatusTasks.generateFeatures` starts by `Heightmap.primeHeightmaps(chunk,
  EnumSet.of(MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES, OCEAN_FLOOR, WORLD_SURFACE))` on the center
  chunk *before* the region is built — so any lazily-primed/incrementally-updated FINAL maps a chunk
  accumulated while its neighbors decorated first are **re-primed from blocks at that moment**;
  earlier incremental state cannot leak into the recompute.
- Read side: `ChunkAccess.getHeight(type, x, z)` lazily primes a missing single type on read
  (`primeHeightmaps(this, EnumSet.of(type))`), then returns `firstAvailable - 1`;
  `WorldGenRegion.getHeight(type, x, z)` = `warnIfReadOutsideWriteZone; getChunk(x>>4, z>>4)
  .getHeight(type, x & 15, z & 15) + 1`.

### 4.3 Reads: `getBlockState` / `getFluidState` / `getHeight`

```java
public BlockState getBlockState(BlockPos pos) {
    int cx = SectionPos.blockToSectionCoord(pos.getX()), cz = ...getZ();
    this.warnIfReadOutsideWriteZone(cx, cz);          // log-only; never blocks the read
    return this.getChunk(cx, cz).getBlockState(pos);  // EMPTY-status request -> succeeds at distance <= 8
}
```

`warnIfReadOutsideWriteZone(cx, cz)`: returns silently if `(cx,cz) == center`; otherwise if
`!isWithinWriteZone(cx,cz)` logs (`Util.logAndPauseIfInIde`):
`"Detected unsafe terrain read during worldgen: reading from chunk [%d, %d] while generating chunk [%d, %d] (distance: %d, write radius: %d), step: %s%s"`.
So **reads inside the 3×3 are silent and supported; reads at distance 2..8 warn but proceed** (§7.3).

---

## 5. RNG, seed, dimension, ticks

- `getSeed()` → the level seed (constructor-cached).
- `getRandom()` → positional random from `RandomState.getOrCreateRandomFactory(
  "minecraft:worldgen_region_random").at(centerChunk.getWorldPosition())` — pure function of
  (worldgen random state, center chunk pos); fresh instance per region; order-independent.
  (Callers during decoration are outside this slice; the instance itself carries no cross-chunk state.)
- `dimensionType()` → cached `level.dimensionType()`; `registryAccess()`, `enabledFeatures()`,
  `getSeaLevel()`, `getMinY()`, `getHeight()` delegate to `ServerLevel` (static per run).
- `getBiomeManager()` → region-local `BiomeManager(this, obfuscateSeed(seed))`;
  `LevelReader.getBiome(pos)` default → `biomeManager.getBiome(pos)` → quart lookups through
  `LevelReader.getNoiseBiome(qx,qy,qz)` default → `getChunk(QuartPos.toSection(qx),
  QuartPos.toSection(qz), ChunkStatus.BIOMES, false).getNoiseBiome(...)` → **stored per-chunk biome
  sections** (written once at the BIOMES stage). `getUncachedNoiseBiome` → `ServerLevel` →
  generator biome source (pure function of seed). No `setBiome`-like method exists on
  `WorldGenRegion`; nothing in the features stage mutates stored biomes (negative finding: the only
  biome-writing path in `ChunkStatusTasks` is `generateBiomes` → `ChunkGenerator.createBiomes`).
- **Scheduled ticks**: `getBlockTicks()`/`getFluidTicks()` return the `WorldGenTickAccess` wrappers;
  `schedule(ScheduledTick)` → `getChunk(tick.pos()).getBlockTicks()/getFluidTicks().schedule(tick)`
  — i.e. the **owner chunk's `ProtoChunkTicks`**, with **no `ensureCanWrite` gate** (only the
  distance-≤8 `getChunk` gate). Tick creation (`LevelAccessor.createTick` defaults) stamps
  `triggerTick = getGameTime() + delay` and `subTickCount = nextSubTickCount()` (region-local
  AtomicLong), **but** `ProtoChunkTicks.schedule` immediately converts to
  `SavedTick(type, pos, /*delay*/ 0, priority)` — gameTime, delay and subTickCount are all
  **dropped** for proto chunks. Dedup + order:

  ```java
  // ProtoChunkTicks:
  private final List<SavedTick<T>> ticks = Lists.newArrayList();                       // insertion order == persisted order
  private final Set<SavedTick<?>> ticksPerPosition = new ObjectOpenCustomHashSet<>(SavedTick.UNIQUE_TICK_HASH);
  public void schedule(ScheduledTick<T> t) { schedule(new SavedTick<>(t.type(), t.pos(), 0, t.priority())); }
  private void schedule(SavedTick<T> t) { if (ticksPerPosition.add(t)) ticks.add(t); }
  // SavedTick$1 (UNIQUE_TICK_HASH): equals/hash on (type, pos) ONLY — priority ignored; first scheduler wins.
  ```

  Features that reference `scheduleTick` (grep over `levelgen/feature/`): `SimpleBlockFeature`,
  `SpringFeature`, `LakeFeature`, `GeodeFeature`.
- `Blender.generateBorderTicks(region, chunk)` (called at the end of `generateFeatures`):
  returns immediately when `chunk.getBlendingData() == null` — **no-op on fresh worlds** (golden runs).
- Inert on purpose (bytecode: constant returns): `getNearestPlayer`→null, `getSkyDarken`→0,
  `players()/getEntities(...)`→empty, `playSound/addParticle/levelEvent/gameEvent`→return,
  `environmentAttributes()`→EMPTY, `isClientSide()`→false. `getLightEngine()` delegates to the
  server light engine but nothing in the write path enqueues light work during features (§4:
  status gate is false).

---

## 6. What a later-decorated neighbor observes from an earlier one

Channels a features-stage task can read, and whether neighbor decoration mutates them:

| channel | read path | mutated by neighbor decoration? |
|---|---|---|
| blocks / fluid states | `getBlockState`/`getFluidState` → owner `ProtoChunk` sections | **YES** — §3.2/§4 writes land in the stored sections; a later chunk sees every block an earlier neighbor placed in it |
| FINAL heightmaps (4) | `getHeight(type,x,z)` → owner chunk map `+1` | **YES** — updated on every write into that chunk (§4.1); also re-primed from blocks at that chunk's own features start |
| `*_WG` heightmaps | same | **NO** — not in `heightmapsAfter(CARVERS)`; frozen post-carve |
| scheduled block/fluid ticks | `getBlockTicks().hasScheduledTick` (features rarely read; they mostly write) | **YES** — owner chunk's `ProtoChunkTicks` list; presence, order, and (type,pos)-dedup winner depend on decoration order |
| post-processing lists | not readable via region API (write-only during gen) | **YES** (artifact) — owner chunk `ShortList[section]`, append order = decoration order |
| block entities (pending "DUMMY" NBT) | `getBlockEntity(pos)` (materializes from NBT on read) | **YES** — follows blocks |
| biomes | `getBiome` → stored biome sections | **NO** — written only at BIOMES stage; no write channel exists |
| structure starts/references | `StructureManager.forWorldGenRegion(region)` | not written by decoration (starts/references stages precede); outside this slice (see A-slice on `applyBiomeDecoration`) |
| light | `getLightEngine()` | **NO** during features — `ProtoChunk.setBlockState` light branch requires status ≥ INITIALIZE_LIGHT; no light state exists at 07 |
| entities | `addFreshEntity` → owner `ProtoChunk.addEntity` | YES in principle; only `EndSpikeFeature` uses it (End only) |
| RNG / seed / registries / sea level | region-cached or ServerLevel | **NO** — pure per (seed, registries, center pos) |

---

## 7. Implications for the order manifest (ADR-007)

### 7.1 Which 07_features dump artifacts are order-sensitive

Two runs identical except for per-chunk decoration order can differ, for a given chunk X's
07_features dump (taken when X's features task completes), in:

1. **Blocks (and fluid states)** — X contains writes from exactly {X itself} ∪ {neighbors within
   chessboard 1 decorated *before* X}. Writes from later neighbors land after the dump point and
   surface in 08+ dumps instead. Additionally X's *own* placement decisions read blocks/heightmaps
   up to the 3×3 boundary, so earlier-neighbor content changes X's own rolls' outcomes (not the RNG
   sequence itself — that seeding is per (seed, chunk), other slice — but the accept/reject and
   ground-height results).
2. **FINAL heightmaps** — pure function of X's blocks at dump time (§4.2), so exactly as
   order-sensitive as blocks, never more. `*_WG` maps: order-insensitive (frozen).
3. **Pending tick lists** — order-sensitive in *list order*, in *dedup winner* (first (type,pos)
   wins, priority of later duplicates discarded), and in *content* (which side of a boundary a
   spring/lake fluid tick falls on). Saved delay is always 0; subTickCount never persisted ⇒ the
   region-local AtomicLong is irrelevant to dumps.
4. **PostProcessing ShortLists** — order-sensitive append order per section, plus content.
5. **Pending block-entity NBT ("DUMMY" entries)** — follows blocks.
6. **Light** — nonexistent at 07; later light stages are pure functions of blocks, so identical iff
   blocks are identical. Biomes: order-insensitive.

⇒ The manifest must record only the **global sequence of features-step executions (dimension,
chunkX, chunkZ)**; replaying that sequence pins 1–5 *provided* within-chunk iteration is
deterministic (supported on the region side: every input the region exposes is either pure
(seed/registries/pos/RNG factory) or a deterministic function of prior writes; the region holds no
hidden mutable state besides `subTickCount`, which is dump-irrelevant).

### 7.2 Where to hook

`WorldGenRegion.<init>` is called for 7 step kinds, so it alone is too broad; the precise 1:1 hook
for the features stage is **entry of `ChunkStatusTasks.generateFeatures`** (or equivalently the
`WorldGenRegion` constructor filtered on `generatingStep.targetStatus() == FEATURES`). Everything
inside runs synchronously on one thread to completion (`applyBiomeDecoration` +
`generateBorderTicks`, then the future completes) — a (dim, cx, cz) record at entry is exactly the
decoration order. The status flip to FEATURES happens strictly after the task
(`ChunkStep.apply` → `thenApply(completeChunkGeneration)`), so hooking task entry vs. completion
yields the same order only if tasks never interleave; if the executor overlaps features tasks of
*non-adjacent* chunks, entry order and completion order can both be recorded — for replay in C the
pair (entry order per chunk + the guarantee that vanilla never runs two *adjacent* features tasks
concurrently, which the 3×3 claim system in `ChunkGenerationTask`/`GeneratingChunkMap` enforces —
claim mechanics are outside this slice) makes entry order sufficient for any pair of chunks that
actually share writable window.

### 7.3 Can anything OUTSIDE the 3×3 window affect X's 07 dump?

- **Write channels: NO.** `ensureCanWrite` hard-bounds `setBlock` (and thus post-processing marks
  and pending BEs) to the 3×3; out-of-zone writes return false without writing.
- **Biome reads: NO** beyond 3×3 — they crash (§2.2), so they cannot silently vary.
- **Tick scheduling & `addFreshEntity`: unbounded up to distance 8 in *code*, but every vanilla
  feature schedules at its own (bounded) write positions, and `addFreshEntity` is End-only.**
- **Block/height reads at distance 2..8: the one real hazard.** They are permitted (warn-only), and
  the returned `ProtoChunk` content is whatever generation state that chunk *currently* has (§2.2
  — the per-status future resolves to the live object), which depends on global scheduling, not on
  anything the manifest records. If any golden-run feature performed such a read, the golden dump
  itself would be scheduler-nondeterministic. Detector: both misuse paths log through
  `Util.logAndPauseIfInIde` with the exact strings
  `"Detected unsafe terrain read during worldgen: reading from chunk ..."` and
  `"Detected setBlock in a far chunk ..."` (BootstrapMethods-recovered). **Grep the golden run's
  server log for `"Detected unsafe terrain read"` / `"Detected setBlock in a far chunk"`; absence
  proves the golden features stage stayed inside the 3×3 and the order manifest is sufficient.**

### 7.4 Verdict on the working hypothesis

Confirmed *for the region/window layer*: the writable window is exactly 3×3; all order-sensitive
07 artifacts (blocks, FINAL heightmaps, ticks, post-processing, pending BEs) are functions of
(seed, registries, chunk pos, decoration order) only — no other free variable exists in
`WorldGenRegion` itself — **conditional on** (a) no out-of-window reads in the golden run (checkable
via §7.3 log strings), and (b) within-chunk iteration determinism, which lives in
`ChunkGenerator.applyBiomeDecoration` (other slice), not here.
