# A3 — WorldgenRandom seeding math: the values the order.manifest records (MC 26.2, unobfuscated server bytecode)

Source of truth: `javap -p -c -constants` (and `javap -v` for BootstrapMethods) against
`/home/ubuntu/projects/hyperchunk/tools/golden/libs/extracted/server-26.2.jar` /
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server`. All pseudocode is a 1:1 reconstruction
from bytecode. Cross-check vectors were produced by compiling a throwaway Java harness against the
real jar (`classpath.txt`) AND independently by a from-scratch Python reimplementation of the
pseudocode below — both agree bit-for-bit on every vector in §6.

Ground covered: `WorldgenRandom` (+ `$Algorithm`), the `applyBiomeDecoration` seeding call site in
`ChunkGenerator`, `XoroshiroRandomSource`, `Xoroshiro128PlusPlus`, `RandomSupport`
(+ `$Seed128bit`), `BitRandomSource` defaults, `SectionPos.origin`.
Neighbors: A-siblings in this dir cover applyBiomeDecoration loop structure / FeatureSorter /
placement internals; A7 of task8 already covered `LegacyRandomSource` + `MarsagliaPolarGaussian`
(re-verified here where load-bearing).

---

## 1. `WorldgenRandom` — class layout (26.2 is a delegating wrapper)

`public class WorldgenRandom extends LegacyRandomSource implements (via super) BitRandomSource`

Fields:
```java
private final RandomSource randomSource;   // the delegate — ALL entropy comes from here
private int count;                          // debug draw counter, no effect on output
```

### 1.1 Constructor
```java
public WorldgenRandom(RandomSource delegate) {
    super(0L);                    // LegacyRandomSource.<init>(0) calls this.setSeed(0) VIRTUALLY →
                                  // lands in WorldgenRandom.setSeed → randomSource still null → early return.
                                  // The inherited LegacyRandomSource LCG state stays 0 and is NEVER used
                                  // (next(int) is overridden, see §1.2).
    this.randomSource = delegate;
}
```

### 1.2 `next(int bits)` — the only primitive; everything else is a BitRandomSource default
```java
public int next(int bits) {
    ++this.count;
    RandomSource d = this.randomSource;
    if (d instanceof LegacyRandomSource legacy) return legacy.next(bits);        // LCG path
    return (int)(d.nextLong() >>> (64 - bits));                                  // Xoroshiro path: TOP bits
}
```

### 1.3 `setSeed(long)` (synchronized)
```java
public synchronized void setSeed(long seed) {
    if (this.randomSource == null) return;   // only during super() construction
    this.randomSource.setSeed(seed);         // FULL deterministic state reset of the delegate (§4.2)
}
```

### 1.4 Inherited defaults actually used by the features stage (BitRandomSource, bytecode-verified)
```java
default long nextLong() {                    // NOTE: two next(32) calls, hi drawn FIRST
    int i = next(32); int j = next(32);
    return ((long)i << 32) + (long)j;        // j SIGN-EXTENDED then added (not or'd)
}
default int nextInt()          { return next(32); }
default int nextInt(int bound) {
    if (bound <= 0) throw IAE;
    if ((bound & (bound-1)) == 0) return (int)((long)bound * (long)next(31) >> 31);   // pow2 fast path
    int b, v;
    do { b = next(31); v = b % bound; } while (b - v + (bound-1) < 0);                 // int-overflow reject
    return v;
}
default boolean nextBoolean()  { return next(1) != 0; }
default float  nextFloat()     { return next(24) * 5.9604645E-8f; }
default double nextDouble()    { return (((long)next(26) << 27) + next(27)) * 1.1102230246251565E-16; }
```
CRITICAL for the C impl: even with a Xoroshiro delegate, feature code calling
`random.nextInt(n)` on the WorldgenRandom goes through these LCG-shaped defaults over
`(int)(xoro_next() >>> (64-bits))` — NOT through `XoroshiroRandomSource.nextInt`'s
unsigned-128-multiply rejection algorithm (§4.4). One `next(bits)` = one full Xoroshiro draw
regardless of `bits`; `nextLong()` therefore burns TWO Xoroshiro draws and is NOT equal to the
raw `Xoroshiro128PlusPlus.nextLong()`. Verified by vector (§6.4).

`fork()` / `forkPositional()` delegate straight to `randomSource`. `nextGaussian()` is
`LegacyRandomSource.nextGaussian` → `gaussianSource` (MarsagliaPolarGaussian over `this`, i.e. over
`next(...)` above); `setSeed` on the delegate also resets the delegate's own gaussian cache (§4.2).

---

## 2. The four seeding methods (exact bytecode semantics)

### 2.1 `public long setDecorationSeed(long levelSeed, int minBlockX, int minBlockZ)`
```java
setSeed(levelSeed);                          // delegate state := f(levelSeed) only
long a = nextLong() | 1L;                    // 2 delegate draws (Xoroshiro) — §1.4 composition
long b = nextLong() | 1L;                    // 2 more
long deco = ((long)minBlockX * a + (long)minBlockZ * b) ^ levelSeed;   // lmul/ladd/lxor, all mod 2^64
setSeed(deco);
return deco;
```

### 2.2 `public void setFeatureSeed(long decorationSeed, int featureIndex, int step)`
```java
long s = decorationSeed + (long)featureIndex + (long)(10000 * step);   // 10000*step is 32-bit imul, THEN i2l
setSeed(s);
```
(imul overflow impossible in practice: step < |GenerationStep$Decoration| = small; but the C impl
should mirror the 32-bit multiply anyway.)

### 2.3 `public void setLargeFeatureSeed(long levelSeed, int x, int z)`  — structures, NOT features stage
```java
setSeed(levelSeed);
long a = nextLong();                         // NO  | 1  here (contrast §2.1)
long b = nextLong();
setSeed(((long)x * a ^ (long)z * b) ^ levelSeed);   // XOR between terms, not ADD (contrast §2.1)
```

### 2.4 `public void setLargeFeatureWithSalt(long levelSeed, int regionX, int regionZ, int salt)`
```java
setSeed((long)regionX * 341873128712L + (long)regionZ * 132897987541L + levelSeed + (long)salt);
```

### 2.5 `public static RandomSource seedSlimeChunk(int chunkX, int chunkZ, long levelSeed, long salt)`
```java
return RandomSource.createThreadLocalInstance(
    (levelSeed + (long)(chunkX*chunkX*4987142) + (long)(chunkX*5947611)
               + (long)(chunkZ*chunkZ) * 4392871L + (long)(chunkZ*389711)) ^ salt);
