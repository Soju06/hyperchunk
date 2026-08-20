# R2 — LightEngine core algorithm + BlockLightEngine (Task 10, Lane B2)

Source of truth: `javap -p -c -constants` of the real 26.2 unobfuscated server jar
(`tools/golden/libs/extracted/server-26.2.jar`). All classes
`net.minecraft.world.level.lighting.*` unless noted. Landmarks cited as
`Method.name @#pc`.

---

## 1. Queue mechanics — what the engine actually uses

`LightEngine` fields (class header + `<init>`):

| field | type | init |
|---|---|---|
| `blockNodesToCheck` | `it.unimi.dsi.fastutil.longs.LongOpenHashSet` | `new LongOpenHashSet(512, 0.5f)` (`<init>` @#5–#16, sipush 512, ldc 0.5f) |
| `decreaseQueue` | `LongArrayFIFOQueue` | `<init>` @#21 |
| `increaseQueue` | `LongArrayFIFOQueue` | `<init>` @#32 |
| `lastChunkPos[2]` / `lastChunk[2]` | 2-entry MRU chunk cache | `CACHE_SIZE = 2` |

**The 26.2 engine does NOT use `SpatialLongSet`, `LeveledPriorityQueue`, or
`DynamicGraphMinFixedPoint`.** Verified by scanning constant pools of every
class file in `net/minecraft/world/level/lighting/`: only
`DynamicGraphMinFixedPoint` itself references `LeveledPriorityQueue`/
`SpatialLongSet`; `DynamicGraphMinFixedPoint` is extended only by the
chunk-distance trackers (`net.minecraft.server.level.ChunkTracker`,
`SectionTracker`, etc.), i.e. it is dead weight for lighting. The light
engine is plain FIFO + hash set.

**Queue entry = TWO consecutive longs.** `enqueueIncrease(long pos, long entry)`
enqueues `pos` then `entry` (`enqueueIncrease` @#4 then @#12); dequeue reads
them back in the same order (`propagateIncreases` @#16 → pos, @#24 → entry).
`pos` is `BlockPos.asLong()` packing: X<<38 | (Z&0x3FFFFFF)<<12 | (Y&0xFFF)
(from `BlockPos.<clinit>`: `PACKED_HORIZONTAL_LENGTH = 1+log2(ceilPow2(30000000)) = 26`,
`PACKED_Y_LENGTH = 64-52 = 12`, `Y_OFFSET=0`, `Z_OFFSET=12`, `X_OFFSET=38`).

`checkBlock(BlockPos)` = `blockNodesToCheck.add(pos.asLong())` (`checkBlock`
@#8), nothing else. Deduplicated, hash-ordered.

`hasLightWork()` = `storage.hasInconsistencies() || !blockNodesToCheck.isEmpty()
|| !decreaseQueue.isEmpty() || !increaseQueue.isEmpty()` (@#4/#14/#24/#34).

## 2. `LightEngine$QueueEntry` bit encoding (64-bit long, low 12 bits used)

Constants (from class `LightEngine$QueueEntry`):
`FROM_LEVEL_BITS = 4`, `DIRECTION_BITS = 6`, `LEVEL_MASK = 15L`,
`DIRECTIONS_MASK = 1008L` (= 0b1111110000, bits 4..9),
`FLAG_FROM_EMPTY_SHAPE = 1024L` (bit 10),
`FLAG_INCREASE_FROM_EMISSION = 2048L` (bit 11).

| bits | meaning |
|---|---|
| 0–3 | `fromLevel` (the light level being propagated *from*; `getFromLevel` = `entry & 15`) |
| 4–9 | direction mask; bit for direction d = `1L << (d.ordinal() + 4)` (`withDirection` @#1–#9, `shouldPropagateInDirection` @#1–#9). Direction ordinals from `Direction.<clinit>`: DOWN=0, UP=1, NORTH=2, SOUTH=3, WEST=4, EAST=5 → bits DOWN=16, UP=32, NORTH=64, SOUTH=128, WEST=256, EAST=512 |
| 10 | FROM_EMPTY_SHAPE — at enqueue time `isEmptyShape(sourceState)` was true; consumer substitutes `AIR.defaultBlockState()` for the source state instead of re-reading the world (`BlockLightEngine.propagateIncrease` @#147–#160) |
| 11 | INCREASE_FROM_EMISSION — entry is an emission seed; may *write* `fromLevel` into its own position (see §4) |

Constructors:
- `decreaseAllDirections(level)` = `withLevel(1008, level)` → all 6 dir bits + level.
- `decreaseSkipOneDirection(level, dir)` = `withLevel(withoutDirection(1008, dir), level)`.
- `increaseLightFromEmission(level, fromEmptyShape)` = `withLevel(1008 | 2048 [| 1024], level)` — all directions.
- `increaseSkipOneDirection(level, fromEmptyShape, dir)` = level + (all dirs minus `dir`) [+1024].
- `increaseOnlyOneDirection(level, fromEmptyShape, dir)` = level + single dir bit [+1024]. (Note: no emission flag.)
- `increaseSkySourceInDirections(down,north,south,west,east)` = level 15 + subset of {DOWN,NORTH,SOUTH,WEST,EAST} — sky only, never UP.
- `withLevel(e, l)` = `(e & ~15) | (l & 15)` — **level is masked to 4 bits**; no value >15 can be encoded.

`PULL_LIGHT_IN_ENTRY = QueueEntry.decreaseAllDirections(1)` = `1008|1 = 1009 = 0x3F1`
(`LightEngine.<clinit>` @#0–#4). Semantics in §5.

`PROPAGATION_DIRECTIONS = Direction.values()` — all 6 directions in ordinal
order DOWN, UP, NORTH, SOUTH, WEST, EAST (`<clinit>` @#7).

## 3. `runLightUpdates()` — global order (LightEngine.runLightUpdates)

```
1. for each pos in blockNodesToCheck (LongOpenHashSet iteration = hash order):
       checkNode(pos)                              // @#8–#27 loop
2. blockNodesToCheck.clear(); blockNodesToCheck.trim(512)   // @#30–#44
3. count += propagateDecreases()                   // @#50–#52  — FULLY drained first
4. count += propagateIncreases()                   // @#57–#59
5. clearChunkCache()                               // @#65
6. storage.markNewInconsistencies(this)            // @#73
7. storage.swapSectionMap()                        // @#80  — publish updating→visible
8. return count (number of processed queue entries)
```

**Decrease queue is fully drained before the increase queue.**
`propagateDecreases` (@#2–#42) loops until `decreaseQueue.isEmpty()`; the
abstract `propagateDecrease` may enqueue *both* further decreases (cascade)
and increases (re-seed); `propagateIncreases` (@#2–#97) then drains
`increaseQueue`; `propagateIncrease` enqueues only increases. So after
`runLightUpdates` both queues are empty.

`propagateIncreases` per-entry logic (@#29–#91):

```
pos   = dequeue(); entry = dequeue()
level = storage.getStoredLevel(pos)                 // @#34 (updating map, UNGUARDED — pos must be in a stored section)
from  = QueueEntry.getFromLevel(entry)              // @#41
if isIncreaseFromEmission(entry) && level < from:   // @#48–#58
    storage.setStoredLevel(pos, from); level = from // @#66  — SELF-WRITE of emission
if level == from: propagateIncrease(pos, entry, level)  // @#79–#88 — stale entries (level != from) dropped
```

`propagateDecreases` per-entry: `propagateDecrease(pos, entry)` unconditionally (@#33).

## 4. `BlockLightEngine.checkNode(long pos)` — the recompute rule

There is **no `getComputedLevel`** in the 26.2 engine (that was the old
`DynamicGraphMinFixedPoint` API). Recompute is expressed through
decrease/pull-in:

```
sectionPos = SectionPos.blockToSection(pos)               // @#1
if !storage.storingLightForSection(sectionPos): return    // @#13–#19
state    = getState(pos)                                  // @#29
emission = getEmission(pos, state)                        // @#38 (gated, see §6)
stored   = storage.getStoredLevel(pos)                    // @#51
if emission < stored:                                     // @#60
    storage.setStoredLevel(pos, 0)                        // @#72
    enqueueDecrease(pos, decreaseAllDirections(stored))   // @#79–#82 (fromLevel = OLD stored)
else:
    enqueueDecrease(pos, PULL_LIGHT_IN_ENTRY)             // @#90–#93 (fromLevel = 1, all dirs)
if emission > 0:
    enqueueIncrease(pos, increaseLightFromEmission(emission, isEmptyShape(state)))  // @#105–#113
```

The `PULL_LIGHT_IN_ENTRY` (fromLevel=1) run through `propagateDecrease` never
clears any neighbor (clear branch requires `neighborLevel <= 1-1 = 0`, but
`neighborLevel == 0` is skipped earlier), so its only effect is the else
branch: every neighbor with level > 0 gets
`increaseOnlyOneDirection(neighborLevel, false, oppositeDir)` — i.e. "ask all
6 neighbors to re-push their light into me". That is how a node's level is
recomputed from neighbors + emission after a block change.

## 5. `BlockLightEngine.propagateIncrease(pos, entry, level)` — exact neighbor rule

(bytecode @#3–#237; `fromState` computed lazily once, cached in local 6)

```
fromState = null
for dir in [DOWN, UP, NORTH, SOUTH, WEST, EAST]:                    // PROPAGATION_DIRECTIONS
    if !QueueEntry.shouldPropagateInDirection(entry, dir): continue // @#33
    nPos = BlockPos.offset(pos, dir)                                // @#45
    if !storage.storingLightForSection(blockToSection(nPos)): continue  // @#62 — hard wall
    nLevel = storage.getStoredLevel(nPos)                           // @#80
    if level - 1 <= nLevel: continue                                // @#91–#95 (cheap upper bound, opacity>=1)
    nState = getState(nPos)                                         // @#116
    newLevel = level - getOpacity(nState)                           // @#126–#129; getOpacity = max(1, state.getLightDampening())  [LightEngine.getOpacity @#0–#5]
    if newLevel <= nLevel: continue                                 // @#134–#136
    if fromState == null:
        fromState = isFromEmptyShape(entry) ? AIR.defaultBlockState()
                                            : getState(pos)         // @#147–#175
    if shapeOccludes(fromState, nState, dir): continue              // @#184
    storage.setStoredLevel(nPos, newLevel)                          // @#201
    if newLevel > 1:                                                // @#206 — level-1 light cannot spread further
        enqueueIncrease(nPos, increaseSkipOneDirection(newLevel, isEmptyShape(nState), dir.getOpposite()))  // @#225–#228
```

Key facts:
- Opacity applied is that of the **destination** block: `max(1, dampening(dest))`
  (`MIN_OPACITY = 1`). The source's opacity plays no role in spreading.
- Occlusion is a separate boolean gate (`shapeOccludes`), not additive.
- Writes are strictly increasing at the written position (`newLevel > nLevel`).
- The back-direction is excluded from the child entry (`skipOneDirection(opposite)`).

## 6. `BlockLightEngine.propagateDecrease(pos, entry)` — exact neighbor rule

(bytecode @#0–#213)

```
from = QueueEntry.getFromLevel(entry)                               // @#1
for dir in PROPAGATION_DIRECTIONS:
    if !shouldPropagateInDirection(entry, dir): continue            // @#36
    nPos = offset(pos, dir)
    if !storingLightForSection(section(nPos)): continue             // @#65
    nLevel = getStoredLevel(nPos)                                   // @#83
    if nLevel == 0: continue                                        // @#88–#90
    if nLevel <= from - 1:                                          // @#96–#102  (i.e. nLevel < from: could depend on us → clear)
        nState   = getState(nPos)                                   // @#115
        emission = getEmission(nPos, nState)                        // @#125
        storage.setStoredLevel(nPos, 0)                             // @#140
        if emission < nLevel:                                       // @#147
            enqueueDecrease(nPos, decreaseSkipOneDirection(nLevel, dir.getOpposite()))   // @#160–#163
        if emission > 0:                                            // @#168
            enqueueIncrease(nPos, increaseLightFromEmission(emission, isEmptyShape(nState)))  // @#181–#184
    else:                                                           // nLevel >= from: independent survivor at the boundary
        enqueueIncrease(nPos, increaseOnlyOneDirection(nLevel, false, dir.getOpposite())) // @#201–#204
```

Decreases ignore opacity and occlusion entirely (over-clear, then the increase
phase re-fills). `fromEmptyShape=false` in the boundary re-seed forces the
increase pass to re-read the survivor's real state — no staleness.

## 7. Opacity / shape-occlusion semantics

- `LightEngine.getOpacity(state)` = `Math.max(1, state.getLightDampening())`
  (@#0–#5). `MIN_OPACITY = 1`.
- `isEmptyShape(state)` = `!state.canOcclude() || !state.useShapeForLightOcclusion()`
  (@#1–#15). **A plain full opaque block (stone: useShapeForLightOcclusion=false)
  is an EMPTY shape** for occlusion purposes — its blocking is entirely via
  dampening=15. Only blocks with `useShapeForLightOcclusion=true` (stairs,
  slabs, pistons, snow layers, …) participate in shape occlusion.
- `getOcclusionShape(state, dir)` = `isEmptyShape(state) ? Shapes.empty()
  : state.getFaceOcclusionShape(dir)` (@#1–#15).
- `shapeOccludes(from, to, dir)` = `Shapes.faceShapeOccludes(
  getOcclusionShape(from, dir), getOcclusionShape(to, dir.getOpposite()))`
  (@#2–#21). Direction convention: `dir` points from source toward destination;
  source contributes its face shape in `dir`, destination its face shape in
  the opposite direction.
- `Shapes.faceShapeOccludes(a, b)` (Shapes bytecode @#0–#57): returns true if
  `a == Shapes.block() || b == Shapes.block()`; false if both `isEmpty()`;
  otherwise true iff `joinIsNotEmpty(block(), joinUnoptimized(a, b, OR),
  ONLY_FIRST)` is false — i.e. the union of the two face shapes covers the
  entire unit face.
- `getLightDampeningInto(from, to, dir, opacity)` (@#0–#73): both empty shapes
  → return `opacity` unchanged; else build fromShape = empty-if-emptyShape
  else `getOcclusionShape()` (full 3D occlusion shape, not face shape), same
  for to; if `Shapes.mergedFaceOccludes(fromShape, toShape, dir)` → return 16
  (fully blocked), else return `opacity`. `mergedFaceOccludes` slices the
  facing boundary layers of both shapes (last layer of the POSITIVE-side
  shape, first layer of the other, after a fuzzy `max==1.0`/`min==0.0`
  check with tolerance 1e-7) and tests whether their union covers the face.
  **This helper is NOT called anywhere in the propagation path.** Whole-jar
  scan: only callers are `world.level.block.SpreadingSnowyBlock` and
  `NyliumBlock` (grass/nylium survival check). Do not use it in the C engine.

## 8. BlockLightEngine specifics

### getEmission (private, @#0–#32)
```
e = state.getLightEmission()
return (e > 0 && storage.lightOnInSection(blockToSection(pos))) ? e : 0
```
`lightOnInSection(sectionPos)` = `columnsWithSources.contains(SectionPos.getZeroNode(sectionPos))`
(LayerLightSectionStorage @#1–#10) — the column (x,z, y=0 node) must have been
enabled via `setLightEnabled(chunkPos, true)`.

**Self-emission rule:** yes, a block emits into its OWN position even if
opaque. The seed entry `increaseLightFromEmission(e, isEmptyShape(state))`
carries the emission; the consumer writes `from` into the entry's own pos
whenever `stored < from` (propagateIncreases @#48–#66, §3) with no opacity or
shape test at the emitting pos. An opaque emitter (jack-o'-lantern:
canOcclude=true, dampening 15, emission 15) therefore stores 15 at its own
pos; spreading from it uses `FROM_EMPTY_SHAPE` = `isEmptyShape(state)` which
is true for full cubes → occlusion behaves as if from air; the *neighbor's*
opacity still applies.

### propagateLightSources(ChunkPos) (@#0–#40)
```
setLightEnabled(chunkPos, true)          // @#3 → storage.columnsWithSources.add(zeroNode)  FIRST
chunk = chunkSource.getChunkForLighting(x, z)   // @#18
if chunk != null:
    chunk.findBlockLightSources((pos, state) ->
        enqueueIncrease(pos.asLong(), increaseLightFromEmission(state.getLightEmission(), isEmptyShape(state))))
        // lambda$propagateLightSources$0 @#0–#18 — raw emission, NOT gated by getEmission
```
`ChunkAccess.findBlockLightSources` = `findBlocks(state -> state.getLightEmission() != 0, consumer)`
(ChunkAccess @#1–#7; predicate = `lambda$findBlockLightSources$0`).
`findBlocks` scan order (ChunkAccess.findBlocks bytecode): sections ascending
`minSectionY..maxSectionY`; skip section if `!section.maybeHas(predicate)`
(palette-level conservative check, `LevelChunkSection.maybeHas` →
`PalettedContainer.maybeHas`); inside a section: `for y 0..15: for z 0..15:
for x 0..15` — `getBlockState(x=var9, y=var7, z=var8)` with y outermost, x
innermost (@#64–#141). Order affects only queue order, not the fixed point.

There is no `chunk.getLights()` in 26.2; `findBlockLightSources` on the
`LightChunk` interface is the only source-discovery API.

### Occlusion/opacity between two adjacent states for block light — summary
`newLevel = level − max(1, dampening(dest))`, propagate iff
`newLevel > storedLevel(dest)` AND NOT
`faceShapeOccludes(faceShape(source, dir), faceShape(dest, dir⁻¹))`
where `faceShape(s,d)` is empty unless `s.canOcclude() && s.useShapeForLightOcclusion()`.

## 9. Storage contract (LayerLightSectionStorage — just enough for B2/Q4)

- `sectionStates: Long2ByteMap` (default 0 = EMPTY). SectionState byte:
  bit 5 (32) = HAS_DATA (section itself is non-air); bits 0–4 = neighborCount
  0..26 = number of the 26 surrounding sections that HAVE DATA
  (SectionState constants; `hasData(B,Z)` @#4/#12, `neighborCount` @#20–#28).
  `SectionType.type(b)`: 0→EMPTY, hasData→LIGHT_AND_DATA, else→LIGHT_ONLY.
- `updateSectionStatus(sectionPos, isEmpty)` (@#0–#161): flips HAS_DATA to
  `!isEmpty`; on any state change calls `putSectionState`; then adds
  `isEmpty ? -1 : +1` to `neighborCount` of ALL 26 neighbor section states
  (@#43–#158). `putSectionState` (@#0–#44): state≠0 → `sectionStates.put`;
  if previous was 0 → `initializeSection` which installs a **fresh all-zero
  DataLayer** into `updatingSectionData` (via `createDataLayer`: queued NBT
  layer if present, else `new DataLayer()` = defaultValue 0, lazy 2048-byte
  nibble array; DataLayer.getData @#7–#34) and marks section+neighbors
  affected. state==0 → remove → `toRemove` set, layer dropped in
  `markNewInconsistencies`.
  **Therefore: a section exists in storage (storingLightForSection == true)
  iff it is non-air itself OR any of its 26 neighbor sections (including
  diagonal, including across chunk borders) is non-air.** All-air sections
  adjacent to terrain get LIGHT_ONLY layers; everything else is a hard wall.
- Registration trigger during worldgen: `ThreadedLevelLightEngine.initializeLight`
  → `lambda$initializeLight$0`: for every section index of the chunk,
  if `!section.hasOnlyAir()` → `updateSectionStatus(SectionPos.of(chunkPos, y), false)`
  (TLLE lambda @#25–#52); plus `setLightEnabled(chunkPos, false)` at 08
  (lambda$initializeLight$2 passes the stage's `false` flag). Post-light block
  mutations re-enter via `ProtoChunk.setBlockState`: when `status.isOrAfter(INITIALIZE_LIGHT)`
  it calls `updateSectionStatus(pos, hasOnlyAir)` on emptiness transitions and,
  if `LightEngine.hasDifferentLightProperties(old, new)` (dampening, emission,
  useShapeForLightOcclusion differ — LightEngine @#7–#40), `lightEngine.checkBlock(pos)`
  (ProtoChunk.setBlockState @#109–#175).
- Double-buffering: `updatingSectionData` (written by the engine;
  `getStoredLevel`/`setStoredLevel` use `getDataLayer(sec, true)` — cached=updating,
  LayerLightSectionStorage.getStoredLevel @#7–#8, setStoredLevel @#34–#38 with
  copy-on-first-write per `changedSections` @#10–#26) vs `visibleSectionData`
  (published by `swapSectionMap` = clone of updating when `changedSections`
  non-empty, @#12–#26). **Reads via `getLightValue` use the visible map**:
  `BlockLightSectionStorage.getLightValue(long)` @#7–#8 uses
  `getDataLayer(sec, false)`, returns **0 if the layer is null** (@#15–#19),
  else `DataLayer.get(x&15, y&15, z&15)` with nibble index `y<<8|z<<4|x`
  (DataLayer.getIndex @#0–#9). The dump harness
  (`getLayerListener(BLOCK).getLightValue`) therefore sees post-swap state only.
- `setStoredLevel` also records the block's section and all neighbor sections
  containing any of the 27 around the block into `sectionsAffectedByLightUpdates`
  (`SectionPos.aroundAndAtBlockPos`, @#70–#85) — bookkeeping for
  `onLightUpdate` notifications; no effect on values.
- `DataLayer.set` masks the value: `(value & 15) << (4*nibble)` (DataLayer.set
  @#28–#31) — levels are physically 0..15.

## 10. Invariant: unique fixed point, order independence

Define, for the block layer, over the finite domain D = all positions whose
section is stored (§9):

```
L(p) = clamp_0..15( max( E(p), max_{d in 6 dirs, q=p+d in D, !occ(q,p,d)} L(q) − op(p) ) )
E(p) = lightEnabled(column(p)) ? lightEmission(state(p)) : 0
op(p) = max(1, lightDampening(state(p)))
occ(q,p,d) = faceShapeOccludes(faceShape(state(q), d), faceShape(state(p), d⁻¹)), d = q→p
```

F is monotone on the finite lattice ({0..15}^D, ≤ pointwise), so it has a
unique least fixed point (Knaster–Tarski), computable by chaotic iteration
from ⊥ = all-zeros in any order that eventually processes every violated
constraint.

**Why the vanilla increase phase computes exactly lfp(F):**
- Every write raises a value (`newLevel > nLevel` in propagateIncrease @#134;
  emission self-write only when `stored < from` @#54–#58) → monotone ascent, so
  the result is ≥ every intermediate and ≤ lfp (each write is justified by an
  F-constraint: emission term or neighbor term).
- Quiescence ⇒ no violated constraint: whenever a node's value is set to v>1,
  an entry with fromLevel=v is enqueued covering all directions except the one
  it came from (which cannot be violated: the parent has level ≥ v+op ≥ v−op).
  A dequeued entry with `stored > from` is dropped, but that node was
  necessarily raised by another write which enqueued its own fresh entry, so
  coverage is preserved. `newLevel == 1` needs no entry: its contribution to
  any neighbor is ≤ 1 − op ≤ 0. Hence at empty-queue, L = F(L), and by the
  ascent bound L = lfp(F). **Final state is independent of queue order, of
  `LongOpenHashSet` hash order in step 1 of runLightUpdates, and of duplicate
  entries.**
- Decrease-then-increase (block changes): the decrease phase clears a superset
  of positions whose value could depend on removed light (any node reachable by
  strictly-descending chains, cleared regardless of opacity/occlusion), seeds
  every boundary survivor back toward the cleared region
  (`increaseOnlyOneDirection`) and re-seeds every emitter it clears; the
  increase phase then rebuilds lfp(F) for the new block state. Because
  `runLightUpdates` drains decreases FULLY before increases (§3), no increase
  can be consumed while stale higher values remain in its support.

**Places where order COULD leak, checked:**
- `withLevel` masks to 4 bits — emission is registry-bounded 0..15, level only
  decreases in propagation, so no wraparound; clamping never activates.
- Stale-entry drop `stored == from` (@#79) — drops are safe (see argument
  above); re-entrant `checkBlock` between stages goes through the hash set +
  full decrease/pull-in, so it recomputes from scratch rather than trusting
  stale entries.
- FROM_EMPTY_SHAPE substitutes AIR for the source state; since
  `getOcclusionShape` returns `Shapes.empty()` for ANY empty-shape state, the
  substitution is exactly equivalent as long as the block at `pos` has not
  changed between enqueue and dequeue — within one `runLightUpdates` during
  worldgen there are no concurrent block edits, so no leak.
- **The one real order dependence is exposure, not algorithm:** values are a
  function of the SET of columns enabled + sources propagated so far (each
  `propagateLightSources` + `runLightUpdates` is a monotone step whose result
  is lfp for the enlarged seed set — set-dependent, order-independent), plus
  any `checkBlock` decreases from neighbor-chunk features placed after this
  chunk's LIGHT stage (ProtoChunk.setBlockState hook, §9). A dump taken inside
  chunk C's own stage-09 continuation therefore depends on WHICH other chunks
  have already run 09/features — matching anchors (3) and (4): 08 dumps
  (registration only, zero block light, no sources) are order-independent; 09
  dumps are not, and torn seqBegin/seqEnd snapshots in the 4-thread bundle are
  expected.

**Consistency with the empirical anchors:**
1. 08 `light_block` all zeros: at 08 only `updateSectionStatus(...)` +
   `setLightEnabled(chunkPos, false)` run; `columnsWithSources` empty, no
   `propagateLightSources`, no increase seeds → every stored layer is the
   all-zero fresh DataLayer; unstored sections read 0 via the null-layer path.
   ✔ (bytecode: TLLE lambdas §9; BlockLightSectionStorage.getLightValue @#15).
2. The 0x00/0xff sky pattern and the uniform y=112 boundary are a
   SkyLightSectionStorage/B3 matter, but the LIGHT_ONLY registration rule
   derived here (non-air OR 26-neighborhood of non-air, cross-chunk) is what
   makes the topmost *stored* section index uniform (max data section 5 in the
   3×3 region → LIGHT_ONLY through section 6 → "above storage" starts at
   section 7 = y 112) even while per-chunk surface sections vary 4..5. ✔
3./4. See order-dependence paragraph above. ✔

## 11. C-ready spec — batch block-light solver (provably same fixed point)

For worldgen stage 09 we never need the decrease machinery if we solve from
zero for the current snapshot (set of enabled chunks + current blocks). The
incremental vanilla process is a sequence of monotone steps each ending at the
lfp for its seed set; re-solving from ⊥ for the same seed set yields the
identical field.

Inputs for the region of interest:
- `blocks[]`: block states readable for every chunk that vanilla's
  `getChunkForLighting` would return; positions in chunks NOT retrievable act
  as `minecraft:bedrock` default state (LightEngine.getState @#26–#35:
  null chunk → BEDROCK) — dampening 15, emission 0, isEmptyShape TRUE (bedrock
  useShapeForLightOcclusion=false), i.e. never receives (15−15=0) and never
  occludes by shape.
- `stored(section) : bool` — replicate §9 exactly: for every chunk that has
  reached 08 by the snapshot time, mark `hasData` for each non-air section;
  `stored = hasData(sec) || any of 26 neighbors hasData`. Apply post-08
  `setBlockState` emptiness transitions in manifest order if features ran.
- `enabled(chunk) : bool` — chunks whose 09 `propagateLightSources` has run by
  the snapshot (manifest order.snapshots).

```c
// nibble grid L, one 16^3 layer per stored section, init 0.
// per-block tables: emission[state] (0..15), damp[state] (0..15),
// empty_shape[state] = !(canOcclude && useShapeForLightOcclusion),
// face_shape[state][dir] (only meaningful when !empty_shape).

static inline int opacity(state s){ return max(1, damp[s]); }
static inline bool occludes(state from, state to, dir d){
    // faceShapeOccludes(getOcclusionShape(from,d), getOcclusionShape(to,opp(d)))
    shape a = empty_shape[from] ? EMPTY : face_shape[from][d];
    shape b = empty_shape[to]   ? EMPTY : face_shape[to][opp(d)];
    if (a == FULL_BLOCK || b == FULL_BLOCK) return true;
    if (is_empty(a) && is_empty(b)) return false;
    return union_covers_full_face(a, b);   // Shapes.faceShapeOccludes tail
}

void solve_block_light(void){
    fifo_init(&q);                        // entries: (packed pos, level) — dir mask optional, see note
    // 1. seed: every emitting block in every ENABLED chunk (raw emission)
    for (chunk c : enabled_chunks)
        for (section ascending, y 0..15, z 0..15, x 0..15)     // vanilla scan order; any order OK
            if (emission[state(p)] > 0) push(&q, p, emission[state(p)], DIRS_ALL, empty_shape[state(p)]);
    // 2. relax to fixed point
    while (!fifo_empty(&q)) {
        (p, from, mask, from_empty) = pop(&q);
        int lvl = L(p);                                   // p is guaranteed stored
        if (lvl < from && is_emission_entry) { L(p) = from; lvl = from; }   // self-write
        if (lvl != from) continue;                        // stale
        state fs = from_empty ? AIR : state(p);
        for (dir d : {DOWN,UP,NORTH,SOUTH,WEST,EAST}) {
            if (!(mask & bit(d))) continue;
            pos n = p + d;
            if (!stored(section(n))) continue;            // hard wall: no write, no queue
            int nl = L(n);
            if (lvl - 1 <= nl) continue;
            state ns = state(n);                          // BEDROCK if chunk unavailable
            int newl = lvl - opacity(ns);
            if (newl <= nl) continue;
            if (occludes(fs, ns, d)) continue;
            L(n) = newl;
            if (newl > 1) push(&q, n, newl, DIRS_ALL & ~bit(opp(d)), empty_shape[ns]);
        }
    }
}
```

Correctness: identical monotone operator as §10; FIFO vs any other work-list
discipline is immaterial; the direction mask and the `newl > 1` cutoff are
pure work-savings (a masked-out back edge can never be a violated constraint).
A C implementation may even drop the direction masks and stale check and use a
plain "push neighbor when raised" worklist — same lfp — but keeping vanilla's
exact rules costs nothing and keeps entry-count parity if we ever need to
compare `runLightUpdates` return values.

Cross-chunk boundary rules (restating §5/§9 as normative):
- Into a stored section of another chunk: propagate normally; block states
  read from that chunk (worldgen guarantees availability; else BEDROCK rule).
- Into a section with NO storage (EMPTY state, e.g. all-air section with an
  all-air 26-neighborhood, or any section of a chunk that never ran 08):
  skipped entirely — no value, and reads report 0. Light does NOT tunnel
  through unstored sections even if re-entering stored space beyond.
- LIGHT_ONLY vs LIGHT_AND_DATA is irrelevant to propagation — both have
  layers; the distinction is only debug/serialization.
- Emission gating: seeds come only from enabled chunks; `checkNode`-style
  recomputations additionally gate emission by `lightOnInSection` — in a batch
  solve these coincide (emit iff chunk enabled).

To reproduce a specific 09 golden dump: run the solver with `enabled` = the
set of chunks whose 09 stage completed at/before the dump's
order-snapshot window, with blocks = post-features state of every chunk at
that instant (features of later manifest positions excluded). If a snapshot is
torn (seqBegin != seqEnd), both endpoint sets must be tried (or the lane-A
forensics consulted) — the algorithm itself contributes no other freedom.

## 12. Loose ends / handoffs

- `SkyLightEngine` overrides checkNode/propagate*/setLightEnabled/
  propagateLightSources with column-source logic (`REMOVE_TOP_SKY_SOURCE_ENTRY`
  etc.) — Lane B3. Its propagateIncrease reuses the same `getOpacity` +
  `shapeOccludes` calls (SkyLightEngine @#133, @#191).
- `LevelLightEngine`/`ThreadedLevelLightEngine` scheduling and the
  `queuedSections`/`retainData` NBT path (markNewInconsistencies @#8–#306) are
  not needed for worldgen-from-scratch: `queuedSections` stays empty, so
  `createDataLayer` always returns a fresh zero layer.
- Emission/dampening/useShapeForLightOcclusion/canOcclude/faceOcclusionShape
  per-state tables must come from our existing block-state registry; the
  `maybeHas` palette prefilter is a pure optimization (predicate re-checked
  per block) and need not be replicated.
