# A7 — RNG substrate + Mth + value/height providers for the carver stack (MC 26.2, unobfuscated server bytecode)

Source of truth: `javap -p -c -constants` (and `javap -v` for BootstrapMethods) against
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server`. All pseudocode is a 1:1 reconstruction
from bytecode. No vanilla-source guessing.

Ground covered here: `WorldgenRandom` (+`$Algorithm`), `LegacyRandomSource`, `SingleThreadedRandomSource`,
`BitRandomSource`, `RandomSource` (statics/defaults), `RandomSupport.generateUniqueSeed`,
`MarsagliaPolarGaussian`, the carver-relevant subset of `Mth`, `UniformFloat`/`TrapezoidFloat`/
`ConstantFloat`/`FloatProvider`, `UniformHeight`, `VerticalAnchor` (+3 impls), `WorldGenerationContext`.
Neighbors: A1 (applyCarvers orchestration), A2 (WorldCarver base), A3 (CaveWorldCarver),
A4 (CanyonWorldCarver), A5 (CarvingContext/aquifer), A6 (CarvingMask/ProtoChunk).

---

## 1. Who constructs the carving RandomSource — `NoiseBasedChunkGenerator.applyCarvers`

Descriptor: `applyCarvers(WorldGenRegion, long /*worldSeed*/, RandomState, BiomeManager, StructureManager, ChunkAccess)`.
(There is NO `GenerationStep$Carving` parameter — see §1.1.) Exact bytecode-verified chain:

```java
public void applyCarvers(WorldGenRegion region, long seed, RandomState rs, BiomeManager bm, StructureManager sm, ChunkAccess chunk) {
    if (SharedConstants.DEBUG_DISABLE_CARVERS) return;
    BiomeManager biomeManager = bm.withDifferentSource((x,y,z) -> lambda$applyCarvers$0(rs, x,y,z)); // quart biome getter
    WorldgenRandom random = new WorldgenRandom(new LegacyRandomSource(RandomSupport.generateUniqueSeed()));
        // ^^^ construction seed is IRRELEVANT for output: every draw path below re-seeds via
        //     setLargeFeatureSeed BEFORE any nextX call. (generateUniqueSeed = nanoTime-xor uniquifier, §3.4)
    int i = 8;                                    // bipush 8; istore 10 (dead local — range constant)
    ChunkPos pos = chunk.getPos();
    NoiseChunk noiseChunk = chunk.getOrCreateNoiseChunk(...lambda$applyCarvers$1...);
    Aquifer aquifer = noiseChunk.aquifer();
    CarvingContext ctx = new CarvingContext(this, region.registryAccess(),
        chunk.getHeightAccessorForGeneration(), noiseChunk, rs, settings.value().surfaceRule());
    CarvingMask mask = ((ProtoChunk) chunk).getOrCreateCarvingMask();

    for (int dx = -8; dx <= 8; ++dx) {                       // outer: x offset
        for (int dz = -8; dz <= 8; ++dz) {                   // inner: z offset (17x17 chunks)
            ChunkPos npos = new ChunkPos(pos.x() + dx, pos.z() + dz);
            ChunkAccess nchunk = region.getChunk(npos.x(), npos.z());
            BiomeGenerationSettings gens = nchunk.carverBiome(() -> lambda$applyCarvers$2(npos, rs));
            Iterable<Holder<ConfiguredWorldCarver<?>>> carvers = gens.getCarvers();   // NO step arg
            int idx = 0;
            for (Holder<ConfiguredWorldCarver<?>> holder : carvers) {
                ConfiguredWorldCarver<?> carver = holder.value();
                random.setLargeFeatureSeed(seed + (long) idx, npos.x(), npos.z());   // §2.3 — 2 nextLong draws
                if (carver.isStartChunk(random)) {                                   // 1 nextFloat (A2)
                    carver.carve(ctx, chunk, biomeManager::getBiome /*Function*/, random, aquifer, npos, mask);
                }
                ++idx;   // idx increments whether or not isStartChunk passed
            }
        }
    }
}
```

- 26.2 delta vs 1.21: `applyCarvers` takes no `GenerationStep.Carving` and
  `BiomeGenerationSettings.getCarvers()` returns a single `Iterable` (carving steps are gone; the
  AIR/LIQUID split no longer exists). `idx` is the position of the carver in that single list.
- The per-carver seed is `worldSeed + idx` (long add), scrambled with the NEIGHBOR chunk coords.
- Deeper carve internals (A3 §, A4 §): `CaveWorldCarver.createTunnel` / `CanyonWorldCarver.doCarve`
  both start with `RandomSource.createThreadLocalInstance(longSeed)` where `longSeed` came from a
  `random.nextLong()` on this WorldgenRandom — that factory returns a **SingleThreadedRandomSource**
  (§3.2), NOT a LegacyRandomSource. Identical LCG math, no atomics.
  26.2 delta: 1.21 used `RandomSource.create(seed)` → LegacyRandomSource here; output-identical.

### 1.1 Callers / neighbors
`applyCarvers` exists only on `NoiseBasedChunkGenerator` in the levelgen tree (grep needs `-a`:
class files are binary). `ChunkStatusTasks`/`ChunkGenerator` orchestration is A1's ground.

---

## 2. `net.minecraft.world.level.levelgen.WorldgenRandom`

`public class WorldgenRandom extends LegacyRandomSource` — fields, declaration order:

```java
private final RandomSource randomSource;   // the wrapped delegate (LegacyRandomSource on the carve path)
private int count;                         // incremented once per next(bits) call; getCount() debug accessor
```

### 2.1 Constructor and the dead inherited state

```java
public WorldgenRandom(RandomSource randomSource) {
    super(0L);                       // LegacyRandomSource ctor → virtual this.setSeed(0L)
    this.randomSource = randomSource;
}
```

`LegacyRandomSource.<init>(0L)` allocates the inherited `AtomicLong seed` + `MarsagliaPolarGaussian`,
then calls `setSeed(0)` **virtually**; `WorldgenRandom.setSeed` begins with
`if (this.randomSource == null) return;` (bytecode `getfield randomSource; ifnonnull 8; return`) —
so during super-construction it is a no-op and the inherited AtomicLong stays 0 and is NEVER used
again (all draws delegate). Do not model the inherited LCG state.

### 2.2 Draw path

```java
public int next(int bits) {
    ++this.count;
    RandomSource rs = this.randomSource;
    if (rs instanceof LegacyRandomSource legacy) return legacy.next(bits);   // carve path: always this branch
    return (int)(this.randomSource.nextLong() >>> (64 - bits));             // non-legacy fallback
}
public synchronized void setSeed(long seed) {
    if (this.randomSource == null) return;
    this.randomSource.setSeed(seed);          // scramble happens inside the delegate (§3.1)
}
public RandomSource fork()                { return this.randomSource.fork(); }
public PositionalRandomFactory forkPositional() { return this.randomSource.forkPositional(); }
```

`nextInt/nextInt(bound)/nextLong/nextBoolean/nextFloat/nextDouble` are NOT overridden — they are the
`BitRandomSource` defaults (§3.3) inherited via `LegacyRandomSource`, all funneling into
`WorldgenRandom.next(bits)` above. `nextGaussian()` is inherited from `LegacyRandomSource` and uses
the OUTER gaussianSource (wrapping `this`); note `WorldgenRandom.setSeed` delegates and therefore
resets only the INNER delegate's gaussian cache, never the outer one — a latent vanilla quirk;
carvers make no nextGaussian draws (grep of Cave/Canyon/WorldCarver: none), so it cannot bite here.

### 2.3 Seeding methods (all four exist in 26.2), exact ops in program order

```java
public long setDecorationSeed(long worldSeed, int minBlockX, int minBlockZ) {
    setSeed(worldSeed);
    long a = nextLong() | 1L;                                    // draw #1,#2 (nextLong = 2x next(32))
    long b = nextLong() | 1L;                                    // draw #3,#4
    long s = ((long) minBlockX * a + (long) minBlockZ * b) ^ worldSeed;
    setSeed(s);
    return s;
}
public void setFeatureSeed(long decorationSeed, int index, int step) {
    setSeed(decorationSeed + (long) index + (long)(10000 * step));   // NO draws
}
public void setLargeFeatureSeed(long seed, int chunkX, int chunkZ) {     // *** THE CARVER SEEDER ***
    setSeed(seed);
    long a = nextLong();                                         // draw #1,#2 — NO |1 here
    long b = nextLong();                                         // draw #3,#4
    setSeed(((long) chunkX * a) ^ ((long) chunkZ * b) ^ seed);
}
public void setLargeFeatureWithSalt(long seed, int regionX, int regionZ, int salt) {
    setSeed((long) regionX * 341873128712L + (long) regionZ * 132897987541L + seed + (long) salt);  // NO draws
}
public static RandomSource seedSlimeChunk(int chunkX, int chunkZ, long worldSeed, long salt) {
    return RandomSource.createThreadLocalInstance(
        (worldSeed + (long)(chunkX * chunkX * 4987142) + (long)(chunkX * 5947611)
                   + (long)(chunkZ * chunkZ) * 4392871L + (long)(chunkZ * 389711)) ^ salt);
}
```

Note `setLargeFeatureSeed` differs from `setDecorationSeed`: XOR (not +) combine, no `| 1`, void return.

### 2.4 `WorldgenRandom$Algorithm`

Enum, ordinal order `LEGACY(0)`, `XOROSHIRO(1)`; field `LongFunction<RandomSource> constructor`;
BootstrapMethods verified: LEGACY → `LegacyRandomSource::new(J)`, XOROSHIRO → `XoroshiroRandomSource::new(J)`;
`newInstance(long)` just applies it. Not used on the carve path (applyCarvers hardcodes
`new LegacyRandomSource`), regardless of `useLegacyRandomSource` in noise settings.

---

## 3. The 48-bit LCG family

### 3.1 `LegacyRandomSource implements BitRandomSource`

Constants (declared in class): `MODULUS_BITS=48`, `MODULUS_MASK=281474976710655L (0xFFFF_FFFF_FFFFL)`,
`MULTIPLIER=25214903917L (0x5DEECE66DL)`, `INCREMENT=11L`. State: `AtomicLong seed` +
`MarsagliaPolarGaussian gaussianSource` (ctor order: seed atomic, gaussian, then `setSeed(arg)`).

```java
public void setSeed(long seed) {
    // CAS from current value; throws ThreadingDetector exception on contention (single-thread: never)
    this.seed.set((seed ^ 25214903917L) & 281474976710655L);
    this.gaussianSource.reset();                                  // clears haveNextNextGaussian
}
public int next(int bits) {
    long s  = this.seed.get();
    long s2 = (s * 25214903917L + 11L) & 281474976710655L;        // CAS store, same contention throw
    this.seed.set(s2);
    return (int)(s2 >> (48 - bits));                              // lshr = ARITHMETIC shift; bits<=32 so
}                                                                 // high garbage is cut by l2i anyway
public RandomSource fork()      { return new LegacyRandomSource(nextLong()); }
public PositionalRandomFactory forkPositional() { return new LegacyRandomSource$LegacyPositionalRandomFactory(nextLong()); }
public double nextGaussian()    { return gaussianSource.nextGaussian(); }
```

Identical to `java.util.Random` semantics == hyperchunk `hc_lcg` (core/src/rng.c: HC_LCG_MUL
0x5DEECE66D, inc 11, mask 48-bit). **No 26.2 surprises found** vs java.util.Random.

### 3.2 `SingleThreadedRandomSource implements BitRandomSource`

Same four constants; plain `long seed` field; `MarsagliaPolarGaussian` is LAZY
(allocated on first `nextGaussian()`; `setSeed` resets it only `if != null`).
`setSeed` = same scramble, no CAS; `next(bits)` = same LCG, `(int)(s2 >> (48 - bits))`.
`fork()` → new SingleThreadedRandomSource(nextLong()); `forkPositional()` → **Legacy**PositionalRandomFactory(nextLong()).
Numerically indistinguishable from LegacyRandomSource — hc_lcg covers both. This is what
`RandomSource.createThreadLocalInstance(long)` returns (used inside createTunnel/doCarve, §1).

### 3.3 `BitRandomSource` default methods (the ONLY implementations of nextX on this path)

Constants: `FLOAT_MULTIPLIER = 5.9604645E-8f` (bits 0x33800000 == exactly 2^-24),
`DOUBLE_MULTIPLIER = 1.1102230246251565E-16` (bits 0x3CA0000000000000 == exactly 2^-53).

```java
default int nextInt()          { return next(32); }
default int nextInt(int bound) {                       // bound <= 0 → IllegalArgumentException
    if ((bound & (bound - 1)) == 0)                    // power of two
        return (int)((long) bound * (long) next(31) >> 31);
    while (true) {                                     // modulo-bias rejection
        int u = next(31);
        int r = u % bound;
        if (u - r + (bound - 1) >= 0) return r;        // int overflow test (iflt loops)
    }
}
default long nextLong()  { int hi = next(32); int lo = next(32); return ((long) hi << 32) + (long) lo; }
                            // ^ lo is SIGN-EXTENDED then added (i2l; ladd) — java.util.Random exact
