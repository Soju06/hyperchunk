# A4 — `net.minecraft.world.level.levelgen.carver.CanyonWorldCarver` (+ `CanyonCarverConfiguration`, `$CanyonShapeConfiguration`) (MC 26.2, unobfuscated server bytecode)

Source of truth: `javap -p -c -constants` (and `javap -v -p` for BootstrapMethods + LocalVariableTables)
against `/home/ubuntu/projects/hyperchunk/tools/golden/work/server`. All pseudocode is a 1:1
reconstruction from bytecode. No vanilla-source guessing. Identifier names below are the REAL names
from the shipped LocalVariableTable/MethodParameters debug info (`sourceChunkPos`, `tunnelSeed`,
`widthFactorPerHeight`, `xRota`/`yRota`, …), not invented.

Companion notes cited without re-derivation: A1 (orchestration — who seeds the WorldgenRandom and
calls `ConfiguredWorldCarver.isStartChunk`/`carve`, A1 §4/§8), A2 (WorldCarver base —
`carveEllipsoid` §2, `carveBlock` §3, `canReach` §5, `CarverConfiguration` §6),
A7 (RNG substrate — LCG §3, `Mth` §4, float/height providers §5/§6).

Class files: `CanyonWorldCarver.class`, `CanyonCarverConfiguration.class`,
`CanyonCarverConfiguration$CanyonShapeConfiguration.class`. No other inner classes.

---

## 1. Class shape and entry points

```java
public class CanyonWorldCarver extends WorldCarver<CanyonCarverConfiguration> {
    public CanyonWorldCarver(Codec<CanyonCarverConfiguration> configurationFactory) { super(configurationFactory); }
    // NO fields, NO static{}, NO getRange() override → getRange() == 4 (A2 §1.1)
}
```

Methods (exact set): ctor; `isStartChunk(CanyonCarverConfiguration, RandomSource)`;
`carve(CarvingContext, CanyonCarverConfiguration, ChunkAccess, Function, RandomSource, Aquifer, ChunkPos, CarvingMask)`;
`private void doCarve(...)`; `private float[] initWidthFactors(...)`;
`private double updateVerticalRadius(...)`; `private boolean shouldSkip(...)`;
two ACC_BRIDGE/ACC_SYNTHETIC bridges (`isStartChunk(CarverConfiguration,…)`,
`carve(…CarverConfiguration…)` — `checkcast CanyonCarverConfiguration` then invokevirtual, nothing
else); `private boolean lambda$doCarve$0(float[], CarvingContext, double, double, double, int)`.

Entry: A1 §8 — `ConfiguredWorldCarver.isStartChunk(random)` / `.carve(...)` delegate here with the
WorldgenRandom that A1 §4.1 just re-seeded via `setLargeFeatureSeed(seed + idx, nposX, nposZ)`.
Registered as `"canyon"` (registry id 2) in `WorldCarver.static{}` (A2 §1).

---

## 2. `isStartChunk` — exactly 1 draw, `<=` comparison

```java
public boolean isStartChunk(CanyonCarverConfiguration configuration, RandomSource random) {
    return random.nextFloat() <= configuration.probability;
    // bytecode: nextFloat; getfield probability; fcmpg; ifgt → false
    // i.e. TRUE iff nextFloat() <= probability. fcmpg: NaN → +1 → ifgt → false (unreachable: not NaN).
}
```

Exactly **1× `nextFloat()`** (= 1 LCG advance, next(24), A7 §3.3). Vanilla canyon probability
= 0.01f (§9). The draw happens on the OUTER WorldgenRandom, immediately after
`setLargeFeatureSeed` (A1 §4.1), before `carve` is entered.

---

## 3. `carve(...)` — 1:1 reconstruction

Descriptor:
```
public boolean carve(CarvingContext context, CanyonCarverConfiguration configuration,
    ChunkAccess chunk, Function<BlockPos, Holder<Biome>> biomeGetter, RandomSource random,
    Aquifer aquifer, ChunkPos sourceChunkPos, CarvingMask mask)
```
(slots: this=0, context=1, configuration=2, chunk=3, biomeGetter=4, random=5, aquifer=6,
sourceChunkPos=7, mask=8. `chunk` is the CENTER chunk being carved; `sourceChunkPos` is the
17×17-scan neighbor chunk that owns this canyon start — A1 §4.)

