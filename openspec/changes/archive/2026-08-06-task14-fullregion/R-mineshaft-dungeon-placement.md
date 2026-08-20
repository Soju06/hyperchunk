# R: Mineshaft 조립·배치 + Monster Room + 구조물 위치 결정 — 26.2 비트단위 시맨틱 (2026-08-04)

조사 전용 리포트. 소스 = vineflower 디컴파일 출력
`tools/golden/work/task14-decomp/` (이하 인용은 이 디렉토리 상대 경로 + 대략적 라인),
추가 디컴파일 산출물은 `/tmp/t14dec/` (BaseSpawner, UniformHeight, Rotation, Util, Mth,
ChunkPos, LegacyRandomSource, BitRandomSource, Direction, WeightedList,
MultiPackResourceManager, GenerationStep). datapack JSON =
`unzip -p tools/golden/libs/extracted/server-26.2.jar data/minecraft/worldgen/...`.

## 0. RNG 기반 (전 섹션 공통)

`WorldgenRandom extends LegacyRandomSource` (WorldgenRandom.java) — 내부는
java.util.Random 과 동일한 48bit LCG (LegacyRandomSource.java:36-44:
`seed = seed*25214903917 + 11 & (2^48-1)`, `next(bits) = (int)(newSeed >> 48-bits)`,
setSeed 는 `(seed ^ 25214903917) & mask`). 드로우 프리미티브 (BitRandomSource.java):

- `nextInt(bound)`: 2의 거듭제곱이면 `(bound * next(31)) >> 31` (1 next); 아니면
  모듈로-리젝션 루프 (보통 1 next, 드물게 재드로우) — JUR 과 동일.
- `nextLong()` = `((long)next(32) << 32) + next(32)` (2 next).
- `nextFloat()` = `next(24) * 5.9604645E-8F` (1 next).
- `nextDouble()` = `(((long)next(26) << 27) + next(27)) * DOUBLE_MULTIPLIER` (2 next).
  DOUBLE_MULTIPLIER 는 소스상 `1.110223E-16F` (float 리터럴) 이지만 그 float 값은
  정확히 2^-53 이므로 JUR nextDouble 과 비트 일치.
- `nextBoolean()` = `next(1) != 0`.

시드 파생 (WorldgenRandom.java:39-64):

- `setLargeFeatureSeed(seed, cx, cz)`: setSeed(seed); a=nextLong(); b=nextLong();
  setSeed(cx*a ^ cz*b ^ seed).
- `setLargeFeatureWithSalt(seed, x, z, salt)`: setSeed(x*341873128712 + z*132897987541
  + seed + salt). (드로우 없음)
- `setDecorationSeed(seed, blockX, blockZ)`: setSeed(seed); a=nextLong()|1;
  b=nextLong()|1; r = blockX*a + blockZ*b ^ seed; setSeed(r); return r.
- `setFeatureSeed(decoSeed, index, step)`: setSeed(decoSeed + index + 10000*step).

`Util.getRandom(arr, r)` = `arr[r.nextInt(len)]` (Util.java:795).
`Mth.randomBetweenInclusive(r, min, max)` = `r.nextInt(max-min+1)+min` (Mth.java:690).
`Mth.nextInt(r, min, max)` = min>=max ? min : 동일식 (Mth.java:146).
`Rotation.getRandom` = Util.getRandom(values(),r) → nextInt(4), 순서
NONE, CW_90, CW_180, CCW_90 (Rotation.java:114).
`Direction` ordinal: DOWN,UP,NORTH,SOUTH,WEST,EAST; `Plane.HORIZONTAL` 순회 순서 =
**NORTH, EAST, SOUTH, WEST** (Direction.java:415).
`ChunkPos.getBlockX(i)`=(cx<<4)+i, `getMiddleBlockX()`=getBlockX(8),
`getMinBlockX()`=cx<<4, `getWorldPosition()`=(minX,0,minZ),
`pack(x,z)` = `x&0xFFFFFFFFL | (z&0xFFFFFFFFL)<<32` (ChunkPos.java).

---

## C. 구조물 위치 결정 (placement calc)

### C.1 우리 리전 structure_set 파라미터 (JSON 그대로)

| set | placement | spacing | separation | salt | frequency | freq_reduction | spread |
|---|---|---|---|---|---|---|---|
| mineshafts | random_spread | 1 | 0 | 0 | 0.004 | legacy_type_3 | linear(기본) |
| trial_chambers | random_spread | 34 | 12 | 94251327 | 1.0(기본) | default(무발동) | linear |
| shipwrecks | random_spread | 24 | 4 | 165745295 | 1.0 | — | linear |
| ocean_ruins | random_spread | 20 | 8 | 14357621 | 1.0 | — | linear |
| ruined_portals | random_spread | 40 | 15 | 34222645 | 1.0 | — | linear |

exclusion_zone: 다섯 세트 모두 없음. structures 목록: mineshafts=[mineshaft,
mineshaft_mesa] (weight 1,1); shipwrecks=[shipwreck, shipwreck_beached];
ocean_ruins=[ocean_ruin_cold, ocean_ruin_warm]; ruined_portals=7종 (ruined_portal,
_desert, _jungle, _swamp, _mountain, _ocean, _nether — **이 JSON 배열 순서**, weight
전부 1); trial_chambers=[trial_chambers] 단일.

frequency 0.004 는 float 코덱 (`Codec.floatRange`) → **0.004F =
0.004000000189989805…** 로 double 승격되어 비교됨 (아래 C.2).

### C.2 isStructureChunk (StructurePlacement.java:80-129, RandomSpreadStructurePlacement.java:66-81)

`isStructureChunk(state, cx, cz)` = isPlacementChunk && applyAdditionalChunkRestrictions
&& applyInteractionsWithOtherStructures(exclusion; 우리 세트는 항상 true).

**getPotentialStructureChunk(seed, cx, cz)** (RandomSpread…:66-75):

```
gridX = floorDiv(cx, spacing); gridZ = floorDiv(cz, spacing)   // Math.floorDiv
r = new WorldgenRandom(new LegacyRandomSource(0))
r.setLargeFeatureWithSalt(seed, gridX, gridZ, salt)            // 주의: grid 좌표!
limit = spacing - separation
offX = spread.evaluate(r, limit)   // LINEAR: nextInt(limit) — 1 드로우
offZ = spread.evaluate(r, limit)   // 2번째 드로우 (TRIANGULAR 면 (nextInt+nextInt)/2)
→ ChunkPos(gridX*spacing + offX, gridZ*spacing + offZ)
```

