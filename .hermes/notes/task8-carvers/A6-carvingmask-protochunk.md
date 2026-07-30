# A6 — `CarvingMask` + `ProtoChunk` write path + worldgen heightmaps (MC 26.2, unobfuscated server bytecode)

Source of truth: `javap -p -c -constants` (and `javap -v` for BootstrapMethods) against
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server`. Classes:
`net/minecraft/world/level/chunk/CarvingMask.class` (+ `CarvingMask$Mask`),
`net/minecraft/world/level/chunk/ProtoChunk.class`, `net/minecraft/world/level/chunk/ChunkAccess.class`,
`net/minecraft/world/level/chunk/LevelChunkSection.class`,
`net/minecraft/world/level/levelgen/Heightmap.class` (+ `Heightmap$Types`, `Heightmap$Usage`),
`net/minecraft/world/level/chunk/status/ChunkStatus.class`, `ChunkStep.class`,
`net/minecraft/world/level/levelgen/BelowZeroRetrogen.class` (+ `$1`),
`net/minecraft/world/level/block/state/BlockBehaviour$BlockStateBase.class`,
`net/minecraft/world/level/chunk/storage/SerializableChunkData.class`.
All pseudocode is a 1:1 reconstruction from bytecode. No vanilla-source guessing.

Established elsewhere and cited, not re-derived: the caller side of `mask.get/set`
and `chunk.setBlockState`/`markPosForPostProcessing` (A2 §2–3), the single-mask /
no-`GenerationStep$Carving` 26.2 delta (A1 §6), `getOrCreateCarvingMask` call site in
`applyCarvers` (A1 §4 step 15), `Blender.addAroundOldChunksCarvingMaskFilter` (A1 §1.2).

---

## 1. `CarvingMask` — complete class

Fields (this is the whole class):

```java
private final int minY;
private final java.util.BitSet mask;
private CarvingMask$Mask additionalMask;        // NOT final; interface: boolean test(int,int,int)
```

### 1.1 Constructors

```java
public CarvingMask(int height, int minY) {
    this.additionalMask = (x, y, z) -> false;   // indy #0 → lambda$new$0: iconst_0; ireturn
    this.minY = minY;
    this.mask = new BitSet(256 * height);       // sipush 256; iload_1; imul
}

public CarvingMask(long[] data, int minY) {     // deserialization ctor
    this.additionalMask = (x, y, z) -> false;
    this.minY = minY;
    this.mask = BitSet.valueOf(data);
}
```

`java.util.BitSet(int)` is an initial *capacity*; `BitSet.set` grows on demand. Capacity
`256*height` is exact for the index range below, so for a C port a fixed
`uint64_t[256*height/64]` = `4*height` words is safe **provided indices stay in range**
(they do — see §1.3 bound analysis).

### 1.2 `getIndex` — exact bit layout

```java
private int getIndex(int x, int y, int z) {
    return (x & 15) | ((z & 15) << 4) | ((y - this.minY) << 8);
}
```

Bytecode: `iload_1; bipush 15; iand` (x) — `iload_3; bipush 15; iand; iconst_4; ishl; ior` (z) —
`iload_2; getfield minY; isub; bipush 8; ishl; ior` (y). So:

- **index = x + 16·z + 256·(y − minY)** — x is bits 0–3, z is bits 4–7, y-offset is bits 8+.
- Parameter order of `getIndex/set/get` is `(x, y, z)`; the **y is ABSOLUTE block Y** and the
  `minY` subtraction happens here, inside the mask (A2 §2 passes `(x0∈[0,15], y0=absolute, z0∈[0,15])`).
- x and z are masked `& 15` inside; y − minY is **not** masked or bounds-checked.

### 1.3 `set` / `get`

```java
public void set(int x, int y, int z) {
    this.mask.set(this.getIndex(x, y, z));
}

