# R-C — 26.2 chunk NBT WRITE path, end to end (Task 12 spec)

Source of truth: `javap -p -c -constants -cp
tools/golden/libs/extracted/server-26.2.jar <fqcn>` (+ `-v` for
BootstrapMethods). Offsets `@n` are bytecode offsets inside the named
method. Empirical anchors: `Probe.java` compiled against the real jar +
DFU 10.0.21 (classpath = `tools/golden/libs/extracted/classpath.txt`),
cross-checked byte-for-byte against `golden/seed1234567890_r.0.0.mca`
chunk (0,0). Serialization is **manual CompoundTag building** for the
skeleton, with **codec (NbtOps) sub-encodes** for exactly four keys:
`block_states`, `biomes` (PalettedContainer codecs), `block_ticks`,
`fluid_ticks` (SavedTick codec list) — plus the never-present-here
`blending_data` / `below_zero_retrogen`.

## 0. Save caller / threading / gametime

`ChunkMap extends SimpleRegionStorage` (class header).
`ChunkMap.save(ChunkAccess)`:

- @11-19 `chunk.tryMarkSaved()` guard (false → no write).
- @25-85 PROTOCHUNK-only skips (`isExistingChunkFull`, EMPTY without
  non-NONE starts).
- @105-114 `SerializableChunkData.copyOf(this.level, chunk)` — runs
  **synchronously on the caller (Server thread)**; all mutable state
  (sections, light layers, heightmaps, ticks) is snapshotted here.
- @122-133 `CompletableFuture.supplyAsync(data::write,
  Util.backgroundExecutor())` — NBT tree building is async, but operates
  only on the snapshot.
- @135-149 `this.write(pos, supplier)` →
  `SimpleRegionStorage.write(ChunkPos, Supplier)` @0-9 →
  `IOWorker.store(pos, supplier)` → (IO thread)
  `RegionFileStorage.write(ChunkPos, CompoundTag)`.

**Gametime**: `copyOf` calls `ServerLevel.getGameTime()` twice — @504-509
feeding `ChunkAccess.getTicksForSerialization(J)` and @577-580 as the
`lastUpdateTime` ctor arg. Same value, captured on the Server thread.
There is no other gametime source in the path.

`RegionFileStorage.write` @25-35:
`out = regionFile.getChunkDataOutputStream(pos)` (=
`new DataOutputStream(version.wrap(new ChunkBuffer(pos)))`,
RegionFile @704-717) then `NbtIo.write(tag, out)`, `out.close()`.

## 1. copyOf(ServerLevel, ChunkAccess) — field capture

Record fields (declaration order): `containerFactory, chunkPos,
minSectionY, lastUpdateTime, inhabitedTime, chunkStatus, blendingData
(BlendingData$Packed), belowZeroRetrogen, upgradeData, carvingMask
(long[]), heightmaps (Map<Heightmap$Types,long[]>), packedTicks
(ChunkAccess$PackedTicks), postProcessingSections (ShortList[]),
lightCorrect, sectionData (List<SectionData>), entities
(List<CompoundTag>), blockEntities (List<CompoundTag>), structureData
(CompoundTag)`.

`SectionData` record = `(int y, LevelChunkSection chunkSection,
DataLayer blockLight, DataLayer skyLight)` (ctor descriptor
`(ILLevelChunkSection;LDataLayer;LDataLayer;)V`).

