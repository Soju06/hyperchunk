# A1 — `net.minecraft.world.level.levelgen.SurfaceSystem` (MC 26.2, unobfuscated server bytecode)

Source of truth: `javap -p -c -constants` (and `javap -v` for BootstrapMethods) against
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server`. All pseudocode is a 1:1 reconstruction
from bytecode. No vanilla-source guessing.

Companion notes: A2 (SurfaceRules$Context — updateXZ/updateY/getMinSurfaceLevel/lazy conditions),
A5 (RandomState — who constructs SurfaceSystem, `getOrCreateNoise` seeding, Noises key table).
This note does NOT re-cover their ground; cross-references are marked "(A2 §x)" / "(A5 §x)".

Class files: `SurfaceSystem.class` + one anonymous inner `SurfaceSystem$1` (a `BlockColumn` view
over the chunk). There is NO other inner class (no records, no codecs — constant pool has no CODEC).

---

## 1. Static fields (`static {}` exact order) and instance fields

```java
public class SurfaceSystem {
    // static{} putstatic order — all defaultBlockState() of:
    private static final BlockState WHITE_TERRACOTTA      = Blocks.DYED_TERRACOTTA.white();     // #1
    private static final BlockState ORANGE_TERRACOTTA     = Blocks.DYED_TERRACOTTA.orange();    // #2
    private static final BlockState TERRACOTTA            = Blocks.TERRACOTTA;                  // #3 (plain Block field)
    private static final BlockState YELLOW_TERRACOTTA     = Blocks.DYED_TERRACOTTA.yellow();    // #4
    private static final BlockState BROWN_TERRACOTTA      = Blocks.DYED_TERRACOTTA.brown();     // #5
    private static final BlockState RED_TERRACOTTA        = Blocks.DYED_TERRACOTTA.red();       // #6
    private static final BlockState LIGHT_GRAY_TERRACOTTA = Blocks.DYED_TERRACOTTA.lightGray(); // #7
    private static final BlockState PACKED_ICE            = Blocks.PACKED_ICE;                  // #8
    private static final BlockState SNOW_BLOCK            = Blocks.SNOW_BLOCK;                  // #9

    // instance fields (declaration order)
    private final BlockState defaultBlock;
    private final int seaLevel;
    private final BlockState[] clayBands;
    private final NormalNoise clayBandsOffsetNoise;
    private final NormalNoise badlandsPillarNoise;
    private final NormalNoise badlandsPillarRoofNoise;
    private final NormalNoise badlandsSurfaceNoise;
    private final NormalNoise icebergPillarNoise;
    private final NormalNoise icebergPillarRoofNoise;
    private final NormalNoise icebergSurfaceNoise;
    private final PositionalRandomFactory noiseRandom;
    private final NormalNoise surfaceNoise;
    private final NormalNoise surfaceSecondaryNoise;
}
```

26.2 delta: dyed terracotta states come from `Blocks.DYED_TERRACOTTA` (a
`net.minecraft.world.level.block.ColorCollection`, methods `white()/orange()/yellow()/brown()/red()/lightGray()`
returning `Object` cast to `Block`) — new block-registry organization; the resulting block states are
the same colored terracotta defaults.

### 1.1 Constructor — exact field-write order + noise creation

Descriptor: `<init>(RandomState, BlockState, int, PositionalRandomFactory)`.
Called exactly once, from the `RandomState` private ctor step (7), with args
`(this /*RandomState*/, settings.defaultBlock(), settings.seaLevel(), this.random /*ROOT positional factory*/)` (A5 §1.3/§9.2).

putfield order (authoritative):

```java
public SurfaceSystem(RandomState randomState, BlockState defaultBlock, int seaLevel, PositionalRandomFactory noiseRandom) {
    super();
    this.defaultBlock = defaultBlock;                                          // (1)
    this.seaLevel = seaLevel;                                                  // (2)
    this.noiseRandom = noiseRandom;                                            // (3)
    this.clayBandsOffsetNoise = randomState.getOrCreateNoise(Noises.CLAY_BANDS_OFFSET);  // (4) "minecraft:clay_bands_offset"
    this.clayBands = generateBands(                                            // (5)
        noiseRandom.fromHashOf(Identifier.withDefaultNamespace("clay_bands")));
        //          ^^^ *** RNG: fromHashOf("minecraft:clay_bands") — NOTE: "clay_bands",
        //              a DIFFERENT string from the noise key "clay_bands_offset" ***
    this.surfaceNoise           = randomState.getOrCreateNoise(Noises.SURFACE);              // (6)  "minecraft:surface"
    this.surfaceSecondaryNoise  = randomState.getOrCreateNoise(Noises.SURFACE_SECONDARY);    // (7)  "minecraft:surface_secondary"
    this.badlandsPillarNoise    = randomState.getOrCreateNoise(Noises.BADLANDS_PILLAR);      // (8)  "minecraft:badlands_pillar"
    this.badlandsPillarRoofNoise= randomState.getOrCreateNoise(Noises.BADLANDS_PILLAR_ROOF); // (9)  "minecraft:badlands_pillar_roof"
    this.badlandsSurfaceNoise   = randomState.getOrCreateNoise(Noises.BADLANDS_SURFACE);     // (10) "minecraft:badlands_surface"
    this.icebergPillarNoise     = randomState.getOrCreateNoise(Noises.ICEBERG_PILLAR);       // (11) "minecraft:iceberg_pillar"
    this.icebergPillarRoofNoise = randomState.getOrCreateNoise(Noises.ICEBERG_PILLAR_ROOF);  // (12) "minecraft:iceberg_pillar_roof"
    this.icebergSurfaceNoise    = randomState.getOrCreateNoise(Noises.ICEBERG_SURFACE);      // (13) "minecraft:iceberg_surface"
}
```

- Each `getOrCreateNoise` = ConcurrentHashMap dedup + `NormalNoise.create(random.fromHashOf("minecraft:<path>"), params)` (A5 §1.4/§3.2).
- The `noiseRandom` factory is the ROOT positional factory (`newInstance(seed).forkPositional()`),
  NOT a "surface"-forked one — so `fromHashOf("minecraft:clay_bands")` and every `noiseRandom.at(x,0,z)`
  below hash directly off the world-seed-level factory.
- All noise instances are created eagerly here; since RandomState builds SurfaceSystem BEFORE the
  router `mapAll` (A5 §1.3), the 9 surface-noise keys are inserted into `noiseIntances` before any
  router noises (only matters for map iteration order, i.e. not at all for output).

---

## 2. `generateBands(RandomSource)` / `makeBands(...)` — clay bands array

Array size **192** (`sipush 192; anewarray BlockState`). RNG source: the single
`fromHashOf("minecraft:clay_bands")` RandomSource, consumed strictly sequentially as follows.

```java
private static BlockState[] generateBands(RandomSource random) {
    BlockState[] bands = new BlockState[192];
    Arrays.fill(bands, TERRACOTTA);

    // pass 1: orange specks
    for (int i = 0; i < bands.length; /* i advances inside */) {
        i += random.nextInt(5) + 1;              // *** RNG #1..k: nextInt(5) each iteration ***
        if (i < bands.length) {
            bands[i] = ORANGE_TERRACOTTA;
        }
        ++i;                                     // iinc 2,1 AFTER the conditional store
    }
    // pass 2..4: colored bands
    makeBands(random, bands, 1, YELLOW_TERRACOTTA);
    makeBands(random, bands, 2, BROWN_TERRACOTTA);
    makeBands(random, bands, 1, RED_TERRACOTTA);

    // pass 5: white bands with light-gray borders
    int whiteCount = random.nextIntBetweenInclusive(9, 15);   // *** RNG: nextInt(7) + 9 ***
    int placed = 0;
    for (int k = 0; placed < whiteCount && k < bands.length; /* k advances inside */) {
        bands[k] = WHITE_TERRACOTTA;
        if (k - 1 > 0 && random.nextBoolean()) {              // *** RNG: nextBoolean ONLY when k-1 > 0 ***
            bands[k - 1] = LIGHT_GRAY_TERRACOTTA;             //     (bytecode: iload4;iconst_1;isub;ifle skip)
        }                                                     //     NOTE: `> 0`, so k==1 does NOT draw
        if (k + 1 < bands.length && random.nextBoolean()) {   // *** RNG: nextBoolean ONLY when k+1 < 192 ***
            bands[k + 1] = LIGHT_GRAY_TERRACOTTA;
        }
        ++placed;
        k += random.nextInt(16) + 4;                          // *** RNG: nextInt(16) ***
    }
    return bands;
}

