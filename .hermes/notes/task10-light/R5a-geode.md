# R5a — GeodeFeature recon (minecraft:geode, 26.2)

Source of truth: `javap -p -c -constants -cp tools/golden/libs/extracted/server-26.2.jar <fqcn>`.
Classes: `net.minecraft.world.level.levelgen.feature.GeodeFeature`,
`...feature.configurations.GeodeConfiguration`, `net.minecraft.world.level.levelgen.GeodeBlockSettings`
/ `GeodeLayerSettings` / `GeodeCrackSettings` (NOTE: the three settings classes live in
`net.minecraft.world.level.levelgen`, NOT in `feature.configurations`).
JOML: `tools/golden/libs/extracted/libraries/org/joml/joml/1.10.8/joml-1.10.8.jar`.
All `place@N` cites are bytecode offsets in `GeodeFeature.place(FeaturePlaceContext)`.

---

## 1. Effective config — `minecraft:amethyst_geode`

From `reference/configured_feature/amethyst_geode.json` + codec defaults (cited from the
codec-builder lambdas; `optionalFieldOf(name, default)`):

| field | value | source |
|---|---|---|
| blocks.filling_provider | simple: `minecraft:air` | JSON |
| blocks.inner_layer_provider | simple: `minecraft:amethyst_block` | JSON |
| blocks.alternate_inner_layer_provider | simple: `minecraft:budding_amethyst` | JSON |
| blocks.middle_layer_provider | simple: `minecraft:calcite` | JSON |
| blocks.outer_layer_provider | simple: `minecraft:smooth_basalt` | JSON |
| blocks.inner_placements | [small_amethyst_bud, medium_amethyst_bud, large_amethyst_bud, amethyst_cluster], each `facing=up, waterlogged=false` (order matters: `Util.getRandom` indexes it) | JSON |
| blocks.cannot_replace | `#minecraft:features_cannot_replace` | JSON |
| blocks.invalid_blocks | `#minecraft:geode_invalid_blocks` | JSON |
| layers.filling | **1.7** (default; JSON `layers:{}`) | `GeodeLayerSettings.lambda$static$0@6` |
| layers.inner_layer | **2.2** (default) | `GeodeLayerSettings.lambda$static$0@30` |
| layers.middle_layer | **3.2** (default) | `GeodeLayerSettings.lambda$static$0@54` |
| layers.outer_layer | **4.2** (default) | `GeodeLayerSettings.lambda$static$0@78` |
| crack.generate_crack_chance | **0.95** | JSON (default 1.0, `GeodeCrackSettings.lambda$static$0@6`) |
| crack.base_crack_size | **2.0** (default) | `GeodeCrackSettings.lambda$static$0@32` |
| crack.crack_point_offset | **2** (default, int) | `GeodeCrackSettings.lambda$static$0@59` |
| use_potential_placements_chance | **0.35** (default) | `GeodeConfiguration.lambda$static$0@60` |
| use_alternate_layer0_chance | **0.083** | JSON (default 0.0, `lambda$static$0@84`) |
| placements_require_layer0_alternate | **true** (default) | `lambda$static$0@106-107` |
| outer_wall_distance | **UniformInt(4,6)** | JSON (default UniformInt(4,5), `lambda$static$0@131-133`) |
| distribution_points | **UniformInt(3,4)** (default) | `lambda$static$0@157-159` |
| point_offset | **UniformInt(1,2)** (default) | `lambda$static$0@183-185` |
| min_gen_offset | **-16** (default) | `lambda$static$0@206` |
| max_gen_offset | **16** (default) | `lambda$static$0@229` |
| noise_multiplier | **0.05** (default) | `lambda$static$0@252` |
| invalid_blocks_threshold | **1** | JSON (required field, `lambda$static$0@274-276`) |

All chance fields are `double` (CHANCE_RANGE = `Codec.doubleRange(0,1)`, `GeodeConfiguration.static{}@0-5`);
layer fields double (range 0.01..50); base_crack_size double (0..5); crack_point_offset int (0..10).

