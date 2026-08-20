# R3 — SkyLightEngine + section storage + ChunkSkyLightSources (Task 10, lane B3)

Source of truth: bytecode of `server-26.2.jar` (unobfuscated). All classes in
`net.minecraft.world.level.lighting` unless noted. All bytecode landmarks cite
`method @ offset` in that class.

---

## 1. LayerLightSectionStorage — section registration & gates

### 1.1 SectionState byte encoding (`LayerLightSectionStorage$SectionState`)

State is one **byte per section** in `Long2ByteMap sectionStates`
(default return value 0 = EMPTY, set in ctor @119 `defaultReturnValue(0)`).

```
bit 5 (0x20)  HAS_DATA_BIT      — this section itself is non-empty (has blocks)
bits 0-4 (0x1f) NEIGHBOR_COUNT  — # of non-empty sections among its 26 neighbors (0..26)
```
- `hasData(b, flag)` = `flag ? b|32 : b&~32` (SectionState.hasData @0-16).
- `neighborCount(b, n)` = `(b&~31) | (n&31)`, throws if n∉[0,26] (@0-30).
- `type(b)`: `b==0 → EMPTY`; `hasData(b) → LIGHT_AND_DATA`; else `LIGHT_ONLY`
  (SectionState.type @0-22). So **LIGHT_ONLY = empty section adjacent (26-neighborhood)
  to ≥1 non-empty section**; LIGHT_AND_DATA = non-empty section itself.

### 1.2 `updateSectionStatus(long sectionPos, boolean isEmpty)` (@0-161)

```
b0 = sectionStates.get(pos)
b1 = SectionState.hasData(b0, !isEmpty)
if (b0 == b1) return                       // no transition → NO neighbor updates
putSectionState(pos, b1)
delta = isEmpty ? -1 : +1
for dx in -1..1: for dy in -1..1: for dz in -1..1:      // loop vars 7,8,9 → offset(pos,dx,dy,dz)
    if (dx|dy|dz)==0 continue                            // skip center @81-96
    nb = sectionStates.get(neighbor)
    putSectionState(neighbor, neighborCount(nb, neighborCount(nb)+delta))   // @124-140
```
The adjacency neighborhood is exactly the **3×3×3 cube minus center = 26 sections**
(±1 in x, y AND z — this is what leaks registration across chunk borders and one
section above the top non-empty section).

### 1.3 DataLayer allocation — `putSectionState` / `initializeSection` / `removeSection`

`putSectionState(pos, b)` (@0-44): if `b!=0` → `map.put`; **if the previous value was 0**
(put returns 0) → `initializeSection(pos)`. If `b==0` → `map.remove`; if previous
was ≠0 → `removeSection(pos)`.

`initializeSection` (@0-52): if `toRemove.remove(pos)` fails (not a pending removal):
`updatingSectionData.setLayer(pos, createDataLayer(pos))`, `changedSections.add(pos)`,
`onNodeAdded(pos)`, `markSectionAndNeighborsAsAffected(pos)`, `hasInconsistencies=true`.

⇒ **A DataLayer is allocated for every section whose state byte becomes non-zero** —
both LIGHT_AND_DATA and LIGHT_ONLY. `removeSection` only queues into `toRemove`
(lazy; processed in `markNewInconsistencies`).

Base `createDataLayer` (@0-27): returns `queuedSections.get(pos)` if present, else
`new DataLayer()` (all-zero, `data==null, defaultValue=0`). Sky overrides it (§2.4).

### 1.4 `retainData(zeroNode, bool)` (@0-29)

Adds/removes the **column** key (`SectionPos.getZeroNode`) in
`columnsToRetainQueuedDataFor`. Effect only inside `markNewInconsistencies`
(@60-125): when a section in `toRemove` is dropped, its queued/current DataLayer is
re-queued instead of discarded iff the column is retained. Worldgen: `initializeLight`
POST calls `retainData(pos,false)` — irrelevant for fresh gen (nothing queued).

### 1.5 `setLightEnabled(zeroNode, bool)` storage effect (@0-29)

Pure set-membership: adds/removes `zeroNode` in `LongSet columnsWithSources`.
Gates:
- `lightOnInSection(sectionPos)` = `columnsWithSources.contains(getZeroNode(sectionPos))` (@0-15).
- `lightOnInColumn(zeroNode)` = direct contains.
- `storingLightForSection(sectionPos)` = `getDataLayer(pos, updating=true) != null`
  (@0-14) — i.e. **section registered** (has a layer in `updatingSectionData`),
  independent of enabled state.

### 1.6 Updating vs visible maps