private static void makeBands(RandomSource random, BlockState[] bands, int minSize, BlockState state) {
    int bandCount = random.nextIntBetweenInclusive(6, 15);    // *** RNG: nextInt(10) + 6 ***
    for (int i = 0; i < bandCount; ++i) {
        int width = minSize + random.nextInt(3);              // *** RNG: nextInt(3) ***
        int start = random.nextInt(bands.length);             // *** RNG: nextInt(192) ***
        for (int m = 0; start + m < bands.length && m < width; ++m) {
            bands[start + m] = state;                         // clipped at array end, no wrap
        }
    }
}
```

`RandomSource.nextIntBetweenInclusive(min,max)` is a default interface method, verified:

```
0: aload_0; 1: iload_2; 2: iload_1; 3: isub; 4: iconst_1; 5: iadd
6: invokeinterface nextInt:(I)I; 11: iload_1; 12: iadd; 13: ireturn      // nextInt(max-min+1) + min
```

Exact per-column loop-1 bound quote (the `if i < len` store, then unconditional `++i`):

```
22: iload_2  23: aload_0  24: iconst_5  25: invokeinterface nextInt:(I)I
30: iconst_1 31: iadd 32: iadd 33: istore_2          // i = i + nextInt(5) + 1
34: iload_2 35: aload_1 36: arraylength 37: if_icmpge 46
40: aload_1 41: iload_2 42: getstatic ORANGE_TERRACOTTA 45: aastore
46: iinc 2, 1  49: goto 16
```

Total sequential draw order: `[nextInt(5)]*` → `nextInt(10)`, then per yellow band
`(nextInt(3), nextInt(192))` → same for brown (minSize 2) → same for red (minSize 1) →
`nextInt(7)` → per white band `(nextBoolean?, nextBoolean?, nextInt(16))` with the two booleans
conditional as quoted. This matches the 1.21 folklore algorithm 1:1 (size 192, same bounds).

---

## 3. `getBand(int x, int y, int z)`

```java
protected BlockState getBand(int x, int y, int z) {
    int offset = (int) Math.round(this.clayBandsOffsetNoise.getValue((double)x, 0.0, (double)z) * 4.0);
    return this.clayBands[(y + offset + this.clayBands.length) % this.clayBands.length];
}
```

Bytecode quote (rounding + index math — every op):

```
 9: invokevirtual NormalNoise.getValue:(DDD)D      // sample at (x, 0.0, z); y of the CALL is dconst_0
