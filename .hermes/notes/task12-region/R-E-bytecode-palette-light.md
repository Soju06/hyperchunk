# R-E — bytecode: PalettedContainer save, DataLayer/light emission, Heightmap, biomes (Task 12)

Source of truth: `javap -p -c -constants -cp
tools/golden/libs/extracted/server-26.2.jar <fqcn>`; DFU internals from
`libraries/com/mojang/datafixerupper/10.0.21/datafixerupper-10.0.21.jar`.
Scratch disassemblies under `/tmp/t12e/*.javap` (regenerate with the line
above). Everything below is bytecode-verified unless marked UNVERIFIED.

26.2 structure note: `PalettedContainer$Strategy` is gone. Top-level
`net.minecraft.world.level.chunk.Strategy` (abstract, 2 anon subclasses) +
top-level `Configuration` interface (`Configuration$Simple`,
`Configuration$Global` records) + record `PalettedContainerFactory`
(per-`ServerLevel`, `ServerLevel.palettedContainerFactory()`).

## 1. Save path is CODEC-based (not manual NBT)

`SerializableChunkData.write()` @218-244: for each `SectionData` with
`chunkSection != null`:
- @218-229 `sectionTag.store("block_states", containerFactory.blockStatesContainerCodec(), section.getStates())`
- @232-244 `sectionTag.store("biomes", containerFactory.biomeContainerCodec(), section.getBiomes())`

`CompoundTag.store(String,Codec,T)` @0-10 → overload @0-23:
`codec.encodeStart(NbtOps.INSTANCE, value).getOrThrow()` then `put(key, tag)`.

Codecs (`PalettedContainerFactory.create` @0-80):
- block states: `PalettedContainer.codecRW(BlockState.CODEC, blockStatesStrategy, AIR.defaultBlockState())`
- biomes: `PalettedContainer.codecRO(biomeRegistry.holderByNameCodec(), biomeStrategy, PLAINS holder)`

`PalettedContainer.codec` @0-28: `RecordCodecBuilder.create(...)`
`.comapFlatMap(unpack, pack)`. Encode side = `lambda$codec$2` @0-7:
`container.pack(strategy)` → `PackedData`, then record encode.
Record fields (`lambda$codec$0` @0-63):
1. `elementCodec.mapResult(orElsePartial(defaultValue)).listOf().fieldOf("palette")`
2. `Codec.LONG_STREAM.lenientOptionalFieldOf("data")`

Encode insertion order **"palette" then "data"** — confirmed via DFU
`RecordCodecBuilder$Instance$4.encode` @0-51: fEncoder (unit fn, adds
nothing), then aEncoder (field 1), then bEncoder (field 2), each appending
into the same `RecordBuilder` (NbtOps record builder does
`CompoundTag.put` per add). `data` key entirely ABSENT when
`PackedData.storage` is `Optional.empty()` (lenientOptionalFieldOf).
`PackedData.bitsPerEntry` (record field 3) is NOT serialized (disk ctor
uses -1 = `UNKNOWN_BITS_PER_ENTRY` on read).

## 2. pack() — REPACK CONFIRMED (fresh palette, first-occurrence scan)

`PalettedContainer.pack(Strategy)` @0-135 (under acquire/release):
- @20-33: `newPalette = new HashMapPalette(storage.getBits())` (fresh, empty;
  backing `CrudeIncrementalIntIdentityHashBiMap` with capacity `1<<bits`,
  ids assigned incrementally by `add`, HashMapPalette @22-31 / CrudeMap
  `add` = `addMapping(k, nextId++)`).
- @41-48: `is = reencodeContents(storage, oldPalette, newPalette)`.
- @50-59: `config = strategy.getConfigurationForPaletteSize(newPalette.getSize())`;
  @61-68 `bits = config.bitsInStorage()`.
- @70-101: if `bits != 0`: `new SimpleBitStorage(bits, entryCount, is)` and
  `Optional.of(Arrays.stream(storage.getRaw()))`; else @106-109
  `Optional.empty()` — **data omitted iff bitsInStorage == 0 iff palette
  size == 1** (blocks and biomes both map bits 0 → ZERO_BITS).
