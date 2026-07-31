# A5 — The scheduler: why per-chunk order varies, and what a manifest linearization means (MC 26.2, unobfuscated server bytecode)

Source of truth: `javap -p -c -constants` (and `javap -v` for BootstrapMethods) against
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server`. All pseudocode is a 1:1 reconstruction
from bytecode. No vanilla-source guessing; where a number is *computed* from builder algebra rather
than read from a constant, it is flagged "derived".

Companion notes: task9pre A2 (`applyBiomeDecoration` internals), A3 (WorldgenRandom decoration
seeding), A6 (hook API surface). This note owns the scheduling/executor plumbing: who runs a
FEATURES body, on which thread, in what order, and with what concurrency guarantees.

HEADLINE RESULT (§5): all generation step bodies of one dimension are funneled through a single
`ConsecutiveExecutor("worldgen")` — **no two step bodies of the same dimension ever execute
concurrently, regardless of `max.bg.threads`**. The features-stage execution order is therefore a
genuine total order in every run, including multi-threaded ones. The order is nondeterministic
run-to-run (§7), but recording it *at body-execution time* and replaying it as a per-chunk sequence
is exactly faithful (§8/§9).

---

## 1. Class inventory (26.2 names)

| Role | Class |
|---|---|
| Per-chunk status futures + exactly-once step bump | `net.minecraft.server.level.GenerationChunkHolder` (superclass of `ChunkHolder`) |
| One "generate chunk P up to status S" state machine | `net.minecraft.server.level.ChunkGenerationTask` |
| Producer interface | `net.minecraft.server.level.GeneratingChunkMap` (impl: `ChunkMap`) |
| Per-chunk-pos priority queue | `net.minecraft.server.level.ChunkTaskPriorityQueue` (+ record `$TasksForChunk(long chunkPos, List<Runnable> tasks)`) |
| Queue front-end (4 internal lanes) | `net.minecraft.server.level.ChunkTaskDispatcher` |
| Serializing executors | `net.minecraft.util.thread.ConsecutiveExecutor` / `PriorityConsecutiveExecutor` (both extend `AbstractConsecutiveExecutor<T>`), interface `net.minecraft.util.thread.TaskScheduler<R>` |
| Shared background pool | `net.minecraft.util.Util` (26.2 delta: moved from `net.minecraft.Util`) + record `net.minecraft.TracingExecutor(ExecutorService service)` |
| Main-thread event loop | `net.minecraft.server.level.ServerChunkCache$MainThreadExecutor extends BlockableEventLoop<Runnable>` |

There is no `ChunkTaskPriorityQueueSorter`/mailbox architecture (1.18–1.20 era) anymore; the
1.21.2+ `ChunkGenerationTask`/`ChunkStep` architecture is what 26.2 ships.

---

## 2. Executor wiring — who owns which thread

### 2.1 `Util` background pool (bytecode-verified)

`net/minecraft/util/Util.class`:

```java
private static final String MAX_THREADS_SYSTEM_PROPERTY = "max.bg.threads";
private static final TracingExecutor BACKGROUND_EXECUTOR = makeExecutor("Main");   // static{}
private static final TracingExecutor IO_POOL       = makeIoExecutor("IO-Worker-", false);
private static final TracingExecutor DOWNLOAD_POOL = makeIoExecutor("Download-", true);

private static int getMaxThreads() {
    String raw = System.getProperty("max.bg.threads");
    if (raw != null) {
        try { int n = Integer.parseInt(raw); if (n >= 1 && n <= 255) return n; /* else log */ }
        catch (NumberFormatException e) { /* log */ }
    }
    return 255;
}

public static int maxAllowedExecutorThreads() {
    return Mth.clamp(Runtime.getRuntime().availableProcessors() - 1, 1, getMaxThreads());
}