12: ldc2_w 4.0d  15: dmul
16: invokestatic Math.round:(D)J  19: l2i          // Math.round(double) -> long, then NARROWED l2i
22..41: clayBands[ (y + offset + 192) % 192 ]      // iadd, iadd(arraylength), irem(arraylength)
```

- `Math.round(double)` = `floor(v + 0.5)` semantics (JDK), result long, truncated `l2i` (safe range here).
- `%` is Java `irem` (truncated, sign of dividend). `y + offset + 192` is the only negative-guard;
  for y < -(192 + offset) the index would go negative — cannot happen for band rules (they run near
  badlands surface heights), but replicate irem exactly anyway.
- The noise argument x/z are the WORLD block coords passed by the caller (band rules pass blockX/blockZ),
  y is the caller's y (plus rule offset) — this method itself does no coordinate transform beyond above.

---

## 4. `getSurfaceDepth(int x, int z)` and `getSurfaceSecondary(int x, int z)`

```java
protected int getSurfaceDepth(int x, int z) {
    double n = this.surfaceNoise.getValue((double)x, 0.0, (double)z);       // "minecraft:surface" noise
    return (int)(n * 2.75 + 3.0
                 + this.noiseRandom.at(x, 0, z).nextDouble() * 0.25);       // *** RNG (positional) ***
}
```

Bytecode quote — constants, RNG point, and the cast (this is `d2i` = TRUNCATION toward zero, NOT floor):

```
 9: invokevirtual NormalNoise.getValue:(DDD)D          // getValue(x, 0.0, z)
13: dload_3  14: ldc2_w 2.75d  17: dmul
18: ldc2_w 3.0d  21: dadd
22: aload_0 getfield noiseRandom
26: iload_1 27: iconst_0 28: iload_2
29: invokeinterface PositionalRandomFactory.at:(III)Lnet/minecraft/util/RandomSource;   // at(x, 0, z)
34: invokeinterface RandomSource.nextDouble:()D
39: ldc2_w 0.25d  42: dmul  43: dadd
44: d2i  45: ireturn
```

- Formula: `(int)(surfaceNoise(x,0,z) * 2.75 + 3.0 + at(x,0,z).nextDouble() * 0.25)`.
- `at(x, 0, z)` forks a FRESH RandomSource from the root positional factory at position (x,0,z);
  exactly one `nextDouble()` is consumed from it. Since noise∈~[-1,1] the sum stays positive in
  practice, so d2i≈floor; but a sufficiently negative noise value would truncate toward 0 — keep d2i.
- Called once per column, eagerly, from `Context.updateXZ` (A2 §1.2); also re-derived inside
  `Context.getMinSurfaceLevel` via the stored `surfaceDepth` (A2 §1.8).

```java
protected double getSurfaceSecondary(int x, int z) {
    return this.surfaceSecondaryNoise.getValue((double)x, 0.0, (double)z);   // "minecraft:surface_secondary"
}
```

(No scaling, no RNG. Lazily consumed via `Context.getSurfaceSecondary` — A2 §1.4.)

---

## 5. Small helpers

```java
private boolean isStone(BlockState state) {
    return !state.isAir() && state.getFluidState().isEmpty();
    // bytecode: isAir() ifne ->false ; getFluidState().isEmpty() ifeq ->false ; else true
}

public int getSeaLevel() { return this.seaLevel; }    // used by Context.getSeaLevel() (A2 §1.6)
```

---

## 6. `SurfaceSystem$1` — the BlockColumn view (anonymous, created per buildSurface call)

`class SurfaceSystem$1 implements net.minecraft.world.level.chunk.BlockColumn`, capturing
`(ChunkAccess protoChunk, BlockPos.MutableBlockPos columnPos, ChunkPos chunkPos)` (+ outer `this`
consumed only via `Objects.requireNonNull`).

```java
public BlockState getBlock(int y) {
    return protoChunk.getBlockState(columnPos.setY(y));   // columnPos X/Z pre-set by buildSurface
}

public void setBlock(int y, BlockState state) {
    LevelHeightAccessor h = protoChunk.getHeightAccessorForGeneration();
    if (h.isInsideBuildHeight(y)) {
        protoChunk.setBlockState(columnPos.setY(y), state);        // 2-ARG overload
        if (!state.getFluidState().isEmpty()) {
            protoChunk.markPosForPostProcessing(columnPos);        // fluids scheduled for post-processing
        }
    }
}

