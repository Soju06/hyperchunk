# R: 구조물 블록 배치 (placeInChunk → placeInWorld) — 26.2 비트단위 시맨틱 (2026-08-04)

조사 전용 리포트. 소스 = vineflower 디컴파일 출력 `tools/golden/work/task14-decomp/`
(이하 경로는 이 디렉토리 상대). 이번 태스크에서 추가 디컴파일:
`structures/` (ShipwreckPieces, OceanRuinPieces, RuinedPortalPiece/Structure 등),
`pools/` (SinglePoolElement, JigsawPlacement 등), `structroot/` (Structure, StructurePiece),
`pieces/` (StructurePiecesBuilder, PiecesContainer),
`misc/` (WorldGenRegion, Block, Util, Mth), `misc2/` (BlockBehaviour, ProtoChunk),
`misc3/` (GenerationStep, WorldgenRandom, RandomSource, Mth),
`misc4/` (LegacyRandomSource, SingleThreadedRandomSource, XoroshiroRandomSource,
BitRandomSource, BlockPos, LevelAccessor), `misc5/` (MappedRegistry,
RegistryDataLoader, ResourceManagerRegistryLoadTask, FileToIdConverter),
`misc6/` (Identifier, Direction, Rotation), `misc7/` (RandomState).
datapack JSON = `unzip -p tools/golden/libs/extracted/server-26.2.jar data/minecraft/worldgen/...`.
RNG 프리미티브 자체(LCG/드로우 수)는 R-mineshaft-dungeon-placement.md §0 과 동일 — 여기서는 차이점만.

## 0. RNG 기반 — 배치 단계 특이사항

- **데코레이션 랜덤은 Xoroshiro 백킹**: `applyBiomeDecoration` 이
  `new WorldgenRandom(new XoroshiroRandomSource(RandomSupport.generateUniqueSeed()))`
  생성 (ChunkGenerator.java:348) — 초기 시드는 즉시 `setDecorationSeed` 로 덮임.
  `WorldgenRandom.next(bits)` = 백킹이 Legacy 가 아니면
  `(int)(randomSource.nextLong() >>> 64-bits)` (WorldgenRandom.java:26-31) —
  **next() 1회 = xoroshiro nextLong 1회**. 따라서 (BitRandomSource 디폴트 기준)
  nextFloat/nextInt(2^k)=xoroshiro 1드로우, nextLong/nextDouble=2드로우,
  nextInt(비2^k)=1드로우+리젝션 재드로우 가능.
  `setSeed` 는 `XoroshiroRandomSource.setSeed` → `Xoroshiro128PlusPlus(upgradeSeedTo128bit(seed))`
  재구성 (XoroshiroRandomSource.java:43-46).
- `setDecorationSeed(worldSeed, minBlockX, minBlockZ)`: setSeed(worldSeed);
  a=nextLong()|1; b=nextLong()|1; deco = minBlockX*a + minBlockZ*b ^ worldSeed;
  setSeed(deco); return deco (misc3/WorldgenRandom.java:39-46).
- `setFeatureSeed(deco, index, step)`: setSeed(deco + index + 10000*step) (:48-51). 드로우 없음.
- 구조물 **스타트** 랜덤 (참고): `Structure.GenerationContext` 가
  `new WorldgenRandom(new LegacyRandomSource(0))` + `setLargeFeatureSeed(seed,cx,cz)`
  (structroot/Structure.java:265-269) — 이쪽은 **Legacy LCG**.
- 위치 시드 해시 `Mth.getSeed(x,y,z)` = `s = x*3129871 ^ z*116129781L ^ y;
  s = s*s*42317861 + s*11; return s >> 16` (misc3/Mth.java:332-336).
- `RandomSource.create(seed)` = **LegacyRandomSource** (misc3/RandomSource.java:23-25).
  `createThreadLocalInstance(seed)` = SingleThreadedRandomSource (동일 LCG, 비원자).
- `LegacyRandomSource.forkPositional()` = nextLong 1드로우 →
  `LegacyPositionalRandomFactory(seed)`; `.at(x,y,z)` =
  `new LegacyRandomSource(Mth.getSeed(x,y,z) ^ factorySeed)` (misc4/LegacyRandomSource.java:52-64).
- `Util.shuffle(list, r)` / `toShuffledList(IntStream, r)`: Fisher-Yates 하강
  `for i=n..2: swap(i-1, r.nextInt(i))` — **n-1 드로우** (misc/Util.java:967-977, 991-998).
- `Direction.Plane.HORIZONTAL.getRandomDirection(r)` = faces[nextInt(4)],
  faces 순서 **NORTH, EAST, SOUTH, WEST** (misc6/Direction.java:425-431, 441-443).
  `Rotation.getRandom(r)` = nextInt(4), 순서 NONE, CW_90, CW_180, CCW_90 (misc6/Rotation.java:117).
- `BitRandomSource.DOUBLE_MULTIPLIER`: 디컴파일 표기는 float 리터럴이지만 바이트코드
  상수는 정확히 `1.1102230246251565E-16` (=2^-53) — javap 로 확인.

## 1. ChunkGenerator.applyBiomeDecoration (ChunkGenerator.java:339-448)

의사코드 (필드/로컬명은 디컴파일 그대로):

