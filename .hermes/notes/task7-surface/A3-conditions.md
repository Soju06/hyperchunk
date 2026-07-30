# A3 — SurfaceRules condition sources (JSON-facing), MC 26.2 bytecode reconstruction

Source of truth: `javap -p -c -constants` on the extracted, unobfuscated 26.2 server classes in
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server`. Nothing below is guessed from vanilla
source memory; where I interpret ambiguous bytecode I quote it and give confidence.

Class-name note: 26.2 uses `net.minecraft.resources.Identifier` (the class formerly known as
`ResourceLocation` in 1.21). All `Mth` helpers used here were disassembled and are listed in §0.

---

## 0. Shared helpers (exact math, from bytecode)

### 0.1 `Mth` (net.minecraft.util.Mth)

```java
// bytecode: dsub, ddiv
static double inverseLerp(double v, double a, double b) { return (v - a) / (b - a); }

// bytecode: dsub, dmul, dadd  ->  a + t*(b-a)
static double lerp(double t, double a, double b) { return a + t * (b - a); }

// map = lerp(inverseLerp(value, from0, from1), to0, to1)   -- NOT clamped
static double map(double value, double from0, double from1, double to0, double to1) {
    return lerp(inverseLerp(value, from0, from1), to0, to1);
}

// lerp2(a, b, c, d, e, f) = lerp(b, lerp(a, c, d), lerp(a, e, f))
// (verified by stack trace of dloads: slot2 pushed first, then lerp(slot0,slot4,slot6),
//  then lerp(slot0,slot8,slot10), outer lerp consumes (b, inner1, inner2))
static double lerp2(double a, double b, double c, double d, double e, double f) {
    return lerp(b, lerp(a, c, d), lerp(a, e, f));
}

static int floor(double d) { return (int)Math.floor(d); }   // d2i after Math.floor
```

### 0.2 `LazyCondition` family (SurfaceRules$LazyCondition / $LazyXZCondition / $LazyYCondition)

```java
abstract class LazyCondition implements Condition {
    protected final Context context;
    private long lastUpdate;
    private Boolean result;
    protected LazyCondition(Context ctx) { this.context = ctx; this.lastUpdate = getContextLastUpdate() - 1L; }
    public boolean test() {
        long now = getContextLastUpdate();
        if (now == this.lastUpdate) {
            if (this.result == null) throw new IllegalStateException("Update triggered but the result is null");
            return this.result;
        }
        this.lastUpdate = now;
        this.result = Boolean.valueOf(compute());
        return this.result;
    }
}
// LazyXZCondition.getContextLastUpdate() -> context.lastUpdateXZ
// LazyYCondition.getContextLastUpdate()  -> context.lastUpdateY
```

### 0.3 `Context` update counters and fields (field-order-exact where stateful)

Constructor initial values: `lastPreliminarySurfaceCellOrigin = Long.MAX_VALUE (9223372036854775807L)`,
`preliminarySurfaceCache = new int[4]`, `noiseSamplers2d = new IdentityHashMap()`,
`noiseSamplers3d = new IdentityHashMap()`, `lastUpdateXZ = -9223372036854775807L` (== `Long.MIN_VALUE + 1`,
NOT `Long.MIN_VALUE`), `lastSurfaceDepth2Update = lastUpdateXZ - 1L`,
`lastMinSurfaceLevelUpdate = lastUpdateXZ - 1L`, `lastUpdateY = -9223372036854775807L`.

```java
// exact write order matters:
protected void updateXZ(int x, int z) {
    ++this.lastUpdateXZ;
    ++this.lastUpdateY;
    this.blockX = x;
    this.blockZ = z;
    this.surfaceDepth = this.system.getSurfaceDepth(x, z);   // SurfaceSystem.getSurfaceDepth(II)I
}

