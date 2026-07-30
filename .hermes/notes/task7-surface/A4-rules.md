# A4 — SurfaceRules rule sources + interfaces (26.2 bytecode reconstruction)

Source of truth: `javap -p -c -constants` (plus `-v` for BootstrapMethods / invokedynamic
resolution) against `/home/ubuntu/projects/hyperchunk/tools/golden/work/server`,
package `net.minecraft.world.level.levelgen` unless noted.

All pseudocode below is a 1:1 reconstruction of the bytecode. No vanilla-source guessing.

---

## 1. Interface shapes

### 1.1 `SurfaceRules$RuleSource` (public interface)

```java
public interface RuleSource extends Function<SurfaceRules.Context, SurfaceRules.SurfaceRule> {
    Codec<RuleSource> CODEC =
        BuiltInRegistries.MATERIAL_RULE.byNameCodec()
            .dispatch(RuleSource::codec, Function.identity());
        // dispatch args: type-getter = RuleSource::codec (verified via BootstrapMethods:
        //   REF_invokeInterface RuleSource.codec:()MapCodec), value-codec-getter = identity.
        // Standard "type" field dispatch codec.

    MapCodec<? extends RuleSource> codec();

    static MapCodec<? extends RuleSource> bootstrap(Registry<MapCodec<? extends RuleSource>> registry) {
        SurfaceRules.register(registry, "bandlands", Bandlands.CODEC);
        SurfaceRules.register(registry, "block",     BlockRuleSource.CODEC);
        SurfaceRules.register(registry, "sequence",  SequenceRuleSource.CODEC);
        return SurfaceRules.register(registry, "condition", TestRuleSource.CODEC);  // last one is returned
    }
}
```

Registration order (registry IDs): `bandlands`(0), `block`(1), `sequence`(2), `condition`(3).

### 1.2 `SurfaceRules$ConditionSource` (public interface)

```java
public interface ConditionSource extends Function<SurfaceRules.Context, SurfaceRules.Condition> {
    Codec<ConditionSource> CODEC =
        BuiltInRegistries.MATERIAL_CONDITION.byNameCodec()
            .dispatch(ConditionSource::codec, Function.identity());

    MapCodec<? extends ConditionSource> codec();

    static MapCodec<? extends ConditionSource> bootstrap(Registry<MapCodec<? extends ConditionSource>> registry) {
        SurfaceRules.register(registry, "biome",                     BiomeConditionSource.CODEC);
        SurfaceRules.register(registry, "noise_threshold",           NoiseThresholdConditionSource.CODEC);
        SurfaceRules.register(registry, "vertical_gradient",         VerticalGradientConditionSource.CODEC);
        SurfaceRules.register(registry, "y_above",                   YConditionSource.CODEC);
        SurfaceRules.register(registry, "water",                     WaterConditionSource.CODEC);
        SurfaceRules.register(registry, "temperature",               Temperature.CODEC);
        SurfaceRules.register(registry, "steep",                     Steep.CODEC);
        SurfaceRules.register(registry, "not",                       NotConditionSource.CODEC);
        SurfaceRules.register(registry, "hole",                      Hole.CODEC);
        SurfaceRules.register(registry, "above_preliminary_surface", AbovePreliminarySurface.CODEC);
        return SurfaceRules.register(registry, "stone_depth",        StoneDepthCheck.CODEC);
    }
}
```

Registration order: `biome`(0), `noise_threshold`(1), `vertical_gradient`(2), `y_above`(3),
`water`(4), `temperature`(5), `steep`(6), `not`(7), `hole`(8),
`above_preliminary_surface`(9), `stone_depth`(10).

### 1.3 `SurfaceRules$SurfaceRule` (package-visible via protected inner; public interface in class file)

```java
public interface SurfaceRule {
    @Nullable BlockState tryApply(int x, int y, int z);   // descriptor (III)Lnet/...BlockState;
}
```