```
(Runtime slime spawning only — `Slime.class` is the sole outside ref; irrelevant to worldgen dumps.)

### 2.6 `WorldgenRandom$Algorithm` enum
```java
enum Algorithm { LEGACY(LegacyRandomSource::new), XOROSHIRO(XoroshiroRandomSource::new);
                 public RandomSource newInstance(long seed) { return constructor.apply(seed); } }
```
BootstrapMethods-verified constructor refs. Referenced only by `NoiseGeneratorSettings` and
`RandomState` (the `useLegacyRandomSource` noise-router plumbing, already handled in earlier
stages) — `applyBiomeDecoration` does NOT consult it; the XOROSHIRO choice there is hard-coded (§3).

---

## 3. What `ChunkGenerator.applyBiomeDecoration` actually seeds with (the call site)

`ChunkGenerator#applyBiomeDecoration(WorldGenLevel, ChunkAccess, StructureManager)`, offsets 90–130:

```java
// offsets 90..107 — delegate is HARD-CODED XoroshiroRandomSource:
WorldgenRandom random = new WorldgenRandom(new XoroshiroRandomSource(RandomSupport.generateUniqueSeed()));
// offsets 109..130:
long decorationSeed = random.setDecorationSeed(level.getSeed(), blockOrigin.getX(), blockOrigin.getZ());
```

- `blockOrigin = SectionPos.of(chunkPos, level.getMinSectionY()).origin()` →
  `BlockPos(sectionToBlockCoord(chunkX), sectionToBlockCoord(minSectionY), sectionToBlockCoord(chunkZ))`
  (SectionPos.origin bytecode verified) → **X = 16·chunkX, Z = 16·chunkZ**. minSectionY only feeds Y,
  which setDecorationSeed never reads.