public boolean get(int x, int y, int z) {
    return this.additionalMask.test(x, y, z)          // raw args: (localX, ABSOLUTE y, localZ)
        || this.mask.get(this.getIndex(x, y, z));
}
```

- `get` = additionalMask-OR-bitset; `set` writes **only** the BitSet (the additional mask is
  read-only overlay). Short-circuit: BitSet not consulted when the filter says true.
- Default `additionalMask` returns false ⇒ for a fresh world `get` ≡ BitSet test.
- **Bound analysis for the C port:** per A2 §2 the carve loops call with
  `y0 ∈ [minY+1 .. minGenY+genDepth−1−topPad]` where for normal generation
  `ctx` height accessor == the chunk (§6), so `0 < y−minY < height` always; index ∈ (255, 256·height−1].
  Never negative, never ≥ 256·height. A fixed array cannot overflow.

### 1.4 `setAdditionalMask` — YES, it exists on 26.2

```java
public void setAdditionalMask(CarvingMask$Mask filter) { this.additionalMask = filter; }
```

Only caller in the entire server jar: `net/minecraft/world/level/levelgen/blending/Blender.class`
(`grep -rla setAdditionalMask net/` → Blender + CarvingMask only), i.e. the old-chunk blending
filter installed by `Blender.addAroundOldChunksCarvingMaskFilter` (A1 §1.2). No-op for fresh
hyperchunk worlds. Note for completeness: the filter's `test` receives the same raw
`(localX, absoluteY, localZ)` triple.

### 1.5 `stream` + serialization form

```java
public Stream<BlockPos> stream(ChunkPos chunkPos) {
    return this.mask.stream()                              // IntStream of set indices, ascending
        .mapToObj(i -> {
            int x = i & 15;                                // lambda$stream$0
            int z = (i >> 4) & 15;                         // ishr (sign-irrelevant, i ≥ 0)
            int y = i >> 8;
            return chunkPos.getBlockAt(x, y + this.minY, z);   // BlockPos(minBlockX+x, y+minY, minBlockZ+z)
        });
}

