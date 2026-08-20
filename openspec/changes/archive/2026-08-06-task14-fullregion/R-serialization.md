# R-serialization — 26.2 청크 NBT: 구조물·블록엔티티 직렬화 포맷 핀다운

조사 기준: `tools/golden/work/task14-decomp/` (vineflower 디컴파일, MC 26.2 서버),
`tools/golden/work/server/` (javap 바이트코드), DFU 10.0.21 (`server.jar` 내장 `META-INF/libraries/com/mojang/datafixerupper/datafixerupper-10.0.21.jar`).
모든 주장은 아래에 파일·메서드 근거를 인용했고, 핵심 항목은 실측 골든
`tools/golden/work/unified-run/r.0.0.captured.mca` (DataVersion **4903**)를 파싱해 교차 검증했다 (검증 스크립트는 임시 `/tmp/nbtdump.py`, 커밋 안 함).

**대전제 — CompoundTag 는 `java.util.HashMap`이다.**
`net.minecraft.nbt.CompoundTag`(디컴파일: `task14-decomp-extra/CompoundTag.java` L14, L45–52):
`private final Map<String, Tag> tags`, 기본 생성자는 `new HashMap<>()`(capacity 16).
→ 디스크에 기록되는 키 순서 = **HashMap 반복 순서** (String.hashCode 기반 버킷 순서;
같은 버킷 충돌 시에는 삽입 순서, 리사이즈 이력은 삽입 시퀀스에 의해 결정).
따라서 아래에 적는 "삽입(put) 순서"는 byte-exact 재현에 load-bearing 하다(충돌·리사이즈 경로 결정).
26.2 BE 저장은 `ValueOutput`(=`TagValueOutput`, 내부에 CompoundTag) 경유이며 put 의미는 동일하다
(`task14-decomp-extra/TagValueOutput.java`: `store()`는 `codec.encodeStart` 성공 시 무조건 `output.put`,
`storeNullable`은 null 이면 생략, `store(MapCodec,·)`은 인코딩된 컴파운드를 `output.merge()`).

---

## 1. SerializableChunkData — "structures" 컴파운드

파일: `task14-decomp/storage/SerializableChunkData.java`

### 1.1 루트 write() 순서 (참고)

`write()` (L412–476) 루트 삽입 순서:
`DataVersion` (NbtUtils.addCurrentDataVersion 이 가장 먼저, Int=4903) → `xPos`(Int) → `yPos`(Int, minSectionY) → `zPos`(Int)
→ `LastUpdate`(Long) → `InhabitedTime`(Long) → `Status`(String, `BuiltInRegistries.CHUNK_STATUS.getKey(...).toString()`)
→ `blending_data`/`below_zero_retrogen` (storeNullable, null 생략) → `UpgradeData` (isEmpty 생략)
→ `sections`(List) → `isLightOn`(Byte, **true 일 때만**) → `block_entities`(List, **항상, 빈 리스트 포함**)
→ [PROTOCHUNK 일 때만: `entities`(List, 항상) → `carving_mask`(LongArray, null 생략)]
→ `block_ticks` → `fluid_ticks` (saveTicks, L478–481) → `PostProcessing`(List) → `Heightmaps`(Compound) → `structures`(Compound).
실측 골든 루트 키 순서(해시 순): `['Status','zPos','block_entities','yPos','LastUpdate','structures','InhabitedTime','xPos','Heightmaps','sections','isLightOn','block_ticks','PostProcessing','DataVersion','fluid_ticks']` — 위 삽입 순서와 정합.

각 section 컴파운드 삽입 순서 (L430–449): `block_states` → `biomes` (chunkSection != null 일 때) → `BlockLight`(ByteArray, non-null 시) → `SkyLight`(ByteArray, non-null 시) → **`Y`(Byte)는 마지막**, 그리고 sectionTag 가 비면 리스트에 추가 자체를 안 함.

### 1.2 packStructureData (L531–558)

```java
CompoundTag outTag = new CompoundTag();
CompoundTag startsTag = new CompoundTag();
Registry<Structure> reg = context.registryAccess().lookupOrThrow(Registries.STRUCTURE);
for (Entry<Structure, StructureStart> e : starts.entrySet()) {
    Identifier key = reg.getKey(e.getKey());
    startsTag.put(key.toString(), e.getValue().createTag(context, pos));
}
outTag.put("starts", startsTag);
CompoundTag referencesTag = new CompoundTag();
for (Entry<Structure, LongSet> e : references.entrySet()) {
    if (!e.getValue().isEmpty()) {
        referencesTag.putLongArray(reg.getKey(e.getKey()).toString(), e.getValue().toLongArray());
    }
}
outTag.put("References", referencesTag);
```