- @0-4 `chunk.canBeSerialized()` guard (IllegalArgumentException).
- **Sections loop** @52-249: `lightEngine =
  level.getChunkSource().getLightEngine()`; bounds
  `for (y = lightEngine.getMinLightSection();
  y < lightEngine.getMaxLightSection(); y++)` — note `if_icmpge` @66:
  upper bound EXCLUSIVE. `LevelLightEngine.getMinLightSection` @0-11 =
  `getMinSectionY() - 1`; `getMaxLightSection` @0-9 =
  `getMinLightSection() + getLightSectionCount()` (= sectionsCount + 2).
  Overworld (−64..320, 24 block sections −4..19): y iterates
  **−5..20 inclusive, 26 iterations**.
  - @69-95 `idx = chunk.getSectionIndexFromSectionY(y)`; `inRange =
    idx >= 0 && idx < sections.length`.
  - @97-137 `blockLight = lightEngine.getLayerListener(LightLayer.BLOCK)
    .getDataLayerData(SectionPos.of(chunkPos, y))`; same with
    `LightLayer.SKY` for `skyLight`.
  - @139-185 presence condition per layer: kept iff
    `layer != null && !layer.isEmpty()`, and then **`layer.copy()`** is
    stored (else null).
  - @187-205 skip section entirely unless
    `inRange || blockLight != null || skyLight != null`.
  - @205-222 `chunkSection = inRange ? sections[idx].copy() : null`.
  - @224-245 `new SectionData(y, chunkSection, blockLight, skyLight)`
    appended to ArrayList (ascending y order).
- **Block entities** @252-330: `new ArrayList(getBlockEntitiesPos()
  .size())`; for each pos: `chunk.getBlockEntityNbtForSaving(pos,
  level.registryAccess())`; null results skipped.
- **entities / carvingMask** @342-396: only when
  `chunk.getPersistedStatus().getChunkType() == ChunkType.PROTOCHUNK`:
  entities = `ProtoChunk.getEntities()` (addAll), carvingMask =
  `ProtoChunk.getCarvingMask().toArray()` if mask non-null. For
  LEVELCHUNK (status `minecraft:full`) both stay `[]`/`null`.
