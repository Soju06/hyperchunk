# R-D — block_ticks / fluid_ticks lifecycle, save encoding, save order (Task 12)

Source of truth: `javap -p -c -constants -cp
tools/golden/libs/extracted/server-26.2.jar <fqcn>` (+ `-v` for bootstrap
methods). Empirical cross-checks against `golden/seed1234567890_r.0.0.mca`
(raw zlib payloads, own NBT parser, /tmp/taskd/dump_*.py). Serialization is
**codec-based** (`SerializableChunkData.saveTicks` → `CompoundTag.store(name,
Codec, value)` @0-26), not manual CompoundTag building.

## 1. Leaf tick scheduling during tree placement

Call chain (all confirmed by bytecode):

1. `TreeFeature.place` @182-208: after decorators,
   `BoundingBox.encapsulatingPositions(concat(roots, trunk, foliage,
   decorations))` → `lambda$place$4` (captures level, trunkPositions,
   decorationPositions, rootPositions).
2. `TreeFeature.lambda$place$4` @0-30: `shape = updateLeaves(level, box,
   trunkPositions, decorationPositions, rootPositions)` →
   `StructureTemplate.updateShapeAtEdge(level, 3, shape, box.minX, box.minY,
   box.minZ)`.
3. `TreeFeature.updateLeaves` @0-529: `BitSetDiscreteVoxelShape(xSpan, ySpan,
   zSpan)`; 7 distance sets; set0 = trunk ∪ decorations (`Sets.union` @57-63);
   seeds inside box filled into the shape @93-134; BFS: pop pos from set i
   @232-246, if i>0 rewrite `DISTANCE=i` via `setBlockKnownShape` (flag
   19 = `BLOCK_UPDATE_FLAGS`, no shape updates) @263-297; fill voxel
   @300-332; for all 6 `Direction.values()` neighbors inside box, not yet
   filled, with `LeavesBlock.getOptionalDistanceAt` present: add to set
   `min(dist, i+1)` @335-521.
4. `StructureTemplate.updateShapeAtEdge(LevelAccessor,int,DiscreteVoxelShape,
   int,int,int)` @0-38 → `shape.forAllFaces(lambda)`.
   `DiscreteVoxelShape.forAllFaces` @0-24 = three `forAllAxisFaces` passes
   (AxisCycle NONE, FORWARD, BACKWARD) — same face-enumeration already
   replicated by the Task-9 features pass.
5. `StructureTemplate.lambda$updateShapeAtEdge$0` @0-147: `pos = min+(x,y,z)`
   @0-16, `neighborPos = pos.relative(dir)` @17-25;
   `state1.updateShape(level, level, pos, dir, neighborPos, state2,
   level.getRandom())` @47-67, if changed `setBlock(pos, new, flags & -2)`
   @79-94 (3 & -2 = 2); then the mirror call on the neighbor with
   `dir.getOpposite()` and `newState1` @95-118, `setBlock` @130-141.
   The **ScheduledTickAccess passed is the LevelAccessor itself** (the
   WorldGenRegion).
6. `BlockBehaviour$BlockStateBase.updateShape` @0-22 → virtual
   `Block.updateShape`.
7. `LeavesBlock.updateShape` (regs: 1=state, 2=LevelReader,
   3=ScheduledTickAccess, 4=pos, 5=dir, 6=neighborPos, 7=neighborState,
   8=random):
   - @0-13: if `WATERLOGGED` → @16-29 `scheduleTick(pos, Fluids.WATER,
     WATER.getTickDelay(level))` (fluid tick, delay 5).
   - @34-41: `i = getDistanceAt(neighborState) + 1`.
   - @43-64: **skip scheduling only when `i == 1 && state.DISTANCE == 1`**
     (if_icmpne 67 @46; if_icmpeq 77 @64).
   - @67-72: `scheduleTick(pos, this, 1)` — **delay constant `iconst_1`
     @71**. Returns state unchanged @77.
   - So the tick is NOT conditioned on "distance would change": any leaf
     face whose across-the-face neighbor is not a distance-0 source (or
     whose own DISTANCE≠1) schedules. Leaf-vs-leaf faces always schedule.
   - `getDistanceAt` @0-9 = `getOptionalDistanceAt(state).orElse(7)`;
     `getOptionalDistanceAt` @0-45: `PREVENTS_NEARBY_LEAF_DECAY` tag → 0,
     `hasProperty(DISTANCE)` → value, else empty. `DECAY_DISTANCE = 7`.