public String toString() { return "ChunkBlockColumn " + chunkPos; }   // indy recipe "ChunkBlockColumn "
```

`ChunkAccess.setBlockState(BlockPos, BlockState)` (2-arg) verified:

```
3: iconst_3
4: invokevirtual setBlockState:(...;I)LBlockState;    // delegates with int flags = 3
```

26.2 delta: the 3rd param of the abstract `setBlockState` is now an `int` (flags, value 3 here);
in 1.21 it was `boolean isMoving = false`. `ProtoChunk.setBlockState(BlockPos, BlockState, int)` is
large (light-engine + heightmap priming via `getPersistedStatus().heightmapsAfter()`); NOT
disassembled in full here — flagged. For parity the critical effects are: (a) out-of-build-height
returns VOID_AIR without writing (redundant with the isInsideBuildHeight guard above),
(b) it PRIMES/UPDATES the WG heightmaps, which buildSurface re-reads after the badlands pre-pass
(§7 step 4) — so heightmap update semantics must match.

- `getBlock` has NO height guard — callers may read y = minY-1 (see §7 stone-depth scan);
  `ProtoChunk.getBlockState` returns `Blocks.VOID_AIR.defaultBlockState()` outside build height
  (verified in ProtoChunk.setBlockState prologue pattern; getBlockState has the same
  `isOutsideBuildHeight -> VOID_AIR` shape in ProtoChunk). VOID_AIR `isAir()` is true.

---

## 7. `buildSurface(...)` — COMPLETE body

Descriptor:
`buildSurface(RandomState, BiomeManager, boolean legacyRandom, WorldGenerationContext, ChunkAccess, NoiseChunk, SurfaceRules$RuleSource, Set<Holder<Biome>>)V`
(caller wiring: A5 §4.2 — `legacyRandom` = `NoiseGeneratorSettings.useLegacyRandomSource()`).

Full 1:1 reconstruction (locals annotated with their bytecode slot):

```java
public void buildSurface(RandomState randomState /*1*/, BiomeManager biomeManager /*2*/,
                         boolean legacyRandom /*3*/, WorldGenerationContext wgContext /*4*/,
                         ChunkAccess chunk /*5*/, NoiseChunk noiseChunk /*6*/,
                         SurfaceRules.RuleSource ruleSource /*7*/, Set<Holder<Biome>> possibleBiomes /*8*/) {
    BlockPos.MutableBlockPos columnPos = new BlockPos.MutableBlockPos();          // slot 9
    ChunkPos chunkPos = chunk.getPos();                                           // slot 10
    int minBlockX = chunkPos.getMinBlockX();                                      // slot 11
    int minBlockZ = chunkPos.getMinBlockZ();                                      // slot 12
    BlockColumn column = new SurfaceSystem$1(this, chunk, columnPos, chunkPos);   // slot 13 (§6)
    SurfaceRules.Context ctx = new SurfaceRules.Context(                          // slot 14 (A2 §1.1)
        this, randomState, chunk, noiseChunk,
        biomeManager::getBiome,        // indy #0 verified: REF_invokeVirtual BiomeManager.getBiome:(BlockPos)Holder
                                       // (preceded by Objects.requireNonNull(biomeManager))
        wgContext, possibleBiomes);
    SurfaceRules.SurfaceRule rule = (SurfaceRules.SurfaceRule) ruleSource.apply(ctx);  // slot 15
    BlockPos.MutableBlockPos biomePos = new BlockPos.MutableBlockPos();           // slot 16

    for (int lx = 0; lx < 16; ++lx) {              // slot 17 — X OUTER
        for (int lz = 0; lz < 16; ++lz) {          // slot 18 — Z INNER
            int worldX = minBlockX + lx;                                          // slot 19
            int worldZ = minBlockZ + lz;                                          // slot 20

            // (1) height read #1 — LOCAL coords, pre-badlands
            int h1 = chunk.getHeight(Heightmap.Types.WORLD_SURFACE_WG, lx, lz) + 1;   // slot 21

            // (2) point the BlockColumn at this column
            columnPos.setX(worldX).setZ(worldZ);

            // (3) biome lookup — y depends on legacyRandom!
            Holder<Biome> biome = biomeManager.getBiome(                          // slot 22
                biomePos.set(worldX, legacyRandom ? 0 : h1, worldZ));

            // (4) eroded-badlands PRE-pass (biome keyed by ResourceKey identity)
            if (biome.is(Biomes.ERODED_BADLANDS)) {
                this.erodedBadlandsExtension(column, worldX, worldZ, h1, chunk);  // chunk as LevelHeightAccessor
            }

            // (5) height read #2 — AFTER the badlands pass (it may have raised the heightmap!)
            int h2 = chunk.getHeight(Heightmap.Types.WORLD_SURFACE_WG, lx, lz) + 1;   // slot 23

            // (6) per-column context update (eager getSurfaceDepth — RNG here, §4)
            ctx.updateXZ(worldX, worldZ);

            // (7) top-down stone-depth state machine
            int stoneDepthAbove = 0;                          // slot 24
            int waterHeight = Integer.MIN_VALUE;              // slot 25  (sentinel -2147483648)
            int stoneBottom = Integer.MAX_VALUE;              // slot 26  (cache of "y of lowest stone in current run")
            int minY = chunk.getMinY();                       // slot 27

            for (int y = h2; y >= minY; --y) {                // slot 28
                BlockState state = column.getBlock(y);        // slot 29
                if (state.isAir()) {
                    stoneDepthAbove = 0;
                    waterHeight = Integer.MIN_VALUE;
                    continue;
                }
                if (!state.getFluidState().isEmpty()) {       // any fluid-bearing state (incl. waterlogged)
                    if (waterHeight == Integer.MIN_VALUE) {
                        waterHeight = y + 1;                  // first fluid surface from above
                    }
                    continue;
                }
                // solid, fluid-free block:
                if (stoneBottom >= y) {                       // cache invalid — rescan downward
                    stoneBottom = DimensionType.WAY_BELOW_MIN_Y;      // = MIN_Y<<4 = -32512 (see below)
                    for (int k = y - 1; k >= minY - 1; --k) {         // NOTE: goes to minY-1 INCLUSIVE
                        BlockState below = column.getBlock(k);        // at minY-1: VOID_AIR (not stone)
                        if (!this.isStone(below)) { stoneBottom = k + 1; break; }
                    }
                }
                ++stoneDepthAbove;
                int stoneDepthBelow = y - stoneBottom + 1;    // slot 30
                ctx.updateY(stoneDepthAbove, stoneDepthBelow, waterHeight, y);    // (A2 §1.3 arg order)

                if (state != this.defaultBlock) {             // REFERENCE equality (if_acmpeq)!
                    continue;                                 // only the exact default state is replaceable
                }
                BlockState result = rule.tryApply(worldX, y, worldZ);   // slot 31
                if (result != null) {
                    column.setBlock(y, result);               // -> setBlockState(pos, result, 3) (§6)
                }
            }

            // (8) frozen-ocean POST-pass
            if (biome.is(Biomes.FROZEN_OCEAN) || biome.is(Biomes.DEEP_FROZEN_OCEAN)) {
                this.frozenOceanExtension(ctx.getMinSurfaceLevel(), (Biome) biome.value(),
                                          column, biomePos, worldX, worldZ, h1);
                //                        ^^^ h1 (slot 21) — the PRE-badlands height; and the shared
                //                        biomePos mutable is reused as scratch inside (§9)
            }
        }
    }
}
```

Key bytecode quotes:

- Biome-sample y select (`legacyRandom ? 0 : h1`):
  ```
  167: iload_3  168: ifeq 175        // legacyRandom == false -> 175
  171: iconst_0 172: goto 177        // true  -> y = 0
  175: iload 21                      // false -> y = h1 (heightmap+1)
  177: iload 20  179: ...set:(III)  182: BiomeManager.getBiome
  ```
- State machine air branch: `281: isAir ifeq 297; 287: iconst_0 istore 24; 290: ldc MIN_VALUE istore 25; goto 454(y--)`.
- Fluid branch: `302: FluidState.isEmpty ifne 324; 308: iload 25; ldc -2147483648; if_icmpne 454; 315: y+1 -> waterHeight`.
- stoneBottom cache test: `324: iload 26; 326: iload 28; 328: if_icmplt 386` — rescan iff `stoneBottom >= y`.
- Rescan bound: `342: iload 30; 344: iload 27; 346: iconst_1; 347: isub; 348: if_icmplt 386` — loop body
  runs for `k >= minY - 1` inclusive (so `column.getBlock(minY-1)` IS invoked; ProtoChunk returns
  VOID_AIR out of bounds → not stone → `stoneBottom = minY`). If (impossibly) everything down to
  minY-1 were stone, stoneBottom stays `WAY_BELOW_MIN_Y`.
- Default-block gate: `411: aload 29; 414: getfield defaultBlock; 417: if_acmpeq 423; 420: goto 454` —
  identity compare against the ctor-captured `settings.defaultBlock()` state.
- tryApply/write: `431: SurfaceRule.tryApply:(III)LBlockState;` args `(worldX, y, worldZ)`;
  `440: ifnull 454; 449: BlockColumn.setBlock:(I,BlockState)`.

`DimensionType.WAY_BELOW_MIN_Y` is computed in `DimensionType.static{}`:
`BITS_FOR_Y = BlockPos.PACKED_Y_LENGTH` (= 64 − 2·(1+log2(ceilPow2(30000000))) = 64 − 2·26 = **12**),
`Y_SIZE = (1<<12) − 32 = 4064`, `MAX_Y = (4064>>1) − 1 = 2031`, `MIN_Y = 2031 − 4064 + 1 = −2032`,
`WAY_BELOW_MIN_Y = MIN_Y << 4 = **−32512**`.

Walk-order summary: **x outer 0..15, z inner 0..15; per column: h1 → biome → (badlands) → h2 →
updateXZ → y from h2 down to minY inclusive → (frozen ocean)**. The y-loop upper bound is the
heightmap read AFTER the badlands pass; there is no minSurfaceLevel-based lower bound in the loop
itself (early-out below the surface comes from the `abovePreliminarySurface` condition inside the
rule tree, A2 §6).

---

## 8. `erodedBadlandsExtension(BlockColumn column, int x, int z, int height /*h1*/, LevelHeightAccessor level)`

```java
private void erodedBadlandsExtension(BlockColumn column, int x, int z, int height, LevelHeightAccessor level) {
    double d0 = 0.2;   // stored, then inlined as literals (javac artifact)
    double pillar = Math.min(
        Math.abs(this.badlandsSurfaceNoise.getValue((double)x, 0.0, (double)z) * 8.25),
        this.badlandsPillarNoise.getValue((double)x * 0.2, 0.0, (double)z * 0.2) * 15.0);
    if (pillar <= 0.0) return;                       // bytecode: dconst_0 dcmpg ifgt 61 — return unless > 0.0
    double d1 = 0.75, d2 = 1.5;                      // stored-then-literal, same artifact
    double roof = Math.abs(this.badlandsPillarRoofNoise.getValue((double)x * 0.75, 0.0, (double)z * 0.75) * 1.5);
    double top = 64.0 + Math.min(pillar * pillar * 2.5, Math.ceil(roof * 50.0) + 24.0);
    int topY = Mth.floor(top);                       // Mth.floor = (int)Math.floor(d) (d2i after Math.floor)
    if (height > topY) return;                       // bytecode: iload4; iload18; if_icmple 146 — return when h1 > topY

    // scan 1: bail out if water sits above the terrain within the pillar range
    for (int k = topY; k >= level.getMinY(); --k) {
        BlockState s = column.getBlock(k);
        if (s.is(this.defaultBlock.getBlock())) break;   // Block-level .is (NOT state identity)
        if (s.is(Blocks.WATER)) return;                  // any water in the way -> no pillar
    }
    // scan 2: fill air with defaultBlock from topY down until first non-air
    for (int k = topY; k >= level.getMinY() && column.getBlock(k).isAir(); --k) {
        column.setBlock(k, this.defaultBlock);
    }
}
```

Constants (all `ldc2_w`-verified): 8.25, 0.2 (both coords), 15.0, 0.75, 1.5, 64.0, 2.5, 50.0, 24.0.
No RNG. Note all three noises sample at y=0.0; the pillar/roof noises use SCALED x/z inputs.
Trigger: `biome.is(Biomes.ERODED_BADLANDS)` on the (heightmap-or-0, legacy-dependent) biome sample.
Runs BEFORE updateXZ / the rule pass; its `setBlock` calls update heightmaps, hence the h2 re-read.

---

## 9. `frozenOceanExtension(int minSurfaceLevel, Biome biome, BlockColumn column, BlockPos.MutableBlockPos pos, int x, int z, int height /*h1*/)`

```java
private void frozenOceanExtension(int minSurfaceLevel, Biome biome, BlockColumn column,
                                  BlockPos.MutableBlockPos pos, int x, int z, int height) {
    double d0 = 1.28;    // javac stored-literal artifact
    double iceberg = Math.min(
        Math.abs(this.icebergSurfaceNoise.getValue((double)x, 0.0, (double)z) * 8.25),
        this.icebergPillarNoise.getValue((double)x * 1.28, 0.0, (double)z * 1.28) * 15.0);
    if (iceberg <= 1.8) return;                      // dcmpg ifgt 67 — return unless > 1.8
    double d1 = 1.17, d2 = 1.5;                      // artifacts
    double roof = Math.abs(this.icebergPillarRoofNoise.getValue((double)x * 1.17, 0.0, (double)z * 1.17) * 1.5);
    double maxTop = Math.min(iceberg * iceberg * 1.2, Math.ceil(roof * 40.0) + 14.0);

    if (biome.shouldMeltFrozenOceanIcebergSlightly(pos.set(x, this.seaLevel, z), this.seaLevel)) {
        maxTop -= 2.0;                               // melt: getTemperature(pos, seaLevel) > 0.1f (verified, §10)
    }
    double bottom;                                   // slot 12
    if (maxTop > 2.0) {                              // dcmpl ifle — else-branch when <= 2.0
        bottom = (double)this.seaLevel - maxTop - 7.0;
        maxTop = maxTop + (double)this.seaLevel;
    } else {
        maxTop = 0.0;
        bottom = 0.0;
    }
    double top = maxTop;                             // slot 22

    // *** RNG: fresh positional fork at (x, 0, z) — SAME position seed as getSurfaceDepth's fork,
    //     but a NEW RandomSource starting from sequence position 0 ***
    RandomSource random = this.noiseRandom.at(x, 0, z);            // slot 24
    int maxSnowDepth = 2 + random.nextInt(4);                      // slot 25  *** RNG draw 1: nextInt(4) ***
    int minSnowY = this.seaLevel + 18 + random.nextInt(10);        // slot 26  *** RNG draw 2: nextInt(10) ***
    int snowPlaced = 0;                                            // slot 27

    for (int y = Math.max(height, (int)top + 1); y >= minSurfaceLevel; --y) {   // slot 28; (int)top = d2i TRUNCATION
        boolean place = false;
        // condition A (above water): air, below iceberg top, 99% chance
        if (column.getBlock(y).isAir() && y < (int)top
                && random.nextDouble() > 0.01) {                   // *** RNG: nextDouble ONLY when air && y < top ***
            place = true;
        }
        // condition B (underwater): water, inside [bottom+1, seaLevel-1], bottom != 0, 85% chance
        else if (column.getBlock(y).is(Blocks.WATER)              // getBlock re-invoked (2nd call)
                && y > (int)bottom && y < this.seaLevel
                && bottom != 0.0                                   // dcmpl ifeq — skip when exactly 0.0
                && random.nextDouble() > 0.15) {                   // *** RNG: nextDouble ONLY when all prior hold ***
            place = true;
        }
        if (place) {
            if (snowPlaced <= maxSnowDepth && y > minSnowY) {      // if_icmpgt / if_icmple — both strict as shown
                column.setBlock(y, SNOW_BLOCK);
                ++snowPlaced;
            } else {
                column.setBlock(y, PACKED_ICE);
            }
        }
    }
}
```

Exact branch quotes (the two chance rolls and their guards):

```
289: isAir ifeq 317                       // not air -> try water branch
295: iload 28; 297: dload 22; 299: d2i; 300: if_icmpge 317    // y >= (int)top -> water branch (no draw)
303..314: nextDouble; ldc2_w 0.01; dcmpl; ifgt 372            // > 0.01 -> place

