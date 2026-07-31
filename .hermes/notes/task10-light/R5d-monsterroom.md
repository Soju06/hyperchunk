# R5d — MonsterRoomFeature 성공 경로 (minecraft:monster_room, 26.2)

발견 경위: 09 게이트의 링 프리픽스 재생에서 fail-loud 발화 (9b 는 검증
단계까지만 구현 — 그리드에서 검증 통과 사례 0). 링 청크에서 검증을
통과하는 던전이 실존한다.

Source of truth: `javap -p -c -constants -cp
tools/golden/libs/extracted/server-26.2.jar <fqcn>`. 인용은 `place@N` =
`MonsterRoomFeature.place` 오프셋.

## 1. 상수

- `MOBS = [skeleton, zombie, zombie, spider]` (`<clinit>`@6-34).
- `AIR = Blocks.CAVE_AIR.defaultBlockState()` (`<clinit>`@37-43) — 방
  굴착/바닥 구멍 전부 **cave_air**.
- safeSetBlock 술어 = `Feature.isReplaceable(#features_cannot_replace)`
  (place@0-6) — 현재 블록이 태그 밖일 때만 쓴다.

## 2. 검증 단계 (place@24-263) — 9b 구현 그대로

j = nextInt(2)+2 (DRAW), k = nextInt(2)+2 (DRAW); xmin=-j-1..xmax=j+1,
zmin=-k-1..zmax=k+1, dy -1..4; dy==±(−1|4) 비고체면 즉시 false; 문(경계
dy==0 에서 자기+위 공기) 1..5 개 밖이면 false. 검증 루프 자체는 드로우 0.

## 3. 방 셸/내부 (place@264-531)

루프: x 밖 (xmin..xmax), **y 중간 3→−1 내림차순**, z 안 (zmin..zmax).
셀당 st = getBlockState 1회 (@307-316).

- 내부 (`x!=xmin && y!=-1 && z!=zmin && x!=xmax && y!=4 && z!=zmax`,
  @318-355; y==4 는 y 범위 3..−1 밖이라 죽은 항):
  `!st.is(CHEST) && !st.is(SPAWNER)` → safeSet(cave_air) (@480-511).
  chest/spawner 보존은 겹친 이전 던전용.
- 셸: 먼저 `p.y >= minY && !below.isSolid()` → **직접 setBlock(cave_air,
  2)** — safeSet 아님, 술어 없음 (@358-404). 아니면 `st.isSolid() &&
  !st.is(CHEST)` 일 때: **dy==−1 이면 nextInt(4) DRAW** — 0 → cobblestone,
  그 외(1..3) → mossy_cobblestone (@427-477); dy!=−1 은 드로우 없이
  cobblestone (@462).

## 4. 상자 (place@532-748)

상자 2개 × 시도 최대 3회. 시도당:
- x = origin.x + nextInt(2j+1) − j (DRAW), z = origin.z + nextInt(2k+1) − k
  (DRAW), y = origin.y (@550-600). **드로우는 시도마다 무조건.**
- `isEmptyBlock` (isAir) 아니면 다음 시도 (@617-629).
- 수평 4방향 (Plane.HORIZONTAL = N,E,S,W) `isSolid()` 카운트 (@635-691,
  드로우 0); 정확히 1 이 아니면 다음 시도.
- 성공: `safeSet(reorient(chest.defaultState))` (@700-719) 후
  `RandomizableContainer.setBlockEntityLootTable(level, random, p,
  SIMPLE_DUNGEON)` — 컨테이너 블록엔티티가 있으면 **nextLong 1 DRAW**
  (RandomizableContainer.setBlockEntityLootTable@28-33). 상자는 공기
  위에만 놓이므로 (술어 통과 보장) 성공 경로에서 항상 드로우. 이후
  현재 상자 종료 (@734 → 다음 ci).

`StructurePiece.reorient@0-227` (드로우 0):
1. N,E,S,W 순회: 이웃이 CHEST 면 상태 그대로 반환 (기본 facing=north,
   @50-62); `isSolidRender()` 이웃을 수집 — 정확히 1개면 그 방향의
   **반대** 를 FACING 으로 (@63-107); 2개 이상이면 null 로 폴백.
2. 폴백 (@108-227): dir = 기본 FACING(north) 에서 solidRender 를 피해
   opposite → clockWise → opposite 순 재시도, 최종 dir 채택.
isSolidRender ≈ 풀 불투명 큐브 = `hc_block_is_full_cube` (isSolid 와
다르다 — 카운트는 isSolid, reorient 는 isSolidRender).

## 5. 스포너 (place@749-848)

`safeSet(origin, spawner.defaultState)` 후 `getBlockEntity(origin)`:
- SpawnerBlockEntity 면 `setEntityId(randomEntityId(random), random)` —
  `randomEntityId` = `Util.getRandom(MOBS)` = **nextInt(4) 1 DRAW**
  (@788-798, randomEntityId@0-10). `BaseSpawner.setEntityId` 자체는 0
  드로우: 신규 스포너의 `spawnPotentials` 는 빈 WeightedList →
  `getRandom` 이 셀렉터 null 조기 반환 (WeightedList.getRandom@0-10,
  BaseSpawner.getOrCreateNextSpawnData@12-34).
- 드로우 조건 = **최종 origin 블록이 spawner** (safeSet 이 이전 던전의
  스포너에 막혀도 그 블록엔티티로 드로우 발생; chest/bedrock 에 막히면
  드로우 없음 — "Failed to fetch" 로그 경로 @804-843).
- 항상 `return true` (@848).

## 6. C 매핑 (features.c monster_room_place)

- 팔레트 추가: cobblestone / mossy_cobblestone / spawner (noOcclusion —
  F_FULL 없음, dampening 0) / chest[facing=N|S|W|E,type=single,wl=false]
  (부분 형상, dampening 0). blocks.c FLAGS 참조.
- reg 에 `tag_features_cannot_replace` 추가 (mr_safe_set).
- 라이트 영향: 굴착 cave_air 가 링 청크 동굴 라이트 지형을 바꾸고
  (블록라이트 유입), cobble/mossy 는 풀 큐브 (dampening 15). 09 게이트가
  ±15블록 셸을 통해 간접 검증한다. spawner/chest 는 발광 0.