protected void updateY(int stoneDepthAbove, int stoneDepthBelow, int waterHeight, int y) {
    ++this.lastUpdateY;
    this.biome = null;
    this.blockY = y;               // param 4
    this.waterHeight = waterHeight; // param 3
    this.stoneDepthBelow = stoneDepthBelow; // param 2
    this.stoneDepthAbove = stoneDepthAbove; // param 1
}
```

(Parameter meaning inferred from field names only; the *store order* is param4→blockY, param3→waterHeight,
param2→stoneDepthBelow, param1→stoneDepthAbove. Confidence in the name/param pairing: high — putfield
names are unobfuscated.)

```java
protected double getSurfaceSecondary() {   // cached per updateXZ
    if (this.lastSurfaceDepth2Update != this.lastUpdateXZ) {
        this.lastSurfaceDepth2Update = this.lastUpdateXZ;
        this.surfaceSecondary = this.system.getSurfaceSecondary(this.blockX, this.blockZ); // (II)D
    }
    return this.surfaceSecondary;
}

protected Holder<Biome> getBiome() {       // cached per updateY via biome=null reset
    if (this.biome == null) {
        this.biome = this.biomeGetter.apply(this.pos.set(this.blockX, this.blockY, this.blockZ));
    }
    return this.biome;
}

public int getSeaLevel() { return this.system.getSeaLevel(); }
```

### 0.4 `Context.getMinSurfaceLevel()` — used by `above_preliminary_surface`

Constants: `HOW_FAR_BELOW_PRELIMINARY_SURFACE_LEVEL_TO_BUILD_SURFACE = 8`, `SURFACE_CELL_BITS = 4`,
`SURFACE_CELL_SIZE = 16`, `SURFACE_CELL_MASK = 15`.

```java
private static int blockCoordToSurfaceCell(int c) { return c >> 4; }   // arithmetic shift (ishr)
private static int surfaceCellToBlockCoord(int c) { return c << 4; }

protected int getMinSurfaceLevel() {       // cached per updateXZ
    if (this.lastMinSurfaceLevelUpdate != this.lastUpdateXZ) {
        this.lastMinSurfaceLevelUpdate = this.lastUpdateXZ;
        int cellX = blockX >> 4;
        int cellZ = blockZ >> 4;
        long packed = ChunkPos.pack(cellX, cellZ);
        // ChunkPos.pack(a, b) = ((long)a & 4294967295L) | (((long)b & 4294967295L) << 32)
        if (this.lastPreliminarySurfaceCellOrigin != packed) {
            this.lastPreliminarySurfaceCellOrigin = packed;
            preliminarySurfaceCache[0] = noiseChunk.preliminarySurfaceLevel( cellX      << 4,  cellZ      << 4);
            preliminarySurfaceCache[1] = noiseChunk.preliminarySurfaceLevel((cellX + 1) << 4,  cellZ      << 4);
            preliminarySurfaceCache[2] = noiseChunk.preliminarySurfaceLevel( cellX      << 4, (cellZ + 1) << 4);
            preliminarySurfaceCache[3] = noiseChunk.preliminarySurfaceLevel((cellX + 1) << 4, (cellZ + 1) << 4);
        }
        // NOTE float division then widen: ((float)(blockX & 15)) / 16.0f  -> f2d
        double tx = (double)((float)(blockX & 15) / 16.0F);
        double tz = (double)((float)(blockZ & 15) / 16.0F);
        int surfLevel = Mth.floor(Mth.lerp2(tx, tz,
                (double)preliminarySurfaceCache[0], (double)preliminarySurfaceCache[1],
                (double)preliminarySurfaceCache[2], (double)preliminarySurfaceCache[3]));
        // = floor(lerp(tz, lerp(tx, c0, c1), lerp(tx, c2, c3)))
        this.minSurfaceLevel = surfLevel + this.surfaceDepth - 8;
    }
    return this.minSurfaceLevel;
}
```

External call noted, not deep-dived: `NoiseChunk.preliminarySurfaceLevel(II)I`.

### 0.5 `Context.getNoiseSampler` (NEW plumbing for `noise_threshold`)

```java
protected DoubleSupplier getNoiseSampler(ResourceKey<NoiseParameters> key, boolean is3d) {
    return is3d ? noiseSamplers3d.computeIfAbsent(key, this::createNoiseSampler3d)
                : noiseSamplers2d.computeIfAbsent(key, this::createNoiseSampler2d);
    // both maps are IdentityHashMap (ResourceKeys are interned)
}