- **Heightmaps** @396-501: `new EnumMap(Heightmap$Types.class)`; iterate
  `chunk.getHeightmaps()` (backing map is
  `Maps.newEnumMap(Types.class)` — `ChunkAccess.<init>` @4-10 → entry
  iteration in **enum ordinal order**: WORLD_SURFACE_WG(0),
  WORLD_SURFACE(1), OCEAN_FLOOR_WG(2), OCEAN_FLOOR(3),
  MOTION_BLOCKING(4), MOTION_BLOCKING_NO_LEAVES(5)); keep entry iff
  `chunk.getPersistedStatus().heightmapsAfter().contains(type)`
  @440-457; value = `heightmap.getRawData()` then **`.clone()`**
  @460-495.
  - `ChunkStatus.<clinit>`: `WORLDGEN_HEIGHTMAPS =
    EnumSet.of(OCEAN_FLOOR_WG, WORLD_SURFACE_WG)` @0-9 for statuses
    empty..surface; `FINAL_HEIGHTMAPS = EnumSet.of(OCEAN_FLOOR,
    WORLD_SURFACE, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES)` @12-27
    for carvers..full (register calls @130-223). So a full chunk
    serializes exactly the 4 FINAL types (WG types filtered out even if
    still present in the chunk's map).
- **Ticks** @504-512: `packedTicks =
  chunk.getTicksForSerialization(level.getGameTime())` (§7).
- **PostProcessing** @514-544: `Arrays.stream(chunk.getPostProcessing())
  .map(lambda$copyOf$0).toArray(ShortList[]::new)`;
  `lambda$copyOf$0` @0-25: `null || isEmpty()` → null, else
  `new ShortArrayList(list)` (copy). Array length = block sectionsCount
  (24 overworld), NOT light section count.
- **Structures** @546-559: `structureData =
  packStructureData(StructurePieceSerializationContext.fromLevel(level),
  chunkPos, chunk.getAllStarts(), chunk.getAllReferences())` (§8). Both
  maps are `Maps.newHashMap()` in `ChunkAccess.<init>`.
- **Ctor call** @564-634 argument sources: `level
  .palettedContainerFactory()`, `chunkPos`, `chunk.getMinSectionY()`,
  `level.getGameTime()`, `chunk.getInhabitedTime()`,
  `chunk.getPersistedStatus()`, `Optionull.map(chunk.getBlendingData(),
  BlendingData::pack)`, `chunk.getBelowZeroRetrogen()`,
  `chunk.getUpgradeData().copy()`, carvingMask, heightmaps EnumMap,
  packedTicks, postProcessing copy, `chunk.isLightCorrect()`,
  sectionData, entities, blockEntities, structureData.

## 2. write() — exact put() sequence into the root CompoundTag

```
 1. root = NbtUtils.addCurrentDataVersion(new CompoundTag())     @0-10
      → putInt("DataVersion", 4903)  (FIRST insertion; NbtUtils
        addCurrentDataVersion @1362-1371 → addDataVersion @1373-1380;
        value = SharedConstants.getCurrentVersion().dataVersion()
        .version() = version.json "world_version": 4903)
 2. putInt("xPos", chunkPos.x())                                 @11-21
 3. putInt("yPos", minSectionY)                                  @24-32   (−4 overworld)
 4. putInt("zPos", chunkPos.z())                                 @35-45
 5. putLong("LastUpdate", lastUpdateTime)                        @48-55
 6. putLong("InhabitedTime", inhabitedTime)                      @58-65
 7. putString("Status", BuiltInRegistries.CHUNK_STATUS
      .getKey(chunkStatus).toString())                           @68-86   ("minecraft:full")
 8. storeNullable("blending_data", BlendingData.Packed.CODEC, v) @89-99   SKIP if null
 9. storeNullable("below_zero_retrogen",
      BelowZeroRetrogen.CODEC, v)                                @102-112 SKIP if null
10. if (!upgradeData.isEmpty())
      put("UpgradeData", upgradeData.write())                    @115-138
      (UpgradeData.isEmpty @625-651: all index[][] rows null AND
       sides EnumSet empty — true for freshly generated chunks)
11. put("sections", <ListTag built @139-324, §3>)                @327-335 ALWAYS
12. if (lightCorrect) putBoolean("isLightOn", true)              @336-347
      (ByteTag 1; NEVER written as false)
13. put("block_entities", new ListTag()+addAll(blockEntities))   @350-378 ALWAYS (may be empty)
14. if (chunkStatus.getChunkType() == PROTOCHUNK):
      put("entities", new ListTag()+addAll(entities))            @392-419
      if (carvingMask != null)
        putLongArray("carving_mask", carvingMask)                @420-434
15. saveTicks(root, packedTicks)                                 @437-444
      @0-10  store("block_ticks",  BLOCK_TICKS_CODEC, blocks())  ALWAYS
      @13-23 store("fluid_ticks",  FLUID_TICKS_CODEC, fluids())  ALWAYS
16. put("PostProcessing", packOffsets(postProcessingSections))   @445-458 ALWAYS
17. hm = new CompoundTag(); heightmaps.forEach((t, a) ->
      hm.put(t.getSerializationKey(), new LongArrayTag(a)))
      (lambda$write$0 @0-17; EnumMap.forEach = ordinal order:
       WORLD_SURFACE, OCEAN_FLOOR, MOTION_BLOCKING,
       MOTION_BLOCKING_NO_LEAVES for a full chunk);
    put("Heightmaps", hm)                                        @459-492 ALWAYS
18. put("structures", structureData)                             @493-504 ALWAYS
```

Conditional keys, summary: `blending_data`, `below_zero_retrogen`
(null → absent), `UpgradeData` (isEmpty → absent), `isLightOn`
(lightCorrect only), `entities` + `carving_mask` (PROTOCHUNK only; mask
additionally non-null). Everything else unconditional. There is **no
`Lights` key** and **no `CarvingMasks` (plural) key** in 26.2.

**Insertion order ≠ byte order.** Byte order = HashMap iteration order
(§4). Full-chunk root (15 keys, golden-verified):

```
Status, zPos, block_entities, yPos, LastUpdate, structures,
InhabitedTime, xPos, Heightmaps, sections, isLightOn, block_ticks,
PostProcessing, DataVersion, fluid_ticks
```

Without `isLightOn` (14 keys) the order is the same minus that key.

## 3. sections list (write @139-324)

- @147-162: `blockStatesCodec =
  containerFactory.blockStatesContainerCodec()`, `biomesCodec =
  containerFactory.biomeContainerCodec()` (hoisted once).
- Per SectionData (ascending y):
  1. `sec = new CompoundTag()` @197-204.
  2. if `chunkSection != null` (i.e. section in block range):
     `sec.store("block_states", blockStatesCodec, sec.getStates())`
     @218-229 then `sec.store("biomes", biomesCodec, sec.getBiomes())`
     @232-244. Unconditional for in-range sections — hasOnlyAir does NOT
     suppress emission.
  3. if `blockLight != null`
     `sec.putByteArray("BlockLight", blockLight.getData())` @247-268.
  4. if `skyLight != null`
     `sec.putByteArray("SkyLight", skyLight.getData())` @271-292.
  5. if `!sec.isEmpty()`: `sec.putByte("Y", (byte) y)` (i2b @313) and
     `list.add(sec)` @295-323. Empty compounds are dropped entirely —
     no Y-only entries; light-only sections outside the block range DO
     get an entry (lights + Y only).
- `DataLayer.getData()` @594-613: if `data == null` materializes the
  2048-byte array (nibble-fill of `defaultValue`), returns the
  **backing array** (no copy; ByteArrayTag wraps it by reference).
  `DataLayer.copy()` (used at copyOf time) @616-633: `data != null` →
  `clone()`, else `new DataLayer(defaultValue)`. `DataLayer.isEmpty`
  @735-746: `data == null && defaultValue == 0` — so a null-data layer
  with non-zero defaultValue IS serialized (as 2048 packed bytes).
- Golden r.0.0 chunk(0,0): 24 section entries — the y=−5 and y=20
  light-border sections had no non-empty DataLayer and were dropped.
  Y bytes: −4 → 0xFC etc.
- `block_states` sub-compound (codec-built, §4 order): keys
  `data` (LongArrayTag, absent for single-entry palette) then
  `palette` (ListTag of compounds `{[Properties,] Name}`); byte order
  is `[data, palette]` and `[Properties, Name]` (hash-driven).
  `biomes`: `[palette]` (+`data` when >1 entry); biome palette elements
  are StringTags.

## 4. CompoundTag internals — the byte-order engine

- `CompoundTag()` @0-11: `this(new HashMap<>())` — **no-arg
  `java.util.HashMap`**: default capacity 16, load factor 0.75, lazy
  table. NOT `(8, 0.8f)`.
- `write(DataOutput)` @0-66: `for (String name : tags.keySet())
  writeNamedTag(name, tags.get(name), out);` then `out.writeByte(0)` —
  iterates the map's keySet directly; emission order IS HashMap
  iteration order.
- `writeNamedTag` @0-36: `out.writeByte(tag.getId())`; `if (id == 0)
  return;` `out.writeUTF(name)`; `tag.write(out)`.
- `put(String, Tag)` = `map.put`; `putBoolean` @0-15 →
  `ByteTag.valueOf(z)`; `putByteArray`/`putIntArray`/`putLongArray`
  @0-19 wrap the array **by reference** (`new ByteArrayTag(b)` etc.).
- `store(name, codec, value)` @982-994:
  `put(name, codec.encodeStart(NbtOps.INSTANCE, value).getOrThrow())`.
  `storeNullable` @0-11: no-op when value null.
- `isEmpty()` = `map.isEmpty()`.

**Java HashMap emulation required for C** (verified against golden):
`h = String.hashCode()` (UTF-16; ASCII = byte values),
`spread = h ^ (h >>> 16)`, `bucket = spread & (cap-1)`. Insert at chain
tail. After inserting, `if (++size > threshold) resize()` — threshold =
0.75·cap, so the **13th put resizes 16→32** (root always crosses this at
`PostProcessing`). Resize splits each chain into lo/hi lists by
`spread & oldCap`, preserving relative order. Treeify (8 nodes/bucket)
is unreachable for every serializer-owned map (max chain length 2), but
codec-built palette-property compounds etc. stay small too.

Key hash table (spread, bucket@16, bucket@32):

| key | spread | b16 | b32 |
|---|---|---|---|
| DataVersion | 538278331 | 11 | 27 |
| xPos | 3655307 | 11 | 11 |
| yPos | 3685155 | 3 | 3 |
| zPos | 3714882 | 2 | 2 |
| LastUpdate | −308664699 | 5 | 5 |
| InhabitedTime | 1716647498 | 10 | 10 |
| Status | −1808652256 | 0 | 0 |
| sections | 947938990 | 14 | 14 |
| isLightOn | −409239920 | 0 | 16 |
| block_entities | −1552867262 | 2 | 2 |
| block_ticks | 1228137847 | 7 | 23 |
| fluid_ticks | 324824221 | 13 | 29 |
| PostProcessing | 2089376634 | 10 | 26 |
| Heightmaps | −810399764 | 12 | 12 |
| structures | 185109096 | 8 | 8 |
| UpgradeData | −177159498 | 6 | 22 |
| entities | −2102082187 | 5 | 21 |
| carving_mask | −1877774760 | 8 | 24 |
| blending_data | 957633580 | 12 | 12 |
| below_zero_retrogen | −1793035973 | 11 | 27 |
| block_states | −601366061 | 3 | 19 |
| biomes | −1388968219 | 5 | 5 |
| BlockLight | −1032786215 | 9 | 25 |
| SkyLight | −1659476605 | 3 | 3 |
| Y | 89 | 9 | 25 |
| palette | −798931430 | 10 | 26 |
| data | 3075972 | 4 | 4 |
| Name | 2420367 | 15 | 15 |
| Properties | 1067407052 | 12 | 12 |
| starts | −892534116 | 12 | 28 |
| References | −916566026 | 6 | 22 |
| i / x / y / z / t / p | 105/120/121/122/116/112 | 9/8/9/10/4/0 | — |
| WORLD_SURFACE | 1350230202 | 10 | 26 |
| OCEAN_FLOOR | −55947807 | 1 | 1 |
| MOTION_BLOCKING | −1669842202 | 6 | 6 |
| MOTION_BLOCKING_NO_LEAVES | −1591068333 | 3 | 19 |

Resulting orders (probe = golden, all ≤12 keys stay at cap 16):
- section `[block_states, SkyLight, biomes, BlockLight, Y]` (whichever
  of the light keys exist; block_states before SkyLight and BlockLight
  before Y are chain-order — insertion order matters in buckets 3 and 9).
- Heightmaps `[OCEAN_FLOOR, MOTION_BLOCKING_NO_LEAVES, MOTION_BLOCKING,
  WORLD_SURFACE]`.
- structures `[References, starts]`.
- block_states `[data, palette]`; palette block entry
  `[Properties, Name]`; tick entry `[p, t, x, i, y, z]` (i before y =
  chain order in bucket 9: i inserted before y).

## 5. ListTag

- Backing `ArrayList<Tag>`; `ListTag()` @0-11.
- `write(DataOutput)` @0-72: `byte t = identifyRawElementType()`;
  `writeByte(t)`; `writeInt(list.size())`; per element
  `wrapIfNeeded(t, tag).write(out)` (payload only, no per-element id).
- `identifyRawElementType()` @0-62: 0 for empty list; else first
  element's id; if any element id differs → 10 (then non-compound
  elements get wrapped in `{"": tag}` marker compounds — never happens
  for chunk data, all our lists are homogeneous).