- `RandomSupport.generateUniqueSeed()` = `SEED_UNIQUIFIER.updateAndGet(l -> l * 1181783497276652981L) ^ System.nanoTime()`
  (uniquifier init 8682522807148012L) — **non-deterministic but IRRELEVANT**: `setDecorationSeed`'s
  first act is `setSeed(levelSeed)`, which rebuilds the entire Xoroshiro state from `levelSeed`
  alone (§4.2). The construction seed never influences any output.
- Later, per feature: `random.setFeatureSeed(decorationSeed, featureIndex, step)` (offset 624–632 in
  the placed-feature loop; offset 285–293 in the structure loop with a per-step structure counter as
  `featureIndex`). Every feature/structure placement is thus seeded by
  `(decorationSeed, featureIndex, step)` with zero RNG state carried across features.
- Exception-path detail: the crash report prints `"Decoration Seed" = decorationSeed` (offset 792) —
  vanilla itself treats it as the reproducibility key.

---

## 4. The Xoroshiro delegate — full derivation chain (C must recompute all of this)

### 4.1 Constants (`RandomSupport`)
```
GOLDEN_RATIO_64 = 0x9E3779B97F4A7C15  (-7046029254386353131)
SILVER_RATIO_64 = 0x6A09E667F3BCC909  ( 7640891576956012809)
```

### 4.2 `XoroshiroRandomSource.setSeed(long seed)` / `new XoroshiroRandomSource(long seed)`
Both do exactly:
```java
this.randomNumberGenerator = new Xoroshiro128PlusPlus(RandomSupport.upgradeSeedTo128bit(seed));
this.gaussianSource.reset();      // setSeed only; ctor starts fresh anyway
```
```java
Seed128bit upgradeSeedTo128bitUnmixed(long seed) {
    long lo = seed ^ SILVER_RATIO_64;
    long hi = lo + GOLDEN_RATIO_64;
    return new Seed128bit(lo, hi);
}
Seed128bit upgradeSeedTo128bit(long seed) { return upgradeSeedTo128bitUnmixed(seed).mixed(); }
Seed128bit mixed() { return new Seed128bit(mixStafford13(seedLo), mixStafford13(seedHi)); }

long mixStafford13(long z) {
    z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;   // -4658895280553007687
    z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;   // -7723592293110705685
    return z ^ (z >>> 31);
}
```

### 4.3 `Xoroshiro128PlusPlus`
```java
Xoroshiro128PlusPlus(long lo, long hi) {
    this.seedLo = lo; this.seedHi = hi;
    if ((lo | hi) == 0L) { seedLo = GOLDEN_RATIO_64; seedHi = SILVER_RATIO_64; }  // zero-state guard
}
long nextLong() {
    long l = seedLo, h = seedHi;
    long n = Long.rotateLeft(l + h, 17) + l;
    h ^= l;
    seedLo = Long.rotateLeft(l, 49) ^ h ^ (h << 21);
    seedHi = Long.rotateLeft(h, 28);
    return n;
}
```
(Note: the zero guard cannot fire from `setSeed` — mixStafford13 of both words being 0
simultaneously does not occur for the lo/hi construction above; guard exists for the
codec-deserialized `(long,long)` constructor.)

### 4.4 Negative finding — methods NOT in play for the deco path
`XoroshiroRandomSource.nextInt(int)` (unsigned 32×32→64 multiply + `Integer.remainderUnsigned`
rejection), `.nextBits(int)`, `.nextFloat/nextDouble` exist but are only reachable when code holds
the XoroshiroRandomSource directly (e.g. via `fork()`/`forkPositional()` inside individual
features). Through the WorldgenRandom wrapper only `RandomSource.nextLong()` (raw) and
`setSeed(long)` are invoked — everything else routes through §1.2/§1.4.
`fork()` = `new XoroshiroRandomSource(x.nextLong(), x.nextLong())` **unmixed/unupgraded** (raw
state words); `forkPositional()` = `new XoroshiroPositionalRandomFactory(x.nextLong(), x.nextLong())`.
Both consume 2 raw draws from the delegate directly (they bypass WorldgenRandom.next, so `count`
does not tick) — matters to feature-internal replay, not to the seeding math itself.

