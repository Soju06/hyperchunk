# A6 — mod-facing API surface for the order.manifest hook (MC 26.2, unobfuscated server bytecode)

Source of truth: `javap -p -c -constants` (and `javap -v` for BootstrapMethods/constant pool) against
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server` and
`/home/ubuntu/projects/hyperchunk/tools/golden/libs/extracted/server-26.2.jar`; jar listings of the
installed fabric server at `tools/golden/work/stagedump-run`. All pseudocode is a 1:1 reconstruction
from bytecode. No vanilla-source guessing. Grep tip verified this session: class files are binary —
`grep -rla '<name>' .` (note `-a`), plain `grep -rl` silently returns nothing on this tree.

Companion notes: A1–A5/A7 (task8-carvers) for carver internals; sibling task9pre-order notes cover
the decoration loop itself. This note covers ONLY what the stage-dump mod needs to touch:
version string, seed, dimension id, ChunkStatus.FEATURES identity, mixin/MixinExtras plumbing,
and the existing dump header contract the manifest will sit next to.

---

## 1. Version string for the manifest header

### 1.1 `net.minecraft.SharedConstants` (class, not record)

```java
public static WorldVersion getCurrentVersion() {
    if (CURRENT_VERSION == null) throw new IllegalStateException("Game version not set");
    return CURRENT_VERSION;                       // private static WorldVersion CURRENT_VERSION
}
public static void tryDetectVersion() {           // idempotent
    if (CURRENT_VERSION == null) CURRENT_VERSION = DetectedVersion.tryDetectVersion();
}
public static final int WORLD_VERSION = 4903;     // compile-time constant in 26.2
public static final String SERIES = "main";
```

### 1.2 `net.minecraft.WorldVersion` is an INTERFACE in 26.2

```java
public interface net.minecraft.WorldVersion {
    DataVersion dataVersion();     // record DataVersion(int version, String series)
    String id();                   // <-- "26.2"
    String name();                 // <-- "26.2"
    int protocolVersion();         // 776
    PackFormat packVersion(PackType);
    java.util.Date buildTime();
    boolean stable();
}
```

**There is NO `getName()`/`getId()` in 26.2** — the accessors are the record-component names
`name()` / `id()`. The concrete impl is the record `net.minecraft.WorldVersion$Simple`
(components in order: `id`, `name`, `dataVersion`, `protocolVersion`, `resourcePackVersion`,
`datapackVersion`, `buildTime`, `stable`).

### 1.3 Where "26.2" actually comes from at runtime

`DetectedVersion.tryDetectVersion()` reads `/version.json` from the server jar via
`Class.getResourceAsStream` → `createFromJson` (keys: `"id"`, `"name"`, `"world_version"`,
`"series_id"`, `"protocol_version"`, `"pack_version"{resource_major/minor,data_major/minor}`,
`"build_time"`, `"stable"`). Extracted from `server-26.2.jar` this session:

```json
{ "id": "26.2", "name": "26.2", "world_version": 4903, "series_id": "main",
  "protocol_version": 776, "stable": true, ... }
```

Fallback `DetectedVersion.BUILT_IN` (only if version.json is missing) has
`id = UUID.randomUUID().toString().replaceAll("-","")`, `name = "Development Version"` — so the
manifest writer should use the real accessor, never hardcode.

**Exact accessor chain for the manifest header:**

```java
WorldVersion v = net.minecraft.SharedConstants.getCurrentVersion();
String gameVersion = v.id();                   // "26.2"  (v.name() is also "26.2" for releases)
int dataVersion    = v.dataVersion().version(); // 4903   (DataVersion.version(), series())
```

---

## 2. Seed + dimension id from inside the features stage

### 2.1 Objects in scope

`ChunkStatusTasks#generateFeatures(WorldGenContext, ChunkStep, StaticCache2D<GenerationChunkHolder>, ChunkAccess)`
(bytecode, exact reconstruction):

```java
public static CompletableFuture<ChunkAccess> generateFeatures(WorldGenContext ctx, ChunkStep step,
        StaticCache2D<GenerationChunkHolder> cache, ChunkAccess chunk) {
    ServerLevel serverLevel = ctx.level();
    Heightmap.primeHeightmaps(chunk, EnumSet.of(MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES,
                                                OCEAN_FLOOR, WORLD_SURFACE));
    WorldGenRegion region = new WorldGenRegion(serverLevel, cache, step, chunk);
    if (!SharedConstants.DEBUG_DISABLE_FEATURES) {
        ctx.generator().applyBiomeDecoration(region, chunk,
                serverLevel.structureManager().forWorldGenRegion(region));
    }
    Blender.generateBorderTicks(region, chunk);
    return CompletableFuture.completedFuture(chunk);
}
```