- **소스 맵**: `ChunkAccess.java` L79–80:
  `structureStarts = Maps.newHashMap()` — **`java.util.HashMap<Structure, StructureStart>`**,
  `structuresRefences = Maps.newHashMap()` — **`java.util.HashMap<Structure, LongSet>`** (fastutil 아님).
  `getAllStarts()/getAllReferences()`는 `Collections.unmodifiableMap` 뷰 (L225–226, L249–250).
  `Structure`는 `equals/hashCode` 오버라이드 없음(javap 확인) → **identity hashCode** 기반 반복 순서.
  단, 디스크 키 순서는 어차피 startsTag(CompoundTag=HashMap)의 String-hash 순서이므로, starts 맵 반복 순서는
  같은 버킷에 두 구조물 키가 충돌할 때만 관측 가능.
- **References 채우기**: `ChunkAccess.addReferenceForStructure` (L243–246):
  `computeIfAbsent(structure, k -> new LongOpenHashSet()).add(reference)` —
  값은 **`LongOpenHashSet`** (기본 capacity 16 / load factor 0.75), 저장 시 **`LongSet.toLongArray()`**
  = LongOpenHashSet 해시테이블 순회 순서(내용+테이블 크기로 결정, 충돌 슬롯만 삽입 순서 의존).
  reference 값은 `ChunkPos.pack(x,z) = (x & 0xFFFFFFFFL) | ((z & 0xFFFFFFFFL) << 32)` (javap 확인).
- **빈 LongSet 엔트리는 생략** (`!e.getValue().isEmpty()`), 그러나 `starts` 와 `References` 키 자체는 **빈 컴파운드라도 항상 기록**.
- 구조물 키 문자열: **STRUCTURE 다이내믹 레지스트리의 `getKey(structure).toString()`** — 예: `"minecraft:trial_chambers"`.
- outTag 삽입 순서: `starts` 먼저, `References` 나중. (실측 디스크 해시 순서: References, starts)
- 로드 시 (`unpackStructureStart`/`unpackStructureReferences`, L560–618): 둘 다 `Maps.newHashMap()` 재구성,
  References 는 `new LongOpenHashSet(long[] 필터결과)`, 체스판거리 >8 인 ref 는 드랍.

---

## 2. StructureStart / StructurePiece 공통 직렬화

### 2.1 StructureStart.createTag — `task14-decomp/StructureStart.java` L115–128

유효(`isValid()` = pieces 비어있지 않음)할 때 삽입 순서:

| # | key | type | 값 |
|---|-----|------|----|
| 1 | `id` | String | `registryAccess().lookupOrThrow(Registries.STRUCTURE).getKey(structure).toString()` |
| 2 | `ChunkX` | Int | **인자로 받은 chunkPos.x()** (= 저장 중인 청크 pos; packStructureData 가 청크 pos 전달) |
| 3 | `ChunkZ` | Int | 동일 |
| 4 | `references` | Int | `this.references` |
| 5 | `Children` | List\<Compound\> | `pieceContainer.save(context)` |

무효면 `{id:"INVALID"}` 만. 실측: `keys=['references','ChunkZ','id','Children','ChunkX']` (해시 순) — 정합.

`Children` 리스트 순서 = `PiecesContainer.pieces` (List.copyOf 로 고정된 생성 순서) 그대로
(`task14-decomp/pieces/PiecesContainer.java` L45–53: for-each 순회로 `piece.createTag` append).

### 2.2 StructurePiece.createTag — `task14-decomp/StructurePiece.java` L88–97

```java
tag.putString("id", BuiltInRegistries.STRUCTURE_PIECE.getKey(this.getType()).toString());
tag.store("BB", BoundingBox.CODEC, this.boundingBox);
tag.putInt("O", orientation == null ? -1 : orientation.get2DDataValue());
tag.putInt("GD", this.genDepth);
this.addAdditionalSaveData(context, tag);
```