```java
{
    int maxDistance = (this.getRange() * 2 - 1) * 16;                 // invokevirtual getRange → 4 ⇒ (8-1)*16 = 112
    double x = (double) sourceChunkPos.getBlockX(random.nextInt(16)); // OUTER draw 1: nextInt(16); getBlockX(i) = (cpX<<4)+i (verified: SectionPos.sectionToBlockCoord)
    int y = configuration.y.sample(random, context);                  // OUTER draw 2: UniformHeight → nextInt(max-min+1)+min (A7 §6.1); stays int (slot 12)
    double z = (double) sourceChunkPos.getBlockZ(random.nextInt(16)); // OUTER draw 3
    float horizontalRotation = random.nextFloat() * 6.2831855F;       // OUTER draw 4; fmul float; 6.2831855f = Mth.TWO_PI (A7 §4.2)
    float verticalRotation = configuration.verticalRotation.sample(random);        // OUTER draw 5: UniformFloat → 1 nextFloat (A7 §5.1)
    double yScale = (double) configuration.yScale.sample(random);     // f2d; vanilla ConstantFloat(3.0) → ZERO draws (A7 §5.3)
    float thickness = configuration.shape.thickness.sample(random);   // OUTER draws 6,7: TrapezoidFloat → 2 nextFloat (A7 §5.2)
    int distance = (int)((float) maxDistance                          // i2f
                         * configuration.shape.distanceFactor.sample(random));     // OUTER draw 8; fmul; f2i (truncate toward 0)
    int initialStep = 0;                                              // istore 21 — DEAD STORE: call site loads iconst_0, not the local
    this.doCarve(context, configuration, chunk, biomeGetter,
                 random.nextLong(),                                   // OUTER draws 9,10 (nextLong = 2× next(32), A7 §3.3) — the tunnelSeed
                 aquifer, x, (double) y /* i2d at call site */, z,
                 thickness, horizontalRotation, verticalRotation,
                 0 /* iconst_0 = step */, distance, yScale, mask);
    return true;                                                      // iconst_1 — UNCONDITIONALLY true
}
```

Load-bearing points:
- Outer-draw program order is exactly: `nextInt(16)`, y-sample `nextInt(58)` (vanilla 10..67),
  `nextInt(16)`, `nextFloat` ×5 (rotation, verticalRotation, thickness ×2, distanceFactor),
  `nextLong`. `yScale` is sampled BETWEEN verticalRotation and thickness in program order but
  contributes 0 draws (ConstantFloat).
- `distance` (= branch count / total steps): `(int)(112.0f * s)` (float `fmul`, then `f2i`
  truncation toward zero). Nominally s ∈ [0.75f, 1.0f) → 84..111, but the uniform sample DOES
  reach exactly 1.0f on the top `nextFloat` value (see §10 O8) → distance ∈ [84, 112].
  Reproduce float math bit-exact, do NOT clamp.
- `doCarve` is called EXACTLY ONCE per canyon start. Canyons have no branch splitting and no
  recursion (no self-call inside `doCarve` — verified, only `carveEllipsoid`).
- `carve` returns `true` regardless of whether any block was carved (`carveEllipsoid` results are
  `pop`ped inside `doCarve`); the caller ignores the value anyway (A1 §4).

---

## 4. `doCarve` — EXACT signature and body

Descriptor (verbatim from javap):
```
private void doCarve(CarvingContext context, CanyonCarverConfiguration configuration,
    ChunkAccess chunk, Function<BlockPos, Holder<Biome>> biomeGetter,
    long tunnelSeed, Aquifer aquifer,
    double x, double y, double z,
    float thickness, float horizontalRotation, float verticalRotation,
    int step, int distance, double yScale, CarvingMask mask)
```
(slots: tunnelSeed=5, aquifer=7, x=8, y=10, z=12, thickness=14, horizontalRotation=15,
verticalRotation=16, step=17, distance=18, yScale=19, mask=21; locals: random=22,
widthFactorPerHeight=23, yRota=24, xRota=25, currentStep=26, horizontalRadius=27,
verticalRadius=29, xc=31, xs=32.)