default boolean nextBoolean() { return next(1) != 0; }
default float  nextFloat()    { return (float) next(24) * 5.9604645E-8f; }           // i2f; fmul — float math
default double nextDouble()   { int hi = next(26); int lo = next(27);
                                return (double)(((long) hi << 27) + (long) lo) * 1.1102230246251565E-16; }
default int nextIntBetweenInclusive(int min, int max) { return nextInt(max - min + 1) + min; }  // RandomSource default
default float  triangle(float  a, float  b) { return a + b * (nextFloat() - nextFloat()); }  // 2 draws; note eval order: first draw minus second
default double triangle(double a, double b) { return a + b * (nextDouble() - nextDouble()); }
```

`RandomSource` statics: `create(long)` → LegacyRandomSource; `createThreadLocalInstance(long)` →
SingleThreadedRandomSource; `createThreadLocalInstance()` (no-arg, netty ThreadLocalRandom seed) and
`createThreadSafe()` → ThreadSafeLegacyRandomSource are not on the carve path.

### 3.4 `RandomSupport.generateUniqueSeed`

`SEED_UNIQUIFIER` AtomicLong init `8682522807148012L`; each call
`updateAndGet(x -> x * 1181783497276652981L) ^ System.nanoTime()`. Non-deterministic by design —
only ever consumed as the throwaway construction seed in §1. (GOLDEN_RATIO_64 = 0x9E3779B97F4A7C15
(-7046029254386353131), SILVER_RATIO_64 = 0x6A09E667F3BCC909 (7640891576956012809) — xoroshiro
ground, listed for completeness.)

### 3.5 `MarsagliaPolarGaussian` (carvers: unused, documented for the record)

Fields: `randomSource`, `double nextNextGaussian`, `boolean haveNextNextGaussian`; `reset()` clears
the flag. `nextGaussian()`: cached value if flagged; else loop `{ a = 2*nextDouble()-1; b = 2*nextDouble()-1;
r = a*a + b*b; }` until `r < 1.0 && r != 0.0`; `m = Math.sqrt(-2*Math.log(r)/r)`; caches `b*m`, returns `a*m`.
(Bytecode compares `dcmpl ifge` and `dcmpl ifeq` — exact java.util.Random polar method.)

---

## 4. `net.minecraft.util.Mth` — exactly the carver-touched subset

Grep of Cave/Canyon/WorldCarver bytecode → Mth calls used: `sin(D)F`, `cos(D)F`, `floor(D)I`,
`abs(F)F`, `randomBetween(RandomSource,FF)F`. Plus `java.lang.Math.abs(D)D`, `Math.max/min(II)`
inside WorldCarver, and `Mth.randomBetweenInclusive` via UniformHeight (§6.1). No Math.exp/sqrt/pow
calls anywhere in the three carver classes.

### 4.1 SIN table — 26.2 delta, load-bearing

Constants: `SIN_QUANTIZATION=65536`, `SIN_MASK=65535`, `COS_OFFSET=16384`,
`SIN_SCALE = 10430.378350470453` (bits **0x40C45F306DC9C883** — this is exactly the
correctly-rounded double of 65536/(2π)). Table (65536 floats) built in `static {}` via
`Util.make(new float[65536], lambda$static$0)`:

```java
private static void lambda$static$0(float[] tab) {
    for (int i = 0; i < tab.length; ++i)
        tab[i] = (float) Math.sin((double) i / 10430.378350470453);   // i2d; ddiv; Math.sin; d2f
}
public static float sin(double x) { return SIN[(int)((long)(x * 10430.378350470453) & 65535L)]; }
                                     //         dmul; d2l (truncate toward zero); land 65535; l2i; faload
