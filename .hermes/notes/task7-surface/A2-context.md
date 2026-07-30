# A2: SurfaceRules$Context + lazy condition machinery (MC 26.2, unobfuscated server bytecode)

Source of truth: `javap -p -c -constants` against `/home/ubuntu/projects/hyperchunk/tools/golden/work/server`.
All pseudocode below is a 1:1 reconstruction of the bytecode. No vanilla-source guessing.

---

## 1. `SurfaceRules$Context` — class layout

```java
public final class SurfaceRules$Context {
    // static constants (from constant pool, -constants)
    private static final int HOW_FAR_BELOW_PRELIMINARY_SURFACE_LEVEL_TO_BUILD_SURFACE = 8;
    private static final int SURFACE_CELL_BITS = 4;
    private static final int SURFACE_CELL_SIZE = 16;
    private static final int SURFACE_CELL_MASK = 15;

    // immutable wiring
    private final SurfaceSystem system;
    private final SurfaceRules$Condition temperature;              // new TemperatureHelperCondition(this)
    private final SurfaceRules$Condition steep;                    // new SteepMaterialCondition(this)
    private final SurfaceRules$Condition hole;                     // new HoleCondition(this)
    private final SurfaceRules$Condition abovePreliminarySurface;  // new AbovePreliminarySurfaceCondition(this)
    private final RandomState randomState;
    private final ChunkAccess chunk;
    private final NoiseChunk noiseChunk;
    private final Function<BlockPos, Holder<Biome>> biomeGetter;
    private final WorldGenerationContext context;                  // stored, never read in THIS class's methods
    private final Set<Holder<Biome>> possibleBiomes;               // stored, never read in THIS class's methods

    // preliminary-surface cell cache
    private long lastPreliminarySurfaceCellOrigin;                 // init Long.MAX_VALUE = 9223372036854775807L
    private final int[] preliminarySurfaceCache;                   // new int[4]

    // NEW in 26.2: per-context noise sampler caches (IdentityHashMap! keyed on ResourceKey identity)
    private final Map<ResourceKey<NormalNoise$NoiseParameters>, DoubleSupplier> noiseSamplers2d; // new IdentityHashMap<>()
    private final Map<ResourceKey<NormalNoise$NoiseParameters>, DoubleSupplier> noiseSamplers3d; // new IdentityHashMap<>()

    // XZ-scoped state
    private long lastUpdateXZ;            // init -9223372036854775807L  (== Long.MIN_VALUE + 1)
    private int blockX;
    private int blockZ;
    private int surfaceDepth;             // EAGER: set in updateXZ
    private long lastSurfaceDepth2Update; // init lastUpdateXZ - 1  (== Long.MIN_VALUE)
    private double surfaceSecondary;      // LAZY
    private long lastMinSurfaceLevelUpdate; // init lastUpdateXZ - 1 (== Long.MIN_VALUE)
    private int minSurfaceLevel;          // LAZY

    // Y-scoped state
    private long lastUpdateY;             // init -9223372036854775807L (== Long.MIN_VALUE + 1)
    private final BlockPos$MutableBlockPos pos;   // new MutableBlockPos() — SHARED scratch pos
    private Holder<Biome> biome;          // LAZY, null sentinel (NOT a memoized Supplier)
    private int blockY;
    private int waterHeight;
    private int stoneDepthBelow;
    private int stoneDepthAbove;
}
```

### 1.1 Constructor (exact assignment order)