private DoubleSupplier createNoiseSampler2d(ResourceKey<NoiseParameters> key) {
    NormalNoise noise = this.randomState.getOrCreateNoise(key);
    return new DoubleSupplier() {          // Context$1
        private long lastUpdateXZ = Context.this.lastUpdateXZ - 1L;
        private double lastNoise;
        public double getAsDouble() {
            if (this.lastUpdateXZ != Context.this.lastUpdateXZ) {
                this.lastNoise = noise.getValue((double)Context.this.blockX, 0.0, (double)Context.this.blockZ);
                this.lastUpdateXZ = Context.this.lastUpdateXZ;   // field write AFTER noise sample
            }
            return this.lastNoise;
        }
    };
}

private DoubleSupplier createNoiseSampler3d(ResourceKey<NoiseParameters> key) {
    NormalNoise noise = this.randomState.getOrCreateNoise(key);
    return new DoubleSupplier() {          // Context$2
        private long lastUpdateY = Context.this.lastUpdateY - 1L;
        private double lastNoise;
        public double getAsDouble() {
            if (this.lastUpdateY != Context.this.lastUpdateY) {
                this.lastNoise = noise.getValue((double)Context.this.blockX, (double)Context.this.blockY, (double)Context.this.blockZ);
                this.lastUpdateY = Context.this.lastUpdateY;
            }
            return this.lastNoise;
        }
    };
}
```

RNG/noise instantiation point: `RandomState.getOrCreateNoise(key)` →
`Noises.instantiate(noises, this.random, key)` (cached in RandomState map; key is the JSON `noise` ResourceKey).

---

## 1. JSON type registration (`SurfaceRules$ConditionSource.bootstrap`)

Registered names, in bytecode order (all under the `worldgen/material_condition` registry via
`SurfaceRules.register(Registry, String, MapCodec)`):

| name | class |
|---|---|
| `biome` | BiomeConditionSource |
| `noise_threshold` | NoiseThresholdConditionSource |
| `vertical_gradient` | VerticalGradientConditionSource |
| `y_above` | YConditionSource |
| `water` | WaterConditionSource |
| `temperature` | Temperature (unit codec) |
| `steep` | Steep (unit codec) |
| `not` | NotConditionSource |
| `hole` | Hole (unit codec) |
| `above_preliminary_surface` | AbovePreliminarySurface (unit codec) |
| `stone_depth` | StoneDepthCheck |

Dispatch: `Registry.byNameCodec().dispatch(...)` — standard `"type"` dispatch.
(The register-call ↔ codec pairing follows textual order of getstatics between the `ldc` string and
each `register` call; the strings themselves are verbatim from the constant pool. Confidence: high.)

Builder-API constants (in `SurfaceRules` static init — useful for cross-checking vanilla JSON):
`ON_FLOOR = stoneDepthCheck(0,false,FLOOR)`, `UNDER_FLOOR = stoneDepthCheck(0,true,FLOOR)`,
`DEEP_UNDER_FLOOR = stoneDepthCheck(0,true,6,FLOOR)`, `VERY_DEEP_UNDER_FLOOR = stoneDepthCheck(0,true,30,FLOOR)`,
`ON_CEILING = stoneDepthCheck(0,false,CEILING)`, `UNDER_CEILING = stoneDepthCheck(0,true,CEILING)`
(3-arg overload passes `secondaryDepthRange = 0`).

---

## 2. BiomeConditionSource + $1BiomeCondition (`biome`)

### (a) Codec
Record field: `biomes : HolderSet<Biome>`.
JSON: `"biome_is"` — `RegistryCodecs.homogeneousList(Registries.BIOME).fieldOf("biome_is")`. Required, no default.

### (b) apply(Context) — NEW constant-folding fast path (not in 1.21)
```java
public Condition apply(Context ctx) {
    if (ctx.possibleBiomes != null) {                 // Set<Holder<Biome>>; @Nullable
        if (canNeverMatch(ctx.possibleBiomes))  return () -> false;  // lambda$apply$0
        if (willAlwaysMatch(ctx.possibleBiomes)) return () -> true;  // lambda$apply$1
    }
    return new BiomeCondition(this, ctx);             // LazyYCondition
}