- @111-127: `new PackedData(newPalette.getEntries(), optLongs, bits)`.

The repack loop — `reencodeContents(BitStorage, Palette old, Palette new)`
@0-85:
```
is = new int[storage.getSize()];        // @0-8
storage.unpack(is);                     // @9-11 (index order 0..size-1)
resize = PaletteResize.noResizeExpected(); // @16
lastOld = -1; lastNew = -1;             // @21-25
for (i = 0; i < is.length; i++) {       // @27-81
    v = is[i];                          // @37-41
    if (v != lastOld) {                 // @43-47 (consecutive-run cache only)
        lastOld = v;                    // @50-52
        lastNew = new.idFor(old.valueFor(v), resize); // @54-70
    }
    is[i] = lastNew;                    // @72-77
}
```
`HashMapPalette.idFor` @0-48: `getId` else `add` (append). ⇒ **serialized
palette order = first-occurrence order of the linear index scan
i = 0..entryCount-1** (the run cache re-calls idFor on non-consecutive
repeats; getId returns the existing id, so order is unaffected). Unused
old-palette entries are never queried → **never appear**.
`HashMapPalette.getEntries` @0-32: ArrayList from CrudeMap.iterator =
`Iterators.forArray(byId)` filtered non-null (@408-415 in CrudeMap javap)
= **id order**.

Index for blocks: `Strategy.getIndex(x,y,z)` @0-15 = `((y<<bits)|z)<<bits|x`
with `bitsPerAxis`=4 → `y<<8|z<<4|x`, entryCount = `1<<12` = 4096
(`Strategy.<init>` @39-45). Biomes: bitsPerAxis=2 → `y<<4|z<<2|x`,
entryCount 64.

### Strategy bits tables (Strategy$1 = block states, Strategy$2 = biomes)

`Strategy.getConfigurationForPaletteSize(size)` @0-10 =
`getConfigurationForBitCount(Mth.ceillog2(size))`. `Mth.ceillog2` @711:
deBruijn log2 of `smallestEncompassingPowerOfTwo(n)`; ceillog2(1)=0.

`Strategy$1.getConfigurationForBitCount` @1-100 (tableswitch 0..8):
0→ZERO_BITS(SingleValue, bits 0); 1,2,3,4→FOUR_BITS_LINEAR;
5→FIVE_BITS_HASHMAP; 6/7/8→SIX/SEVEN/EIGHT_BITS_HASHMAP;
default→`new Configuration$Global(globalPaletteBitsInMemory, requestedBits)`.
For `Configuration$Simple` bitsInMemory==bitsInStorage==bits,
alwaysRepack=false; `Global` alwaysRepack=true (read path only),
bitsInStorage = the requested (=ceillog2) value.
⇒ **block-state serialized bits = size==1 ? 0 : max(4, ceillog2(size))**;
>8 bits would use raw ceillog2 (global palette on read — with a "palette"
list still written; palette list is always present).

`Strategy$2.getConfigurationForBitCount` @1-68 (tableswitch 0..3):
0→ZERO_BITS; 1→ONE_BIT_LINEAR; 2→TWO_BITS_LINEAR; 3→THREE_BITS_LINEAR;
default→`Global(globalBits, requested)`.
⇒ **biome serialized bits = ceillog2(size) exactly, NO min clamp** (0 for
single-biome sections → no `data`).

## 3. SimpleBitStorage packing (LSB-first, no spanning)

`SimpleBitStorage.<init>(int bits,int size,long[])` @0-146:
`Validate.inclusiveBetween(1,32,bits)`; `mask=(1L<<bits)-1` @23-29;
`valuesPerLong=(char)(64/bits)` @32-38 (integer division — 9 bits → 7);
`expectedLen=(size+valuesPerLong-1)/valuesPerLong` @87-100, mismatch →
InitializationException @113-129.