- `updatingSectionData` (M) mutated by the light thread; `visibleSectionData`
  is a `volatile` snapshot replaced in `swapSectionMap()` (@12-26: only when
  `changedSections` non-empty; `copy()` + `disableCache()`).
- Copy-on-write: `getDataLayerToWrite` (@0-50) copies the layer the first time a
  section enters `changedSections`; `setStoredLevel` (@0-88) does the same then
  `DataLayer.set(relX,relY,relZ,v)` and marks `sectionsAffectedByLightUpdates`
  (±1-block section neighborhood, client-notification only).
- `LightEngine.runLightUpdates()` (LightEngine @0-84): drain `blockNodesToCheck`
  → `checkNode` each; `propagateDecreases()`; `propagateIncreases()`;
  `storage.markNewInconsistencies(this)`; `storage.swapSectionMap()`.

### 1.7 Read path used by the dump harness

`LevelLightEngine.getLayerListener(SKY)` returns the `SkyLightEngine` itself
(LevelLightEngine.getLayerListener @23-38). `LightEngine.getLightValue(BlockPos)`
= `storage.getLightValue(pos.asLong())` (LightEngine @0-11) →
`SkyLightSectionStorage.getLightValue(long)` = `getLightValue(long, updating=false)`
(@0-6). **Dumps read the visible snapshot** (last `swapSectionMap`).

---

## 2. SkyLightSectionStorage + SkyDataLayerStorageMap

### 2.1 topSections semantics

`SkyDataLayerStorageMap` adds to the base layer map:
- `Long2IntOpenHashMap topSections` — **key = `SectionPos.getZeroNode(sectionPos)`**
  (= section long with the 20 y-bits zeroed; `getZeroNode(long)` = `& -1048576L`,
  SectionPos @420-425), i.e. a per-(chunk-column) key.
  **value = (highest section y that has a stored DataLayer in that column) + 1.**
- `int currentLowestY` — global minimum stored section y; starts
  `Integer.MAX_VALUE` (SkyLightSectionStorage ctor @23 `ldc 2147483647`), and is the
  map's `defaultReturnValue` (kept in sync when lowered).

Maintenance (`onNodeAdded` @0-103): `y = SectionPos.y(pos)`; if `currentLowestY > y`
→ `currentLowestY = y` and `topSections.defaultReturnValue(currentLowestY)`;
then if `topSections.get(zeroNode) < y+1` → `put(zeroNode, y+1)`.

`onNodeRemoved` (@0-115): if the removed section was the top (`get(zeroNode)==y+1`):
walk down while `!storingLightForSection && hasLightDataAtOrBelow(y)`; if a stored
section found → `put(zeroNode, y'+1)` else `remove(zeroNode)`.

Helpers: `hasLightDataAtOrBelow(sy)` = `sy >= currentLowestY` (@0-19);
`isAboveData(sectionPos)` = `top==currentLowestY || SectionPos.y >= top` (@0-50);
`getTopSectionY(zeroNode)` = `topSections.get` (@0-14);
`getBottomSectionY()` = `currentLowestY`.

### 2.2 `getLightValue(long blockPos, boolean updating)` — the exact read rule (@0-173)

```
sec  = blockToSection(blockPos); y = SectionPos.y(sec)
map  = updating ? updatingSectionData : visibleSectionData
top  = map.topSections.get(getZeroNode(sec))       // default = map.currentLowestY
if (top == map.currentLowestY || y >= top):        // "no data in column" OR "above data"  @51-65
    if (updating && !lightOnInSection(sec)) return 0        // @68-82
    return 15                                                // @83-85
layer = map.getLayer(sec)
if (layer == null):                                 // gap section below stored data @96-146
    flat = blockPos & -16L                          // BlockPos.getFlatIndex: zero low 4 y-bits @BlockPos 249
    do { y++; if (y >= top) return 15;              // @111-121
         sec = offset(sec, UP); layer = map.getLayer(sec) } while (layer == null)
    return layer.get(relX, 0, relZ)                 // bottom slice of first stored layer above
return layer.get(relX, relY, relZ)
```

Key consequences:
- **Non-updating (visible/dump) reads return 15 for everything at/above `topSections`
  — regardless of light-enabled state.** The `lightOnInSection` guard only applies
  to `updating=true` reads (used internally during propagation).
- A column with **no stored sections at all** (topSections miss → default ==
  currentLowestY) reads 15 at **every** y (visible read).
- A **gap** (unregistered section between stored ones) reads the **bottom (relY=0)
  slice of the nearest stored layer above**; if none below `top`, 15.
- A chunk **not yet light-enabled but registered** (state at 08) reads its stored
  all-zero layers → 0 below `topSections`, 15 at/above. ✔ anchor (2) shape.