Placement (`reference/placed_feature/amethyst_geode.json`): rarity_filter chance=24 →
in_square → height_range uniform(above_bottom+6 .. absolute 30) → biome. (Placement modifiers
are the standard pipeline, not re-derived here.)

---

## 2. Complete place() algorithm

Inputs: `cfg` (above), `random` = `ctx.random()` (place@8-12), `origin` = `ctx.origin()`
(place@13-17), `level` (place@19-23). Returns bool.

Notation: `rF`=random.nextFloat, `rD`=random.nextDouble, `rI(n)`=random.nextInt(n).
`UniformInt(a,b).sample(r)` = `r.nextInt(b-a+1) + a`
(`UniformInt.sample@0-12` → `Mth.randomBetweenInclusive@0-13`).

```c
// ---- setup ----
int minGen = cfg.minGenOffset;                    // -16   (place@25-29)
int maxGen = cfg.maxGenOffset;                    // +16   (place@31-35)
List<Pair<BlockPos,int>> points = new LinkedList; //       (place@37-40)

// DRAW 1 (ctx.random): number of distribution points
int nPoints = cfg.distributionPoints.sample(random);      // rI(2)+3 → 3..4  (place@42-52)

// Internal noise RNG — SEPARATE stream, see §4.2. Created unconditionally.
WorldgenRandom wr = new WorldgenRandom(new LegacyRandomSource(level.getSeed())); // place@54-75
NormalNoise noise = NormalNoise.create(wr, /*firstOctave*/ -4, /*amps*/ {1.0});  // place@77-88

List<BlockPos> crackPoints = new LinkedList;       // (place@93-96)
double d = (double)nPoints / (double)cfg.outerWallDistance.maxInclusive();
                                    // nPoints/6.0 for amethyst  (place@98-111, int→double ddiv)

// inverted-distance thresholds — ALL plain double, Math.sqrt (place@132-187):
double L_fill  = 1.0 / Math.sqrt(layers.filling);            // no +d !  (place@132-142)
double L_inner = 1.0 / Math.sqrt(layers.innerLayer  + d);    //          (place@144-156)
double L_mid   = 1.0 / Math.sqrt(layers.middleLayer + d);    //          (place@159-171)
double L_outer = 1.0 / Math.sqrt(layers.outerLayer  + d);    //          (place@174-186)

// DRAW 2 (rD): crack size threshold — drawn ALWAYS, before the crack-chance roll
double L_crack = 1.0 / Math.sqrt( (crack.baseCrackSize + random.nextDouble() / 2.0)
                                  + (nPoints > 3 ? d : 0.0) );
        // expression tree: ((base + rD/2.0) + extra), then sqrt, then 1.0/x
        // (place@189-223; nPoints>3 test at place@206-209 if_icmple)

// DRAW 3 (rF): crack flag — drawn ALWAYS
bool doCrack = (double)random.nextFloat() < crack.generateCrackChance;   // place@225-246

// ---- distribution points + invalid-block early abort ----
int invalid = 0;                                              // place@248-249
for (int i = 0; i < nPoints; i++) {                           // place@251-385
    // DRAWS 4..: three outerWallDistance samples, in x,y,z order
    int ox = cfg.outerWallDistance.sample(random);            // rI(3)+4  (place@261-271)
    int oy = cfg.outerWallDistance.sample(random);            // rI(3)+4  (place@273-283)
    int oz = cfg.outerWallDistance.sample(random);            // rI(3)+4  (place@285-295)
    BlockPos p = origin.offset(ox, oy, oz);                   // place@297-305
    BlockState s = level.getBlockState(p);                    // place@310-314
    if (s.isAir() || s.is(cfg.blocks.invalidBlocks)) {        // place@321-339 (tag §6)
        if (++invalid > cfg.invalidBlocksThreshold)           // threshold=1 (place@342-351)
            return false;                                     // EARLY ABORT (place@354-355)
            // note: pointOffset for this p NOT drawn; draws already consumed stay consumed
    }
    // DRAW: per-point offset (only when not aborted)
    points.add(Pair.of(p, cfg.pointOffset.sample(random)));   // rI(2)+1  (place@356-381)
}

// ---- crack points ----
if (doCrack) {                                                // place@388-390
    int k = random.nextInt(4);                                // DRAW (rI(4)) place@393-395
    int j = nPoints * 2 + 1;                                  // 7 or 9   (place@402-408)
    if      (k == 0) { add origin.offset(j,7,0); offset(j,5,0); offset(j,1,0); } // @410-467
    else if (k == 1) { add origin.offset(0,7,j); offset(0,5,j); offset(0,1,j); } // @470-528
    else if (k == 2) { add origin.offset(j,7,j); offset(j,5,j); offset(j,1,j); } // @531-592
    else             { add origin.offset(0,7,0); offset(0,5,0); offset(0,1,0); } // @595-643
}

// ---- per-block loop ----
List<BlockPos> potential = new ArrayList;                     // place@644-647
Predicate<BlockState> canPlace = st -> !st.is(cfg.blocks.cannotReplace);
        // lambda$place$0@0-13, bound at place@649-665; used by EVERY safeSetBlock

for (BlockPos pos : BlockPos.betweenClosed(origin.offset(minGen,minGen,minGen),
                                           origin.offset(maxGen,maxGen,maxGen))) {
        // place@667-697; iteration order §5.1: x fastest, then y, then z
        // 33*33*33 = 35937 cells for amethyst defaults
    double nv = noise.getValue((double)pos.x, (double)pos.y, (double)pos.z)
                * cfg.noiseMultiplier;                        // *0.05  (place@721-748)
    double sum  = 0.0;                                        // place@751-752
    double csum = 0.0;                                        // place@754-755

    for (Pair<BlockPos,int> pt : points)                      // insertion order (place@757-825)
        sum = sum + ( invSqrt( pos.distSqr(pt.first) + (double)pt.second ) + nv );
        // NOTE: nv added ONCE PER POINT. Inner add (invSqrt+nv) first, then outer add
        // (stack at place@788-822: sum, inv, nv → dadd → dadd). invSqrt/distSqr: §3.

    for (BlockPos cp : crackPoints)                           // place@828-884 (empty if !doCrack)
        csum = csum + ( invSqrt( pos.distSqr(cp) + (double)crack.crackPointOffset ) + nv );
        // crackPointOffset = 2 (int→double), nv once per crack point

    if (sum < L_outer) continue;                              // place@887-895 (dcmpg ifge)

    if (doCrack && csum >= L_crack && sum < L_fill) {         // place@898-916 (dcmpl iflt / dcmpg ifge)
        safeSetBlock(level, pos, Blocks.AIR.defaultBlockState(), canPlace); // place@919-932
        for (Direction dir : DIRECTIONS) {                    // DOWN,UP,NORTH,SOUTH,WEST,EAST (§5.4)
            BlockPos np = pos.relative(dir);                  // place@962-966
            FluidState fs = level.getFluidState(np);          // place@971-980
            if (!fs.isEmpty())                                // place@982-987
                level.scheduleTick(np, fs.getType(), /*delay*/0); // place@990-1000
        }
        continue;                                             // place@1011
    }
    if (sum >= L_fill) {                                      // place@1014-1019 (dcmpl iflt)
        safeSetBlock(level, pos, fillingProvider.getState(level,random,pos), canPlace);
        continue;                                             // place@1022-1045; simple provider: 0 draws (§7.3)
    }
    if (sum >= L_inner) {                                     // place@1048-1053
        // DRAW (rF): ALWAYS in this branch
        bool alt = (double)random.nextFloat() < cfg.useAlternateLayer0Chance;  // place@1056-1076
        if (alt) safeSetBlock(level, pos, alternateInnerLayerProvider.getState(...), canPlace); // @1078-1106
        else     safeSetBlock(level, pos, innerLayerProvider.getState(...), canPlace);          // @1109-1129
        if (!cfg.placementsRequireLayer0Alternate || alt)     // place@1132-1141 (true → need alt)
            // DRAW (rF): only when the gate above passes
            if ((double)random.nextFloat() < cfg.usePotentialPlacementsChance)  // place@1144-1156
                potential.add(pos.immutable());               // place@1159-1166 — COPY! (cursor is mutable)
        continue;                                             // place@1172
    }
    if (sum >= L_mid)   { safeSetBlock(middleLayerProvider); continue; }  // place@1175-1206
    if (sum >= L_outer) { safeSetBlock(outerLayerProvider);  }            // place@1209-1237 (always true here)
}

// ---- buds / clusters ----
List<BlockState> placements = cfg.blocks.innerPlacements;     // place@1243-1248
for (BlockPos pos : potential) {                              // insertion order (place@1250-...)
    // DRAW (rI(4)): Util.getRandom = list.get(random.nextInt(size)) — §7.2. Drawn per
    // potential pos even if no direction succeeds below.
    BlockState bud = Util.getRandom(placements, random);      // place@1281-1287
    for (Direction dir : DIRECTIONS) {                        // DOWN,UP,NORTH,SOUTH,WEST,EAST
        if (bud.hasProperty(FACING))
            bud = bud.setValue(FACING, dir);                  // place@1319-1343 (all 4 amethyst states have it)
        BlockPos np = pos.relative(dir);                      // place@1345-1352
        BlockState ns = level.getBlockState(np);              // place@1354-1363
        if (bud.hasProperty(WATERLOGGED))
            bud = bud.setValue(WATERLOGGED, ns.getFluidState().isSource()); // place@1365-1398 (§7.5)
        if (BuddingAmethystBlock.canClusterGrowAtState(ns)) { // §7.4
            safeSetBlock(level, np, bud, canPlace);           // place@1408-1417
            break;                                            // place@1420 goto 1429 — FIRST direction wins
        }
    }
}
return true;                                                  // place@1432-1433
```