| key | type | 비고 |
|-----|------|------|
| `id` | String | STRUCTURE_PIECE 레지스트리 키. 등록명은 `setPieceId(..., "ORP")` 등 → `id.toLowerCase(Locale.ROOT)` (`pieces/StructurePieceType.java` L84–90). 관련 값: jigsaw=`minecraft:jigsaw`, OceanRuin=`minecraft:orp`, Shipwreck=`minecraft:shipwreck`, RuinedPortal=`minecraft:rupo` |
| `BB` | **IntArray[6]** | `BoundingBox.CODEC = Codec.INT_STREAM.comapFlatMap(...)` — `IntStream.of(minX,minY,minZ,maxX,maxY,maxZ)` (`BoundingBox.java` L25–31). NbtOps 의 INT_STREAM → `IntArrayTag` |
| `O` | Int | orientation null → **-1**; 아니면 `Direction.get2DDataValue()` (NORTH=2 — TemplateStructurePiece 는 항상 NORTH 로 세팅) |
| `GD` | Int | genDepth |

그 뒤 서브클래스 `addAdditionalSaveData` 필드가 이어서 삽입된다.

---

## 3. 피스 타입별 추가 필드

### 3.1 PoolElementStructurePiece (id=`minecraft:jigsaw`) — `task14-decomp/PoolElementStructurePiece.java` L70–88

삽입 순서 (super 공통 4개 뒤):

| # | key | type | 생략 규칙 |
|---|-----|------|-----------|
| 5 | `PosX` | Int | 항상 |
| 6 | `PosY` | Int | 항상 |
| 7 | `PosZ` | Int | 항상 |
| 8 | `ground_level_delta` | Int | 항상 |
| 9 | `pool_element` | Compound | 항상. `tag.store("pool_element", StructurePoolElement.CODEC, registry-ops, element)` |
| 10 | `rotation` | String | 항상. `Rotation.LEGACY_CODEC` = `ExtraCodecs.legacyEnum(Rotation::valueOf)` → **enum name() 대문자** (`"NONE"`, `"CLOCKWISE_90"`, `"CLOCKWISE_180"`, `"COUNTERCLOCKWISE_90"`) — `ExtraCodecs.java` L495–502: encode = `Enum::toString` |
| 11 | `junctions` | List\<Compound\> | 항상 (빈 리스트 포함). 리스트 순서 = `this.junctions` ArrayList 추가 순서 |
| 12 | `liquid_settings` | String | **`this.liquidSettings != JigsawStructure.DEFAULT_LIQUID_SETTINGS`(=`APPLY_WATERLOGGING`) 일 때만**. 값: `"ignore_waterlogging"`/`"apply_waterlogging"` (`LiquidSettings.java`, StringRepresentable). trial_chambers 는 `ignore_waterlogging`이라 **기록됨** (실측 확인) |

**`pool_element` 컴파운드** — `pools/StructurePoolElement.java` L31–37:
`CODEC = BuiltInRegistries.STRUCTURE_POOL_ELEMENT.byNameCodec().dispatch("element_type", ...)`.
DFU 10.0.21 `KeyDispatchCodec.encode` (javap 확인): **element MapCodec 필드를 먼저 encode 하고, `element_type` 키를 마지막에 add** (uncompressed 경로: `encoder.encode(input, ops, prefix)` → `keyCodec.encode(...)`).
`SinglePoolElement.CODEC` group 순서 (`pools/SinglePoolElement.java` L48–51, L61–71):

삽입 순서: `location`(String, Identifier) → `processors`(String — `StructureProcessorType.LIST_CODEC` = `RegistryFileCodec(Registries.PROCESSOR_LIST)`: 레지스트리 참조 홀더는 **키 문자열**로 인코딩, 인라인이면 컴파운드) → `projection`(String, `"rigid"`/`"terrain_matching"` — `StructureTemplatePool.Projection` StringRepresentable) → `override_liquid_settings`(optionalFieldOf — **Optional.empty 생략**) → `element_type`(String, 예 `"minecraft:single_pool_element"`; legacy 는 `"minecraft:legacy_single_pool_element"`).
실측(trial_chambers): `{location, processors:"minecraft:trial_chambers_copper_bulb_degradation", projection:"rigid", element_type:"minecraft:single_pool_element"}` — 정합.

**`junctions` 엔트리** — `pools/JigsawJunction.serialize` (ImmutableMap.builder → `ops.createMap`): 삽입 순서
`source_x`(Int) → `source_ground_y`(Int) → `source_z`(Int) → `delta_y`(Int) → `dest_proj`(String, `Projection.getName()` = `"rigid"`/`"terrain_matching"`). 모두 항상 기록.
(NbtOps.createMap 은 ImmutableMap 순회 순서대로 CompoundTag 에 put.)