private static TracingExecutor makeExecutor(String name) {          // name = "Main"
    int n = maxAllowedExecutorThreads();
    ExecutorService svc = n <= 0
        ? MoreExecutors.newDirectExecutorService()                   // dead branch: clamp lower bound is 1
        : new ForkJoinPool(n,
              pool -> { // lambda$makeExecutor$0, counter = new AtomicInteger(1)
                  String tn = "Worker-" + name + "-" + counter.getAndIncrement(); // recipe "Worker--"
                  ForkJoinWorkerThread t = new Util$2(pool, tn, name); t.setName(tn); return t;
              },
              Util::onThreadException,
              /*asyncMode*/ true);                                   // iconst_1 → FIFO mode
    return new TracingExecutor(svc);
}
```

- **`-Dmax.bg.threads=1` ⇒ `maxAllowedExecutorThreads() = clamp(nproc−1, 1, 1) = 1`: ONE
  ForkJoinPool worker named `Worker-Main-1`** (counter starts at 1). The direct-executor branch is
  unreachable (clamp lower bound 1), so it is always an FJP, never caller-runs.
- `TracingExecutor.execute(Runnable)` → `service.execute(wrapUnnamed(r))`; `wrapUnnamed` is identity
  unless Tracy profiling is attached. `forName(String)` returns the raw service outside IDE/Tracy.
- `MinecraftServer.<init>` does `this.executor = Util.backgroundExecutor()` (putfield #438); this
  exact `Executor` is passed: `MinecraftServer.createLevels` → `ServerLevel.<init>(…, Executor, …)`
  → `ServerChunkCache.<init>(…, Executor arg5, …)` → `ChunkMap.<init>(…, Executor arg5, …)`.

### 2.2 `ChunkMap.<init>` executor graph (bytecode offsets 316–402)

```java
ConsecutiveExecutor worldgenExec = new ConsecutiveExecutor(executor, "worldgen"); // slot 18
ConsecutiveExecutor lightExec    = new ConsecutiveExecutor(executor, "light");    // slot 19
this.worldgenTaskDispatcher = new ChunkTaskDispatcher(worldgenExec, executor);
this.lightTaskDispatcher    = new ChunkTaskDispatcher(lightExec,    executor);
this.lightEngine = new ThreadedLevelLightEngine(lightGetter, this, hasSkyLight, lightExec, lightTaskDispatcher);
```

- `worldgenExec` (slot 18) is used EXACTLY ONCE — as the `TaskScheduler` of the worldgen
  dispatcher. Negative finding: no other reference to slot 18 in the ctor, and grep of the class
  tree finds `ConsecutiveExecutor` referenced under `server/level` only by `ChunkMap`,
  `ThreadedLevelLightEngine`, `ChunkTaskDispatcher`.
- One `ChunkMap` per `ServerLevel` ⇒ one worldgen ConsecutiveExecutor **per dimension**.

### 2.3 `AbstractConsecutiveExecutor` — the serializer (full reconstruction)

```java
class AbstractConsecutiveExecutor<T extends Runnable> implements Runnable, TaskScheduler<T> {
    AtomicReference<Status> status = new AtomicReference<>(SLEEPING);  // SLEEPING/RUNNING/CLOSED
    StrictQueue<T> queue;    // ConsecutiveExecutor: QueueStrictQueue(ConcurrentLinkedQueue) — FIFO
                             // PriorityConsecutiveExecutor: FixedPriorityQueue(n) — n ConcurrentLinkedQueues, pop scans 0..n-1
    Executor executor;       // = Util.backgroundExecutor() here

    public void schedule(T task) { queue.push(task); registerForExecution(); }
    private void registerForExecution() {
        if (!isClosed() && !queue.isEmpty() && status.compareAndSet(SLEEPING, RUNNING))
            executor.execute(this);                      // one pool submission per task-run
    }
    public void run() {                                  // executes on an FJP worker
        try { Runnable r = queue.pop(); if (r != null) Util.runNamed(r, name); } // ONE task
        finally { status.compareAndSet(RUNNING, SLEEPING); registerForExecution(); }
    }
}
```

The `SLEEPING→RUNNING` CAS means **at most one `run()` is ever in flight**: every task scheduled on
a given ConsecutiveExecutor is mutually excluded from every other, and the CAS + concurrent queue
give a happens-before edge from task N to task N+1 even when they land on different pool threads.
`PriorityConsecutiveExecutor.scheduleWithResult(int priority, Consumer<CompletableFuture>)` wraps
into `StrictQueue$RunnableWithPriority(priority, …)`; plain `TaskScheduler.scheduleWithResult(c)`
(default method) is what the worldgen ConsecutiveExecutor uses (priority-less FIFO).

---

## 3. Producer side (main thread): ticket → `ChunkGenerationTask` → dispatcher

### 3.1 `ServerChunkCache.getChunkFutureMainThread(int x, int z, ChunkStatus status, boolean create)`

```java
ChunkPos pos = new ChunkPos(x, z); long key = pos.pack();
int level = ChunkLevel.byStatus(status);            // 33 + FULL_CHUNK_STEP.getAccumulatedRadiusOf(status)
ChunkHolder holder = getVisibleChunkIfPresent(key);
if (create) {
    addTicket(new Ticket(TicketType.UNKNOWN, level), pos);
    if (chunkAbsent(holder, level)) { runDistanceManagerUpdates(); holder = …; /* crash if still absent */ }
}
return chunkAbsent(holder, level) ? UNLOADED_CHUNK_FUTURE
                                  : holder.scheduleChunkGenerationTask(status, this.chunkMap);