### 2.3 Empirical anchor: uniform f-boundary at world y = 112 at 08

- Overworld light sections span **one extra section below and above the world**;
  a normal chunk's non-empty sections are contiguous `-4 .. T` where
  `T = (max top non-air y) >> 4` ∈ {4,5} for the grid (WORLD_SURFACE max 79..92 ⇒
  top non-air y 78..91).
- `initializeLight`'s PRE task registers every non-empty section with
  `updateSectionStatus(pos, false)` (ThreadedLevelLightEngine.lambda$initializeLight$0
  @0-61); §1.2's ±1 marking registers sections `-5 .. T+1` in the chunk's own column
  **and in all 8 neighbor chunk columns**.
- Therefore `topSections[column] = max(T_nbhd)+2`, where `T_nbhd` is the max
  top-non-air-section over the 3×3 chunk neighborhood (self + 8 neighbors whose
  registrations are visible at read time).
- Grid: some chunks have T=5 ⇒ their registration marks section 6 in their own and
  neighbors' columns ⇒ `topSections = 7` ⇒ first always-15 block y = 16·7 = **112**. ✔
- For a chunk whose own T=4: its own registration alone would give topSections=6
  (boundary 96). The observed 112 ⇒ **every T=4 grid chunk had a T=5 chunk in its
  3×3 neighborhood whose 08 PRE registration was already in the visible snapshot at
  dump time** (both bundles). This is not forced by the pyramid (see §5.2) but is
  strongly favored by batching: `runUpdate` executes **all PRE tasks in the window
  first, then `runLightUpdates` (swap), then POSTs** (ThreadedLevelLightEngine.runUpdate
  @26-158), so any neighbor `initializeLight` queued in the same batch as the dumped
  chunk's POST is visible even if enqueued later. Implementer should verify against
  golden heightmaps that every T=4 grid chunk has a T=5 3×3-neighbor (incl. off-grid
  ring chunks — they also reach 08).
- Below 112 everything reads the stored layers, which are all-zero at 08 (§2.4 + §5). ✔
- Anchor (1) (block light all zeros at 08): `BlockLightSectionStorage.getLightValue`
  has **no** above-top rule — `layer==null → 0`, else nibble (BlockLightSectionStorage
  .getLightValue @13-19); all layers zero at 08 ⇒ all zeros. ✔

### 2.4 Sky `createDataLayer` override (@0-129) — layer initialization content

```
if queuedSections has one → use it
top = topSections.get(zeroNode)                    // BEFORE onNodeAdded of this node
if (top == currentLowestY || sectionY >= top):     // creating above existing data
    return lightOnInSection(pos) ? new DataLayer(15)   // uniform-15 (data==null, default 15)
                                 : new DataLayer()      // uniform-0
else:                                              // creating BELOW existing data
    walk sec=pos+UP upward until getDataLayer(updating) != null
    return repeatFirstLayer(found)                 // @124-126
```
`repeatFirstLayer` (@0-59): if `isDefinitelyHomogenous` (data==null) → `copy()`;
else new 2048-byte array where the found layer's **bottom slice (first 128 bytes =
relY=0 plane)** is replicated into all 16 slices. (DataLayer nibble layout:
`index = relY<<8 | relZ<<4 | relX`, byte `index>>1`, low nibble = even index —
DataLayer.getIndex @0-10, get @12-35.)

At 08 (nothing enabled, all layers zero) every allocation path yields zero layers. ✔

---

## 3. SkyLightEngine

Queue entry encoding (`LightEngine$QueueEntry`): 64-bit —
bits 0-3 `fromLevel`; bits 4-9 direction set, bit for dir d = `1 << (d.ordinal()+4)`
with Direction ordinals DOWN=0, UP=1, NORTH=2, SOUTH=3, WEST=4, EAST=5;
bit 10 (1024) FLAG_FROM_EMPTY_SHAPE; bit 11 (2048) FLAG_INCREASE_FROM_EMISSION.
Constructors: `decreaseAllDirections(l)` = 1008|l; `decreaseSkipOneDirection(l,d)`;
`increaseSkipOneDirection(l,fromEmpty,d)`; `increaseOnlyOneDirection(l,fromEmpty,d)`;
`increaseSkySourceInDirections(down,north,south,west,east)` = level 15 + chosen dir
bits (QueueEntry @94-131). Sky statics (SkyLightEngine `static{}` @0-31):
`REMOVE_TOP_SKY_SOURCE_ENTRY = decreaseAllDirections(15)`,
`REMOVE_SKY_SOURCE_ENTRY = decreaseSkipOneDirection(15, UP)`,
`ADD_SKY_SOURCE_ENTRY = increaseSkipOneDirection(15, false, UP)`.
`PULL_LIGHT_IN_ENTRY = decreaseAllDirections(1)` (LightEngine static @0-4).
Queues: two `LongArrayFIFOQueue`s, each entry = 2 longs (pos, entry); FIFO order.
`PROPAGATION_DIRECTIONS = Direction.values()` = D,U,N,S,W,E.

