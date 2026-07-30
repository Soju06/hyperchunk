# A3 — `net.minecraft.world.level.levelgen.carver.CaveWorldCarver` (MC 26.2, unobfuscated server bytecode)

Source of truth: `javap -p -c -constants` (and `javap -v` for BootstrapMethods/LocalVariableTables)
against `/home/ubuntu/projects/hyperchunk/tools/golden/work/server`. All pseudocode is a 1:1
reconstruction from bytecode. No vanilla-source guessing. Variable names are the REAL names from the
unobfuscated build's LocalVariableTable (quoted verbatim where they matter).

Companions: A1 (orchestration), A2 (`WorldCarver` base — `carveEllipsoid`/`carveBlock`/`canReach`/
`CarverConfiguration`), A4 (Canyon), A5 (CarvingContext/Aquifer), A6 (CarvingMask), A7 (RNG/Mth/
value providers). Facts established there are cited, not re-derived.

Config cross-reference: `reference/carver/cave.json` and `reference/carver/cave_extra_underground.json`
(both `"type": "minecraft:cave"` → this class; §8).

Class files: `CaveWorldCarver.class` (this note) + `CaveCarverConfiguration.class` (§8). One
invokedynamic total (the skip-checker lambda, §7); no inner classes.

---

## 1. Class shape and method inventory

```java
public class CaveWorldCarver extends WorldCarver<CaveCarverConfiguration> {
    public CaveWorldCarver(Codec<CaveCarverConfiguration> codec) { super(codec); }  // nothing else
}
```

Declared methods (bytecode order): ctor, `isStartChunk(CaveCarverConfiguration, RandomSource)`,
`carve(CarvingContext, CaveCarverConfiguration, ChunkAccess, Function, RandomSource, Aquifer,
ChunkPos, CarvingMask)`, `getCaveBound()`, `getThickness(RandomSource)`, `getYScale()`,
`createRoom(...)`, `createTunnel(...)`, `private static shouldSkip(DDDD)Z`, two ACC_BRIDGE
methods (checkcast-to-`CaveCarverConfiguration` forwards for the abstract `isStartChunk`/`carve`
of A2 §1.3 — no logic), and `private static lambda$carve$0` (§7).