`.set(index,value)` @0-83: `cell=cellIndex(index)` (magic-number division
by valuesPerLong, MAGIC table); `shift=(index - cell*valuesPerLong)*bits`
@37-50; `data[cell] = data[cell] & ~(mask<<shift) | ((value&mask)<<shift)`
@52-82. `get` @26-50: `(data[cell] >>> shift) & mask`. ⇒ value k of a long
occupies bits `[k*bits, (k+1)*bits)` — **first value in the lowest bits,
values never span longs, tail bits of each long unused (zero)**.

`<init>(int bits,int size,int[] values)` @0-113 (the pack ctor): per long,
`datum` built j = valuesPerLong-1 … 0 as `datum = (datum<<bits) |
(values[i+j] & mask)` @34-66 — same LSB-first layout; partial final long
@93-157 identical loop over the remaining `size - i` values.

`ZeroBitStorage`: getBits()=0 @78-81, getRaw()=static empty long[0] @67-70,
get=0, set validates value==0, unpack fills 0.

## 4. Palette entry NBT — block states

Two encoders exist; the **section palette uses `BlockState.CODEC`**, not
NbtUtils.

(a) `BlockState.CODEC` = `StateHolder.codec(BLOCK byNameCodec, ...)` @0-20:
`Codec.dispatch("Name", state→owner, block→ lambda$codec$1)`.
`lambda$codec$1` @0-65: singleton state → `MapCodec.unit` (⇒ compound is
`{Name}` only); else `stateDefinition.propertiesCodec().codec()
.lenientOptionalFieldOf("Properties").xmap(...)`.
DFU `KeyDispatchCodec.encode` adds the `"Name"` key to the record builder
BEFORE running the element encoder (prefix.add(typeKey,…) then
encoder.encode) ⇒ **insertion order: `Name`, then `Properties`**.
`Name` value = `BuiltInRegistries.BLOCK.getKey(block).toString()`
("minecraft:stone").

Properties compound (codec path): `StateDefinition.createCodec` @0-82
iterates `propertiesByName.entrySet()` and folds
`appendPropertyCodec` @320-341:
`Codec.mapPair(accum, property.valueCodec().fieldOf(name).orElseGet(...))`.
`propertiesByName = ImmutableSortedMap.copyOf(map)` (`StateDefinition.<init>`
@55-61) — **properties sorted ascending by name** (Java natural String
order). DFU `PairMapCodec.encode` @33-48 (datafixerupper-10.0.21):
`first.encode(a, ops, second.encode(b, ops, prefix))` — the SECOND
(later-appended) codec writes first. Folding is left-nested, so the
recursion bottoms out at the last-appended property ⇒ **codec-path
insertion order into the Properties CompoundTag is DESCENDING property
name**; the accumulated unit MapCodec adds nothing. (Iteration order on
disk is the CompoundTag HashMap-emulation's business; insertion order
matters only for same-bucket chains/treeify — use descending-name
insertion for palette entries.)

(b) `NbtUtils.writeBlockState` @307: `putString("Name",
BLOCK.getKey(block).toString())` @8-26, then `writeStateProperties` @286:
if `!isSingletonState()` build compound via `getValues()` stream forEach →
`putString(property.getName(), value.valueName())` (`lambda$writeState
Properties$0` @0-15), then `put("Properties", tag)` @30-34.
`StateHolder.getValues()` @327: `IntStream.range(0, propertyKeys.length)`
over the state's `propertyKeys`/`propertyValues` parallel arrays
(`lambda$getValues$0` @0-15) — array built from the sorted map
(`createMultiPropertyStates` … `Collection.toArray`) ⇒ **ASCENDING name
insertion** in this path. Note the asymmetry vs (a); NbtUtils path is used
by block entities/structures, not by section palettes.