```java
protected Context(SurfaceSystem system, RandomState randomState, ChunkAccess chunk,
                  NoiseChunk noiseChunk, Function<BlockPos, Holder<Biome>> biomeGetter,
                  WorldGenerationContext context, Set<Holder<Biome>> possibleBiomes) {
    super();                                                        // Object.<init>
    this.temperature = new TemperatureHelperCondition(this);        // #14
    this.steep = new SteepMaterialCondition(this);                  // #21
    this.hole = new HoleCondition(this);                            // #27
    this.abovePreliminarySurface = new AbovePreliminarySurfaceCondition(this); // #33
    this.lastPreliminarySurfaceCellOrigin = 9223372036854775807L;   // Long.MAX_VALUE
    this.preliminarySurfaceCache = new int[4];
    this.noiseSamplers2d = new IdentityHashMap<>();
    this.noiseSamplers3d = new IdentityHashMap<>();
    this.lastUpdateXZ = -9223372036854775807L;                      // Long.MIN_VALUE + 1
    this.lastSurfaceDepth2Update = this.lastUpdateXZ - 1L;          // reads field just written
    this.lastMinSurfaceLevelUpdate = this.lastUpdateXZ - 1L;
    this.lastUpdateY = -9223372036854775807L;
    this.pos = new BlockPos.MutableBlockPos();
    this.system = system;
    this.randomState = randomState;
    this.chunk = chunk;
    this.noiseChunk = noiseChunk;
    this.biomeGetter = biomeGetter;
    this.context = context;
    this.possibleBiomes = possibleBiomes;
}
```

IMPORTANT ORDERING DETAIL: the four `new *Condition(this)` calls run FIRST, while `this.lastUpdateXZ` /
`this.lastUpdateY` are still 0 (default). `LazyCondition.<init>` sets `lastUpdate = getContextLastUpdate() - 1
= 0 - 1 = -1`. Since the constructor then sets `lastUpdateXZ = lastUpdateY = Long.MIN_VALUE+1`, and `updateXZ`
increments before any `test()`, `lastUpdate == -1` never equals the live counter until ~2^63 updates — so the
first `test()` always computes. Same non-issue for `Context$1/$2` samplers (created lazily, after counters are live).

### 1.2 `updateXZ(int x, int z)` — exact body

```java
protected void updateXZ(int x, int z) {
    ++this.lastUpdateXZ;      // getfield #60, ladd 1, putfield #60
    ++this.lastUpdateY;       // ALSO increments the Y counter (invalidates all Y-lazy state)
    this.blockX = x;
    this.blockZ = z;
    this.surfaceDepth = this.system.getSurfaceDepth(x, z);   // EAGER — SurfaceSystem.getSurfaceDepth:(II)I
}
```

- Eager: `surfaceDepth` only.
- Lazy (counter-invalidated, recomputed on demand): `surfaceSecondary`, `minSurfaceLevel`,
  all `LazyXZCondition`s (steep, hole), all 2d noise samplers.
- RNG/noise NOTE: `SurfaceSystem.getSurfaceDepth(II)I` is a call into another class (contains the
  surface-noise + positional-random sampling); not in this cluster — see A1/SurfaceSystem report.

### 1.3 `updateY(int stoneDepthAbove, int stoneDepthBelow, int waterHeight, int blockY)` — exact body

Bytecode maps params: local1→stoneDepthAbove, local2→stoneDepthBelow, local3→waterHeight, local4→blockY.
Assignment order (matters only for exception-free code, but reproduced exactly):

```java
protected void updateY(int stoneDepthAbove, int stoneDepthBelow, int waterHeight, int blockY) {
    ++this.lastUpdateY;
    this.biome = null;                    // invalidate lazy biome (null sentinel, NOT Suppliers.memoize)
    this.blockY = blockY;                 // local 4
    this.waterHeight = waterHeight;       // local 3
    this.stoneDepthBelow = stoneDepthBelow; // local 2
    this.stoneDepthAbove = stoneDepthAbove; // local 1
}
```

DIFF vs 1.21: signature is 4 ints (1.21 had 6: ..., blockX, blockY, blockZ) and biome is a plain
nullable field instead of `Suppliers.memoize(...)`. blockX/blockZ come from `updateXZ` state.

### 1.4 `getSurfaceSecondary()` — lazy, keyed on lastUpdateXZ

```java
protected double getSurfaceSecondary() {
    if (this.lastSurfaceDepth2Update != this.lastUpdateXZ) {   // lcmp ifeq -> skip when EQUAL
        this.lastSurfaceDepth2Update = this.lastUpdateXZ;      // stamp BEFORE compute
        this.surfaceSecondary = this.system.getSurfaceSecondary(this.blockX, this.blockZ); // (II)D
    }
    return this.surfaceSecondary;
}
```

### 1.5 `getBiome()` — lazy, null-sentinel (invalidated by updateY)