NOT overridden: `getRange()` (stays A2's hard-coded `4`), everything in the carveEllipsoid/
carveBlock stack (A2 §2/§3 applies unchanged).

`carve`'s ChunkPos parameter is named **`sourceChunkPos`** — it is the NEIGHBOR chunk position the
orchestrator is iterating (A7 §1 `npos`), and it seeds the start-position sampling (§3). The chunk
being MUTATED is `chunk` (the center chunk); `createTunnel`'s `canReach` cull uses `chunk.getPos()`,
not `sourceChunkPos` (§6).

---

## 2. `isStartChunk(CaveCarverConfiguration, RandomSource)` — exactly 1 draw

```java
public boolean isStartChunk(CaveCarverConfiguration configuration, RandomSource random) {
    return random.nextFloat() <= configuration.probability;
    // bytecode: nextFloat; getfield probability; fcmpg; ifgt → false; else true
}
```

- **One `nextFloat()` draw, comparison is `<=` (inclusive)**, float compare via `fcmpg; ifgt`
  (NaN would compare "greater" → false; unreachable since nextFloat ∈ [0,1)).
- `probability` is loaded from the config (`cave` 0.15f / `cave_extra_underground` 0.07f, §8) —
  the getfield reads `CaveCarverConfiguration.probability:F`, the field inherited from
  `ProbabilityFeatureConfiguration` (A2 §6).
- Called via `ConfiguredWorldCarver.isStartChunk` right after `setLargeFeatureSeed` (A7 §1), so this
  nextFloat is the FIRST draw of the per-(carver,chunk) sequence.

---

## 3. `carve(...)` — full reconstruction

Descriptor:
```
public boolean carve(CarvingContext context /*1*/, CaveCarverConfiguration configuration /*2*/,
    ChunkAccess chunk /*3*/, Function<BlockPos,Holder<Biome>> biomeGetter /*4*/,
    RandomSource random /*5*/, Aquifer aquifer /*6*/, ChunkPos sourceChunkPos /*7*/,
    CarvingMask mask /*8*/)
```

```java
{
    int maxDistance = SectionPos.sectionToBlockCoord(getRange() * 2 - 1);
        // getRange()=4 (A2 §1.1, invokevirtual — not overridden here) → (4*2-1)<<4 = 7<<4 = 112
        // SectionPos.sectionToBlockCoord(int) is literally `i << 4` (bytecode-verified)
    int caveCount = random.nextInt(random.nextInt(random.nextInt(getCaveBound()) + 1) + 1);
        // *** 3 nested nextInt draws, innermost first: a=nextInt(15); b=nextInt(a+1); caveCount=nextInt(b+1)
        // getCaveBound()=15 (§4). caveCount ∈ [0, 14]. a=0 still costs all 3 draws (nextInt(1) draws).

    for (int cave = 0; cave < caveCount; ++cave) {
        double x = (double) sourceChunkPos.getBlockX(random.nextInt(16));   // draw, then (cpX<<4)+i, i2d
        double y = (double) configuration.y.sample(random, context);        // HeightProvider.sample(RandomSource, WorldGenerationContext)
            // vanilla configs: UniformHeight → EXACTLY 1 draw: nextInt(max-min+1)+min (A7 §6.1)
            // cave: min=aboveBottom(8)→−56, max=absolute(180) → nextInt(237) − 56
            // cave_extra_underground: −56 .. 47 → nextInt(104) − 56          (A7 §6.2/§6.3)
        double z = (double) sourceChunkPos.getBlockZ(random.nextInt(16));   // draw, i2d
        double horizontalRadiusMultiplier = (double) configuration.horizontalRadiusMultiplier.sample(random); // 1 draw (uniform, A7 §5.1), f2d
        double verticalRadiusMultiplier   = (double) configuration.verticalRadiusMultiplier.sample(random);   // 1 draw, f2d
        double floorLevel                 = (double) configuration.floorLevel.sample(random);                 // 1 draw, f2d
        WorldCarver.CarveSkipChecker skipChecker =
            (c, xd, yd, zd, worldY) -> shouldSkip(xd, yd, zd, floorLevel);  // captures the DOUBLE floorLevel (§7)

        int tunnels = 1;
        if (random.nextInt(4) == 0) {                                       // draw — room roll, 1-in-4
            double yScale = (double) configuration.yScale.sample(random);   // 1 draw (uniform), f2d
                // *** yScale is sampled ONLY inside the room branch — no room roll, no yScale draw ***
            float thickness = 1.0F + random.nextFloat() * 6.0F;             // draw; fmul then fadd, all float
            createRoom(context, configuration, chunk, biomeGetter, aquifer,
                       x, y, z, thickness, yScale, mask, skipChecker);      // 0 draws inside (§5)
            tunnels += random.nextInt(4);                                   // draw AFTER createRoom → tunnels ∈ [1,4]
        }

        for (int i = 0; i < tunnels; ++i) {
            float horizontalRotation = random.nextFloat() * 6.2831855F;     // draw; float constant 6.2831855f (= Mth.TWO_PI, A7 §4.2)
            float verticalRotation   = (random.nextFloat() - 0.5F) / 4.0F;  // draw; fsub then fdiv, float
            float thickness = getThickness(random);                          // 3–5 draws (§4)
            int distance = maxDistance - random.nextInt(maxDistance / 4);   // draw: 112 − nextInt(28) ∈ [85, 112]
                // maxDistance/4 computed at runtime (iload; iconst_4; idiv), not a folded constant
            int initialStep = 0;   // DEAD STORE (slot 31, never loaded); the call passes literal iconst_0
            createTunnel(context, configuration, chunk, biomeGetter,
                         random.nextLong(),                                  // *** tunnel seed: drawn HERE, on the
                                                                             // OUTER WorldgenRandom, AFTER the distance
                                                                             // draw (arg-eval order) = 2 next(32) calls
                         aquifer, x, y, z,
                         horizontalRadiusMultiplier, verticalRadiusMultiplier,
                         thickness, horizontalRotation, verticalRotation,
                         /*step*/ 0, /*dist*/ distance,
                         getYScale() /* = 1.0D, no draw (§4) */,
                         mask, skipChecker);                                 // 0 draws on the outer random inside (§6)
        }
    }
    return true;   // iconst_1 unconditionally — even when caveCount == 0
}
```

Load-bearing typing notes:
- `x`/`y`/`z` are ints widened with `i2d` — block-corner coordinates, no +0.5 anywhere here.
- All three FloatProvider samples and yScale are widened `f2d` immediately; the tunnel keeps the
  multipliers as doubles from then on. `thickness`/`horizontalRotation`/`verticalRotation` remain
  floats.
- Room branch draw order: yScale sample FIRST, then the radius nextFloat, then (after createRoom,
  which draws nothing) `nextInt(4)` for extra tunnels.
- `nextInt(16)` and `nextInt(4)` are power-of-two bounds → exactly one `next(31)` each, no rejection
  loop; `nextInt(15)/nextInt(28)/nextInt(237)/…` use the rejection loop (A7 §3.3; a re-draw is
  astronomically rare but must be modeled).

---

## 4. Helper methods — exact bodies

```java
protected int getCaveBound() { return 15; }        // bipush 15

protected double getYScale() { return 1.0D; }      // dconst_1

protected float getThickness(RandomSource random) {
    float thickness = random.nextFloat() * 2.0F + random.nextFloat();   // draw#1 *2.0f (fmul), + draw#2 (fadd)
    if (random.nextInt(10) == 0) {                                      // draw#3 — 1-in-10 "giant" roll
        thickness *= random.nextFloat() * random.nextFloat() * 3.0F + 1.0F;
        // draws #4,#5; eval order: (nf4 * nf5) fmul, * 3.0f fmul, + 1.0f fadd, THEN fmul into thickness
    }
    return thickness;   // ∈ [0,3) normally, [0,12) after the giant roll; all float arithmetic
}
```

Draw count: always 2 nextFloat + 1 nextInt(10); +2 nextFloat when the nextInt(10)==0.

Nether contrast (out of scope, for completeness): `NetherWorldCarver` overrides
`getCaveBound()`→10, `getYScale()`→5.0D, and `getThickness` → `(nf*2.0f + nf) * 2.0f`
(always exactly 2 draws, NO nextInt(10) branch), plus a custom `carveBlock`. It inherits this
class's `carve`/`createRoom`/`createTunnel` verbatim.

---

## 5. `createRoom` — exact args to carveEllipsoid

Descriptor: `(context, configuration, chunk, biomeGetter, aquifer, double x /*6*/, double y /*8*/,
double z /*10*/, float thickness /*12*/, double yScale /*13*/, mask /*15*/, skipChecker /*16*/)` — void.

```java
protected void createRoom(...) {
    double horizontalRadius = 1.5D + (double)(Mth.sin(1.5707963705062866D) * thickness);
        // ldc2_w 1.5707963705062866d = (double)Mth.HALF_PI (float 1.5707964f widened; javac folded the f2d)
        // Mth.sin(double)→float (A7 §4.1): index = (long)(1.5707963705062866 * 10430.378350470453) & 65535
        //   = trunc(16384.000455926336) = 16384 → SIN[16384] (== 1.0f for a correctly generated table,
        //   but implement it as the table lookup, not a constant)
        // then FLOAT multiply with thickness (fmul), f2d, dadd with 1.5d
    double verticalRadius = horizontalRadius * yScale;                  // dmul (yScale already double)
    carveEllipsoid(context, configuration, chunk, biomeGetter, aquifer,
                   x + 1.0D, y, z,                                      // *** center X gets +1.0; y and z UNCHANGED ***
                   horizontalRadius, verticalRadius, mask, skipChecker); // A2 §2; boolean result POPPED
}
```

- **Zero RNG draws.** Both radius doubles derive from already-sampled values.
- No `+0.5` offsets exist here (the half-block offsets live inside `carveEllipsoid`'s per-block
  loop, A2 §2). The only center adjustment is `x + 1.0`.
- `verticalRadius = horizontalRadius * yScale` — the config `yScale` scales rooms only (tunnels use
  the `getYScale()`/recursion constant, §6).

---

## 6. `createTunnel` — full reconstruction

Descriptor (slots in comments):
```
protected void createTunnel(CarvingContext context /*1*/, CaveCarverConfiguration configuration /*2*/,
    ChunkAccess chunk /*3*/, Function biomeGetter /*4*/, long tunnelSeed /*5*/, Aquifer aquifer /*7*/,
    double x /*8*/, double y /*10*/, double z /*12*/,
    double horizontalRadiusMultiplier /*14*/, double verticalRadiusMultiplier /*16*/,
    float thickness /*18*/, float horizontalRotation /*19*/, float verticalRotation /*20*/,
    int step /*21*/, int dist /*22*/, double yScale /*23*/,
    CarvingMask mask /*25*/, CarveSkipChecker skipChecker /*26*/)
```

```java
{
    RandomSource random = RandomSource.createThreadLocalInstance(tunnelSeed);
        // → SingleThreadedRandomSource, same 48-bit LCG as java.util.Random (A7 §1, §3.2).
        // ALL draws below are on this fork; the outer WorldgenRandom is untouched.
    int splitPoint = random.nextInt(dist / 2) + dist / 4;      // fork draw #1; idiv twice (int division)
        // trunk: dist ∈ [85,112] → nextInt(42..56) + 21..28
    boolean steep = random.nextInt(6) == 0;                    // fork draw #2 — 1-in-6
    float yRota = 0.0F;   // slot 30 — yaw (Y-axis rotation) momentum
    float xRota = 0.0F;   // slot 31 — pitch (X-axis rotation) momentum

    for (int currentStep = step; currentStep < dist; ++currentStep) {   // if_icmpge exit; step param inclusive
        double horizontalRadius = 1.5D +
            (double)(Mth.sin((double)(3.1415927F * (float)currentStep / (float)dist)) * thickness);
            // exact op order: ldc 3.1415927f (Mth.PI); i2f currentStep; fmul; i2f dist; fdiv  → FLOAT angle
            // f2d; Mth.sin(double)→float (A7 §4.1); fmul thickness (FLOAT); f2d; dadd 1.5d
        double verticalRadius = horizontalRadius * yScale;               // dmul
        float cosX = Mth.cos((double)verticalRotation);                  // f2d; Mth.cos→float; local "cosX"
        x += (double)(Mth.cos((double)horizontalRotation) * cosX);       // cos(yaw)*cosPitch: fmul FLOAT, f2d, dadd
        y += (double) Mth.sin((double)verticalRotation);                 // sin(pitch): f2d, dadd (no fmul)
        z += (double)(Mth.sin((double)horizontalRotation) * cosX);       // sin(yaw)*cosPitch: fmul FLOAT, f2d, dadd

        verticalRotation  *= steep ? 0.92F : 0.7F;                       // fmul; constants 0.92f / 0.7f
        verticalRotation  += xRota * 0.1F;                               // fmul 0.1f; fadd
        horizontalRotation += yRota * 0.1F;                              // fmul 0.1f; fadd
        xRota *= 0.9F;                                                   // fmul 0.9f
        yRota *= 0.75F;                                                  // fmul 0.75f
        xRota += (random.nextFloat() - random.nextFloat()) * random.nextFloat() * 2.0F;
            // fork draws #a,#b,#c: (a−b) fsub; * c fmul; * 2.0f fmul; fadd — assoc: ((a−b)*c)*2.0f
        yRota += (random.nextFloat() - random.nextFloat()) * random.nextFloat() * 4.0F;
            // fork draws #d,#e,#f: ((d−e)*f)*4.0f — pitch-momentum (xRota) update comes FIRST, then yaw (yRota)

        if (currentStep == splitPoint && thickness > 1.0F) {             // fcmpl; ifle → no split (NaN: no split)
            createTunnel(context, configuration, chunk, biomeGetter,
                random.nextLong(),                                       // child-1 seed (2 next(32) on THIS fork)
                aquifer, x, y, z,                                        // CURRENT (post-move) position
                horizontalRadiusMultiplier, verticalRadiusMultiplier,
                random.nextFloat() * 0.5F + 0.5F,                        // child thickness ∈ [0.5,1.0): fmul; fadd
                horizontalRotation - 1.5707964F,                         // yaw − HALF_PI (float 1.5707964f), fsub
                verticalRotation / 3.0F,                                 // pitch/3: fdiv
                currentStep, dist,                                       // child resumes at THIS step, same dist
                1.0D,                                                    // *** literal dconst_1, NOT getYScale() ***
                mask, skipChecker);
            createTunnel(context, configuration, chunk, biomeGetter,
                random.nextLong(),                                       // child-2 seed — drawn AFTER child 1 RETURNS
                aquifer, x, y, z,
                horizontalRadiusMultiplier, verticalRadiusMultiplier,
                random.nextFloat() * 0.5F + 0.5F,
                horizontalRotation + 1.5707964F,                         // yaw + HALF_PI (fadd)
                verticalRotation / 3.0F,
                currentStep, dist, 1.0D, mask, skipChecker);
            return;                                                      // parent tunnel ENDS at the split
        }

        if (random.nextInt(4) != 0) {                                    // fork draw #g — ==0 skips carving (25%)
            if (!canReach(chunk.getPos(), x, z, currentStep, dist, thickness)) {
                return;                                                  // A2 §5; note chunk.getPos() = CENTER chunk,
            }                                                            // and the float-add width trap in A2 §5
            carveEllipsoid(context, configuration, chunk, biomeGetter, aquifer,
                x, y, z,
                horizontalRadius * horizontalRadiusMultiplier,           // dmul — config multiplier applied HERE only
                verticalRadius   * verticalRadiusMultiplier,             // dmul
                mask, skipChecker);                                      // A2 §2; boolean result popped
        }
        // (skipped step: the nextInt(4) draw still happened; loop continues)
    }
}
```

Load-bearing details:

- **The fork.** `tunnelSeed` was drawn on the outer WorldgenRandom (§3 / split recursion); every
  draw inside `createTunnel` is on the fresh `SingleThreadedRandomSource`. Recursive children fork
  AGAIN from `random.nextLong()` of the PARENT's fork. Child 1 runs to completion (consuming only
  its own fork) before child 2's seed is drawn — so the parent-fork draw order around a split is:
  `nextLong₁ (2), nextFloat₁ (1), [child 1 executes], nextLong₂ (2), nextFloat₂ (1), [child 2], return`.