```

`ServerChunkCache.getChunk(…)` (main thread) then does
`this.mainThreadProcessor.managedBlock(future::isDone); future.join()` — the main thread spins its
own `BlockableEventLoop` while waiting. `MainThreadExecutor.pollTask()` override:
`if (runDistanceManagerUpdates()) return true; lightEngine.tryScheduleUpdate(); return super.pollTask();`

### 3.2 `GenerationChunkHolder.scheduleChunkGenerationTask(ChunkStatus, ChunkMap)`

```java
if (isStatusDisallowed(status)) return UNLOADED_CHUNK_FUTURE;   // vs volatile highestAllowedStatus
CompletableFuture<ChunkResult<ChunkAccess>> f = getOrCreateFuture(status);  // futures: AtomicReferenceArray by status index
if (f.isDone()) return f;
ChunkGenerationTask t = this.task.get();                         // AtomicReference<ChunkGenerationTask>
if (t == null || status.isAfter(t.targetStatus)) rescheduleChunkTask(chunkMap, status);
return f;

private void rescheduleChunkTask(ChunkMap map, ChunkStatus status) {  // status may be null
    ChunkGenerationTask n = status != null ? map.scheduleGenerationTask(status, getPos()) : null;
    ChunkGenerationTask old = this.task.getAndSet(n);
    if (old != null) old.markForCancellation();                  // volatile boolean, polled cooperatively
}
```

### 3.3 `ChunkMap` producer methods (all main-thread)

```java
public ChunkGenerationTask scheduleGenerationTask(ChunkStatus status, ChunkPos pos) {
    ChunkGenerationTask t = ChunkGenerationTask.create(this, status, pos);
    this.pendingGenerationTasks.add(t);                      // plain ArrayList — main-thread confined
    return t;
}
public void runGenerationTasks() {                           // GeneratingChunkMap impl
    this.pendingGenerationTasks.forEach(this::runGenerationTask);
    this.pendingGenerationTasks.clear();
}
private void runGenerationTask(ChunkGenerationTask task) {
    GenerationChunkHolder center = task.getCenter();
    this.worldgenTaskDispatcher.submit(() -> {               // lambda$runGenerationTask$0
        CompletableFuture<?> wait = task.runUntilWait();
        if (wait != null) wait.thenRun(() -> runGenerationTask(task));  // resubmit from COMPLETING thread
    }, center.getPos().pack(), center::getQueueLevel);
}
```

`runGenerationTasks()` is called ONLY from `ServerChunkCache.runDistanceManagerUpdates()` (after
`DistanceManager.runAllUpdates` + `promoteChunkMap`), which is called from: `ServerChunkCache.tick`,
`getChunkFutureMainThread`, `save`, `addTicketAndLoadWithRadius`, and
`MainThreadExecutor.pollTask` — all on the Server thread. Negative finding: grep -rla
`runGenerationTasks` hits only `ServerChunkCache`, `GeneratingChunkMap`, `ChunkMap`.

Second producer of `scheduleChunkGenerationTask`: `ChunkMap.getChunkRangeFuture(holder, radius,
IntFunction<ChunkStatus>)` (used by `prepareTickingChunk`/`prepareAccessibleChunk`/
`prepareEntityTickingChunk`, radius 1, main-thread via `updateFutures`). No other call sites
(grep -rla `scheduleChunkGenerationTask` → `ChunkMap`, `ServerChunkCache`, `GenerationChunkHolder`
only).

---

## 4. `ChunkTaskDispatcher` + `ChunkTaskPriorityQueue` — the ordering machinery

### 4.1 Dispatcher: 4 fixed lanes on its own PriorityConsecutiveExecutor

```java
public class ChunkTaskDispatcher implements ChunkHolder.LevelChangeListener, AutoCloseable {
    public static final int DISPATCHER_PRIORITY_COUNT = 4;
    final ChunkTaskPriorityQueue queue;                     // named executor.name() + "_queue"
    final TaskScheduler<Runnable> executor;                 // = the "worldgen" ConsecutiveExecutor
    final PriorityConsecutiveExecutor dispatcher = new PriorityConsecutiveExecutor(4, bgExecutor, "dispatcher");
    protected boolean sleeping = true;                      // only touched inside dispatcher lane tasks