public static float cos(double x) { return SIN[(int)((long)(x * 10430.378350470453 + 16384.0) & 65535L)]; }
                                     //         dmul; dadd 16384.0 (0x40D0000000000000); d2l; land
```

26.2 deltas vs 1.21 (both matter for bit-exactness):
1. Signature is `(double) -> float` (no float overload exists at all in 26.2); carvers pass double
   angles directly — no f2d round-trip.
2. Table generation is `Math.sin(i / SIN_SCALE)` — a DIVISION by 65536/(2π), not the 1.21
   `Math.sin(i * Math.PI * 2.0 / 65536.0)` multiply-divide chain; per-index results can differ in
   the last double ULP before the d2f, so regenerate the table with this exact expression.
3. Index math is `d2l` (long truncation toward zero, saturating at Long.MIN/MAX) then `& 65535` —
   for negative products this yields the two's-complement low 16 bits (e.g. −2.9→−2→65534);
   truncation is toward zero, NOT floor, so inputs in (−1.0, 0.0)·(1/SCALE) map to index 0.

### 4.2 Scalar helpers (exact bytecode)

```java
public static int   floor(double d) { return (int) Math.floor(d); }        // Math.floor then d2i (saturating)
public static int   floor(float  f) { return (int) Math.floor((double) f); } // 26.2: f2d + Math.floor (1.21 used branchy int cast; same results)
public static long  lfloor(double d){ return (long) Math.floor(d); }
public static float abs(float f)    { return Math.abs(f); }
public static float sqrt(float f)   { return (float) Math.sqrt((double) f); }   // not carver-called; listed as neighbor of sin
public static float randomBetween(RandomSource r, float min, float max) {
    return r.nextFloat() * (max - min) + min;                                // ALL float ops: fsub; fmul; fadd
}
public static int randomBetweenInclusive(RandomSource r, int min, int max) {
    return r.nextInt(max - min + 1) + min;                                   // 1 draw
}
// bounds-checked variants (return min without a draw when min >= max / min > max):
public static int    nextInt   (RandomSource r, int min, int max)      { return min >= max ? min : r.nextInt(max - min + 1) + min; }
public static float  nextFloat (RandomSource r, float min, float max)  { return min >= max ? min : r.nextFloat() * (max - min) + min; }  // fcmpl iflt
public static double nextDouble(RandomSource r, double min, double max){ return min >= max ? min : r.nextDouble() * (max - min) + min; }
public static double square(double d) { return d * d; }                     // (float/int/long overloads exist)
```

`PI = 3.1415927f`, `TWO_PI = 6.2831855f`, `HALF_PI = 1.5707964f` (public float constants — if A3/A4
quote `6.2831855F` widened to double in angle math, it is this float constant via f2d, NOT
`Math.PI * 2`). `Math.floor/abs/max/min/sqrt` are exactly-specified (0.5-ulp) — safe in C.
`Math.sin` (used only for table GENERATION) is the 1-ulp-tolerance `java.lang.Math` intrinsic — see OPEN.

---

## 5. Float providers (`net.minecraft.util.valueproviders`)

Interfaces: `SampledFloat { float sample(RandomSource); }`;
`FloatProvider extends SampledFloat { float min(); float max(); MapCodec codec(); }`.

### 5.1 `UniformFloat` — record(float min, float max); JSON `minecraft:uniform` {min_inclusive, max_exclusive}

```java
public float sample(RandomSource r) { return Mth.randomBetween(r, this.min, this.max); }
// == nextFloat() * (max - min) + min      *** exactly 1 draw, all float arithmetic ***
```

### 5.2 `TrapezoidFloat` — record(float min, float max, float plateau); JSON `minecraft:trapezoid`

```java
public float sample(RandomSource r) {
    float range = this.max - this.min;              // fsub
    float g = (range - this.plateau) / 2.0F;        // fsub; fconst_2; fdiv
    float h = range - g;                            // fsub
    return this.min + r.nextFloat() * h             // *** draw #1 scaled by (range - g) ***
                    + r.nextFloat() * g;            // *** draw #2 scaled by g ***
}                                                   // eval order: min + d1*h first (fadd), then + d2*g
```

Canyon `thickness` (min 0, max 6, plateau 2): range=6, g=2, h=4 → `0 + nf1*4 + nf2*2` (2 draws).

### 5.3 `ConstantFloat` — record(float value); `ZERO` static; `sample` returns value, **0 draws**;
`min()==max()==value`. Canyon JSON encodes `vertical_radius_center_factor: 0.0` /
`vertical_radius_default_factor: 1.0` as plain floats — but in `CanyonShapeConfiguration` these are
**raw `float` fields** (`verticalRadiusDefaultFactor`, `verticalRadiusCenterFactor`), not
FloatProviders; no sampling ever happens for them (§7 field list).

MultipliedFloats / ClampedNormalFloat exist but are referenced by no carver config — out of scope.

---

## 6. Height providers + vertical anchors

`heightproviders/` contents: BiasedToBottomHeight, ConstantHeight, HeightProvider (abstract,
`public abstract int sample(RandomSource, WorldGenerationContext)`), HeightProviderType,
TrapezoidHeight, UniformHeight, VeryBiasedToBottomHeight, WeightedListHeight. All four shipped
carver configs use ONLY `minecraft:uniform` for `y` (§7).

### 6.1 `UniformHeight` — fields `VerticalAnchor minInclusive, maxInclusive` (+LongSet warnedFor)

```java
public int sample(RandomSource random, WorldGenerationContext ctx) {
    int min = this.minInclusive.resolveY(ctx);      // resolve order: min FIRST
    int max = this.maxInclusive.resolveY(ctx);      // then max
    if (min > max) { /* one-time LOGGER.warn per (min,max) pair */ return min; }   // 0 draws
    return Mth.randomBetweenInclusive(random, min, max);   // *** exactly 1 draw: nextInt(max-min+1)+min ***
}
```

### 6.2 `VerticalAnchor` — 3 record impls, exact `resolveY(WorldGenerationContext)`

```java
Absolute(int y)         : return y;
AboveBottom(int offset) : return ctx.getMinGenY() + offset;
BelowTop(int offset)    : return ctx.getGenDepth() - 1 + ctx.getMinGenY() - offset;
```

JSON forms `{"absolute": n} / {"above_bottom": n} / {"below_top": n}` (Either codec, no draws).
`CarverConfiguration.lavaLevel` is a VerticalAnchor (all overworld carvers: above_bottom 8 →
resolves to minGenY+8 = **−56**; nether_cave: above_bottom 10).

### 6.3 `WorldGenerationContext`

```java
public WorldGenerationContext(ChunkGenerator gen, LevelHeightAccessor level) {
    this.minY   = Math.max(level.getMinY(),  gen.getMinY());      // field 1
    this.height = Math.min(level.getHeight(), gen.getGenDepth()); // field 2
}
public int getMinGenY()  { return minY; }
public int getGenDepth() { return height; }
```

`CarvingContext extends WorldGenerationContext` (ctor forwards `(NoiseBasedChunkGenerator, heightAccessor)`
— A5 §). Overworld: minGenY=−64, genDepth=384. Resolved carver-y ranges:
cave uniform(above_bottom 8 → −56, absolute 180); cave_extra_underground uniform(−56, absolute 47);
canyon uniform(absolute 10, absolute 67); nether_cave uniform(absolute 0, below_top 1 → 127−1=126 for
128-depth nether).

---

## 7. Config wiring (datapack ↔ classes) — draw-relevant fields only

`data/minecraft/worldgen/configured_carver/{cave,cave_extra_underground,canyon,nether_cave}.json`.
Class field lists (public finals, javap order):

- `CarverConfiguration extends ProbabilityFeatureConfiguration` (probability float):
  `HeightProvider y; FloatProvider yScale; VerticalAnchor lavaLevel; CarverDebugSettings debugSettings; HolderSet<Block> replaceable;`
- `CaveCarverConfiguration`: + `FloatProvider horizontalRadiusMultiplier, verticalRadiusMultiplier, floorLevel;`
- `CanyonCarverConfiguration`: + `FloatProvider verticalRotation; CanyonShapeConfiguration shape;`
- `CanyonShapeConfiguration`: `FloatProvider distanceFactor, thickness; int widthSmoothness; FloatProvider horizontalRadiusFactor; float verticalRadiusDefaultFactor, verticalRadiusCenterFactor;`

Values: cave probability 0.15f, cave_extra 0.07f, canyon 0.01f, nether_cave 0.2f.
cave/cave_extra: yScale uniform[0.1,0.9), horizontal_radius uniform[0.7,1.4),
vertical_radius uniform[0.8,1.3), floor_level uniform[−1.0,−0.4). canyon: yScale **constant 3.0**
(ConstantFloat — 0 draws), vertical_rotation uniform[−0.125,0.125), shape: distance_factor
uniform[0.75,1.0), thickness trapezoid(0,6,2), width_smoothness 3, horizontal_radius_factor
uniform[0.75,1.0), vertical_radius default 1.0f / center 0.0f (raw floats).
nether_cave: yScale/h/v/floor all ConstantFloat (0.5/1.0/1.0/−0.7) — 0 draws each.

---

## 8. OPEN items

- OPEN: `Math.sin` used to generate the SIN table is `java.lang.Math` (1-ulp spec, HotSpot
  intrinsic), not `StrictMath`. On x86-64 HotSpot the intrinsic matches fdlibm for all observed
  inputs, but this is not a JLS guarantee — hyperchunk should generate the table with fdlibm sin
  and diff all 65536 floats against a golden dump from the actual JVM once, before trusting it.
- OPEN: `d2l` in Mth.sin/cos saturates at Long.MIN/MAX for |x·SCALE| ≥ 2^63; carver angles are
  tiny so unreachable in practice, but a C `(int64_t)` cast of such a value is UB — clamp if
  defensive coding is wanted (not needed for parity on sane worlds).
- OPEN: `UniformFloat` nominal max is exclusive, but `min + nextFloat()*(max−min)` can round UP to
  exactly `max` in float for some (min,max) pairs; bit-exact C float math reproduces this
  automatically — just do not "fix" it with a clamp.
- OPEN (neighbor ground, flag only): whether `ChunkAccess.carverBiome` caches the
  BiomeGenerationSettings per neighbor chunk between the 17×17 iterations of DIFFERENT center
  chunks does not affect RNG (no draws in the supplier), but affects performance modeling — A1/A6.

---

## 9. sulfur scan

Assigned classes (`WorldgenRandom*`, `LegacyRandomSource`, `SingleThreadedRandomSource`,
`BitRandomSource`, `RandomSource`, `RandomSupport`, `Mth`, `valueproviders/*`, `heightproviders/*`,
`VerticalAnchor*`, `WorldGenerationContext`): **no 'sulfur' occurrences** (case-insensitive strings
scan of constant pools). Tree-wide, levelgen contains it only in `Noises.class`
(`SULFUR_CAVE_GRADIENT` / `"minecraft:sulfur_cave_gradient"` — a noise key, surface/density ground,
not carvers). `data/minecraft/worldgen/biome/sulfur_caves.json` lists carvers
`["minecraft:cave", "minecraft:cave_extra_underground", "minecraft:canyon"]` — the standard three,
as expected; no sulfur-specific carver exists.