public long[] toArray() { return this.mask.toLongArray(); }
```

NBT round trip (`SerializableChunkData`, bytecode-verified):

- **Write** (only when `status.getChunkType() == ChunkType.PROTOCHUNK`):
  `protoChunk.getCarvingMask()`; if non-null → `toArray()` → `tag.putLongArray("carving_mask", data)`.
  `BitSet.toLongArray()` is little-endian bit order (bit i of word i/64) and **truncates trailing
  all-zero words** — round-trips exactly through `BitSet.valueOf`.
- **Read**: `tag.getLongArray("carving_mask")` (Optional, orElse null) →
  `new CarvingMask(data, chunk.getMinY())` → `protoChunk.setCarvingMask(mask)`.

26.2 delta: flat `carving_mask` LongArray key. The 1.21-era `CarvingMasks` compound
(`{AIR:[long], LIQUID:[long]}`) is gone; the datafixer
`net/minecraft/util/datafix/fixes/CarvingStepRemoveFix.class` contains both strings
(`CarvingMasks` → `carving_mask`) — bytecode-level confirmation of the single-mask migration
(matches A1 §6).

---

## 2. `ProtoChunk` — fields and carving mask accessors

Declared fields (complete list — confirms A1 §6's "single carvingMask field"):

```java
private static final Logger LOGGER;
private volatile LevelLightEngine lightEngine;
private volatile ChunkStatus status;                        // starts ChunkStatus.EMPTY
private final List<CompoundTag> entities;
private CarvingMask carvingMask;                            // SINGLE field, no per-step map
private BelowZeroRetrogen belowZeroRetrogen;
private final ProtoChunkTicks<Block> blockTicks;
private final ProtoChunkTicks<Fluid> fluidTicks;
```

(Everything else — `sections`, `heightmaps`, `postProcessing`, `skyLightSources`,
`levelHeightAccessor`, `noiseChunk`, … — lives on `ChunkAccess`.)

### 2.1 `getOrCreateCarvingMask` — sizing

```java
public CarvingMask getOrCreateCarvingMask() {
    if (this.carvingMask == null) {
        this.carvingMask = new CarvingMask(this.getHeight(), this.getMinY());
    }
    return this.carvingMask;
}
// plus trivial: getCarvingMask() → field (may be null); setCarvingMask(m) → field = m.
```

`getHeight()`/`getMinY()` are `ChunkAccess` methods that delegate to the
`levelHeightAccessor` field (the ServerLevel's dimension accessor) — **always the full chunk
height**, even for upgrading chunks (overworld: height 384, minY −64 ⇒ BitSet capacity 98304 =
1536 longs). The upgrade accessor (§6) narrows *carve bounds*, never the mask.

### 2.2 `ProtoChunk.getBlockState(BlockPos)` — 1:1 (assignment item 7)

```java
public BlockState getBlockState(BlockPos pos) {
    int y = pos.getY();
    if (this.isOutsideBuildHeight(y)) {                   // y < getMinY() || y > getMaxY()
        return Blocks.VOID_AIR.defaultBlockState();       // NOT plain air
    }
    LevelChunkSection section = this.getSection(this.getSectionIndex(y));
    if (section.hasOnlyAir()) {                           // nonEmptyBlockCount == 0
        return Blocks.AIR.defaultBlockState();
    }
    return section.getBlockState(pos.getX() & 15, y & 15, pos.getZ() & 15);
}
```

- `isOutsideBuildHeight(int)` (LevelHeightAccessor default): `y < getMinY() || y > getMaxY()`
  (`if_icmplt` / `if_icmple` — maxY inclusive-inside). `getMaxY = minY + height − 1` = 319 overworld.
- `getSectionIndex(y)` (default) = `SectionPos.blockToSectionCoord(y) − getMinSectionY()`
  = `(y >> 4) − minSectionY`. `getSection(i)` = `getSections()[i]`, no null risk: the ChunkAccess
  ctor runs `replaceMissingSections` filling every null slot with an empty `LevelChunkSection`.
- x/z are masked `& 15` — callers may pass absolute or relative coords interchangeably
  (load-bearing for the heightmap rescan, §5.2).
- `getFluidState(pos)` is the same shape (VOID branch returns `Fluids.EMPTY.defaultFluidState()`).
- VOID_AIR: `isAir()` true, `blocksMotion()` false — for heightmap predicates it behaves as air.

---

## 3. `ProtoChunk.setBlockState(BlockPos, BlockState, int flags)` — FULL reconstruction

The 2-arg convenience used by carvers (A2 §3 step 6) is on **ChunkAccess**:

```java
public BlockState setBlockState(BlockPos pos, BlockState state) {
    return this.setBlockState(pos, state, 3);             // iconst_3 — verified
}
```

The 3-arg override on ProtoChunk (locals: x=4, y=5, z=6, sectionIndex=7, section=8,
sectionWasOnlyAir=9, relX=10, relY=11, relZ=12, oldState=13):

```java
public BlockState setBlockState(BlockPos pos, BlockState state, int flags) {   // `flags` NEVER loaded
    int x = pos.getX(), y = pos.getY(), z = pos.getZ();

    if (this.isOutsideBuildHeight(y)) {                                        // (1)
        return Blocks.VOID_AIR.defaultBlockState();
    }
    int sectionIndex = this.getSectionIndex(y);
    LevelChunkSection section = this.getSection(sectionIndex);                 // (2) plain array read
    boolean sectionWasOnlyAir = section.hasOnlyAir();
    if (sectionWasOnlyAir && state.is(Blocks.AIR)) {                           // (3) empty-section air shortcut
        return state;                                                          //     returns the INPUT state
    }
    int relX = SectionPos.sectionRelative(x);                                  // & 15
    int relY = SectionPos.sectionRelative(y);
    int relZ = SectionPos.sectionRelative(z);
    BlockState oldState = section.setBlockState(relX, relY, relZ, state);      // (4) → (.., true) §3.1

    if (this.status.isOrAfter(ChunkStatus.INITIALIZE_LIGHT)) {                 // (5) FALSE during CARVERS §3.2
        boolean nowOnlyAir = section.hasOnlyAir();
        if (nowOnlyAir != sectionWasOnlyAir) {
            this.lightEngine.updateSectionStatus(pos, nowOnlyAir);
        }
        if (LightEngine.hasDifferentLightProperties(oldState, state)) {
            this.skyLightSources.update(this, relX, y, relZ);
            this.lightEngine.checkBlock(pos);
        }
    }

    EnumSet<Heightmap.Types> after = this.getPersistedStatus().heightmapsAfter();  // (6) §3.2
    EnumSet<Heightmap.Types> missing = null;
    for (Heightmap.Types type : after) {                                       // (7) lazy prime check
        Heightmap h = this.heightmaps.get(type);           // heightmaps = Maps.newEnumMap(Types.class)
        if (h == null) {
            if (missing == null) missing = EnumSet.noneOf(Heightmap.Types.class);
            missing.add(type);
        }
    }
    if (missing != null) {
        Heightmap.primeHeightmaps(this, missing);                              // (8) §5.3
    }
    for (Heightmap.Types type : after) {                                       // (9) EnumSet = ordinal order
        this.heightmaps.get(type).update(relX, y, relZ, state);                //     ABSOLUTE y; result popped
    }
    return oldState;                                                           // (10)
}
```

Exact semantics:

1. **Out-of-height** (y < −64 or y > 319 overworld): returns `VOID_AIR.defaultBlockState()`,
   writes nothing, updates nothing. (Carve loops never hit this — A2 §2 clamps — but the grass
   fixup at `pos.below()` could only hit it at y = minY, which the carve bounds also exclude.)
2. No section "acquire/release": ProtoChunk uses a plain `sections[i]` read
   (LevelChunk's acquire/unlock discipline does not exist here).
3. **Empty-section air shortcut**: placing any `minecraft:air`-block state into an all-air
   section is a full no-op — **skips heightmap updates too** — and returns the *input* state
   (quirk: not the old state; indistinguishable for air-into-air, but note bytecode
   `aload_2; areturn`). `state.is(Blocks.AIR)` is exact-block AIR: CAVE_AIR does NOT shortcut.
   Carvers place CAVE_AIR/WATER/LAVA (A2 §4), so carver writes never take this branch.
4. Palette write via §3.1; returns previous state.
5. Light hooks: dead during worldgen carving (§3.2). No `markPosForPostProcessing`, no
   `markUnsaved`, no block-entity handling anywhere in this method.
6.–9. Heightmap maintenance: see §3.2 for which Types, §5 for `update`/`primeHeightmaps`.
10. **Return = previous BlockState** at that position. A2 §3's carver caller discards it.

**flags is a dead parameter** — no `iload_3` anywhere in the body. `flags=3`
(UPDATE_NEIGHBORS|UPDATE_CLIENTS) is meaningful only for `Level.setBlock`; for ProtoChunk it is
ignored, resolving A2 §3(3)'s OPEN item.

### 3.1 `LevelChunkSection.setBlockState(relX, relY, relZ, state)` — counter maintenance

```java
public BlockState setBlockState(int x, int y, int z, BlockState state) {
    return this.setBlockState(x, y, z, state, true);                 // locked
}
public BlockState setBlockState(int x, int y, int z, BlockState state, boolean useLocks) {
    BlockState old = useLocks ? this.states.getAndSet(x, y, z, state)   // PalettedContainer
                              : this.states.getAndSetUnchecked(x, y, z, state);
    if (!old.isAir()) {
        --this.nonEmptyBlockCount;
        if (old.isRandomlyTicking()) --this.tickingBlockCount;
        FluidState f = old.getFluidState();
        if (!f.isEmpty()) { --this.fluidCount; if (f.isRandomlyTicking()) --this.tickingFluidCount; }
    }
    if (!state.isAir()) {
        ++this.nonEmptyBlockCount;
        if (state.isRandomlyTicking()) ++this.tickingBlockCount;
        FluidState f = state.getFluidState();
        if (!f.isEmpty()) { ++this.fluidCount; if (f.isRandomlyTicking()) ++this.tickingFluidCount; }
    }
    return old;                                                      // counters are short (i2s)
}
```

`hasOnlyAir()` = `nonEmptyBlockCount == 0`. Carving stone→CAVE_AIR decrements
`nonEmptyBlockCount` (isAir uses the state's air flag: CAVE_AIR counts as air), so a fully carved
section can revert to `hasOnlyAir() == true`, re-enabling the §2.2/§3(3) shortcuts. Counters are
`short` with `i2s` truncation (never overflows: ≤4096 blocks).

### 3.2 Which heightmap Types during CARVERS — proof

`ChunkStatus` static{} (bytecode-verified constants):

```java
WORLDGEN_HEIGHTMAPS = EnumSet.of(Types.OCEAN_FLOOR_WG, Types.WORLD_SURFACE_WG);
FINAL_HEIGHTMAPS    = EnumSet.of(Types.OCEAN_FLOOR, Types.WORLD_SURFACE,
                                 Types.MOTION_BLOCKING, Types.MOTION_BLOCKING_NO_LEAVES);