8. `ScheduledTickAccess.scheduleTick(BlockPos, Block, int)` default @0-20:
   `getBlockTicks().schedule(createTick(pos, block, delay))`.
   `LevelAccessor.createTick(pos, T, int)` default @0-24:
   `new ScheduledTick(type, pos, getGameTime() + delay, nextSubTickCount())`
   (NORMAL priority via the 4-arg ScheduledTick ctor @0-12).
9. `WorldGenRegion` (net.minecraft.**server.level**.WorldGenRegion):
   `getBlockTicks`/`getFluidTicks` @865-875 return fields built in ctor @4-35
   as `WorldGenTickAccess(this::lambda$new$0 / $1)`; `lambda$new$0` @0-8 =
   `getChunk(pos).getBlockTicks()`, `lambda$new$1` = `...getFluidTicks()`.
   `WorldGenTickAccess.schedule` @0-22:
   `containerGetter.apply(tick.pos()).schedule(tick)` — **the tick lands in
   the per-chunk container of the chunk CONTAINING the pos** (ProtoChunk →
   `ProtoChunkTicks`, ProtoChunk.getBlockTicks @62-66). For already-FULL
   neighbors, `ImposterProtoChunk.getBlockTicks` @0-18 returns
   `BlackholeTickAccess.emptyContainer()` when `allowWrites == false`
   (ChunkStatusTasks.full wraps with `false`) — **worldgen schedules into
   FULL chunks are dropped**.
   `WorldGenRegion.getGameTime` = `levelData.getGameTime()` (LevelAccessor
   default @40-44; levelData = `ServerLevel.getLevelData()` ctor @78-83) —
   real server gametime, **irrelevant** (discarded, §2).
   `nextSubTickCount` @980-987 = per-region `AtomicLong.getAndIncrement` —
   also discarded.

## 2. ProtoChunkTicks — what is stored

- Fields: `ticks` = `Lists.newArrayList()` (ArrayList, ctor @4-13);
  `ticksPerPosition` = `ObjectOpenCustomHashSet(SavedTick.UNIQUE_TICK_HASH)`
  @11-22.
- `schedule(ScheduledTick)` @0-26: builds
  `new SavedTick(t.type(), t.pos(), 0, t.priority())` — **delay hard-coded
  `iconst_0` @12**; the ScheduledTick's triggerTick and subTickOrder are
  DISCARDED. "delay 1" → stored `t = 0`. Gametime does not matter.
- private `schedule(SavedTick)` @0-24: `if (ticksPerPosition.add(tick))
  ticks.add(tick)` — **dedup FIRST-WINS**, key = (type reference-equality,
  pos.equals) per `SavedTick$1`: hashCode @0-18 = `31*pos.hashCode() +
  type.hashCode()`, equals @17-47 `type == && pos.equals`. Delay/priority
  are NOT part of the key.
- `pack(long)` @71-75: **returns the `ticks` list itself; gametime ignored**.
- `load(List<SavedTick>)` @84-98: forEach → private schedule — order AND
  stored delay preserved, same dedup.
- `scheduledTicks()` @77-82 = `List.copyOf(ticks)`.

## 3. Save encoding — the t math

- `ScheduledTick` record: `type, pos, triggerTick(J), priority,
  subTickOrder(J)`. `toSavedTick(long gt)` @0-26: `delay = (int)(triggerTick
  - gt)` (lsub @16-17, l2i @18).
- `SavedTick.unpack(long gt, long sub)` @55-72: `new ScheduledTick(type, pos,
  gt + delay, priority, sub)`.
- Save entry: `SerializableChunkData.copyOf` @504-509:
  `chunk.getTicksForSerialization(serverLevel.getGameTime())`.
  - `ProtoChunk.getTicksForSerialization` @74-86 →
    `ProtoChunkTicks.pack` ×2 → raw list, `t` = stored delay (0).
  - `LevelChunk.getTicksForSerialization` @272-284 →
    `LevelChunkTicks.pack(gametime)`.
- `LevelChunkTicks.pack(long)` @0-106: `out = new
  ArrayList(queue.size())`; if `pendingTicks != null` **addAll(pendingTicks)
  FIRST, verbatim (original t preserved)** @17-34; copy `tickQueue` to a new
  ArrayList @35-46; `sort(SUB_TICK_ORDERING)` @48-58 where
  `SUB_TICK_ORDERING = Comparator.comparingLong(ScheduledTick::subTickOrder)`
  (clinit @0-8; bootstrap arg `REF_invokeVirtual ScheduledTick.subTickOrder`);
  append `tick.toSavedTick(gametime)` each @60-105.
