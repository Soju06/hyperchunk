# Task 11 완료 상태 + Task 12 핸드오프 (2026-07-31)

## Task 11 결과 요약

- **바이트코드 확인 (R1-bytecode-spawn-full.md)**: SPAWN =
  `generateSpawn` → `spawnOriginalMobs` → `spawnMobsForChunkGeneration`
  — mob 부기만, 블록/하이트맵/바이옴/라이트 쓰기 제로 (읽기 전용).
  FULL = `LevelChunk(ServerLevel, ProtoChunk, ...)` 전환 —
  `FINAL_HEIGHTMAPS` 4종만 `setRawData` 비트 복사 (전환 시점 프라임/
  재계산 없음, *_WG 조용히 소멸), 블록/바이옴 섹션 참조 승계.
  핸드오프의 실측 diff 와 완전 일치.
- **구현**: `gen_spawn_full_stages.c` — 두 스테이지 모두 `promoted`
  마커 (10/11). `heightmap_final[4]` 가 곧 생존 표현이라 복사 무비용;
  `promoted==11` = "*_WG 부재" 계약. FULL 후 *_WG 재읽기는 기존
  `rg->wg_dropped` 재프라임 경로가 같은 의미 (ImposterProtoChunk
  lazy-prime 과 수렴 — R1 §3).
- **게이트 (test_spawn_full, ctest `spawn_full`)**: 04..06 체인 + 두
  번들 order-replay, 10/11 스냅샷 이벤트에서 blocks/biomes/heightmaps
  대조 (36 덤프 게이트). 결과: **번들당 12/18 0-diff + 6 덤프가 09
  캡과 정확히 같은 잔차** (primary c.-1.-1 1/1·c.0.-1 78/39·c.1.-1
  11/10, alt 1/1·87/39·53/37; 10 과 11 이 청크별 동일 카운트 = 순수
  이월), biomes 전부 0, OF_WG 재프라임 잔차 0, **신규 발산 0**.
  `HC_LIGHT_STRICT=1` 공용 스위치 (골든 재기록 후 풀 0-diff 강제).
- 11_full 의 hm 4종 값 대조는 라이브 FINAL 맵과 그대로 성립 (09 와
  동일한 이유 — FINAL 은 NBT 직렬화라 리로드로 안 끊긴다).

## Task 12 핸드오프 — 리전 출력 (실측 인벤토리)

전부 `golden/seed1234567890_r.0.0.mca` 파싱 실측 (tools/golden/mca.py;
이번 런에서 핵심 수치 재검증 완료). 게이트 정의부터:

### 게이트 = canonical payload 해시 (compare_regions.py)

`--canonical-hash` = sha256( 존재 청크 인덱스 오름차순으로
`pack(">I", index) || mask_last_update(zlib 해제 payload)` ) —
LastUpdate 8바이트만 0 마스킹, 그 외 재직렬화/정렬 없음. 골든 값
`ea3fd98c…7ec` (SHA256SUMS `#canonical-payload`).

**따라서 자유**: 섹터 배치/타임스탬프/패딩(스테일 가비지 포함)/zlib
레벨·프레이밍. zlib 은 stored-block (무압축 DEFLATE) 도 유효 — ADR-003
D1 (무의존성) 지키면서 압축기 없이 쓸 수 있다.
**따라서 바이트 정확 필수**: 해제된 NBT 스트림 전부 — 키 방출 순서
(바닐라 HashMap 순서, 아래 고정 순서 실측), 태그 id, 빈 리스트
etag=End(0), 무명 루트 컴파운드(빈 이름), modified-UTF-8, 패킹 규칙.

### 컨테이너 (참고용)

1024/1024 청크 존재 (32x32 전부, Status 전부 "minecraft:full"),
압축 타입 전부 2(zlib), 길이 필드 = 압축길이+1 (타입 바이트 포함),
타임스탬프 전부 비제로. 6개 청크 섹터 꼬리에 스테일 가비지 (idx 32/33/
65/456/457/481) — canonical 게이트엔 무관.

### 청크 NBT 스키마 (1024 청크 전부 동일 15키, 방출 순서 고정)

`Status, zPos, block_entities, yPos, LastUpdate, structures,
InhabitedTime, xPos, Heightmaps, sections, isLightOn, block_ticks,
PostProcessing, DataVersion, fluid_ticks`

- DataVersion **4903**, yPos **-4**, isLightOn 1, InhabitedTime 0,
  LastUpdate 8 (마스킹됨). 부재 키: entities/Lights/UpgradeData/
  CarvingMasks/blending_data/below_zero_retrogen.
