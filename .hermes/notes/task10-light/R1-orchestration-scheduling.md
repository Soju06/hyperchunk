# Task 10 / Lane B1 — Light-stage orchestration, scheduling, dump-time visibility (26.2 bytecode recon)

Source of truth: `javap -p -c -constants` against
`tools/golden/libs/extracted/server-26.2.jar` (real unobfuscated 26.2 server).
All claims cite method + bytecode landmarks. Harness facts cite
`tools/golden/stage-dump-mod/.../StageDumper.java` and `mixin/ChunkStepMixin.java`.

---

## 1. Pyramid entries and stage task functions

### 1.1 ChunkPyramid (GENERATION_PYRAMID / LOADING_PYRAMID)

`ChunkPyramid.static{}` builds both pyramids with `Builder.step(status, λ)`;
InvokeDynamic bootstrap indices #13..#24 (generation) and #25..#36 (loading)
map 1:1 to `lambda$static$0..23` (verified in the BootstrapMethods table:
bsm #21→`lambda$static$8`, #22→`lambda$static$9`, #33→`lambda$static$20`,
#34→`lambda$static$21`).

Relevant step lambdas (both pyramids identical for the two light stages):

| step | lambda (gen / load) | body |
|---|---|---|
| FEATURES | `lambda$static$7` / — | `addRequirement(STRUCTURE_STARTS, 8)` (`bipush 8` at #4), `addRequirement(CARVERS, 1)` (`iconst_1` at #12), `blockStateWriteRadius(1)` (`iconst_1` at #16), task = `ChunkStatusTasks.generateFeatures` (bsm #6) |
| INITIALIZE_LIGHT | `lambda$static$8` / `lambda$static$20` | **no `addRequirement` at all, no blockStateWriteRadius** — only `setTask(bsm #3 = ChunkStatusTasks.initializeLight)` |
| LIGHT | `lambda$static$9` / `lambda$static$21` | `addRequirement(INITIALIZE_LIGHT, 1)` (`getstatic INITIALIZE_LIGHT; iconst_1` at #1..#5), `setTask(bsm #2 = ChunkStatusTasks.light)` |
| SPAWN | `lambda$static$10` | `addRequirement(BIOMES, 1)`, task = `generateSpawn` (bsm #5) |

So: **INITIALIZE_LIGHT dependency radius = 0** (only the implicit
same-chunk parent FEATURES), **LIGHT dependency radius = 1 on
INITIALIZE_LIGHT** (⇒ all 8 neighbors must have finished 08, hence all 8
neighbors have finished FEATURES, before a chunk's 09 task can start).
Neither light step has a `blockStateWriteRadius`.

`ChunkStatus` itself carries no radii in 26.2 — it is just
`{index, parent, chunkType, heightmapsAfter}` (`ChunkStatus.<init>` putfields
#50/#53/#57/#65); order EMPTY..FULL gives INITIALIZE_LIGHT index 8, LIGHT
index 9 (matches dump prefixes `08_`, `09_`).

### 1.2 ChunkStatusTasks.initializeLight (static)

```
initializeLight(ctx, step, cache, chunk):
  tlle = ctx.lightEngine()                     // invokevirtual WorldGenContext.lightEngine at #1
  chunk.initializeLightSources()               // invokevirtual ChunkAccess.initializeLightSources at #7
  ((ProtoChunk)chunk).setLightEngine(tlle)     // checkcast ProtoChunk at #11, invokevirtual #211 at #16
  isLighted = isLighted(chunk)                 // invokestatic at #20
  return tlle.initializeLight(chunk, isLighted)// invokevirtual TLLE.initializeLight at #30
```

`isLighted(ChunkAccess)` = `chunk.getPersistedStatus().isOrAfter(LIGHT) &&
chunk.isLightCorrect()` (bytecode #1..#25). **For fresh worldgen this is
always false** (persisted status < LIGHT).

### 1.3 ChunkStatusTasks.light (static)

```
light(ctx, step, cache, chunk):
  isLighted = isLighted(chunk)                 // invokestatic #215 at #1
  return ctx.lightEngine().lightChunk(chunk, isLighted)  // invokevirtual TLLE.lightChunk at #13
```

Both tasks return the light engine's future directly — **the stage does its
tiny synchronous part on the worldgen executor and *completes* via the light
engine** (see §2/§4). Contrast: pure-sync stages return
`CompletableFuture.completedFuture(chunk)`.

### 1.4 Where the persisted status is bumped

`ChunkStep.apply`: if `chunk.getPersistedStatus().isBefore(targetStatus)`,
runs `task.doWork(...)` then `.thenApply(lambda$apply$0)` (invokedynamic at
#58) → `completeChunkGeneration(chunk, profiledDuration)` which does
`((ProtoChunk)chunk).setPersistedStatus(targetStatus)` (invokevirtual #95).
Non-async `thenApply` ⇒ for 08/09 this runs **on the thread that completes
the light-engine future** (the "light" lane, §4).

`GenerationChunkHolder.applyStep` then chains `.handle(lambda$applyStep$0)`
→ `completeFuture(targetStatus, chunk)` which completes the per-status
`AtomicReferenceArray futures[index]` entry — this is what neighbor
`ChunkGenerationTask`s wait on.

---

## 2. ThreadedLevelLightEngine (`net.minecraft.server.level.ThreadedLevelLightEngine`)

Extends `LevelLightEngine`. Fields (constructor putfields):
`consecutiveExecutor` (`ConsecutiveExecutor`, the **"light"** lane),
`lightTasks` (`ObjectArrayList<Pair<TaskType,Runnable>>`),
`chunkMap`, `taskDispatcher` (`ChunkTaskDispatcher`, the light dispatcher),
`scheduled` (`AtomicBoolean`). Constants: `DEFAULT_BATCH_SIZE = 1000`,
`taskPerBatch = 1000` (sipush 1000 at ctor #19). Super ctor called with
`(lightChunkGetter, /*block*/ true (iconst_1), /*sky*/ hasSkyLight (iload_3))`.

`TaskType` enum = `{PRE_UPDATE, POST_UPDATE}` (only two constants).

### 2.1 addTask plumbing

- `addTask(x, z, TaskType, Runnable)` → `addTask(x, z,
  chunkMap.getChunkQueueLevel(ChunkPos.pack(x,z)), type, run)` (invokevirtual
  ChunkMap.getChunkQueueLevel at #12).
- `addTask(x, z, IntSupplier queueLevel, TaskType, Runnable)` →
  `taskDispatcher.submit(λ, pack(x,z), queueLevel)` where λ =
  `lambda$addTask$0` = `{ lightTasks.add(Pair.of(type, run)); if
  (lightTasks.size() >= 1000) runUpdate(); }` (if_icmplt 34 against
  sipush 1000).

So *every* public API call turns into a runnable that, when the dispatcher
finally runs it **on the "light" ConsecutiveExecutor lane**, appends a
(type, runnable) pair to `lightTasks`. `lightTasks` and `runUpdate()` are
only ever touched from the light lane ⇒ no locks needed.

`ChunkMap.getChunkQueueLevel(long)` → IntSupplier =
`min(holder.getQueueLevel(), PRIORITY_LEVEL_COUNT-1)`, or
`PRIORITY_LEVEL_COUNT-1` if no visible holder
(`lambda$getChunkQueueLevel$0`). `PRIORITY_LEVEL_COUNT = ChunkLevel.MAX_LEVEL
+ 2` (ChunkTaskPriorityQueue static{}).

### 2.2 Public methods used by the pipeline (all enqueue PRE_UPDATE unless noted)

| method | queue level | PRE_UPDATE runnable (super = LevelLightEngine) |
|---|---|---|
| `checkBlock(BlockPos)` | `getChunkQueueLevel(pos)` | `super.checkBlock(pos.immutable())` (`lambda$checkBlock$0`) |
| `updateChunkStatus(ChunkPos)` (unload only) | `()->0` | `retainData(pos,false); setLightEnabled(pos,false); for each light section: queueSectionData(BLOCK/SKY, null); for each level section: updateSectionStatus(pos, true)` (`lambda$updateChunkStatus$1`) |
| `updateSectionStatus(SectionPos, bool)` | `()->0` (lambda$updateSectionStatus$0 `iconst_0`) | `super.updateSectionStatus(pos, isEmpty)` |
| `propagateLightSources(ChunkPos)` | `getChunkQueueLevel` | `super.propagateLightSources(pos)` |
| `setLightEnabled(ChunkPos, bool)` | `getChunkQueueLevel` | `super.setLightEnabled(pos, b)` |
| `queueSectionData(layer, SectionPos, DataLayer)` | `()->0` | `super.queueSectionData(...)` |
| `retainData(ChunkPos, bool)` | `()->0` (lambda$retainData$0) | `super.retainData(pos, b)` |
| `runLightUpdates()` | — | **throws UnsupportedOperationException** ("Ran automatically on a different thread!") |
| `waitForPendingTasks(x,z)` | — | `CompletableFuture.runAsync(()->{}, run -> addTask(x, z, POST_UPDATE, run))` |

### 2.3 initializeLight(ChunkAccess, boolean isLighted) — bytecode #0..#56

1. `addTask(x, z, PRE_UPDATE, lambda$initializeLight$0)`:
   for `i in 0..getSectionsCount()`: if `!sections[i].hasOnlyAir()` →
   `super.updateSectionStatus(SectionPos.of(pos, sectionYFromIndex(i)),
   /*empty=*/false)` (iconst_0 at #51). **Only non-empty sections are
   registered; nothing is propagated, no light enabled.**
2. returns `CompletableFuture.supplyAsync(lambda$initializeLight$2,
   executor = lambda$initializeLight$3)` where the *executor* wraps the
   supplier as a **POST_UPDATE** task (`addTask(x, z, POST_UPDATE, run)`),
   and the supplier does `super.setLightEnabled(pos, isLighted /*=false in
   worldgen*/); super.retainData(pos, false); return chunk`.

### 2.4 lightChunk(ChunkAccess, boolean isLighted) — bytecode #0..#59

1. synchronously (on the calling worldgen thread): `chunk.setLightCorrect(false)`
   (invokevirtual #185 at #7).
2. `addTask(x, z, PRE_UPDATE, lambda$lightChunk$0)`: `if (!isLighted)
   super.propagateLightSources(pos)` — for worldgen always taken.
   (`LevelLightEngine.propagateLightSources` fans out block then sky;
   `BlockLightEngine.propagateLightSources` starts with
   `setLightEnabled(pos, true)` at #0..#3 then
   `chunk.findBlockLightSources(...)`; `SkyLightEngine.propagateLightSources`
   starts with `storage.setLightEnabled(zeroNode, true)` at #12..#21.
   **Light becomes "enabled" only at 09, not at 08.**)
3. returns `CompletableFuture.supplyAsync(lambda$lightChunk$2 =
   { chunk.setLightCorrect(true); return chunk; }, executor =
   lambda$lightChunk$3 = run -> addTask(x, z, POST_UPDATE, run))`.

### 2.5 Batch processing: tryScheduleUpdate / runUpdate

`tryScheduleUpdate()` (called from (a)
`ServerChunkCache$MainThreadExecutor.pollTask()` — every main-thread poll:
`if (runDistanceManagerUpdates()) return true; lightEngine.tryScheduleUpdate();
return super.pollTask();` — and (b) the ChunkMap unload path after
`updateChunkStatus`):

```
if ((!lightTasks.isEmpty() || super.hasLightWork()) && scheduled.CAS(false,true))
    consecutiveExecutor.schedule(() -> { runUpdate(); scheduled.set(false); })
```

`runUpdate()` (private; runs only on the light lane — either via the schedule
above or inline from `lambda$addTask$0` at the 1000 threshold):

```
batch = min(lightTasks.size(), 1000)          // sipush 1000, Math.min at #12
it = lightTasks.iterator()
phase 1: for j < batch: p = it.next(); if p.first == PRE_UPDATE: p.second.run()
it.back(batch)                                 // rewind
super.runLightUpdates()                        // LevelLightEngine.runLightUpdates: block engine first, then sky (invokevirtual #50 twice)
phase 2: for j < batch: p = it.next(); if p.first == POST_UPDATE: p.second.run(); it.remove()  // removes ALL batch entries
```

**Future-completion timing (exact):** the POST_UPDATE runnable *is* the
`CompletableFuture.AsyncSupply` for the stage future. It runs in phase 2,
i.e. strictly after *all* PRE_UPDATE runnables of the same batch have run and
after **one full `runLightUpdates()`** has drained the engine
(`LightEngine.runLightUpdates` bytecode order: drain `blockNodesToCheck` via
`checkNode` → `propagateDecreases()` → `propagateIncreases()` →
`clearChunkCache()` → `storage.markNewInconsistencies(this)` →
`storage.swapSectionMap()` — landmarks #51/#59/#64/#69/#77). The supplier
completes the future on the light-lane thread; all non-async dependents
(`completeChunkGeneration` → harness dump → `GenerationChunkHolder.completeFuture`)
run synchronously right there, inside phase 2, before the next batch entry.

Entries beyond the first 1000 stay queued for the next `runUpdate`. Entries
appended while `runUpdate` is running cannot join the current batch: appends
themselves are dispatcher tasks queued behind the running lane task.

---

## 3. Executor infrastructure (who actually runs what)

### 3.1 ChunkMap wiring (ChunkMap.<init> bytecode #316..#405)

```
worldgenConsecutive = new ConsecutiveExecutor(executor, "worldgen")   // ldc "worldgen" at #322
lightConsecutive    = new ConsecutiveExecutor(executor, "light")      // ldc "light"   at #341
worldgenTaskDispatcher = new ChunkTaskDispatcher(worldgenConsecutive, executor)
lightTaskDispatcher    = new ChunkTaskDispatcher(lightConsecutive, executor)
lightEngine = new ThreadedLevelLightEngine(lightChunkGetter, this,
                 level.dimensionType().hasSkyLight(), lightConsecutive, lightTaskDispatcher)
```

`executor` here is the Executor handed down from
`ServerChunkCache` ← `ServerLevel` ← `MinecraftServer` — vanilla's shared
background worker pool (`Util.backgroundExecutor()`); its thread count is
what differs between the golden bundles (1 vs 4 workers).

### 3.2 ConsecutiveExecutor / AbstractConsecutiveExecutor

- `ConsecutiveExecutor` = `AbstractConsecutiveExecutor<Runnable>` over a
  `StrictQueue$QueueStrictQueue(ConcurrentLinkedQueue)`.
- `schedule(T)` = `queue.push(t); registerForExecution()`.
- `registerForExecution()`: `if (canBeScheduled() && setRunning())
  executor.execute(this)` — status `AtomicReference<Status>`
  SLEEPING/RUNNING/CLOSED gate ⇒ **at most one pool thread runs a lane at a
  time** (consecutive semantics), but the lane migrates freely between pool
  threads.
- `run()`: `pollTask()` (pops and runs **one** task), `setSleeping()`,
  `registerForExecution()` — one task per execute-slice, then requeue behind
  other Runnables on the shared pool.

`PriorityConsecutiveExecutor` (used inside each ChunkTaskDispatcher, name
"dispatcher", 4 priorities — `DISPATCHER_PRIORITY_COUNT = 4`) is the same
with a `FixedPriorityQueue`.

### 3.3 ChunkTaskDispatcher

Fields: `queue` (`ChunkTaskPriorityQueue`), `executor` (the TaskScheduler =
the "worldgen"/"light" ConsecutiveExecutor), `dispatcher`
(PriorityConsecutiveExecutor), `sleeping` (starts true).

- `onLevelChange` → dispatcher prio 0: `queue.resortChunkTasks(...)`.
- `release` → prio 1: `queue.release(chunkPos, clearQueue)` + wake.
- `submit(run, pos, queueLevelSupplier)` → prio 2 (`iconst_2` at #8):
  `lambda$submit$0` = `{ lvl = supplier.getAsInt(); queue.submit(run, pos,
  lvl); if (sleeping) { sleeping=false; pollTask(); } }`.
- `pollTask()` → prio 3: `lambda$pollTask$0` = `{ tasks = queue.pop(); if
  (tasks == null) sleeping = true; else scheduleForExecution(tasks); }`.
- `scheduleForExecution(TasksForChunk)`: maps each runnable to
  `executor.scheduleWithResult(cf -> { run.run(); cf.complete(Unit); })`
  (lambda$scheduleForExecution$0/1), then
  `CompletableFuture.allOf(futures).thenAccept(v -> pollTask())`
  (lambda$scheduleForExecution$3). **The dispatcher feeds the lane one
  chunk's task-list at a time and only pops the next chunk after the previous
  chunk's runnables all finished on the lane.**

`ChunkTaskPriorityQueue`: `List<Long2ObjectLinkedOpenHashMap<List<Runnable>>>
queuesPerPriority` (one linked map per level, insertion-ordered per chunk);
`pop()` takes `firstLongKey()/removeFirst()` from the lowest non-empty level
(`topPriorityQueueIndex` scan) → FIFO per queue level, all pending runnables
for that chunk at once.

Net effect for the light engine: each `ThreadedLevelLightEngine.xxx()` call
becomes queue.submit → (dispatcher lane) → light lane runs
`lightTasks.add(...)`. Per-chunk ordering of light API calls is preserved
(same submission lane; per-chunk FIFO in the priority queue at a given level).
`runUpdate` interleaves as a separately scheduled light-lane task.

### 3.4 Worldgen side

`ChunkMap.runGenerationTask(task)` →
`worldgenTaskDispatcher.submit(() -> task.runUntilWait().thenAccept(reschedule),
centerPos, holder::getQueueLevel)` (bytecode #6..#34, lambda$runGenerationTask$0/1)
⇒ `ChunkStep.apply`/`doWork` — including
`ChunkStatusTasks.initializeLight/light`'s synchronous bodies — run on the
**"worldgen" lane**; the 08/09 futures they return complete later on the
**"light" lane** (§2.5).

---

## 4. Read path & visibility (what a dump can observe)

### 4.1 getLayerListener / getLightValue

`LevelLightEngine.getLayerListener(layer)` returns the layer's `LightEngine`
object itself (blockEngine/skyEngine fields), or
`LayerLightEventListener$DummyLightLayerEventListener.INSTANCE` if that
engine is null (bytecode #4..#38). Overworld: block always created
(TLLE passes iconst_1), sky created because `hasSkyLight()`.

`LightEngine.getLightValue(BlockPos)` = `storage.getLightValue(pos.asLong())`
(invokevirtual #279).

- `BlockLightSectionStorage.getLightValue(long)`:
  `getDataLayer(blockToSection(pos), /*cached=*/false)` → **`false` branch
  selects `visibleSectionData`** (LayerLightSectionStorage.getDataLayer(J Z):
  `iload_3; ifeq 12; …updatingSectionData… / …visibleSectionData…`), null ⇒ 0,
  else `layer.get(x&15, y&15, z&15)`.
- `SkyLightSectionStorage.getLightValue(long)` → `getLightValue(pos, false)`:
  reads the **visible** `SkyDataLayerStorageMap` (checkcast at #27..#31);
  `top = visible.topSections.get(zeroNode(section))` (default
  `currentLowestY`); if `sectionY >= top` ⇒ **return 15** (bipush 15 at #83;
  the `cached==true` lightOnInSection check at #68..#82 is skipped for
  `false`); else walk up from the section until a non-null DataLayer is found
  (returning 15 if the walk reaches `top`), then `layer.get(...)`.

### 4.2 Snapshot semantics — updating vs visible

`LayerLightSectionStorage` fields: `protected volatile M visibleSectionData`,
`protected final M updatingSectionData`, `volatile boolean hasInconsistencies`,
plus `changedSections`, `sectionsAffectedByLightUpdates`, `queuedSections`
(a `Long2ObjectMaps.synchronize`d map — the only cross-thread-written
container), `toRemove`.

- All engine mutations go to `updatingSectionData`, with **copy-on-first-write
  per publication cycle**: `setStoredLevel` / `getDataLayerToWrite` copy the
  DataLayer (`DataLayer.copy()` / `copyDataLayer`) the first time a section
  enters `changedSections` (bytecode: `changedSections.add(j) ifeq …` then
  `copyDataLayer` at setStoredLevel #20..#26). Hence DataLayer byte arrays
  reachable from a published visible map are **never mutated afterwards**.
- `swapSectionMap()` (called at the END of every `LightEngine.runLightUpdates`):
  if `changedSections` non-empty → `visibleSectionData =
  updatingSectionData.copy(); visible.disableCache(); changedSections.clear()`
  (bytecode #12..#33; volatile store), then notifies
  `chunkSource.onLightUpdate` for `sectionsAffectedByLightUpdates`.
- `markNewInconsistencies(engine)` (immediately before swap): consumes
  `toRemove` (drops layers, `onNodeRemoved`) and merges `queuedSections`
  into the updating map for stored sections; guarded by the volatile
  `hasInconsistencies` flag.
- `updateSectionStatus(long, empty)` maintains `sectionStates`
  (`SectionState`: byte, `HAS_DATA_BIT=32`, low 5 bits neighborCount 0..26)
  for the section **and all 26 neighbors** (`SectionPos.offset(j,±1,±1,±1)`
  loop); a section whose state byte becomes non-zero for the first time gets
  `initializeSection` → fresh DataLayer into **updating** map +
  `changedSections.add` + `onNodeAdded` + `hasInconsistencies=true`.
  Sky override `onNodeAdded`: `topSections.put(zeroNode, sectionY+1)` if
  greater (bytecode #94..#99).

**Consequence for dumps:** a `getLightValue` read from ANY thread sees the
last snapshot published by the most recent completed `runLightUpdates()` —
an internally consistent, immutable state. A reader can never see a
half-propagated queue state; it can only be *stale* (missing batches that
have not yet swapped) — and since the harness dump runs on the light lane
itself (§5), it is not even stale: it reads exactly the post-batch swap.
The only mid-flight structure a foreign thread could observe is
`queuedSections` (synchronized), which the read path consults only via
`getDataLayerData`, not via `getLightValue`.

---

## 5. Chunk-side hooks

### 5.1 ChunkAccess

- `initializeLightSources()` = `skyLightSources.fillFrom(this)`
  (invokevirtual ChunkSkyLightSources.fillFrom at #5).
  `ChunkSkyLightSources` = per-chunk 16×16 `BitStorage` heightmap of the
  lowest sky-reachable Y (details = lane B2/B3); `fillFrom` scans down from
  `getHighestFilledSectionIndex()`.
- `findBlockLightSources(BiConsumer)` = `findBlocks(state ->
  state.getLightEmission() != 0, consumer)` (lambda$findBlockLightSources$0).
  Consumed only by `BlockLightEngine.propagateLightSources` (09).

### 5.2 ProtoChunk

- `setLightEngine(LevelLightEngine)` — plain putfield #138; the field is
  null during all pre-08 stages and is assigned **inside the 08 task body on
  the worldgen lane**, before any status ≥ INITIALIZE_LIGHT can be observed.
- `setBlockState(pos, state, flags)` light hook — guard is the ProtoChunk's
  **own status field**: `getstatic ChunkStatus.INITIALIZE_LIGHT;
  invokevirtual isOrAfter` at #109..#119 (`this.status`, set only by
  `setPersistedStatus`, which `ChunkStep.completeChunkGeneration` calls when
  the 08 future completes). If `status.isOrAfter(INITIALIZE_LIGHT)`:
  1. if the section's `hasOnlyAir()` flipped → `lightEngine.updateSectionStatus(pos, nowEmpty)` (#122..#143);
  2. if `LightEngine.hasDifferentLightProperties(old, new)` (#146..#152) →
     `skyLightSources.update(this, relX, y, relZ)` (#155..#166) **and**
     `lightEngine.checkBlock(pos)` (#170..#175).
  Below INITIALIZE_LIGHT: no light interaction at all (only heightmaps).

### 5.3 WorldGenRegion.setBlock (features path)

`ensureCanWrite` → `getChunk(pos).setBlockState(pos, state, flags)`
(invokevirtual ChunkAccess.setBlockState at #22) — **no direct light-engine
call in WorldGenRegion**; the only light effect is the ProtoChunk hook above.
So: a feature (running for neighbor chunk D, blockStateWriteRadius 1) that
writes into chunk C **does** enqueue `checkBlock`/`updateSectionStatus`
PRE_UPDATE tasks iff C's status is already ≥ INITIALIZE_LIGHT. This is
possible: C.08 needs only C.FEATURES; D.FEATURES needs only C ≥ CARVERS.
Not possible after C.09 is *scheduled*… wait — it IS still possible after
C.09 completes? No: D.FEATURES(radius-1 write into C) has no upper bound on
C's status, but C.09 requires all radius-1 neighbors ≥ 08, not ≥ FEATURES —
neighbors D at radius 1 must have finished FEATURES (since 08 requires
FEATURES for the same chunk). Therefore **all radius-1 feature writes into C
happen before C.09 starts**; radius-2..8 dependencies never write blocks.
Post-09 writes into C can only come from stages ≥ 09 of neighbors — none of
which write block states.

### 5.4 LevelChunk.setBlockState (post-FULL, for completeness)

Same pattern, unconditional (LevelChunk implies status FULL):
`hasOnlyAir` flip → `level.getChunkSource().getLightEngine().updateSectionStatus`
(invokevirtual #447 at #223), then `hasDifferentLightProperties` →
`skyLightSources.update + checkBlock`. Not reachable during the dumped
worldgen window.

---

## 6. Exact event sequence for chunk C, FEATURES → 08 → 09 → dump

Notation: **W** = "worldgen" lane, **L** = "light" lane, **D-lt** = light
dispatcher lane, **M** = server main thread. All lanes multiplex over the
same worker pool (1 thread in the primary bundle, 4 in the alt bundle).

1. **FEATURES (W):** `generateFeatures` runs; `WorldGenRegion.setBlock`
   into C or its radius-1 neighbors hits only ProtoChunk hooks; for chunks
   still < INITIALIZE_LIGHT nothing light-related happens. If a *neighbor* is
   already ≥ 08, checkBlock/updateSectionStatus tasks for that neighbor go
   to the light dispatcher.
2. **08 task body (W):** `initializeLightSources()` (fills C's
   `ChunkSkyLightSources`), `setLightEngine(tlle)`, then
   `tlle.initializeLight(C, false)`:
   - submit(PRE: register all non-empty sections of C) → D-lt → L:
     `lightTasks += (PRE, updateSectionStatus×k)`
   - submit(POST: AsyncSupply of the 08 future) → D-lt → L:
     `lightTasks += (POST, supply)`
   The worldgen task for C is now "waiting" (its `ChunkGenerationTask`
   re-schedules when the returned future completes).
3. **L, next runUpdate batch N** (triggered by M's `tryScheduleUpdate` poll
   or the ≥1000 inline path):
   - phase 1 runs C's `updateSectionStatus(sec, false)` calls (plus whatever
     other chunks' PRE tasks are in the batch): sectionStates/neighborCount
     bookkeeping, fresh all-zero DataLayers into the updating maps,
     sky `topSections[col] = maxRegisteredSectionY+1`;
   - `runLightUpdates()`: nothing to propagate for C (no checkBlock, light
     disabled, queues empty for C) → `swapSectionMap()` publishes the new
     sections;
   - phase 2 reaches C's POST entry: `setLightEnabled(C, false)` (no-op set),
     `retainData(C, false)`, future completes → synchronously on L:
     `completeChunkGeneration` (**status := INITIALIZE_LIGHT**) → **harness
     mixin dump 08** (`ChunkStepMixin` wraps `ChunkStep.apply`'s future with
     a non-async `thenApply(dump)`, so the dump executes here, on L, inside
     phase 2, BEFORE `GenerationChunkHolder.completeFuture` publishes 08 to
     dependents) → `completeFuture(INITIALIZE_LIGHT, C)`.
4. **09 scheduling:** only after step 3's `completeFuture` can any
   `ChunkGenerationTask` targeting C.LIGHT (or using C as a radius-1
   dependency) proceed. `ChunkStatusTasks.light` (W): `setLightCorrect(false)`,
   `tlle.lightChunk(C, false)` → submit(PRE: `propagateLightSources(C)`),
   submit(POST: supply).
5. **L, runUpdate batch M (M ≥ N+1):** phase 1 runs
   `propagateLightSources(C)` — `setLightEnabled(true)` for both layers +
   enqueue block sources (from `findBlockLightSources`) and sky sources —
   plus any other chunks' PRE tasks that landed in the same batch (including
   deferred `checkBlock`s from feature writes into ≥08 chunks);
   `runLightUpdates()` drains **everything queued by the whole batch** and
   swaps; phase 2 completes C's 09 future → status := LIGHT → **dump 09**
   (again on L, inside phase 2) → `completeFuture(LIGHT, C)`.

### 6.1 Can a dump observe a partially-propagated state?

- **Within one batch: no.** Reads target `visibleSectionData`, which is
  republished only by `swapSectionMap()` at the end of a full
  `runLightUpdates()` drain, and published maps are immutable (copy-on-write
  DataLayers, volatile store). The dump additionally runs on L itself, so no
  later batch can interleave before the read.
- **Across batches: bounded.** The dump for stage S of chunk C sees exactly
  the propagation of all PRE tasks in batches ≤ its own batch. What is NOT
  guaranteed to be included: PRE tasks still queued in `lightTasks` beyond
  the 1000-entry batch cap, and tasks still in the dispatcher queue. So a
  09 dump is "quiescent for its batch", not "globally quiescent".
- **Single shared worker thread (primary bundle):** strictly deterministic —
  every lane slice is a FIFO of `executor.execute` calls on one thread; the
  dump content is a pure function of the (deterministic) submission order.
  While L runs the dump, W cannot run ⇒ `seqBegin == seqEnd` always in
  order.snapshots.
- **4 worker threads (alt bundle):** the dump still reads a consistent
  post-swap snapshot (same-batch guarantee holds — L is one lane), but
  (a) *which* other chunks' PRE tasks (their 08 registrations, their 09
  propagation, feature-write checkBlocks) share or precede C's batch is
  timing-dependent ⇒ 09 values vary with bundle order; (b) W keeps applying
  features on other chunks concurrently with the dump ⇒ torn
  `seqBegin != seqEnd` snapshots (anchor 4).

### 6.2 Reconciliation with the empirical anchors

1. **08 `light_block` all zeros:** at 08 no `propagateLightSources`, no
   `checkBlock` has run for any chunk whose light could reach C — block-light
   PRE work for any chunk starts only in its 09 batch, and any chunk D whose
   block light could reach C (radius ≤ 1) has `D.09 ⇒ C ≥ 08 completed`,
   i.e. D's propagation is enqueued strictly after C's 08 dump finished
   (the dump runs before `completeFuture(08)`, §6 step 3). Registered
   sections hold fresh all-zero DataLayers ⇒ every value 0. ✔
2. **08 `light_sky` only 0x00/0xff rows, f-region from y=112 uniformly:**
   sky reads return 15 iff `sectionY >= visible.topSections[chunkColumn]`,
   else 0 from an all-zero layer (no propagation yet, exact 0/f dichotomy).
   `topSections` is per chunk-column (`zeroNode`), fed by `onNodeAdded` =
   (highest section that ever got a DataLayer)+1; DataLayers are created for
   every section with a non-zero SectionState byte — i.e. non-empty sections
   AND their 26-neighborhood (`updateSectionStatus` neighbor loop) ⇒
   `topSections = (max non-empty section over the 3×3 chunk neighborhood) + 1
   (neighbor registration) + 1 (onNodeAdded)`. Grid top non-air sections are
   4..5; every dumped chunk has a radius-1 neighbor with top section 5 ⇒
   topSections = 7 ⇒ 15 from section 7 = world y 112, uniformly. Per-chunk
   WORLD_SURFACE variation (79..92) stays within section 4..5 and therefore
   does not move the boundary. ✔ (bytecode-consistent; the exact "every chunk
   has a top-section-5 neighbor" is an empirical property of this seed area)
3. **08 order-independent, 09 order-dependent:** 08's dump-visible state for
   C depends only on section registrations (its own + same-batch peers —
   which cannot change any of C's values: other chunks' registrations only
   add zero layers and can only *raise* topSections of C's column via the
   3×3 rule, which is order-independent because ALL radius-1 neighbors'
   registrations… are NOT guaranteed before C's 08 dump — but chunks are
   dumped, and compared, per chunk: C's own column topSections depends on
   neighbor chunk registrations. Empirically uniform at y=112 in both
   bundles because in both bundle orders the radius-1 neighbors that carry
   top-section-5 had already registered (their 08 PRE precedes C's 08 POST
   whenever their 08 ran earlier — in the harness the 9 dumped chunks sit in
   the center of a 17×17 generated area and neighbor 08s are forced by C.09's
   radius-1 requirement… note C.09 ordering does not help C.08's dump; the
   uniformity across bundles is an empirical outcome, i.e. in both runs the
   relevant neighbors' 08 registrations happened to precede — implementers
   should reproduce the *rule*, then verify the 112 boundary against goldens).
   09 depends on which neighbors' 09 propagation and which feature-write
   checkBlocks landed in earlier/same batches ⇒ order-dependent. ✔
4. **09 torn seq windows in the alt bundle:** dump runs on L while W applies
   features concurrently ⇒ `OrderManifest.currentSeq()` moves between
   `seqBegin`/`seqEnd` (StageDumper.dump samples both around the file
   writes). Single-thread bundle can never tear. ✔

### 6.3 Implementation guidance for the C port (orchestration layer)

- Model the light engine as: `lightTasks` list of (PRE|POST, closure);
  `runUpdate(batch=1000)` = run PREs in order → full drain
  (`checkNodes; propagateDecreases; propagateIncreases; markNewInconsistencies;
  swapVisible`) → run POSTs in order (stage completions + dumps happen here).
- For the primary (single-thread) bundle, exact parity requires reproducing
  the global interleaving: main-loop `tryScheduleUpdate` polling plus
  dispatcher FIFO. A simpler sufficient model may be possible if the golden
  interleaving collapses (e.g. one runUpdate per stage call) — verify against
  order.snapshots (lane A's forensics).
- The visible/updating split matters only if dumps can race the engine; in a
  faithful single-threaded replay, reads at POST time equal the updating
  state after the batch drain.

---

## Appendix: quick class → role map

| class | role |
|---|---|
| `ChunkStatusTasks.initializeLight/light` | stage bodies (worldgen lane) |
| `ChunkPyramid.lambda$static$8/9/20/21` | step defs (radius 0 / radius 1 on 08) |
| `ChunkStep.apply` + `completeChunkGeneration` | status bump on future completion |
| `GenerationChunkHolder.applyStep/completeFuture` | per-status future publication |
| `ChunkMap.<init>` #316..#405 | "worldgen"/"light" lanes + dispatchers wiring |
| `ThreadedLevelLightEngine` | queue facade; PRE/POST; batch 1000; futures |
| `ServerChunkCache$MainThreadExecutor.pollTask` | the `tryScheduleUpdate` pump |
| `ChunkTaskDispatcher` / `ChunkTaskPriorityQueue` | per-chunk, per-level FIFO feeding a lane |
| `ConsecutiveExecutor`/`AbstractConsecutiveExecutor` | single-flight lane over shared pool |
| `LevelLightEngine` | block+sky fan-out; getLayerListener returns engines |
| `LightEngine.runLightUpdates` | drain + markNewInconsistencies + swap |
| `LayerLightSectionStorage` | updating vs volatile visible; copy-on-write |
| `Block/SkyLightSectionStorage.getLightValue` | read from **visible**; sky top-section=15 rule |
| `ProtoChunk.setBlockState` | light hook guard `status ≥ INITIALIZE_LIGHT` |
| `WorldGenRegion.setBlock` | delegates; no direct light calls |
| `ChunkStepMixin` (harness) | non-async `thenApply(dump)` ⇒ dump on light lane, pre-completeFuture |
