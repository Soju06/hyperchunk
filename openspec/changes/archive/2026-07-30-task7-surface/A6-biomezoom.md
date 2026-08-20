# A6 — BiomeManager zoom + Biome temperature + heightmap conventions (MC 26.2, bytecode-verified)

Source of truth: `javap -p -c -constants` on unobfuscated 26.2 server classes in
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server`. Everything below is reconstructed
from bytecode only; where an external library's behavior matters (Guava sha256) it is flagged.

---

## 1. net.minecraft.util.LinearCongruentialGenerator

```java
public class LinearCongruentialGenerator {
    private static final long MULTIPLIER = 6364136223846793005L;
    private static final long INCREMENT  = 1442695040888963407L;

    public static long next(long seed, long salt) {
        seed *= seed * 6364136223846793005L + 1442695040888963407L;  // seed = seed * (seed*MULT + INC)
        seed += salt;
        return seed;
    }
}
```
Exact bytecode order: `lload_0; lload_0; ldc2_w MULT; lmul; ldc2_w INC; ladd; lmul` → `seed * ((seed * MULT) + INC)`, then `+ salt`. All wrapping 64-bit arithmetic.

---

## 2. net.minecraft.world.level.biome.BiomeManager

Fields (order of ctor writes): `noiseBiomeSource` first, then `biomeZoomSeed` (long).

Constants: `ZOOM_BITS = 2`, `ZOOM = 4`, `ZOOM_MASK = 3`,
`CHUNK_CENTER_QUART = QuartPos.fromBlock(8) = 2` (computed in `static{}`).

### 2.1 obfuscateSeed(long)

```java
public static long obfuscateSeed(long seed) {
    return Hashing.sha256().hashLong(seed).asLong();
}
```
Bytecode is exactly 3 calls: `Hashing.sha256()` → `HashFunction.hashLong(J)` → `HashCode.asLong()`.
The byte-order semantics are Guava's (not visible in this bytecode; confidence: high, standard
Guava contract):
- `hashLong(l)` hashes the 8 bytes of `l` in **little-endian** order (byte 0 = lowest 8 bits).
- `asLong()` assembles the **first 8 bytes of the SHA-256 digest, little-endian**
  (`bytes[0] | bytes[1]<<8 | ... | bytes[7]<<56`).

### 2.2 getBiome(BlockPos) — the full zoom

Exact Java-equivalent reconstruction (locals named per bytecode slots):

```java
public Holder<Biome> getBiome(BlockPos pos) {
    int i = pos.getX() - 2;               // read order: getX, getY, getZ
    int j = pos.getY() - 2;
    int k = pos.getZ() - 2;
    int l  = i >> 2;                      // arithmetic shift (ishr) of (coord - 2)
    int i1 = j >> 2;
    int j1 = k >> 2;
    double d0 = (double)(i & 3) / 4.0;    // iand 3, i2d, ddiv 4.0
    double d1 = (double)(j & 3) / 4.0;
    double d2 = (double)(k & 3) / 4.0;
    int k1 = 0;                           // best index
    double d3 = Double.POSITIVE_INFINITY; // ldc2_w Infinity

    for (int l1 = 0; l1 < 8; l1++) {
        boolean flagX = (l1 & 4) == 0;    // bit 4 -> X
        boolean flagY = (l1 & 2) == 0;    // bit 2 -> Y
        boolean flagZ = (l1 & 1) == 0;    // bit 1 -> Z
        int    xq = flagX ? l  : l  + 1;
        int    yq = flagY ? i1 : i1 + 1;
        int    zq = flagZ ? j1 : j1 + 1;
        double xf = flagX ? d0 : d0 - 1.0;
        double yf = flagY ? d1 : d1 - 1.0;
        double zf = flagZ ? d2 : d2 - 1.0;
        double d4 = getFiddledDistance(this.biomeZoomSeed, xq, yq, zq, xf, yf, zf);
        if (d3 > d4) {                    // dcmpl; ifle skip  ==> update ONLY on strict >
            k1 = l1;
            d3 = d4;
        }
    }

    int xFinal = (k1 & 4) == 0 ? l  : l  + 1;
    int yFinal = (k1 & 2) == 0 ? i1 : i1 + 1;
    int zFinal = (k1 & 1) == 0 ? j1 : j1 + 1;
    return this.noiseBiomeSource.getNoiseBiome(xFinal, yFinal, zFinal);
}
```

Key exactness points:
- Offsets: **`-2` on each block coordinate before `>>2`** (so it is `(x-2)>>2`, arithmetic shift;
  and fraction `((x-2)&3)/4.0`). Confirmed: `iconst_2 isub`, then `iconst_2 ishr` / `iconst_3 iand`.
- Candidate evaluation order: `l1 = 0..7`, **X is the high bit (4), Y is bit 2, Z is bit 1**;
  bit clear = base quart / base frac; bit set = quart+1 / frac−1.0.
  Order therefore: (x0,y0,z0), (x0,y0,z1), (x0,y1,z0), (x0,y1,z1), (x1,y0,z0), (x1,y0,z1), (x1,y1,z0), (x1,y1,z1).
- Tie-break: bytecode at the comparison is
  ```
  246: dload 15      // best
  248: dload 30      // candidate
  250: dcmpl
  251: ifle 262      // skip update when best <= candidate
  ```
  Update requires **best > candidate strictly**; on a tie the EARLIER index wins (lowest l1).
  (`dcmpl` yields −1 on NaN, so a NaN candidate never wins; initial best is +Infinity so index 0
  always wins the first comparison unless d4 is +Inf too.)

### 2.3 getFiddledDistance(long seed, int x, int y, int z, double xf, double yf, double zf)

```java
private static double getFiddledDistance(long seed, int x, int y, int z,
                                         double xf, double yf, double zf) {
    long l = seed;
    l = LinearCongruentialGenerator.next(l, (long)x);
    l = LinearCongruentialGenerator.next(l, (long)y);
    l = LinearCongruentialGenerator.next(l, (long)z);
    l = LinearCongruentialGenerator.next(l, (long)x);   // x,y,z mixed in TWICE, in x,y,z order
    l = LinearCongruentialGenerator.next(l, (long)y);
    l = LinearCongruentialGenerator.next(l, (long)z);
    double fx = getFiddle(l);
    l = LinearCongruentialGenerator.next(l, seed);      // re-mix with ORIGINAL seed as salt
    double fy = getFiddle(l);
    l = LinearCongruentialGenerator.next(l, seed);
    double fz = getFiddle(l);
    return Mth.square(zf + fz) + Mth.square(yf + fy) + Mth.square(xf + fx);
}
```
- Fiddle assignment order: first fiddle → **x**, second → **y**, third → **z**.
- **Summation order (FP-exact): `(square(zf+fz) + square(yf+fy)) + square(xf+fx)` — z first, then y, then x.**
  Bytecode: `dload 9 (zf) … square; dload 7 (yf) … square; dadd; dload 5 (xf) … square; dadd`.
- `Mth.square(double d)` = `d * d` (verified).

### 2.4 getFiddle(long)

```java
private static double getFiddle(long l) {
    double d = (double)Math.floorMod(l >> 24, 1024) / 1024.0;   // lshr = ARITHMETIC shift right
    return (d - 0.5) * 0.9;
}
```
- Constant is **1024** (both the `floorMod` int bound `sipush 1024` and the divisor `1024.0`), not 512.
- `l >> 24` is `lshr` = **arithmetic** (sign-propagating) shift in JVM; `Math.floorMod(long, int)`
  then maps to [0,1024). Range of result: [−0.45, +0.45).

### 2.5 getNoiseBiomeAtPosition variants + getNoiseBiomeAtQuart

No zoom/fiddle in any of these — direct delegation:

```java
public Holder<Biome> getNoiseBiomeAtPosition(double x, double y, double z) {
    return getNoiseBiomeAtQuart(QuartPos.fromBlock(Mth.floor(x)),
                                QuartPos.fromBlock(Mth.floor(y)),
                                QuartPos.fromBlock(Mth.floor(z)));
}
public Holder<Biome> getNoiseBiomeAtPosition(BlockPos pos) {
    return getNoiseBiomeAtQuart(QuartPos.fromBlock(pos.getX()),
                                QuartPos.fromBlock(pos.getY()),
                                QuartPos.fromBlock(pos.getZ()));
}
public Holder<Biome> getNoiseBiomeAtQuart(int x, int y, int z) {
    return this.noiseBiomeSource.getNoiseBiome(x, y, z);
}
```
`Mth.floor(double)` = `(int)Math.floor(d)` (verified).

### 2.6 BiomeManager$NoiseBiomeSource

```java
public interface BiomeManager$NoiseBiomeSource {
    Holder<Biome> getNoiseBiome(int quartX, int quartY, int quartZ);
}
```

---

## 3. net.minecraft.core.QuartPos

```java
BITS = 2; SIZE = 4; MASK = 3; SECTION_TO_QUARTS_BITS = 2;
fromBlock(int i)   = i >> 2;   // arithmetic ishr
quartLocal(int i)  = i & 3;
toBlock(int i)     = i << 2;
fromSection(int i) = i << 2;
toSection(int i)   = i >> 2;
```

---

## 4. net.minecraft.world.level.biome.Biome

Fields (ctor write order): `temperatureCache` (ThreadLocal.withInitial), `climateSettings`,
`generationSettings`, `mobSettings`, `attributes`, `specialEffects`.
`TEMPERATURE_CACHE_SIZE = 1024`.

### 4.1 Static noise init (exact seeds/octaves, in `static{}` order)

```java
TEMPERATURE_NOISE        = new PerlinSimplexNoise(new WorldgenRandom(new LegacyRandomSource(1234L)), ImmutableList.of(0));
FROZEN_TEMPERATURE_NOISE = new PerlinSimplexNoise(new WorldgenRandom(new LegacyRandomSource(3456L)), ImmutableList.of(-2, -1, 0));
BIOME_INFO_NOISE         = new PerlinSimplexNoise(new WorldgenRandom(new LegacyRandomSource(2345L)), ImmutableList.of(0));
```
All three are `PerlinSimplexNoise` (Simplex-based), NOT PerlinNoise. RNG chain is
`WorldgenRandom` wrapping `LegacyRandomSource` (java.util.Random-style LCG).

### 4.2 getHeightAdjustedTemperature(BlockPos, int seaLevel)  — private

```java
private float getHeightAdjustedTemperature(BlockPos pos, int seaLevel) {
    float f = this.climateSettings.temperatureModifier.modifyTemperature(pos, getBaseTemperature());
    int i = seaLevel + 17;                                   // threshold = seaLevel + 17 (NOT hard-coded 80)
    if (pos.getY() > i) {                                    // strict > (if_icmple skips)
        float f1 = (float)(TEMPERATURE_NOISE.getValue(
                       (double)((float)pos.getX() / 8.0F),   // i2f, fdiv 8.0f, THEN f2d !
                       (double)((float)pos.getZ() / 8.0F),
                       false) * 8.0);                        // double *8.0, then d2f
        return f - (f1 + (float)pos.getY() - (float)i) * 0.05F / 40.0F;
        // exact float op order: ((f1 + (float)y) - (float)i) * 0.05f, then / 40.0f, then f - result
    } else {
        return f;
    }
}
```
Precision landmines:
- The x/8 and z/8 divisions are performed in **float** (`i2f; ldc 8.0f; fdiv`) and only then
  widened to double (`f2d`). Do NOT compute `x/8.0` in double.
- `useNoiseOffsets` argument to `getValue` is `false` (iconst_0) → SimplexNoise xo/yo offsets are
  NOT added to the sample position.
- The final expression is all-float: `fadd, fsub, fmul 0.05f, fdiv 40.0f, fsub`.
- Threshold constants: **0.05f / 40.0f** (yes both) and **seaLevel + 17**.

### 4.3 getTemperature(BlockPos, int seaLevel) — private, per-thread cache

```java
private float getTemperature(BlockPos pos, int seaLevel) {
    long key = pos.asLong();                                  // NOTE: seaLevel NOT part of the key!
    Long2FloatLinkedOpenHashMap map = this.temperatureCache.get();
    float f = map.get(key);                                   // defaultReturnValue = Float.NaN
    if (!Float.isNaN(f)) return f;
    float f1 = getHeightAdjustedTemperature(pos, seaLevel);
    if (map.size() == 1024) map.removeFirstFloat();           // FIFO eviction at exactly 1024
    map.put(key, f1);
    return f1;
}
```
Cache mechanics:
- ThreadLocal init (lambda$new$0): `new Biome$1(this, 1024, 0.25f)` where `Biome$1` extends
  `Long2FloatLinkedOpenHashMap` and **overrides `rehash(int)` to a no-op**; then
  `defaultReturnValue(Float.NaN)`.
- Observability: the cached value equals the recomputed value (pure function of pos)
  **as long as seaLevel is constant per Biome+thread**. Because the key omits seaLevel, querying
  the same Biome instance at the same pos with a *different* seaLevel can return a stale value.
  Within one dimension seaLevel is fixed, so for worldgen parity the cache is value-transparent
  and a C port can omit it entirely.

### 4.4 warmEnoughToRain / coldEnoughToSnow / friends

```java
public boolean warmEnoughToRain(BlockPos pos, int seaLevel) {
    return getTemperature(pos, seaLevel) >= 0.15F;            // fcmpl; iflt → false when < 0.15 or NaN
}
public boolean coldEnoughToSnow(BlockPos pos, int seaLevel) {
    return !warmEnoughToRain(pos, seaLevel);
}
public boolean shouldMeltFrozenOceanIcebergSlightly(BlockPos pos, int seaLevel) {
    return getTemperature(pos, seaLevel) > 0.1F;              // strict >, fcmpl/ifle
}
public Biome.Precipitation getPrecipitationAt(BlockPos pos, int seaLevel) {
    if (!hasPrecipitation()) return Precipitation.NONE;
    return coldEnoughToSnow(pos, seaLevel) ? Precipitation.SNOW : Precipitation.RAIN;
}
public float getBaseTemperature() { return climateSettings.temperature; }
```
All temperature entry points take `(BlockPos, int seaLevel)` — the seaLevel-parameter form
(callers like `shouldFreeze`/`shouldSnow` fetch `LevelReader.getSeaLevel()`).

### 4.5 Biome$TemperatureModifier

Enum with `NONE("none")` and `FROZEN("frozen")`; `CODEC = StringRepresentable.fromEnum(values)`.

- `NONE.modifyTemperature(pos, t)` → returns `t` unchanged.
- `FROZEN.modifyTemperature(pos, t)`:
```java
public float modifyTemperature(BlockPos pos, float t) {
    double d0 = FROZEN_TEMPERATURE_NOISE.getValue((double)pos.getX() * 0.05,   // i2d — DOUBLE math here
                                                  (double)pos.getZ() * 0.05, false) * 7.0;
    double d1 = BIOME_INFO_NOISE.getValue((double)pos.getX() * 0.2,
                                          (double)pos.getZ() * 0.2, false);
    double d2 = d0 + d1;
    if (d2 < 0.3) {                                          // dcmpg/ifge → strict <
        double d3 = BIOME_INFO_NOISE.getValue((double)pos.getX() * 0.09,
                                              (double)pos.getZ() * 0.09, false);
        if (d3 < 0.8) {                                      // strict <
            return 0.2F;
        }
    }
    return t;
}
```
Constants: 0.05 / 7.0 / 0.2 / 0.3 / 0.09 / 0.8 / 0.2f. Note the FROZEN path converts int coords
directly `i2d` (double), unlike getHeightAdjustedTemperature's float-divide.

### 4.6 Biome$ClimateSettings (record) + codecs

Record components, in order: `boolean hasPrecipitation; float temperature;
Biome$TemperatureModifier temperatureModifier; float downfall;`

`ClimateSettings.CODEC` (MapCodec, RecordCodecBuilder, JSON field names):

| JSON field | codec | default |
|---|---|---|
| `has_precipitation` | Codec.BOOL, fieldOf (required) | — |
| `temperature` | Codec.FLOAT, fieldOf (required) | — |
| `temperature_modifier` | TemperatureModifier.CODEC, optionalFieldOf | `NONE` |
| `downfall` | Codec.FLOAT, fieldOf (required) | — |

`Biome.DIRECT_CODEC` (RecordCodecBuilder, 5 entries; ClimateSettings/generation/mobs are inlined
MapCodecs — flattened, no extra field name):
1. `Biome$ClimateSettings.CODEC` (inline MapCodec → the 4 fields above at top level)
2. `EnvironmentAttributeMap.CODEC_ONLY_POSITIONAL` optionalFieldOf **`"attributes"`**, default `EnvironmentAttributeMap.EMPTY`  ← **NEW vs 1.21**
3. `BiomeSpecialEffects.CODEC` fieldOf **`"effects"`** (required)
4. `BiomeGenerationSettings.CODEC` (inline MapCodec)
5. `MobSpawnSettings.CODEC` (inline MapCodec)

`NETWORK_CODEC`: 3 entries — ClimateSettings.CODEC inline, `EnvironmentAttributeMap.NETWORK_CODEC`
optionalFieldOf `"attributes"` default EMPTY, `BiomeSpecialEffects.CODEC` fieldOf `"effects"`;
generation/mob settings replaced by `BiomeGenerationSettings.EMPTY` / `MobSpawnSettings.EMPTY`.
`CODEC = RegistryFileCodec.create(Registries.BIOME, DIRECT_CODEC)`;
`LIST_CODEC = RegistryCodecs.homogeneousList(...)`.

`Biome$Precipitation`: enum `NONE("none")`, `RAIN("rain")`, `SNOW("snow")`.

---

## 5. net.minecraft.world.level.levelgen.synth.PerlinSimplexNoise

Fields: `SimplexNoise[] noiseLevels; double highestFreqValueFactor; double highestFreqInputFactor;`

### 5.1 Constructor (RNG consumption points!)

```java
public PerlinSimplexNoise(RandomSource random, List<Integer> octaves) {
    this(random, new IntRBTreeSet(octaves));   // sorted ascending, deduped
}
private PerlinSimplexNoise(RandomSource random, IntSortedSet octaves) {
    if (octaves.isEmpty()) throw new IllegalArgumentException("Need some octaves!");
    int i = -octaves.firstInt();               // -(smallest octave)
    int j = octaves.lastInt();                 // largest octave
    int k = i + j + 1;                         // total level count
    if (k < 1) throw new IllegalArgumentException("Total number of octaves needs to be >= 1");

    SimplexNoise simplex = new SimplexNoise(random);      // RNG: 3×nextDouble + 256×nextInt(256-n)
    int l = j;
    this.noiseLevels = new SimplexNoise[k];
    if (l >= 0 && l < k && octaves.contains(0)) {
        this.noiseLevels[l] = simplex;                    // index j holds octave 0
    }
    for (int i1 = l + 1; i1 < k; i1++) {                  // descending octaves below 0
        if (i1 >= 0 && octaves.contains(l - i1)) {
            this.noiseLevels[i1] = new SimplexNoise(random);   // RNG consumed
        } else {
            random.consumeCount(262);                     // skip = 262 × nextInt() (no-arg)
        }
    }
    if (l > 0) {                                          // positive octaves exist
        long seed2 = (long)(simplex.getValue(simplex.xo, simplex.yo, simplex.zo)
                            * 9.223372036854776E18);      // 2^63 as double
        WorldgenRandom random2 = new WorldgenRandom(new LegacyRandomSource(seed2));
        for (int j1 = l - 1; j1 >= 0; j1--) {
            if (j1 < k && octaves.contains(l - j1)) {
                this.noiseLevels[j1] = new SimplexNoise(random2);
            } else {
                random2.consumeCount(262);
            }
        }
    }
    this.highestFreqInputFactor = Math.pow(2.0, (double)j);
    this.highestFreqValueFactor = 1.0 / (Math.pow(2.0, (double)k) - 1.0);
}
```
For the three Biome noises:
- octaves {0}: k=1, noiseLevels=[oct0], inputFactor=1.0, valueFactor=1.0.
  RNG use: exactly one SimplexNoise ctor off `WorldgenRandom(LegacyRandomSource(seed))`.
- octaves {-2,-1,0}: i=2, j=0, k=3, noiseLevels=[oct0, oct-1, oct-2] built by 3 consecutive
  SimplexNoise ctors from the SAME random (no consumeCount happens, all present);
  inputFactor=1.0, valueFactor=1.0/7.0 = 0.14285714285714285.
- The `l > 0` reseed branch (fork via `getValue(xo,yo,zo) * 9.223372036854776E18`) is NOT taken
  for any of the Biome noises (j==0 for all three).
- `RandomSource.consumeCount(n)` (default interface method, verified) = `for(i=0;i<n;i++) nextInt();`.

### 5.2 getValue(double x, double y, boolean useNoiseOffsets)

```java
public double getValue(double x, double y, boolean useNoiseOffsets) {
    double d = 0.0;
    double f = this.highestFreqInputFactor;
    double g = this.highestFreqValueFactor;
    for (SimplexNoise level : this.noiseLevels) {         // index 0 .. k-1 in order
        if (level != null) {
            d += level.getValue(x * f + (useNoiseOffsets ? level.xo : 0.0),
                                y * f + (useNoiseOffsets ? level.yo : 0.0)) * g;
        }
        f /= 2.0;
        g *= 2.0;
    }
    return d;
}
```
Biome always calls with `useNoiseOffsets = false` → offsets never added.
Note f/g are updated even for null slots.

---

## 6. net.minecraft.world.level.levelgen.synth.SimplexNoise

Statics: `SQRT_3 = Math.sqrt(3.0)` (runtime-computed), `F2 = 0.5 * (SQRT_3 - 1.0)`,
`G2 = (3.0 - SQRT_3) / 6.0`.

`GRADIENT` (16 × int[3], only 0..11 reachable due to `% 12`):
```
{1,1,0},{-1,1,0},{1,-1,0},{-1,-1,0},
{1,0,1},{-1,0,1},{1,0,-1},{-1,0,-1},
{0,1,1},{0,-1,1},{0,1,-1},{0,-1,-1},
{1,1,0},{0,-1,1},{-1,1,0},{0,-1,-1}
```

### 6.1 Constructor (RNG consumption, exact order)

```java
public SimplexNoise(RandomSource random) {
    this.p = new int[512];
    this.xo = random.nextDouble() * 256.0;    // 1st
    this.yo = random.nextDouble() * 256.0;    // 2nd
    this.zo = random.nextDouble() * 256.0;    // 3rd
    for (int i = 0; i < 256; i++) p[i] = i;   // indices 256..511 stay 0 (unused: p(i)=p[i&255])
    for (int i = 0; i < 256; i++) {
        int j = random.nextInt(256 - i);      // 256 calls: bounds 256,255,...,1
        int k = p[i];
        p[i] = p[j + i];
        p[j + i] = k;
    }
}
private int p(int i) { return p[i & 255]; }
```

### 6.2 getCornerNoise3D(int gradIndex, double x, double y, double z, double offset)

```java
private double getCornerNoise3D(int gi, double x, double y, double z, double offset) {
    double d = offset - x * x - y * y - z * z;   // exact order: ((offset - x*x) - y*y) - z*z
    double r;
    if (d < 0.0) {                               // dcmpg/ifge: strict < → 0
        r = 0.0;
    } else {
        d = d * d;
        r = d * d * dot(GRADIENT[gi], x, y, z);  // (d*d) * dot
    }
    return r;
}
protected static double dot(int[] g, double x, double y, double z) {
    return (double)g[0] * x + (double)g[1] * y + (double)g[2] * z;   // ((gx*x) + gy*y) + gz*z
}
```

### 6.3 getValue(double x, double y) — 2D (used by PerlinSimplexNoise)

```java
public double getValue(double x, double y) {
    double s = (x + y) * F2;
    int i = Mth.floor(x + s);
    int j = Mth.floor(y + s);
    double t  = (double)(i + j) * G2;
    double X0 = (double)i - t;
    double Y0 = (double)j - t;
    double x0 = x - X0;
    double y0 = y - Y0;
    int i1, j1;
    if (x0 > y0) { i1 = 1; j1 = 0; }     // dcmpl/ifle: strict >; tie → (0,1)
    else         { i1 = 0; j1 = 1; }
    double x1 = x0 - (double)i1 + G2;
    double y1 = y0 - (double)j1 + G2;
    double x2 = x0 - 1.0 + 2.0 * G2;
    double y2 = y0 - 1.0 + 2.0 * G2;
    int ii = i & 255;
    int jj = j & 255;
    int gi0 = p(ii + p(jj)) % 12;                 // irem (Java %); operands non-negative here
    int gi1 = p(ii + i1 + p(jj + j1)) % 12;
    int gi2 = p(ii + 1 + p(jj + 1)) % 12;
    double n0 = getCornerNoise3D(gi0, x0, y0, 0.0, 0.5);   // z = 0.0, attenuation offset 0.5
    double n1 = getCornerNoise3D(gi1, x1, y1, 0.0, 0.5);
    double n2 = getCornerNoise3D(gi2, x2, y2, 0.0, 0.5);
    return 70.0 * (n0 + n1 + n2);                 // sum order (n0 + n1) + n2
}
```

### 6.4 getValue(double x, double y, double z) — 3D (used only by PerlinSimplexNoise fork-seed path)

```java
public double getValue(double x, double y, double z) {
    double s = (x + y + z) * 0.3333333333333333;          // (x+y)+z, then *1/3
    int i = Mth.floor(x + s), j = Mth.floor(y + s), k = Mth.floor(z + s);
    double t  = (double)(i + j + k) * 0.16666666666666666; // 1/6
    double x0 = x - ((double)i - t);
    double y0 = y - ((double)j - t);
    double z0 = z - ((double)k - t);
    int i1,j1,k1, i2,j2,k2;
    if (x0 >= y0) {                        // dcmpl/iflt on (x0,y0)
        if (y0 >= z0)      { i1=1;j1=0;k1=0; i2=1;j2=1;k2=0; }   // x>=y>=z
        else if (x0 >= z0) { i1=1;j1=0;k1=0; i2=1;j2=0;k2=1; }   // x>=z>y... (x>=y, y<z, x>=z)
        else               { i1=0;j1=0;k1=1; i2=1;j2=0;k2=1; }   // z>x>=y
    } else {
        if (y0 < z0)       { i1=0;j1=0;k1=1; i2=0;j2=1;k2=1; }   // z>y>x  (dcmpg/ifge)
        else if (x0 < z0)  { i1=0;j1=0;k1=0+0; i2=0;j2=1;k2=1; } // — see exact values below
        else               { i1=0;j1=1;k1=0; i2=1;j2=1;k2=0; }
    }
    // exact middle-else values from bytecode:
    //   y0 <  z0            → (0,0,1),(0,1,1)
    //   y0 >= z0 && x0 < z0 → (0,1,0),(0,1,1)
    //   y0 >= z0 && x0 >= z0→ (0,1,0),(1,1,0)
    double x1 = x0 - (double)i1 + 0.16666666666666666;
    double y1 = y0 - (double)j1 + 0.16666666666666666;
    double z1 = z0 - (double)k1 + 0.16666666666666666;
    double x2 = x0 - (double)i2 + 0.3333333333333333;
    double y2 = y0 - (double)j2 + 0.3333333333333333;
    double z2 = z0 - (double)k2 + 0.3333333333333333;
    double x3 = x0 - 1.0 + 0.5;
    double y3 = y0 - 1.0 + 0.5;
    double z3 = z0 - 1.0 + 0.5;
    int ii = i & 255, jj = j & 255, kk = k & 255;
    int gi0 = p(ii + p(jj + p(kk))) % 12;
    int gi1 = p(ii + i1 + p(jj + j1 + p(kk + k1))) % 12;
    int gi2 = p(ii + i2 + p(jj + j2 + p(kk + k2))) % 12;
    int gi3 = p(ii + 1 + p(jj + 1 + p(kk + 1))) % 12;
    double n0 = getCornerNoise3D(gi0, x0, y0, z0, 0.6);   // attenuation offset 0.6 in 3D
    double n1 = getCornerNoise3D(gi1, x1, y1, z1, 0.6);
    double n2 = getCornerNoise3D(gi2, x2, y2, z2, 0.6);
    double n3 = getCornerNoise3D(gi3, x3, y3, z3, 0.6);
    return 32.0 * (((n0 + n1) + n2) + n3);
}
```
Comparison operators in the 3D simplex ordering, verified from bytecode:
`x0 vs y0`: `dcmpl; iflt` → first branch requires `x0 >= y0` (NaN falls to else);
inside: `y0 vs z0`: `dcmpl; iflt` → `y0 >= z0`; then `x0 vs z0`: `dcmpl; iflt` → `x0 >= z0`.
Else side: `y0 vs z0`: `dcmpg; ifge` → `y0 < z0`; then `x0 vs z0`: `dcmpg; ifge` → `x0 < z0`.

---

## 7. net.minecraft.world.level.levelgen.Heightmap (+ $Types)

### 7.1 Predicates (the exact isOpaque lambdas)

From `Heightmap` statics + BootstrapMethods (verified via `javap -v`):
- `NOT_AIR` = `state -> !state.isAir()` (lambda$static$0).
- `MATERIAL_MOTION_BLOCKING` = **method reference `BlockBehaviour$BlockStateBase::blocksMotion`**
  (BootstrapMethods entry 1 → `REF_invokeVirtual ...BlockStateBase.blocksMotion:()Z`).
  It does NOT include a fluid check.

From `Heightmap$Types` static init:

| Type | id | serializationKey | Usage | isOpaque |
|---|---|---|---|---|
| WORLD_SURFACE_WG | 0 | "WORLD_SURFACE_WG" | WORLDGEN | `!state.isAir()` |
| WORLD_SURFACE | 1 | "WORLD_SURFACE" | CLIENT | `!state.isAir()` |
| OCEAN_FLOOR_WG | 2 | "OCEAN_FLOOR_WG" | WORLDGEN | `state.blocksMotion()` |
| OCEAN_FLOOR | 3 | "OCEAN_FLOOR" | LIVE_WORLD | `state.blocksMotion()` |
| MOTION_BLOCKING | 4 | "MOTION_BLOCKING" | CLIENT | `state.blocksMotion() \|\| !state.getFluidState().isEmpty()` (Types.lambda$static$0) |
| MOTION_BLOCKING_NO_LEAVES | 5 | "MOTION_BLOCKING_NO_LEAVES" | CLIENT | `(state.blocksMotion() \|\| !state.getFluidState().isEmpty()) && !(state.getBlock() instanceof LeavesBlock)` (Types.lambda$static$1) |

`keepAfterWorldgen()` = `usage != WORLDGEN` (WG maps are discarded post-gen);
`sendToClient()` = `usage == CLIENT`.

### 7.2 Storage / indexing conventions

```java
Heightmap(ChunkAccess chunk, Types type) {
    this.isOpaque = type.isOpaque();
    this.chunk = chunk;
    int bits = Mth.ceillog2(chunk.getHeight() + 1);   // e.g. 384+1=385 → 9 bits
    this.data = new SimpleBitStorage(bits, 256);
}
private static int getIndex(int x, int z) { return x + z * 16; }
private int getFirstAvailable(int index)  { return data.get(index) + chunk.getMinY(); }
public int getFirstAvailable(int x, int z){ return getFirstAvailable(getIndex(x, z)); }
public int getHighestTaken(int x, int z)  { return getFirstAvailable(getIndex(x, z)) - 1; }
private void setHeight(int x, int z, int value) { data.set(getIndex(x, z), value - chunk.getMinY()); }
```
Semantics: the stored value is `highestOpaqueY + 1` (relative to minY). `getFirstAvailable` =
first air-above-surface Y; `getHighestTaken` = Y of the topmost opaque block.

### 7.3 update(int x, int y, int z, BlockState state) — called from setBlockState paths

```java
public boolean update(int x, int y, int z, BlockState state) {
    int first = getFirstAvailable(x, z);
    if (y <= first - 2) return false;                     // if_icmpgt: proceed only when y > first-2
    if (this.isOpaque.test(state)) {
        if (y >= first) {                                 // if_icmplt skips → requires y >= first
            setHeight(x, z, y + 1);
            return true;
        }
    } else if (first - 1 == y) {                          // top block was removed/replaced by non-opaque
        BlockPos.MutableBlockPos m = new BlockPos.MutableBlockPos();
        for (int yy = y - 1; yy >= chunk.getMinY(); yy--) {
            m.set(x, yy, z);
            if (this.isOpaque.test(chunk.getBlockState(m))) {
                setHeight(x, z, yy + 1);
                return true;
            }
        }
        setHeight(x, z, chunk.getMinY());
        return true;
    }
    return false;
}
```

### 7.4 primeHeightmaps(ChunkAccess, Set<Types>)

```java
public static void primeHeightmaps(ChunkAccess chunk, Set<Types> types) {
    if (types.isEmpty()) return;
    int n = types.size();
    ObjectList<Heightmap> list = new ObjectArrayList<>(n);
    ObjectListIterator<Heightmap> it = list.iterator();      // single iterator reused throughout!
    int top = chunk.getHighestSectionPosition() + 16;
    BlockPos.MutableBlockPos m = new BlockPos.MutableBlockPos();
    for (int x = 0; x < 16; x++) {                           // outer loop = X
        for (int z = 0; z < 16; z++) {                       // inner loop = Z
            for (Types t : types) list.add(chunk.getOrCreateHeightmapUnprimed(t));
            for (int y = top - 1; y >= chunk.getMinY(); y--) {
                m.set(x, y, z);
                BlockState s = chunk.getBlockState(m);
                if (s.is(Blocks.AIR)) continue;              // fast-path skips ONLY minecraft:air
                while (it.hasNext()) {
                    Heightmap h = it.next();
                    if (h.isOpaque.test(s)) {
                        h.setHeight(x, y + 1, z);            // note arg order (x, z=..., wait) see below
                        it.remove();
                    }
                }
                if (list.isEmpty()) break;
                it.back(n);                                  // rewind iterator n positions
            }
        }
    }
}
```
Bytecode arg check on the setHeight call inside priming: `iload 7 (x); iload 8 (z); iload 9 + 1 (y+1)`
→ `setHeight(x, z, y+1)` — consistent with `getIndex(x, z)`.
The mutable pos is set as `m.set(x, y, z)` (`iload 7, iload 9, iload 8`).
Scan starts at `chunk.getHighestSectionPosition() + 16 - 1` and runs down to `chunk.getMinY()`
inclusive. Un-hit columns keep stored 0 → `getFirstAvailable` = `minY`.

### 7.5 Which types are primed / updated at which stage (call-site evidence, not deep-dived)

- Noise stage: `NoiseBasedChunkGenerator` (doFill) does
  `chunk.getOrCreateHeightmapUnprimed(Types.OCEAN_FLOOR_WG)` and
  `...(Types.WORLD_SURFACE_WG)` and calls `Heightmap.update(x, y, z, state)` on both for every
  placed block — the WG maps are built incrementally during noise, NOT primed.
- Features stage: `ChunkStatusTasks.generateFeatures` calls
  `Heightmap.primeHeightmaps(chunk, EnumSet.of(MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES, OCEAN_FLOOR, WORLD_SURFACE))`.
- `ProtoChunk.setBlockState` updates all maps in `getPersistedStatus().heightmapsAfter()`,
  priming any missing ones first (surface stage runs with the WG pair per status config;
  the status EnumSets live in ChunkStatus — out of cluster).

---

## 8. net.minecraft.world.level.dimension.DimensionType — WAY_BELOW_MIN_Y

Static init chain (all runtime-computed, verified):
```java
BITS_FOR_Y = BlockPos.PACKED_Y_LENGTH;             // = 64 - 2*26 = 12
Y_SIZE     = (1 << BITS_FOR_Y) - 32;               // 4096 - 32   = 4064
MAX_Y      = (Y_SIZE >> 1) - 1;                    //             = 2031
MIN_Y      = MAX_Y - Y_SIZE + 1;                   //             = -2032
WAY_ABOVE_MAX_Y = MAX_Y << 4;                      //             = 32496
WAY_BELOW_MIN_Y = MIN_Y << 4;                      //             = -32512
```
(`BlockPos.PACKED_HORIZONTAL_LENGTH = 1 + log2(smallestEncompassingPowerOfTwo(30000000)) = 26`.)
So **WAY_BELOW_MIN_Y = -32512** — same value as 1.21.

---

## 9. NEW / CHANGED vs Minecraft 1.21 (from bytecode only)

1. **`getHeightAdjustedTemperature` threshold is `seaLevel + 17`**, taken from the new
   `int seaLevel` parameter threaded through `getTemperature`/`warmEnoughToRain`/
   `coldEnoughToSnow`/`getPrecipitationAt` — 1.21.0 used a hard-coded `y > 80`. (63+17=80, so
   default-sea-level worlds are value-identical; non-63 sea levels diverge.)
2. **`Biome` has a new `attributes` field** (`net.minecraft.world.attribute.EnvironmentAttributeMap`,
   JSON `"attributes"`, optional, default EMPTY) in both DIRECT and NETWORK codecs — new
   environment-attribute system in 26.x.
3. `DimensionType` ctor gained `DimensionType$Skybox`, `CardinalLighting$Type`,
   `EnvironmentAttributeMap`, `HolderSet<Timeline>`, `Optional<Holder<WorldClock>>` params —
   record layout changed; Y constants unchanged.
4. BiomeManager zoom, LCG constants, getFiddle (1024 / 0.9), SimplexNoise, PerlinSimplexNoise,
   Heightmap logic: bit-identical structure to 1.21 (no drift found).
5. Nothing sulfur-cave-specific appears in this cluster.

## 10. Ambiguities & confidence notes

- `obfuscateSeed` endianness: bytecode only proves the Guava call chain; little-endian-in /
  little-endian-first-8-bytes-out is Guava's documented, stable behavior (confidence high, but
  verify once against a golden biome-zoom vector).
- Tie-break bytecode quoted verbatim in §2.2 (`dcmpl; ifle`): interpretation "update only on
  strictly smaller distance, earliest of the 8 candidates wins ties" — confidence: certain.
- `getFiddle` shift: `1: bipush 24; 3: lshr` — `lshr` is the *arithmetic* long shift in JVM
  (logical would be `lushr`); with `Math.floorMod(...,1024)` the result is well-defined for
  negative mixed seeds — confidence: certain.
- `MATERIAL_MOTION_BLOCKING`: resolved via BootstrapMethods to a bare `blocksMotion()` method
  ref (no fluid term), quoted in §7.1 — confidence: certain.
- `WorldgenRandom.next(int)` delegates to the wrapped `LegacyRandomSource.next(int)` when the
  wrapped source is a LegacyRandomSource (and bumps a call counter) — the Biome static noises
  therefore consume the plain Java-LCG stream of `LegacyRandomSource(1234/3456/2345)`.