Value stringification (`Property$Value.valueName()` @46-53 =
`property.getName(value)`):
- `BooleanProperty.getName` = `Boolean.toString` ("true"/"false")
- `IntegerProperty.getName` = `Integer.toString`
- `EnumProperty.getName` = `StringRepresentable.getSerializedName()`
`isSingletonState()` @316-326 = `propertyKeys.length == 0` ⇒ Properties
omitted entirely for property-less blocks.

## 5. Biome palette entry

`Registry.holderByNameCodec()` @15-23: `referenceHolderWithLifecycle()`
(built on `Identifier.CODEC`) `.flatComapMap(...)` ⇒ each biome palette
entry is a **StringTag** `holder.key().location().toString()`
("minecraft:plains"). Single-entry sections: palette=[one string], no
`data` (bits 0, §2). Bits for n>1: ceillog2(n) (§2, Strategy$2).

## 6. DataLayer

Fields: `protected byte[] data` (nullable), `private int defaultValue`.
Ctors: `()` → `(0)`; `(int defaultValue)` data=null @22-29;
`(byte[])` @31-54 keeps the array (no copy), requires length==2048.
- `getIndex(x,y,z)` @77-88 = `y<<8 | z<<4 | x`.
- `get(index)` @90-114: data==null → defaultValue; else
  `(data[index>>1] >> (4*(index&1))) & 15` — **byteIndex = index>>1,
  nibbleIndex = index&1, EVEN index = LOW nibble**.
- `set(index,v)` @116-154: `getData()` first (inflates), then
  `b = b & ~(15<<(4*nib)) | ((v&15)<<(4*nib))`.
- `fill(int v)` @170-178: `defaultValue = v; data = null` (dematerializes!).
- `getData()` @202-222: if data==null, allocate 2048 and, if
  defaultValue!=0, `Arrays.fill(packFilled(defaultValue))`;
  `packFilled` @180-200 duplicates the nibble into both halves
  (15 → 0xFF). Returns the internal array (no copy).
- `copy()` @224-242: data==null → `new DataLayer(defaultValue)`
  (preserves default, stays unmaterialized); else clone of data.
- `isEmpty()` @343-354: **`data == null && defaultValue == 0`**. A
  materialized all-zero array is NOT empty; an unmaterialized default-15
  layer is NOT empty either.

## 7. THE LIGHT EMISSION RULE

### 7a. Save-path plumbing

`SerializableChunkData.copyOf(ServerLevel, ChunkAccess)` section loop
@52-249: `for (i = lightEngine.getMinLightSection(); i <
getMaxLightSection(); i++)`. `LevelLightEngine` @367-384:
minLightSection = minSectionY-1; maxLightSection = min + sectionCount+2.
For -64..320 world (block sections -4..19): light sections **-5..20**
(loop bound 21 exclusive).
- @69-95: `inBlocksRange = 0 <= getSectionIndexFromSectionY(i) < sections.length`.
- @97-137: `blockLayer = getLayerListener(BLOCK).getDataLayerData(SectionPos.of(pos,i))`,
  same for SKY. `LevelLightEngine.getLayerListener` @184: returns the
  Block/Sky `LightEngine` itself. `LightEngine.getDataLayerData(SectionPos)`
  @513-521 → `storage.getDataLayerData(sectionPos.asLong())`.
- @139-185: keep `layer.copy()` only if `layer != null && !layer.isEmpty()`,
  else null (both layers independently).
- @187-202: **skip section entirely iff !inBlocksRange && both copies null**.
- @205-245: `SectionData(y=i, chunkSection = inBlocksRange ?
  sections[idx].copy() : null, blockLight, skyLight)`.

`write()` per SectionData @197-324 — compound insertion order:
`block_states` (if chunkSection!=null), `biomes` (ditto), `BlockLight`
(`putByteArray(name, layer.getData())` @255-268 — materializes
defaultValue-only layers into 2048 bytes here), `SkyLight` @279-292, then
if compound non-empty `putByte("Y", (byte)y)` @303-314 and append to the
`sections` ListTag (ascending y, loop order). ⇒ **all 24 block-range
sections always emit a compound** (block_states+biomes always present —
`LevelChunkSection.getStates/getBiomes` never null), Y=-5/Y=20 only when
they carry a non-empty light layer.