Argument order is `(x, y, z)` per how callers pass them (SequenceRule/TestRule forward
iload_1, iload_2, iload_3 unchanged; Bandlands binds `SurfaceSystem.getBand(III)` directly —
see §6).

### 1.4 `SurfaceRules$Condition` (package-private interface)

```java
interface Condition {
    boolean test();     // no arguments — all position state lives in Context
}
```

---

## 2. `SurfaceRules` outer class

### 2.1 Static fields (from `static {}`)

Bytecode of `<clinit>` in exact order:

```java
public static final ConditionSource ON_FLOOR             = stoneDepthCheck(0, false, CaveSurface.FLOOR);
public static final ConditionSource UNDER_FLOOR          = stoneDepthCheck(0, true,  CaveSurface.FLOOR);
public static final ConditionSource DEEP_UNDER_FLOOR     = stoneDepthCheck(0, true, 6,  CaveSurface.FLOOR);
public static final ConditionSource VERY_DEEP_UNDER_FLOOR= stoneDepthCheck(0, true, 30, CaveSurface.FLOOR);
public static final ConditionSource ON_CEILING           = stoneDepthCheck(0, false, CaveSurface.CEILING);
public static final ConditionSource UNDER_CEILING        = stoneDepthCheck(0, true,  CaveSurface.CEILING);
```

### 2.2 Factory methods (exact bytecode-equivalent)

```java
public static ConditionSource stoneDepthCheck(int offset, boolean addSurfaceDepth, CaveSurface surfaceType) {
    return new StoneDepthCheck(offset, addSurfaceDepth, 0, surfaceType);
    // ctor descriptor (IZILnet/.../CaveSurface;)V — 3rd int param (secondaryDepthRange) hardcoded 0
}

public static ConditionSource stoneDepthCheck(int offset, boolean addSurfaceDepth,
                                              int secondaryDepthRange, CaveSurface surfaceType) {
    return new StoneDepthCheck(offset, addSurfaceDepth, secondaryDepthRange, surfaceType);
}

public static ConditionSource not(ConditionSource target) {
    return new NotConditionSource(target);
}

public static ConditionSource yBlockCheck(VerticalAnchor anchor, int surfaceDepthMultiplier) {
    return new YConditionSource(anchor, surfaceDepthMultiplier, false);   // addStoneDepth=false
}

public static ConditionSource yStartCheck(VerticalAnchor anchor, int surfaceDepthMultiplier) {
    return new YConditionSource(anchor, surfaceDepthMultiplier, true);    // addStoneDepth=true
}

public static ConditionSource waterBlockCheck(int offset, int surfaceDepthMultiplier) {
    return new WaterConditionSource(offset, surfaceDepthMultiplier, false);
}

public static ConditionSource waterStartCheck(int offset, int surfaceDepthMultiplier) {
    return new WaterConditionSource(offset, surfaceDepthMultiplier, true);
}

@SafeVarargs
public static ConditionSource isBiome(HolderGetter<Biome> biomes, ResourceKey<Biome>... keys) {
    Objects.requireNonNull(biomes);
    return new BiomeConditionSource(HolderSet.direct(biomes::getOrThrow, keys));
    // lambda resolved via BootstrapMethods: REF_invokeInterface
    //   HolderGetter.getOrThrow:(ResourceKey)Holder$Reference
}

public static ConditionSource noiseCondition2d(ResourceKey<NormalNoise.NoiseParameters> noise, double minThreshold) {
    return noiseCondition2d(noise, minThreshold, 1.7976931348623157E308);   // Double.MAX_VALUE, exact ldc2_w constant
}

public static ConditionSource noiseCondition2d(ResourceKey<NormalNoise.NoiseParameters> noise,
                                               double minThreshold, double maxThreshold) {
    return new NoiseThresholdConditionSource(noise, minThreshold, maxThreshold, false);  // is3d=false
}

public static ConditionSource noiseCondition3d(ResourceKey<NormalNoise.NoiseParameters> noise, double minThreshold) {
    return noiseCondition3d(noise, minThreshold, 1.7976931348623157E308);   // Double.MAX_VALUE
}

public static ConditionSource noiseCondition3d(ResourceKey<NormalNoise.NoiseParameters> noise,
                                               double minThreshold, double maxThreshold) {
    return new NoiseThresholdConditionSource(noise, minThreshold, maxThreshold, true);   // is3d=true
}

public static ConditionSource verticalGradient(String randomName, VerticalAnchor trueAtAndBelow,
                                               VerticalAnchor falseAtAndAbove) {
    return new VerticalGradientConditionSource(Identifier.parse(randomName), trueAtAndBelow, falseAtAndAbove);
    // NOTE: net.minecraft.resources.Identifier (26.x rename of ResourceLocation), Identifier.parse(String)
}

public static ConditionSource steep()                    { return Steep.INSTANCE; }
public static ConditionSource hole()                     { return Hole.INSTANCE; }
public static ConditionSource abovePreliminarySurface()  { return AbovePreliminarySurface.INSTANCE; }
public static ConditionSource temperature()              { return Temperature.INSTANCE; }

public static RuleSource ifTrue(ConditionSource ifTrue, RuleSource thenRun) {
    return new TestRuleSource(ifTrue, thenRun);
}

public static RuleSource sequence(RuleSource... rules) {
    if (rules.length == 0) {                                  // `ifne` on arraylength → throws only when == 0
        throw new IllegalArgumentException("Need at least 1 rule for a sequence");
    }
    return new SequenceRuleSource(Arrays.asList(rules));
}

public static RuleSource state(BlockState state) { return new BlockRuleSource(state); }

public static RuleSource bandlands() { return Bandlands.INSTANCE; }

private static <A> MapCodec<? extends A> register(Registry<MapCodec<? extends A>> registry,
                                                  String name, MapCodec<? extends A> codec) {
    return (MapCodec<? extends A>) Registry.register(registry, name, codec);
}
```

