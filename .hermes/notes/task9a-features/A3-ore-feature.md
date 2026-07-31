# A3 — `OreFeature` / `ScatteredOreFeature` (MC 26.2, unobfuscated server bytecode)

Source of truth: `javap -p -c -constants` (plus `javap -v` for BootstrapMethods) against
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server`. Classes disassembled in this
session: `OreFeature`, `ScatteredOreFeature`, `Feature` (isAdjacentToAir/checkNeighbors,
static registration), `configurations/OreConfiguration` (+`$TargetBlockState`),
`templatesystem/{RuleTest,RuleTestType,TagMatchTest,BlockMatchTest,BlockStateMatchTest,RandomBlockMatchTest}`,
`chunk/BulkSectionAccess`, `chunk/LevelChunkSection`, `chunk/ChunkAccess` (getHeight),
`levelgen/Heightmap` (getFirstAvailable), `core/SectionPos`, `core/Direction`,
`core/TypedInstance`, `state/StateHolder`, `state/BlockBehaviour$BlockStateBase`,
`server/level/WorldGenRegion` (getHeight, warnIfReadOutsideWriteZone), `util/Mth`
(sin/cos/lerp/ceil/floor + SIN table init), `LevelHeightAccessor` (isOutsideBuildHeight).
Everything below is a 1:1 bytecode reconstruction unless explicitly marked UNVERIFIED.

RNG convention: `random` is the per-feature `WorldgenRandom` (Xoroshiro-backed wrapper, see
A3-worldgenrandom-seeding.md) already positioned by `setFeatureSeed`. Every draw below names
the exact wrapper method; the C replay must burn identical draws in identical order.

---

## 1. `OreFeature.place(FeaturePlaceContext)` — exact

```java
public boolean place(FeaturePlaceContext<OreConfiguration> ctx) {
    RandomSource random = ctx.random();
    BlockPos origin     = ctx.origin();
    WorldGenLevel level = ctx.level();
    OreConfiguration cfg = (OreConfiguration) ctx.config();

    // DRAW 1: random.nextFloat()
    float angle = random.nextFloat() * 3.1415927f;          // fmul, float; constant is (float)PI
    float f7    = (float) cfg.size / 8.0f;                  // i2f then fdiv, float
    int   i8    = Mth.ceil((((float)cfg.size / 16.0f) * 2.0f + 1.0f) / 2.0f);  // ALL float ops, then ceil

    // Endpoints: JDK Math.sin / Math.cos on (double)angle  — NOT the Mth table!
    double x1 = (double)origin.getX() + Math.sin((double)angle) * (double)f7;  // dmul, dadd (double)
    double x2 = (double)origin.getX() - Math.sin((double)angle) * (double)f7;  // Math.sin called AGAIN (same value)
    double z1 = (double)origin.getZ() + Math.cos((double)angle) * (double)f7;
    double z2 = (double)origin.getZ() - Math.cos((double)angle) * (double)f7;  // Math.cos called twice too

    // (local slot17 = 2, only used as the "2" below — compiled as iconst_2)
    // DRAW 2, DRAW 3: random.nextInt(3)  (wrapper nextInt(bound), next(31) rejection per A3 seeding note)
    double y1 = (double)(origin.getY() + random.nextInt(3) - 2);   // int arithmetic, THEN i2d
    double y2 = (double)(origin.getY() + random.nextInt(3) - 2);

    int minX   = origin.getX() - Mth.ceil(f7) - i8;
    int minY   = origin.getY() - 2 - i8;
    int minZ   = origin.getZ() - Mth.ceil(f7) - i8;
    int width  = 2 * (Mth.ceil(f7) + i8);          // used for BOTH x and z extent
    int height = 2 * (2 + i8);

    for (int x = minX; x <= minX + width; x++) {            // inclusive upper bound
        for (int z = minZ; z <= minZ + width; z++) {        // inclusive upper bound
            if (minY <= level.getHeight(Heightmap.Types.OCEAN_FLOOR_WG, x, z)) {
                return doPlace(level, random, cfg, x1, x2, z1, z2, y1, y2,
                               minX, minY, minZ, width, height);
            }
        }
    }
    return false;   // no column high enough: doPlace never runs, only draws 1–3 burned
}
```

Slot evidence: doPlace descriptor `(WorldGenLevel,RandomSource,OreConfiguration,DDDDDD,IIIII)` with
argument order `d9,d11,d13,d15,d18,d20, l22,i23,j24,k25,l26` = `x1,x2,z1,z2,y1,y2, minX,minY,minZ,width,height`.

Key fp facts:

- `angle` is `nextFloat()*π_f` in **float**; the sin/cos calls are `java.lang.Math.sin/cos(double)`
  on the widened float. There is **no Mth table lookup** for the endpoint angle.
- `Math.sin` and `Math.cos` are each invoked **twice** (bytecode 75/93 and 111/129) with the same
  argument — value-identical, so C may compute once. JDK `Math.sin/cos` on Linux x64 is the
  fdlibm/StrictMath-compatible implementation for these (UNVERIFIED as to whether the C libm
  matches bit-exactly; hyperchunk must use a Java-fdlibm-equivalent `sin/cos` — same issue class
  as elsewhere in the port; recommend jdk fdlibm port).
- `Mth.ceil(float f)` bytecode: `(int) Math.ceil((double) f)` (f2d, Math.ceil, d2i).
- `Mth.floor(double d)` bytecode: `(int) Math.floor(d)`.
- Heightmap precheck: `WorldGenRegion.getHeight(type,x,z)` =
  `getChunk(x>>4,z>>4).getHeight(type, x&15, z&15) + 1`, and `ChunkAccess.getHeight` =
  `Heightmap.getFirstAvailable(x&15,z&15) - 1`. Net: **precheck compares against
  `firstAvailable` = (highest set block y) + 1** of OCEAN_FLOOR_WG. It also calls
  `warnIfReadOutsideWriteZone(chunkX,chunkZ)` first — log-only (`Util.logAndPauseIfInIde`), no
  behavioral effect in production.
- Scan order is x-major then z; the **first** qualifying column triggers doPlace and the loop
  returns immediately. If doPlace runs, its return value is place()'s return value.
- NEGATIVE: there is **no 0.15 constant anywhere in 26.2 OreFeature** (the old
  `if (nextFloat() < …)` era branch does not exist). Constants present: 3.1415927f, 8.0f, 16.0f,
  16.0d, 2.0d, -1.0d, 0.5d only.

## 2. `OreFeature.doPlace` — exact

Signature: `protected boolean doPlace(WorldGenLevel level, RandomSource random,
OreConfiguration cfg, double x1, double x2, double z1, double z2, double y1, double y2,
int minX, int minY, int minZ, int width, int height)`.

### 2.1 Sphere array fill (draws!)

```java
int placed = 0;                                    // slot21
BitSet visited = new BitSet(width * height * width);   // nbits = k25*l26*k25
BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
int size = cfg.size;                               // slot24
double[] shape = new double[size * 4];             // [i*4+0]=cx, +1=cy, +2=cz, +3=radius