`LayerLightSectionStorage.getDataLayerData(long)` @152-168:
`queuedSections.get(pos)` first (only populated by disk-load
`queueSectionData`; empty for freshly generated chunks — drained by
`markNewInconsistencies` @195-299 into `updatingSectionData` and removed),
else `getDataLayer(pos, false)` = `visibleSectionData.getLayer(pos)`.
`swapSectionMap` @666-709 publishes updating→visible at the end of every
`runLightUpdates`; at save they are content-identical.

### 7b. When does a section HAVE a layer in the storage map?

`sectionStates: Long2ByteMap` (default 0). Byte layout
(`SectionState`): bit 5 (32) = HAS_DATA, bits 0-4 = neighborCount (0..26).

`updateSectionStatus(long pos, boolean isEmpty)` @521-598:
`newState = SectionState.hasData(cur, !isEmpty)`; if changed:
`putSectionState(pos, newState)` @36-40, then for all 26 neighbors
(dx,dz,dy ∈ {-1,0,1} minus origin) @54-161:
`putSectionState(n, neighborCount(curN, neighborCount(curN) + (isEmpty?-1:+1)))`.
`putSectionState` @600-622: state!=0 → map.put; **if previous value was 0 →
`initializeSection(pos)`**; state==0 → map.remove → `removeSection`
(queues layer removal from the map via toRemove/`markNewInconsistencies`
@60-67 `updatingSectionData.removeLayer`).

`initializeSection(long)` @624-652: unless cancelling a pending removal,
`updatingSectionData.setLayer(pos, createDataLayer(pos))`, changedSections
add, `onNodeAdded(pos)`, hasInconsistencies=true.

Registration source: `ThreadedLevelLightEngine.initializeLight` →
`lambda$initializeLight$0` @0-61: for every section index of the chunk,
**`if (!section.hasOnlyAir()) updateSectionStatus(SectionPos.of(pos, y),
false)`** (`LevelChunkSection.hasOnlyAir` @237 = `nonEmptyBlockCount == 0`;
recalcBlockCounts counts non-`isAir()` states via `count()`). Later
empty↔non-empty flips go through the same updateSectionStatus (ChunkAccess
setBlockState path). Then `lambda$initializeLight$2`:
`setLightEnabled(pos, flag)`, `retainData(pos, false)`.

⇒ **storage map contains section s iff hasData(s) OR some 26-neighbor
(including diagonal, cross-chunk) has hasData** — i.e. the non-empty
section set dilated by 1 in x/y/z (chebyshev).

### 7c. BlockLight layer content lifecycle

`BlockLightSectionStorage` overrides nothing relevant;
`LayerLightSectionStorage.createDataLayer` @286-301 = queued layer or
`new DataLayer()` (empty). Only `setStoredLevel` @195-236 (nibble write,
also copy-on-write via changedSections) materializes it. `LightEngine`
propagation only writes into sections where `storingLightForSection`
(@89-99 = updating map has a layer). ⇒ **BlockLight emitted for s iff
s ∈ storage map AND ≥1 block-light nibble write ever hit s** (writes of
value 0 count; materialization is permanent — nothing ever nulls `data`
for block light).

### 7d. SkyLight specifics (SkyLightSectionStorage + SkyLightEngine)