### 3.1 `propagateLightSources(ChunkPos)` (@0-618) — exact seeding

```
zeroNode = getZeroNode(cx, cz)
storage.setLightEnabled(zeroNode, true)            // FIRST thing @12-21 (storage-level; columnsWithSources.add)
S   = sources(cx,cz)  or emptyChunkSources         // ChunkSkyLightSources via getChunkForLighting
SN  = sources(cx,cz-1) or empty; SS = (cx,cz+1); SW = (cx-1,cz); SE = (cx+1,cz)
top = storage.getTopSectionY(zeroNode); bottom = storage.getBottomSectionY()
for sy = top-1 down to bottom:                     // @195-615
    layer = storage.getDataLayerToWrite(asLong(cx,sy,cz)); if null: continue  // @237-242
    y0 = 16*sy; y15 = y0+15; anyLower = false
    for dz = 0..15: for dx = 0..15:                // outer var20=dz, inner var21=dx
        low  = S.getLowestSourceY(dx,dz)
        if (low > y15) continue                    // column floor above this section @293-300
        lowN = dz==0  ? SN.get(dx,15) : S.get(dx,dz-1)
        lowS = dz==15 ? SS.get(dx,0)  : S.get(dx,dz+1)
        lowW = dx==0  ? SW.get(15,dz) : S.get(dx-1,dz)
        lowE = dx==15 ? SE.get(0,dz)  : S.get(dx+1,dz)
        maxNb = max(lowN,lowS,lowW,lowE)
        for y = y15 down to max(y0, low):          // @448-579
            layer.set(dx, y&15, dz, 15)            // direct fill, level 15 @460-473
            if (y == low || y < maxNb):            // @476-487
                enqueueIncrease(BlockPos(16cx+dx, y, 16cz+dz),
                    increaseSkySourceInDirections(y==low, y<lowN, y<lowS, y<lowW, y<lowE))
        if (low < y0) anyLower = true              // @582-590
    if (!anyLower) break                           // stop descending @604-609
```
- Fill is **direct** (`DataLayer.set` on the write-copy), not via BFS: every block of
  every column from its `lowestSourceY` up to the top stored section is set to 15.
- Seeds: only at `y == lowestSourceY` (with DOWN bit) and/or `y <` some 4-neighbor
  column's lowestSourceY (with the corresponding horizontal bit(s)). Level always 15.
- `getLowestSourceY` returns `NEGATIVE_INFINITY` for fully-open columns (§4), so
  `y==low` never fires there and `low<y0` keeps the descent going to `bottom`.
- Timing: `setLightEnabled(true)` happens **before** seeding, inside the same
  PRE task (scheduled by `ThreadedLevelLightEngine.propagateLightSources` /
  `lightChunk` PRE — lambda$lightChunk$0 @0-9 runs it only when `!isLighted`).
  Propagation itself happens in the following `runLightUpdates` on the light thread.

Note: the *engine-level* `SkyLightEngine.setLightEnabled(ChunkPos,true)` (@0-158)
additionally bulk-`fill(15)`s all still-empty stored layers above
`blockToSectionCoord(getHighestLowestSourceY()-1)+1`. That path is only invoked with
`true` for **already-lit chunk loads** (`initializeLight(chunk, isLighted=true)` POST);
during fresh worldgen `initializeLight` passes `isLighted=false`
(ChunkStatusTasks.isLighted = persistedStatus≥LIGHT && isLightCorrect, @11-25) and
`propagateLightSources` calls the **storage-level** enable directly — the bulk-fill
shortcut never runs during generation.

### 3.2 `checkNode(long)` — sky-specific (@0-175); worldgen relevance