isPlacementChunk = (결과 == (cx,cz) 정확 일치). mineshafts 는 spacing=1/sep=0 →
nextInt(1)=0 두 번 → 모든 청크가 placement chunk (드로우는 일어나지만 결과 무관).

**frequency 검사** (frequency < 1.0 일 때만; 즉 mineshafts 만):
`legacy_type_3` = legacyProbabilityReducerWithDouble (StructurePlacement.java:110-114):

```
r = new WorldgenRandom(new LegacyRandomSource(0))
r.setLargeFeatureSeed(levelSeed, cx, cz)     // salt 무시! 소스 청크 좌표 사용
return r.nextDouble() < 0.004000000189989805  // (double)0.004F
```

참고로 다른 reducer: `default` = setLargeFeatureWithSalt(seed, salt, cx, cz) — **인자
순서가 (salt, x, z)** — 후 nextFloat; `legacy_type_2` = setLargeFeatureWithSalt(seed,
cx, cz, 10387320) 후 nextFloat; `legacy_type_1` = setSeed(cx>>4 관련 별도식). 우리
대상엔 legacy_type_3 만 실사용.

### C.3 createStructures 흐름 (ChunkGenerator.java:497-627)

STRUCTURE_STARTS 스테이지. `state.possibleStructureSets()` 를 순서대로 forEach:

- possibleStructureSets = STRUCTURE_SET 레지스트리 listElements 를
  `hasBiomesForStructureSet` (해당 세트 구조물들의 biome HolderSet 이
  biomeSource.possibleBiomes 와 교집합 존재) 로 필터
  (ChunkGeneratorStructureState.java:56-74). **레지스트리 순서 = 데이터팩 파일
  Identifier 알파벳순** — RegistryDataLoader 가 순회하는
  `MultiPackResourceManager.listResources` 가 TreeMap 반환
  (/tmp/t14dec/MultiPackResourceManager.java:81-90). 즉 ancient_cities,
  buried_treasures, desert_pyramids, end_cities, igloos, jungle_temples, mineshafts,
  nether_complexes, nether_fossils, ocean_monuments, ocean_ruins, pillager_outposts,
  ruined_portals, shipwrecks, strongholds, swamp_huts, trail_ruins, trial_chambers,
  villages, woodland_mansions.
- 세트 처리 순서는 **RNG 상호 영향 없음** (모든 RNG 가 세트별 fresh 인스턴스 + 청크
  좌표 시드). 한 청크에 서로 다른 구조물 start 여러 개 공존 가능 (구조물별 저장).
- 세트별: 먼저 세트 내 구조물 중 이미 valid start 가 이 청크에 있으면 그 세트 skip
  (:516-523). 그 후 isStructureChunk (C.2). 통과 시:
  - structures.size()==1 (trial_chambers): 가중치 RNG **생성 안 함**, 바로
    tryGenerateStructure.
  - size()>1: `r = WorldgenRandom(LegacyRandomSource(0));
    r.setLargeFeatureSeed(levelSeed, cx, cz)` (**frequency reducer 와 동일 시딩의
    별도 인스턴스**). total=Σweight. while(!options.isEmpty()):
    `choice = r.nextInt(total)` [드로우]; weight 누적 감산으로 엔트리 선택;
    tryGenerateStructure 성공 → return; 실패 → 그 엔트리 제거, total -= weight,
    **루프 재드로우** (mineshafts: nextInt(2), 실패 시 nextInt(1)=0 한 번 더).