- **Draws-per-step invariant:** 6 nextFloat (momentum) always; then EITHER the split block
  (2+1, child, 2+1) and return, OR 1 nextInt(4) (+0 more — canReach and carveEllipsoid draw nothing,
  A2). A step that fails `canReach` returns AFTER its 6+1 draws.
- **Rotation update order matters** (all float): scale pitch by 0.92f/0.7f → add xRota·0.1f to pitch
  → add yRota·0.1f to yaw → decay xRota ·0.9f → decay yRota ·0.75f → refresh xRota (3 draws, ×2.0f)
  → refresh yRota (3 draws, ×4.0f). The DECAYED momenta are what get the random increments; the
  increments only influence the NEXT step's rotations.
- **Movement uses the PRE-update rotations** (position advance happens before the rotation update),
  and `horizontalRadius` for the step is computed even earlier — from `currentStep` before the move.
- **Split condition:** `currentStep == splitPoint && thickness > 1.0F` (strict `>`, `fcmpl; ifle`).
  Child thickness `nextFloat()*0.5f+0.5f < 1.0f` strictly ⇒ **children can never split again;
  recursion depth is at most 2** (trunk + 2 children). C impl: no unbounded recursion.
  Children recompute their own `splitPoint` from the same `dist`; since they start at
  `currentStep ≥ dist/4 + 0` it may or may not be ahead of them — irrelevant given thickness < 1.
