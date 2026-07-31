# R1 — SPAWN/FULL bytecode confirmation (Task 11)

Source of truth: `javap -p -c -constants -cp
tools/golden/libs/extracted/server-26.2.jar <fqcn>` (+ `-v` for
BootstrapMethods). Confirms the empirical 09→10→11 handoff diff
(task10-light/A §Task 11) mechanically.

## Task binding (26.2 moved it out of ChunkStatus)

`ChunkStatus.<clinit>` only registers statuses + heightmap sets. Binding
lives in `ChunkPyramid.<clinit>` via `ChunkStep$Builder.setTask`:

| status | GENERATION_PYRAMID | LOADING_PYRAMID |
|---|---|---|
| SPAWN | `lambda$static$10` @117-125: `addRequirement(BIOMES, 1)` @1-5, task = `ChunkStatusTasks.generateSpawn` (bsm #5) | `lambda$static$22` = identity → Builder default task = `ChunkStatusTasks::passThrough` (Builder `<init>` @10-15, bsm #0). On load SPAWN does literally nothing |
| FULL | `lambda$static$11` @128-136: task = `ChunkStatusTasks.full` (bsm #1), no requirements | `lambda$static$23`: same `ChunkStatusTasks.full` |

## 1. SPAWN = mob bookkeeping only (CONFIRMED)

`ChunkStatusTasks.generateSpawn` @0-32: `isUpgrading()` guard → `new
WorldGenRegion` → `ChunkGenerator.spawnOriginalMobs(region)` →
`completedFuture(chunk)`. Nothing else.

`NoiseBasedChunkGenerator.spawnOriginalMobs` @0-85:
`disableMobGeneration()` guard; RNG = fresh `WorldgenRandom(LegacyRandom
Source(uniqueSeed))` immediately reseeded by
`setDecorationSeed(seed, minBlockX, minBlockZ)` @59-76 (same seeding as
features deco); → `NaturalSpawner.spawnMobsForChunkGeneration`.

`spawnMobsForChunkGeneration` @0-~660: CREATURE category only,
`SPAWN_MOBS` gamerule gate @32-53 (harness sets false → whole body
skipped in the golden recording), spawn rolls, `getTopNonCollidingPos`
(reads `getHeight`/`getBlockState` only), `EntityType.create`,
`addFreshEntityWithPassengers`. **No setBlockState / setHeightmap /
biome-container / light-engine call anywhere in the path — block and
heightmap access is read-only.** Dump-observable state: pure
pass-through. C side: `hc_gen_spawn_stage` = status marker only.

## 2. FULL = ProtoChunk→LevelChunk conversion (heightmap pruning only)

`ChunkStatusTasks.full` @0-41 → `supplyAsync(lambda$full$0,
mainThreadExecutor)` (conversion on Server thread — matches the
order.snapshots `Server_thread` column for 11_full). `lambda$full$0`:
`new LevelChunk(level, protoChunk, postLoad)` @34-52,
`replaceProtoChunk(new ImposterProtoChunk(levelChunk, false))` @55-66,
then setFullStatus/runPostLoad/setLoaded/registerAllBlockEntities/
registerTickContainer/setUnsavedListener.

Heightmaps in `LevelChunk.<init>(ServerLevel, ProtoChunk, ...)` @181-263:

- **Surviving set**: iterates `ProtoChunk.getHeightmaps()` @181-185,
  keeps entries where `ChunkStatus.FULL.heightmapsAfter().contains(key)`
  @214-230. `FINAL_HEIGHTMAPS = EnumSet.of(OCEAN_FLOOR, WORLD_SURFACE,
  MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES)` (`ChunkStatus.<clinit>`
  @12-30); FULL registered with it @215-229.
- **Values copied, NOT recomputed**: `setHeightmap(key, getRawData())`
  @233-257 → `Heightmap.setRawData` @0-29 = `System.arraycopy` of the
  packed longs — bit-exact carry. `Heightmap.primeHeightmaps` is NOT in
  the FULL path at all. **No "prime missing kinds" branch** — the loop
  enumerates the proto's map, so an absent FINAL kind simply stays
  absent (lazy prime on much-later first read via `ChunkAccess.getHeight`
  @15-70). The 4 FINAL kinds are always present because
  `ChunkStatusTasks.generateFeatures` @6-22 primes exactly those 4 at
  FEATURES entry (our `hc_gen_features_chunk` does the same).
- ***_WG dropped silently**: non-FINAL entries skipped @230→192, no side
  effect. Explains 10→11 pure-deletion diff (c.0.0 6→4, reloaded 8
  chunks 5→4).

Blocks/biomes: section objects carried by reference (ctor @22-23, base
`ChunkAccess.<init>` @110-142 arraycopy of the section array only) —
no mutation. Ticks repackaged (`unpackBlockTicks`/`unpackFluidTicks`
@10-15), block entities re-registered, structure starts/references
transferred, `skyLightSources` + `setLightCorrect(proto.isLightCorrect())`
carried @263-276. Nothing touches dump-observable state.

C side: `hc_gen_full_stage` = status marker; `heightmap_final[4]` is
already the surviving representation (copy is free), `promoted == 11`
is the "*_WG absent" contract.

## 3. Post-FULL write semantics (why wg_dropped covers c.0.0 too)

After conversion the holder exposes `ImposterProtoChunk(levelChunk,
false)` — `allowWrites=false`, so later neighbor-deco *writes* into a
FULL chunk are dropped by vanilla, and *_WG reads reprime lazily from
current blocks on the LevelChunk. Our region model reprimes *_WG after
`rg->wg_dropped` (set at manifest seq 9) — for c.0.0, which hits FULL at
seq 9 in both bundles (order.snapshots), the reload wave and the FULL
conversion converge to the same reprime-on-first-read semantics, which
is why the shared `wg_dropped` flag is sufficient. Write-drop after FULL
is NOT modeled; gate evidence (36/36 dumps ≤ inherited caps, no new
divergence) shows no post-FULL write into grid chunks occurs in the
observation window.

Scratch disassemblies were under /tmp (ChunkStatus/ChunkStatusTasks/
ChunkPyramid/ChunkStepBuilder/LevelChunk/ChunkAccess/Heightmap/
NoiseBasedChunkGenerator/NaturalSpawner .javap) — regenerate with the
javap line above if needed.