- tryGenerateStructure (:591-627): references = 기존 start 의 getReferences() 또는 0;
  biomePredicate = structure.biomes()::contains (#has_structure/* 태그);
  `structure.generate(...)`; start.isValid() (pieces 비어있지 않음) 이면
  setStartForStructure (청크의 HashMap<Structure,StructureStart>).

### C.4 Structure.generate / biome 검사 시점 (Structure.java:90-155, 228-292)

- GenerationContext 생성 시 `context.random()` =
  `WorldgenRandom(LegacyRandomSource(0))` + **setLargeFeatureSeed(seed, cx, cz)**
  (Structure.java:273-277). → (A)의 질문 답: 맞음, setLargeFeatureSeed.
- `findValidGenerationPoint` = findGenerationPoint(context) 후
  `.filter(isValidBiome)` — **stub.position()** 에서
  `biomeSource.getNoiseBiome(QuartPos.fromBlock(x)=x>>2, y>>2, z>>2,
  randomState.sampler())` 로 노이즈 바이옴 산출 (실제 청크 불필요), predicate 는
  구조물 JSON biomes 태그 (:142-155).
- GenerationStub 의 generator 가 `Either.left(consumer)` 면 piece 조립이
  **biome 검사 통과 후** `getPiecesBuilder()` 에서 실행 (shipwreck/ocean_ruin/
  ruined_portal/jigsaw). `Either.right(builder)` 면 findGenerationPoint 안에서
  이미 조립 완료 (mineshaft) — biome 실패해도 RNG 는 이미 소모됨.
- start bbox: `StructureStart.getBoundingBox()` =
  `structure.adjustBoundingBox(piecesContainer.calculateBoundingBox())` — piece bbox
  들의 encapsulation (StructurePiece.createBoundingBox → BoundingBox.
  encapsulatingBoxes; **여백 inflate 없음**). adjustBoundingBox 는
  terrain_adaptation != NONE 일 때만 `inflatedBy(12)` (Structure.java:86-88) —
  trial_chambers(encapsulate) 만 +12, mineshaft/shipwreck/ocean_ruin/ruined_portal 은
  NONE → 그대로.

### C.5 각 구조물 findGenerationPoint 개요 + 청크 의존성

**공통**: `chunkGenerator.getFirstOccupiedHeight(x,z,type,heightAccessor,randomState)`
= `getBaseHeight(...) - 1` (ChunkGenerator.java:693-695). NoiseBasedChunkGenerator 의
getBaseHeight/getBaseColumn 은 iterateNoiseColumn (노이즈 전용) — **실제 청크 데이터를
읽지 않음**. heightAccessor 는 proto 청크 (min/max Y 만 사용). 즉 위치 결정 단계는
전부 노이즈 예측만으로 재현 가능.

- **trial_chambers** (JigsawStructure.java:153-170 + JigsawPlacement.java:53-165):
  draw#1 `startHeight.sample` = uniform(absolute -40 .. absolute -20) →
  `Mth.randomBetweenInclusive(r, -40, -20)` = nextInt(21)-40
  (UniformHeight.java:36-48; JSON 확인 완료). startPos = (minBlockX, y, minBlockZ).
  PoolAliasLookup.create 는 `RandomSource.create(seed).forkPositional().at(startPos)`
  로 **별도 RNG** — context.random 무소모 (alias/PoolAliasLookup.java:20-32).
  addPieces: draw#2 `Rotation.getRandom` nextInt(4); draw#3
  `centerPool.getRandomTemplate` = templates.get(nextInt(size)) (StructureTemplatePool
  .java:114-118; templates 는 weight 만큼 원소 복제된 확장 리스트).
  start_jigsaw_name 없음 → anchor=startPos. projectStartToHeightmap 없음 → bottomY =
  startPos.y. dimension_padding 10 검사. **stub 위치 = (start piece bbox 중심 x,
  샘플된 y, bbox 중심 z)** — 이 좌표에서 biome 검사. 이후 재귀 조립은
  Either.left consumer (biome 통과 후).
- **shipwreck_beached** (ShipwreckStructure.java:30-53): findGenerationPoint =
  onTopOfChunkCenter(WORLD_SURFACE_WG) → stub = (midX=+8, getFirstOccupiedHeight(midX,
  midZ), midZ=+8) — **드로우 0 으로 biome 검사**. 통과 후 generatePieces: draw#1
  rotation nextInt(4); draw#2 템플릿 = Util.getRandom(STRUCTURE_LOCATION_BEACHED[11],r)
  → nextInt(11) (ocean 변형은 20종 → nextInt(20)); piece 는 (minBlockX, 90, minBlockZ)
  기준. `isTooBigToFitInWorldGenRegion` (sizeX>32 || sizeY>32,
  ShipwreckPieces.java:175-178) 인 템플릿만 이 시점에 높이 확정: beached →
  `Structure.getLowestY` (bbox 4모서리 getFirstOccupiedHeight min; 노이즈) 후
  draw#3 `calculateBeachedPosition(minY,r)` = minY - sizeY/2 - nextInt(3); 아니면
  (대부분) **postProcess 시점으로 이연** — feature 스테이지 RNG 에서, 실제 리전
  하이트맵으로 min/mean 계산 후 beached 는 nextInt(3) 드로우
  (ShipwreckPieces.java:148-187).
- **ocean_ruin_warm** (OceanRuinStructure.java:45-55): onTopOfChunkCenter(
  OCEAN_FLOOR_WG) → stub 서 biome 검사 (드로우 0). 통과 후: draw#1 rotation
  nextInt(4); OceanRuinPieces.addPieces (:152-166): draw#2 `nextFloat() <= 0.3F`
  (isLarge; **<= 주의**); draw#3 템플릿 = isLarge? Util.getRandom(BIG_WARM_RUINS[4])
  : Util.getRandom(WARM_RUINS[44]); isLarge && draw#4 `nextFloat() <= 0.9F` →
  addClusterRuins (:168-198): allPositions 서 Mth.nextInt ×16 (8포지션 × x,z 각 1),
  draw `Mth.nextInt(r,4,8)` (ruins 수), 루프마다 nextInt(list.size) + rotation
  nextInt(4), parentBB 비교차 시 addPiece (small, 추가 드로우 없음 — warm 은
  getSmallWarmRuin nextInt(44)). 높이는 piece postProcess 에서 실제 하이트맵 사용.
- **ruined_portal_ocean** (RuinedPortalStructure.java:78-253): setups 1개 → 가중치
  드로우 없음. `sample(r, airPocketProbability=0.0)` → **0.0 이면 드로우 없이 false**
  (:176-182). draw#1 `nextFloat() < 0.05F` (giant?); draw#2 템플릿 nextInt(10)
  (giant 면 nextInt(3)); draw#3 rotation = Util.getRandom(Rotation.values) nextInt(4);
  draw#4 mirror = `nextFloat() < 0.5F ? NONE : FRONT_BACK`. bbox = 템플릿 변환 bbox
  (basePos = (minBlockX,0,minBlockZ)). surfaceY = getBaseHeight(bbox중심,
  OCEAN_FLOOR_WG[on_ocean_floor], …)-1 (노이즈). findSuitableY (:188-249):
  on_ocean_floor 는 else 분기 → newY = surfaceY (**드로우 없음**; in_mountain/
  underground/partly_buried/in_nether 만 randomBetweenInclusive 드로우). 이후 bbox
  바닥 4모서리의 **getBaseColumn (노이즈 컬럼)** 을 뽑아 projectedY 를 newY 부터
  minY(=dimMinY+15=-49) 까지 내리며 각 y 에서 4컬럼 중 heightmap.isOpaque 인 모서리
  ≥3 이면 반환. stub = (minBlockX, projectedY, minBlockZ) 에서 biome 검사. piece 는
  consumer (biome 통과 후) — properties.cold = coldEnoughToSnow(노이즈 바이옴) 등,
  추가 드로우 없음.

### C.6 createReferences (ChunkGenerator.java:639-672)

STRUCTURE_REFERENCES 스테이지. range=8 고정:

```
for sourceX = cx-8 .. cx+8:        # 외측 루프 X
  for sourceZ = cz-8 .. cz+8:      #  내측 Z (17×17 = 289 청크)
    key = ChunkPos.pack(sourceX, sourceZ)
    for start in region.getChunk(sourceX,sourceZ).getAllStarts().values():
      if start.isValid()
         && start.getBoundingBox()               # adjust 포함 (trial_chambers +12)
              .intersects(minBlockX, minBlockZ, minBlockX+15, minBlockZ+15):
              # 2D XZ 전용 오버로드 (BoundingBox.java:135-137) — Y 무시
        centerChunk.addReferenceForStructure(start.getStructure(), key)
```

저장 = 센터 청크의 `Map<Structure, LongSet>`; 셋은 `LongOpenHashSet` 이고 add 순서 =
위 스캔 순서 (ChunkAccess.java:243-246). **주의**: 이후 소비 시
(StructureManager.fillStartsForStructure) LongSet **순회 순서 = fastutil
오픈어드레싱 해시 순서** (삽입순 아님) — 같은 구조물 start 가 한 청크에 2+ 참조되면
postProcess 순서/RNG 에 영향. C 포팅 시 LongOpenHashSet(기본 cap 16, lf 0.75,
HashCommon.mix) 순회를 에뮬레이션해야 함 (단일 참조가 대부분이라 보통 무관).

references 카운팅: `StructureStart.references` 는 createReferences 와 무관 —
`addReference()` (references++) 는 locate 명령 경로 (StructureManager.addReference)
에서만; 월드젠은 fetchReferences 로 기존 값 승계 (재생성 시), 신규 0.
getMaxReferences()=1, canBeReferenced() = references < 1 (StructureStart.java:138-152).

### C.7 postProcess 디스패치 (features 스테이지, ChunkGenerator.applyBiomeDecoration :339-438)

`decorationSeed = random.setDecorationSeed(levelSeed, minBlockX, minBlockZ)`.
스텝 루프 stepIndex 0..10 (Decoration ordinal: RAW_GENERATION 0, LAKES 1,
LOCAL_MODIFICATIONS 2, **UNDERGROUND_STRUCTURES 3**, **SURFACE_STRUCTURES 4**,
STRONGHOLDS 5, UNDERGROUND_ORES 6, UNDERGROUND_DECORATION 7, FLUID_SPRINGS 8,
VEGETAL_DECORATION 9, TOP_LAYER_MODIFICATION 10). 각 스텝에서 **구조물 먼저**:
STRUCTURE 레지스트리 전체를 step ordinal 로 groupingBy 한 리스트를 레지스트리(=
알파벳) 순으로 순회하며 `random.setFeatureSeed(decorationSeed, index, stepIndex)`
(index = 그 스텝 내 등장 순번). 26.2 스텝3 (underground_structures) 구조물 순번:
**buried_treasure=0, mineshaft=1, mineshaft_mesa=2, trail_ruins=3, trial_chambers=4**.
스텝4 (surface_structures) 순번: bastion_remnant=0, desert_pyramid=1, end_city=2,
igloo=3, jungle_pyramid=4, mansion=5, monument=6, ocean_ruin_cold=7,
**ocean_ruin_warm=8**, pillager_outpost=9, ruined_portal=10, ruined_portal_desert=11,
_jungle=12, _mountain=13, _nether=14, **_ocean=15**, _swamp=16, **shipwreck=17,
shipwreck_beached=18**, stronghold=19, swamp_hut=20, village_desert=21..
village_taiga=25.

setFeatureSeed 후 `startsForStructure(sectionPos, structure)` (레퍼런스 LongSet →
start 목록, C.6 순회 주의) 의 각 start 에 **같은 random 인스턴스를 이어서** 사용해
`start.placeInChunk(level, sm, gen, random, getWritableArea(chunk), centerPos)`.
getWritableArea = (minBlockX, dimMinY+1, minBlockZ) ~ (+15, maxY, +15)
(ChunkGenerator.java:440-448).

`StructureStart.placeInChunk` (StructureStart.java:91-113): referencePos =
pieces[0].bbox 의 (centerX, minY, centerZ); pieces 리스트 순서(=조립 DFS 순서)대로
`piece.bbox.intersects(chunkBB)` (3D) 인 piece 만 `postProcess(...)`. 그 뒤
structure.afterPlace (mineshaft 등은 no-op).

---

## A. Mineshaft (normal 타입 기준)

### A.1 findGenerationPoint + 수직 이동 (MineshaftStructure.java:43-72)

context.random() 은 C.4 대로 setLargeFeatureSeed(seed, cx, cz) 시딩. 드로우 순서:

1. `context.random().nextDouble()` — **폐기 드로우** (레거시 확률검사 잔재, :44).
2. startPos = (16cx+8, 50, 16cz) — **x 는 중앙(+8), z 는 최소(+0)** 비대칭 주의.
3. `MineShaftRoom(0, random, 16cx+2, 16cz+2, type)` (MineshaftPieces.java:1103-1117):
   bbox = BoundingBox(west, 50, north, west+7+**nextInt(6)**, 54+**nextInt(6)**,
   north+7+**nextInt(6)**) — 인자 평가 순서대로 draw dX, dY, dZ. orientation 미설정
   (null). builder.addPiece(room).
4. `room.addChildren(room, builder, random)` — A.2 재귀 전체.
5. normal 타입: `builder.moveBelowSeaLevel(seaLevel=63, minY=getMinY()=-64, random,
   offset=10)` (StructurePiecesBuilder.java:30-41):
   ```
   maxY = 63 - 10 = 53
   bbox = 전체 piece encapsulation
   y1Pos = bbox.getYSpan() + (-64) + 1
   if (y1Pos < maxY) y1Pos += nextInt(maxY - y1Pos)   // 조건부 1 드로우
   dy = y1Pos - bbox.maxY()
   offsetPiecesVertically(dy)   // 모든 piece.move(0,dy,0);
                                // Room 은 move 오버라이드로 childEntranceBoxes 도 이동
   return dy
   ```
   (참고 moveInsideHeights (:43-55): span=high-low+1-YSpan; span>1 이면
   y0=low+nextInt(span) 아니면 low; dy=y0-bbox.minY — normal mineshaft 미사용.)
   mesa 분기 (:62-68): getBaseHeight(bbox중심, WORLD_SURFACE_WG) 후 surface>sea 면
   randomBetweenInclusive(63, surface) 드로우 — 참고만.
6. stub = startPos.offset(0, dy, 0) = (16cx+8, 50+dy, 16cz) → 이 좌표에서 C.4 biome
   검사 (#has_structure/mineshaft). **piece 조립·드로우는 biome 검사보다 먼저 전부
   끝나 있음** (Either.right).

bbox 반영: start bbox 는 지연 계산 (piece 최종 좌표 encapsulation, inflate 없음) —
moveBelowSeaLevel 은 자동 반영 (C.4).

### A.2 조립 재귀 (MineshaftPieces.java)

**generateAndAddPiece** (:89-116) — 모든 자식 생성의 관문:

1. `depth > 8` → null (**드로우 없음**).
2. `|footX - startPiece.bbox.minX| > 80 || |footZ - startPiece.bbox.minZ| > 80` →
   null (드로우 없음).
3. `createRandomShaftPiece(acc, r, footX, footY, footZ, dir, depth+1, type)` (:51-86):
   - draw `sel = nextInt(100)`.
   - sel ≥ 80 → **Crossing**: findCrossing (:658-681): draw `nextInt(4)==0 ? y1=6 :
     y1=2`; 로컬 박스 NORTH(-1,0,-4, 3,y1,0) / SOUTH(-1,0,0, 3,y1,4) /
     WEST(-4,0,-1, 0,y1,3) / EAST(0,0,-1, 4,y1,3) 을 (footX,footY,footZ) 만큼 move;
     `findCollisionPiece` (piece 리스트 선형 스캔, 첫 intersects) 있으면 null.
     성공 시 MineShaftCrossing 생성자 (:647-656): **드로우 0**; this.direction=dir
     저장하지만 setOrientation 안 함 (orientation=null); isTwoFloored = YSpan>3.
   - 70 ≤ sel < 80 → **Stairs**: findStairs (:1333-1350, 드로우 0): 박스
     NORTH(0,-5,-8, 2,2,0) / SOUTH(0,-5,0, 2,2,8) / WEST(-8,-5,0, 0,2,2) /
     EAST(0,-5,0, 8,2,2) + move; 충돌 시 null. 생성자 setOrientation(dir).
   - sel < 70 → **Corridor**: findCorridorSize (:160-184): draw
     `len = nextInt(3) + 2`; len..1 내림 루프 (추가 드로우 없음): blockLen=5*len,
     박스 NORTH(0,0,-(blockLen-1), 2,2,0) / SOUTH(0,0,0, 2,2,blockLen-1) /
     WEST(-(blockLen-1),0,0, 0,2,2) / EAST(0,0,0, blockLen-1,2,2) + move, 첫
     비충돌 채택; 전부 충돌 → null. 성공 시 MineShaftCorridor 생성자 (:141-157):
     setOrientation(dir); draw `hasRails = nextInt(3)==0`; `spiderCorridor =
     !hasRails && nextInt(23)==0` — **hasRails 면 nextInt(23) 드로우 생략**
     (단락평가); numSections = (축이 Z 면 ZSpan 아니면 XSpan)/5.
4. piece != null 이면 `acc.addPiece(piece)` 후 **즉시**
   `piece.addChildren(startPiece, acc, random)` (DFS; 리스트 순서 = 생성 순서).

**MineShaftRoom.addChildren** (:1124-1246): depth=0(자신 genDepth);
heightSpace = max(YSpan-4, 1). 4개 벽 패스 순서 = **북벽 → 남벽 → 서벽 → 동벽**.
각 패스:

```
pos = 0
while (pos < span):                    # 북/남: span=XSpan, 서/동: ZSpan
  pos += nextInt(span)                 # draw
  if (pos + 3 > span) break
  y = minY + nextInt(heightSpace) + 1  # draw (인자 평가 시점)
  child = generateAndAddPiece(...)     # 재귀 (드로우는 위 규칙)
  if child: childEntranceBoxes.add(벽별 박스)
  pos += 4                             # child null 이어도 +4
```

발 좌표: 북 (minX+pos, y, minZ-1, NORTH); 남 (minX+pos, y, maxZ+1, SOUTH);
서 (minX-1, y, minZ+pos, WEST); 동 (maxX+1, y, minZ+pos, EAST).
entrance 박스: 북 = (childX0, childY0, minZ .. childX1, childY1, minZ+1); 남 =
(…, maxZ-1 .. maxZ); 서 = (minX .. minX+1, child yz); 동 = (maxX-1 .. maxX, …).

**MineShaftCorridor.addChildren** (:186-368): depth = this.genDepth.
draw `endSel = nextInt(4)`. orientation 별 (끝단 연장; y 인자에서 draw
`minY - 1 + nextInt(3)`):

| orient | endSel≤1 (직진) | endSel==2 | else(3) |
|---|---|---|---|
| NORTH | (minX, y*, minZ-1, N) | (minX-1, y*, minZ, W) | (maxX+1, y*, minZ, E) |
| SOUTH | (minX, y*, maxZ+1, S) | (minX-1, y*, maxZ-3, W) | (maxX+1, y*, maxZ-3, E) |
| WEST | (minX-1, y*, minZ, W) | (minX, y*, minZ-1, N) | (minX, y*, maxZ+1, S) |
| EAST | (maxX+1, y*, minZ, E) | (maxX-3, y*, minZ-1, N) | (maxX-3, y*, maxZ+1, S) |

(y* = minY-1+nextInt(3), 브랜치 선택 후 드로우.) 이어서 `depth < 8` 이면 측면 분기:
E/W 코리도는 `for x = minX+3; x+3 <= maxX; x += 5`, N/S 코리도는 z 동일 스캔; 각
반복 draw `sel = nextInt(5)`; E/W 축: 0→(x, minY, minZ-1, NORTH, depth+1),
1→(x, minY, maxZ+1, SOUTH, depth+1); N/S 축: 0→(minX-1, minY, z, WEST, depth+1),
1→(maxX+1, minY, z, EAST, depth+1); 2~4 는 무동작.

**MineShaftCrossing.addChildren** (:683-869): 저층 3분기 (드로우 없음, 재귀만):
NORTH: (minX+1,minY,minZ-1,N), (minX-1,minY,minZ+1,W), (maxX+1,minY,minZ+1,E);
SOUTH: (minX+1,minY,maxZ+1,S), (minX-1,minY,minZ+1,W), (maxX+1,minY,minZ+1,E);
WEST: (minX+1,minY,minZ-1,N), (minX+1,minY,maxZ+1,S), (minX-1,minY,minZ+1,W);
EAST: (minX+1,minY,minZ-1,N), (minX+1,minY,maxZ+1,S), (maxX+1,minY,minZ+1,E).
isTwoFloored 면 상층 4게이트, 순서 N→W→E→S, 각각 draw `nextBoolean()` 후 통과 시
(minX+1, minY+4, minZ-1, N) / (minX-1, minY+4, minZ+1, W) /
(maxX+1, minY+4, minZ+1, E) / (minX+1, minY+4, maxZ+1, S).

**MineShaftStairs.addChildren** (:1352-1407): 직진 1개, 드로우 없음:
N (minX, minY, minZ-1) / S (minX, minY, maxZ+1) / W (minX-1, minY, minZ) /
E (maxX+1, minY, minZ).

### A.3 StructurePiece 공통 헬퍼 (StructurePiece.java)

- **좌표 변환** getWorldPos(x,y,z) (:140-174): orientation==null → 항등 (**Room,
  Crossing 이 여기 해당 — postProcess 에서 절대좌표 사용**). 아니면:
  worldX: N/S → minX+x; W → maxX−z; E → minX+z.
  worldY: minY+y. worldZ: N → maxZ−z; S → minZ+z; W/E → minZ+x.
- **setOrientation** (:564-588): N → mirror NONE, rot NONE; S → LEFT_RIGHT, NONE;
  W → LEFT_RIGHT, CW_90; E → NONE, CW_90. orientation 을 아예 설정 안 한 piece
  (Room/Crossing) 는 mirror/rotation 필드가 **null** — placeBlock 의
  `mirror != Mirror.NONE` 이 true 가 되어 `state.mirror(null)/rotate(null)` 호출되나
  기본 Block.mirror/rotate 는 인자 무시 항등이라 air/판자에는 무해 (C 포팅 시 항등
  처리).
- **placeBlock** (:176-206): pos=getWorldPos; `chunkBB.isInside(pos)` (3D) 아니면
  무동작; `canBeReplaced(level,x,y,z,chunkBB)` — 기본 true, **MineShaftPiece
  오버라이드 (:1016-1022): 기존 블록이 type 의 planks/wood/fence 또는 IRON_CHAIN 이면
  거부** (getBlock 경유 — chunkBB 밖은 AIR 로 읽힘); mirror/rotate 적용;
  `level.setBlock(pos, state, 2)`; 그 위치 fluid 비어있지 않으면
  `level.scheduleTick(pos, fluidType, 0)` (유체틱 레코딩 대상!); 블록이
  SHAPE_CHECK_BLOCKS {NETHER_BRICK_FENCE, TORCH, WALL_TORCH, OAK/SPRUCE/DARK_OAK/
  PALE_OAK/ACACIA/BIRCH/JUNGLE_FENCE, LADDER, IRON_BARS} 면
  `chunk.markPosForPostProcessing(pos)` (RAIL/COBWEB/IRON_CHAIN 은 아님).
  플래그는 항상 2; 하이트맵 갱신은 ProtoChunk.setBlockState 쪽 로직 (상태별
  heightmapsAfter 프라임, ProtoChunk.java:116-150) — features 스테이지 기존 구현과
  동일.
- **getBlock** (:214-219): chunkBB 밖 → `Blocks.AIR.defaultBlockState()`, 안 →
  실제 상태.
- **isInterior** (:221-226): pos=getWorldPos(x, y+1, z); chunkBB 밖 → false;
  `pos.y < level.getHeight(OCEAN_FLOOR_WG, pos.x, pos.z)` — **라이브 하이트맵**
  (같은 postProcess 안에서 놓인 블록에 따라 변함).
- **generateBox** (:247-273): y→x→z 3중 루프 (y 외측!), skipAir 면 기존 블록 air 일
  때 스킵, 쉘(경계) = edgeBlock / 내부 = fillBlock, 전부 placeBlock. 드로우 없음.
- **generateMaybeBox** (:322-353): 같은 루프 순서, **셀마다 무조건 draw
  `nextFloat()`** 후 `> prob` 면 스킵 (skipAir/hasToBeInside 검사는 드로우 뒤).
- **maybeGenerateBlock** (:355-368): draw `nextFloat() < prob` 이면 placeBlock.
- **generateUpperHalfSphere** (:370-407): 드로우 없음 (float 나눗셈 스칼라식 —
  1.05F 임계).
- **fillColumnDown (base)** (:409-424): pos=getWorldPos(x,startY,z); chunkBB 안이면
  `isReplaceableByStructures(state)` (= isAir || liquid || GLOW_LICHEN || SEAGRASS ||
  TALL_SEAGRASS, :426-428) && y > minY+1 인 동안 setBlock(state,2) 후 아래로.
- **createChest (base, 블록 체스트)** (:430-508): mineshaft 는 미사용 (Corridor 가
  오버라이드) — chunkBB.isInside && 기존이 CHEST 아니면: blockState==null 이면
  `reorient` (드로우 없음: HORIZONTAL N,E,S,W 순회로 유일 솔리드 이웃 반대향, 아니면
  FACING 순환 탐색); setBlock flag 2; ChestBlockEntity 면
  `setLootTable(key, random.nextLong())` [draw, next×2].
- **createDispenser** (:510-531): 동일 패턴 + nextLong. 참고.
- **makeBoundingBox** (:76-82): 축별 폭/깊이 스왑. **BoundingBox.getCenter** =
  (minX+(spanX)/2, minY+spanY/2, minZ+spanZ/2) (BoundingBox.java:257-259).

### A.4 postProcess — 배치 (feature 스테이지, C.7 의 random 이어짐)

공통 선행: **isInInvalidLocation(level, chunkBB)** (MineshaftPieces.java:1038-1087):
piece bbox 를 ±1 팽창 후 chunkBB 로 클립한 [x0..x1]×[y0..y1]×[z0..z1] 에 대해 (1)
중심점 `level.getBiome` 이 #minecraft:mineshaft_blocking (= deep_dark 만) 이면 true;
(2) 상/하면 (y0,y1 각 x×z 전체), (3) 북/남면 (z0,z1; x 외측→y 내측), (4) 서/동면
(x0,x1; z 외측→y 내측) 순서로 `liquid()` 발견 시 true. **true 면 그 piece 의
postProcess 전체 무동작 → 드로우 0** (RNG 정렬에 결정적). getBiome 은 리전 바이옴 +
BiomeManager 지터 (기존 구현 자산).

**MineShaftRoom.postProcess** (:1248-1301): 드로우 0. 절대좌표 generateBox 로
(minX, minY+1, minZ)–(maxX, min(minY+3, maxY), maxZ) CAVE_AIR; childEntranceBoxes
각각 (x0, maxY-2, z0)–(x1, maxY, z1) CAVE_AIR; generateUpperHalfSphere(minX, minY+4,
minZ, maxX, maxY, maxZ, CAVE_AIR, skipAir=false).

**MineShaftCorridor.postProcess** (:398-478) — 전체 드로우 시퀀스 (로컬좌표,
orientation 변환 적용; len = numSections*5 − 1):

1. generateBox(0,0,0, 2,1,len, CAVE_AIR, CAVE_AIR, false) — 드로우 0.
2. generateMaybeBox(r, **0.8F**, 0,2,0, 2,2,len, CAVE_AIR, CAVE_AIR, false, false)
   — **draw nextFloat ×3×(len+1)** (y=2 한 층, x 0..2 → z 0..len 순).
3. spiderCorridor 면 generateMaybeBox(r, **0.6F**, 0,0,0, 2,1,len, COBWEB, CAVE_AIR,
   false, hasToBeInside=true) — **draw nextFloat ×6×(len+1)** (y=0,1 × x 0..2 ×
   z 0..len; isInterior 는 드로우 후 검사).
4. 섹션 루프 s = 0..numSections−1, z = 2+5s:
   a. `placeSupport(level, chunkBB, 0, 0, z, 2, 2, r)` (:566-601): 먼저
      `isSupportingBox` (:1028-1036) — x=0..2 의 (x, 3, z) getBlock 이 하나라도
      isAir 면 false (**chunkBB 밖은 air 로 읽혀 false 됨**) → **드로우 0 으로
      스킵**. 지지되면: 양쪽 fence 기둥 generateBox (0,0,z)-(0,1,z) fence[WEST=true],
      (2,0,z)-(2,1,z) fence[EAST=true] (드로우 0); draw `nextInt(4)`; ==0 →
      상단 판자 2개 generateBox (양끝만; 드로우 0, 총 1드로우); 아니면 상단 판자
      generateBox(0,2,z)-(2,2,z) 후 **draw nextFloat** maybeGenerateBlock(0.05F,
      1, 2, z−1, WALL_TORCH[FACING=SOUTH]) + **draw nextFloat** maybeGenerateBlock(
      0.05F, 1, 2, z+1, WALL_TORCH[FACING=NORTH]) (총 3드로우).
   b. maybePlaceCobWeb ×8 (:603-609) 순서: (0.1F, 0,2,z−1), (0.1F, 2,2,z−1),
      (0.1F, 0,2,z+1), (0.1F, 2,2,z+1), (0.05F, 0,2,z−2), (0.05F, 2,2,z−2),
      (0.05F, 0,2,z+2), (0.05F, 2,2,z+2). 각각: **isInterior 가 먼저** — false 면
      드로우 0; true 면 draw `nextFloat() < p`, 통과 시 hasSturdyNeighbours (6방향
      Direction.values 순, 스터디 이웃 ≥2; 드로우 0) 성립 시 placeBlock(COBWEB).
   c. draw `nextInt(100)`; ==0 → createChest(2, 0, z−1, abandoned_mineshaft).
   d. draw `nextInt(100)`; ==0 → createChest(0, 0, z+1, abandoned_mineshaft).
      **Corridor.createChest 오버라이드 (:370-396, 마인카트 체스트)**: pos=
      getWorldPos; `chunkBB.isInside && getBlockState(pos).isAir() &&
      !getBlockState(pos.below()).isAir()` 실패 시 **드로우 0** 반환. 성공 시:
      draw `nextBoolean()` → RAIL[SHAPE = NORTH_SOUTH : EAST_WEST] 를 placeBlock
      (E/W 코리도는 rotation CW_90 로 shape 회전됨); CHEST_MINECART 엔티티 생성
      (엔티티 자체 RNG 는 worldgen random 과 무관), `chest.setLootTable(key,
      random.nextLong())` [**draw nextLong**]; level.addFreshEntity.
   e. spiderCorridor && !hasPlacedSpider 면: draw `newZ = z − 1 + nextInt(3)`;
      pos = getWorldPos(1, 0, newZ); `chunkBB.isInside && isInterior(1,0,newZ)` 면
      hasPlacedSpider=true, `level.setBlock(pos, SPAWNER, 2)` (placeBlock 아님 —
      canBeReplaced 미적용), SpawnerBlockEntity 면 `setEntityId(CAVE_SPIDER, r)` —
      **드로우 0** (BaseSpawner.setEntityId → getOrCreateNextSpawnData:
      spawnPotentials 빈 WeightedList → selector null → Optional.empty, 드로우 없음;
      /tmp/t14dec/BaseSpawner.java:64-66, 349-356 + WeightedList.java:79-86).
      실패해도 nextInt(3) 은 이미 소모, hasPlacedSpider 는 false 유지 → 다음 섹션
      재시도.
5. 바닥 판자: x=0..2 (외측) × z=0..len: setPlanksBlock(planks, x, −1, z)
   (:1089-1097): isInterior 면 기존 상태가 위면 sturdy 아니면 setBlock(planks, 2).
   드로우 0.
6. placeDoubleLowerOrUpperSupport(0, −1, 2) (:480-490): (0,−1,2)/(2,−1,2) 가 판자면
   fillPillarDownOrChainUp (:512-548) — 아래로 지지 탐색 (≤20) 해서 기둥(wood) 채움
   또는 위로 (≤50) fence+IRON_CHAIN 체인 — 전부 setBlock flag 2, **드로우 0**.
   numSections>1 이면 (0, −1, len−2) 한 번 더.
7. hasRails 면: z=0..len 각각 getBlock(1, −1, z) 이 !isAir && isSolidRender 일 때만
   p = isInterior(1,0,z) ? 0.7F : 0.9F 로 **draw nextFloat** maybeGenerateBlock(
   RAIL[NORTH_SOUTH]) — 바닥 미충족 z 는 드로우 없음.

**MineShaftCrossing.postProcess** (:871-995): **드로우 0**. 절대좌표.
isTwoFloored 면 십자 에어박스 5개 (하층 2 + 상층 2 + 중간층 연결 1), 아니면 2개;
placeSupportPillar ×4 ((minX+1|maxX−1) × (minZ+1|maxZ−1), y0=minY, y1=maxY):
(x, maxY+1, z) 가 air 아니면 generateBox 판자 기둥; 바닥: 전 평면 (y=minY−1)
setPlanksBlock.

**MineShaftStairs.postProcess** (:1409-1426): 드로우 0. 로컬좌표 에어박스:
(0,5,0)-(2,7,1), (0,0,7)-(2,2,8), i=0..4 계단형.

---

## B. monster_room (MonsterRoomFeature.java:32-135)

Feature id `minecraft:monster_room`, config 없음. 배치: placed_feature
`monster_room` = count 10 → in_square → height_range uniform(absolute 0 ..
below_top 0) → biome; `monster_room_deep` = count 4 → in_square → uniform(
above_bottom 6 .. absolute −1) → biome. 바이옴 feature 리스트 **스텝 인덱스 3**
(plains.json features[3] = [monster_room, monster_room_deep] — UNDERGROUND_STRUCTURES
스텝, C.7 의 구조물 배치 직후 같은 스텝에서 실행). RNG = setFeatureSeed(decoSeed,
featureSorter 전역 인덱스, 3); 파이프라인은 lazy 스트림 — 포지션 1개씩 in_square
(nextInt(16)×2) → height (nextInt 1회) → biome → place() 순으로 드로우 인터리브
(count 10 은 ConstantInt → 드로우 0). 이 부분은 features 스테이지 기존 구현 관행과
동일.

place(origin, r) — air 술어: `level.isEmptyBlock(pos)` = `getBlockState(pos).isAir()`;
"solid" = `BlockState.isSolid()` (legacy solid 플래그). AIR 상수 = CAVE_AIR.
`safeSetBlock(level,pos,state,pred)` = 기존 상태가 pred (= **not** in
#minecraft:features_cannot_replace 태그) 통과 시 `setBlock(pos, state, 2)`
(Feature.java:275-283). 시퀀스:

1. draw#1 `xr = nextInt(2) + 2`; draw#2 `zr = nextInt(2) + 2`. (y 범위 −1..4 고정,
   벽 범위 x ∈ [−xr−1, xr+1], z ∈ [−zr−1, zr+1].)
2. **검증 스캔** (드로우 0): dx 외측 (minX..maxX) → dy (−1..4) → dz (minZ..maxZ):
   dy==−1 && !solid → **return false**; dy==4 && !solid → **return false**;
   (dx 또는 dz 가 경계) && dy==0 && isEmpty(pos) && isEmpty(pos.above()) →
   holeCount++.
3. `holeCount < 1 || holeCount > 5` → return false (드로우 0).
4. **조각/벽 루프**: dx (minX..maxX) → **dy 3 내림 −1 까지** → dz (minZ..maxZ):
   - 경계 (dx==minX || dy==−1 || dz==minZ || dx==maxX || dy==4[도달불가] ||
     dz==maxZ):
     - `pos.y >= level.getMinY() && !getBlockState(pos.below()).isSolid()` →
       `level.setBlock(pos, CAVE_AIR, 2)` (드로우 0);
     - else if `state.isSolid() && !state.is(CHEST)`:
       `dy == −1 && r.nextInt(4) != 0` — **dy==−1 인 경우에만 draw** (단락평가);
       != 0 → MOSSY_COBBLESTONE safeSetBlock, ==0 (또는 dy≠−1 벽/천장) →
       COBBLESTONE safeSetBlock.
   - 내부: `!is(CHEST) && !is(SPAWNER)` → CAVE_AIR safeSetBlock (드로우 0).
5. **체스트 2회 시도**: for cc=0..1: for i=0..2 (3회 재시도):
   draw `xc = X + nextInt(2*xr+1) − xr`; draw `zc = Z + nextInt(2*zr+1) − zr`
   (y = origin.y). isEmpty(chestPos) 면 HORIZONTAL (N,E,S,W) 4방 solid 카운트;
   **정확히 1** 이면: safeSetBlock(chest, `StructurePiece.reorient` 결과 — 드로우 0);
   `RandomizableContainer.setBlockEntityLootTable(level, r, pos,
   chests/simple_dungeon)` → BE 가 RandomizableContainer 면 **draw nextLong**
   (RandomizableContainer.java:44-50); inner break (남은 i 시도 소거, cc 다음 회차).
   isEmpty 실패나 wallCount≠1 이면 드로우는 xc/zc 2개만 소모하고 다음 i.
6. **스포너**: safeSetBlock(origin, SPAWNER) (드로우 0); getBlockEntity(origin) 이
   SpawnerBlockEntity 면 `setEntityId(randomEntityId(r), r)` — 인자 평가로
   **draw `nextInt(4)`**: MOBS = [SKELETON, ZOMBIE, ZOMBIE, SPIDER] (skeleton 25%,
   zombie 50%, spider 25%); setEntityId 자체는 드로우 0 (A.4-4e 와 동일 근거).
   BE 캐스팅 실패 시 nextInt(4) 는 **평가되지 않음** (인자식이 조건 안에 있음 —
   `if (getBlockEntity instanceof …)` 성공 후에만 호출).
7. return true.

주의: safeSetBlock 이 #features_cannot_replace 로 거부해도 **드로우는 이미 끝난 뒤**
(모시/코블 선택 드로우는 safeSetBlock 이전).

---

## 검증 노트 / C 포팅 시 함정 목록

1. **동일 시딩 3중 사용 (mineshaft)**: legacy_type_3 reducer, 가중치 선택 RNG,
   GenerationContext RNG 가 전부 `setLargeFeatureSeed(seed, cx, cz)` 이지만 **별도
   인스턴스** — 이어쓰기 아님.
2. mineshaft frequency 비교는 `nextDouble() < (double)0.004F` — float 승격값
   0.004000000189989805 사용. ocean_ruin 의 isLarge 는 `nextFloat() <= 0.3F` (≤).
3. Room/Crossing 은 orientation=null → postProcess 가 절대좌표; Corridor/Stairs 는
   로컬좌표 + 방향 변환 (getWorldX/Z 표, S/W 는 mirror, W/E 는 CW_90 rotate 가
   블록상태에 적용 — RAIL shape, FENCE 면 속성, WALL_TORCH FACING 이 회전됨).
4. isInInvalidLocation 이 true 인 piece 는 postProcess 드로우 0 — 유체/deep_dark 에
   따라 청크별 RNG 시퀀스가 갈라짐. 또 isInterior/isSupportingBox/체스트 배치 조건이
  라이브 블록·하이트맵 상태를 읽으므로 **배치 순서 (piece DFS 순, 스텝 내 구조물
  순번, 같은 random 이어쓰기)** 가 모두 드로우 수에 영향.
5. corridor 생성자에서 hasRails=true 면 spiderCorridor 용 nextInt(23) 이 **생략**됨
   (단락평가). monster_room 의 mossy 드로우도 dy==−1 에서만.
6. createReferences 는 반경 8 (17×17), **2D XZ intersects**, bbox 는 adjust 후
   (trial_chambers 만 +12). 참조 LongSet 순회 (postProcess 순서) 는 LongOpenHashSet
   해시 순서.
7. 위치 결정 (C.5) 은 전부 노이즈 예측 (getBaseHeight/getBaseColumn) — 청크 데이터
   불필요. 반면 shipwreck/ocean_ruin 의 최종 높이 보정과 mineshaft 배치 전체는
   feature 스테이지에서 실제 리전 상태 필요.
8. 스포너 setEntityId 는 어떤 경로든 드로우 0 (빈 WeightedList).
9. structure set / structure 레지스트리 순서 = Identifier 알파벳순 (TreeMap).
   스텝별 구조물 순번은 C.7 표 참조 (mineshaft = step3 index1, 등).
10. moveBelowSeaLevel 의 nextInt 는 `y1Pos < maxY` 일 때만 — YSpan 이 116(= 53+64−1
    …) 이상인 거대 mineshaft 는 드로우 생략 (실제로는 거의 항상 드로우 발생).

## 요약 (구현 착수 포인트)

- 배치 판정: C.2 산식 그대로 (mineshaft: 청크마다 nextDouble<0.004F 만),
  가중치 선택 C.3, context RNG = setLargeFeatureSeed.
- mineshaft 조립: room bbox 3드로우 → addChildren DFS (draw 규칙 A.2) →
  moveBelowSeaLevel 1드로우(조건부) → biome 검사 (노이즈).
- 배치: 스텝3 setFeatureSeed(decoSeed, 1, 3) 후 piece DFS 순 postProcess, 코리도
  드로우 시퀀스 A.4.
- monster_room: 스텝3 feature, place() 드로우 시퀀스 B (2 + 바닥별 + 2×3×2 + 1).