- **Skip roll:** `nextInt(4) == 0` → do not carve this step (bytecode: `ifne` to the carve block,
  else `goto` loop-increment). 1.12-era folklore sometimes inverts this; 26.2 carves on ≠0.
- `canReach` gets `(chunk.getPos(), x, z, currentStep, dist, thickness)` — the chunk BEING carved;
  `width` param = trunk `thickness` float (A2 §5 does `width + 2.0f + 16.0f` in FLOAT).
- `splitPoint`/`steep` are drawn once per tunnel even if `step` starts near `dist` (children).
  `nextInt(dist/2)` with trunk dist ≥ 85 never sees a ≤0 bound (no exception path in practice).

---

## 7. The CarveSkipChecker lambda — exact expression

One invokedynamic in `carve` (BootstrapMethods #0, LambdaMetafactory, verified):
captured state = the DOUBLE `floorLevel` (already `f2d`-widened sample);
instantiated methodtype `(CarvingContext, D, D, D, I)Z` → `lambda$carve$0`.

```java
private static boolean lambda$carve$0(double floorLevel, CarvingContext c,
                                      double xd, double yd, double zd, int worldY) {
    return shouldSkip(xd, yd, zd, floorLevel);      // c and worldY are IGNORED
}

private static boolean shouldSkip(double xd, double yd, double zd, double floorLevel) {
    if (yd <= floorLevel) {          // dcmpg; ifgt skips the return — i.e. return true when yd <= floorLevel
        return true;                 //   (inclusive; NaN yd → "not <=" → falls through)
    }
    return xd * xd + yd * yd + zd * zd >= 1.0D;
        // eval order: xd² (dmul), + yd² (dmul,dadd), + zd² (dmul,dadd); dconst_1; dcmpl; iflt → false
        // i.e. return true when sum >= 1.0 (inclusive); NaN sum → dcmpl −1 → false (carve!) — unreachable in practice
}
```

- `xd/yd/zd` are A2 §2's ellipsoid-relative coords (`(blockX+0.5−cx)/hRadius`, `(y0−0.5−cy)/vRadius`,
  `(blockZ+0.5−cz)/hRadius`) — so `floorLevel` (vanilla uniform [−1.0,−0.4)) is compared against the
  RELATIVE y: blocks at or below `floorLevel·vRadius` under the ellipsoid center are never carved
  (flat cave floors).