`SkyDataLayerStorageMap` adds `topSections: Long2IntOpenHashMap`
(key = column zeroNode) and `currentLowestY` (init `Integer.MAX_VALUE`,
also used as the map's defaultReturnValue).

`onNodeAdded(pos)` @111-161 (runs at the END of initializeSection, i.e.
AFTER createDataLayer): `y = SectionPos.y(pos)`; if `currentLowestY > y`:
currentLowestY = y and re-set defaultReturnValue; if
`topSections.get(col) < y+1` → `put(col, y+1)`. ⇒ **topSections[col] =
1 + max y of any section ever added in that column** — including the
purely-neighbor-induced section at (own topmost non-empty y)+1, because
dy=+1/dx=dz=0 is in the 26-neighborhood. So for a column whose highest
non-empty section is 6: sections -5..7 are in storage and topSections = 8.
`onNodeRemoved` @163-219 lowers/removes it (not hit during gen).

`createDataLayer(pos)` @221-282 (order matters: consults topSections
BEFORE this node's own onNodeAdded):
- queued layer → return it.
- `top = topSections.get(col)`; if `top == currentLowestY` (no entry) or
  `y >= top`: `lightOnInSection(pos)` (= columnsWithSources contains col,
  set by `setLightEnabled(col,true)`) ? `new DataLayer(15)` (unmaterialized
  default-15 → isEmpty()==false → EMITTED as 2048×0xFF) : `new DataLayer()`.
- else (below top): walk UP from pos+1 until a layer exists;
  `repeatFirstLayer(that)` @284-317: if `isDefinitelyHomogenous`
  (data==null) → `copy()` (inherits defaultValue!); else new 2048 array
  with the bottom horizontal slice (first 128 bytes) copied to all 16
  y-slices.
  During initializeLight registration columnsWithSources is still empty
  (setLightEnabled runs after the section loop / at propagate), so all
  gen-time creations are EMPTY layers.

`SkyLightEngine.propagateLightSources(ChunkPos)` @0-618 (runs at LIGHT
stage per chunk, after that chunk + neighbors are registered):
- @12-21 `storage.setLightEnabled(zeroNode, true)` (adds col to
  columnsWithSources).
- @152-175: `k = storage.getTopSectionY(zeroNode)` (= topSections[col]),
  `bottom = storage.getBottomSectionY()` (= currentLowestY).
- @195-205 loop `n = k-1` down to `bottom`:
  `layer = storage.getDataLayerToWrite(SectionPos.asLong(cx, n, cz))`
  (@223-235; null → skip @237-242).
  Per column (x,z) 0..15 @265-…: `lowestSourceY = chunkSkyLightSources
  .getLowestSourceY(x,z)`; skip column if `lowestSourceY > sectionTop+15`
  @293-300; else for `y = sectionTop+15` down to
  `max(sectionBottom, lowestSourceY)` @444-457:
  **`layer.set(x, sectionRelative(y), z, 15)`** @460-473 (explicit nibble
  writes → materializes), enqueueing edge sources @476-… where y hits
  lowestSourceY or dips below a neighbor column's lowest source.
  ⇒ every stored section n < topSections whose y-range intersects
  `[lowestSourceY(x,z), ∞)` for at least one of its 16×16 columns gets
  materialized with 15s here. Downward/lateral cave propagation later
  writes deeper sections via `setStoredLevel`.

### 7e. Explaining the golden cutoffs (surface y≈60-110, non-empty block
sections -4..6, light sections -5..20)

- **No SkyLight at Y≥8**: sections ≥8 are ≥2 above the topmost non-empty
  section (6) in every column of the 3×3 neighborhood ⇒ sectionStates==0 ⇒
  no layer in visibleSectionData ⇒ `getDataLayerData` returns null.
  (Query-time all-15 for y≥top is synthesized in
  `SkyLightSectionStorage.getLightValue` @29-109 — walk-up + "return 15"
  @114-123 — but the SAVE path never calls it.)
- **SkyLight present at Y=7**: Y=7 is in storage (neighbor of 6) and
  `topSections = 8`, so the @195-205 pre-fill loop starts at n = 7 and
  writes 15s into it (every column with lowestSourceY ≤ 127-region top).
- **SkyLight absent at Y≤0 despite being in storage** (sections -5..0 have
  layers): created empty (`new DataLayer()`), the pre-fill only reaches
  down to lowestSourceY, and no propagation write ever reached them ⇒
  `data==null && defaultValue==0` ⇒ `isEmpty()` ⇒ dropped at copyOf
  @168-173. The deepest emitted section (Y=1) is simply the deepest one
  that received ≥1 sky nibble write.
- **BlockLight only Y=-4..4**: same storage set (-5..7), but block layers
  only materialize on write; block-light sources/propagation in these
  chunks never wrote outside sections -4..4 (lava/aquifer sources low,
  nothing above y≈79). Y=-5 and 5..7 layers exist but stay isEmpty.

### 7f. Predicate for our C engine

Track per LightLayer ∈ {BLOCK, SKY} and per light-section s = (cx,sy,cz),
sy ∈ [minSection-1, maxSection+1]:

1. `has_data[s]`: set when the owning chunk registers at initializeLight
   with section non-empty (`nonEmptyBlockCount != 0`), or on a later
   empty→non-empty updateSectionStatus; cleared on the reverse.
2. `in_storage[s] = has_data[s] || ∃ 26-neighbor n: has_data[n]`
   (may be recomputed at save; vanilla maintains it incrementally, and
   incremental vs recomputed differ only through the layer-content side
   effects of add/remove, which don't occur during pure generation).
3. Layer content state per (layer, s): `materialized` flag + `defv`
   (nibble). Transitions, in vanilla event order:
   - created on the 0→nonzero sectionStates transition:
     BLOCK → (materialized=0, defv=0).
     SKY → if created at/above the column's current top-mark or column has
     no mark: (0, lightEnabled(col) ? 15 : 0); else copy the
     state/bottom-slice of the nearest existing layer above (repeat rule
     §7d).
   - any nibble write (`set`) → materialized=1 (allocate 2048B initialized
     to defv-packed nibbles first).
   - SKY pre-fill at propagateLightSources: explicit set(…,15) writes per
     §7d bounds (our engine already replays these if it replays vanilla's
     propagation; the important part is that they count as writes).
4. **EMIT layer for s iff `in_storage[s] && (materialized || defv != 0)`.**
   Bytes = materialized ? the 2048-byte array : 2048 × (defv|defv<<4).
   Per-column top-mark bookkeeping: `top[col] = 1 + max sy ever added in
   col` (update AFTER creating the layer for that node), `lowest = min sy
   ever added` (global), map default = `lowest`.

Section compound skip rule (copyOf): emit a `sections` entry for every
block-range section always; for sy = minSection-1 / maxSection+1 only if
a light layer passes rule 4.

## 8. Heightmap

- ctor @15-39: `bits = Mth.ceillog2(chunk.getHeight()+1)` — 384+1=385 →
  **9 bits**; `new SimpleBitStorage(9, 256)` → valuesPerLong = 64/9 = 7,
  **37 longs** (ceil(256/7)), last long holds 4 values (bits 36..63 of
  long 36 zero).
- index @342-349 = `x + z*16` (x fastest).
- `setHeight(x,z,h)` @273-286 stores `h - minY`; callers pass
  `y+1` of the highest matching block (`update` @38-44, `primeHeightmaps`
  @204-214 both `setHeight(..., y+1)`), `getFirstAvailable` @261-271
  returns `stored + minY`. ⇒ **raw value = (highest blocking y)+1 − minY =
  y + 65 for minY=-64; 0 when no blocking block in the column**.
- `getRawData()` @335-340 = `data.getRaw()` (internal array, no copy).
  `copyOf` @460-495 stores `getRawData().clone()` into a fresh
  `EnumMap<Heightmap.Types,long[]>`, filtered by
  `persistedStatus.heightmapsAfter().contains(key)` @440-457 (FULL →
  the 4 FINAL kinds).
- `write()` @459-492: `heightmaps.forEach(lambda$write$0)` — EnumMap ⇒
  **ordinal order**: WORLD_SURFACE(1), OCEAN_FLOOR(3), MOTION_BLOCKING(4),
  MOTION_BLOCKING_NO_LEAVES(5); key = `Types.getSerializationKey()` =
  enum name string; value = `new LongArrayTag(longs)` (wraps, no copy);
  all under compound `"Heightmaps"`. (ChunkAccess.heightmaps itself is
  `Maps.newEnumMap` — `getHeightmaps()` = entrySet, ordinal order, so the
  copyOf iteration is deterministic too.)
- `setRawData` @288-333 (load path) = System.arraycopy when length
  matches — bit-exact carry.

## 9. Misc facts for the serializer agent

- `LevelChunkSection.copy()` (copy ctor @0-60) copies counts + deep-copies
  both containers (`PalettedContainer.copy()` → `Data.copy()` → storage
  copy + palette copy) — pack() runs on the copies; `acquire()/release()`
  threading check only.
- `pack()` reads `data.storage`/`data.palette` directly; the in-memory
  configuration is irrelevant to output except through `storage.getBits()`
  (only used as the transient HashMapPalette capacity hint).
- `PalettedContainer.unpack` (read path, for round-trip guard) @435-563:
  size-1 palettes accept missing data (ZeroBitStorage); bits mismatch
  between `bitsInStorage(paletteSize)` and stored longs is re-encoded via
  `reencodeContents` when `alwaysRepack || bitsInMemory != bitsInStorage`.
- `isLightOn`: written only when `lightCorrect` (write() @336-349, value
  always byte 1). `Y` is a ByteTag ((byte)y, so -5..20 as signed byte).
- `putByteArray`/`putLongArray` wrap the array without copying (@235-272
  CompoundTag) — safe because copyOf already cloned.
- Biome containers in sections are `PalettedContainerRO` — `codecRO`
  encode path is identical (`PalettedContainerRO.pack` interface method,
  same `PalettedContainer.pack` implementation).

## C implementation rules

1. **Palette repack (blocks & biomes)**: at save, scan storage indices
   0..N-1 (N=4096 blocks / 64 biomes; index = y<<8|z<<4|x resp.
   y<<4|z<<2|x). Maintain old→new id cache keyed on the previous index's
   old id (pure optimization); append each first-seen value to the new
   palette in scan order. Serialized palette = that list; entries: blocks =
   compound {Name:string [, Properties:{...}]} (Properties keys INSERTED in
   DESCENDING property-name order for the HashMap emulation; property list
   itself is the block's name-sorted property set; values: bool→"true"/
   "false", int→decimal, enum→getSerializedName), biomes = "ns:path"
   string.
2. **Bits/packing**: n = palette size. blocks: bits = n==1?0:max(4,
   ceil_log2(n)); biomes: bits = ceil_log2(n) (0 when n==1). bits==0 →
   omit "data" key entirely. Else pack values LSB-first, floor(64/bits)
   per long, no spanning, tail longs zero-padded; long count =
   ceil(N / floor(64/bits)). Compound insertion order: "palette" then
   "data" (LongArrayTag).
3. **Light emission predicate**: emit `BlockLight`/`SkyLight` for light
   section s iff `in_storage[s] && !(data==NULL && defv==0)` per §7f;
   in_storage = 1-dilation (26-neighborhood) of the "section non-empty at
   initializeLight registration" set; sky layers get defv=15 only when
   created above the per-column top-mark in an already-light-enabled
   column, and get materialized by the propagate-time explicit 15-writes
   down to each column's lowestSourceY; block layers materialize only on
   propagation writes. Emitted bytes: nibble array (even index = low
   nibble, index = y<<8|z<<4|x), unmaterialized default-15 → 2048×0xFF.
4. **Section list**: y ascending from minLightSection(-5) to
   maxLightSection-1(20); block-range sections always get
   {block_states, biomes[, BlockLight][, SkyLight], Y}; boundary sections
   (-5, 20) only appear with a passing light layer ({BlockLight|SkyLight,
   Y}). Y = signed byte tag, inserted LAST.
5. **Heightmap packing**: per kind (ordinal order WORLD_SURFACE,
   OCEAN_FLOOR, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES; keys = enum
   names) pack 256 values (index x + z*16) of `highest_blocking_y + 1 -
   minY` (0 if none) at 9 bits, 7/long, 37 longs, LSB-first.