No RNG consumption anywhere in the outer class or in any class of this cluster —
these are pure structural/dispatch classes. (The RNG for bandlands lives inside
`SurfaceSystem.getBand`, outside this cluster.)

---

## 3. `SequenceRuleSource` / `SequenceRule`

### 3.1 `SequenceRuleSource` — record, 1 component

- Component: `List<RuleSource> sequence` (private final field, canonical private ctor stores it directly, no copy).
- CODEC (from `<clinit>`, verified with BootstrapMethods):

```java
static final MapCodec<SequenceRuleSource> CODEC =
    RuleSource.CODEC.listOf()
        .xmap(SequenceRuleSource::new, SequenceRuleSource::sequence)
        .fieldOf("sequence");
```

JSON shape: `{ "type": "minecraft:sequence", "sequence": [ <RuleSource>, ... ] }`.
No defaults; `sequence` is required. (The `length >= 1` check exists only in the Java
`sequence(...)` factory — the codec itself has no size validation in this bytecode.)

### 3.2 `SequenceRuleSource.apply(Context)` — SINGLETON SPECIAL CASE confirmed

```java
public SurfaceRule apply(Context ctx) {
    if (this.sequence.size() == 1) {                       // if_icmpne 36 → equality test, exactly == 1
        return this.sequence.get(0).apply(ctx);            // returns the child rule DIRECTLY — no SequenceRule wrapper
    }
    ImmutableList.Builder<SurfaceRule> builder = ImmutableList.builder();
    for (RuleSource rs : this.sequence) {                  // List.iterator() — declaration order
        builder.add(rs.apply(ctx));                        // children applied EAGERLY, in order
    }
    return new SequenceRule(builder.build());
}
```

Key facts:
- Singleton sequence collapses to the child's rule (identity pass-through).
- Otherwise all children are materialized immediately (eager `apply`), in list order,
  into an ImmutableList.

### 3.3 `SequenceRule` — record, 1 component `List<SurfaceRule> rules`

