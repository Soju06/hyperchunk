# Task 10 — 구현/잔차 노트 (run 4, 2026-07-31)

## 이 런에서 닫은 것

1. **melon canSurvive** (`features.c can_survive_state`): 26.2 에 MelonBlock
   클래스가 없다 — `Blocks` 2-인자 `register(BlockItemId, Properties)` →
   BootstrapMethods #10 = `Block::new` (plain Block). `BlockBehaviour
   .canSurvive` 기본 = `iconst_1` (항상 true). 새 태그 체인
   `supports_melon_stem_fruit → #supports_stem_fruit →
   #supports_vegetation` 은 StemBlock/AttachedStemBlock 성장 로직용이지
   과일 블록의 canSurvive 가 아니다.

2. **noise_based_count 배치 모디파이어** (`features_compile.c` +
   `features.c`): kelp_warm 이 실행에 도달하며 필요해짐.
   `NoiseBasedCountPlacement.count` = `d2i(Math.ceil((BIOME_INFO_NOISE(
   x/noise_factor, z/noise_factor) + noise_offset) * ratio))`, 드로우 0,
   CODEC: noise_offset optionalFieldOf 기본 0.0. RepeatingPlacement —
   count 회 반복.

3. **\*_WG 하이트맵 드롭/재프라임 모델** (`hc_feat_height` +
   `hc_feat_region_t.wg_dropped` + `hc_chunk_t.heightmap_wg_reprimed`):
   이번 런의 핵심 시맨틱 발견.

## \*_WG 드롭 모델 — 증거 사슬

- `ChunkStatus` 등록: empty..surface = WORLDGEN_HEIGHTMAPS {OF_WG, WS_WG},
  carvers 부터 = FINAL_HEIGHTMAPS {OF, WS, MB, MBNL}.
  `ProtoChunk.setBlockState` 는 `getPersistedStatus().heightmapsAfter()`
  타입만 증분 갱신 (프라임 누락분은 그 자리에서 프라임). persisted 는
  `ChunkStep.completeChunkGeneration` 에서 스테이지 완료 후 전진.
  ⇒ 데코 중(persisted=CARVERS) 쓰기는 FINAL 4종만 갱신, *_WG 는 동결.
- 실측: 그리드 9청크 07 vs 06 — OF_WG/WS_WG **0 diff 9/9** (동결 확인).
  링2 16청크 — WS_WG 14~247 컬럼, OF_WG 0~51 컬럼 diff (재프라임 확인).
- 링 재프라임의 타입별 시점 차이가 결정적: c.-2.-2 인테리어에서 OF_WG 는
  자기 트리 미반영(step-6 광석 pre-check 가 첫 읽기), WS_WG 는 자기 트리
  반영(step-9 patch_grass 가 첫 읽기 — 07 WS_WG ≈ 07 WORLD_SURFACE,
  1컬럼 잔차 = 읽기 후 배치분). ⇒ **타입별 첫-읽기 지연 재프라임 후 재동결**.
- 원인: 기록 서버가 manifest seq 9 직전 (그리드 features 완료 후, 링
  데코 시작 전) 전 청크를 저장/언로드/리로드 — *_WG 는 NBT 미직렬화라
  드롭. 그리드 청크도 08 이후 리로드 (R6 §5: 09 에서 WS_WG 소멸 +
  OF_WG 재프라임) — 링 데코(seq≥10)의 그리드 크로스-읽기가 재프라임을
  트리거한다. 리플레이 규칙: **entry 9 재생 직전 rg.wg_dropped=1** (두
  번들 공통; ALT 도 첫 9 엔트리 = 그리드).
- 광석 pre-check (`OreFeature.place` 의 OF_WG 풋프린트 게이트,
  `min_y <= getHeight(OCEAN_FLOOR_WG,x,z)`) 가 이 값을 소비 — 게이트
  플립 = doPlace 드로우 전부 소실 = 이후 위치 전면 시프트. c.-2.-2
  (6,0) ore_dirt pos2 (y=84, min_y=79) 가 최초 발견 사례 (풋프린트 내
  OF_WG ≥79 는 210컬럼 중 4개 — 스필 캐노피가 만든 79~83).