- Write: `SerializableChunkData.write` @437-442 → `saveTicks(tag,
  packedTicks)` @0-26: `tag.store("block_ticks", BLOCK_TICKS_CODEC,
  ticks.blocks())`, `tag.store("fluid_ticks", FLUID_TICKS_CODEC,
  ticks.fluids())`. clinit @0-35: `BLOCK_TICKS_CODEC =
  SavedTick.codec(BuiltInRegistries.BLOCK.byNameCodec()).listOf()`; FLUID
  likewise.

Why leaves t=0 and water t=5 coexist in chunk (1,0):

- Leaves: worldgen path → ProtoChunkTicks forces delay 0 (§2). At FULL,
  `LevelChunk(ServerLevel, ProtoChunk, ...)` ctor @11/@15 calls
  `ProtoChunk.unpackBlockTicks/unpackFluidTicks` @686-698 → private
  `ProtoChunk.unpackTicks` @677-683 = `new
  LevelChunkTicks<>(protoTicks.scheduledTicks())`; the `LevelChunkTicks(List)`
  ctor @32-87 stores the list as `pendingTicks` (order kept) and only adds
  probes to `ticksPerPosition` @63-83 — **no triggerTick assigned**.
  `LevelChunkTicks.unpack(long)` @0-74 is called ONLY from
  `ServerLevel.startTickingChunk` @0-8 (`unpackTicks(getGameTime())`,
  `LevelChunk.unpackTicks` @1512-1521; jar-wide constant-pool grep: sole
  caller; invoked from `ChunkMap.lambda$prepareTickingChunk$2` @23-31 right
  after `postProcessGeneration`). Two cases, byte-identical output:
  - never promoted to ticking: pack() emits pendingTicks verbatim → t=0;
  - promoted at gametime G and saved at the same G (golden: `LastUpdate=8`
    for all 1024 chunks): unpack(8) gives trigger=8+0, subTickOrder
    `-N..-1` assigned in list order (`ineg` of size @7-17, `iinc 3,1` @56);
    pack(8) sorts by subTickOrder (order reproduced) and `t = 8-8 = 0`.
  **No gametime assumption needed for the leaves rows.**
- Water t=5: these were scheduled on the LIVE ServerLevel, not through
  ProtoChunkTicks. `Level.scheduleTick` → `LevelTicks.schedule` @0-50
  (routes to the containing chunk's `LevelChunkTicks` via `allContainers`,
  errors if absent); `Level.nextSubTickCount` @0-11 = `subTickCount++`
  (level-global monotonic long, starts 0). triggerTick =
  gametime_sched + 5 (`WaterFluid.getTickDelay` @0-1 `iconst_5`). Saved
  `t = trigger − 8 = 5` ⇒ **gametime_sched = 8 = LastUpdate**: the schedule
  and the save happened at the same gametime. ASSUMPTION (stated): the
  recording's gametime advanced by exactly N=0 between the fluid schedules
  and the save — empirically exact in the golden (all 10 370 live fluid
  rows region-wide are t=5, LastUpdate=8 everywhere).
- The live scheduling pass is `LevelChunk.postProcessGeneration(ServerLevel)`
  @0-243 (from `ChunkMap.lambda$prepareTickingChunk$2` @23, i.e. ticking
  promotion): per marked postProcessing pos: fluid non-empty →
  `FluidState.tick` @99-106; `instanceof LiquidBlock` → `BlockState.tick`
  @109-129 (`LiquidBlock.tick` @0-27 is bubble-column only); else
  `Block.updateFromNeighbourShapes` @135-142 + `setBlock(pos, new, 276)`
  @152-160; then `ShortList.clear()` @167-169 (⇒ golden PostProcessing = 24
  empty lists in every chunk). `Block.updateFromNeighbourShapes` @0-80
  iterates `UPDATE_SHAPE_ORDER`, passing the LevelAccessor as
  ScheduledTickAccess. `FlowingFluid.tick` @0-109: non-source → if new state
  differs `setBlock(pos,·,3)` + `scheduleTick(pos, newState.getType(),
  getSpreadDelay(...))` @63-97 (`getSpreadDelay` @0-5 = `getTickDelay` = 5);
  then `spread()`. Spread's `setBlock(...,3)` fires neighbor updates →
  `LiquidBlock.neighborChanged` @0-30 / `onPlace` @0-30: `shouldSpreadLiquid`
  → `Level.scheduleTick(pos, state.getFluidState().getType(),
  fluid.getTickDelay(level))` — sources save as `minecraft:water`, flowing
  as `minecraft:flowing_water`.