```java
{
    RandomSource random = RandomSource.createThreadLocalInstance(tunnelSeed);
        // 26.2: SingleThreadedRandomSource (A7 §3.2); 1.21 used RandomSource.create → LegacyRandomSource. Output-identical LCG.
    float[] widthFactorPerHeight = this.initWidthFactors(context, configuration, random);   // §5 — INNER draws, once, up front
    float yRota = 0.0F;                                    // fstore 24 — horizontal-rotation momentum
    float xRota = 0.0F;                                    // fstore 25 — vertical-rotation momentum

    for (int currentStep = step; currentStep < distance; ++currentStep) {     // if_icmpge exits; step is always 0 (§3)
        // (a) base radius — MIXED float/double, exact ops:
        double horizontalRadius = 1.5
            + (double)(Mth.sin((double)((float)currentStep * 3.1415927F / (float)distance))  // i2f; fmul PI(float); i2f; fdiv; f2d; Mth.sin(D)F (A7 §4.1)
                       * thickness);                                                          // fmul (FLOAT); f2d; dadd 1.5d
        // (b) vertical radius from the PRE-factor horizontal radius:
        double verticalRadius = horizontalRadius * yScale;                                    // dmul (both double)
        // (c) INNER draw 1 — horizontal radius factor:
        horizontalRadius = horizontalRadius
            * (double) configuration.shape.horizontalRadiusFactor.sample(random);            // 1 nextFloat; f2d; dmul
        // (d) INNER draw 2 — vertical radius update (§6):
        verticalRadius = this.updateVerticalRadius(configuration, random, verticalRadius,
                                                   (float) distance, (float) currentStep);   // i2f on both
        // (e) walk — Mth table sin/cos on f2d-widened float angles (A7 §4.1):
        float xc = Mth.cos((double) verticalRotation);     // (D)F
        float xs = Mth.sin((double) verticalRotation);
        x += (double)(Mth.cos((double) horizontalRotation) * xc);   // fmul (FLOAT), f2d, dadd
        y += (double) xs;                                            // f2d, dadd
        z += (double)(Mth.sin((double) horizontalRotation) * xc);
        // (f) rotation evolution — ALL float ops, THIS order:
        verticalRotation = verticalRotation * 0.7F;                  // fmul
        verticalRotation = verticalRotation + xRota * 0.05F;         // fmul; fadd
        horizontalRotation = horizontalRotation + yRota * 0.05F;
        xRota = xRota * 0.8F;
        yRota = yRota * 0.5F;
        // (g) INNER draws 3,4,5:
        xRota = xRota + (random.nextFloat() - random.nextFloat()) * random.nextFloat() * 2.0F;
            // bytecode: nf; nf; fsub; nf; fmul; fconst_2; fmul; fadd — ((d1−d2)*d3)*2, then + xRota
        // (h) INNER draws 6,7,8:
        yRota = yRota + (random.nextFloat() - random.nextFloat()) * random.nextFloat() * 4.0F;
        // (i) INNER draw 9 — carve gate (draw happens EVERY step, before any early-out):
        if (random.nextInt(4) != 0) {                                // pow-2 bound: single next(31), (int)(4L*u >> 31)
            if (!canReach(chunk.getPos(), x, z, currentStep, distance, thickness)) {  // A2 §5; CENTER chunk pos, width = thickness
                return;                                              // aborts ALL remaining steps of this canyon
            }
            this.carveEllipsoid(context, configuration, chunk, biomeGetter, aquifer,
                x, y, z, horizontalRadius, verticalRadius, mask,
                (ctx2, xd, yd, zd, y2) -> this.shouldSkip(ctx2, widthFactorPerHeight, xd, yd, zd, y2));  // §7
            // result popped
        }
        // nextInt(4)==0 ⇒ skip carving this step (no canReach, no ellipsoid) but keep walking
    }
}
```