- 효과: 링 트레이스 diff 가 step 6 발산 → **step 9 (9,7) trees_jungle
  발산으로 전면 후퇴**. 09 게이트 3653 → 2917 fails, 블록 diff 청크
  8/8 → 3/8.

## 잔차 진단 (남은 2917 의 단일 근원)

**워크드 예시** (c.-2.-2, seq 9, (9,7) trees_jungle position #8):

- 두 번들/트레이스에서 #1..#7 완전 일치 (캐노피 피드백 읽기 71/82/96
  포함 — FINAL 맵 라이브 갱신 모델 정확).
- #8 (-18,y,-31): 바닐라 y=69 (placed=0), 우리 y=68 (placed=1 — 부시
  식재). 68 = 현재 블록의 순수값 (06: grass top 67, 그 위 공기; 선행
  스필/배치 없음 — 검증). 69 = "y=68 에 blocking" = **mid-carve 상태**
  (05: dirt@68+grass@69 → 카버가 69, 68 순 제거; 69 는 그 사이 순간).
- 즉 리로드로 복원된 바닐라 FINAL 맵의 NBT 기준선이 카버-경합 컬럼에서
  mid-carve 스냅샷을 담고 있다. 정합 메커니즘 후보는 "비동기 저장이
  카버 태스크와 경합하며 저장 사본에 FINAL 4종을 프라임" (persisted 규칙
  ·06 덤프 kind 집합·전 관측과 모순 없는 유일 후보) — 단 **월클록
  레이스라 골든에서 재구성 불가**.
- 이 한 셀이 부시 1개를 플립 → 링 정글 청크들의 step-9 초목 전면
  캐스케이드 → (a) 북쪽 그리드 c.0.-1/c.1.-1/c.-1.-1 블록 스필 diff
  (fern 1개 ~ 87개), (b) 링 발광원(cave_vines/berries 등)·캐노피 변화의
  그리드 경계 라이트 유입 diff (c.1.0 primary light_block 289 등,
  블록 0-diff 인 채로).

## 게이트 형태 (fallback 프로토콜)

- 08: 풀 0-diff 게이트, 두 번들 18/18 GREEN (유지).
- 09: 12/18 덤프 0-diff + 6 덤프 잔차-캡 (RESID 테이블, 측정값 그대로 —
  초과 즉시 FAIL, 개선은 통과). c.0.0 두 번들 0-diff (S={자신} 윈도),
  full-window 0-diff: c.-1.0, c.1.1 (양 번들), alt c.0.1/c.-1.1/c.1.0.
- 재활성 조건: autosave-경합 없는 골든 재기록 (기록 하네스에서 자동
  저장/언로드 비활성 또는 세이브 배리어) 후 `HC_LIGHT_STRICT=1`.
- 진단 도구 (이 런에서 추가, env-게이트): `HC_LIGHT_TRACE_DIAG=1`
  (전 리플레이 청크 골든 트레이스 라인 대조 + 링2 06 블록 대조),
  `HC_LIGHT_TRACE_DUMP_DIR` (ours/gold 트레이스 페어 파일 덤프),
  `HC_LIGHT_MAX_PREFIX` (기존 개발 캡).

## 번들 질문의 답 (스코프 §1)

- 08/09 덤프는 번들 간 **동일하지 않다** — 단 08 의 light_block/
  light_sky 는 18/18 동일 (08 라이트는 블록 무관: 섹션 등록 + top 규칙).
  08/09 blocks·heightmaps 와 09 라이트는 07 블록 diff (데코 순서 경주)
  와 S-셋 타이밍의 함수 (R6 §2/§4).
- 고정점 vs 스케줄: **스케줄 재현 필요** (R6 §4 — ALT 조기 덤프는 링
  광원 유입 전의 비수렴 상태를 포착; sky 는 로컬 고정점이지만 경계
  유입이 스케줄 의존). 리플레이는 번들별 manifest/snapshots 순서를
  따른다.