### 3.2 OceanRuinPieces$OceanRuinPiece (id=`minecraft:orp`) — `structures/OceanRuinPieces.java` L326–332

TemplateStructurePiece 공통(3.5) 뒤에:

| # | key | type | 값/생략 |
|---|-----|------|---------|
| 9 | `Rot` | String | `Rotation.LEGACY_CODEC` → 대문자 name (실측 `"CLOCKWISE_90"`). 항상 |
| 10 | `Integrity` | Float | 항상 (실측 0.8f) |
| 11 | `BiomeType` | String | `OceanRuinStructure.Type.LEGACY_CODEC` = legacyEnum → **대문자 name** `"WARM"`/`"COLD"` (실측 `"WARM"`). 항상 |
| 12 | `IsLarge` | Byte | Boolean, 항상 |

### 3.3 ShipwreckPieces$ShipwreckPiece (id=`minecraft:shipwreck`) — `structures/ShipwreckPieces.java` L115–120

TemplateStructurePiece 공통 뒤에, **이 순서**:

| # | key | type | 값/생략 |
|---|-----|------|---------|
| 9 | `isBeached` | Byte | 항상 |
| 10 | `Rot` | String | LEGACY 대문자 name, 항상 |
| 11 | `height_adjusted` | Byte | 항상 (로드: `getBooleanOr("height_adjusted", false)`) |

(isBeached 가 Rot 보다 먼저인 것 주의 — OceanRuin 과 순서가 다르다.)

### 3.4 RuinedPortalPiece (id=`minecraft:rupo`) — `structures/RuinedPortalPiece.java` L102–108

TemplateStructurePiece 공통 뒤에:

| # | key | type | 값/생략 |
|---|-----|------|---------|
| 9 | `Rotation` | String | `Rotation.LEGACY_CODEC` 대문자 name. 항상 |
| 10 | `Mirror` | String | `Mirror.LEGACY_CODEC` = legacyEnum → **대문자** `"NONE"`/`"LEFT_RIGHT"`/`"FRONT_BACK"`. 항상 |
| 11 | `VerticalPlacement` | String | `VerticalPlacement.CODEC` = `StringRepresentable.fromEnum` → **소문자 serialized name** (`"on_land_surface"`, `"partly_buried"`, `"on_ocean_floor"`, `"in_mountain"`, `"underground"`, `"in_nether"`). 항상 |
| 12 | `Properties` | Compound | 항상 |

`Properties.CODEC` (L332–346, RecordCodecBuilder — 전부 무조건 `fieldOf`, 생략 없음) encode 순서:
`cold`(Byte) → `mossiness`(Float) → `air_pocket`(Byte) → `overgrown`(Byte) → `vines`(Byte) → `replace_with_blackstone`(Byte).
실측 정합 (해시 순서로 관측됨).

### 3.5 TemplateStructurePiece 공통 — `task14-decomp/TemplateStructurePiece.java` L73–78

super(id/BB/O/GD) 뒤 삽입 순서:

| # | key | type |
|---|-----|------|
| 5 | `TPX` | Int (templatePosition.x) |
| 6 | `TPY` | Int |
| 7 | `TPZ` | Int |
| 8 | `Template` | String (`templateName`; 생성 시 `templateLocation.toString()` 예: `"minecraft:shipwreck/rightsideup_backhalf_degraded"`) |

`placeSettings` 자체는 저장하지 않음 — 로드 시 서브클래스가 tag 의 Rot/Mirror/... 로 `makeSettings(...)` 재구성 (`TemplateStructurePiece(type, tag, manager, Function<Identifier,StructurePlaceSettings>)` L52–66; boundingBox 도 저장된 BB 를 쓰지 않고 `template.getBoundingBox(placeSettings, templatePosition)` 로 재계산).

---

## 4. block_entities 직렬화

### 4.1 리스트 생성 경로 — `SerializableChunkData.copyOf` (L355–362)

```java
List<CompoundTag> blockEntities = new ArrayList<>(chunk.getBlockEntitiesPos().size());
for (BlockPos blockPos : chunk.getBlockEntitiesPos()) {
    CompoundTag t = chunk.getBlockEntityNbtForSaving(blockPos, level.registryAccess());
    if (t != null) blockEntities.add(t);
}
```