- **sections**: 24개 (Y -4..19 Byte, 오름차순). 키 순서
  `block_states, [SkyLight,] biomes, [BlockLight,] Y`.
  - block_states: `data, palette` 순서. palette 엔트리는 `Properties,
    Name` (Properties 가 Name **앞**), Properties 값 전부 String.
    `data` LongArray 는 **팔레트 1개면 생략**; bits =
    max(4, ceil(log2(n))), long 경계 비월경 (64//bits 개/long) — 실측
    4bit→256L, 5bit→342L, 6bit→410L. 리전 전체 블록 Name 175종.
  - biomes: palette = String 리스트, 팔레트 1개면 data 생략; bits =
    ceil(log2(n)) — **min-4 클램프 없음** (2개→1bit→1L, 3~4개→2bit→2L).
  - SkyLight/BlockLight: ByteArray[2048] 니블, **희소 방출** — 전부
    밝음(위쪽)/전부 어둠 섹션은 키 자체가 생략. 실측 분포: SkyLight
    Y=1..7 만, BlockLight Y=-4..4 만. 방출 조건 = 라이트 엔진의 섹션
    데이터-레이어 존재 규칙 — **Task 12 에서 바이트코드로 확정 필요**
    (우리 엔진은 값은 산출; 어떤 섹션을 직렬화하느냐가 남은 규칙).
- **Heightmaps**: 4종, 순서 `OCEAN_FLOOR, MOTION_BLOCKING_NO_LEAVES,
  MOTION_BLOCKING, WORLD_SURFACE`. 각 LongArray[**37**] = 256 x **9bit**
  (7개/long, bit63 항상 0). raw = 우리 저장값(최고 블로킹 y+1) + 64.
- **PostProcessing**: 정확히 24개 내부 리스트, 전부 빈(etag=End) —
  리전 전체 엔트리 0.
- **structures**: 모든 청크에 `starts`+`References` 컴파운드 (비어도
  존재). starts 는 1020 청크에서 빈 컴파운드.

### 그리드 4청크 (r.0.0 에 실재하는 우리 재생 가능 청크) 실측

r.0.0 ∩ 3x3 그리드 = **(0,0), (1,0), (0,1), (1,1)** — 넷 다 이번 10/11
게이트에서 **blocks/hm/biomes 풀 0-diff** (잔차 캡 청크는 전부 음수
좌표 = 다른 리전 파일, 미캡처). 넷의 실측:

- block_entities: **0 개** (넷 다 빈 리스트) — monster_room 스포너/
  cocoa 등 그리드엔 없음. 리전 전체 195 BE (49청크) 는 전부 구조물
  (trial_chambers/dungeon/shipwreck 등) — 그리드 밖.
- structures.starts/References: **넷 다 빈 컴파운드** — 구조물 생성
  없이도 그리드 게이트 성립.
- **block_ticks: 있음!** (0,0) 681 / (1,0) 817 / (0,1) 419 / (1,1)
  558 엔트리 — 전부 `{i: oak_leaves|jungle_leaves, p:0, t:0, x,y,z
  절대좌표}`. 나무 배치가 스케줄한 leaf distance 틱. **C 쪽 신규
  상태: features 중 스케줄-틱 기록 + NBT 리스트 순서 재현** — 어디서
  스케줄되는지 (LeavesBlock updateShape → scheduleTick?) 와 저장
  순서(tick 큐 순회 순서)를 바이트코드로 확정해야 한다. t 의미
  (남은 지연, 저장 시점 상대) 포함.
- **fluid_ticks**: (1,0) 만 8개 (water/flowing_water, t=5, 경계
  컬럼) — 물 흐름 스케줄. 같은 기록 요구.

### Task 12 스코프 제안

1. **게이트 형태**: r.0.0 전체 canonical 해시는 1024 청크 (구조물,
   9x9 밖 청크) 를 요구하므로 불가. 대신 **per-chunk canonical
   payload 게이트**: 그리드 4청크의 zlib-해제 payload (LastUpdate
   마스킹) 를 우리 직렬화와 바이트 대조. compare_regions.py 의
   `nbt_diff` 가 이미 구조 diff 를 준다.
2. **C 쪽 산출물**: NBT writer (무명 루트, modified-UTF-8, 고정 키
   순서), 섹션 palette+data 패커 (블록 min-4bit / 바이옴 무클램프,
   팔레트 구성·순서 규칙은 바이트코드 확정 필요 — PalettedContainer
   write 경로), 하이트맵 9bit 패커, 라이트 섹션 방출 규칙, 스케줄-틱
   기록기, zlib stored-block 래퍼 (or 미니 DEFLATE).
3. **선행 바이트코드 항목**: ChunkSerializer(26.2 명칭 SerializableChunkData?)
   의 필드 방출 순서 근거, PalettedContainer 직렬화 (팔레트 순서),
   LevelChunkTicks.save (틱 순서/t 인코딩), LightEngine 섹션 직렬화
   조건, leaf/water 틱 스케줄 지점.
4. **커버리지 경계**: 골든 .mca 는 entities/·poi/ 미캡처 — C 쪽도
   불요. 음수 좌표 그리드 5청크는 리전 파일 미캡처라 게이트 불가
   (재기록 시 r.-1.-1 등 회수 고려). 라이트는 최종 고정점 (전 청크
   S) — 09 게이트의 관측 윈도보다 넓다: 링 청크 라이트 상태가 처음
   으로 게이트에 들어온다 (light-stage 커버리지 경계 메모 참조).

## 커버리지 경계 (이 게이트가 못 보는 것)

- spawn 의 실제 mob 산출 (SPAWN_MOBS=false 로 기록 — 덤프에 엔티티
  없음; 우리도 미구현이 정답).
- FULL 후 그리드 밖 이벤트 (imposter write-drop 은 미모델 — 관측
  윈도 내 발생 0 이 게이트 증거).
- 잔차-캡 6 덤프의 캡 이하 회귀 (09 와 동일).
- *_WG 프루닝의 "값" — 11 덤프에 *_WG 가 없으므로 부재만 검증.