`net.minecraft.world.level.chunk.status.WorldGenContext` is a record; accessors (component names):
`level()` → `ServerLevel`, `generator()` → `ChunkGenerator`, `structureManager()` →
`StructureTemplateManager`, `lightEngine()`, `mainThreadExecutor()`, `unsavedListener()`.
No seed component — go through the level.

### 2.2 Seed

- `net.minecraft.world.level.WorldGenLevel` (interface,
  `extends ServerLevelAccessor`) declares `public abstract long getSeed();`.
- `net.minecraft.server.level.WorldGenRegion implements WorldGenLevel`:
  ctor `WorldGenRegion(ServerLevel, StaticCache2D<GenerationChunkHolder>, ChunkStep, ChunkAccess)`
  does `this.seed = serverLevel.getSeed()` (putfield `seed:J`; the same value also feeds
  `BiomeManager.obfuscateSeed(seed)` for the region's BiomeManager). `getSeed()` returns the field.
- `net.minecraft.server.level.ServerLevel#getSeed()`:

```java
public long getSeed() {
    return this.server.getWorldGenSettings().options().seed();
    // MinecraftServer.getWorldGenSettings() -> WorldGenSettings.options() -> WorldOptions.seed()
}
```

- Corroboration that this is THE decoration seed: `ChunkGenerator#applyBiomeDecoration` builds
  `new WorldgenRandom(new XoroshiroRandomSource(worldGenLevel.getSeed()))` and calls
  `WorldgenRandom.setDecorationSeed(getSeed(), minBlockX, minBlockZ)` (invokeinterface
  `WorldGenLevel.getSeed:()J` at offset 112 of that method).

**Chains available to the mod (equivalent values):**

```java
long seed = ctx.level().getSeed();          // from WorldGenContext (ServerLevel)
long seed = region.getSeed();               // from the WorldGenLevel passed to applyBiomeDecoration
```

### 2.3 Dimension id

- `WorldGenRegion` has **no** `dimension()` method (negative finding: `javap -p` lists only
  `dimensionType()` → `DimensionType`, which carries no id). Go through the level:
- `WorldGenRegion#getLevel()` → returns the `level:Lnet/minecraft/server/level/ServerLevel;` field.
- `net.minecraft.world.level.Level#dimension()` → returns field
  `dimension:Lnet/minecraft/resources/ResourceKey;` (`ResourceKey<Level>`). Inherited by ServerLevel.
- `net.minecraft.resources.ResourceKey#identifier()` → returns field
  `identifier:Lnet/minecraft/resources/Identifier;`. **Confirmed: 26.2 name is `identifier()`**
  (no `location()` exists on this class).
- `net.minecraft.resources.Identifier#toString()` → StringConcat recipe `:` =
  `namespace + ":" + path` (e.g. `"minecraft:overworld"`). Also `getNamespace()`, `getPath()`.

```java
String dim = ctx.level().dimension().identifier().toString();          // from WorldGenContext
String dim = ((WorldGenRegion) level).getLevel().dimension().identifier().toString(); // from region
```

The existing `StageDumper.wants(...)` already uses exactly
`ctx.level().dimension().identifier().toString()` — same chain, already proven at runtime.

---

## 3. `ChunkStatus.FEATURES`

Class `net.minecraft.world.level.chunk.status.ChunkStatus`.

- **Field:** `public static final ChunkStatus FEATURES;` — assigned 8th in `static {}`, registered as
  `register("features", CARVERS, FINAL_HEIGHTMAPS, ChunkType.PROTOCHUNK)`
  (ldc `"features"` at offset 147, putstatic `FEATURES` at 161).
  Full registration order: `empty, structure_starts, structure_references, biomes, noise, surface,
  carvers, features, initialize_light, light, spawn, full`.
- **`getIndex()`** returns field `index:I`; ctor computes
  `this.index = (parent == null) ? 0 : parent.getIndex() + 1`. Parent chain gives
  empty=0 … carvers=6, **FEATURES.getIndex() == 7**. (Runtime-corroborated by the existing dump
  filename prefix `07_features` = `String.format("%02d_%s", status.getIndex(), strippedName)`.)
- **`getName()`** (bytecode):

```java
public String getName() {
    return BuiltInRegistries.CHUNK_STATUS.getKey(this).toString();
}
```

  `register` goes through `Registry.register(BuiltInRegistries.CHUNK_STATUS, "features", new ChunkStatus(...))`
  → `Identifier.parse("features")` (default namespace) — so **`getName()` returns
  `"minecraft:features"`, namespaced**. `StageDumper.stageName()` strips the `"minecraft:"` prefix
  before use; the manifest should reuse `StageDumper.stageName(status)` for consistency with the
  existing dump headers ("features", not "minecraft:features").
- Wiring: `ChunkStatusTasks::generateFeatures` appears as a method-handle constant in
  `ChunkPyramid` only (constant pool `REF_invokeStatic … generateFeatures`), used twice in
  `ChunkPyramid.<clinit>` — once per pyramid (`GENERATION_PYRAMID`, `LOADING_PYRAMID`), each via
  `ChunkPyramid$Builder.step(ChunkStatus.FEATURES, UnaryOperator)` (offsets 84 and 229).
  **Negative finding:** `grep -rla generateFeatures net/` → only `ChunkPyramid.class` and
  `ChunkStatusTasks.class`; no other call sites.

`ChunkStep` (record) surface used by the existing mixin, unchanged:
`apply(WorldGenContext, StaticCache2D<GenerationChunkHolder>, ChunkAccess) → CompletableFuture<ChunkAccess>`,
`targetStatus() → ChunkStatus`, plus `directDependencies()/accumulatedDependencies()/blockStateWriteRadius()/task()`.

---

## 4. MixinExtras availability under Fabric Loader 0.19.3

Checked artifacts (installed server at `tools/golden/work/stagedump-run`):

| artifact | mixinextras classes? |
|---|---|
| `libraries/net/fabricmc/fabric-loader/0.19.3/fabric-loader-0.19.3.jar` | **yes — nested `META-INF/jars/mixinextras-fabric-0.5.4.jar`** (727805 bytes) |
| `libraries/net/fabricmc/sponge-mixin/0.17.3+mixin.0.8.7/sponge-mixin-0.17.3+mixin.0.8.7.jar` | no |
| `fabric-server-launch.jar` | no |
| `.fabric/processedMods/` | **`mixinextras-0.5.4-edfcdd3df0b35281.jar`** — proof the loader actually extracted+loaded it as a mod on this install |

Inside the nested jar (506 class entries):

- **Package is `com.llamalad7.mixinextras` — NOT `io.github.llamalad7`** (negative finding vs. the
  task prompt's guess; no `io/github/llamalad7/` entries exist).
- `com/llamalad7/mixinextras/injector/wrapoperation/WrapOperation.class` + `Operation.class` present;
  also `injector/wrapmethod/WrapMethod.class`, `injector/ModifyExpressionValue.class`, sugar, etc.
- Its `fabric.mod.json`: `id "mixinextras"`, version `0.5.4`, `provides ["com_github_llamalad7_mixinextras"]`,
  own mixin config `mixinextras.init.mixins.json`, `depends {"fabricloader": ">=0.14.25"}` —
  i.e. it self-initializes as a Fabric mod; **no manual `MixinExtras.init()` call needed**.
- Manifest: `Fabric-Loom-Remap: false` (names are already runtime names; fine for our loom-less build).

**Answer: yes, the mod can use `@WrapOperation` (and `@WrapMethod`, `@ModifyExpressionValue`) at
runtime — MixinExtras 0.5.4 is bundled by loader 0.19.3 itself and was loaded on the actual
stagedump run.** The only gap is compile-time: `stage-dump-mod/build.gradle` currently has

```groovy
dependencies {
    compileOnly files(new File(extracted, "server-${targetVersion}.jar"))
    compileOnly fileTree(dir: new File(extracted, 'libraries'), include: '**/*.jar')
    compileOnly fileTree(dir: fabricLibs, include: 'sponge-mixin-*.jar')
}
```

— no mixinextras on the compile classpath. To compile `@WrapOperation`, extract the nested jar once
(e.g. `unzip tools/golden/libs/fabric/fabric-loader-0.19.3.jar 'META-INF/jars/mixinextras-fabric-0.5.4.jar'`
into `tools/golden/libs/fabric/`) and add `compileOnly fileTree(dir: fabricLibs, include: 'mixinextras-*.jar')`.
Plain `@Inject` needs no build change.

### 4.1 How a second mixin class gets registered

`src/main/resources/hyperchunk-stagedump.mixins.json` (current, verbatim):

```json
{
  "required": true,
  "package": "dev.hyperchunk.stagedump.mixin",
  "compatibilityLevel": "JAVA_25",
  "mixins": [ "ChunkStepMixin" ],
  "injectors": { "defaultRequire": 1 }
}
```

Add the new class under the same package (`dev/hyperchunk/stagedump/mixin/…`) and append its simple
name to the `"mixins"` array. Nothing else changes: `fabric.mod.json` already points at this config
(`"mixins": ["hyperchunk-stagedump.mixins.json"]`, `"depends": {"minecraft": "26.2"}`). The existing
hook is `ChunkStepMixin.hyperchunk$dumpAfterStage` — `@Inject(method = "apply", at = @At("RETURN"),
cancellable = true)` on `ChunkStep`, composing `cir.getReturnValue().thenApply(...)`.

---

## 5. Existing dump header contract (what the manifest sits next to)

`golden/stages/seed1234567890/c.0.0/07_features.blocks.txt`, first lines (verbatim):

```
# hyperchunk golden stage dump v1
# kind blocks
# chunk 0 0
# stage features
# minY -64 maxY 319 height 384
# order y in [minY..maxY] asc, then z in [0..15], one row of 16 x-asc palette indices
palette 0 minecraft:bedrock
palette 1 minecraft:deepslate[axis=y]
...
```

`06_carvers.blocks.txt` is identical in shape (`# stage carvers`, own palette, then `data` +
16-int rows). Header writer is `StageDumper.header(w, chunk, stage, kind, order)`; filenames are
`%02d_%s.<kind>.txt` with `%02d` = `ChunkStatus.getIndex()` and `%s` = `stageName(status)`
(= `getName()` minus `"minecraft:"`). Files live under `<dumpDir>/c.<x>.<z>/`; per-run root is
keyed by seed dir (`seed1234567890/`). Dump gating properties: `hyperchunk.dump.dir`,
`hyperchunk.dump.stages` (names WITHOUT the `NN_` prefix), `hyperchunk.dump.dimension`
(default `minecraft:overworld`), center/radius.

---

## 6. Implications for the order manifest

1. **Header fields, exact chains:** game version `SharedConstants.getCurrentVersion().id()` ("26.2");
   data version `…getCurrentVersion().dataVersion().version()` (4903); seed `ctx.level().getSeed()`
   (identical to the value seeding `WorldgenRandom.setDecorationSeed`); dimension
   `ctx.level().dimension().identifier().toString()`. All reachable from the objects the existing
   mixin already holds (`WorldGenContext`) — no new capture plumbing needed.
2. **Stage identity:** key manifest records by the same convention as dumps: index 7 + stripped name
   "features" via `StageDumper.stageName(ChunkStatus.FEATURES)`; don't write raw `getName()`
   ("minecraft:features") or the files won't line up with `07_features.*`.
3. **Hook placement:** `ChunkGenerator#applyBiomeDecoration(WorldGenLevel, ChunkAccess, StructureManager)`
   has exactly one production call site (`ChunkStatusTasks.generateFeatures`); the only other
   override is `DebugLevelSource` (debug-world grid, no features — irrelevant to golden runs).
   So a mixin on `ChunkGenerator` (or interception inside `generateFeatures`) observes 100% of
   features-stage decoration. `NoiseBasedChunkGenerator`/`FlatLevelSource` do NOT override it
   (grep -rla `applyBiomeDecoration` → only ChunkGenerator, ChunkStatusTasks, DebugLevelSource).
4. **Mixin tech:** `@WrapOperation` is available (MixinExtras 0.5.4 bundled in loader 0.19.3,
   package `com.llamalad7.mixinextras.…`) — usable for wrapping the per-feature `place` call sites
   inside `applyBiomeDecoration` if the manifest needs per-placement capture; requires the one-line
   build.gradle classpath addition (§4). Plain `@Inject` on a second mixin class needs only a
   `mixins.json` array append.
5. **Per-chunk trigger:** the existing `ChunkStep.apply` RETURN hook fires once per (chunk, stage)
   after completion — the right place to flush a per-chunk manifest record next to the `07_*` dumps;
   ordering data itself must be captured inside `applyBiomeDecoration` (sibling notes).