- **리스트 순서** = `ChunkAccess.getBlockEntitiesPos()` (L161–165):
  `Sets.newHashSet(pendingBlockEntities.keySet())` 후 `addAll(blockEntities.keySet())` —
  **매 저장마다 새로 만드는 `java.util.HashSet<BlockPos>` 의 반복 순서**.
  `Vec3i.hashCode() = (y + z*31)*31 + x` (javap 확인). 초기 capacity 는 `new HashSet<>(collection)` 규칙
  (`max(16, (int)(pending.size()/0.75f)+1)`), 이후 addAll 로 리사이즈 가능.
- 맵 구현: `ChunkAccess.java` L81–82 — `pendingBlockEntities = Maps.newHashMap()` (**HashMap\<BlockPos,CompoundTag\>**),
  `blockEntities = new Object2ObjectOpenHashMap()` (**fastutil**). 단, 저장 순서에는 위 HashSet 만 관여.
- **LevelChunk (full) 의 getBlockEntityNbtForSaving** (`LevelChunk.java` L467–483):
  라이브 BE → `blockEntity.saveWithFullMetadata(registryAccess)` 후 **`result.putBoolean("keepPacked", false)` 를 마지막에 추가**;
  라이브가 없고 pending 만 있으면 → pending tag **copy 후 `keepPacked=true` 추가**.
  주의: `this.getBlockEntity(blockPos)` 호출이 pending 승격(promote) 부수효과를 가질 수 있음(4.5).
- **ProtoChunk 의 getBlockEntityNbtForSaving** (`ProtoChunk.java` L293–296):
  라이브 BE → `saveWithFullMetadata` (**keepPacked 없음**); 아니면 pending tag **원본 그대로** (DUMMY 포함, copy 없음).
- 파싱 측 (`SerializableChunkData.parse` L142): `block_entities` 는 CompoundTag 리스트로만 보관;
  full 청크 로드 시 `postLoadChunk` (L514–527) 가 `keepPacked=true` → `setBlockEntityNbt`(pending 유지),
  아니면 `BlockEntity.loadStatic` → `setBlockEntity`.

### 4.2 BlockEntity 공통 골격 — `task14-decomp/BlockEntity.java`

26.2 는 `ValueOutput`(TagValueOutput) 기반. `saveWithFullMetadata(ValueOutput)` (L138–141):

```
saveWithoutMetadata(output);   // = saveAdditional(output); output.store("components", DataComponentMap.CODEC, this.components);
saveMetadata(output);          // = saveId(output);  putInt x; putInt y; putInt z;
```

**컴파운드 삽입 순서**: [서브클래스 `saveAdditional` 필드들…] → `components`(Compound) → `id`(String, `BuiltInRegistries.BLOCK_ENTITY_TYPE.byNameCodec()`) → `x`(Int) → `y`(Int) → `z`(Int) → (LevelChunk 저장이면) `keepPacked`(Byte).

- **`components` 는 빈 맵이어도 항상 기록된다** — `saveWithoutMetadata` (L170–173) 는 무조건
  `output.store("components", DataComponentMap.CODEC, ...)`; `DataComponentMap.CODEC`(dispatchedMap 계열)은 빈 맵을
  빈 컴파운드 `{}` 로 성공 인코딩하고 `TagValueOutput.store` 는 성공 시 무조건 put (javap 로 saveWithoutMetadata 바이트코드 확인, 실측 골든 전 BE 에 `components:{}` 존재). ※ isEmpty 생략하던 과거 버전과 다름.
- `id`/`x`/`y`/`z` 는 **뒤쪽**이다 (앞이 아님).

### 4.3 각 BE 타입 (saveAdditional 삽입 순서; ⊕ = 그 뒤 공통 components/id/x/y/z/keepPacked)

**RandomizableContainer 공통 규칙** (`RandomizableContainer.java` L58–71 trySaveLootTable):
`lootTable != null` 이면 `LootTable`(String, `LootTable.KEY_CODEC` = Identifier) 기록, `LootTableSeed`(Long)는 **seed != 0 일 때만** 기록하고 **true 반환 → Items 저장 전체 생략** (상호배타).
lootTable == null 이면 false → `ContainerHelper.saveAllItems(output, items)` (`ContainerHelper.java` L22–39):
`alsoWhenEmpty=true` 라 **`Items` 리스트는 빈 인벤토리라도 항상 기록**; 엔트리는 빈 슬롯 제외, 슬롯 인덱스 오름차순.
엔트리 = `ItemStackWithSlot.CODEC`: `Slot`(Byte, optionalAlwaysPresent → **항상 기록**) → `ItemStack.MAP_CODEC` merge: `id`(String) → `count`(Int, optionalAlwaysPresentFieldOf → **count=1 도 항상 기록**) → `components`(**빈 패치면 생략**).