    // lane 0: onLevelChange(pos, IntSupplier queueLevelGetter, int targetLevel, IntConsumer setter)
    //   → queue.resortChunkTasks(queueLevelGetter.getAsInt(), pos, targetLevel); setter.accept(targetLevel);
    // lane 1: release(long pos, Runnable afterwards, boolean clearQueue)
    //   → queue.release(pos, clearQueue); onRelease(pos)/*no-op*/; if (sleeping) {sleeping=false; pollTask();} afterwards.run();
    // lane 2: submit(Runnable task, long pos, IntSupplier queueLevelGetter)
    //   → int lvl = queueLevelGetter.getAsInt();                  // EVALUATED AT DRAIN TIME, not submit time
    //     queue.submit(task, pos, lvl);
    //     if (sleeping) { sleeping = false; pollTask(); }
    // lane 3: pollTask()
    //   → TasksForChunk t = queue.pop();
    //     if (t == null) sleeping = true; else scheduleForExecution(t);

    protected void scheduleForExecution(TasksForChunk t) {
        CompletableFuture.allOf(t.tasks().stream()
                .map(r -> executor.scheduleWithResult(fut -> { r.run(); fut.complete(Unit.INSTANCE); }))
                .toArray(CompletableFuture[]::new))
            .thenAccept(v -> pollTask());                   // next batch only after this batch finished
    }
}
```

Because `sleeping` is only read/written inside dispatcher-lane tasks (single consecutive executor),
**at most one `TasksForChunk` batch is in flight at a time**, and each batch's runnables execute
FIFO on the worldgen ConsecutiveExecutor. Negative finding: `worldgenTaskDispatcher` is referenced
in `ChunkMap` only at ctor putfield, `onLevelChange` (resort), `runGenerationTask` (submit),
`hasWork`, `close` — `ChunkTaskDispatcher.release` is never invoked on the worldgen dispatcher
(only the light path uses it, via `ThreadedLevelLightEngine`).

### 4.2 Priority queue: level-indexed, insertion-ordered per level

```java
public class ChunkTaskPriorityQueue {
    public static final int PRIORITY_LEVEL_COUNT = ChunkLevel.MAX_LEVEL + 2;
    // ChunkLevel static{}: FULL_CHUNK_STEP = GENERATION_PYRAMID.getStepTo(FULL);
    //   RADIUS_AROUND_FULL_CHUNK = FULL_CHUNK_STEP.accumulatedDependencies().getRadius();  // derived: 11
    //   MAX_LEVEL = 33 + RADIUS_AROUND_FULL_CHUNK;                                         // derived: 44 → COUNT 46
    final List<Long2ObjectLinkedOpenHashMap<List<Runnable>>> queuesPerPriority; // size COUNT
    volatile int topPriorityQueueIndex = PRIORITY_LEVEL_COUNT;