Load-bearing points:
- **(b) before (c)**: `verticalRadius` is derived from `horizontalRadius` BEFORE the
  horizontalRadiusFactor multiply. With vanilla yScale=3.0, verticalRadius starts at 3× the
  unfactored base radius.
- `sin(currentStep·π/distance)`: numerator/denominator math is entirely FLOAT
  (`(float)currentStep * 3.1415927f / (float)distance`), widened f2d only for the `Mth.sin(D)F`
  table lookup; the result (float) is multiplied by `thickness` in FLOAT, then widened and added
  to the double literal `1.5`. At currentStep=0 the angle is exactly 0.0 → SIN[0] = 0.0f →
  base radius exactly 1.5.
- Walk deltas are computed as FLOAT products (`Mth.cos(yaw)*xc`) then f2d-added to the double
  positions. `y += (double)xs` — pitch sine only, no cos on y.
- The `nextInt(4)` gate carves on ≠0 (i.e. 3 of 4 steps) and SKIPS on ==0 — but the three draw
  groups (g)(h)(i) are unconditional, so the inner draw ladder is a fixed 9 advances per step
  regardless of the gate.
- `canReach` failure `return`s out of `doCarve` entirely (not `continue`); every remaining
  step's 9 draws are NOT made. (Only inner-RNG-visible; the outer stream was already done.)
- There are NO `(dist & 3)`-style skips and NO extra draws anywhere else in the loop — folklore
  about a per-step `nextInt` position jitter belongs to caves, not canyons.

---

## 5. `initWidthFactors` — the width-smoothness array

Descriptor: `private float[] initWidthFactors(CarvingContext context, CanyonCarverConfiguration configuration, RandomSource random)`.
Called exactly once per `doCarve` (i.e. once per canyon), FIRST thing after the inner RNG is
constructed — all its draws precede all step draws.

```java
{
    int depth = context.getGenDepth();                    // overworld: 384 (A7 §6.3)
    float[] widthFactorPerHeight = new float[depth];
    float widthFactor = 1.0F;
    for (int yIndex = 0; yIndex < depth; ++yIndex) {
        if (yIndex == 0                                                     // ifeq → recompute, NO nextInt draw at 0
            || random.nextInt(configuration.shape.widthSmoothness) == 0) {  // 1 draw per yIndex >= 1
            widthFactor = 1.0F + random.nextFloat() * random.nextFloat();   // 2 draws; fmul then fadd: 1.0f + (d1*d2)
        }
        widthFactorPerHeight[yIndex] = widthFactor * widthFactor;           // stored SQUARED, every iteration
    }
    return widthFactorPerHeight;
}
```

Draw count with vanilla `width_smoothness = 3`, depth = 384:
- yIndex 0: **2× nextFloat** (unconditional; short-circuit skips the nextInt).
- yIndex 1..383: **1× nextInt(3)** each; when it returns 0 (p = 1/3), **+2× nextFloat**.
- Total = 2 + 383 + 2·#zeros — VARIABLE (expected ≈ 640 advances). `nextInt(3)` is non-pow-2 →
  rejection loop (A7 §3.3); for bound 3 only `u = 2^31−1` rejects (r=1, u−r+2 overflows) — must be
  modeled, hc_lcg's java-nextInt path already does.
- The array is (re)computed per canyon; it is captured by the skip-checker lambda and never
  mutated afterward. `widthSmoothness` is codec-enforced ≥ 1 (§8); a hypothetical 1 would make
  `nextInt(1)` always 0 (single pow-2 draw) → recompute every row.

---

## 6. `updateVerticalRadius` — exact signature and body

Descriptor: `private double updateVerticalRadius(CanyonCarverConfiguration configuration, RandomSource random, double verticalRadius, float distance, float currentStep)`.