for (int i = 0; i < size; i++) {
    float t = (float) i / (float) size;            // FLOAT division
    double cx = Mth.lerp((double) t, x1, x2);      // lerp(d,a,b) = a + d*(b-a), all double
    double cy = Mth.lerp((double) t, y1, y2);
    double cz = Mth.lerp((double) t, z1, z2);
    // DRAW (one per sphere, unconditionally, size draws total): random.nextDouble()
    double dr = random.nextDouble() * (double) size / 16.0;      // (nextDouble()*size)/16.0, double
    // radius: Mth.sin is the TABLE lookup here, signature Mth.sin(double)->float:
    //   SIN[(int)((long)(x * 10430.378350470453) & 65535)],  SIN[i]=(float)Math.sin(i/10430.378350470453)
    // argument is the FLOAT product (3.1415927f * t) widened to double:
    double radius = ((double)(Mth.sin((double)(3.1415927f * t)) + 1.0f) * dr + 1.0) / 2.0;
    //             ^ float add (sin+1.0f) BEFORE f2d; then double mul/add/div
    shape[i*4 + 0] = cx;
    shape[i*4 + 1] = cy;
    shape[i*4 + 2] = cz;
    shape[i*4 + 3] = radius;
}
```

Note lerp axis mapping (slots): cx from (x1,x2)=slots 4/6, cy from (y1,y2)=slots 12/14,
cz from (z1,z2)=slots 8/10 — i.e. the y lerp uses the two `nextInt(3)` endpoints.

Mth.sin(double) bytecode (26.2 changed the parameter to double; the table is unchanged):
```
SIN[ (int)( (long)(x * 10430.378350470453) & 65535L ) ]     // d2l TRUNCATION, not floor
SIN[i] = (float) Math.sin( (double) i / 10430.378350470453 )   // 10430.378... = 65536/(2π)
```
Mth.cos(double) = `SIN[(int)((long)(x*10430.378350470453 + 16384.0) & 65535L)]` (not used here).

### 2.2 Overlap pruning (no draws)

```java
for (int i = 0; i < size - 1; i++) {
    if (shape[i*4+3] <= 0.0) continue;             // dcmpg ifgt: skips NaN too (radius never NaN)
    for (int j = i + 1; j < size; j++) {
        if (shape[j*4+3] <= 0.0) continue;
        double dx = shape[i*4+0] - shape[j*4+0];
        double dy = shape[i*4+1] - shape[j*4+1];
        double dz = shape[i*4+2] - shape[j*4+2];
        double dr = shape[i*4+3] - shape[j*4+3];
        if (dr * dr > dx*dx + dy*dy + dz*dz) {     // strict >, sum order: (dx²+dy²)+dz²
            if (dr > 0.0) shape[j*4+3] = -1.0;     // keep the larger sphere
            else          shape[i*4+3] = -1.0;
        }
    }
}
```
(After setting `shape[i*4+3] = -1.0` the inner loop **continues** with the now-dead i — the
bytecode branch after the else-store goes to the j-increment, and the i-radius is not re-read
inside the j loop except via the freshly stored value? No: dr is recomputed each j iteration
from `shape[i*4+3]`, which is now -1.0, so subsequent j comparisons use -1.0. Exactly mirror
the array mutation.)

### 2.3 Placement sweep (BulkSectionAccess, try-with-resources)

```java
try (BulkSectionAccess bulk = new BulkSectionAccess(level)) {
    for (int i = 0; i < size; i++) {
        double r = shape[i*4+3];
        if (r < 0.0) continue;                     // dcmpg ifge: proceeds when r >= 0
        double cx = shape[i*4+0], cy = shape[i*4+1], cz = shape[i*4+2];
        int x0 = Math.max(Mth.floor(cx - r), minX);
        int y0 = Math.max(Mth.floor(cy - r), minY);
        int z0 = Math.max(Mth.floor(cz - r), minZ);
        int x9 = Math.max(Mth.floor(cx + r), x0);  // upper bounds clamped to the LOWER bound
        int y9 = Math.max(Mth.floor(cy + r), y0);
        int z9 = Math.max(Mth.floor(cz + r), z0);

        for (int x = x0; x <= x9; x++) {                       // x outer
            double ddx = ((double)x + 0.5 - cx) / r;           // int→double, +0.5, sub, div (all double)
            if (ddx * ddx >= 1.0) continue;                    // dcmpg ifge — proceed only if < 1
            for (int y = y0; y <= y9; y++) {                   // y middle
                double ddy = ((double)y + 0.5 - cy) / r;
                if (ddx*ddx + ddy*ddy >= 1.0) continue;        // ddx*ddx recomputed, then + ddy*ddy
                for (int z = z0; z <= z9; z++) {               // z inner
                    double ddz = ((double)z + 0.5 - cz) / r;
                    if (ddx*ddx + ddy*ddy + ddz*ddz >= 1.0) continue;  // (ddx²+ddy²)+ddz²
                    if (level.isOutsideBuildHeight(y)) continue;   // y < minBuildY || y > maxBuildY
                    int bit = (x - minX) + (y - minY) * width + (z - minZ) * width * height;
                    if (visited.get(bit)) continue;
                    visited.set(bit);                          // set BEFORE writability checks
                    pos.set(x, y, z);
                    if (!level.ensureCanWrite(pos)) continue;  // 3x3 window soft-fail (A4 note)
                    LevelChunkSection section = bulk.getSection(pos);
                    if (section == null) continue;             // y outside section range
                    int sx = x & 15, sy = y & 15, sz = z & 15; // SectionPos.sectionRelative = v & 15
                    BlockState cur = section.getBlockState(sx, sy, sz);
                    for (TargetBlockState target : cfg.targetStates) {
                        if (canPlaceOre(cur, bulk::getBlockState, random, cfg, target, pos)) {
                            section.setBlockState(sx, sy, sz, target.state, /*locked=*/false);
                            placed++;
                            break;                             // bytecode: goto z-increment
                        }
                        // else: continue to NEXT target (each call may draw — see §4)
                    }
                }
            }
        }
    }
}   // close(): release all acquired sections
return placed > 0;
```

BitSet notes: allocated with `width*height*width` bits; the clamped bounds guarantee
`0 <= bit < width*height*width` (algebra: floor(c±r) stays within [min, min+extent] given
i8 >= size/16 + 0.5 >= r_max and the ±2 y offsets; verified bounds arithmetic, not just trusted).
Java BitSet would auto-grow anyway; C can use a fixed bit array of that size. Bit layout is
x + y*width + z*width*height.

## 3. `canPlaceOre` / `shouldSkipAirCheck` / `isAdjacentToAir` — exact, incl. draw order

```java
public static boolean canPlaceOre(BlockState state, Function<BlockPos,BlockState> getter,
                                  RandomSource random, OreConfiguration cfg,
                                  TargetBlockState target, BlockPos.MutableBlockPos pos) {
    if (!target.target.test(state, random)) return false;      // (1) rule test — may draw (see §4)
    if (shouldSkipAirCheck(random, cfg.discardChanceOnAirExposure)) return true;  // (2) maybe draws
    return !isAdjacentToAir(getter, pos);                       // (3) no draws
}