- Marked-position producers (jar-wide grep for `markPosForPostProcessing`):
  NoiseBasedChunkGenerator, WorldCarver, Feature, StructurePiece, Blender,
  MultifaceGrowthFeature (+ MultifaceSpreader$SpreadConfig).

## 4. Save ORDER

- `LevelChunkTicks.tickQueue = new PriorityQueue(ScheduledTick.DRAIN_ORDER)`
  (ctor @5-14). `DRAIN_ORDER` = `lambda$static$0` @0-47: triggerTick, then
  priority, then subTickOrder (`INTRA_TICK_DRAIN_ORDER` = priority,
  subTickOrder — not used for save).
- **Heap order is irrelevant to the save**: `pack` does NOT stream the
  queue; it copies to an ArrayList and explicitly sorts by
  `subTickOrder` (§3). NBT list order rule:
  `pendingTicks (in stored order) ++ sort_by_subTickOrder(queue)`.
  ProtoChunkTicks: pure ArrayList insertion order after first-wins dedup.
- Load: `SerializableChunkData` parse @190-232 decodes the lists and applies
  `SavedTick.filterTickListForChunk` (@0-27: stream filter
  `ChunkPos.pack(pos) == chunkpos.pack()`, order-preserving); status ≥ FULL
  → `new LevelChunkTicks<>(list)` @592-604 (pendingTicks = list); proto →
  `ProtoChunkTicks.load(list)` @633-638 (order + delay preserved).
- Therefore **save→load→save is order- and byte-stable**: subTickOrder is
  monotone at insert (worldgen region AtomicLong / level counter / unpack's
  −N..−1), `List.sort` is on distinct keys (and TimSort is stable), and
  re-serialization rebases t only if an `unpack` happened in between:
  `t' = t − (gametime_save' − gametime_unpack)` (= t when both are the same
  tick, as in the golden).

## 5. Fluid ticks during worldgen proper