```java
protected Holder<Biome> getBiome() {
    if (this.biome == null) {
        this.biome = (Holder<Biome>) this.biomeGetter.apply(
            this.pos.set(this.blockX, this.blockY, this.blockZ));   // MutableBlockPos.set(III)
    }
    return this.biome;
}
```

Side effect: mutates the shared `pos` field to (blockX, blockY, blockZ).

### 1.6 `getSeaLevel()` — NEW vs 1.21

```java
public int getSeaLevel() { return this.system.getSeaLevel(); }   // SurfaceSystem.getSeaLevel:()I
```

### 1.7 Cell coordinate helpers

```java
private static int blockCoordToSurfaceCell(int c) { return c >> 4; }   // ishr — ARITHMETIC shift (floor div 16)
private static int surfaceCellToBlockCoord(int c) { return c << 4; }   // ishl
```

### 1.8 `getMinSurfaceLevel()` — the preliminary-surface bilinear machinery

Two-level cache: outer keyed on `lastUpdateXZ` (per-column), inner keyed on the packed cell origin
(per 16x16 cell — the 4 corner `preliminarySurfaceLevel` calls are only redone when the column crosses
into a new cell).

```java
protected int getMinSurfaceLevel() {
    if (this.lastMinSurfaceLevelUpdate != this.lastUpdateXZ) {
        this.lastMinSurfaceLevelUpdate = this.lastUpdateXZ;                 // stamp before compute
        int cellX = blockCoordToSurfaceCell(this.blockX);                   // blockX >> 4
        int cellZ = blockCoordToSurfaceCell(this.blockZ);                   // blockZ >> 4
        long key = ChunkPos.pack(cellX, cellZ);                             // ChunkPos.pack:(II)J  [1.21 name: asLong]
        if (this.lastPreliminarySurfaceCellOrigin != key) {
            this.lastPreliminarySurfaceCellOrigin = key;
            // exact corner order and coords (block coords = cell << 4):
            this.preliminarySurfaceCache[0] = this.noiseChunk.preliminarySurfaceLevel( cellX      << 4,  cellZ      << 4);
            this.preliminarySurfaceCache[1] = this.noiseChunk.preliminarySurfaceLevel((cellX + 1) << 4,  cellZ      << 4);
            this.preliminarySurfaceCache[2] = this.noiseChunk.preliminarySurfaceLevel( cellX      << 4, (cellZ + 1) << 4);
            this.preliminarySurfaceCache[3] = this.noiseChunk.preliminarySurfaceLevel((cellX + 1) << 4, (cellZ + 1) << 4);
        }
        int lerped = Mth.floor(Mth.lerp2(
            (double)((float)(this.blockX & 15) / 16.0f),   // tx: int -> i2f -> fdiv 16.0f -> f2d
            (double)((float)(this.blockZ & 15) / 16.0f),   // tz: same float-then-widen path
            (double)this.preliminarySurfaceCache[0],       // c00  (x0,z0)
            (double)this.preliminarySurfaceCache[1],       // c10  (x1,z0)
            (double)this.preliminarySurfaceCache[2],       // c01  (x0,z1)
            (double)this.preliminarySurfaceCache[3]));     // c11  (x1,z1)
        this.minSurfaceLevel = lerped + this.surfaceDepth - 8;   // iadd, bipush 8, isub
    }
    return this.minSurfaceLevel;
}
```

Precision notes for the C port:
- The interpolation fractions are computed in FLOAT (`(blockX & 15)` is 0..15, `i2f`, `fdiv 16.0f`)
  then widened to double. Since n/16 for n in 0..15 is exact in binary float, `(double)((float)n/16.0f)
  == n/16.0` exactly — but replicate the float path anyway to be safe.
- `Mth.lerp2:(DDDDDD)D` argument order is `(tx, tz, c00, c10, c01, c11)` — standard vanilla lerp2:
  `lerp(tz, lerp(tx, c00, c10), lerp(tx, c01, c11))` (signature only; Mth not deep-dived here).