```java
{
    float verticalMultiplier = 1.0F - Mth.abs(0.5F - currentStep / distance) * 2.0F;
        // fdiv (float); fsub from 0.5f; Mth.abs(F)F = Math.abs; fconst_2 fmul; fsub from 1.0f
        // triangle profile: 0 at the ends (currentStep=0 and →distance), ~1 at the middle
    float factor = configuration.shape.verticalRadiusDefaultFactor
                 + configuration.shape.verticalRadiusCenterFactor * verticalMultiplier;   // fmul; fadd
    return (double) factor * verticalRadius                                  // f2d; dmul FIRST
         * (double) Mth.randomBetween(random, 0.75F, 1.0F);                  // *** 1 nextFloat *** ; f2d; dmul SECOND
}
```

- `Mth.randomBetween(r, 0.75f, 1.0f)` = `nextFloat()*(1.0f−0.75f)+0.75f` — all float ops (A7 §4.2).
  The draw is UNCONDITIONAL (even though vanilla center_factor = 0 makes `factor` == 1.0f exactly:
  `1.0f + 0.0f * vm` — with vm possibly ±0, `0.0f*vm` is ±0.0f and `1.0f + ±0.0f = 1.0f`).
- Evaluation order matters for double rounding: `((double)factor * verticalRadius) * (double)rb`,
  NOT `factor * (verticalRadius * rb)`.
- Vanilla net effect: `verticalRadius *= uniform[0.75, 1.0)` per step, sampled on the inner RNG.

---

## 7. The CarveSkipChecker lambda + `shouldSkip`

BootstrapMethods (javap -v): indy #0 `shouldSkip` → `LambdaMetafactory.metafactory`, impl
`REF_invokeVirtual CanyonWorldCarver.lambda$doCarve$0:([FLnet/minecraft/world/level/levelgen/carver/CarvingContext;DDDI)Z`,
samMethodType == instantiatedMethodType == `(CarvingContext;DDDI)Z`. Captures `(this,
widthFactorPerHeight)` — slot 23 array reference, per-canyon. `lambda$doCarve$0` just forwards
(note the arg swap: captured array is param 1, forwarded as `shouldSkip` arg 2):

```java
private boolean lambda$doCarve$0(float[] widthFactorPerHeight, CarvingContext context1,
                                 double xd, double yd, double zd, int y1) {
    return this.shouldSkip(context1, widthFactorPerHeight, xd, yd, zd, y1);
}

private boolean shouldSkip(CarvingContext context, float[] widthFactorPerHeight,
                           double xd, double yd, double zd, int y) {
    int yIndex = y - context.getMinGenY();                    // isub
    return (xd * xd + zd * zd)                                // dmul; dmul; dadd
           * (double) widthFactorPerHeight[yIndex - 1]        // iconst_1 isub; faload; f2d; dmul
           + yd * yd / 6.0                                    // dmul; ldc2_w 6.0d; ddiv; dadd
           >= 1.0;                                            // dcmpl; iflt → false. NaN → false (would carve).
}
```

- `xd/yd/zd` are the ellipsoid-relative coords already divided by hRadius/vRadius/hRadius in
  `carveEllipsoid` (A2 §2); `y` is the absolute block Y (`y0`).
- **Index is `y − minGenY − 1`.** In-bounds proof from A2 §2: the y loop runs
  `y0 ∈ (minY, maxY]` with `minY ≥ minGenY+1` ⇒ index ≥ 1 (slot 0 is written by §5 but never
  read), and `maxY ≤ minGenY + genDepth − 1` even for upgrading chunks ⇒ index ≤ genDepth − 2.
  No OOB possible.