- **Empty list bytes = `00` (type) + `00 00 00 00` (length)** — golden
  `block_entities`/`fluid_ticks` show element type 0.
- `packOffsets(ShortList[])` @1557-1605: outer `ListTag`; for each of
  the 24 array slots: inner `ListTag`; if slot non-null, add
  `ShortTag.valueOf(list.getShort(i))` per element (ShortTag id 2);
  outer.add(inner) **always** → exactly 24 inner lists. Outer element
  type byte = 9 (first element is a ListTag); inner empties are
  `00 00000000`. Golden hex (roundtrip probe):
  `09 <len> 50 6f ... 09 00000018 (00 00000000)×24`-shaped.

## 6. Root emission (NbtIo) — unnamed root, no wrapper

`NbtIo.write(CompoundTag, DataOutput)` @0-5 →
`writeUnnamedTagWithFallback` @0-12 (wraps the output in
`NbtIo$StringFallbackDataOutput`, which only intercepts
`UTFDataFormatException` and rewrites the string as `""` — irrelevant
for ASCII chunk keys) → `writeUnnamedTag` @0-37:
`writeByte(10)`, `writeUTF("")` (bytes `00 00`), `tag.write(out)`.

The region payload root **is** the chunk compound: id `0x0A`, empty
name, then map entries. No outer compound, no `"Level"` wrapper.
Golden chunk(0,0) decompressed payload starts `0a 00 00 08 0006 Status…`
(100081 bytes). Strings use `java.io.DataOutputStream.writeUTF`
modified UTF-8: u16 BE byte-length then bytes (ASCII = identity).