```java
public @Nullable BlockState tryApply(int x, int y, int z) {
    for (Iterator<SurfaceRule> it = this.rules.iterator(); it.hasNext(); ) {   // iterator, NOT index loop
        SurfaceRule rule = it.next();
        BlockState result = rule.tryApply(x, y, z);
        if (result != null) {                              // ifnull → first NON-NULL wins
            return result;
        }
    }
    return null;                                           // all children null → null
}
```

First-non-null semantics, strict forward iteration order, args forwarded verbatim `(x,y,z)`.
No caching, no short-circuit beyond first non-null.

---

## 4. `BlockRuleSource` / `StateRule`

### 4.1 `BlockRuleSource` — record, 2 components (`resultState`, `rule`)

- Canonical (private) ctor: `(BlockState resultState, StateRule rule)` — stores fields in that order.
- Convenience (private) ctor used by codec and `state(...)`:

```java
private BlockRuleSource(BlockState state) {
    this(state, new StateRule(state));       // StateRule pre-built at construction time
}
```

- CODEC (`<clinit>` + BootstrapMethods):

```java
static final MapCodec<BlockRuleSource> CODEC =
    BlockState.CODEC
        .xmap(BlockRuleSource::new, BlockRuleSource::resultState)
        .fieldOf("result_state");
```

JSON shape: `{ "type": "minecraft:block", "result_state": { "Name": ..., "Properties": {...} } }`
(field name confirmed as the string constant `result_state`; inner shape belongs to
`BlockState.CODEC`, not analyzed here). Only `resultState` round-trips; `rule` is derived.

### 4.2 `BlockRuleSource.apply(Context)`

```java
public SurfaceRule apply(Context ctx) {
    return this.rule;      // context ignored entirely; returns the shared pre-built StateRule
}
```

### 4.3 `StateRule` — record, 1 component `BlockState state`

```java
public BlockState tryApply(int x, int y, int z) {
    return this.state;     // never null, position ignored → terminal rule
}
```

---

## 5. `TestRuleSource` / `TestRule`

### 5.1 `TestRuleSource` — record, 2 components (`ifTrue`, `thenRun`)

- Canonical private ctor: `(ConditionSource ifTrue, RuleSource thenRun)`, fields stored in that order.
- CODEC — RecordCodecBuilder, from `lambda$static$0` (string constants directly visible in bytecode):

```java
static final MapCodec<TestRuleSource> CODEC = RecordCodecBuilder.mapCodec(instance ->
    instance.group(
        ConditionSource.CODEC.fieldOf("if_true").forGetter(TestRuleSource::ifTrue),
        RuleSource.CODEC.fieldOf("then_run").forGetter(TestRuleSource::thenRun)
    ).apply(instance, TestRuleSource::new)
);
```

JSON shape: `{ "type": "minecraft:condition", "if_true": <ConditionSource>, "then_run": <RuleSource> }`.
Both fields required (`fieldOf`, no `optionalFieldOf`), no defaults.

### 5.2 `TestRuleSource.apply(Context)`

```java
public SurfaceRule apply(Context ctx) {
    return new TestRule(
        this.ifTrue.apply(ctx),      // Condition built FIRST (bytecode order: getfield ifTrue, apply)
        this.thenRun.apply(ctx)      // then the followup SurfaceRule
    );
}
```

Both sub-applies are eager. Evaluation order matters only if Context application is
stateful in a sub-source (e.g. lazy-condition registration order) — order is
ifTrue-then-thenRun, exactly.

### 5.3 `TestRule` — record, 2 components (`condition`, `followup`)

```java
public @Nullable BlockState tryApply(int x, int y, int z) {
    if (!this.condition.test()) {          // ifne 14: test()==false → return null
        return null;
    }
    return this.followup.tryApply(x, y, z);  // test()==true → delegate (may itself return null)
}
```

Exact semantics: condition false → `null` (rule "does not apply"); condition true →
whatever the followup returns, including null. `Condition.test()` takes NO position
args — position comes from the shared mutable Context.