private boolean canNeverMatch(Set<Holder<Biome>> possible) {
    for (Holder<Biome> h : this.biomes)               // iterate the rule's HolderSet
        if (possible.contains(h)) return false;       // Set.contains(Object)
    return true;
}

private boolean willAlwaysMatch(Set<Holder<Biome>> possible) {
    for (Holder<Biome> h : possible)                  // iterate the chunk's possible biomes
        if (!this.biomes.contains(h)) return false;   // HolderSet.contains(Holder)
    return true;
}
```

### (c) test body (`$1BiomeCondition extends LazyYCondition`)
```java
protected boolean compute() {
    return BiomeConditionSource.this.biomes.contains(context.getBiome());  // HolderSet.contains(Holder<Biome>)
}
```
Storage/comparison is **Holder identity within a HolderSet** (1.21 stored a `Set<ResourceKey<Biome>>` +
predicate `holder.is(key)`); the `possibleBiomes` short-circuit is also new. For a C port: per-biome-id
bitset is equivalent as long as holders map 1:1 to registry ids.

---

## 3. NoiseThresholdConditionSource + $1NoiseThresholdCondition (`noise_threshold`)

### (a) Codec
Record fields: `noise : ResourceKey<NoiseParameters>`, `minThreshold : double`, `maxThreshold : double`,
`is3d : boolean`.

JSON fields:
- `"noise"` — `ResourceKey.codec(Registries.NOISE)`, required
- `"min_threshold"` — `Codec.DOUBLE`, required
- `"max_threshold"` — `Codec.DOUBLE`, required
- `"is_3d"` — `Codec.BOOL.optionalFieldOf("is_3d", Boolean.valueOf(false))` → **optional, default `false`** — **NEW field vs 1.21**

Builder helpers: `noiseCondition2d(key, min)` / `noiseCondition3d(key, min)` use
`maxThreshold = 1.7976931348623157E308` (`Double.MAX_VALUE`), and is3d = 0 / 1 respectively.

### (b) apply(Context)
```java
public Condition apply(Context ctx) {
    DoubleSupplier sampler = ctx.getNoiseSampler(this.noise, this.is3d);   // see §0.5
    return new NoiseThresholdCondition(this, sampler);
}
```
**The condition is a plain `Condition`, NOT a Lazy(XZ/Y)Condition** — result caching lives inside the
shared per-key DoubleSupplier (2d: keyed to lastUpdateXZ, sampled at `(x, 0.0, z)`; 3d: keyed to
lastUpdateY, sampled at `(x, y, z)`). Two rules referencing the same noise key share one supplier
(IdentityHashMap per Context), so the noise is evaluated at most once per column step.

### (c) test body — comparison operators
```java
public boolean test() {
    double v = sampler.getAsDouble();
    return v >= this.minThreshold && v <= this.maxThreshold;   // BOTH INCLUSIVE
}
```
Raw bytecode for the comparisons (NaN behavior):
```
18: dcmpl        // v vs minThreshold ; NaN -> -1
19: iflt  38     // false if v < min (or NaN)
30: dcmpg        // v vs maxThreshold ; NaN -> +1
31: ifgt  38     // false if v > max (or NaN)
```
So NaN fails both ways → `false`. Inclusive `>= min`, `<= max`.

Noise getter chain: `Context.getNoiseSampler` → `RandomState.getOrCreateNoise(ResourceKey)` →
`Noises.instantiate(HolderGetter, PositionalRandomFactory random, key)` (RandomState's base `random`
factory; the string used for the noise's own `fromHashOf` lives inside `Noises.instantiate` — outside
this cluster, noted only).

---

## 4. VerticalGradientConditionSource + $1VerticalGradientCondition (`vertical_gradient`)

### (a) Codec
Record fields: `randomName : Identifier`, `trueAtAndBelow : VerticalAnchor`, `falseAtAndAbove : VerticalAnchor`.

JSON fields (all required, no defaults):
- `"random_name"` — `Identifier.CODEC`
- `"true_at_and_below"` — `VerticalAnchor.CODEC`
- `"false_at_and_above"` — `VerticalAnchor.CODEC`

Builder helper `SurfaceRules.verticalGradient(String, ...)` calls `Identifier.parse(name)`.

### (b) apply(Context) — anchor resolution + RNG factory creation happen HERE (once per chunk-context)
```java
public Condition apply(Context ctx) {
    int trueY  = this.trueAtAndBelow.resolveY(ctx.context);   // WorldGenerationContext
    int falseY = this.falseAtAndAbove.resolveY(ctx.context);
    PositionalRandomFactory factory = ctx.randomState.getOrCreateRandomFactory(this.randomName);
    return new VerticalGradientCondition(this, ctx, trueY, falseY, factory);  // LazyYCondition
}
```

**RNG key derivation** (from `RandomState` bytecode):
```java
public PositionalRandomFactory getOrCreateRandomFactory(Identifier id) {
    return positionalRandoms.computeIfAbsent(id,
        id2 -> this.random.fromHashOf(id).forkPositional());
}
// PositionalRandomFactory default method:
// fromHashOf(Identifier id) { return fromHashOf(id.toString()); }   // e.g. "minecraft:bedrock_floor"
```
i.e. **key string = the JSON `random_name` Identifier rendered as `namespace:path`**, hashed via
`fromHashOf(String)` on the RandomState's base positional factory, then `forkPositional()` → cached per
Identifier in `RandomState.positionalRandoms`.

### (c) compute() — exact math and comparisons
```java
protected boolean compute() {
    int y = context.blockY;
    if (y <= trueAtAndBelow) return true;    // if_icmpgt 18 -> fallthrough true when y <= trueY (INCLUSIVE)
    if (y >= falseAtAndAbove) return false;  // if_icmplt 28 -> fallthrough false when y >= falseY (INCLUSIVE)
    double gradient = Mth.map((double)y, (double)trueAtAndBelow, (double)falseAtAndAbove, 1.0, 0.0);
    // NOT clampedMap — plain Mth.map; early-outs above guarantee trueY < y < falseY
    RandomSource rs = randomFactory.at(context.blockX, y, context.blockZ);   // at(x, y, z) — arg order X, Y, Z
    return (double)rs.nextFloat() < gradient;    // nextFloat (NOT nextDouble), widened f2d, STRICT <
}
```
Raw compare bytecode: `74: nextFloat ... 79: f2d  80: dload_2  81: dcmpg  82: ifge 89` → returns
false when `(double)nextFloat >= gradient` (or NaN); true iff strictly less.

RNG consumption: exactly **one `nextFloat()`** from `factory.at(blockX, blockY, blockZ)` and only when
`trueY < blockY < falseY`. `Mth.map(y, trueY, falseY, 1.0, 0.0)` expands to
`1.0 + ((y - trueY) / (falseY - trueY)) * (0.0 - 1.0)` with all-double arithmetic.

---

## 5. WaterConditionSource + $1WaterCondition (`water`)

### (a) Codec
Record fields: `offset : int`, `surfaceDepthMultiplier : int`, `addStoneDepth : boolean`.

JSON fields (all required, no defaults):
- `"offset"` — `Codec.INT`
- `"surface_depth_multiplier"` — `Codec.intRange(-20, 20)`
- `"add_stone_depth"` — `Codec.BOOL`

Builder helpers: `waterBlockCheck(offset, mult)` → addStoneDepth=false; `waterStartCheck` → true.

### (b) apply → `new WaterCondition(this, ctx)` (LazyYCondition), no precomputation.

### (c) compute()
```java
protected boolean compute() {
    return context.waterHeight == -2147483648        // Integer.MIN_VALUE sentinel => "no water" => TRUE
        || context.blockY + (addStoneDepth ? context.stoneDepthAbove : 0)
             >= context.waterHeight + this.offset + context.surfaceDepth * this.surfaceDepthMultiplier;
}
```
Bytecode ops: sentinel check `if_icmpeq 75` (jump to true); main compare `if_icmplt 79` → **`>=` inclusive**.
Field reads in order: `waterHeight` (sentinel), `blockY`, `addStoneDepth?stoneDepthAbove:0`, `waterHeight`
(again), `offset`, `surfaceDepth`, `surfaceDepthMultiplier`. All 32-bit int arithmetic (`imul`, `iadd`) —
overflow wraps per Java, irrelevant for sane data but bit-exactness note.

`context.surfaceDepth` is the per-column value set in `updateXZ` from `SurfaceSystem.getSurfaceDepth(x,z)`
(covered in the SurfaceSystem report).

---

## 6. YConditionSource + $1YCondition (`y_above`)

### (a) Codec
Record fields: `anchor : VerticalAnchor`, `surfaceDepthMultiplier : int`, `addStoneDepth : boolean`.

JSON fields (all required, no defaults):
- `"anchor"` — `VerticalAnchor.CODEC`
- `"surface_depth_multiplier"` — `Codec.intRange(-20, 20)`
- `"add_stone_depth"` — `Codec.BOOL`

Builder helpers: `yBlockCheck(anchor, mult)` → addStoneDepth=false; `yStartCheck` → true.

### (b) apply → `new YCondition(this, ctx)` (LazyYCondition). Note: **anchor is NOT resolved in apply**.

### (c) compute()
```java
protected boolean compute() {
    return context.blockY + (addStoneDepth ? context.stoneDepthAbove : 0)
        >= this.anchor.resolveY(context.context)      // resolved on every compute (cheap, pure)
           + context.surfaceDepth * this.surfaceDepthMultiplier;
}
```
Compare: `if_icmplt 71` → **`>=` inclusive**. Same int arithmetic pattern as WaterCondition, without
any water/sentinel involvement. `add_stone_depth` adds `stoneDepthAbove` to the *left* side (blockY).

---

## 7. StoneDepthCheck + $1StoneDepthCondition (`stone_depth`)

### (a) Codec
Record fields: `offset : int`, `addSurfaceDepth : boolean`, `secondaryDepthRange : int`,
`surfaceType : CaveSurface`.

JSON fields (all required, no defaults):
- `"offset"` — `Codec.INT`
- `"add_surface_depth"` — `Codec.BOOL`
- `"secondary_depth_range"` — `Codec.INT`
- `"surface_type"` — `CaveSurface.CODEC` (string `"ceiling"` | `"floor"`, see §10)

### (b) apply(Context)
```java
public Condition apply(Context ctx) {
    boolean ceiling = this.surfaceType == CaveSurface.CEILING;   // reference compare of enum
    return new StoneDepthCondition(this, ctx, ceiling);          // LazyYCondition
}
```

### (c) compute() — exact formula
```java
protected boolean compute() {
    int depth = ceiling ? context.stoneDepthBelow : context.stoneDepthAbove;  // CEILING -> below, FLOOR -> above
    int surf  = this.addSurfaceDepth ? context.surfaceDepth : 0;
    int secondary = (this.secondaryDepthRange == 0)
        ? 0
        : (int)Mth.map(context.getSurfaceSecondary(), -1.0, 1.0, 0.0, (double)this.secondaryDepthRange);
        // d2i => truncation toward zero, NOT floor
    return depth <= 1 + this.offset + surf + secondary;   // if_icmpgt -> false, so <= INCLUSIVE
}
```
`Mth.map(v, -1.0, 1.0, 0.0, R)` expands to `0.0 + ((v - (-1.0)) / (1.0 - (-1.0))) * (R - 0.0)`
= `((v + 1.0) / 2.0) * R` in exact double ops (per §0.1: inverseLerp then lerp; note lerp form is
`to0 + t*(to1-to0)` with `to0 = 0.0`). `getSurfaceSecondary()` is the per-XZ cached
`SurfaceSystem.getSurfaceSecondary(blockX, blockZ)` (§0.3).

Evaluation order note: `getSurfaceSecondary()` is only called when `secondaryDepthRange != 0`
(`ifne 61` guards the call), which matters for the XZ-cache warm-up sequence but not for values.

---

## 8. NotConditionSource + NotCondition (`not`)

### (a) Codec
Record field: `target : ConditionSource`.
`CODEC = ConditionSource.CODEC.xmap(NotConditionSource::new, NotConditionSource::target).fieldOf("invert")`
→ JSON: `{ "type": "minecraft:not", "invert": <condition> }`. Required.

### (b)/(c)
```java
public Condition apply(Context ctx) { return new NotCondition(this.target.apply(ctx)); }
// NotCondition is a record implementing Condition directly (no laziness of its own):
public boolean test() { return !this.target.test(); }
```

---

## 9. Singletons: `steep`, `hole`, `temperature`, `above_preliminary_surface`

All four are single-constant enums (`INSTANCE`) with `CODEC = MapCodec.unit(INSTANCE)` (no JSON fields).
`apply(Context)` returns the pre-constructed condition stored on the Context
(`ctx.steep` / `ctx.hole` / `ctx.temperature` / `ctx.abovePreliminarySurface`), so all rules share one
instance per chunk context.

### 9.1 Context$SteepMaterialCondition (LazyXZCondition)
```java
protected boolean compute() {
    int lx = context.blockX & 15;
    int lz = context.blockZ & 15;
    int zm = Math.max(lz - 1, 0);
    int zp = Math.min(lz + 1, 15);
    ChunkAccess chunk = context.chunk;
    int hNorth = chunk.getHeight(Heightmap$Types.WORLD_SURFACE_WG, lx, zm);
    int hSouth = chunk.getHeight(Heightmap$Types.WORLD_SURFACE_WG, lx, zp);
    if (hSouth >= hNorth + 4) return true;          // if_icmplt 85 skips
    int xm = Math.max(lx - 1, 0);
    int xp = Math.min(lx + 1, 15);
    int hWest = chunk.getHeight(Heightmap$Types.WORLD_SURFACE_WG, xm, lz);
    int hEast = chunk.getHeight(Heightmap$Types.WORLD_SURFACE_WG, xp, lz);
    return hWest >= hEast + 4;                      // note the ASYMMETRY: +z side high OR -x side high
}
```
Both comparisons `>=` (bytecode `if_icmplt` → false path). Heightmap: `WORLD_SURFACE_WG`, clamped to
chunk-local 0..15 (no cross-chunk reads).

### 9.2 Context$HoleCondition (LazyXZCondition)
```java
protected boolean compute() { return context.surfaceDepth <= 0; }   // ifgt -> false, so <= 0
```

### 9.3 Context$TemperatureHelperCondition (LazyYCondition)
```java
protected boolean compute() {
    return context.getBiome().value()
        .coldEnoughToSnow(context.pos.set(context.blockX, context.blockY, context.blockZ),
                          context.getSeaLevel());
}
```
External call noted, not deep-dived: `Biome.coldEnoughToSnow(BlockPos, int seaLevel)` — the extra
`seaLevel` int parameter is newer than classic 1.21.0 (`coldEnoughToSnow(BlockPos)`).

### 9.4 Context$AbovePreliminarySurfaceCondition — plain `Condition`, NOT lazy
```java
public boolean test() { return context.blockY >= context.getMinSurfaceLevel(); }  // if_icmplt -> false; >= INCLUSIVE
```
(Caching is inside `getMinSurfaceLevel()` itself, §0.4.)

---

## 10. CaveSurface (net.minecraft.world.level.levelgen.placement.CaveSurface)

```java
public enum CaveSurface implements StringRepresentable {
    CEILING(Direction.UP,    1, "ceiling"),
    FLOOR  (Direction.DOWN, -1, "floor");
    // CODEC = StringRepresentable.fromEnum(CaveSurface::values)  -> JSON strings "ceiling" / "floor"
    // getDirection() / getY() (the +1/-1 step) / getSerializedName()
}
```

---

## 11. VerticalAnchor and inner classes — exact resolveY math

`VerticalAnchor.CODEC = Codec.xor(Absolute.CODEC, Codec.xor(AboveBottom.CODEC, BelowTop.CODEC))`
xmapped through merge/split — i.e. JSON is a one-key object, exactly one of:

| JSON | class | resolveY(WorldGenerationContext ctx) |
|---|---|---|
| `{"absolute": y}` | Absolute | `return y;` |
| `{"above_bottom": off}` | AboveBottom | `return ctx.getMinGenY() + off;` |
| `{"below_top": off}` | BelowTop | `return ctx.getGenDepth() - 1 + ctx.getMinGenY() - off;` |

BelowTop bytecode order: `getGenDepth(); iconst_1; isub; getMinGenY(); iadd; offset; isub` →
`(getGenDepth() - 1) + getMinGenY() - offset`. All int.

All three int codecs are `Codec.intRange(DimensionType.MIN_Y, DimensionType.MAX_Y)` — these are
runtime-computed statics, not pool constants: `BITS_FOR_Y = BlockPos.PACKED_Y_LENGTH;
Y_SIZE = (1 << BITS_FOR_Y) - 32; MAX_Y = (Y_SIZE >> 1) - 1; MIN_Y = MAX_Y - Y_SIZE + 1`
(with PACKED_Y_LENGTH = 12 that is MIN_Y = -2032, MAX_Y = 2031 — validation range only, does not
affect worldgen math).

Convenience: `VerticalAnchor.BOTTOM = aboveBottom(0)`, `TOP = belowTop(0)`.

External calls noted, not deep-dived: `WorldGenerationContext.getMinGenY()I`, `getGenDepth()I`.

---

## 12. Deltas vs 1.21 (bytecode-observed; flagged for the C port)

1. **`noise_threshold` gained `"is_3d"`** (optional, default `false`). 3d samples
   `NormalNoise.getValue(x, y, z)` cached per Y-step; 2d keeps the 1.21 behavior `getValue(x, 0.0, z)`
   cached per XZ-step. The condition object is no longer a LazyXZCondition — caching moved into shared
   per-noise-key `DoubleSupplier`s held in two `IdentityHashMap`s on the Context. Sulfur-cave surface
   rules presumably use the 3d form.
2. **`biome` stores a `HolderSet<Biome>`** (registry-tag capable via `RegistryCodecs.homogeneousList`)
   instead of a key set, and `apply()` constant-folds to `()->true` / `()->false` using the new
   `Context.possibleBiomes : Set<Holder<Biome>>` (nullable) — `canNeverMatch` / `willAlwaysMatch`.
3. **`Identifier`** replaces `ResourceLocation` (`vertical_gradient.random_name`); factory key remains
   `identifier.toString()` fed to `fromHashOf(String)` then `forkPositional()`, cached per Identifier
   on RandomState.
4. **`Biome.coldEnoughToSnow` now takes `(BlockPos, int seaLevel)`**, with the sea level pulled from
   `SurfaceSystem.getSeaLevel()` through the Context.
5. `getMinSurfaceLevel` caches the 4 `preliminarySurfaceLevel` corners keyed by
   `ChunkPos.pack(cellX, cellZ)` with sentinel `Long.MAX_VALUE`, and does the bilinear weight math in
   **float** (`(blockX & 15) / 16.0f`) before widening — worth replicating exactly.