`safeSetBlock` (`Feature.safeSetBlock@0-27`): `if (predicate.test(level.getBlockState(pos)))
level.setBlock(pos, state, /*flags*/ 2);` — the predicate tests the block CURRENTLY at pos.

### 2.1 ctx.random draw ledger (amethyst config)

| # | draw | bound / kind | where |
|---|---|---|---|
| 1 | nextInt(2)+3 | distribution_points | place@42-52 |
| 2 | nextDouble | crack size term | place@196 |
| 3 | nextFloat | vs 0.95 crack chance | place@225 |
| per point ×nPoints | nextInt(3)+4 ×3, then nextInt(2)+1 | wall dist x/y/z, point offset | place@261-370 |
| — | *early `return false` possible between wall-dist and point-offset draws* | | place@354 |
| if doCrack | nextInt(4) | crack orientation | place@393 |
| per inner-layer block | nextFloat [+ nextFloat] | alt-layer0; then potential-placement iff (!requireAlt \|\| alt) | place@1056, @1144 |
| per potential pos | nextInt(4) | bud pick | place@1281-1287 |

No other ctx.random draws exist in the method. SimpleStateProvider never draws (§7.3).
The early abort wastes draws already made, but the feature RNG is per-feature salted
(setFeatureSeed) so no cross-feature realignment concern.