- The same single checker instance (same captured floorLevel) is shared by the room and ALL tunnels
  of one cave start; each cave start (loop iteration) creates a fresh one from its own floorLevel draw.
- All double arithmetic; both comparisons are on doubles.

---

## 8. `CaveCarverConfiguration` — fields + codec

```java
public class CaveCarverConfiguration extends CarverConfiguration {
    public static final Codec<CaveCarverConfiguration> CODEC;   // RecordCodecBuilder.create (Codec, not MapCodec)
    // instance fields, declaration/putfield order after super(...):
    public final FloatProvider horizontalRadiusMultiplier;   // (1)
    public final FloatProvider verticalRadiusMultiplier;     // (2)
    public final FloatProvider floorLevel;                   // (3)
}
```

Base fields/codec = A2 §6 (`probability`, `y`, `yScale`, `lavaLevel`, `debugSettings`, `replaceable`).

CODEC group order (4 entries, `lambda$static$0` bytecode-verified):
1. `CarverConfiguration.CODEC.forGetter(identity)` — the base MapCodec INLINED (flat JSON, no wrapper key)
2. `FloatProviders.CODEC.fieldOf("horizontal_radius_multiplier")` — REQUIRED
3. `FloatProviders.CODEC.fieldOf("vertical_radius_multiplier")` — REQUIRED
4. `FloatProviders.codec(-1.0F, 1.0F).fieldOf("floor_level")` — REQUIRED, **range-validated [−1,1]**
   (the only range-checked field; ldc −1.0f, fconst_1)