### 4.5 Composed decoration-seed derivation, C-ready
```c
// state = two u64 words
static void xoro_from_seed(u64 seed, u64 *lo, u64 *hi) {
    u64 l = seed ^ 0x6A09E667F3BCC909ULL;
    u64 h = l + 0x9E3779B97F4A7C15ULL;
    *lo = stafford13(l); *hi = stafford13(h);        // zero-guard unreachable here
}
static u64 xoro_next(u64 *lo, u64 *hi) { /* §4.3 verbatim */ }

static u64 wgr_next_long(u64 *lo, u64 *hi) {         // WorldgenRandom.nextLong over xoro delegate
    u64 i = xoro_next(lo,hi) >> 32;                  // next(32): TOP 32 bits, draw 1
    u64 j = xoro_next(lo,hi) >> 32;                  //                        draw 2
    return ((u64)(i32)i << 32) + (u64)(i64)(i32)j;   // sign-extend j, then add (== (i<<32)+j mod 2^64)
}

u64 deco_seed(u64 level_seed, i32 block_x, i32 block_z, u64 *lo, u64 *hi) {
    xoro_from_seed(level_seed, lo, hi);
    u64 a = wgr_next_long(lo,hi) | 1;
    u64 b = wgr_next_long(lo,hi) | 1;
    u64 d = ((u64)(i64)block_x * a + (u64)(i64)block_z * b) ^ level_seed;
    xoro_from_seed(d, lo, hi);                       // trailing setSeed — state left at f(d)
    return d;
}
void feature_seed(u64 deco, i32 index, i32 step, u64 *lo, u64 *hi) {
    xoro_from_seed(deco + (u64)(i64)index + (u64)(i64)(i32)(10000 * step), lo, hi);
}
```

---

## 5. Dependency analysis — is the per-chunk deco seed order-free?

**Yes.** `setDecorationSeed` output is a pure function of `(levelSeed, 16*chunkX, 16*chunkZ)`:
- `setSeed(levelSeed)` destroys ALL prior delegate state (both words rebuilt from `levelSeed`, §4.2);
- the non-deterministic construction seed (`generateUniqueSeed`) is dead by the same token;
- `count` is write-only debug; the inherited LegacyRandomSource LCG word is inert (§1.1);
- no registry / biome / neighbor-chunk / thread input anywhere in §2.1;
- with `x=z=0` it degenerates to `deco == levelSeed` for ANY delegate (vector §6.1) — worth an
  assert in the mod.
Likewise `setFeatureSeed` is pure in `(decorationSeed, featureIndex, step)` and resets all state.
⇒ The manifest's seed column is a **consistency check, not information**. The only free variables
the manifest must carry are (a) which chunks get decorated in what order — irrelevant to output IF
the purity above holds end-to-end — and (b) per chunk, the executed sequence of
`(step, featureIndex → placed-feature/structure identity)`, because `featureIndex` is an index into
run-time-assembled per-step lists (FeatureSorter output / biome feature sets — sibling notes).

**Delegate-dependence caveat (measured, §6.2):** the SAME `setDecorationSeed(seed,x,z)` yields
DIFFERENT values on a Legacy vs Xoroshiro delegate (the `a`,`b` draws differ). 26.2 hard-codes
Xoroshiro for `applyBiomeDecoration`; `NoiseBasedChunkGenerator.spawnOriginalMobs` calls
`setDecorationSeed(region.getSeed(), pos.getMinBlockX(), pos.getMinBlockZ())` on a
`new WorldgenRandom(new LegacyRandomSource(generateUniqueSeed()))` — different derived value,
different stage (spawn), no interaction with 07_features. Do not share one C routine blindly.

### Call-site census (grep -rla over work/server, `net/` + `com/`)
- `setDecorationSeed`: **ChunkGenerator** (features, Xoroshiro), **NoiseBasedChunkGenerator**
  (spawnOriginalMobs, Legacy), WorldgenRandom itself. No other refs.
- `setFeatureSeed`: **ChunkGenerator** only (+self). No other refs.
- `setLargeFeatureSeed`: NoiseBasedChunkGenerator, Structure$GenerationContext, StructurePlacement,
  OceanMonumentStructure, StrongholdStructure (+self) — all structure machinery.
- `setLargeFeatureWithSalt`: RandomSpreadStructurePlacement, StructurePlacement (+self).
- `seedSlimeChunk`: entity/monster/cubemob/Slime only (+self).
- `applyBiomeDecoration` refs: ChunkGenerator (def), **ChunkStatusTasks** (the features
  chunk-status task; sole worldgen caller, followed by `Blender.generateBorderTicks`),
  **DebugLevelSource** (override for the debug world type — not our path). No other overrides exist.