```
sec = blockToSection(pos)
low = lightOnInSection(sec) ? getLowestSourceY(x, z, INT_MAX) : INT_MAX
if (low != INT_MAX) updateSourcesInColumn(x, z, low)     // resync fills/seeds in the column
if (!storingLightForSection(sec)) return
if (y >= low):   // position is a sky source
    enqueueDecrease(pos, REMOVE_SKY_SOURCE_ENTRY); enqueueIncrease(pos, ADD_SKY_SOURCE_ENTRY)
else:
    lvl = getStoredLevel(pos)
    if (lvl > 0): setStoredLevel(pos,0); enqueueDecrease(pos, decreaseAllDirections(lvl))
    else:         enqueueDecrease(pos, PULL_LIGHT_IN_ENTRY)   // re-pull from lit neighbors
```
`updateSourcesInColumn` (@0-33) = `removeSourcesBelow(x,z,low,16*bottomSectionY)`
+ `addSourcesAbove(...)`: remove = walk stored sections downward from `low-1`,
clearing 15s and enqueueing `REMOVE_TOP_SKY_SOURCE_ENTRY` (all-dirs) for the block
just below the old floor, `REMOVE_SKY_SOURCE_ENTRY` (skip UP) for the rest (@139-159);
add = fill 15s from `max(sectionBottom, low)` up to `isAboveData`, enqueueing
`ADD_SKY_SOURCE_ENTRY`-style seeds where `y >= max(4-neighbor lows)` or `y == low`
(@190-209). `checkNode` runs only for `checkBlock`ed positions — during worldgen that
means **post-08 block writes** via `ProtoChunk.setBlockState` (§5.3), not the 08/09
stage tasks themselves.

### 3.3 `propagateIncrease(pos, entry, level)` (@0-257)

```
fromState = null (lazy); emptyBelow = countEmptySectionsBelowIfAtBorder(pos)
for dir in [D,U,N,S,W,E]:
    if !shouldPropagateInDirection(entry, dir): continue
    n = pos + dir
    if !storingLightForSection(section(n)): continue            // registration gate @57-75
    cur = getStoredLevel(n); if (level-1 <= cur) continue
    st  = getState(n)                                            // missing chunk → BEDROCK state (LightEngine.getState @29-35)
    nl  = level - max(1, st.getLightDampening())                 // getOpacity, MIN_OPACITY=1
    if (nl <= cur) continue
    if (fromState==null) fromState = isFromEmptyShape(entry) ? AIR : getState(pos)
    if (shapeOccludes(fromState, st, dir)) continue              // faceShapeOccludes of occlusion shapes
    setStoredLevel(n, nl)
    if (nl > 1) enqueueIncrease(n, increaseSkipOneDirection(nl, isEmptyShape(st), dir.opposite))
    propagateFromEmptySections(n, dir, nl, /*increase*/true, emptyBelow)
```
**There is no "downward 15 keeps 15" special case in the BFS** — since 1.20 the
non-attenuating vertical rule is realized entirely by the *source-column model*:
every block with direct sky access is a level-15 source via §3.1's direct fill /
ChunkSkyLightSources, so BFS attenuation is direction-uniform (`opacity ≥ 1`).
`isEmptyShape(state)` = `!canOcclude() || !useShapeForLightOcclusion()` (LightEngine
@146-157); `shapeOccludes(a,b,dir)` = `faceShapeOccludes(occlusionShape(a,dir),
occlusionShape(b,dir.opposite))` with empty-shape states short-circuiting to
`Shapes.empty()` (LightEngine.getOcclusionShape @0-18).

### 3.4 `propagateDecrease(pos, entry)` (@0-180)

Standard removal wave: for each allowed dir, if neighbor stored and level≠0:
if `nLevel <= fromLevel-1` → zero it, enqueueDecrease(`decreaseSkipOneDirection(nLevel,
dir.opposite)`), `propagateFromEmptySections(n, dir, nLevel, false, emptyBelow)`;
else `enqueueIncrease(n, increaseOnlyOneDirection(nLevel, false, dir.opposite))`
(re-flood frontier). Not exercised by fresh 08/09 generation (no removals), but
reachable post-08 via `checkBlock`.

### 3.5 Missing-section shortcuts ("currently lit above" gap handling)

`countEmptySectionsBelowIfAtBorder(pos)` (@0-148): returns >0 only when
`relY(pos)==0` **and** pos is on a horizontal section border (relX∈{0,15} or
relZ∈{0,15}); counts consecutive sections directly below that are
`!storingLightForSection` yet `hasLightDataAtOrBelow` (within the global stored range).

`propagateFromEmptySections(pos, dir, level, isIncrease, count)` (@0-223): if
`count>0` and the step crossed a **horizontal** section edge
(`crossedSectionEdge`: NORTH→relZ==15, SOUTH→relZ==0, WEST→relX==15, EAST→relX==0,
UP/DOWN→false; verified via `SkyLightEngine$1.$SwitchMap` NORTH=1,SOUTH=2,WEST=3,EAST=4):
for each of the `count` sections below the crossing that **is** stored, write
`level` (increase) or 0 (decrease) into the whole 16-block vertical run at (x,z)
and enqueue `increaseSkipOneDirection(level, fromEmpty=true, dir.opposite)` /
`decreaseSkipOneDirection`. Rationale: an unregistered gap section's value is
*implicitly* the bottom slice of the section above (§2.2), so a horizontal spill out
of that bottom slice must be materialized down the whole gap range in the
destination column. **Inert during grid generation**: registration per chunk column
is contiguous (`-5..T+1`), so `count==0` always; implement for general parity,
assert-unreachable in the 9-grid test.