- **ChestBlockEntity** (`ChestBlockEntity.java` L101–106): `super.saveAdditional`(= BaseContainerBlockEntity: `lock`(NO_LOCK 이면 생략, `LockCode.addToTag`) → `CustomName`(storeNullable, null 생략)) → [LootTable/LootTableSeed | Items]. ⊕
  실측(loot): `{LootTable, components, keepPacked, x, y, z, id, LootTableSeed}`.
- **BarrelBlockEntity** (`BarrelBlockEntity.java` L69–74): Chest 와 동일 패턴. 실측(no-loot): `Items` 빈 리스트 존재.
- **DispenserBlockEntity** (`DispenserBlockEntity.java` L89–94): 동일 패턴 (RandomizableContainerBlockEntity 계열).
- **HopperBlockEntity** (`HopperBlockEntity.java` L61–68): [LootTable/LootTableSeed | Items] → **`TransferCooldown`(Int, 항상, 기본 -1 이나 worldgen 배치시 0)**. 삽입 순서상 TransferCooldown 이 Items 뒤.
- **SpawnerBlockEntity** (`SpawnerBlockEntity.java` L51–54) → `BaseSpawner.save` (`task14-decomp-extra/BaseSpawner.java` L303–313) 삽입 순서:
  `Delay`(Short) → `MinSpawnDelay`(Short) → `MaxSpawnDelay`(Short) → `SpawnCount`(Short) → `MaxNearbyEntities`(Short) → `RequiredPlayerRange`(Short) → `SpawnRange`(Short) → `SpawnData`(Compound, **storeNullable: nextSpawnData null 이면 생략**) → `SpawnPotentials`(List, **항상 — 빈 WeightedList 는 빈 리스트**).
  기본값(필드 초기값, L49–58): Delay 20, Min 200, Max 800, Count 4, MaxNearby 6, ReqRange 16, Range 4 — **기본값이어도 전부 기록** (생략 규칙 없음).
  `SpawnData.CODEC` (`SpawnData.java` L14–22): `entity`(Compound, 항상) → `custom_spawn_rules`(optional 생략) → `equipment`(optional 생략). `SpawnPotentials` 엔트리 = `WeightedList.codec`: `{data:<SpawnData>, weight:Int}`.
  실측(dungeon): SpawnData=`{entity:{id:"minecraft:cave_spider"}}`, SpawnPotentials 빈 리스트, 나머지 기본값 그대로.
- **TrialSpawnerBlockEntity** (`TrialSpawnerBlockEntity.java` L45–48) → `TrialSpawner.store` (`trialspawner/TrialSpawner.java` L103–105):
  ① `output.store(TrialSpawnerStateData.Packed.MAP_CODEC, data.pack())` merge — 필드 (`TrialSpawnerStateData.java` L318–329, 전부 lenientOptionalFieldOf → **기본값이면 생략**): `registered_players`(List<IntArray[4]>, def ∅) → `current_mobs`(def ∅) → `cooldown_ends_at`(Long, def 0) → `next_mob_spawns_at`(Long, def 0) → `total_mobs_spawned`(Int, def 0) → `spawn_data`(Optional) → `ejecting_loot_table`(Optional).
  ② `output.store(TrialSpawner.FullConfig.MAP_CODEC, config)` merge — (`TrialSpawner.java` L462–469): `normal_config` → `ominous_config` (`TrialSpawnerConfig.CODEC` = RegistryFileCodec: **레지스트리 참조 홀더면 키 String**, 인라인이면 컴파운드; default(`Holder.direct(DEFAULT)`) 와 같으면 생략) → `target_cooldown_length`(Int, def 36000 생략) → `required_player_range`(Int, def 14 생략). ⊕
  실측(trial chamber worldgen 직후): `normal_config`/`ominous_config` String 둘만 존재 — state data 전부 기본값 생략, cooldown/range 기본값 생략.