---

## 3. FP notes (bit-parity critical)

1. **`Mth.invSqrt(double)` is NOT the Quake hack in 26.2.** `Mth.invSqrt(double)@0-4`
   → `org.joml.Math.invsqrt(double)` (JOML 1.10.8) = `dconst_1; Math.sqrt; ddiv`
   → **exactly `1.0 / sqrt(d)`** in doubles. (`Mth.fastInvSqrt` still exists with the
   0x5FE6EB50C7B537AA bit hack + one Newton step, `Mth.fastInvSqrt@0-44`, but GeodeFeature
   calls `Mth.invSqrt` — constant-pool #247 at place@816 and place@875.) C: `1.0 / sqrt(x)`;
   IEEE-754 correctly-rounded sqrt ⇒ bit-exact.
2. Thresholds use `Math.sqrt` directly (place@138/153/168/183/219). Operand order:
   `1.0 / sqrt(layer + d)` — the add is INSIDE the sqrt; filling has NO `+ d`.
3. `d = (double)nPoints / (double)maxInclusive` — i2d, i2d, ddiv (place@98-111). For
   amethyst: 3/6.0=0.5 or 4/6.0=0.6666666666666666.
4. `BlockPos.distSqr(Vec3i)` (`Vec3i.distSqr@0-19` → `distToLowCornerSqr@0-45`):
   `dx=(double)this.x - (double)o.x; dy=..., dz=...; return (dx*dx + dy*dy) + dz*dz;`
   add order: (x²+y²) then +z². All integer-valued — exact.
5. Accumulation: `sum = sum + (invSqrt(distSqr + (double)offset) + nv)` — inner add first
   (place@816-822: invSqrt result, nv, dadd, then dadd with sum). Same shape for csum with
   `(double)crackPointOffset` (place@868-881). Point iteration order fixed (LinkedList).
6. `nv = NormalNoise.getValue(x,y,z) * 0.05` (dmul, place@741-748).
7. Chance compares: `(double)nextFloat() < chance` — f2d then `dcmpg` (place@231-238,
   @1062-1068, @1150-1156); strict less-than. Layer compares `>=` via `dcmpl iflt`.
8. NormalNoise math (all double): `getValue(x,y,z) = (first.getValue(x,y,z) +
   second.getValue(x*F, y*F, z*F)) * valueFactor`, F = 1.0181268882175227
   (`NormalNoise.getValue@0-52`). `valueFactor = 0.16666666666666666 /
   expectedDeviation(maxIdx-minIdx)`; `expectedDeviation(k) = 0.1 * (1.0 + 1.0/(double)(k+1))`
   (`NormalNoise.<init>@149-162`, `expectedDeviation@0-12`). For amps=[1.0]: k=0 → 0.2 →
   valueFactor = 0.8333333333333333 (compute via the expression, don't hardcode).
   `PerlinNoise.getValue(x,y,z)` = 5-arg with yScale=0,yMax=0 (`PerlinNoise.getValue@0-10`).
   PerlinNoise: `lowestFreqInputFactor = pow(2.0, firstOctave)` = 2^-4 = 0.0625,
   `lowestFreqValueFactor = pow(2,n-1)/(pow(2,n)-1)` = 1.0 for n=1 (`PerlinNoise.<init>@343-381`).
   No sin/cos anywhere in this path (ImprovedNoise is gradient-dot based) — the
   hc_jdk_sin/cos rule does not apply here.

---

## 4. RNG provenance

### 4.1 Feature random
`ctx.random()` (place@8-12) is the placement-pipeline WorldgenRandom (LegacyRandomSource-based,
salted per feature index via setFeatureSeed) — already on our side. All §2.1 draws come from it.

### 4.2 Internal noise random — the ONLY other RandomSource created
place@54-88: `new WorldgenRandom(new LegacyRandomSource(level.getSeed()))` then
`NormalNoise.create(wr, -4, new double[]{1.0})`. Depends ONLY on the world seed ⇒ the geode
noise is **identical for every geode in a world; build once and cache**.

Construction chain (every step cited):
- `WorldgenRandom.next(int)` delegates to the wrapped LegacyRandomSource's `next`
  (`WorldgenRandom.next@15-32`), and `forkPositional()` delegates to the wrapped source
  (`WorldgenRandom.forkPositional@0-9`) ⇒ stream identical to plain `LegacyRandomSource(worldSeed)`
  == `hc_lcg_t` seeded with worldSeed.
- `NormalNoise.create(RandomSource,int,double[])@0-20` → `create(rand, NoiseParameters(-4,[1.0]))@41-49`
  → `new NormalNoise(rand, params, /*useNewFactory*/ true)`.
- `NormalNoise.<init>` with true (`@21-49`): `first = PerlinNoise.create(rand,-4,[1.0])`, then
  `second = PerlinNoise.create(rand,-4,[1.0])` — sequentially on the SAME rand.
- `PerlinNoise.create@0-17` → `new PerlinNoise(rand, Pair(-4,[1.0]), /*useNew*/ true)`;
  ctor `@56-137` (non-legacy branch): `factory = rand.forkPositional()`, then for each
  amplitude index i with amp≠0: `noiseLevels[i] = new ImprovedNoise(factory.fromHashOf("octave_" + (firstOctave+i)))`
  (string recipe `octave_`, PerlinNoise BootstrapMethods #387). Zero-amp octaves consume
  NOTHING in this branch (legacy skipOctave/262 is the `false` branch only, `@140-263`).
- `LegacyRandomSource.forkPositional@0-11`: `new LegacyPositionalRandomFactory(this.nextLong())`
  — consumes **1 nextLong (= 2 LCG next() calls)** from the parent.
- `LegacyPositionalRandomFactory.fromHashOf@0-19`: `new LegacyRandomSource((long)s.hashCode() ^ this.seed)`
  — Java String.hashCode, sign-extended to long. `"octave_-4".hashCode() == 440898198`
  (verified with JDK). Implement `java_string_hash` (h = 31*h + c over UTF-16 units; ASCII here).
- `ImprovedNoise.<init>@0-137`: `xo = nextDouble()*256.0; yo = nextDouble()*256.0;
  zo = nextDouble()*256.0;` then `p[i]=i` and Fisher-Yates: for i in 0..255:
  `j = nextInt(256-i); swap(p[i], p[i+j])` — all on the fromHashOf-derived LCG.

Parent-stream total: exactly **2 nextLong** (one per PerlinNoise). Sequence for worldSeed S:
```
lcgA = LegacyRandomSource(S)
f1   = lcgA.nextLong()                      // factory seed for `first`
imp1 = ImprovedNoise(lcg(440898198 ^ f1))   // 3 nextDouble + 256 nextInt(256-i)
f2   = lcgA.nextLong()                      // factory seed for `second`
imp2 = ImprovedNoise(lcg(440898198 ^ f2))
```
Our `hc_octaves_init`/`hc_normal_noise_init` (core/include/hc_noise.h) implement the same
factory SHAPE but for xoroshiro (`hc_xoro_t`, md5 fromHashOf). Geode needs the **LCG flavor**:
`hc_lcg_next_long` fork + `javaStringHash ^ seed` fromHashOf + an `hc_perlin_init_from_lcg`.
Sampling side (`hc_perlin_sample`, wrap, valueFactor) is shared unchanged.

---

## 5. Iteration orders

1. **Bounding-box scan** `BlockPos.betweenClosed(a,b)` (`@885-918`): componentwise
   min/max of the two corners → `betweenClosed(minX,minY,minZ,maxX,maxY,maxZ)@1002-1035`
   → iterator `BlockPos$4.computeNext@19-90`: for `index` in 0..volume-1:
   `x = minX + index % width; y = minY + (index / width) % height; z = minZ + (index / width) / height`
   with `width = maxX-minX+1`, `height = maxY-minY+1`. **x fastest, then y, then z slowest.**
   The cursor is a single reused MutableBlockPos — hence `pos.immutable()` copies for the
   potential list (place@1163).
2. **points / crackPoints**: LinkedLists, iterated in insertion order (§2 order).
3. **potential**: ArrayList, insertion order == scan order (1).
4. **DIRECTIONS** = `Direction.values()` (GeodeFeature static{}@0-3): declaration order
   **DOWN, UP, NORTH, SOUTH, WEST, EAST** = (0,-1,0),(0,1,0),(0,0,-1),(0,0,1),(-1,0,0),(1,0,0).
   Used for crack fluid-tick scan (place@935-1008) and bud placement (place@1292-1426, break
   on first success).

---

## 6. Tags & predicates

- `#minecraft:geode_invalid_blocks` (`reference/tags/block/geode_invalid_blocks.json`):
  **bedrock, water, lava, ice, packed_ice, blue_ice**. Sole use: distribution-point validity
  `state.is(HolderSet)` at place@329-339 (`invalidBlocks()` place@333), OR'd with `state.isAir()`
  (place@321-326), incrementing the abort counter vs `invalid_blocks_threshold` (=1: the 2nd
  air-or-invalid point aborts).
- `#minecraft:features_cannot_replace` (`reference/tags/block/features_cannot_replace.json`):
  bedrock, spawner, chest, end_portal_frame, reinforced_deepslate, trial_spawner, vault.
  Bound into `canPlace = st -> !st.is(cannotReplace)` (lambda$place$0@0-13) and applied by
  every safeSetBlock (air-crack, all 4 layers, buds).

---

## 7. Support routines

1. `UniformInt.sample@0-12` → `Mth.randomBetweenInclusive@0-13` = `r.nextInt(max-min+1)+min`.
2. `Util.getRandom(List,RandomSource)@0-18` = `list.get(r.nextInt(list.size()))`.
3. `SimpleStateProvider.getState@0-4` returns the stored state — **zero RNG draws** (all five
   amethyst providers are simple; a datapack could substitute drawing providers — out of scope).
4. `BuddingAmethystBlock.canClusterGrowAtState@0-32` =
   `state.isAir() || (state.is(Blocks.WATER) && state.getFluidState().isFull())`;
   `FluidState.isFull@0-14` = `getAmount() == 8` (source-level water).
5. `FluidState.isSource@0-8` = `getType().isSource(this)` (true for still/source fluid) —
   drives WATERLOGGED on buds. `FluidState.isEmpty@0-7` = `getType().isEmpty()` — drives
   crack fluid-tick scheduling.
6. `scheduleTick(pos, fluid, 0)` on crack-air neighbors (place@990-1000): if we serialize
   `fluid_ticks` in chunk NBT, golden compare will see these; delay 0, ordered by scan (§5.1)
   then direction (§5.4).

## 8. Implementation notes (ours)

- Cache the NormalNoise per world seed (§4.2); do NOT rebuild per geode call (bit-identical
  anyway, just wasteful).
- Reuse `hc_lcg_t` (core/include/hc_rng.h) for §4.2; new primitives needed: LCG forkPositional
  (nextLong), LCG fromHashOf (javaStringHash ^ seed — hash "octave_-4" = 440898198), and an
  ImprovedNoise init that consumes from an LCG instead of `hc_xoro_t` (`hc_perlin_init_from`
  is xoro-only today).
- Layer settings load: `layers: {}` must yield 1.7/2.2/3.2/4.2 defaults; crack defaults
  base 2.0 / offset 2; chance codecs are doubles — parse with strtod (no float rounding here;
  the Codec.FLOAT gap memory note does not apply, all geode fields are DOUBLE/INT codecs).
- Early-abort path must leave the feature RNG exactly where vanilla leaves it *within this
  feature* (irrelevant downstream due to per-feature salting, but keep for probe parity).
- Buds are placed at the NEIGHBOR position (pos.relative(dir)), facing=dir, on the first
  growable direction in DOWN,UP,N,S,W,E order; the nextInt(4) state pick is consumed even
  when no direction is growable.