// register(name, parent, heightmapsAfter, chunkType) — per-status field `heightmapsAfter`:
EMPTY, STRUCTURE_STARTS, STRUCTURE_REFERENCES, BIOMES, NOISE, SURFACE  → WORLDGEN_HEIGHTMAPS
CARVERS, FEATURES, INITIALIZE_LIGHT, LIGHT, SPAWN, FULL                → FINAL_HEIGHTMAPS
```

`heightmapsAfter()` returns that per-status EnumSet; `isOrAfter` = `getIndex() >= other.getIndex()`
(registration order = index: EMPTY 0 … SURFACE 5, CARVERS 6, … FULL 11).

**When does `status` flip to CARVERS?** `ChunkStep.apply` runs `task.doWork(...)` and only in the
completion (`thenApply` → `completeChunkGeneration`) does
`if (chunk instanceof ProtoChunk p && p.getPersistedStatus().isBefore(targetStatus)) p.setPersistedStatus(targetStatus)`.
So **while the CARVERS task executes, `getPersistedStatus()` == SURFACE** and every
`setBlockState` during carving updates exactly
**{WORLD_SURFACE_WG, OCEAN_FLOOR_WG}** (EnumSet iteration = ordinal order: WORLD_SURFACE_WG (0)
first, then OCEAN_FLOOR_WG (2); the two updates are independent so order can't matter).
The FINAL set (incl. MOTION_BLOCKING) only starts being maintained by writes during FEATURES.

`primeHeightmaps` is called lazily ONLY for Types with **no map object at all** in the EnumMap
(`heightmaps.get(t) == null`). A map created via `getOrCreateHeightmapUnprimed` but never primed
would NOT be re-primed here. In normal generation both WG maps already exist and are
incrementally exact by the end of NOISE/SURFACE (hyperchunk's C surface stage mirrors this), so
step (8) does not run during carving; §5.3 gives the exact algorithm anyway.

Also verified on ProtoChunk: the light branch reads the `status` field directly
(`getfield status; isOrAfter(INITIALIZE_LIGHT)`), same value as `getPersistedStatus()`.

---

## 4. `Heightmap$Types` — ordinals, ids, predicates (assignment item 4b)

static{} order / `$values` array (all bytecode constants):

| ordinal | name | id | serializationKey | Usage | isOpaque predicate |
|---|---|---|---|---|---|
| 0 | `WORLD_SURFACE_WG` | 0 | `"WORLD_SURFACE_WG"` | WORLDGEN | `Heightmap.NOT_AIR` |
| 1 | `WORLD_SURFACE` | 1 | `"WORLD_SURFACE"` | CLIENT | `Heightmap.NOT_AIR` |
| 2 | `OCEAN_FLOOR_WG` | 2 | `"OCEAN_FLOOR_WG"` | WORLDGEN | `Heightmap.MATERIAL_MOTION_BLOCKING` |
| 3 | `OCEAN_FLOOR` | 3 | `"OCEAN_FLOOR"` | LIVE_WORLD | `Heightmap.MATERIAL_MOTION_BLOCKING` |
| 4 | `MOTION_BLOCKING` | 4 | `"MOTION_BLOCKING"` | CLIENT | Types.lambda$static$0 |
| 5 | `MOTION_BLOCKING_NO_LEAVES` | 5 | `"MOTION_BLOCKING_NO_LEAVES"` | CLIENT | Types.lambda$static$1 |

`Usage` enum: `WORLDGEN(0), LIVE_WORLD(1), CLIENT(2)`; `keepAfterWorldgen()` = usage != WORLDGEN
(⇒ the two `_WG` maps are dropped from the map at promotion), `sendToClient()` = usage == CLIENT.

The predicates, from `Heightmap.class` BootstrapMethods (`javap -v`):

- **`NOT_AIR`** — indy #0 → `invokestatic Heightmap.lambda$static$0`:

  ```java
  state -> !state.isAir()
  ```

- **`MATERIAL_MOTION_BLOCKING`** — indy #1 → **`REF_invokeVirtual
  BlockBehaviour$BlockStateBase.blocksMotion:()Z`** — a plain method reference,
  **no fluid term of any kind**:

  ```java
  state -> state.blocksMotion()
  ```

  `BlockStateBase.blocksMotion()` bytecode:

  ```java
  public boolean blocksMotion() {
      Block block = this.getBlock();
      return block != Blocks.COBWEB && block != Blocks.BAMBOO_SAPLING && this.isSolid();
  }
  public boolean isSolid() { return this.legacySolid; }   // cached flag baked at state init
  ```

  Water and lava states have `legacySolid == false` ⇒ `blocksMotion()` false ⇒
  **fluids DO NOT count for OCEAN_FLOOR_WG / OCEAN_FLOOR**. (The `legacySolid` bake
  (`calculateSolid`) is not re-derived here; hyperchunk's per-state blocks-motion flags are
  already golden-validated by the surface stage.)

- `MOTION_BLOCKING` (Types.lambda$static$0 — irrelevant during carving, FINAL set only):

  ```java
  state -> state.blocksMotion() || !state.getFluidState().isEmpty()
  ```

- `MOTION_BLOCKING_NO_LEAVES` (Types.lambda$static$1):

  ```java
  state -> (state.blocksMotion() || !state.getFluidState().isEmpty())
        && !(state.getBlock() instanceof LeavesBlock)
  ```

### 4.1 Carve-state effect table (what the C side must reproduce)

Per A2 §4, a carver write is CAVE_AIR, WATER (aquifer/canyon), or LAVA; the grass fixup may also
write a surface-rule state. Predicate results:

| placed state | `isAir()` | `blocksMotion()` | WORLD_SURFACE_WG | OCEAN_FLOOR_WG |
|---|---|---|---|---|
| `cave_air` | true | false | non-opaque → rescan if at top (§5.1) | non-opaque → rescan if at top |
| `water` (source) | false | false | **opaque** → raises to y+1 if y ≥ firstAvailable | non-opaque → rescan if at top |
| `lava` (source) | false | false | opaque (same as water) | non-opaque → rescan if at top |

So carving air lowers both maps via the descending rescan; carving **water/lava lowers only
OCEAN_FLOOR_WG** (WORLD_SURFACE_WG treats fluid as surface and will not drop — and cannot rise
either, since carve targets sit below the existing surface).

---

## 5. `Heightmap` — storage and the `update` path

### 5.1 Storage + `update(x, y, z, state)` — 1:1

```java
public Heightmap(ChunkAccess chunk, Types type) {
    this.isOpaque = type.isOpaque();
    this.chunk = chunk;
    int bits = Mth.ceillog2(chunk.getHeight() + 1);        // overworld: ceillog2(385) = 9
    this.data = new SimpleBitStorage(bits, 256);           // 256 columns, zero-init
}
private static int getIndex(int x, int z) { return x + z * 16; }
private int  getFirstAvailable(int idx)   { return this.data.get(idx) + this.chunk.getMinY(); }
public  int  getFirstAvailable(int x, int z) { return getFirstAvailable(getIndex(x, z)); }
public  int  getHighestTaken(int x, int z)   { return getFirstAvailable(getIndex(x, z)) - 1; }
private void setHeight(int x, int z, int y)  { this.data.set(getIndex(x, z), y - this.chunk.getMinY()); }
```

Stored value = `firstAvailable − minY` where `firstAvailable` = absolute Y of the first
non-opaque cell above the topmost opaque one (0 ⇒ column empty ⇒ firstAvailable = minY).

```java
public boolean update(int x, int y, int z, BlockState state) {   // x,z ∈ [0,15]; y ABSOLUTE
    int firstAvailable = this.getFirstAvailable(x, z);
    if (y <= firstAvailable - 2) {                     // strictly below the surface cell: no-op
        return false;
    }
    if (this.isOpaque.test(state)) {
        if (y >= firstAvailable) {                     // new opaque at/above surface: raise
            this.setHeight(x, z, y + 1);
            return true;
        }
        // opaque at y == firstAvailable-1 (already the top): fall through → false
    } else if (firstAvailable - 1 == y) {              // top opaque block replaced by non-opaque:
        BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
        for (int yy = y - 1; yy >= this.chunk.getMinY(); --yy) {   // DESCENDING rescan, minY INCLUSIVE
            pos.set(x, yy, z);                         // NOTE: x,z are the section-relative values
            if (this.isOpaque.test(this.chunk.getBlockState(pos))) {
                this.setHeight(x, z, yy + 1);
                return true;
            }
        }
        this.setHeight(x, z, this.chunk.getMinY());    // no opaque left: firstAvailable = minY
        return true;
    }
    return false;
}
```

Load-bearing details for the C port (verifies the surface-stage implementation survives carving):

- Branch structure is exactly: `y <= fa−2 → false`; else opaque? `(y >= fa → raise)` :
  `(fa−1 == y → rescan)`; everything else false. A non-opaque write ABOVE the surface
  (y ≥ fa) is a no-op — e.g. carving air through already-carved air above the surface.
- The rescan starts at `y − 1` (does not re-test y itself; the new state is already in the
  section — ProtoChunk writes the palette at (4) *before* update at (9)) and runs down to
  `chunk.getMinY()` **inclusive** (`if_icmplt` exit).
- **Rescan access pattern**: `chunk.getBlockState(new MutableBlockPos(x, yy, z))` where x/z are
  the *relative* coords passed in by ProtoChunk. This round-trips through the full
  `ProtoChunk.getBlockState` (§2.2): out-of-range y impossible (yy ≥ minY, yy < y ≤ maxY),
  empty-section shortcut returns AIR, x/z re-masked `& 15` so relative-as-absolute is harmless.
  It reads the chunk's CURRENT contents — including blocks carved earlier in the same
  ellipsoid/column (y descends in carveEllipsoid per A2 §2, so lower blocks are still uncarved
  when the rescan passes them; the rescan therefore lands on the next uncarved opaque block,
  which a later carve of that very block will re-lower — the incremental invariant holds).
- Return value: true iff stored height changed; ProtoChunk pops it.

### 5.2 `primeHeightmaps(ChunkAccess, Set<Types>)` — 1:1 (lazy branch of §3 step 8)

```java
public static void primeHeightmaps(ChunkAccess chunk, Set<Types> types) {
    if (types.isEmpty()) return;
    int n = types.size();
    ObjectList<Heightmap> list = new ObjectArrayList<>(n);
    ObjectListIterator<Heightmap> iter = list.iterator();          // created ONCE, before loops
    int scanStartY = chunk.getHighestSectionPosition() + 16;       // top of highest non-empty section + 16
    BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            for (Types t : types) list.add(chunk.getOrCreateHeightmapUnprimed(t));  // append n entries
            for (int y = scanStartY - 1; y >= chunk.getMinY(); --y) {
                pos.set(x, y, z);
                BlockState state = chunk.getBlockState(pos);
                if (state.is(Blocks.AIR)) continue;                // exact-block AIR skip (not cave_air!)
                while (iter.hasNext()) {
                    Heightmap h = iter.next();
                    if (h.isOpaque.test(state)) {
                        h.setHeight(x, z, y + 1);
                        iter.remove();                             // satisfied for this column
                    }
                }
                if (list.isEmpty()) break;                         // all satisfied → next column
                iter.back(n);                                      // rewind n for next y
            }
        }
    }
}
```

Quirks preserved as-is: the single iterator across a mutating fastutil list (append leaves
prior positions valid; `back(n)` rewinds); columns with unsatisfied maps leave stale entries in
`list` (their height stays 0 = minY, the correct "empty column" encoding);
`getHighestSectionPosition()` = `getHighestFilledSectionIndex()` (top-down first
`!hasOnlyAir()` section, −1 if none) mapped to that section's min block Y, or `getMinY()` if all
empty. `state.is(Blocks.AIR)` is an exact-block fast-path — CAVE_AIR/VOID_AIR do get predicate-
tested (both fail NOT_AIR anyway via `isAir()`; folklore that this branch uses `isAir()` is wrong,
though behaviorally equivalent for the WG predicates).

`getOrCreateHeightmapUnprimed(t)` = `heightmaps.computeIfAbsent(t, t2 -> new Heightmap(this, t2))`
on the `Maps.newEnumMap(Types.class)` field.

---

## 6. `getHeightAccessorForGeneration` + upgrade interplay (assignment item 6)

```java
// ChunkAccess (base):
public LevelHeightAccessor getHeightAccessorForGeneration() { return this; }
public boolean isUpgrading() { return this.getBelowZeroRetrogen() != null; }
public BelowZeroRetrogen getBelowZeroRetrogen() { return null; }