```
if SharedConstants.debugVoidTerrain(centerPos): return
sectionPos = SectionPos.of(centerPos, level.getMinSectionY()); origin = sectionPos.origin()
structuresByStep = STRUCTURE 레지스트리.stream().collect(groupingBy(s -> s.step().ordinal()))
                   // 스트림 순서 = 레지스트리 id 순 (아래 §6) → 각 그룹 리스트도 그 순서 유지
featureList = this.featuresPerStep.get()
random = new WorldgenRandom(new XoroshiroRandomSource(uniqueSeed))
decorationSeed = random.setDecorationSeed(level.getSeed(), origin.getX(), origin.getZ())
possibleBiomes = (center±1 3x3 청크의 모든 섹션 바이옴) ∩ biomeSource.possibleBiomes()
generationSteps = max(Decoration.values().length /*=11*/, featureList.size())
for stepIndex in 0..generationSteps-1:
    index = 0                                     // ← 구조물 전용 카운터, 스텝마다 0 리셋
    if structureManager.shouldGenerateStructures():          // worldOptions.generateStructures
        for structure in structuresByStep.getOrDefault(stepIndex, []):
            random.setFeatureSeed(decorationSeed, index, stepIndex)   // 구조물당 1회
            structureManager.startsForStructure(sectionPos, structure)
                .forEach(start -> start.placeInChunk(level, sm, this, random,
                                                     getWritableArea(chunk), centerPos))
            index++                                // 스타트가 0개여도 증가
    if stepIndex < featureList.size():
        possibleFeaturesThisStep = { indexMapping(f) | f ∈ 각 바이옴의 features()[stepIndex] }
        indexArray = sort(possibleFeaturesThisStep)          // 오름차순
        for globalIndexOfFeature in indexArray:
            random.setFeatureSeed(decorationSeed, globalIndexOfFeature, stepIndex)
            feature.placeWithBiomeCheck(level, this, random, origin)
```

핀 포인트:

- **구조물 인덱스와 feature 인덱스는 완전 분리.** 구조물은 스텝 내 0,1,2,…
  (레지스트리 순서), feature 는 `featuresPerStep` 의 전역 인덱스
  (`stepFeatureData.indexMapping`). 서로 영향 없음.
- 각 스텝에서 **구조물이 feature 보다 먼저**.
- setFeatureSeed 는 **구조물 단위** 1회 — 같은 구조물의 여러 스타트, 스타트 내
  모든 피스, afterPlace 까지 **동일 WorldgenRandom 스트림을 이어서** 소비.
- `getWritableArea(chunk)` (:440-448): 스타트마다 forEach 람다 안에서 **새 BB 생성**
  — x/z = 센터 청크 16×16, y = [heightAccessorForGeneration.getMinY()+1, getMaxY()].
  (RuinedPortal 이 이 BB 를 encapsulate 로 변형해도 다음 스타트에 영향 없음.)
- `startsForStructure(sectionPos, structure)` (StructureManager.java:65-72):
  센터 청크를 **STRUCTURE_REFERENCES** 단계로 얻어 `getReferencesForStructure(structure)`
  — 구조물별 `LongSet` (packed ChunkPos of start-chunks; ChunkAccess.java:238-246,
  `LongOpenHashSet`). 각 ref 마다 (fillStartsForStructure :74-90) start 청크를
  **STRUCTURE_STARTS** 단계로 얻고 `getStartForStructure` → valid 만 수집.
  같은 구조물 스타트가 여러 개면 순회 순서 = **LongOpenHashSet 반복 순서**
  (fastutil 해시 배치; C 재현시 동일 해시/삽입 순서 필요).

## 2. StructureStart.placeInChunk (StructureStart.java:91-113)

```
pieces = pieceContainer.pieces()            // 조립 순서 그대로 (직렬화 "Children" 순서)
if pieces.isEmpty(): return
centerBB = pieces[0].boundingBox
referencePos = BlockPos(centerBB.center.x, centerBB.minY, centerBB.center.z)
for piece in pieces:                         // 리스트 순서
    if piece.getBoundingBox().intersects(chunkBB):     // 피스 자체 BB, inclusive 교차
        piece.postProcess(level, sm, generator, random, chunkBB, chunkPos, referencePos)
structure.afterPlace(level, sm, generator, random, chunkBB, chunkPos, pieceContainer)
```

- **random = setFeatureSeed 된 그 WorldgenRandom 인스턴스가 모든 피스에 공유.**
- referencePos = **첫 피스 BB 중심의 (x,z), y=minY** — 프로세서 체인의
  `referencePos` 파라미터로 그대로 전달됨 (AxisAlignedLinearPosTest 등이 사용).
- `StructureStart.getBoundingBox()` (:81-89) = pieces 전체 BB 를
  `structure.adjustBoundingBox` — terrain_adaptation != none 이면 **12 인플레이트**
  (structroot/Structure.java:87-89). 단 placeInChunk 의 피스 필터에는 **사용 안 함**.
- `afterPlace` 디폴트 no-op (structroot/Structure.java:153-162). 오버라이드는
  DesertPyramid / WoodlandMansion 뿐 — shipwreck/ocean_ruin/ruined_portal/trial_chambers
  는 전부 no-op.
- 청크 상태: 배치 중(FEATURES 생성) 센터 청크의 persistedStatus=CARVERS.
  `ProtoChunk.setBlockState` 는 매 호출마다 `persistedStatus.heightmapsAfter()` 의
  하이트맵만 갱신 (misc2/ProtoChunk.java:146-166) — CARVERS/FEATURES 부터는
  FINAL 4종 (OCEAN_FLOOR, WORLD_SURFACE, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES)
  (status/ChunkStatus.java:21-32). 즉 ***_WG 하이트맵은 카버 종료 시점에 동결*** —
  구조물의 WORLD_SURFACE_WG / OCEAN_FLOOR_WG 읽기는 같은 스텝에서 이미 놓인
  블록들의 영향을 받지 않는다. `WorldGenRegion.getHeight` =
  `chunk.getHeight(type, x&15, z&15) + 1` = heightmap.getFirstAvailable
  (misc/WorldGenRegion.java:404-408, ChunkAccess.java:191-203; "첫 빈 칸 y").

## 3. TemplateStructurePiece.postProcess → StructureTemplate.placeInWorld

### 3.1 TemplateStructurePiece.postProcess (TemplateStructurePiece.java:80-117)

1. `placeSettings.setBoundingBox(chunkBB)` — 피스 객체에 지속 (다음 청크에서 갱신됨).
2. `this.boundingBox = template.getBoundingBox(placeSettings, templatePosition)` 재계산.
3. `template.placeInWorld(level, templatePosition, referencePos, placeSettings, random, **2**)`.
4. 성공시 data marker 패스: `filterBlocks(templatePosition, placeSettings, STRUCTURE_BLOCK)`
   — nbt.mode == DATA 인 것만 `handleDataMarker(metadata, pos, level, random, chunkBB)`.