317: getBlock(y).is(Blocks.WATER) ifeq 414                    // skip
334: iload 28; dload 12; d2i; if_icmple 414                   // y <= (int)bottom -> skip
342: iload 28; getfield seaLevel; if_icmpge 414               // y >= seaLevel -> skip
351: dload 12; dconst_0; dcmpl; ifeq 414                      // bottom == 0.0 -> skip
358..369: nextDouble; ldc2_w 0.15; dcmpl; ifle 414            // <= 0.15 -> skip

372: iload 27; iload 25; if_icmpgt 403                        // snowPlaced > maxSnowDepth -> packed ice
379: iload 28; iload 26; if_icmple 403                        // y <= minSnowY -> packed ice
386..397: setBlock(y, SNOW_BLOCK); iinc 27,1
403..409: setBlock(y, PACKED_ICE)
```

RNG-order subtlety for the port: when condition A's first two guards pass but `nextDouble() <= 0.01`,
control FALLS INTO condition B (which re-reads the block; air is not water, so it skips) — the failed
A-roll is consumed and no B-roll happens. When the block is air but `y >= (int)top`, NO draw is
consumed. Underwater draws happen only when the four B-guards pass in the quoted order.

Constants: 8.25, 1.28, 15.0, threshold 1.8, 1.17, 1.5, 1.2, 40.0, 14.0, melt −2.0, gate 2.0, −7.0,
snow base 2 + nextInt(4), snow min y = seaLevel + 18 + nextInt(10), rolls 0.01 / 0.15.
Trigger: `biome.is(FROZEN_OCEAN) || biome.is(DEEP_FROZEN_OCEAN)`; loop floor =
`ctx.getMinSurfaceLevel()` (A2 §1.8 — bilinear preliminary-surface, `+ surfaceDepth - 8`); loop
ceiling = `max(h1, (int)top + 1)` where h1 is the PRE-badlands heightmap+1 (badlands and frozen
ocean are mutually exclusive biomes, so h1==h2 in practice on these columns).

---

## 10. Direct dependencies verified / flagged

- `Biome.shouldMeltFrozenOceanIcebergSlightly(BlockPos, int seaLevel)` — verified tiny:
  ```java
  return this.getTemperature(pos, seaLevel) > 0.1f;    // fcmpl ifle — strict >
  ```
  `Biome.getTemperature(BlockPos,int)` uses a 1024-entry per-thread `Long2FloatLinkedOpenHashMap`
  cache keyed on `pos.asLong()`, delegating to `getHeightAdjustedTemperature(pos, seaLevel)` —
  temperature machinery is deterministic (no RNG) but NOT deep-dived here; flag for the biome
  cluster if frozen-ocean parity is chased (only affects the −2.0 melt adjustment).
- `NoiseChunk.preliminarySurfaceLevel(int, int)` → `public int preliminarySurfaceLevel(II)`, backed by
  `Long2IntMap preliminarySurfaceLevelCache` + `computePreliminarySurfaceLevel(J)` and a
  `DensityFunction preliminarySurfaceLevel` field — non-trivial, lives in NoiseChunk (stage-04
  cluster). Signature recorded; only reached via `Context.getMinSurfaceLevel` (frozen-ocean columns
  and abovePreliminarySurface conditions).
- `ChunkAccess.getHeight(Heightmap$Types, int, int)` — signature only (A2 §7 note); called with
  LOCAL chunk coords here.
- `Mth.floor(D)I` — verified: `(int)Math.floor(d)` (`Math.floor` then `d2i`).
- `PositionalRandomFactory.at(III)` / `fromHashOf(Identifier)` — interface methods; `fromHashOf(Identifier)`
  default = `fromHashOf(id.toString())` (A5 §1.4). Implementations (Xoroshiro/Legacy positional hash)
  are RNG-cluster scope.
- `ChunkPos.getMinBlockX/getMinBlockZ` — signature only (`chunkX << 4` folklore; not disassembled).
- `topMaterial(...)` (§11) is called from the CARVER stage, not stage-05; included for completeness.

## 11. `topMaterial(...)` — carver-support entry point (complete)

```java
public Optional<BlockState> topMaterial(SurfaceRules.RuleSource ruleSource, CarvingContext carvingContext,
                                        Function<BlockPos, Holder<Biome>> biomeGetter, ChunkAccess chunk,
                                        NoiseChunk noiseChunk, BlockPos pos, boolean hasFluid) {
    SurfaceRules.Context ctx = new SurfaceRules.Context(this, carvingContext.randomState(), chunk,
        noiseChunk, biomeGetter, carvingContext /* CarvingContext IS-A WorldGenerationContext */,
        null /* possibleBiomes == null! */);
    SurfaceRules.SurfaceRule rule = (SurfaceRules.SurfaceRule) ruleSource.apply(ctx);
    int x = pos.getX(), y = pos.getY(), z = pos.getZ();
    ctx.updateXZ(x, z);
    ctx.updateY(1, 1, hasFluid ? y + 1 : Integer.MIN_VALUE, y);   // stoneDepthAbove=1, stoneDepthBelow=1
    return Optional.ofNullable(rule.tryApply(x, y, z));
}
```

(`possibleBiomes = aconst_null` — any biome-set condition evaluated through this path would NPE /
must be treated as absent; carver rules evidently avoid it.)

---

## 12. RNG / noise consumption points — complete ordered list

Construction time (once per RandomState, §1.1):
1. `getOrCreateNoise` seeds (dedup'd, stateless positional hashing) in order:
   `minecraft:clay_bands_offset`, `minecraft:surface`, `minecraft:surface_secondary`,
   `minecraft:badlands_pillar`, `minecraft:badlands_pillar_roof`, `minecraft:badlands_surface`,
   `minecraft:iceberg_pillar`, `minecraft:iceberg_pillar_roof`, `minecraft:iceberg_surface`.
2. `noiseRandom.fromHashOf("minecraft:clay_bands")` → sequential draws of §2
   (nextInt(5)*, [nextInt(10); (nextInt(3),nextInt(192))*] ×3, nextInt(7), (nextBoolean?,nextBoolean?,nextInt(16))*).

Per column (x,z) during buildSurface:
3. `updateXZ` → `getSurfaceDepth`: `surfaceNoise.getValue(x,0,z)` + **`noiseRandom.at(x,0,z).nextDouble()`** (always).
4. Lazily (only if a rule asks): `surfaceSecondaryNoise.getValue(x,0,z)` (once per column, memoized A2 §1.4);
   per-rule noises via `Context.getNoiseSampler` (A2 §1.9); `clayBandsOffsetNoise.getValue(x,0,z)` per `getBand` call.
5. Eroded badlands columns: 3 noise samples (badlandsSurface at (x,0,z); badlandsPillar at (0.2x,0,0.2z);
   badlandsPillarRoof at (0.75x,0,0.75z) — roof only if pillar > 0). No RandomSource draws.
6. Frozen-ocean columns: 2–3 noise samples (icebergSurface (x,0,z); icebergPillar (1.28x,0,1.28z);
   icebergPillarRoof (1.17x,0,1.17z) if > 1.8), then **`noiseRandom.at(x,0,z)`** (a fresh source with the
   SAME positional seed as #3's — draws restart from sequence index 0): `nextInt(4)`, `nextInt(10)`,
   then per-y conditional `nextDouble()` per §9's exact guard order.

No other RandomSource/positional-fork call exists anywhere in the class (checked every
invokeinterface/invokevirtual in the disassembly).

---

## 13. 26.2 deltas vs 1.21 folklore

1. **No sulfur-cave logic in SurfaceSystem itself.** No `sulfur` string, no SULFUR_CAVE_GRADIENT
   reference in the constant pool. The 26.2 sulfur-cave surface work lives entirely in the rule/data
   layer (A5 §3.3 key #51 `sulfur_cave_gradient` + A2's new 3D noise samplers). buildSurface's shape
   is the classic 1.21-era algorithm.
2. `buildSurface` final param is `Set<Holder<Biome>>` (3×3 palette union) instead of 1.21's
   `Registry<Biome>` (A5 §10); the set is only stored into the Context.
3. `Context.updateY` is the 4-int form `(stoneDepthAbove, stoneDepthBelow, waterHeight, blockY)` —
   call sites here match (buildSurface: `(24, 30, 25, 28)` slots; topMaterial: `(1, 1, fluid?y+1:MIN, y)`).
4. `ChunkAccess.setBlockState` 2-arg convenience → 3-arg with **int flags = 3** (1.21: `boolean false`).
5. Terracotta constants via `Blocks.DYED_TERRACOTTA` ColorCollection (registry reshuffle, same states).
6. `ResourceLocation`→`Identifier`; clay-bands hash string is still `"minecraft:clay_bands"`.
7. Everything else — 192-band array and its RNG script, 2.75/3.0/0.25 surface-depth formula,
   badlands (8.25/0.2/15/0.75/1.5/64/2.5/50/24) and iceberg (8.25/1.28/15/1.8/1.17/1.5/1.2/40/14/
   0.01/0.15/2+nextInt(4)/seaLevel+18+nextInt(10)) constants, x-outer/z-inner walk, dual heightmap
   read around the badlands pre-pass, reference-equality default-block gate — identical to the
   1.21-era structure.

## 14. Ambiguities / confidence

- All method bodies above are fully linear bytecode; reconstruction is exact. Confidence: certain
  for SurfaceSystem + SurfaceSystem$1.
- `WAY_BELOW_MIN_Y = -32512` is derived through two static-init chains
  (BlockPos.PACKED_Y_LENGTH=12 → DimensionType MIN_Y=-2032 → <<4), both quoted from bytecode;
  `Mth.smallestEncompassingPowerOfTwo(30000000) = 2^25` and `log2 = 25` are computed, not quoted —
  confidence high (standard values). The constant only matters in the (unreachable) all-stone-to-
  minY-1 branch and as the stoneDepthBelow base before the first non-stone is found.
- `ProtoChunk.getBlockState` out-of-height → VOID_AIR: asserted from the setBlockState prologue
  pattern; getBlockState itself not fully quoted. Only affects the `getBlock(minY-1)` probe, where any
  air-like return gives `stoneBottom = minY`. Confidence: high; verify if a bottom-of-world diff appears.
- `ProtoChunk.setBlockState(pos, state, 3)` heightmap-priming internals not disassembled (large);
  parity requirement: WORLD_SURFACE_WG must reflect badlands-pass writes before the h2 re-read.
- The stored-then-unused doubles (0.2/0.75/1.5/1.28/1.17) are javac artifacts of source-level local
  constants that were also inlined at use sites; semantics unaffected. Confidence: certain.
- `Biome.getTemperature` internals (height-adjusted temperature + ThreadLocal LRU cache) are out of
  cluster; only the `> 0.1f` gate is asserted here.