`LiquidBlock.updateShape` @0-85: if own `getFluidState().isSource()` OR
neighbor's `isSource()` @0-18 → `scheduleTick(pos,
state.getFluidState().getType(), this.fluid.getTickDelay(level))` @21-39
(then bubble-column check @44-65, then `super.updateShape`). Water delay = 5
(`WaterFluid.getTickDelay` @0-1); lava delay not checked here (UNVERIFIED,
not needed: all 83 lava rows region-wide saved t=0 = proto path). Any such
schedule during features/carvers goes through WorldGenRegion →
ProtoChunkTicks → **t forced to 0** (region-wide 1 193 fluid rows with t=0).
The t=5 rows are exclusively the §3 postProcessGeneration/live pass.

## 6. Tick entry NBT

- Codec field (declaration/insertion) order — `SavedTick.lambda$codec$1`
  @0-74: `i` (type codec `.fieldOf("i")` @2-4), pos MapCodec, `t`
  (`Codec.INT.fieldOf("t")` @26-31), `p` (`TickPriority.CODEC.fieldOf("p")`
  @44-49); pos MapCodec (`lambda$codec$0` @0-67): `x`,`y`,`z` all
  `Codec.INT` — absolute block coordinates (BlockPos fields).
- Tag types: i = String (registry `byNameCodec`, e.g.
  `minecraft:oak_leaves`); x,y,z,t,p = Int. `TickPriority.CODEC =
  Codec.INT.xmap(TickPriority::byValue, TickPriority::getValue)` (clinit
  @107-125); NORMAL.value = 0 (clinit @48-55).
- **Byte order of keys in the file: `p, t, x, i, y, z`** — verified on all
  106 759 entries of the golden region. Cause: `CompoundTag.tags = new
  java.util.HashMap()` (ctor @27-30); insertion order is the codec order
  i,x,y,z,t,p; single-char keys, table cap 16 (6 entries < 12 threshold):
  buckets p→0, t→4, x→8, i→9, y→9, z→10; bucket 9 chains i before y
  (insertion order) → iteration p,t,x,i,y,z. C writer: emit these six keys
  literally in byte order `p,t,x,i,y,z`.
- Empty list: `fluid_ticks` with 0 entries = TAG_List with element type 0
  (End) and length 0 (golden chunk (0,0)).

## 7. FULL conversion

`ProtoChunk.unpackBlockTicks/unpackFluidTicks` @686-698 → `new
LevelChunkTicks<>(scheduledTicks())` — **order preserved; no triggerTick
yet** (pendingTicks). triggerTick is assigned only at
`ServerLevel.startTickingChunk` → `LevelChunk.unpackTicks(gametime)`
@1512-1521 → `LevelChunkTicks.unpack(gt)`: `trigger = gt + savedDelay`,
`subTickOrder = -pending.size() .. -1` in list order — sorts BEFORE any
later live schedule (counter ≥ 0) and reproduces list order on repack.

## Golden empirical (grid chunks (0,0),(1,0),(0,1),(1,1))

- block_ticks: 681 / 817 / 419 / 558 entries; all `t=0, p=0`; i ∈
  {oak_leaves, jungle_leaves} only; not sorted by any coordinate (insertion
  order; e.g. (0,1) starts (0,89,26),(0,89,27),(0,76,30),(0,72,18)…).
- fluid_ticks: only (1,0): 8 entries, all `t=5, p=0`:
  water×6 at (31,−32..−27,0) ascending y, flowing_water at (26,−21,12),
  water at (25,−21,12). Others empty.
- Region-wide (context, OUTSIDE the 4-chunk gate): block t ∈ {0:92034,
  2:3076 (sand/gravel/suspicious_sand/cave_air, live delay-2 shape updates),
  5:82 (bubble_column), 1:4}; fluid t ∈ {5:10370, 0:1193}; every chunk
  LastUpdate=8, PostProcessing = 24 empty lists.
- CAUTION for Task-12 blocks parity: the same gametime-8 pass that produced
  the t=5 rows also MUTATED blocks (`FlowingFluid.tick` `setBlock(·,3)` +
  spread; `postProcessGeneration` `setBlock(·,276)`), i.e. region payload
  block data can differ from the 11_full dump at fluid-spread positions
  (the flowing_water at (26,−21,12) is such a product). UNVERIFIED whether
  any of the 4 grid chunks' section bytes actually differ — the Task-12
  empirical extraction should diff blocks, not only ticks.

## C implementation rule

(a) **Positions that get a block tick in the features replay**: during each
tree's post-placement pass (`updateShapeAtEdge`, flags 3), for every exposed
face (both sides) of the updateLeaves voxelshape, run updateShape; a state
that is leaves (has DISTANCE; `PREVENTS_NEARBY_LEAF_DECAY` handled via
distance 0) at position q schedules `(leaves_block_of(q), q)` UNLESS
`getDistanceAt(across_state)+1 == 1 && q.DISTANCE == 1`
(LeavesBlock.updateShape @34-72); waterlogged leaves additionally schedule a
fluid tick `(water, q)` (@0-29). Record into the proto tick list of the
chunk CONTAINING q (WorldGenTickAccess.schedule @0-22), dedup key
(block, pos) FIRST-WINS (ProtoChunkTicks @0-24, SavedTick$1). Schedules
aimed at chunks already past FULL are dropped (ImposterProtoChunk @0-18).

(b) **Saved t value**: 0 for every worldgen-scheduled tick — delay is
overwritten with `iconst_0` at `ProtoChunkTicks.schedule` @12 and passes
through pack (@71-75), load (@84-98), FULL conversion (pendingTicks), and
even an unpack+repack at unchanged gametime (`unpack` trigger = G+0,
`toSavedTick` t = G−G = 0). p = 0 (NORMAL). The grid fluid rows are t=5:
live postProcess schedules with delay 5 (WaterFluid.getTickDelay @0-1;
FlowingFluid.tick @63-97; LiquidBlock.onPlace/neighborChanged @0-30) at
gametime 8 == save gametime (LastUpdate=8; assumption N=0 stated in §3).

(c) **NBT list order**: per containing chunk, chronological order of the
schedule calls after first-wins dedup (ProtoChunkTicks ArrayList); on the
save side `LevelChunkTicks.pack` @17-58 emits pendingTicks (that same order)
first, then live-scheduled ticks sorted by their global schedule sequence
(`SUB_TICK_ORDERING` = comparingLong(subTickOrder), Level.nextSubTickCount
@0-11 monotone) — for the grid chunks: all leaves in worldgen insertion
order, then (fluid list only) the postProcess water rows in schedule order.
Entry compound key byte order: `p, t, x, i, y, z` (§6); i as
`minecraft:<name>` String; x,y,z absolute; empty list = TAG_List(End, 0).