---

## 4. ChunkSkyLightSources

### 4.1 Representation

Per chunk: `SimpleBitStorage heightmap` with **256 entries** (16×16 columns,
`index(x,z) = x + 16*z`, @index 0-6) and `bits = ceillog2(maxY+2 - (minY-1) + 1)`
(ctor @38-58; overworld: ceillog2(386) = 9 bits). Field `minY = level.getMinY()-1`
(= −65). Stored value = `lowestSourceY - minY`; decode `get(i) + minY` (@get 0-15).
Sentinel: stored value == field `minY` ⇒ `extendSourcesBelowWorld` returns
`NEGATIVE_INFINITY = Integer.MIN_VALUE` (@0-12) — "column open to below the world".
`getLowestSourceY(x,z)` = decode + sentinel-extend (@0-15).
`getHighestLowestSourceY()` = max raw entry + minY, sentinel-extended (@0-52).

Semantics: **lowestSourceY = the lowest world y such that skylight can pass from
above the chunk straight down to y without hitting an occluding pair** — i.e. the
y of the block sitting directly on top of the first occluder; every block at
`y >= lowestSourceY` in that column is a direct level-15 sky source.

### 4.2 The occlusion predicate — `isEdgeOccluded(above, below)` (@0-30)

```
if (below.getLightDampening() != 0) return true
return Shapes.faceShapeOccludes(getOcclusionShape(above, DOWN),
                                getOcclusionShape(below, UP))
```
NOT `propagatesSkylightDown`: it is *any* light dampening (opacity ≥ 1) on the lower
block, OR the pair's facing occlusion shapes sealing the shared face (above's DOWN
face vs below's UP face; empty-shape states contribute `Shapes.empty()`). Water
(dampening 1) is therefore a floor; glass/leaves-with-0-dampening depend on shapes.

### 4.3 Initial fill — `ChunkAccess.initializeLightSources()` → `fillFrom(chunk)`

`ChunkStatusTasks.initializeLight` calls `chunk.initializeLightSources()`
**synchronously on the gen worker before scheduling the light tasks** (@7), which is
`skyLightSources.fillFrom(this)` (ChunkAccess @961-967).

`fillFrom` (@0-79): `i = chunk.getHighestFilledSectionIndex()`; if −1 → `fill(minY)`
(all sentinel). Else per column (z outer, x inner):
`set(index(x,z), max(findLowestSourceY(chunk, i, x, z), minY))`.

`findLowestSourceY(chunk, topIdx, x, z)` (@0-189): scan sections `topIdx..0`
downward; maintain `posAbove` starting at `16*(sectionY(topIdx)+1)` and
`stateAbove` starting AIR. Empty section (`hasOnlyAir`) → `stateAbove = AIR`,
`posAbove = sectionBottom(sy)` (skip wholesale). Non-empty → for relY 15..0:
`state = section.getBlockState(x, relY, z)`; if `isEdgeOccluded(stateAbove, state)`
→ **return posAbove.y**; else advance (`stateAbove=state`, posAbove walks down).
Fallthrough → field `minY` (sentinel).

### 4.4 Incremental update — `update(level, x, y, z)` (@0-142), used post-08

```
if (y+1 < cur) return false                       // strictly below current floor-1: no effect
// pair (y+1, y):
if updateEdge(idx, cur, above=(x,y+1,z), below=(x,y,z)): return true
// pair (y, y-1):
return updateEdge(idx, cur, above=(x,y,z), below=(x,y-1,z))

updateEdge(idx, cur, posA, stA, posB, stB):        // @0-55
    if isEdgeOccluded(stA, stB):
        if (posA.y > cur) { set(idx, posA.y); return true }     // floor raised
    else if (posA.y == cur):
        set(idx, findLowestSourceBelow(level, posB, stB)); return true  // floor removed → rescan down
    return false
```
`findLowestSourceBelow` (@0-92): walk down from posB with the same
`isEdgeOccluded(above, below)` pair test until occluded (return the above-y) or
world bottom (return sentinel). Called from `ProtoChunk.setBlockState` when
`status ≥ INITIALIZE_LIGHT` and `LightEngine.hasDifferentLightProperties(old,new)`
(different dampening/emission/useShapeForLightOcclusion), together with
`lightEngine.checkBlock(pos)` (ProtoChunk.setBlockState @109-175).