- `Mth.floor:(D)I` then INT arithmetic: `+ surfaceDepth - 8` (8 = HOW_FAR_BELOW_PRELIMINARY_SURFACE_LEVEL_TO_BUILD_SURFACE).
- `surfaceDepth` used here is the EAGER value from `updateXZ` — so `getMinSurfaceLevel` depends on
  `updateXZ` having run for this column. `ChunkPos.pack:(II)J` (renamed from `asLong`) is the cell cache key;
  sentinel `Long.MAX_VALUE` guarantees first call misses.
- Because `blockCoordToSurfaceCell` is `>> 4` (arithmetic), negative coords floor correctly:
  blockX = -1 → cell -1 → corners at -16 and 0.

### 1.9 `getNoiseSampler(ResourceKey<NormalNoise$NoiseParameters> key, boolean is3d)` — NEW in 26.2

```java
protected DoubleSupplier getNoiseSampler(ResourceKey<NormalNoise.NoiseParameters> key, boolean is3d) {
    if (is3d) {
        return this.noiseSamplers3d.computeIfAbsent(key, this::createNoiseSampler3d);  // indy #0 -> createNoiseSampler3d
    } else {
        return this.noiseSamplers2d.computeIfAbsent(key, this::createNoiseSampler2d);  // indy #1 -> createNoiseSampler2d
    }
}
```

Bootstrap methods verified: InvokeDynamic #0 → `REF_invokeVirtual Context.createNoiseSampler3d`,
InvokeDynamic #1 → `REF_invokeVirtual Context.createNoiseSampler2d`.

Cache maps are `IdentityHashMap` — keyed on ResourceKey REFERENCE identity (ResourceKeys are interned
in vanilla, so this works; a C port should key on the resolved noise parameter identity).

```java
private DoubleSupplier createNoiseSampler2d(ResourceKey<NormalNoise.NoiseParameters> key) {
    NormalNoise noise = this.randomState.getOrCreateNoise(key);  // RandomState.getOrCreateNoise:(ResourceKey;)NormalNoise
    return new Context$1(this, noise);
}
private DoubleSupplier createNoiseSampler3d(ResourceKey<NormalNoise.NoiseParameters> key) {
    NormalNoise noise = this.randomState.getOrCreateNoise(key);
    return new Context$2(this, noise);
}
```

Noise instantiation point: `RandomState.getOrCreateNoise(ResourceKey)` — in vanilla this forks the
positional random with the noise parameters' registry name string (`fromHashOf(location)`), but that
happens inside RandomState, NOT here. No direct RNG consumption anywhere in this cluster.

---

## 2. `SurfaceRules$Context$1` — cached 2D noise sampler (NOT a stub context)

```java
class Context$1 implements DoubleSupplier {
    private long lastUpdateXZ;              // = this$0.lastUpdateXZ - 1 at construction (forces first compute)
    private double lastNoise;
    final NormalNoise val$noise;
    final Context this$0;

    public double getAsDouble() {
        if (this.lastUpdateXZ != this$0.lastUpdateXZ) {
            this.lastNoise = val$noise.getValue((double)this$0.blockX, 0.0, (double)this$0.blockZ);
            this.lastUpdateXZ = this$0.lastUpdateXZ;   // stamp AFTER compute (unlike Context's stamp-first)
        }
        return this.lastNoise;
    }
}
```

- Sample point: `getValue(blockX, 0.0, blockZ)` — y is literal `dconst_0` = 0.0.
- Memo keyed on `lastUpdateXZ`: one sample per column update.

## 3. `SurfaceRules$Context$2` — cached 3D noise sampler

```java
class Context$2 implements DoubleSupplier {
    private long lastUpdateY;               // = this$0.lastUpdateY - 1 at construction
    private double lastNoise;
    final NormalNoise val$noise;
    final Context this$0;

    public double getAsDouble() {
        if (this.lastUpdateY != this$0.lastUpdateY) {
            this.lastNoise = val$noise.getValue((double)this$0.blockX, (double)this$0.blockY, (double)this$0.blockZ);
            this.lastUpdateY = this$0.lastUpdateY;     // stamp AFTER compute
        }
        return this.lastNoise;
    }
}
```

- Sample point: `getValue(blockX, blockY, blockZ)`. Memo keyed on `lastUpdateY`
  (which `updateXZ` ALSO bumps, so a new column always invalidates 3d samplers too).