- **VaultBlockEntity** (`vault/VaultBlockEntity.java` L66–71) 삽입 순서: `config` → `shared_data` → `server_data` (모두 `store(String, Codec, ...)` — **항상 기록**, 빈 컴파운드 가능).
  `VaultConfig.CODEC` (`vault/VaultConfig.java` L26–37, 전부 lenientOptional → 기본값 생략): `loot_table`(String, def `chests/trial_chambers/reward`) → `activation_range`(Double, def 4.0) → `deactivation_range`(Double, def 4.5) → `key_item`(Compound, `ItemStack.lenientOptionalFieldOf` — **EMPTY 생략**; 기본 비교 아님: 항상 기록됨에 주의 — DEFAULT 의 key_item(trial_key)과 같아도 Optional 로는 non-empty라 기록) → `override_loot_table_to_display`(Optional 생략).
  `VaultServerData.CODEC` (L20–28): `rewarded_players`(def ∅) → `state_updating_resumes_at`(Long def 0) → `items_to_eject`(def ∅) → `total_ejections_needed`(Int def 0) — 전부 기본값 생략 → **worldgen 직후엔 `server_data:{}`**.
  `VaultSharedData.CODEC` (L16–25): `display_item`(EMPTY 생략) → `connected_players`(def ∅) → `connected_particles_range`(Double def 4.5) — worldgen 직후 **`shared_data:{}`**. ⊕
  실측: `config:{key_item:{count:1, id:"minecraft:trial_key"}}`, `server_data:{}`, `shared_data:{}` — 정합.
- **DecoratedPotBlockEntity** (`DecoratedPotBlockEntity.java` L46–55): `sherds`(List\<String\> — `PotDecorations.CODEC`: `ordered()` = [back,left,right,front] **항상 4개**, 빈 슬롯은 `minecraft:brick` 으로 채움(`PotDecorations.java` L56–58); **decorations == EMPTY 이면 sherds 키 자체 생략**) → [trySaveLootTable → LootTable/LootTableSeed | `item`(Compound, ItemStack.CODEC, **비어있으면 생략**)]. ⊕
  실측: `{LootTable, components, keepPacked, sherds:[4 String], x, y, z, id, LootTableSeed}`.
- **BrushableBlockEntity** (`BrushableBlockEntity.java` L221–226): [trySaveLootTable → `LootTable`/`LootTableSeed` | `item`(비어있으면 생략)] 뿐. **`hit_direction` 은 26.2 에서 저장 안 함** (transient). ⊕
  실측: `{LootTable, components, keepPacked, x, y, z, id, LootTableSeed}`.

### 4.4 features 스테이지에서 BE 가 생기는 경로

`ProtoChunk.setBlockState` (`ProtoChunk.java` L115–170) 는 **BE 를 만들지 않는다** (섹션/하이트맵/라이트만).
BE 생성은 **`WorldGenRegion`** (`task14-decomp-extra/WorldGenRegion.java`):

- `WorldGenRegion.setBlock` (L288–327): `chunk.setBlockState` 후, `blockState.hasBlockEntity()` 이고
  청크가 **PROTOCHUNK** 타입이면 → **DUMMY pending NBT** 를 넣는다:
  ```java
  CompoundTag tag = new CompoundTag();
  tag.putInt("x", ...); tag.putInt("y", ...); tag.putInt("z", ...);
  tag.putString("id", "DUMMY");
  chunk.setBlockEntityNbt(tag);   // ChunkAccess L322-327: blockEntities 에 없을 때만 pendingBlockEntities.put
  ```
  (LEVELCHUNK 타입이면 즉시 `newBlockEntity` → `setBlockEntity`.) 이전 상태가 BE 블록이었는데 새 상태가 아니면 `removeBlockEntity`.
- `WorldGenRegion.getBlockEntity` (L196–227): 라이브 BE 없으면 pending tag 확인 —
  `"DUMMY"` 면 `((EntityBlock)state.getBlock()).newBlockEntity(pos, state)` 로 **fresh 생성**,
  아니면 `BlockEntity.loadStatic`; 성공 시 `chunk.setBlockEntity(be)` — `ProtoChunk.setBlockEntity` (L173–176) 는
  `pendingBlockEntities.remove(pos)` 후 `blockEntities.put(pos, be)` (Object2ObjectOpenHashMap).
  → 구조물/피처가 loot table 을 세팅(`RandomizableContainer.setBlockEntityLootTable` L44–49: `getBlockEntity` 후 `setLootTable(lootTable, random.nextLong())`)하는 순간 라이브 BE 로 승격된다. 손대지 않은 BE (예: 그냥 놓인 furnace)는 DUMMY pending 으로 남는다.

### 4.5 full 승격과 최종 순서 결정