---

## 5. Cross-chunk gating & anchor reconciliation

### 5.1 Who is visible to the engine

`ServerChunkCache.getChunkForLighting(x,z)` (@577-595): returns the chunk iff its
holder exists and it has reached **`ChunkStatus.INITIALIZE_LIGHT.getParent()` =
FEATURES**. Used for block states (`getState`; missing chunk ⇒ BEDROCK default) and
`getSkyLightSources` (`getChunkSources`; missing ⇒ `emptyChunkSources`, whose
freshly-constructed 9-bit heightmap reads all-sentinel = NEG_INF, i.e. "fully open" —
so no horizontal seeds are generated toward missing chunks in §3.1).

### 5.2 Pyramid dependencies (GENERATION_PYRAMID, ChunkPyramid static)

- INITIALIZE_LIGHT step = `lambda$static$8` (BSM #21): **no neighbor requirement**,
  task = `ChunkStatusTasks.initializeLight`.
- LIGHT step = `lambda$static$9` (BSM #22): **`addRequirement(INITIALIZE_LIGHT, 1)`**
  — all chunks within radius 1 must have completed 08.
- FEATURES step = `lambda$static$7` (BSM #20): STRUCTURE_STARTS r=8, CARVERS r=1,
  `blockStateWriteRadius(1)`.

### 5.3 Does A's skylight spread into not-yet-enabled B? — YES, and reconciliation

Bytecode answer: propagation gates **only** on `storingLightForSection` (section
registered), never on the destination chunk's enabled bit (§3.3 @57-75). So when A
runs `propagateLightSources`, its boundary seeds write real values into B's
registered (LIGHT_ONLY/LIGHT_AND_DATA) DataLayers.

Reconciliation with anchors (1)/(2) (B dumped at 08 reads 0 below 112, both bundles,
order-independent):
1. **Adjacent A cannot be lit before B's 08 dump.** A's LIGHT requires B at
   INITIALIZE_LIGHT (radius-1 requirement, §5.2); the dump runs *inside* B's 08
   stage future continuation, so B's status only completes (and A's 09 only becomes
   schedulable) after the dump finished.
2. **Non-adjacent A cannot reach B.** All skylight ingress into a chunk comes from
   seeds at/inside A or boundary spill; BFS decays ≥1/block, so light from A
   penetrates ≤15 blocks past A's border — it can dirty the adjacent chunk only,
   never a chunk 2+ away. `propagateFromEmptySections` also only writes in the
   section column immediately across the crossing edge.
3. Post-08 `checkBlock`/`skyLightSources.update` hooks (§4.4) fire only for writes
   into a ≥08 chunk; with FEATURES write radius 1 and LIGHT's radius-1-08
   requirement, feature writes land in chunks < 08 during the pre-09 window of the
   writer's neighborhood, and PULL_LIGHT_IN re-pulls on an all-zero field are no-ops.
   (This same hook set is what makes **09** dumps features-order-dependent and
   "torn": adjacent chunks' 09 spill + concurrent manifest movement — anchors (3)/(4).)
4. The 09 order-dependence is exactly the boundary spill of point 1 once B has
   passed 08: whichever neighbors ran `propagateLightSources` before B's 09 dump
   have already written 15/14/... into B's border columns.

### 5.4 Threading model recap (for the C port's determinism argument)

All mutation happens on one `ConsecutiveExecutor` ("light thread"):
`runUpdate` = run ≤1000 queued tasks' **PRE** entries → `LevelLightEngine.
runLightUpdates()` (checkNodes, decreases, increases, swap) → run the same window's
**POST** entries and remove them (ThreadedLevelLightEngine.runUpdate @26-158; also
force-run when the queue reaches 1000, lambda$addTask$0 @15-31). `initializeLight`
= PRE(register sections) + POST(setLightEnabled(pos, isLighted=false→no-op remove);
retainData(pos,false)); its returned future completes on the POST. `lightChunk` =
PRE(`propagateLightSources` iff !isLighted) + POST(setLightCorrect(true), complete).

---

## 6. C-ready spec — batch skylight fixed point

Inputs: per-chunk blocks (final at that chunk's 08), set `R` of chunks that have run
08-registration, set `S ⊆ R` of chunks that have run 09-seeding ("enabled"), world
section range `[minSec-1, maxSec+1]`.

```c
// ---- storage ----
// sections: hash (scx,sy,scz) -> nibble[4096] layer   (registered set)
// top[cx][cz]: highest registered sy + 1; absent -> INT_MIN sentinel semantics
// enabled[cx][cz]: bool (columnsWithSources)
// srcY[chunk][z][x]: int lowestSourceY (INT_MIN = open column)

// ---- phase A: registration (per chunk c in R, any order — commutative) ----
for each section sy of c with any non-air block:            // hasOnlyAir == false
    for (dx,dy,dz) in 3x3x3:                                 // center sets HAS_DATA; all raise refcount
        key = (c.x+dx, sy+dy, c.z+dz)
        if (first registration of key): alloc zero layer; top[col(key)] = max(top, y(key)+1)
// srcY: fillFrom(c) per §4.3 (scan from highest filled section; isEdgeOccluded pairs)

// ---- phase B: seeding (per chunk c in S, in vanilla completion order for 09-parity) ----
enabled[c] = true
seed per §3.1: for sy = top[c]-1 .. minSec-1 (global lowest registered):
    direct-fill 15s per column from max(sectionBottom, srcY) .. sectionTop (skip if srcY>secTop)
    enqueueIncrease at y==srcY (DOWN bit) and y<neighborSrcY (that horizontal bit), level 15
    stop descending when no column had srcY < sectionBottom
// neighbor srcY lookups use the 4 adjacent chunks' tables; chunk < FEATURES -> INT_MIN

// ---- phase C: BFS fixed point (single FIFO pass; increases only for fresh gen) ----
while increaseQueue not empty:
    (pos, entry) = dequeue
    if (getStoredLevel(pos) != entry.fromLevel) continue      // stale (emission flag n/a for sky)
    for dir in {D,U,N,S,W,E} where entry has dir bit:
        n = pos+dir
        if (!registered(section(n))) continue
        cur = level(n); if (entry.level-1 <= cur) continue
        nl = entry.level - max(1, dampening(state(n)));  if (nl <= cur) continue
        from = entry.fromEmptyShape ? AIR : state(pos)
        if (shapeOccludes(from, state(n), dir)) continue      // face occlusion shapes
        level(n) = nl
        if (nl > 1) enqueue(n, {nl, allDirs - dir.opposite, isEmptyShape(state(n))})
        // propagateFromEmptySections: no-op while registration is column-contiguous (assert)
    // state(x) for unloaded chunk == BEDROCK (dampening 15, full cube)

// ---- phase D: reads (dump semantics; use the post-fixed-point state == visible) ----
int sky_get(bx, by, bz):
    sy = by >> 4; col = (bx>>4, bz>>4)
    t = top_or_default(col)                                   // default = global currentLowestY
    if (t == default_sentinel || sy >= t) return 15           // ABOVE-DATA RULE (y >= 16*t ⇒ 15)
    L = layer(bx>>4, sy, bz>>4)
    while (L == NULL) { sy++; if (sy >= t) return 15; L = layer(...); by &= ~15; } // gap: bottom slice above
    return nibble(L, bx&15, by&15, bz&15)
```

Parity-critical points:
- Above-top reads are 15 **even for never-enabled chunks** (visible-read rule §2.2).
- `top` is per column and raised by **neighbor** registrations (±1 x/z/y) — source of
  the uniform y=112 at 08.
- Seeding fill is direct (no BFS attenuation vertically inside source columns);
  BFS is direction-uniform with opacity ≥ 1 — no downward-15 special case.
- Occlusion needs exactly two block properties + shapes: `getLightDampening`,
  `canOcclude && useShapeForLightOcclusion` (else empty shape), `getFaceOcclusionShape`
  + `Shapes.faceShapeOccludes` / `mergedFaceOccludes` (the latter only in
  `getLightDampeningInto`, not on the sky value path).
- For 08 parity: run phases A(+fillFrom) for all chunks that completed 08 before the
  dumped chunk's dump; no phase B/C — everything reads 0 below `16*top`, 15 at/above.
- For 09 parity of chunk X: phases B/C must have run for exactly the set of chunks
  whose `propagateLightSources` preceded X's dump in the bundle's order (order matters
  only within X's 8-neighborhood, distance ≥2 cannot reach X).

## Open questions / hand-offs

- The uniform-112 anchor for own-T=4 chunks proves neighbor 08 registrations were
  visible at dump time in both bundles, but 08 has no pyramid dependency — the exact
  visibility set for a C reproduction of *arbitrary* schedules is lane-A/B4 territory
  (light task batching, `getChunkQueueLevel` priorities). For the 9-grid goldens,
  "all chunks that completed 08 before the dump" matched both bundles.
- Verify from golden heightmaps that every T=4 grid chunk has a T=5 chunk in its 3×3
  neighborhood (including off-grid ring chunks that reach 08).
- `Shapes.faceShapeOccludes` / occlusion-shape bit-exactness is shared with the
  block-light lane (B2) — single C implementation.