---

## 4. `LazyCondition` / `LazyXZCondition` / `LazyYCondition` — memoization protocol

```java
abstract class LazyCondition implements SurfaceRules$Condition {
    protected final Context context;
    private long lastUpdate;      // = getContextLastUpdate() - 1 at construction
    private Boolean result;       // boxed; null until first compute

    protected LazyCondition(Context ctx) {
        this.context = ctx;
        this.lastUpdate = this.getContextLastUpdate() - 1L;
    }

    public boolean test() {
        long now = this.getContextLastUpdate();
        if (now == this.lastUpdate) {                        // lcmp ifne -> recompute branch
            if (this.result == null)
                throw new IllegalStateException("Update triggered but the result is null");
            return this.result.booleanValue();
        } else {
            this.lastUpdate = now;                           // stamp BEFORE compute
            this.result = Boolean.valueOf(this.compute());
            return this.result.booleanValue();
        }
    }

    protected abstract long getContextLastUpdate();
    protected abstract boolean compute();
}

abstract class LazyXZCondition extends LazyCondition {
    protected long getContextLastUpdate() { return this.context.lastUpdateXZ; }
}
abstract class LazyYCondition extends LazyCondition {
    protected long getContextLastUpdate() { return this.context.lastUpdateY; }
}
```

Protocol: compute() runs at most once per counter tick; equality (`==`) comparison, stamp written
before compute (a compute() that recursively calls test() on itself would return the stale/null result —
does not occur in practice).

---

## 5. `Context$HoleCondition` (LazyXZ)

```java
protected boolean compute() {
    return this.context.surfaceDepth <= 0;    // ifgt -> false; i.e. TRUE iff surfaceDepth <= 0
}
```

## 6. `Context$AbovePreliminarySurfaceCondition` (NOT lazy — direct Condition)

```java
public boolean test() {
    return this.context.blockY >= this.context.getMinSurfaceLevel();   // if_icmplt -> false; >= is exact
}
```

No memoization of its own; relies on getMinSurfaceLevel()'s internal cache.

## 7. `Context$SteepMaterialCondition` (LazyXZ)

```java
protected boolean compute() {
    int x = this.context.blockX & 15;                       // local chunk coords
    int z = this.context.blockZ & 15;
    int zN = Math.max(z - 1, 0);                            // clamp to chunk edge (NO neighbor chunk reads)
    int zS = Math.min(z + 1, 15);
    ChunkAccess chunk = this.context.chunk;
    int hNorth = chunk.getHeight(Heightmap.Types.WORLD_SURFACE_WG, x, zN);   // (Types, x, z) -> int
    int hSouth = chunk.getHeight(Heightmap.Types.WORLD_SURFACE_WG, x, zS);
    if (hSouth >= hNorth + 4) {                             // if_icmplt 85 -> fall through when <
        return true;                                        // terrain rises toward +Z: north-facing slope
    }
    int xW = Math.max(x - 1, 0);
    int xE = Math.min(x + 1, 15);
    int hWest = chunk.getHeight(Heightmap.Types.WORLD_SURFACE_WG, xW, z);
    int hEast = chunk.getHeight(Heightmap.Types.WORLD_SURFACE_WG, xE, z);
    return hWest >= hEast + 4;                              // terrain rises toward -X: east-facing slope
}
```

Exact facts:
- Heightmap type: `WORLD_SURFACE_WG`.
- Threshold: `+ 4`, comparison `>=` in both axes.
- ASYMMETRY (sign convention): Z-axis test is `h(z+1) >= h(z-1) + 4` (steeper going SOUTH),
  X-axis test is `h(x-1) >= h(x+1) + 4` (steeper going WEST). Directions are opposite between axes.
- Neighbor offsets are CLAMPED to [0,15] within the current chunk (edge columns compare against
  themselves on one side, halving the effective gradient there).
- `chunk.getHeight` returns the heightmap value (in vanilla: highest blocked Y + ... — signature only,
  `ChunkAccess.getHeight:(Lnet/minecraft/world/level/levelgen/Heightmap$Types;II)I`, not deep-dived).