---

## 6. `Bandlands`

Singleton enum (`INSTANCE`, sole constant) implementing `RuleSource`.

- CODEC (`<clinit>`): `MapCodec.unit(Bandlands.INSTANCE)` — JSON shape is just
  `{ "type": "minecraft:bandlands" }`, no fields.

### 6.1 `Bandlands.apply(Context)`

```java
public SurfaceRule apply(Context ctx) {
    SurfaceSystem system = ctx.system;            // getfield Context.system (a field, not a getter)
    Objects.requireNonNull(system);
    return system::getBand;                       // method-ref bound as the SurfaceRule
}
```

invokedynamic resolved via BootstrapMethods:
`REF_invokeVirtual net/minecraft/world/level/levelgen/SurfaceSystem.getBand:(III)Lnet/.../BlockState;`
with interface method type `(III)BlockState` — i.e. `SurfaceRule.tryApply(x, y, z)`
maps 1:1 onto `SurfaceSystem.getBand(x, y, z)` with IDENTICAL argument order (x, y, z; no swizzle).

`SurfaceSystem.getBand` signature (not deep-dived here, cluster boundary):
`protected BlockState getBand(int, int, int)` — clay-bands lookup lives in SurfaceSystem
(A-cluster for SurfaceSystem covers its RNG: the band noise/offset RNG is consumed there,
not in this class).

Note: `Context.system` is accessed as a direct field read (`getfield`), so Context exposes
`system` at least package-visibly.

---

## 7. Cross-cutting exactness notes

- **Nullability protocol**: `SurfaceRule.tryApply` returning `null` = "no result, try next".
  `SequenceRule` implements first-non-null; `TestRule` converts condition-false to null;
  `StateRule` never returns null; `getBand` nullability is SurfaceSystem's business
  (it can return null — SequenceRule handles it either way).
- **No RNG, no noise, no hashing** in any method of this cluster. Zero calls to
  `RandomSource`, `forkPositional`, `fromHashOf`, or `at(x,y,z)` appear in any bytecode here.
- **Apply-time vs test-time split**: all `RuleSource.apply(Context)` / `ConditionSource.apply(Context)`
  work happens once per Context (per chunk in practice); `tryApply(x,y,z)` / `test()` are the
  per-block hot path. The C compiler should mirror this two-phase structure.
- **`register` return value**: `bootstrap` returns the LAST registered codec
  (`condition` for rules, `stone_depth` for conditions) — this is only used to force
  classloading/registration; irrelevant for data.

## 8. New / changed vs 1.21 (from bytecode evidence only)

1. **`noiseCondition2d` / `noiseCondition3d`** — 1.21 had a single `noiseCondition(key, min[, max])`.
   26.2 splits it and threads a new `boolean` (2d=false / 3d=true) into
   `NoiseThresholdConditionSource.<init>(ResourceKey, double, double, Z)`. A 3D surface-noise
   threshold condition is new machinery (plausibly for sulfur caves; the condition class itself
   is another cluster — see its report for how the flag changes sampling).
2. **`Identifier.parse(String)`** — `ResourceLocation` is renamed `net.minecraft.resources.Identifier`
   in 26.x; `verticalGradient` uses `Identifier.parse` instead of `new ResourceLocation(...)`.
3. Registry names unchanged from 1.21 (`bandlands`/`block`/`sequence`/`condition`;
   all 11 condition names identical, and NO new condition or rule types were added to
   `bootstrap` — sulfur-cave surface logic must therefore ride on existing types
   (noise_threshold-3d + vertical_gradient etc.), not a new "type" string).
4. Everything else (sequence singleton collapse, first-non-null, TestRule short-circuit,
   Bandlands→getBand method ref, `result_state`/`if_true`/`then_run` field names,
   `Double.MAX_VALUE` default max threshold, VERY_DEEP_UNDER_FLOOR=30, DEEP=6) matches
   the 1.21-era structure bit-for-bit at this layer.