## 7. Ticks

Codecs (`SerializableChunkData.<clinit>` @0-35): `BLOCK_TICKS_CODEC =
SavedTick.codec(BuiltInRegistries.BLOCK.byNameCodec()).listOf()`;
FLUID likewise on `BuiltInRegistries.FLUID`.

`SavedTick.codec(typeCodec)` @0-19 (record `(T type, BlockPos pos,
int delay, TickPriority priority)`):
- `posCodec = RecordCodecBuilder.mapCodec(Codec.INT.fieldOf("x"),
  Codec.INT.fieldOf("y"), Codec.INT.fieldOf("z"))` (lambda$codec$0
  @0-67) — a MapCodec **inlined** into the tick record (no nested
  compound).
- fields (lambda$codec$1 @0-74): `typeCodec.fieldOf("i")`, `posCodec`
  (→ x, y, z), `Codec.INT.fieldOf("t")` (delay),
  `TickPriority.CODEC.fieldOf("p")`. All `fieldOf` = mandatory: `p`/`t`
  are written even when 0.
- `TickPriority.CODEC` = `Codec.INT.xmap(...)` (TickPriority @107-125)
  → IntTag of the priority value (EXTREMELY_HIGH=−3 … NORMAL=0 …
  EXTREMELY_LOW=3).