## 8. `Context$TemperatureHelperCondition` (LazyYCondition — per-Y memo!)

```java
protected boolean compute() {
    return ((Biome)this.context.getBiome().value())
        .coldEnoughToSnow(
            this.context.pos.set(this.context.blockX, this.context.blockY, this.context.blockZ),
            this.context.getSeaLevel());
}
```

- Method called: `Biome.coldEnoughToSnow:(Lnet/minecraft/core/BlockPos;I)Z` — TWO-ARG form
  (BlockPos, int seaLevel). The int is `Context.getSeaLevel()` = `SurfaceSystem.getSeaLevel()`.
- BlockPos: the shared mutable `pos` set to exactly (blockX, blockY, blockZ) — the current block,
  not a heightmap-projected position.
- `getBiome()` may itself set `pos` first (same coords) — order: getBiome() resolves biome (setting pos),
  then pos is set again, then coldEnoughToSnow is invoked. Net effect identical.

---

## 9. Codecs / JSON

None of the classes in this cluster contain CODEC static init or RecordCodecBuilder strings.
(The ConditionSource records — Steep/Hole/Temperature/AbovePreliminarySurface — live in sibling classes
`SurfaceRules$Steep` etc., out of scope for A2.)

## 10. RNG consumption points

- NONE directly in this cluster. No nextDouble/nextInt/forkPositional/fromHashOf calls appear in any
  method here.
- Indirect: `RandomState.getOrCreateNoise(ResourceKey)` (called once per distinct noise key from
  createNoiseSampler2d/3d) and `SurfaceSystem.getSurfaceDepth(II)I` / `getSurfaceSecondary(II)D`
  (called from updateXZ / getSurfaceSecondary) contain the actual noise/positional-random usage —
  covered by the SurfaceSystem report.
- `NormalNoise.getValue(DDD)D` is stateless sampling.

## 11. NEW vs Minecraft 1.21 (bytecode-observed deltas)

1. **`getNoiseSampler(key, boolean is3d)` + `noiseSamplers2d/3d` IdentityHashMaps + `Context$1/$2`
   DoubleSupplier caches are NEW.** `Context$1`/`Context$2` are NOT stub contexts — they are memoizing
   noise samplers (2d keyed on lastUpdateXZ sampling at y=0.0; 3d keyed on lastUpdateY sampling at
   blockY). The 3d variant is entirely new machinery (1.21 noise_threshold conditions were 2d-only,
   sampled uncached inside the condition) — almost certainly serving sulfur-cave-era 3D surface rules.
2. **`getSeaLevel()` on Context delegating to `SurfaceSystem.getSeaLevel()`** — new plumbing.
3. **`Biome.coldEnoughToSnow(BlockPos, int seaLevel)`** — 2-arg signature (1.21: 1-arg).
4. **`updateY` takes 4 ints** (stoneDepthAbove, stoneDepthBelow, waterHeight, blockY) — no blockX/Y/Z
   triple; **biome is a plain null-sentinel field**, not `Suppliers.memoize`.
5. **`ChunkPos.pack(II)J`** — rename of `ChunkPos.asLong` in these mappings.
6. Core math unchanged vs 1.21: cell bits 4, 4-corner preliminarySurfaceLevel, float-fraction lerp2,
   `floor(lerp2) + surfaceDepth - 8`, steep threshold `>= +4` on WORLD_SURFACE_WG with clamped
   in-chunk neighbors.

## 12. Ambiguities

- `Context$1/$2` constructors show `putfield val$noise` from `aload_2` and `this$0` from `aload_1`
  BEFORE `Object.<init>` — this is standard javac nest/captured-var ordering, no semantic ambiguity.
  Confidence: certain.
- `Mth.lerp2(DDDDDD)D` corner-argument semantics (c00,c10,c01,c11 with tx-then-tz nesting) are taken
  from the call-site ORDER only; Mth itself was not disassembled here (out of cluster). The corner
  coords passed to `preliminarySurfaceLevel` are unambiguous (quoted above). Confidence: high for
  order, verify Mth.lerp2 body in its own pass.
- All other methods: bytecode fully linear, zero ambiguity.