    protected void submit(Runnable r, long pos, int level) {
        queuesPerPriority.get(level).computeIfAbsent(pos, k -> Lists.newArrayList()).add(r);
        topPriorityQueueIndex = Math.min(topPriorityQueueIndex, level);
    }
    protected void resortChunkTasks(int oldLevel, ChunkPos pos, int newLevel) {
        if (oldLevel >= PRIORITY_LEVEL_COUNT) return;
        List<Runnable> moved = queuesPerPriority.get(oldLevel).remove(pos.pack());
        /* advance topPriorityQueueIndex past empty levels if needed */
        if (moved != null && !moved.isEmpty()) {
            queuesPerPriority.get(newLevel).computeIfAbsent(pos.pack(), …).addAll(moved); // APPENDS AT TAIL
            topPriorityQueueIndex = Math.min(topPriorityQueueIndex, newLevel);
        }
    }
    public TasksForChunk pop() {          // called only from dispatcher lane 3
        if (!hasWork()) return null;
        var m = queuesPerPriority.get(topPriorityQueueIndex);
        long pos = m.firstLongKey(); List<Runnable> tasks = m.removeFirst();  // linked-map INSERTION order
        /* advance topPriorityQueueIndex past empty levels */
        return new TasksForChunk(pos, tasks);
    }
}
```

Within one priority level, chunks are served in **insertion order** (`Long2ObjectLinkedOpenHashMap`
firstLongKey/removeFirst). A resort moves a chunk **to the tail** of the new level. The priority of
a submit is `holder.getQueueLevel()` sampled when dispatcher lane 2 drains (§7-N1).
`ChunkHolder.<init>` sets `queueLevel = MAX_LEVEL + 1` initially; `ChunkHolder.updateFutures`
(main thread, after distance-manager runs) ends with
`onLevelChange.onLevelChange(pos, this::getQueueLevel, this.ticketLevel, this::setQueueLevel)`
fanned out by `ChunkMap.onLevelChange` to BOTH dispatchers — so `queueLevel` converges to
`ticketLevel`, applied asynchronously on dispatcher lane 0.

---

## 5. Consumer side: `ChunkGenerationTask.runUntilWait()` — where step bodies actually run

```java
public CompletableFuture<?> runUntilWait() {          // body of the worldgen-dispatcher runnable
    while (true) {
        CompletableFuture<?> wait = waitForScheduledLayer();
        if (wait != null) return wait;                // ≥1 pending future in current layer
        if (markedForCancellation || scheduledStatus == targetStatus) { releaseClaim(); return null; }
        scheduleNextLayer();
    }
}
private void scheduleNextLayer() {
    ChunkStatus next = (scheduledStatus == null) ? ChunkStatus.EMPTY
        : (!needsGeneration && scheduledStatus == EMPTY && !canLoadWithoutGeneration())
            ? (needsGeneration = true, ChunkStatus.EMPTY)               // re-run EMPTY layer at gen radius
            : ChunkStatus.getStatusList().get(scheduledStatus.getIndex() + 1);
    scheduleLayer(next, needsGeneration);
    scheduledStatus = next;
}
private void scheduleLayer(ChunkStatus status, boolean generate) {
    int r = (generate ? ChunkPyramid.GENERATION_PYRAMID : LOADING_PYRAMID)
                .getStepTo(this.targetStatus).getAccumulatedRadiusOf(status);
    for (int x = pos.x - r; x <= pos.x + r; x++)                        // x OUTER ascending
        for (int z = pos.z - r; z <= pos.z + r; z++) {                  // z INNER ascending
            GenerationChunkHolder h = cache.get(x, z);
            if (markedForCancellation || !scheduleChunkInLayer(status, generate, h)) return;
        }
}
private boolean scheduleChunkInLayer(ChunkStatus status, boolean generate, GenerationChunkHolder h) {
    ChunkStatus persisted = h.getPersistedStatus();
    boolean mustGen = persisted == null || status.isAfter(persisted);
    ChunkPyramid pyr = mustGen ? GENERATION_PYRAMID : LOADING_PYRAMID;  // (mustGen && !generate) → ISE
    CompletableFuture<ChunkResult<ChunkAccess>> f = h.applyStep(pyr.getStepTo(status), chunkMap, cache);
    ChunkResult<ChunkAccess> now = f.getNow(null);
    if (now == null)      { scheduledLayer.add(f); return true; }       // pending → wait list
    if (now.isSuccess())  return true;
    markForCancellation(); return false;
}
private CompletableFuture<?> waitForScheduledLayer() {
    while (!scheduledLayer.isEmpty()) {
        CompletableFuture<ChunkResult<ChunkAccess>> last = scheduledLayer.getLast();
        ChunkResult<ChunkAccess> now = last.getNow(null);
        if (now == null) return last;                                   // still pending → suspend task
        scheduledLayer.removeLast();
        if (!now.isSuccess()) markForCancellation();
    }
    return null;
}
```

`ChunkGenerationTask.create(map, status, pos)`:
`radius = GENERATION_PYRAMID.getStepTo(status).getAccumulatedRadiusOf(EMPTY)`;
`cache = StaticCache2D.create(pos.x, pos.z, radius, (x,z) -> map.acquireGeneration(ChunkPos.pack(x,z)))`
(`ChunkMap.acquireGeneration` = `updatingChunkMap.get(key)` + `increaseGenerationRefCount`).
`releaseClaim()` = `cache.get(center).removeTask(this)` + `cache.forEach(map::releaseGeneration)`.

### 5.1 `GenerationChunkHolder.applyStep` — exactly-once per (chunk, status), globally

```java
CompletableFuture<ChunkResult<ChunkAccess>> applyStep(ChunkStep step, GeneratingChunkMap map, StaticCache2D cache) {
    if (isStatusDisallowed(step.targetStatus())) return UNLOADED_CHUNK_FUTURE;
    return acquireStatusBump(step.targetStatus())            // startedWork.compareAndExchange(parent, target)
        ? map.applyStep(this, step, cache).handle((chunk, thr) -> {   // lambda$applyStep$0
              if (thr != null) BlockableEventLoop.relayDelayCrash(CrashReport.forThrowable(thr, "Exception chunk generation/loading"));
              else completeFuture(step.targetStatus(), chunk);
              return ChunkResult.of(chunk); })
        : getOrCreateFuture(step.targetStatus());            // loser: just observe winner's future
}
```

The `startedWork` CAS guarantees each (chunk, status) step body is entered **once per JVM run** no
matter how many overlapping ChunkGenerationTasks exist.

### 5.2 `ChunkMap.applyStep` — the step body is synchronous on the calling thread

```java
public CompletableFuture<ChunkAccess> applyStep(GenerationChunkHolder h, ChunkStep step, StaticCache2D cache) {
    if (step.targetStatus() == ChunkStatus.EMPTY) return scheduleChunkLoad(h.getPos());   // async, see below
    ChunkAccess parent = cache.get(pos.x, pos.z).getChunkIfPresentUnchecked(step.targetStatus().getParent());
    if (parent == null) throw new IllegalStateException("Parent chunk missing");
    return step.apply(this.worldGenContext, cache, parent);      // runs INLINE on current thread
}
```

`ChunkStep.apply` calls `this.task.doWork(ctx, this, cache, chunk)` inline (plus JFR profiling and a
`thenApply(completeChunkGeneration)` that sets `ProtoChunk.setPersistedStatus`).
`ChunkStatusTasks.generateFeatures` (verified) is fully synchronous:
`Heightmap.primeHeightmaps(chunk, EnumSet.of(MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES, OCEAN_FLOOR, WORLD_SURFACE))`
→ `new WorldGenRegion(level, cache, step, chunk)` → (unless `DEBUG_DISABLE_FEATURES`)
`generator.applyBiomeDecoration(region, chunk, level.structureManager().forWorldGenRegion(region))`
→ `Blender.generateBorderTicks` → `completedFuture(chunk)`. Same shape for structure_starts…carvers.
The only async step bodies are:

- EMPTY: `scheduleChunkLoad` = `readChunk` (IO pool) → `thenApplyAsync(parse, Util.backgroundExecutor().forName("parseChunk"))`
  → `thenCombine(poiManager.prefetch)` → **`thenApplyAsync(…, this.mainThreadExecutor)`** (and
  `exceptionallyAsync(mainThreadExecutor)`) — completes on the Server thread;
- INITIALIZE_LIGHT / LIGHT: return `ThreadedLevelLightEngine` futures (light dispatcher path).

### 5.3 The serialization theorem (the load-bearing fact)

Chain of custody for a FEATURES body:
`ChunkStatusTasks.generateFeatures` ← `ChunkStep.apply` ← `ChunkMap.applyStep` ←
`GenerationChunkHolder.applyStep` ← `ChunkGenerationTask.scheduleChunkInLayer` ← `scheduleLayer` ←
`runUntilWait` ← the Runnable submitted by `ChunkMap.runGenerationTask` ← popped as part of a
`TasksForChunk` batch ← executed via `worldgenExec.scheduleWithResult` on the **"worldgen"
ConsecutiveExecutor**.

Since (a) that executor runs at most one task at a time (§2.3 CAS), (b) it is fed only by the
worldgen dispatcher (§2.2 negative finding), and (c) the dispatcher keeps at most one batch in
flight (§4.1), **every generation step body of a dimension — including every FEATURES body —
executes under a global per-dimension mutex.** With `max.bg.threads=255` the bodies may hop between
`Worker-Main-N` threads, but never overlap in time; the CAS/queue handoff provides happens-before
between consecutive bodies. (Cross-dimension bodies can overlap, but dimensions share no chunks,
seeds or RNG streams.)

Grep evidence for (b)+(c): `ChunkTaskDispatcher.submit` invoked only at `ChunkMap.runGenerationTask`;
`GenerationChunkHolder.applyStep` invoked only from `ChunkGenerationTask.scheduleChunkInLayer`
(grep -rla `applyStep` → GenerationChunkHolder, GeneratingChunkMap, ChunkGenerationTask, ChunkMap;
javap of each shows the calls listed above and no others).

---

## 6. GENERATION_PYRAMID radii used by the layer walk (for completeness)

From `ChunkPyramid.static{}` + lambdas (`javap -c`), requirement list per step
(`addRequirement(status, radius)`, `blockStateWriteRadius`):

| step | requirements | writeRadius |
|---|---|---|
| EMPTY | — | — |
| STRUCTURE_STARTS | — | — |
| STRUCTURE_REFERENCES | SS@8 | — |
| BIOMES | SS@8 | — |
| NOISE | SS@8, BIOMES@1 | 0 |
| SURFACE | SS@8, BIOMES@1 | 0 |
| CARVERS | SS@8 | 0 |
| **FEATURES** | **SS@8, CARVERS@1** | **1** |
| INITIALIZE_LIGHT | — | — |
| LIGHT | INITIALIZE_LIGHT@1 | — |
| SPAWN | BIOMES@1 | — |
| FULL | — | — |

FEATURES `directDependencies` (index=radius): `[0..1]=CARVERS, [2..8]=STRUCTURE_STARTS` — a features
body reads radius-1 neighbors at ≥CARVERS and may WRITE blocks at chessboard distance ≤1
(`WorldGenRegion.writeRadius=1`). Derived accumulated radii (builder algebra per task8 A1 §2.1, not
re-verified numerically): FEATURES step acc radius of EMPTY = 10; FULL = 11 ⇒ `MAX_LEVEL = 44`,
`PRIORITY_LEVEL_COUNT = 46`. These constants only size the priority queue; they do not affect
manifest design.

Consequence of the layer walk + radii: before any FEATURES body for chunk A runs, every chunk within
chessboard distance 1 of A has completed CARVERS (the task's CARVERS layer, radius ≥ features
radius + 1, completed first and `waitForScheduledLayer` gated on it). A chunk CAN still receive
block writes from a neighbor's later FEATURES after its own FEATURES/INITIALIZE_LIGHT/LIGHT ran
(INITIALIZE_LIGHT adds no radius requirement) — see §9(b).

---

## 7. Residual run-to-run order nondeterminism with `-Dmax.bg.threads=1`

Measured fact (committed NOTES.md): features order differs run-to-run even with one bg worker.
Reconciliation — the single worker serializes *execution*, but the *queue contents at each pop* are
built by a main-thread/worker race. Grounded mechanisms, in decreasing expected impact:

- **N1 — queue-level sampling race.** Priority of a submit = `holder.getQueueLevel()` evaluated when
  dispatcher lane 2 drains (§4.1), while `queueLevel` is only updated by lane-0 tasks queued by the
  main thread's `updateFutures`. Initial value is `MAX_LEVEL+1` (§4.2) until the first lane-0 task
  lands. Whether a given submit is filed under the stale or fresh level depends on lane arrival
  order in the `FixedPriorityQueue` — pure wall-clock.
- **N2 — resort-vs-pop race.** `resortChunkTasks` re-appends the chunk at the TAIL of the new
  level's insertion-ordered map (§4.2). If the resort drains before the chunk is popped, its
  position changes; after, it doesn't. Same logical workload ⇒ different pop order.
- **N3 — resubmission arrival race.** When `runUntilWait` suspends on a pending layer future, the
  `thenRun(() -> runGenerationTask(task))` resubmit executes on whatever thread completes that
  future: the **main thread** for EMPTY layers (`scheduleChunkLoad` finishes with
  `thenApplyAsync(mainThreadExecutor)`, §5.2) and the light path for LIGHT layers. The resubmit
  re-enters lane 2 and is appended at the current tail of its level — its position relative to
  fresh submits depends on IO latency and main-loop timing.
- **N4 — producer batching.** How many `pendingGenerationTasks` accumulate before each
  `runGenerationTasks()` drain depends on tick boundaries and `managedBlock` spin timing on the main
  thread (§3.3), changing submit grouping and hence insertion order.
- **Not a source:** FJP work-stealing (1 worker); concurrent step bodies (impossible, §5.3);
  `RandomSupport.generateUniqueSeed()`-style seeding (features RNG is fully re-seeded per chunk —
  task9pre A3).

All four mechanisms permute only *which chunk's batch is popped next*; they cannot interleave two
bodies or split one chunk's features into parts. This is exactly why the committed golden runs show
differing 07 orders but each run is internally consistent.

---

## 8. Q3 — concurrency safety: can overlapping FEATURES regions run concurrently?

**No — structurally impossible in 26.2, in any configuration.** There is no region/lock machinery
to make it safe either (negative findings: no locks in `GenerationChunkHolder`/`ChunkGenerationTask`
beyond the atomics above; `StaticCache2D` is a plain final array; `acquireStatusBump` only
deduplicates the SAME chunk's step; `ChunkAccess.carverBiome`/`getOrCreateNoiseChunk` lazy caches
are unsynchronized). Vanilla's only defense is the per-dimension worldgen ConsecutiveExecutor mutex
(§5.3) — and it is airtight: every FEATURES body in a recording run occupies a disjoint time
interval, with happens-before edges between consecutive intervals.

Therefore the features-stage execution history of ANY run (single- or multi-threaded) **is already a
total order per dimension**; "linearization" is not an approximation we impose but the literal
schedule. A manifest that assigns seq = position of the body in that order loses nothing.

---

## 9. Q4 — implications for the order manifest

**Verdict: (per-chunk total order, keyed at decoration-seed-set time) is sufficient for bit-exact
07_features+ replay, including manifests recorded under `max.bg.threads>1` — with the caveats
below.** Combined with A2/A3 (within-body iteration is deterministic given seed/registries/chunk
pos) this confirms the working hypothesis: cross-chunk body order is the only free variable.

1. **Hook placement:** record inside the body, not at scheduling. Submission/pop order ≠ execution
   order only in degenerate cases, but execution order is the thing that matters and it is trivially
   observable: any point inside `generateFeatures`/`applyBiomeDecoration` (e.g. where
   `setDecorationSeed(worldSeed, minBlockX, minBlockZ)` fires — A3) executes under the global mutex,
   so a plain `ArrayList.add` / monotonically increasing counter needs **no synchronization** and
   can never tear or interleave. Record `(seq, dimension, chunkX, chunkZ)`; optionally the
   decoration seed as a checksum.
2. **Per-chunk uniqueness:** `acquireStatusBump` guarantees exactly one FEATURES body per chunk per
   run — the manifest is a permutation of the generated chunk set, never a multiset. (Cancellation
   via `markForCancellation` happens between layers, not inside a body; a cancelled task never
   half-runs a features body.)
3. **Record everything, not just the compare set.** FEATURES writes at chessboard distance ≤1
   (writeRadius 1), so the 9-chunk compare set's final state depends on the features order of every
   chunk within distance 1 of it — and transitively the RNG of those chunks is position-seeded (A3),
   so only the ORDER of the enclosing set matters, not far-away chunks. Simplest correct rule: the
   manifest lists ALL features bodies executed in the run, in seq order; the C replay executes the
   full list.
4. **Dump timing ≠ features-completion time.** A chunk can be written by a neighbor's later FEATURES
   after its own FEATURES — and even after its INITIALIZE_LIGHT/LIGHT (§6: INITIALIZE_LIGHT adds no
   radius requirement). A per-chunk 07 dump taken at that chunk's features completion is NOT final.
   The 07_features dump must be taken after the last features body that can write into the chunk
   (safe choice: after the whole recorded order has executed), and the C side must compare at the
   same point. This is a Task-2 (dump-mod) requirement, flagged here because the scheduler is what
   makes it visible.
5. **Multi-threaded recordings are fine.** §5.3/§8: even at `max.bg.threads=255` bodies are totally
   ordered with happens-before between them; a seq counter incremented inside the body is coherent.
   No need to force `=1` for recording (though it changes nothing to keep it).
6. **Replay needs no queue emulation.** Priorities, tickets, lanes, resorts (§4, §7) only decide the
   order; they have zero effect on body contents. The C replay should consume the manifest order
   directly and must NOT try to re-derive it from ticket logic — that logic is wall-clock-dependent
   by construction.
7. **What the manifest need NOT record** (all deterministic given seed+registries+pos, per A1–A3 and
   §5/§6): the 17×17 loops and per-step internals; the layer-walk iteration order (x-outer/z-inner
   ascending — affects only which holder futures get created first, not body content); the
   loading-vs-generation pyramid choice (fresh world ⇒ always generation); heightmap priming (part
   of the body itself).

---

## 10. Negative findings (explicit)

- `net.minecraft.Util` does not exist in 26.2; the class is `net.minecraft.util.Util`
  (`javap` error "class not found" on the former, class file present for the latter).
- No `ChunkTaskPriorityQueueSorter`, no `ProcessorMailbox`/`ProcessorHandle` chunk mailboxes in
  `server/level` — replaced by `ChunkTaskDispatcher`/`ConsecutiveExecutor`.
- grep -rla `scheduleChunkGenerationTask` over the class tree: only `ChunkMap`, `ServerChunkCache`,
  `GenerationChunkHolder`.
- grep -rla `runGenerationTasks`: only `ServerChunkCache`, `GeneratingChunkMap`, `ChunkMap`.
- grep -rla `applyStep`: only `GenerationChunkHolder`, `GeneratingChunkMap`, `ChunkGenerationTask`,
  `ChunkMap` (no mods/other stages reuse the step machinery).
- The worldgen `ConsecutiveExecutor` (ChunkMap ctor local slot 18) has exactly one consumer (the
  worldgen `ChunkTaskDispatcher`); `ChunkTaskDispatcher.release` is never called for it in `ChunkMap`.
- `ChunkTaskDispatcher.submit` for the worldgen dispatcher has exactly one call site
  (`ChunkMap.runGenerationTask`).
- No worldgen step body is ever scheduled on the main thread executor: `ChunkMap.mainThreadExecutor`
  appears in the generation path only as the completion executor of `scheduleChunkLoad` and the
  `thenApplyAsync` of prepare*Chunk futures (post-FULL conversion), never around `ChunkStep.apply`.

## 11. OPEN items

- OPEN: accumulated-radius numbers (FEATURES=10, FULL=11 ⇒ MAX_LEVEL=44) are derived from the
  `ChunkStep$Builder` algebra documented in task8 A1 §2.1, not re-read from runtime constants; only
  queue sizing depends on them.
- OPEN: `BlockableEventLoop.relayDelayCrash` path (step body threw) aborts the server — manifest
  recording can ignore partial runs, noted for completeness.
- OPEN: `StaticCache2D.create` argument order `(centerX, centerZ, radius, initializer)` inferred
  from the single call site + `releaseClaim` usage; internal layout not disassembled (not
  order-relevant).