- DFU 10.0.21 RecordCodecBuilder appends fields in **declaration
  order** (empirical, JsonOps: `{"i","x","y","z","t","p"}`);
  `NbtOps$NbtRecordBuilder.initBuilder` @0-7 = `new CompoundTag()`,
  `.append` @0-8 = `CompoundTag.put` — so each tick entry is a fresh
  HashMap compound with insertion order i,x,y,z,t,p → **byte order
  `[p, t, x, i, y, z]`** (golden: 681 block_ticks entries, all
  `p:Int t:Int x:Int i:String y:Int z:Int`).
- The list itself: `Codec.listOf()` encode → `NbtOps.createCollector`
  (empty prefix = EndTag → `GenericListCollector()` @878-887 whose
  `accept` @77-85 = `ListTag.add`) → plain ListTag; element-type byte
  from `identifyRawElementType` (10 for compounds, **0 when empty**).

Values: `LevelChunk.getTicksForSerialization(J)` @272-285 =
`new PackedTicks(blockTicks.pack(t), fluidTicks.pack(t))`.
`LevelChunkTicks.pack(long)` @193-237:
1. `out = new ArrayList(tickQueue.size())`; if `pendingTicks != null`
   (loaded-but-never-ticked chunk) addAll pendingTicks first;
2. copy `tickQueue` (PriorityQueue) into an ArrayList and
   `sort(SUB_TICK_ORDERING)` where `SUB_TICK_ORDERING =
   Comparator.comparingLong(ScheduledTick::subTickOrder)` (clinit @0-8)
   → **ascending subTickOrder = exact scheduling order**;