- `ChunkStatusTasks.full` (`status/ChunkStatusTasks.java` L206–249): `new LevelChunk(level, protoChunk, postLoad)` →
  `runPostLoad()` → `setLoaded(true)` → `registerAllBlockEntitiesAfterLevelLoad()`.
- `LevelChunk(ServerLevel, ProtoChunk, ...)` (`LevelChunk.java` L130–171):
  `protoChunk.getBlockEntities().values()` 를 **Object2ObjectOpenHashMap 순회 순서**로 `setBlockEntity` (→ LevelChunk 의 `blockEntities`(역시 ChunkAccess 의 Object2ObjectOpenHashMap)에 put),
  이어 `this.pendingBlockEntities.putAll(protoChunk.getBlockEntityNbts())` (DUMMY 들 이관). 중복 키는 에러 로그만.
- pending → 라이브 실체화는 **`LevelChunk.postProcessGeneration`** (L588–630) 끝부분:
  `ImmutableList.copyOf(this.pendingBlockEntities.keySet())` (**HashMap\<BlockPos\> 반복 순서**) 순회하며
  `getBlockEntity(pos)` → `promotePendingBlockEntity` (L634–660): DUMMY 는 `newBlockEntity`, 아니면 `loadStatic`,
  성공 시 `addAndRegisterBlockEntity`; 마지막에 `pendingBlockEntities.clear()`.
  (또한 `LevelChunk.getBlockEntity(pos, CHECK)` (L379–406) 자체가 pending 을 lazy 승격시키므로 저장 시점 조회로도 승격될 수 있음.)
- **저장되는 최종 리스트 순서는 맵 순회 순서가 아니라**, 4.1 의 `getBlockEntitiesPos()` 가 만드는
  **일회용 `java.util.HashSet<BlockPos>` 의 반복 순서**가 유일한 결정자다
  (pending 키들 먼저 넣고 blockEntities 키 addAll — full 정착 후엔 pending 이 비므로 사실상 라이브 키만).

---

## 부록 — 실측 골든 요약 (r.0.0.captured.mca, DataVersion 4903)

- BE 컴파운드 실측 키(해시 순) 예: mob_spawner
  `components, MaxNearbyEntities, RequiredPlayerRange, SpawnCount, SpawnData, MaxSpawnDelay, Delay, keepPacked, x, y, z, id, SpawnRange, MinSpawnDelay, SpawnPotentials`
- structures 실측: start 키 `['references','ChunkZ','id','Children','ChunkX']`; References 값 LongArray; 빈 starts 도 `starts:{}` 존재.
- trial_chambers 청크(16,14): Children 258개, jigsaw child 키에 `liquid_settings:"ignore_waterlogging"` 존재, `rotation:"NONE"`(대문자), junctions 5키 전부 존재.
- 대문자 legacy enum 실증: `Rot:"CLOCKWISE_90"`, `BiomeType:"WARM"`, `Rotation/Mirror:"NONE"`; 소문자 StringRepresentable 실증: `VerticalPlacement:"on_ocean_floor"`, `projection:"rigid"`, `dest_proj:"rigid"`.

## hyperchunk 구현 시 주의 포인트 (요약)

1. CompoundTag=HashMap 재현: 각 컴파운드의 **정확한 put 시퀀스**(위 표)가 충돌·리사이즈를 결정.
2. `components:{}` 는 모든 BE 에 **항상** 존재 (26.2 변경점).
3. `id/x/y/z` 는 BE 컴파운드의 **뒤**, `keepPacked` 는 full(LevelChunk) 저장에서만 그 **맨 뒤**.
4. LootTable ↔ Items 상호배타; `LootTableSeed` 는 seed==0 생략; no-loot 컨테이너는 빈 `Items` 리스트도 기록.
5. `pool_element` 는 필드 먼저·`element_type` 마지막 (DFU 10.0.21 KeyDispatchCodec).
6. legacy enum(Rot/Rotation/Mirror/BiomeType)은 대문자 name(), StringRepresentable(projection/dest_proj/VerticalPlacement/liquid_settings)은 소문자.
7. block_entities 리스트 순서 = fresh `HashSet<BlockPos>`(pending 먼저, live addAll) 반복 순서; `Vec3i.hashCode=(y+z*31)*31+x`.
8. References LongArray = `LongOpenHashSet.toLongArray()` (해시테이블 순서), 빈 셋 엔트리 생략, `starts`/`References` 키는 항상 존재.