Constructors: 9-arg primary; 8-arg convenience inserting `CarverDebugSettings.DEFAULT`; 4-arg
`(CarverConfiguration, FloatProvider×3)` copying the base's five getfields — the codec uses the 4-arg
form (Function4 in the apply).

JSON cross-check (`reference/carver/cave.json` / `cave_extra_underground.json`, identical except):
probability 0.15 vs 0.07; `y` max_inclusive absolute 180 vs 47. Shared: y min above_bottom 8 (→ −56),
yScale uniform [0.1,0.9), horizontal_radius_multiplier uniform [0.7,1.4), vertical_radius_multiplier
uniform [0.8,1.3), floor_level uniform [−1.0,−0.4), lava_level above_bottom 8 (→ −56, A2 §4),
replaceable `#minecraft:overworld_carver_replaceables` (A2 §8). Both carry explicit `debug_settings`
blocks (debug_mode absent ⇒ false ⇒ inert; cave uses crimson_button as air_state, cave_extra uses
oak_button — cosmetic only). All FloatProviders are `minecraft:uniform` → 1 draw each (A7 §5.1);
`y` is `minecraft:uniform` height → 1 draw (A7 §6.1).

---

## 9. RNG draw ladder — one full carve invocation (THE table for the C port)

Preconditions (A7 §1): outer random is WorldgenRandom(LegacyRandomSource); per (carver-index, neighbor
chunk): `setLargeFeatureSeed(worldSeed + idx, npos.x, npos.z)` = 4 `next(32)` draws + reseed, then:

**OUTER WorldgenRandom** (`nextFloat`=1×next(24), `nextInt(2^k)`=1×next(31), other `nextInt`=next(31)
with rejection loop, `nextLong`=next(32)×2 — A7 §3.3):

| # | draw | expression | notes |
|---|------|-----------|-------|
| O1 | nextFloat | `isStartChunk: nf <= probability` | false → carve not called, ladder ends |
| O2 | nextInt(15) | `a` | getCaveBound()=15 |
| O3 | nextInt(a+1) | `b` | bound may be 1 (still draws) |
| O4 | nextInt(b+1) | `caveCount ∈ [0,14]` | 0 → return true, no more draws |
| — | *per cave start (×caveCount):* | | |
| O5 | nextInt(16) | x = blockX(sourceChunkPos, i) | pow-2 |
| O6 | nextInt(237) | y = draw − 56 | cave; cave_extra: nextInt(104) − 56 |
| O7 | nextInt(16) | z = blockZ(sourceChunkPos, i) | pow-2 |
| O8 | nextFloat | horizontalRadiusMultiplier = nf·0.7f+0.7f | uniform [0.7,1.4) |
| O9 | nextFloat | verticalRadiusMultiplier = nf·0.5f+0.8f | uniform [0.8,1.3) |
| O10 | nextFloat | floorLevel = nf·0.6f+(−1.0f) | uniform [−1.0,−0.4); captured by checker |
| O11 | nextInt(4) | room roll | pow-2; ≠0 → skip O12–O14 |
| O12 | nextFloat | yScale = nf·0.8f+0.1f | ONLY if O11==0 |
| O13 | nextFloat | room radius = 1.0f + nf·6.0f | createRoom: 0 further draws |
| O14 | nextInt(4) | tunnels = 1 + draw | pow-2 |
| — | *per tunnel (×tunnels, default 1):* | | |
| O15 | nextFloat | horizontalRotation = nf·6.2831855f | |
| O16 | nextFloat | verticalRotation = (nf−0.5f)/4.0f | |
| O17 | nextFloat | getThickness t = nf·2.0f + … | |
| O18 | nextFloat | … + nf | |
| O19 | nextInt(10) | giant roll | ≠0 → skip O20–O21 |
| O20 | nextFloat | t *= (nf·… | ONLY if O19==0 |
| O21 | nextFloat | …·nf)·3.0f+1.0f | |
| O22 | nextInt(28) | distance = 112 − draw | pow-2? no (28) — rejection loop |
| O23–O24 | nextLong | tunnelSeed | 2×next(32); createTunnel draws nothing further on outer |

**FORKED SingleThreadedRandomSource** per createTunnel(seed) — trunk seed = O23/24, child seeds drawn
on the parent's fork:

| # | draw | expression | notes |
|---|------|-----------|-------|
| F1 | nextInt(dist/2) | splitPoint = draw + dist/4 | dist/2 ∈ [42,56] trunk |
| F2 | nextInt(6) | steep = (draw == 0) | |
| — | *per step, currentStep = step … dist−1:* | | |
| F3 | nextFloat | xRota Δ: a | pitch momentum |
| F4 | nextFloat | b | |
| F5 | nextFloat | c → xRota += ((a−b)·c)·2.0f | |
| F6 | nextFloat | yRota Δ: d | yaw momentum |
| F7 | nextFloat | e | |
| F8 | nextFloat | f → yRota += ((d−e)·f)·4.0f | |
| — | IF currentStep==splitPoint && thickness>1.0f: | | |
| F9–F10 | nextLong | child-1 seed | |
| F11 | nextFloat | child-1 thickness = nf·0.5f+0.5f | then child 1 runs on ITS fork |
| F12–F13 | nextLong | child-2 seed | drawn after child 1 completes |
| F14 | nextFloat | child-2 thickness | then child 2 runs; parent returns |
| — | ELSE: | | |
| F9' | nextInt(4) | ==0 → skip carve (draw consumed) | pow-2 |
| — | canReach false → return (after F9'); else carveEllipsoid (0 draws, A2) | | |

Children never split (thickness < 1.0f) → child ladders are F1,F2,(F3–F8,F9')×steps only.
`carve` returns true unconditionally.

---

## 10. 26.2 deltas vs 1.21 folklore

- `RandomSource.createThreadLocalInstance(tunnelSeed)` replaces 1.21's `RandomSource.create(seed)` —
  SingleThreadedRandomSource instead of LegacyRandomSource; output-identical LCG (A7 §1/§3.2).
- `Mth.sin/cos` are `(double)→float` in 26.2 (A7 §4.1): every trig call here does `f2d` on the float
  rotation first, and createRoom's argument is the folded double constant `1.5707963705062866d`
  ((double)HALF_PI_f). 1.21 called float overloads; same table indices for these inputs, but the
  TABLE ITSELF is generated differently in 26.2 (A7 §4.1 delta 2) — use A7's expression.
- Structure of `carve`/`createRoom`/`createTunnel`/`shouldSkip` is otherwise IDENTICAL to the 1.21
  shape (draw counts, order, constants). No new draws, no removed draws found.
- `carve` is invoked with the single-list carver iteration (no AIR/LIQUID steps — A7 §1 delta);
  `sourceChunkPos` is the neighbor chunk in the 17×17 scan.
- `CaveCarverConfiguration.CODEC` uses `FloatProviders.CODEC` / `FloatProviders.codec(min,max)`
  statics (26.2 registry-dispatch class, A2 §6 delta); key names unchanged
  (`horizontal_radius_multiplier`, `vertical_radius_multiplier`, `floor_level`).
- Folklore traps confirmed-absent: no `nextInt(nextInt(nextInt(40)…))`-era bounds (bound is 15);
  the "carve only when nextInt(4)==0" inversion some old writeups have is wrong — 26.2 SKIPS on ==0;
  `isStartChunk` is `<=` not `<`; room center offset is `x+1.0` with NO 0.5 offsets in this class.

---

## 11. sulfur scan

`strings CaveWorldCarver.class | grep -i sulfur` and same for `CaveCarverConfiguration.class`:
**no hits** (both exit 1). Consistent with A2 §10/A7 §9 — sulfur interaction is purely data-side
(replaceables tag + surface rules), nothing cave-carver-specific.

---

## 12. OPEN items

- OPEN: `nextInt(bound)` rejection-loop re-draws (non-pow-2 bounds O2/O3/O4/O6/O19/O22, F1, F2) are
  theoretically possible and MUST be modeled with the exact A7 §3.3 algorithm; no cave-specific
  handling exists.
- OPEN (A5/A6 ground): everything downstream of `carveEllipsoid` — aquifer substance, mask bit
  layout — is inherited unchanged from A2; nothing cave-specific to verify there.
- OPEN (cosmetic): the dead `initialStep` local (§3) suggests javac folded a `final int` — irrelevant
  for parity, recorded because the bytecode stores-then-ignores slot 31.
- VERIFIED (tree-wide `grep -rla`): `createTunnel`/`createRoom` appear ONLY in
  `CaveWorldCarver.class`; `getThickness` only there + `NetherWorldCarver.class` (its override,
  §4). No external callers exist — the reconstruction above is the complete call graph.