3. per tick `ScheduledTick.toSavedTick(gametime)` @0-26:
   `delay = (int)(triggerTick − gametime)`, priority preserved.

`ProtoChunkTicks.pack(long)` @0-4 returns its stored
`List<SavedTick>` unchanged (gametime ignored) — proto saves re-emit
loaded/scheduled SavedTicks as-is.

## 8. structures

`packStructureData(ctx, pos, starts, references)` @1386-1477:
- `root = new CompoundTag(); startsTag = new CompoundTag()`;
- for each entry of `getAllStarts()` (a `Maps.newHashMap()` keyed by
  `Structure` — identity hashCode ⇒ iteration order is
  **JVM-run-dependent when ≥2 structures**; golden region: empty):
  `startsTag.put(registry.getKey(structure).toString(),
  start.createTag(ctx, pos))` @45-112;
- `root.put("starts", startsTag)` @115-125 — ALWAYS, even empty;
- `refsTag = new CompoundTag()`; for each `getAllReferences()` entry:
  `if (value.isEmpty()) continue` @170-188;
  `refsTag.putLongArray(key.toString(), longSet.toLongArray())`
  @191-232 → **LongArrayTag (id 12)** entries;
- `root.put("References", refsTag)` @238-248 — ALWAYS.

Byte order `[References, starts]` (b16 6 vs 12). Golden bytes for the
empty case: `0a 000a "structures" | 0a 000a "References" 00 |
0a 0006 "starts" 00 | 00`.

## 9. Root scalar tag types

`xPos`/`yPos`/`zPos` IntTag(3); `LastUpdate`/`InhabitedTime` LongTag(4);
`Status` StringTag(8) = registry id string (`"minecraft:full"`);
`isLightOn` ByteTag(1) value 1, present iff `lightCorrect`;
`DataVersion` IntTag(3) = **4903** (26.2), inserted first (write @0-10).

## 10. blending_data / below_zero_retrogen on fresh worlds

Both use `storeNullable` (skip-on-null). `copyOf` sources:
`Optionull.map(chunk.getBlendingData(), BlendingData::pack)` @589-601 —
`ChunkAccess.blendingData` is only non-null for old-world blending
(`isOldNoiseGeneration` @705-713 keys off the same field);
`chunk.getBelowZeroRetrogen()` @604-607 — only set by the pre-1.18
upgrade path. Freshly generated 26.2 chunks: both null → **both keys
absent** (golden root confirms; `UpgradeData` likewise absent because
`UpgradeData.isEmpty` @625-651 — all `index` rows null and `sides`
empty; `entities`/`carving_mask` absent because status full =
LEVELCHUNK).

## Golden anchors (r.0.0.mca, chunk 0,0; probe /tmp/t12c/Probe.java)

- Root: 15 keys in the §2 order; root name `""`; payload 100081 B.
- 24 section entries (y=−5/20 dropped: no light layers there);
  observed section shapes: `[block_states, biomes, Y]`,
  `[block_states, biomes, BlockLight, Y]` (y=−2..1),
  `[block_states, SkyLight, biomes, Y]` (y=3..6-ish).
- Heightmaps: 4 × LongArrayTag `long[37]` in
  `[OCEAN_FLOOR, MOTION_BLOCKING_NO_LEAVES, MOTION_BLOCKING,
  WORLD_SURFACE]`.
- block_ticks: 681 compound entries `[p,t,x,i,y,z]`; fluid_ticks empty
  (element type 0); PostProcessing 24 empty inner lists, outer element
  type 9; structures both-empty; block_states `[data, palette]`,
  biomes `[palette]` with StringTag palette.

UNVERIFIED (out of scope here, flagged for Task-D/RegionFile work):
`RegionFileVersion.wrap` deflate parameters/version-byte selection and
ChunkBuffer header/sector layout were not disassembled in this report
(only the call shape @704-717); pin them in the region-container note.