- Pitfall: plain `grep -rl` (without `-a`) silently returned zero hits on these .class files in this
  environment; all censuses above used `grep -rla`.

---

## 6. Cross-check vectors — world seed **1234567890** (jar-executed AND python-model, bit-identical)

### 6.1 Decoration seeds (Xoroshiro delegate — the real features path)
| chunk | block origin (x,z) | decorationSeed (signed i64) |
|---|---|---|
| (0,0)   | (0,0)     | `1234567890` (degenerate: == levelSeed) |
| (1,0)   | (16,0)    | `-7060597322161768638` |
| (-1,0)  | (-16,0)   | `7060597322161768610` |
| (0,1)   | (0,16)    | `1296893547007966818` |
| (1,1)   | (16,16)   | `-5763703776066719598` |
| (-1,-1) | (-16,-16) | `5763703776066719506` |
| (10,-20)| (160,-320)| `-4310123779316098254` |

Intermediates for `levelSeed=1234567890`: `a = -3900051846512839847`, `b = 5845663369647300555`
(post-`|1`). Xoroshiro state after `setSeed(1234567890)`: `lo = -1830858561583430087`,
`hi = -5671731991842687364`. First 4 RAW `Xoroshiro128PlusPlus.nextLong()` from that state:
`-3900051847162792453, 5404024164299963211, 5845663370893641180, -1128398228392132376`
(note `a` = top32(draw1)·2^32 + top32(draw2), NOT draw1 — §1.4 composition made visible).

### 6.2 Legacy-delegate contrast (spawnOriginalMobs path — must NOT be used for features)
| block origin | Legacy decorationSeed |
|---|---|
| (0,0)  | `1234567890` |
| (16,0) | `9067626870530334850`  (vs Xoroshiro `-7060597322161768638`) |

### 6.3 Feature seeds off deco(0,0) `d = 1234567890` (pure arithmetic)
`featureSeed(d, idx, step) = d + idx + 10000*step` → e.g. `(idx=2,step=1) → 1234577892`,
`(idx=0,step=2) → 1234587890`.

### 6.4 Post-reseed draw vectors (state continuity check for the C impl)
After `setDecorationSeed(1234567890, 16, 0)` (state = f(-7060597322161768638)), next three
`WorldgenRandom.nextLong()`:
`654184706891912386, 2989823146729223195, 7180858201083779893`.
Then `setFeatureSeed(-7060597322161768638, 3, 6)`: `nextInt(16) = 3`, `nextInt(16) = 0`,
`nextFloat = 0.6951269f` (bits of `0.6951268911361694…` truncated to float via `next(24)*2^-24`).

---

## 7. Implications for the order manifest (ADR-007 Tier-2 gate)

1. **Seed column = validation, not input.** Record per chunk: `chunkX, chunkZ, decorationSeed`.
   The C replayer recomputes `deco_seed(levelSeed, 16*cx, 16*cz)` (§4.5) and asserts equality —
   any mismatch means the C Xoroshiro/mix/compose chain is wrong, caught before feature diffing.
   Include the chunk-(0,0) degenerate case in unit tests plus the §6.1 table as fixed vectors.
2. **What actually needs recording:** the executed `(step, featureIndex, feature-id)` triples per
   chunk (and the structure-loop counter events), because `featureIndex` binds to run-time list
   contents; the RNG contributes nothing order-dependent. Per-chunk decoration order across chunks
   is the remaining free variable exactly as hypothesized — nothing in the seeding math couples
   chunks.
3. **Hook placement:** wrapping/observing `setFeatureSeed` + `setDecorationSeed` on the single
   WorldgenRandom instance created per `applyBiomeDecoration` call captures every seeding event of
   the features stage (census §5: no other worldgen callers). `spawnOriginalMobs` fires
   `setDecorationSeed` too — filter by delegate type or by stage if hooking the method globally.
4. **C RNG contract for features:** implement WorldgenRandom-over-Xoroshiro as §1.2+§1.4
   (top-bits `next(bits)`, two-draw `nextLong`, BitRandomSource `nextInt` bound logic), NOT
   XoroshiroRandomSource's native ops; implement `fork/forkPositional` as raw two-draw state
   captures (§4.4) for features that use them.