protected static boolean shouldSkipAirCheck(RandomSource random, float chance) {
    if (chance <= 0.0f) return true;    // fcmpg ifgt — NO draw (all discard=0.0 ores)
    if (chance >= 1.0f) return false;   // fcmpl iflt — NO draw (discard=1.0: buried diamond/lapis, debris)
    return random.nextFloat() >= chance;    // DRAW: nextFloat; skip-check iff >= chance (fcmpl iflt)
}
```

So per candidate block, per target in list order, the draw sequence is:
`test()` draws (zero for tag/block/blockstate match) → one `nextFloat()` iff
`0 < discard < 1` and the test passed → neighbor reads (no draws). A block "fails" a target
(loop continues to the next target) when the test fails, OR when the air roll said
"do the check" (`nextFloat() < chance`, or `chance >= 1`) and a neighbor is air. Both the
discard roll and the neighbor reads therefore repeat for later targets.

`Feature.isAdjacentToAir(getter, pos)` = `checkNeighbors(getter, pos, BlockStateBase::isAir)`
(BootstrapMethods #3 resolves the predicate to `BlockBehaviour$BlockStateBase.isAir()Z` —
the cached `isAir` field of the state, i.e. exactly `state.isAir()`, no material logic):

```java
public static boolean checkNeighbors(Function<BlockPos,BlockState> getter, BlockPos pos,
                                     Predicate<BlockState> pred) {
    MutableBlockPos m = new MutableBlockPos();
    for (Direction d : Direction.values()) {       // DOWN, UP, NORTH, SOUTH, WEST, EAST (enum order, verified)
        m.setWithOffset(pos, d);
        if (pred.test(getter.apply(m))) return true;   // short-circuits on first air neighbor
    }
    return false;
}
```

The getter inside doPlace is `bulk::getBlockState` (BootstrapMethods verified:
`BulkSectionAccess.getBlockState`): section-cached read; **returns `Blocks.AIR.defaultBlockState()`
if the neighbor's section index is out of the world's section range** (above/below build limits
count as air-exposure). Neighbor reads go through `WorldGenRegion.getChunk` on first touch of a
chunk column (region cache; features step has the full region available, so ±1-block neighbors
of any writable pos are readable).

## 4. RuleTest subtypes — `test(BlockState, RandomSource)` exact

Dispatch: `RuleTest.CODEC` keyed on `"predicate_type"` (registry `rule_test`); registered names
(RuleTestType static init): `always_true`, `block_match`, `blockstate_match`, `tag_match`,
`random_block_match`, `random_blockstate_match`.

| type | test body (bytecode) | draws |
|---|---|---|
| `tag_match` | `state.is(tag)` | none |
| `block_match` | `state.is(block)` | none |
| `blockstate_match` | `state == this.blockState` (reference `if_acmpne` — state identity) | none |
| `random_block_match` | `state.is(block) && random.nextFloat() < probability` (fcmpg; nextFloat only if block matched) | 0 or 1 nextFloat |

`state.is(...)` resolves through `TypedInstance` defaults (BlockStateBase implements
`TypedInstance<Block>`):
- `is(TagKey)` = `typeHolder().is(tag)` — **block-level** tag membership via the block's Holder
  (26.2 moved `is` off BlockStateBase into TypedInstance; semantics: tag contains the *block*,
  properties irrelevant).
- `is(Block)` = `typeHolder().value() == block` (reference identity; compiled as
  `is(Ljava/lang/Object;)Z` due to erasure).

Overworld ores use only `tag_match` (plus `block_match` for nether `ore_magma`); no
random_* rule appears in any shipped ore JSON (checked all `ore_*.json`).

Codec JSON names: `TargetBlockState` = `{ "target": <RuleTest>, "state": <BlockState> }`
(fields `target`, `state`, constants verified in `$TargetBlockState` constant pool).
`OreConfiguration` codec fields: `"targets"` (list), `"size"` (`intRange(0, 64)`),
`"discard_chance_on_air_exposure"` (`floatRange(0.0f, 1.0f)`); all three `fieldOf` (required,
no defaults). Java fields: `List<TargetBlockState> targetStates; int size; float discardChanceOnAirExposure;`.

## 5. The write path — `BulkSectionAccess` + `LevelChunkSection.setBlockState(...,false)`

`BulkSectionAccess` (full class, 4 fields: `level`, `Long2ObjectOpenHashMap acquiredSections`,
`lastSection`, `lastSectionKey`):

```java
LevelChunkSection getSection(BlockPos pos) {
    int idx = level.getSectionIndex(pos.getY());            // sectionYFromBlockY - minSectionY
    if (idx < 0 || idx >= level.getSectionsCount()) return null;
    long key = SectionPos.asLong(pos);                      // section coords packed
    if (lastSection == null || lastSectionKey != key) {
        lastSection = acquiredSections.computeIfAbsent(key, k -> {
            ChunkAccess chunk = level.getChunk(SectionPos.blockToSectionCoord(pos.getX()),
                                               SectionPos.blockToSectionCoord(pos.getZ()));
            LevelChunkSection s = chunk.getSection(idx);
            s.acquire();                                    // PalettedContainer.acquire (thread-guard only)
            return s;
        });
        lastSectionKey = key;
    }
    return lastSection;
}
BlockState getBlockState(BlockPos pos) {
    LevelChunkSection s = getSection(pos);
    if (s == null) return Blocks.AIR.defaultBlockState();
    return s.getBlockState(pos.getX()&15, pos.getY()&15, pos.getZ()&15);
}
void close() { for (LevelChunkSection s : acquiredSections.values()) s.release(); }
```

`LevelChunkSection.setBlockState(x,y,z,state,false)`:
`states.getAndSetUnchecked(...)` (vs `getAndSet` when locked=true — the flag only selects the
PalettedContainer lock-assert path, identical data effect), then updates the section's
`nonEmptyBlockCount` / `tickingBlockCount` / `fluidCount` shorts from old→new state. Nothing else.

Answers to the decisive questions:

- **(a) Heightmaps are NOT updated by OreFeature writes.** The write goes
  `BulkSectionAccess.getSection → LevelChunkSection.setBlockState(sx,sy,sz,state,false)`,
  bypassing `WorldGenRegion.setBlock`/`ProtoChunk.setBlockState` entirely — no heightmap
  `update()`, no post-processing marks, no block-entity/tick logic. (Contrast A4 §: the
  ProtoChunk path updates the 4 FINAL heightmaps during features.) The C ore writer must
  write raw section data and leave heightmaps untouched. BUT place() *reads* OCEAN_FLOOR_WG
  (frozen WG map) for the precondition; and section `nonEmptyBlockCount` bookkeeping is
  irrelevant for parity of block data.
- **(b) `ensureCanWrite` IS consulted** — once per candidate block position (bytecode offset
  766), after the BitSet visit-mark, before section access. Standard 3x3 window soft-fail
  (A4 note §"ensureCanWrite"): out-of-window or out-of-region positions are silently skipped
  (and remain marked visited in the BitSet).
- **(c) Cross-chunk writes: yes.** Chunk resolution is
  `level.getChunk(blockX>>4, blockZ>>4)` (the LevelAccessor interface method, dispatching to
  `WorldGenRegion.getChunk` → StaticCache2D holder lookup). Any position that passes
  `ensureCanWrite` (within writeRadius of the center chunk) resolves fine. Sections are
  cached per `SectionPos.asLong` key with a 1-entry hot cache in front of the hash map;
  acquire/release is only a leak/thread guard (PalettedContainer), no data semantics.

## 6. `ScatteredOreFeature.place` — exact (uses the OTHER write path)

`MAX_DIST_FROM_ORIGIN = 7` (constant). Registered as `"scattered_ore"`.

```java
public boolean place(FeaturePlaceContext<OreConfiguration> ctx) {
    WorldGenLevel level = ctx.level();
    RandomSource random = ctx.random();
    OreConfiguration cfg = ctx.config();
    BlockPos origin = ctx.origin();

    // DRAW 1: nextInt(size + 1)  → count ∈ [0, size]
    int count = random.nextInt(cfg.size + 1);
    MutableBlockPos pos = new MutableBlockPos();
    for (int i = 0; i < count; i++) {
        // offsetTargetPos(pos, random, origin, Math.min(i, 7)):
        //   DRAWS: 3 axes in order x, y, z; each axis = nextFloat(), nextFloat():
        //   off = Math.round((random.nextFloat() - random.nextFloat()) * (float)dist);
        //   (fsub then fmul in float; Math.round(F)I = floor(x+0.5f) semantics)
        //   pos.setWithOffset(origin, offX, offY, offZ);
        BlockState cur = level.getBlockState(pos);            // WorldGenLevel read (not bulk)
        for (TargetBlockState target : cfg.targetStates) {
            if (OreFeature.canPlaceOre(cur, level::getBlockState, random, cfg, target, pos)) {
                level.setBlock(pos, target.state, 2);          // flag 2 — FULL setBlock path!
                break;
            }
        }
    }
    return true;                                               // ALWAYS true, even count==0
}
```

Draws per iteration: 6 nextFloat (2 per axis) + canPlaceOre draws per §3/§4. With
`dist = min(i, 7)`: iteration 0 still burns all 6 nextFloats (offset forced to round(±0*0)=0).
Note `i` caps at 7 blocks from origin. Getter for the air check is
`level::getBlockState` (`BlockGetter.getBlockState` interface ref, BootstrapMethods verified),
i.e. WorldGenRegion reads (which throw/log outside the region rather than returning AIR —
different out-of-world behavior than BulkSectionAccess, but scattered positions are ≤7+1
blocks from origin, inside the window). **Writes use `WorldGenLevel.setBlock(pos, state, 2)`
= WorldGenRegion.setBlock → ensureCanWrite → ProtoChunk.setBlockState → updates the 4 FINAL
heightmaps** (A4 note). So the two ore feature types differ in heightmap side effects.
Both shipped scattered_ore configs (ancient debris) have discard=1.0 → no discard-roll draws,
air check always performed.

## 7. Census — feature type / size / discard / targets (from shipped JSON)

Tags (data/minecraft/tags/block/): `stone_ore_replaceables` = {stone, granite, diorite,
andesite}; `deepslate_ore_replaceables` = {deepslate, tuff}; `base_stone_overworld` =
{stone, granite, diorite, andesite, tuff, deepslate}.

`T(tag→state)` abbreviates one tag_match target. `S` = stone_ore_replaceables,
`D` = deepslate_ore_replaceables, `B` = base_stone_overworld.

| configured_feature | type | size | discard | targets |
|---|---|---|---|---|
| ore_dirt | ore | 33 | 0.0 | T(B→dirt) |
| ore_gravel | ore | 33 | 0.0 | T(B→gravel) |
| ore_granite | ore | 64 | 0.0 | T(B→granite) |
| ore_diorite | ore | 64 | 0.0 | T(B→diorite) |
| ore_andesite | ore | 64 | 0.0 | T(B→andesite) |
| ore_tuff | ore | 64 | 0.0 | T(B→tuff) |
| ore_coal / ore_coal_buried | ore | 17 | 0.0 / 0.5 | T(S→coal_ore), T(D→deepslate_coal_ore) |
| ore_iron / ore_iron_small | ore | 9 / 4 | 0.0 | T(S→iron_ore), T(D→deepslate_iron_ore) |
| ore_gold / ore_gold_buried | ore | 9 | 0.0 / 0.5 | T(S→gold_ore), T(D→deepslate_gold_ore) |
| ore_redstone | ore | 8 | 0.0 | T(S→redstone_ore), T(D→deepslate_redstone_ore) |
| ore_diamond_small/medium/large/buried | ore | 4/8/12/8 | 0.5/0.5/0.7/1.0 | T(S→diamond_ore), T(D→deepslate_diamond_ore) |
| ore_lapis / ore_lapis_buried | ore | 7 | 0.0 / 1.0 | T(S→lapis_ore), T(D→deepslate_lapis_ore) |
| ore_copper_small / ore_copper_large | ore | 10 / 20 | 0.0 | T(S→copper_ore), T(D→deepslate_copper_ore) |
| ore_clay | ore | 33 | 0.0 | T(B→clay) |
| ore_emerald | ore | 3 | 0.0 | T(S→emerald_ore), T(D→deepslate_emerald_ore) |
| ore_infested | ore | 9 | 0.0 | T(S→infested_stone), T(D→infested_deepslate) |
| ore_ancient_debris_small / _large | **scattered_ore** | 2 / 3 | 1.0 | (nether) |
| ore_magma, ore_blackstone, ore_gravel_nether, ore_nether_gold, ore_quartz, ore_soul_sand | ore | 33/33/33/10/14/12 | 0.0 | (nether; ore_magma uses block_match netherrack) |

Not OreFeature (census only, other agents): `underwater_magma` → type
`minecraft:underwater_magma`; `disk_clay`/`disk_sand`/`disk_gravel`/`disk_grass` → type
`minecraft:disk`.

Plains biome step 6 (UNDERGROUND_ORES) placed-feature order (biome/plains.json features[6]):
ore_dirt, ore_gravel, ore_granite_upper, ore_granite_lower, ore_diorite_upper,
ore_diorite_lower, ore_andesite_upper, ore_andesite_lower, ore_tuff, ore_coal_upper,
ore_coal_lower, ore_iron_upper, ore_iron_middle, ore_iron_small, ore_gold, ore_gold_lower,
ore_redstone, ore_redstone_lower, ore_diamond, ore_diamond_medium, ore_diamond_large,
ore_diamond_buried, ore_lapis, ore_lapis_buried, ore_copper, underwater_magma, disk_sand,
disk_clay, disk_gravel. All `ore_*` placed features in that list resolve to `minecraft:ore`
configured features (none scattered in the overworld; scattered_ore is nether-only:
ancient debris). `ore_iron_small` is plain `minecraft:ore` size 4 — NOT scattered
(contradicts 1.18-era memory hypotheses; verified from JSON).

## 8. Negative findings

- **No biome consultation** anywhere in OreFeature/ScatteredOreFeature (no getBiome calls).
- **No tick scheduling**, no `markAboveForPostProcessing`, no `markPosForPostProcessing`.
- **No fork()/forkPositional()** — all RNG through the passed-in `random` wrapper methods
  (`nextFloat`, `nextInt(int)`, `nextDouble` only).
- **No 0.15 (or any similar) magic constant** in 26.2 OreFeature.
- The angle sin/cos are **JDK `Math.sin/cos`**, not the Mth table; the per-sphere radius sine
  IS the Mth table (`Mth.sin(double)`, 26.2 signature takes double, same 65536-entry float table,
  d2l-truncating index).
- OreFeature reads heightmaps (OCEAN_FLOOR_WG precheck) but never writes them; heightmap
  updates happen only via ScatteredOreFeature's `setBlock(…, 2)` path.
- `blockstate_match` compares by **reference identity** (`==`), not equals().
- `ScatteredOreFeature.place` always returns `true` (affects `placed_feature` return
  bookkeeping in the decoration walk, if anything downstream cares).

## 9. Implications for the C implementation

1. **RNG replay for `minecraft:ore`:** exactly `1×nextFloat + 2×nextInt(3)` in place(), then
   (only if the heightmap precheck passes) `size × nextDouble` in the fill loop, then per
   candidate block per target: `1×nextFloat` iff `0 < discard < 1` and the rule test passed.
   discard 0.0 and 1.0 burn no per-block draws. If the precheck fails, the feature still
   burned the first three draws — the shared feature-random position must reflect that.
2. **Rule tests are pure block-identity/tag checks** — precompute per-block-ID bitmasks for
   the three tags; no per-state data needed (tag membership is block-level). blockstate_match
   (unused by ores) would need state-identity, i.e. full state key compare in C.
3. **Write path:** write directly into section storage (our chunk block array), **do not touch
   heightmaps**, do not mark post-processing. Do apply the ensureCanWrite 3x3/writeRadius
   window test per position (A4), *after* recording the visited bit. Section-null (y out of
   world) → skip. Air-exposure neighbor reads must treat out-of-world sections as AIR.
4. **FP widths to match:** angle math float→double as annotated; `t = (float)i/(float)size`;
   radius formula mixes a float table-sin + float `+1.0f` before widening; everything in the
   sweep (`+0.5`, subtract, divide, squares) is double. `Math.round(F)` in scattered =
   `floor(f + 0.5f)` on the float. Use fdlibm-compatible sin/cos for the two JDK calls
   (flagged: verify C libm bit-match on the golden gate; this is the one UNVERIFIED fp hazard).
5. **Iteration order** is load-bearing everywhere: sphere index order for draws; x→y→z sweep;
   target-list order; Direction order DOWN,UP,NORTH,SOUTH,WEST,EAST with short-circuit.
6. **ScatteredOreFeature** (nether only) uses the heightmap-updating setBlock path — if/when
   the nether is in scope, its writer must route through the ProtoChunk-equivalent update of
   the 4 FINAL heightmaps, unlike the ore bulk writer.
7. The `getHeight` precheck must return `firstAvailable` (highest set + 1) from the frozen
   OCEAN_FLOOR_WG map of the *neighbor* chunk containing (x,z) — cross-chunk reads happen at
   region edges.

## Adversarial verification (independent re-derivation, 2026-07-31)

Independently re-disassembled from 26.2 bytecode: `OreFeature` (place/doPlace/canPlaceOre/
shouldSkipAirCheck + constant pool + BootstrapMethods), `ScatteredOreFeature` (all 3 methods +
BootstrapMethods), `Feature` (checkNeighbors/isAdjacentToAir/BootstrapMethod #3, static-init
registration of `"ore"`→OreFeature / `"scattered_ore"`→ScatteredOreFeature), `Mth`
(ceil(F)/floor(D)/lerp(DDD)/sin(D)/cos(D) + SIN-table lambda), `Direction` static init,
`SectionPos.sectionRelative`, `BulkSectionAccess` (whole class), `LevelChunkSection.setBlockState`,
`WorldGenRegion.getHeight`/`warnIfReadOutsideWriteZone`, `ChunkAccess.getHeight`,
`Heightmap.getFirstAvailable`, `LevelHeightAccessor.isOutsideBuildHeight`, `TypedInstance.is(*)`,
`BlockBehaviour$BlockStateBase` (typeHolder, no plain `is(TagKey)` override), all five RuleTest
subtypes, `RuleTestType` registration strings, `OreConfiguration` codec strings,
`MutableBlockPos.setWithOffset(Vec3i,III)`; plus re-extraction of the full `ore_*.json` census,
the three tag files, and `biome/plains.json features[6]`.

**Verdict: the RNG draw sequences, all FP widths, all draw-gating world reads, and the write-path
claims (sections-not-setBlock, no heightmap update, ensureCanWrite after BitSet.set, cross-chunk
via getChunk) are CONFIRMED exactly as written**, including every bytecode offset spot-checked
(nextFloat@26, nextInt(3)@148/165, ensureCanWrite@766-774, sin/cos double-calls@75/93/111/129,
float `(sin+1.0f)` before f2d@123-125, dcmpg/dcmpl comparison senses, target-loop break→z-increment).
The constant pool contains exactly {3.1415927f, 8.0f, 16.0f, 16.0d, 2.0d, -1.0d, 0.5d} — the
"no 0.15" negative finding stands. Corrections and caveats:

1. **REFUTED (census annotation, §4/§7):** "`block_match` for nether `ore_magma`" understates it —
   ALL SIX netherrack-hosted nether ores use `block_match` on `minecraft:netherrack`:
   `ore_magma`, `ore_blackstone`, `ore_gravel_nether`, `ore_nether_gold`, `ore_quartz`,
   `ore_soul_sand` (re-verified from the shipped JSON). Sizes/discards in the §7 row are correct;
   `tag_match`-only holds for the OVERWORLD ores only. (No `random_*` rule anywhere — confirmed.)
2. **REFUTED (trivial, §5):** `LevelChunkSection.setBlockState` updates FOUR counters, not three:
   `nonEmptyBlockCount`, `tickingBlockCount`, `fluidCount`, **and `tickingFluidCount`** (bytecode
   offsets 114-122 / 197-205). "Nothing else" is otherwise correct. Immaterial for block-data parity.
3. **Caveat (§2.3 BitSet bounds):** the bounds algebra holds in *real* arithmetic (all margins
   strict: `r < dr + 0.5 < size/16 + 0.5 <= i8`, so `floor(c±r)` stays within `[min, min+extent-1]`
   ... except y where margin is a full 2), but at large |coords| (~10^7) `ulp(cx±r)` (~2^-27) can
   exceed the worst-case real margin (~2^-51 when `nextDouble()→1`, `sin(angle)→1`, `size≡8 mod 16`),
   so `floor(cx+r) == minX+width` is not provably impossible under double rounding. Java is immune
   (BitSet auto-grows; layout stays injective). **C must not hard-assert `bit < width*height*width`**:
   allocate `(width+1)*(height+1)*(width+1)` bits (same strides `width`, `width*height`) or
   bounds-check-and-grow. Y indices provably stay ≤ height-2, x/z ≤ width.
4. **Caveat (§6 Math.round):** JDK `Math.round(float)` is NOT compiled as `floor(x+0.5f)` with a
   float add — since JDK-8010430 it computes `floor(a + 0.5)` on the EXACT value via bit-twiddling
   (no double-rounding). Verified by enumeration that for the reachable inputs here
   (`a = fl((k·2^-24)·d)`, `d ∈ 0..7`; the fsub of two nextFloats is exact) no divergent value
   (e.g. `0.49999997f`, `n±0.5∓ulp` forms) is representable/reachable, so `floorf(a+0.5f)` happens
   to agree at THIS call site — but the C port should implement the exact semantics anyway
   (`(int)floor((double)a + 0.5)` is bit-identical to the JDK for all floats |a|<2^30).
5. Minor confirmations beyond the note's citations: `Mth.SIN` init is `(float)Math.sin((double)i
   / 10430.378350470453)` — a *division*, exactly as written (not the 1.21-era `i * (2π/65536)`
   multiply form; do not substitute a multiply in C). `setWithOffset(Vec3i,III)` adds (dx,dy,dz)
   in x,y,z order, so the scattered-ore 6-draw axis order x,y,z is confirmed at the Vec3i level too.
   `BlockStateBase` has no plain `is(TagKey)` override (only `is(TagKey,Predicate)`), so the
   TypedInstance default (block-holder-level tag membership) is indeed what RuleTests hit.

refuted: 2 (both immaterial to RNG/block parity); everything load-bearing for the C replay: CONFIRMED.