// ProtoChunk override:
public LevelHeightAccessor getHeightAccessorForGeneration() {
    return this.isUpgrading() ? BelowZeroRetrogen.UPGRADE_HEIGHT_ACCESSOR : this;
}
public BelowZeroRetrogen getBelowZeroRetrogen() { return this.belowZeroRetrogen; }   // field
```

`BelowZeroRetrogen.UPGRADE_HEIGHT_ACCESSOR` (`BelowZeroRetrogen$1`, anonymous
LevelHeightAccessor): **`getHeight() = 64`, `getMinY() = −64`** — exactly the pre-1.18
below-zero band [−64, −1]. This is the accessor handed to `CarvingContext` (A1 §7.2), so for an
upgrading chunk `ctx.getMinGenY() = −64`, `ctx.getGenDepth() = 64` and A2 §2's y bounds become
`minY ≥ −63`, `maxY ≤ −64 + 64 − 1 − topPad` with `topPad = 0` (isUpgrading) `= −1`.
For normal generation (belowZeroRetrogen == null, always the case for fresh hyperchunk worlds):
accessor == the chunk itself, full height, `topPad = 7` (A2 §2 (4)).

---

## 7. `markPosForPostProcessing` — storage format (Task 9 handoff)

```java
public void markPosForPostProcessing(BlockPos pos) {
    if (this.isInsideBuildHeight(pos)) {                  // getMinY() <= y <= getMaxY(); else silently dropped
        ChunkAccess.getOrCreateOffsetList(this.postProcessing, this.getSectionIndex(pos.getY()))
                   .add(packOffsetCoordinates(pos));
    }
}