5. jigsaw 패스: `filterBlocks(..., JIGSAW)` — nbt.final_state 파싱(실패시 air) →
   `level.setBlock(pos, state, **3**)`.

`filterBlocks` (StructureTemplate.java:232-249): `settings.getRandomPalette(palettes, pos)`
로 팔레트 선택 → 그 팔레트의 `blocks(block)` 캐시 (blocks 리스트에서 타입 필터,
원 순서 유지) → 변환좌표가 `settings.getBoundingBox()`(=chunkBB) 안인 것만.
따라서 **마커/직소 처리도 현재 청크 BB 안의 것만, (Y,X,Z) 정렬 순서** (§3.2 참조).

### 3.2 팔레트와 블록 순회 순서 (StructureTemplate.java)

- **팔레트 선택 = RNG 드로우 1회**: `StructurePlaceSettings.getRandomPalette`
  (StructurePlaceSettings.java:140-147) = `palettes.get(getRandom(pos).nextInt(n))`.
  `getRandom(pos)` (:111-117): `settings.random` 이 설정돼 있으면 그것, 아니면
  `RandomSource.create(Mth.getSeed(pos))` — **26.2 바닐라 배치 경로에서 setRandom 을
  호출하는 곳 없음** (shipwreck/ocean ruin/ruined portal/pool element 모두) →
  전부 templatePosition-시드 일회용 Legacy 랜덤. 팔레트 1개여도 nextInt(1) 드로우는
  발생하지만 일회용이므로 공유 스트림 영향 없음. placeInWorld 와 filterBlocks 가
  각각 별도로 이 드로우를 한다 (매번 새 인스턴스, 같은 시드 → 같은 결과).
- **blocks() 순서 ≠ NBT 파일 순서**: 로드시 (load → loadPalette :755-775 →
  buildInfoList :168-184) 3버킷 분류 후 각 버킷을 `(Y,X,Z)` 콤퍼레이터로 정렬,
  `full + other + blockEntities` 순으로 연결:
  - blockEntities: `nbt != null` 인 블록,
  - full: nbt 없음 && `!hasDynamicShape()` && `isCollisionShapeFullBlock(EmptyBlockGetter, ZERO)`,
  - other: 나머지 (공기, 슬랩, 계단, 문 등).

### 3.3 processBlockInfos (StructureTemplate.java:459-503)

- `processOnlyInCurrentChunk = !any(processor.evaluatesEntirePieceState())` —
  오버라이드는 **CappedProcessor 만 true** (CappedProcessor.java:37-40; 그 외 전부
  디폴트 false, StructureProcessor.java:36-38).
- 블록 리스트 순서대로: worldPos = `transform(relPos, mirror, rot, pivot) + position`;
  processOnlyInCurrentChunk && chunkBB 있음 && 밖 → **프로세서 자체를 안 돌림**
  (RNG 포함 완전 스킵). 통과분은 nbt copy 후 프로세서 체인을 **settings 등록 순서**로,
  null 반환시 그 블록 탈락(체인 중단).
- 이후 모든 프로세서의 `finalizeProcessing` 을 등록 순서로 (CappedProcessor 만 유의미).

### 3.4 프로세서별 RNG (templatesystem/)

- **RuleProcessor** (RuleProcessor.java:26-47): 블록마다
  `RandomSource.create(Mth.getSeed(worldPos))` **자체 생성** — 공유 랜덤 아님!
  룰을 JSON 순서로: `rule.test = inputPredicate.test(state, random)
  && locPredicate.testAgainstWorldState(level, worldPos, random)
  && posPredicate.test(relPos, worldPos, referencePos, random)` — 단락 평가
  (ProcessorRule.java:54-65). 매치되면 `outputState` + `getOutputTag(random, nbt)`.
  - **RandomBlockMatchTest.test** (RandomBlockMatchTest.java:27-29):
    `state.is(block) && random.nextFloat() < probability` — **nextFloat 는 블록이
    매치될 때만 소비** (per-block 룰 랜덤에서).
  - **AppendLoot.apply** (rule/blockentity/AppendLoot.java): nbt 에 LootTable +
    `LootTableSeed = random.nextLong()` — 같은 per-block 룰 랜덤에서 2 next.
- **BlockRotProcessor**(integrity) (BlockRotProcessor.java:40-54):
  `random = settings.getRandom(pos)` → (setRandom 없음) **위치시드 일회용**.
  rottableBlocks 미설정 또는 매치시 `nextFloat() > integrity` 면 블록 탈락(null).
  블록당 1 nextFloat. ※ RuleProcessor 와 같은 위치시드 → **두 프로세서의 랜덤은
  같은 시퀀스를 각자 1번째 드로우부터 재생** (상관된 스트림, 바닐라 동작).
- **BlockAgeProcessor**(mossiness) (BlockAgeProcessor.java): `settings.getRandom(pos)`
  위치시드. 블록 타입별 드로우 (그 랜덤의 시퀀스 순서):
  - stone/stone_bricks/chiseled_stone_bricks (:65-77): ①nextFloat ≥0.5 → 교체 없음 (1드로우).
    <0.5 → ②nextInt(4)+③nextInt(2) (non-mossy 계단 방향/half),
    ④nextInt(4)+⑤nextInt(2) (mossy 계단), ⑥nextFloat<mossiness (배열 선택),
    ⑦nextInt(2) (원소 선택) = 총 7드로우.
  - STAIRS 태그 (:80-89): ①nextFloat ≥0.5 → 없음; <0.5 → ②nextFloat<mossiness,
    ③nextInt(2) = 3드로우.
  - SLABS/WALLS (:92-99): nextFloat<mossiness 1드로우.
  - OBSIDIAN (:102-104): nextFloat<0.15 (crying) 1드로우.