- Semantics: skip (don't carve) when `(xd²+zd²)·widthFactor²[row] + yd²/6 ≥ 1.0`. The `/6.0`
  flattens the vertical cutoff (canyon cross-sections extend further vertically than the raw
  vRadius scaling alone); the per-row squared width factor (§5) makes the canyon wall ragged
  per Y level. Comparison `>= 1.0` skip / `< 1.0` carve via `dcmpl; iflt`.

---

## 8. `CanyonCarverConfiguration` + `$CanyonShapeConfiguration` — fields, ctors, codecs

### 8.1 `CanyonCarverConfiguration extends CarverConfiguration`

```java
public class CanyonCarverConfiguration extends CarverConfiguration {
    public static final Codec<CanyonCarverConfiguration> CODEC;   // RecordCodecBuilder.create
    // instance fields, declaration order:
    public final FloatProvider verticalRotation;    // (1)
    public final CanyonShapeConfiguration shape;    // (2)
}
```

Constructors:
- 8-arg: `(float probability, HeightProvider y, FloatProvider yScale, VerticalAnchor lavaLevel,
  CarverDebugSettings debugSettings, HolderSet<Block> replaceable, FloatProvider verticalRotation,
  CanyonShapeConfiguration shape)` → `super(first six)` (A2 §6), then putfield order:
  `verticalRotation`, `shape`.
- 3-arg: `(CarverConfiguration config, FloatProvider verticalRotation, CanyonShapeConfiguration shape)`
  → unpacks `config.{probability,y,yScale,lavaLevel,debugSettings,replaceable}` (getfield order =
  that list) into the 8-arg ctor. **The codec uses this 3-arg ctor.**

CODEC group order (JSON keys):
1. `CarverConfiguration.CODEC.forGetter(c -> c)` — the base MapCodec INLINED, so the base keys
   (`probability`, `y`, `yScale`, `lava_level`, `debug_settings`, `replaceable` — spellings and
   required-ness per A2 §6) live flat in the same JSON object;
2. `FloatProviders.CODEC.fieldOf("vertical_rotation")` — REQUIRED;
3. `CanyonShapeConfiguration.CODEC.fieldOf("shape")` — REQUIRED.

### 8.2 `CanyonCarverConfiguration$CanyonShapeConfiguration`

Plain public static nested class (not a record).

```java
public static class CanyonShapeConfiguration {
    public static final Codec<CanyonShapeConfiguration> CODEC;
    // instance fields, declaration order:
    public final FloatProvider distanceFactor;          // (1)
    public final FloatProvider thickness;               // (2)
    public final int widthSmoothness;                   // (3)
    public final FloatProvider horizontalRadiusFactor;  // (4)
    public final float verticalRadiusDefaultFactor;     // (5) raw float — NEVER sampled (A7 §5.3)
    public final float verticalRadiusCenterFactor;      // (6) raw float — NEVER sampled
}
```

Ctor params in declaration order `(distanceFactor, thickness, widthSmoothness,
horizontalRadiusFactor, verticalRadiusDefaultFactor, verticalRadiusCenterFactor)`; putfield order
is the compiler-shuffled `widthSmoothness, horizontalRadiusFactor, verticalRadiusDefaultFactor,
verticalRadiusCenterFactor, distanceFactor, thickness` (recorded for fidelity; no semantic effect).

CODEC group order (JSON keys), ALL `fieldOf` = REQUIRED, no defaults:
1. `FloatProviders.CODEC.fieldOf("distance_factor")`
2. `FloatProviders.CODEC.fieldOf("thickness")`
3. `ExtraCodecs.POSITIVE_INT.fieldOf("width_smoothness")` — bytecode-verified
   `intRangeWithMessage(1, 2147483647, …)`, so widthSmoothness ≥ 1 always (keeps §5's
   `nextInt(bound)` legal)
4. `FloatProviders.CODEC.fieldOf("horizontal_radius_factor")`
5. `Codec.FLOAT.fieldOf("vertical_radius_default_factor")`
6. `Codec.FLOAT.fieldOf("vertical_radius_center_factor")`

---

## 9. Vanilla data cross-reference (`reference/carver/canyon.json` = `data/minecraft/worldgen/configured_carver/canyon.json`)

`type: "minecraft:canyon"`; config: `probability` **0.01f**; `y` uniform absolute **10..67**
(⇒ `nextInt(58)+10`); `yScale` **3.0** (plain float JSON → ConstantFloat, **0 draws**, A7 §5.3);
`lava_level` above_bottom 8 ⇒ y ≤ −56 lava (A2 §4, A7 §6.2); `replaceable`
`#minecraft:overworld_carver_replaceables` (A2 §8); `vertical_rotation` uniform **[−0.125, 0.125)**
(⇒ `nf*0.25f − 0.125f`, float ops); shape: `distance_factor` uniform **[0.75, 1.0)**, `thickness`
trapezoid **(0, 6, 2)** (⇒ `nf1*4f + nf2*2f`, A7 §5.2), `width_smoothness` **3**,
`horizontal_radius_factor` uniform **[0.75, 1.0)**, `vertical_radius_default_factor` **1.0f**,
`vertical_radius_center_factor` **0.0f**.

Note: the shipped canyon.json DOES carry a `debug_settings` block (air_state warped_button,
water_state candle, lava_state orange stained glass, barrier_state glass) — but `debug_mode` is
absent ⇒ false ⇒ `isDebugEnabled` false (A2 §4); the block is inert for parity. (Data's air_state
differs from the code DEFAULT acacia_button of A2 §7 — data wins, still irrelevant.)

---

## 10. Complete RNG draw ladder — one canyon (program order)

### OUTER stream — WorldgenRandom, state as left by `setLargeFeatureSeed(worldSeed + idx, nposX, nposZ)` (A1 §4.1)

| # | call | LCG advances | consumer | value (vanilla config) |
|---|------|--------------|----------|------------------------|
| O0 | `nextFloat()` | 1 (next(24)) | `isStartChunk` §2 | proceed iff ≤ 0.01f; on fail NOTHING below happens |
| O1 | `nextInt(16)` | 1 (pow-2, next(31)) | start x | `x = (nposX<<4) + O1` as double |
| O2 | `nextInt(58)` | 1 (+rejection: only u ∈ [2147483640, 2147483647], 8/2³¹) | UniformHeight y | `y = O2 + 10` (int) |
| O3 | `nextInt(16)` | 1 | start z | `z = (nposZ<<4) + O3` as double |
| O4 | `nextFloat()` | 1 | horizontalRotation | `O4 * 6.2831855f` |
| O5 | `nextFloat()` | 1 | verticalRotation (uniform) | `O5*0.25f + (−0.125f)` |
| — | yScale sample | **0** | ConstantFloat 3.0 | `yScale = 3.0d` |
| O6, O7 | `nextFloat()` ×2 | 2 | thickness (trapezoid 0/6/2) | `O6*4f + O7*2f` |
| O8 | `nextFloat()` | 1 | distanceFactor (uniform) | `distance = (int)(112f * (O8*0.25f+0.75f))` ∈ [84, 112] — 112 IS reachable: O8 = 0.99999994f → `*0.25f` = 0.24999999f (0.2499999851) → `+0.75f` rounds to exactly 1.0f (A7 OPEN, verified numerically) |
| O9, O10 | `nextLong()` | 2 (next(32) ×2, hi then lo, lo sign-extended — A7 §3.3) | tunnelSeed | seeds the inner RNG |

Total outer: 1 (start check) + 10 advances. The outer stream is NOT touched again by this carver —
the next consumer is the next carver/neighbor-chunk `setLargeFeatureSeed` (which rescrambles anyway).

### INNER stream — `SingleThreadedRandomSource(tunnelSeed)` (fresh, scrambled seed per A7 §3.1/§3.2)

Phase 1, `initWidthFactors` (§5), once:

| yIndex | draws |
|--------|-------|
| 0 | `nextFloat`, `nextFloat` (2 advances) |
| 1 … depth−1 (=383) | `nextInt(3)` (1 advance; reject-loop only on u = 2³¹−1); if it returned 0: `nextFloat`, `nextFloat` (+2) |

Phase 2, per step `currentStep = 0 … distance−1` (9 advances per step, unconditional):

| # | call | advances | consumer |
|---|------|----------|----------|
| I1 | `nextFloat()` | 1 | horizontalRadiusFactor: `hR *= (double)(I1*0.25f+0.75f)` |
| I2 | `nextFloat()` | 1 | `updateVerticalRadius` §6: `vR = ((double)factor * vR) * (double)(I2*0.25f+0.75f)` |
| I3, I4, I5 | `nextFloat()` ×3 | 3 | `xRota += (I3 − I4) * I5 * 2.0f` |
| I6, I7, I8 | `nextFloat()` ×3 | 3 | `yRota += (I6 − I7) * I8 * 4.0f` |
| I9 | `nextInt(4)` | 1 (pow-2) | ==0 ⇒ skip this step's ellipsoid; ≠0 ⇒ canReach → (return \| carveEllipsoid) |

Early termination: `canReach` false (only checked when I9 ≠ 0) returns from `doCarve` — steps
after `currentStep` contribute NO draws. `carveEllipsoid`/`carveBlock`/aquifer make ZERO draws
(A2 preamble). Nothing after the loop draws.

---

## 11. 26.2 deltas vs 1.21 folklore

1. **Inner RNG factory**: `RandomSource.createThreadLocalInstance(J)` →
   `SingleThreadedRandomSource` (1.21: `RandomSource.create(J)` → `LegacyRandomSource`).
   Bit-identical output (A7 §1, §3.2) — a code delta, not a parity delta.
2. **`Mth.sin/cos` are `(double)→float`** (A7 §4.1): all four trig calls in `doCarve` widen float
   angles via f2d and index the table with double·SIN_SCALE + d2l — 1.21 used float overloads with
   a float SCALE multiply and f2i. Same table indices for almost all inputs but NOT provably all;
   use the 26.2 double path (and the 26.2 table generator `sin(i / 10430.378350470453)`).
3. **No carving-step split**: canyon runs from the flat `getCarvers()` list; there is no
   AIR/LIQUID `GenerationStep.Carving` dispatch (A1 §6). Seeding is `worldSeed + idx` with idx the
   flat list position (canyon is idx 2 in vanilla overworld biomes' `["cave",
   "cave_extra_underground", "canyon"]`, per-biome data).
4. **Algorithm body is UNCHANGED vs 1.18–1.21 mojmap folklore** — now bytecode-verified rather
   than assumed: same constants (1.5 base, π ramp, 0.7/0.05/0.8/0.5 momentum, 2×/4× jitter,
   `nextInt(4)` gate, /6.0 vertical flatten, squared width factors, smoothness draw layout), same
   draw counts, same `y − minGenY − 1` row index.
5. `carveEllipsoid`'s new `isUpgrading` topPad and the 2-arg `setBlockState` are base-class deltas
   (A2 §2/§3), inherited here unchanged.

---

## 12. sulfur scan

`strings … | grep -ci sulfur` over `CanyonWorldCarver.class`, `CanyonCarverConfiguration.class`,
`CanyonCarverConfiguration$CanyonShapeConfiguration.class`: **0 hits each**. No sulfur
special-casing in the canyon carver (expected; data-side sulfur interactions are A2 §8/§10 ground).

---

## 13. OPEN items

- OPEN: `getRange()` is invoked virtually in §3; CanyonWorldCarver does not override it and no
  subclass of CanyonWorldCarver exists in the vanilla tree (value = 4, A2 §1.1). If modded
  subclasses ever matter, `maxDistance` must track the override — for vanilla parity hardcode 112.
- OPEN (inherited from A7): UniformFloat samples can round up to exactly `max` in float
  (affects `distance` potentially hitting 112 and horizontal/vertical factors hitting 1.0).
  Reproduce the float expression bit-exactly; never clamp.
- OPEN (inherited from A7): `nextInt(3)` in §5 and `nextInt(58)` in §10 need the java rejection
  loop modeled (hc_lcg already does); rejection probabilities 1/2³¹ and 8/2³¹.
- OPEN (neighbor ground): `CarvingContext.getMinGenY/getGenDepth` values come from
  `WorldGenerationContext(gen, chunk.getHeightAccessorForGeneration())` (A7 §6.3, A5) — the note
  assumes overworld −64/384; the C impl should take these from the same context object it feeds
  `carveEllipsoid`, not constants.
- Note (not open): `widthFactorPerHeight[0]` is written but provably never read (§7 bounds);
  `initialStep` in §3 is a dead store (call site passes literal 0). A C impl may skip neither the
  slot-0 STORE draws (they consume RNG!) nor materialize the dead local — the draws in §5 happen
  for yIndex 0 regardless.