public static short packOffsetCoordinates(BlockPos pos) {
    return (short)((pos.getX() & 15) | ((pos.getY() & 15) << 4) | ((pos.getZ() & 15) << 8));
}
// inverse (promotion side):
public static BlockPos unpackOffsetCoordinates(short s, int sectionY, ChunkPos cp) {
    return new BlockPos(SectionPos.sectionToBlockCoord(cp.x(), s & 15),          // (cp.x<<4) + …
                        SectionPos.sectionToBlockCoord(sectionY, (s >>> 4) & 15),
                        SectionPos.sectionToBlockCoord(cp.z(), (s >>> 8) & 15));
}
```

- **Packing: x = bits 0–3, y = bits 4–7, z = bits 8–11** (12 bits used, per-section-relative).
  ⚠ Different layout from CarvingMask.getIndex (there z is the middle nibble).
- Storage: `ChunkAccess.postProcessing` = `ShortList[getSectionsCount()]` (24 sections
  overworld), lazily `new ShortArrayList()` per section via `getOrCreateOffsetList`
  (plain array-slot check, no locking). **Appends unconditionally — duplicates are kept.**
- Serialized as the `"PostProcessing"` list-of-lists in SerializableChunkData; drained at
  promotion to LevelChunk (fluid ticks — outside carving scope). During carving it is fed only
  by A2 §3 steps (7)/(8) (aquifer-fluid placements + fluid topMaterial), so hyperchunk's
  current no-op affects saved NBT `PostProcessing` + post-FULL fluid scheduling, never
  generation-time block placement. Task 9 (features) inherits the same lists.

---

## 8. 26.2 deltas vs 1.21 folklore

1. **Single `carvingMask` field** on ProtoChunk (field list §2) — no
   `Map<GenerationStep.Carving, CarvingMask>`; accessors take no step argument. Confirms A1 §6.
2. NBT: flat **`carving_mask`** LongArray; 1.21's `CarvingMasks{AIR,LIQUID}` compound removed,
   with `CarvingStepRemoveFix` datafixer as in-jar evidence (§1.5).
3. `ChunkAccess.setBlockState(pos, state)` 2-arg convenience delegating with **`int flags = 3`**;
   the 3-arg abstract takes `int flags` where 1.21 had `boolean isMoving`. ProtoChunk ignored
   both then and now (no `iload_3`).
4. `LevelHeightAccessor` renames: `getMinY/getMaxY/isInsideBuildHeight` (1.21:
   `getMinBuildHeight/getMaxBuildHeight`); `getMaxY` is now INCLUSIVE top (319), reflected in
   `isOutsideBuildHeight`'s `if_icmple`.
5. `Heightmap$Types` gained explicit `id` field + `BY_ID`/`STREAM_CODEC`; predicate assignments
   and Usage classes are semantically unchanged from 1.21. `OCEAN_FLOOR_WG`'s predicate is now
   literally a `BlockStateBase::blocksMotion` method reference (1.21 spelled it as a lambda) —
   same semantics.
6. `CarvingMask` class shape (ctor args, getIndex layout, additionalMask default-false lambda,
   stream/toArray) is unchanged vs 1.21 — folklore-safe, now bytecode-verified.
7. `Heightmap.update`/`primeHeightmaps`/`getFirstAvailable` — identical branch structure to the
   1.21-era shape, all constants re-verified (`fa−2` guard, `y−1` rescan start, minY-inclusive
   loop, `iter.back(n)`).
8. `heightmapsAfter` per-status sets unchanged: WG pair through SURFACE, FINAL quad from CARVERS
   onward — and the status flip to CARVERS happens **after** the task (§3.2), so carving updates
   the WG pair only.

---

## 9. sulfur scan (assignment item 9)

`strings <class> | grep -ci sulfur` over `CarvingMask.class`, `CarvingMask$Mask.class`,
`ProtoChunk.class`, `Heightmap.class`, `Heightmap$Types.class`, `Heightmap$Usage.class`:
**0 hits in every assigned class.** (Consistent with A1 §11 / A2 §10: sulfur exists only in
datapack biome/surface data, not in carver/chunk plumbing.)

---

## 10. OPEN items

1. `BlockStateBase.legacySolid` bake (`calculateSolid`) not re-derived here — the authority for
   per-state `blocksMotion` on the C side remains the surface-stage golden-validated block
   flags. If a future stage needs the bake rule itself, disassemble
   `BlockBehaviour$BlockStateBase.initCache`/`calculateSolid`.
2. `Blender`'s additional-mask filter (old-chunk blending) is installed via
   `setAdditionalMask` with `(localX, absoluteY, localZ)` semantics but its predicate internals
   were not reconstructed (fresh worlds: never installed — A1 §1.2).
3. `ImposterProtoChunk` (read-only wrapper over a FULL LevelChunk; relevant only to
   already-generated chunks) overrides this surface with no-ops/delegates — not reconstructed.
4. `SimpleBitStorage.get/set` word math not reproduced here (A7/prior stages own BitStorage);
   heightmap values fit 9 bits (overworld), 256 entries.