- **BlockIgnoreProcessor**: 리스트 포함 블록 탈락, RNG 없음.
  STRUCTURE_AND_AIR = [AIR, STRUCTURE_BLOCK], STRUCTURE_BLOCK = [STRUCTURE_BLOCK]
  (BlockIgnoreProcessor.java:21-23).
- **ProtectedBlockProcessor**: 월드의 기존 블록이 태그(#features_cannot_replace)면
  탈락. RNG 없음.
- **LavaSubmergedBlockProcessor**: 기존 월드 블록이 LAVA 이고 새 블록 셰이프가
  풀블록이 아니면 → LAVA 로 치환. RNG 없음.
- **BlackstoneReplaceProcessor**: 고정 매핑 + FACING/HALF/TYPE 복사. RNG 없음.
- **JigsawReplacementProcessor** (JigsawReplacementProcessor.java:25-55):
  JIGSAW → nbt.final_state 파싱; `structure_void` 면 **탈락(null)**, 파싱 실패도 null.
  nbt 제거된 새 info. RNG 없음.
- **GravityProcessor** (GravityProcessor.java:28-57): y = `level.getHeight(hm, x, z)
  + offset + templateRelativePos.getY()`. WG→비WG 하이트맵 스왑은
  `level instanceof ServerLevel` 일 때만 — 월드젠(WorldGenRegion)에서는 WG 유지. RNG 없음.
- **CappedProcessor** (CappedProcessor.java:42-88): evaluatesEntirePieceState=true.
  finalizeProcessing 에서:
  `random = RandomSource.createThreadLocalInstance(worldSeed).forkPositional().at(position)`
  — = LegacyRandomSource(seed₀=SingleThreaded(worldSeed).nextLong()) 의 positional,
  최종 시드 `Mth.getSeed(position) ^ factorySeed`, position = placeInWorld 의
  `position` 파라미터 (= templatePosition / piece position). **월드시드+위치 결정적,
  공유 스트림 아님.** 드로우: `limit.sample(random)` (ConstantInt → 0드로우),
  `Util.toShuffledList(0..n-1)` → **n-1 nextInt**, 이후 셔플 순서로
  delegate.processBlock (RuleProcessor → per-block 위치시드 랜덤) 를 교체 성공
  카운트가 limit 에 닿을 때까지.

### 3.5 placeInWorld 본체 (StructureTemplate.java:263-431)

순서대로 (updateMode = TemplateStructurePiece 경로 **2**, SinglePoolElement 경로 **18**):

1. 팔레트 선택 (§3.2, 드로우 1회 — 일회용 랜덤).
2. `processBlockInfos` (§3.3-3.4).
3. **배치 루프** (processed 리스트 순서):
   - `boundingBox == null || boundingBox.isInside(pos)` — chunkBB 게이트
     (CappedProcessor 있는 조합에서 리스트가 전 블록을 담고 있어도 여기서 잘림).
   - `previousFluidState = settings.shouldApplyWaterlogging() ? level.getFluidState(pos) : null`
     (LiquidSettings.APPLY_WATERLOGGING 일 때만; LiquidSettings.java).
   - `state = info.state.mirror(mirror).rotate(rotation)`.
   - **nbt 있으면 먼저 `setBlock(pos, BARRIER, 820)`** (기존 BE 안전 제거;
     820 = 4|16|32|256|512).
   - `level.setBlock(pos, state, updateMode)` 성공시:
     - placed 리스트에 (pos, nbt) 추가, min/max 갱신.
     - nbt 있고 BE 생성됐으면:
       - **BE instanceof RandomizableContainer →
         `nbt.putLong("LootTableSeed", random.nextLong())` — 공유 피처 랜덤에서
         nextLong 1회 (xoroshiro 2드로우).** 템플릿 nbt 에 LootTable 이 없어도
         무조건 소비. 해당 BE: Chest/TrappedChest, Barrel, Dispenser/Dropper,
         Hopper, ShulkerBox, Crafter, **DecoratedPot** (RandomizableContainerBlockEntity
         하위 + DecoratedPotBlockEntity; RandomizableContainer.java, 디컴파일 flat 트리).
         Vault/TrialSpawner/BrushableBlock 은 **아님**.
       - `blockEntity.loadWithComponents(nbt)` — 템플릿 nbt 컴파운드 그대로
         (주입된 LootTableSeed 포함) BE 에 로드.
     - **워터로깅 병합** (previousFluidState != null):
       - 새 state 의 fluidState 가 source → `lockedFluids.add(pos)`.
       - 아니고 블록이 LiquidBlockContainer → `placeLiquid(level, pos, state, prevFluid)`
         (물속 배치시 waterlogged 자동 true); prevFluid 가 source 아니면 `toFill.add`.
4. **유체 플러드필** (:337-365): 방향 배열 **[UP, NORTH, EAST, SOUTH, WEST]**;
   toFill 각 pos 의 이웃에서 source (lockedFluids 제외) 발견시 placeLiquid, 변화가
   없을 때까지 반복. RNG 없음.
5. 하나라도 놓였으면 (minX<=maxX):
   - `!settings.getKnownShape()` 일 때만: `updateShapeAtEdge` (:437-457) —
     placed 블록들로 만든 BitSetDiscreteVoxelShape 의 **경계면마다** 양측
     `state.updateShape(..., level.getRandom())` 후 변화시
     `setBlock(pos, newState, updateMode & -2)` (=2). ※ `level.getRandom()` =
     **WorldGenRegion.random** = `randomState.getOrCreateRandomFactory(
     "minecraft:worldgen_region_random").at(센터청크 월드좌표)` (misc/WorldGenRegion.java:88,
     misc7/RandomState.java:70-72) — 월드시드+센터청크 결정적, 공유 스트림과 별개.
     (바인/포도류 가지치기가 여기서 발생 — 기존 메모리 노트와 합치.)
   - placed 순서로: `!knownShape` 이면 `Block.updateFromNeighbourShapes(state, level, pos)`
     — 이웃 순서 **WEST, EAST, NORTH, SOUTH, DOWN, UP** (misc2/BlockBehaviour.java:90-92,
     misc/Block.java:199-210); 변화시 `setBlock(pos, newState, updateMode & -2 | 16)` (=**18**);
     `level.updateNeighborsAt(...)` 은 **LevelAccessor 디폴트 no-op** — WorldGenRegion
     미오버라이드 (misc4/LevelAccessor.java:60-61) → 월드젠 중 무효.
   - nbt 있던 자리는 `blockEntity.setChanged()`.
6. `!ignoreEntities` 면 placeEntities (:505-541): entityInfoList (템플릿 "entities"
   순서) — nbt 로 EntityType.create, `finalizeEntities && Mob` 일 때만 finalizeSpawn
   (레벨 랜덤 소비, 공유 스트림 아님), addFreshEntityWithPassengers.

**setBlock 플래그 / 마킹 / 하이트맵**:

- 플래그 상수 (misc/Block.java:89-109): 1=NEIGHBORS, 2=CLIENTS, 4=INVISIBLE,
  8=IMMEDIATE, **16=KNOWN_SHAPE**, 32=SUPPRESS_DROPS, 256=SKIP_BLOCK_ENTITY_SIDEEFFECTS,
  512=SKIP_ON_PLACE.
- 구조물 본배치: TemplateStructurePiece 경로 **flag 2**, SinglePoolElement 경로
  **flag 18**; 셰이프 보정 재배치 18; 경계 보정 2; jigsaw final_state / RuinedPortal
  후처리 / 바인·잎 **flag 3**; 마커 침대·체스트 등 개별 setBlock **flag 2**.
- `WorldGenRegion.setBlock` (misc/WorldGenRegion.java:284-325):
  `chunk.setBlockState(pos, state, flags)` 후 **(flags & 16) == 0 이고
  `state.getPostProcessPos(this, pos) != null`** 이면
  `chunk.markPosForPostProcessing(pos)`. PostProcess 는 블록 Properties 의 함수
  (디폴트 null; misc2/BlockBehaviour.java:848-850, 980-983, 1025). → flag 2/3 배치만
  마킹 후보, flag 18/820 은 절대 마킹 안 됨.
- 하이트맵: §2 말미 — ProtoChunk.setBlockState 가 FINAL 4종을 즉시 갱신,
  *_WG 는 동결.

## 4. 구조물별 배치 + postProcess 특이사항

### 4.1 Shipwreck (structures/ShipwreckPieces.java, ShipwreckStructure.java)

- **스타트** (Legacy 랜덤, 참고): `Rotation.getRandom` (nextInt(4)) →
  `Util.getRandom(템플릿 배열)` (nextInt(11) beached / nextInt(20) ocean;
  배열 순서는 ShipwreckPieces.java:34-68 리터럴 그대로), 위치 =
  (minBlockX, **90**, minBlockZ), 피벗 고정 **(4,0,15)**. 피스 1개, genDepth 0.
  `isTooBigToFitInWorldGenRegion()` = **sizeX>32 || sizeY>32** (X 와 Y! Z 아님 —
  javap 바이트코드 확인) 이면 스타트 시점에 높이 확정: beached →
  `getLowestY(4모서리 WORLD_SURFACE_WG)` 후 `calculateBeachedPosition` =
  `minY - sizeY/2 - random.nextInt(3)` (context.random); ocean → 4모서리 평균.
- **placement postProcess** (ShipwreckPieces.java:139-173): `!heightAdjusted &&
  !tooBig` 이면 — 템플릿 풋프린트 전 칼럼 `level.getHeight(isBeached ?
  WORLD_SURFACE_WG : OCEAN_FLOOR_WG)` 스캔으로 mean/minY (RNG 없음, 동결 하이트맵),
  `adjustPositionHeight(beached ? minY - sizeY/2 - **random.nextInt(3)** : mean)` —
  **beached 만 공유 피처 랜덤에서 nextInt(3) 1드로우**, `heightAdjusted` 래치로
  **최초 교차 청크에서 단 1회** (이후 청크·재로드시 NBT "height_adjusted" 로 스킵).
  이어 super.postProcess.
- placeSettings (makeSettings :122-128): rotation, Mirror.NONE, pivot(4,0,15),
  프로세서 = **[BlockIgnoreProcessor.STRUCTURE_AND_AIR]** 만. liquid 디폴트
  APPLY_WATERLOGGING → 물속 부분 waterlogged 병합 동작. knownShape=false.
- placeInWorld 공유 드로우: 배치된 RandomizableContainer BE (템플릿 체스트 등)
  마다 nextLong 1회 (§3.5).
- **data marker** (:130-137): map_chest/treasure_chest/supply_chest →
  `RandomizableContainer.setBlockEntityLootTable(level, random, **pos.below()**, loot)`
  — 아래 블록 BE 가 RandomizableContainer 일 때만 **nextLong 1회** (공유 랜덤).
  마커 순회는 (Y,X,Z) 정렬·chunkBB 필터 (§3.1/3.2).

### 4.2 Ocean ruins (structures/OceanRuinPieces.java, OceanRuinStructure.java)

- **스타트** (Legacy, 참고 — OceanRuinStructure.java:49-55 → OceanRuinPieces.addPieces
  :152-166): `Rotation.getRandom` → `nextFloat ≤ largeProbability` (isLarge) →
  addPiece: WARM = `Util.getRandom(8 또는 4 템플릿)` 1드로우, 피스 1개;
  COLD = `nextInt(8|4)` **인덱스 1드로우를 brick/cracked/mossy 3피스가 공유**
  (integrity 0.9|0.8 / 0.7 / 0.5, 같은 위치·회전 — 순차 덮어쓰기 설계).
  large 면 추가 `nextFloat ≤ clusterProbability` → addClusterRuins (:168-197):
  `allPositions` 8좌표 × 2 = **16 Mth.nextInt 드로우** (:199-210 식 그대로),
  `ruins = Mth.nextInt(4,8)` 1드로우, 각 i<ruins: `nextInt(remaining)` +
  `Rotation.getRandom` (**BB 교차로 버려져도 드로우는 소비**) + addPiece 드로우.
- **placement postProcess** (:360-380, **래치 없음 — 교차 청크마다 재실행**):
  ① `templatePosition.y = level.getHeight(OCEAN_FLOOR_WG, x, z)` (동결 하이트맵);
  ② `getHeight` 보정 스캔 (:382-415): 풋프린트 각 칼럼을 y-1 부터 아래로
  공기/물/얼음 스킵하며 바닥 탐색, `topY-minY>2 && area>width-2` 면 `y=minY+1`.
  RNG 없음, 하이트맵 동결이라 청크간 결과 동일. 이어 super.postProcess.
- placeSettings (:297-309): **[BlockRotProcessor(integrity),
  BlockIgnoreProcessor.STRUCTURE_AND_AIR, CappedProcessor(RuleProcessor([
  BlockMatchTest(SAND|GRAVEL) → SUSPICIOUS_SAND|GRAVEL 기본상태 + AppendLoot(
  OCEAN_RUIN_WARM|COLD_ARCHAEOLOGY)]), ConstantInt(5))]** (:56-61, 127-142).
  CappedProcessor 때문에 **evaluatesEntirePieceState → 전 템플릿 블록이 프로세싱**
  (BlockRot 드로우는 위치시드라 청크 불변; capped 선택도 월드시드+templatePosition
  결정적) — 배치는 §3.5 의 chunkBB 게이트가 자름 → 청크간 일관.
- suspicious sand/gravel = **BrushableBlockEntity → RandomizableContainer 아님** —
  placeInWorld 의 nextLong 없음; 시드는 AppendLoot (per-block 룰 랜덤) 에서.
- **data marker** (:334-358): "chest" → `setBlock(pos, CHEST[waterlogged =
  fluidState.is(WATER)], 2)`; BE 가 ChestBlockEntity 면 `setLootTable(
  UNDERWATER_RUIN_BIG|SMALL, **random.nextLong()**)` — 공유 랜덤 1드로우.
  "drowned" → Drowned 생성·`finalizeSpawn` (레벨/지역 랜덤 소비, 공유 스트림 아님)
  후 `setBlock(pos, y>seaLevel ? AIR : WATER, 2)`.

### 4.3 Ruined portal (structures/RuinedPortalPiece.java, RuinedPortalStructure.java)

- **스타트** (Legacy, 참고 — RuinedPortalStructure.java:78-174): setups>1 이면
  가중 nextFloat 1드로우; `sample(airPocketProbability)` — **확률 0/1 이면 드로우
  없음**, 아니면 nextFloat; giant 판정 nextFloat<0.05 → `nextInt(3)` giant /
  `nextInt(10)` normal; `Util.getRandom(Rotation.values())` nextInt(4);
  mirror = nextFloat<0.5 ? NONE : FRONT_BACK; findSuitableY (:188-249):
  IN_NETHER = airPocket ? RBI(32,100) : (nextFloat<0.5 ? RBI(27,29) : RBI(29,100))
  (2드로우), IN_MOUNTAIN/UNDERGROUND = getRandomWithinInterval (min<max 일 때만
  1드로우), PARTLY_BURIED = RBI(2,8) 1드로우, ON_LAND_SURFACE/ON_OCEAN_FLOOR = 0.
  (RBI = Mth.randomBetweenInclusive.) 피벗 = (sizeX/2, 0, sizeZ/2).
- **placement postProcess** (RuinedPortalPiece.java:174-201): **게이트 —
  `chunkBB.isInside(templateBB.getCenter())` 일 때만 전체 실행** (센터를 포함한
  청크가 단독으로 전 구조물 배치). `chunkBB.encapsulate(boundingBox)` 로
  **writable-area BB 를 템플릿 전체로 확장** (청크 경계 밖 = 리전 이웃 청크에도
  기록; getWritableArea 가 스타트별 새 BB 라 부작용 없음). 이후:
  1. `super.postProcess` (placeInWorld flag 2 + 마커(핸들러 no-op :203-206) +
     jigsaw 패스).
  2. `spreadNetherrack(random, level)` (:252-288) — **공유 랜덤**:
     ① `nextInt(max(1, 8 - (XSpan+ZSpan)/2/2))` 1드로우 (distanceAdjustment);
     ② 이중 루프 **x = cx-14..cx+14 (외측), z = cz-14..cz+14 (내측)** (cx,cz =
     this.boundingBox 중심; 14 = 확률표 길이): `adjustedDistance =
     max(0, |dx|+|dz| + adj) < 14` 인 후보마다 **nextDouble 1회 (2 next)** —
     배치 여부 무관하게 소비; 확률표
     {1,1,1,1,1,1,1,.9,.9,.8,.7,.6,.4,.2}[adjustedDistance] 통과시:
     surfaceY = `getHeight(placement 별 WG 하이트맵)-1`, y = followGroundSurface ?
     surfaceY : min(BB.minY, surfaceY); `|y-BB.minY|<=3 &&
     canBlockBeReplacedByNetherrackOrMagma` 면:
     `placeNetherrackOrMagma` — **`!cold && nextFloat<0.07` → magma, cold 면
     드로우 0** (:301-307, 단락평가!), setBlock flag 3;
     overgrown 시 `maybeAddLeavesAbove` — **nextFloat 1드로우 먼저**, 조건
     (NETHERRACK && 위 공기) 통과시 JUNGLE_LEAVES[persistent] flag 3 (:223-227);
     `addNetherrackDripColumn(pos.below())` (:240-250) —
     placeNetherrackOrMagma (0-1드로우) 후 `while(cap>0 && nextFloat<0.5)`:
     내려가며 placeNetherrackOrMagma, cap 8. (루프 조건 nextFloat 1 + !cold 시
     magma nextFloat 1 / 반복.)
  3. `addNetherrackDripColumnsBelowPortal` (:229-238): x=[minX+1,maxX-1] 외측,
     z=[minZ+1,maxZ-1] 내측, y=BB.minY 에 NETHERRACK 인 곳마다 drip column
     (위와 동일 드로우).
  4. vines||overgrown 시: `BlockPos.betweenClosedStream(this.getBoundingBox())` —
     **x 최속, y 중간, z 최외** 순회 (misc4/BlockPos.java:412-435); pos 마다
     vines 먼저: `maybeAddVines` (:208-221) — 상태가 !air && !vine 이면
     **getRandomHorizontalDirection = nextInt(4) 소비** (배치 가능 여부 무관),
     이웃이 air 이고 해당 면이 풀페이스면 VINE flag 3; 그 다음 overgrown:
     maybeAddLeavesAbove (nextFloat).
- placeSettings (makeSettings :130-161): **[BlockIgnore(airPocket ?
  STRUCTURE_BLOCK : STRUCTURE_AND_AIR), RuleProcessor(rules), BlockAgeProcessor(
  mossiness), ProtectedBlockProcessor(#features_cannot_replace),
  LavaSubmergedBlockProcessor, (replace_with_blackstone 시 +BlackstoneReplace)]**.
  rules 순서: ① GOLD_BLOCK --RandomBlockMatch(0.3)--> AIR;
  ② lava 룰 = ON_OCEAN_FLOOR → BlockMatch(LAVA)→MAGMA / cold →
  BlockMatch(LAVA)→NETHERRACK / 그 외 → RandomBlockMatch(LAVA,0.2)→MAGMA;
  ③ !cold 면 NETHERRACK --RandomBlockMatch(0.07)--> MAGMA (:140-145, 163-172).
  룰·에이지 랜덤 전부 per-block 위치시드 (§3.4) — **공유 스트림 소비는 위
  postProcess 후처리에서만**.

### 4.4 Trial chambers (PoolElementStructurePiece.java, pools/SinglePoolElement.java)

- step = **underground_structures**, terrain_adaptation = encapsulate (스타트 BB
  12 인플레이트에만 영향), liquid_settings = **"ignore_waterlogging"**,
  start_height uniform(-40..-20), start_pool trial_chambers/chamber/end
  (jar `worldgen/structure/trial_chambers.json`).
- **피스 순서** = 조립 순서: 센터 피스 먼저 (pools/JigsawPlacement.java:130),
  이후 배치 성공 순으로 append (:506; 큐는 placementPriority 의
  SequencedPriorityIterator :284, :511) — **GenDepth 로 재정렬하지 않음**
  (PoolElementStructurePiece 의 genDepth 는 항상 0; StructurePiecesBuilder /
  PiecesContainer 는 삽입 순서 보존, pieces/StructurePiecesBuilder.java:12-16, 58-60).
  placeInChunk 의 피스별 BB∩chunkBB 필터만 적용.
- `postProcess` → `place(..., keepJigsaws=false)` → `element.place`
  (PoolElementStructurePiece.java:90-126) → **SinglePoolElement.place**
  (SinglePoolElement.java:139-165):
  - settings (getSettings :167-185): BB=chunkBB, rotation, **knownShape=true**,
    ignoreEntities=false, **finalizeEntities=true**, liquidSettings =
    element.override_liquid_settings ?? 구조물 liquidSettings (trial chambers →
    IGNORE_WATERLOGGING → **워터로깅 병합/플러드필 전부 스킵**), 프로세서 =
    **[BlockIgnoreProcessor.STRUCTURE_BLOCK, JigsawReplacementProcessor,
    <element JSON "processors" 리스트 순서>, <projection 프로세서: RIGID=없음,
    TERRAIN_MATCHING=GravityProcessor(WORLD_SURFACE_WG,-1)
    (pools/StructureTemplatePool.java:128-130)>]**.
  - `template.placeInWorld(level, this.position, referencePos, settings, random, **18**)`
    — knownShape=true + flag 16 → **updateShapeAtEdge / updateFromNeighbourShapes /
    markPosForPostProcessing 전부 없음**.
  - **공유 랜덤 드로우 = 배치된 RandomizableContainer BE 당 nextLong 1회**
    (체스트·배럴·디스펜서·드로퍼·호퍼·크래프터·**장식 항아리** — trial chambers
    템플릿에 다수). Vault / TrialSpawner BE 는 해당 없음 — 템플릿 nbt 가
    loadWithComponents 로 그대로 로드.
  - **jigsaw 블록 최종 치환**: JigsawReplacementProcessor 가 **프로세싱 단계에서**
    final_state 로 교체 (structure_void → 블록 탈락). PoolElementStructurePiece 는
    TemplateStructurePiece 가 아니므로 별도 jigsaw 패스 없음.
  - **data marker**: `getDataMarkers(..., absolute=false)` (BB 없는 회전-only
    settings 로 filterBlocks — 팔레트 드로우는 위치시드 일회용) →
    `StructureTemplate.processBlockInfos(level, position, referencePos,
    실제 settings, 마커들)` (변환+chunkBB 필터+전체 프로세서 체인!) →
    `this.handleDataMarker(...)` — **StructurePoolElement 디폴트 no-op**
    (pools/StructurePoolElement.java:76-84), trial chambers 전용 오버라이드 없음.
- **datapack 프로세서** (jar): trial chambers 계열 processor_list 는
  `trial_chambers_copper_bulb_degradation` 하나 —
  RuleProcessor 룰 3개 (JSON 순서): waxed_copper_bulb
  --random_block_match 0.1--> waxed_oxidized_copper_bulb[lit=true,powered=false],
  --0.33333334--> waxed_weathered_..., --0.5--> waxed_exposed_...;
  + protected_blocks #features_cannot_replace. per-block 위치시드 랜덤에서
  **매치된 룰마다 nextFloat 순차 소비, 첫 통과 룰에서 중단** — 결과 분포
  10% / 30% / 30% / 30% 유지.
- pool_aliases (random_group/random, 스타트 시 PoolAliasLookup.create(poolAliases,
  startPos, seed) — 조립 시점 결정) 은 배치 단계와 무관.

## 5. RandomizableContainer.setLootTable (RandomizableContainer.java)

- `static setBlockEntityLootTable(BlockGetter, RandomSource, BlockPos, ResourceKey)`
  (:44-50): BE 가 RandomizableContainer 면
  `setLootTable(lootTable, **random.nextLong()**)` → `setLootTable(key)` +
  `setLootTableSeed(seed)` (BE 필드). **BE 가 아니면 드로우도 없음.**
- 저장: `trySaveLootTable` (:59-72) — nbt "LootTable" + "LootTableSeed"
  (seed==0 이면 키 생략). 로드: `tryLoadLootTable` (:52-57).
- 루팅 시점: `unpackLootTable` (:74-92) — `lootTable.fill(..., lootTableSeed)`,
  월드젠과 무관 (지연 평가).
- placeInWorld 경로 (§3.5) 는 setLootTable 을 거치지 않고 **템플릿 nbt 에
  "LootTableSeed" 를 직접 주입** 후 loadWithComponents — 템플릿에 "LootTable" 키가
  없어도 nextLong 은 소비된다는 점이 유일한 차이.

## 6. Structure step 정의 + 스텝 내 인덱스 회계

`Structure.step()` = StructureSettings.step = datapack JSON "step"
(structroot/Structure.java:79-81, :272-292). GenerationStep.Decoration 서수
(misc3/GenerationStep.java:8-18): 0 raw_generation, 1 lakes, 2 local_modifications,
**3 underground_structures**, **4 surface_structures**, 5 strongholds,
6 underground_ores, 7 underground_decoration, 8 fluid_springs,
9 vegetal_decoration, 10 top_layer_modification.

**레지스트리 순서** = 등록 순서 = **리소스 Identifier 정렬 순**
(misc5/ResourceManagerRegistryLoadTask.java:60 `sorted(Entry.comparingByKey())`;
Identifier.compareTo 는 **path 우선, namespace 차선** — misc6/Identifier.java:151-158;
MappedRegistry.stream = byId 순 — misc5/MappedRegistry.java:196, 222).
바닐라 단독 기준 (jar `worldgen/structure/*.json` 34개, step 확인 완료):

- **UNDERGROUND_STRUCTURES (step 3)**: 0 buried_treasure, **1 mineshaft**,
  2 mineshaft_mesa, 3 trail_ruins, **4 trial_chambers**.
- **SURFACE_STRUCTURES (step 4)**: 0 bastion_remnant, 1 desert_pyramid, 2 end_city,
  3 igloo, 4 jungle_pyramid, 5 mansion, 6 monument, **7 ocean_ruin_cold,
  8 ocean_ruin_warm**, 9 pillager_outpost, **10 ruined_portal, 11 ruined_portal_desert,
  12 ruined_portal_jungle, 13 ruined_portal_mountain, 14 ruined_portal_nether,
  15 ruined_portal_ocean, 16 ruined_portal_swamp, 17 shipwreck, 18 shipwreck_beached**,
  19 stronghold, 20 swamp_hut, 21 village_desert, 22 village_plains,
  23 village_savanna, 24 village_snowy, 25 village_taiga.
- **UNDERGROUND_DECORATION (step 7)**: 0 ancient_city, 1 fortress, 2 nether_fossil.
- terrain_adaptation: trial_chambers=encapsulate (그 외 조사 4종 none) —
  스타트 getBoundingBox 12 인플레이트 (§2).

⚠ 모드/데이터팩이 구조물을 추가하면 정렬 삽입으로 **뒤 인덱스가 전부 밀림**
(setFeatureSeed 의 index 가 달라짐) — C 구현은 레지스트리 스냅샷에서 인덱스를
계산해야 함.

## 7. C 구현 함정 체크리스트

1. **공유 스트림 소비 지점 총정리** (setFeatureSeed 후, 소비 순서 = 스타트 순 →
   피스 순 → 피스 내):
   - placeInWorld: 배치 성공한 RandomizableContainer BE 당 nextLong.
   - Shipwreck(beached, 크기 적합): 최초 배치 청크에서 nextInt(3) 1회.
   - 마커: shipwreck 체스트 마커 nextLong (BE 조건부), ocean ruin "chest" nextLong.
   - RuinedPortal postProcess: nextInt(…) 1회 + 후보당 nextDouble + 배치·기둥·잎·바인
     드로우 (§4.3 순서 엄수; cold 의 placeNetherrackOrMagma 는 무드로우).
   - 그 외 프로세서 RNG 는 전부 위치시드/월드시드 결정적 별도 스트림.
2. **위치시드 스트림은 프로세서마다 새 인스턴스** — RuleProcessor 와
   BlockRot/BlockAge 가 같은 pos 에서 같은 시퀀스를 각자 처음부터 재생 (상관).
3. 블록 순회는 (Y,X,Z) 정렬 3버킷 연결 순서 (§3.2) — NBT blocks 순서 아님.
   full 판정에 `isCollisionShapeFullBlock` 필요 (블록 셰이프 테이블).
4. chunkBB 사전 필터 (processOnlyInCurrentChunk) 가 CappedProcessor 존재 여부로
   토글 — ocean ruin 만 전-블록 프로세싱.
5. `nextInt(1)` 도 드로우 1회 (팔레트 1개일 때 포함) — 단 일회용 랜덤이라
   공유 스트림 무영향.
6. WorldgenRandom(Xoroshiro) 의 nextLong = 2 드로우, nextInt(비2^k) 리젝션 루프
   재현 필요.
7. LongOpenHashSet(references) / 조립 순서 / SequencedPriorityIterator 등
   컨테이너 순서가 피스·스타트 순서를 정의.
8. flag 시맨틱: 본배치 2(템플릿피스)/18(풀엘리먼트), 후처리 마킹은
   (flags&16)==0 && PostProcess 속성 블록만.
9. *_WG 하이트맵 동결 vs FINAL 4종 실시간 갱신 — Y 앵커·GravityProcessor 재현의 핵심.
10. updateShapeAtEdge/updateFromNeighbourShapes 는 템플릿피스 경로 전용,
    랜덤은 `worldgen_region_random` positional (월드시드+센터청크 결정적).
