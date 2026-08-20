# Task 12 완료 상태 + Task 13 핸드오프 (2026-07-31)

## Task 12 결과 요약

산출물 (전부 무의존성, ADR-003 D1):

- **core/src/nbt.c + hc_nbt.h** — 빅엔디언 NBT 라이터. 컴파운드 방출
  순서 = java.util.HashMap(cap 16, LF .75) 순회 에뮬레이션 (버킷
  오름차순 + 버킷 내 삽입 순서; 최종 캐퍼시티만 모델하면 충분 — 리사이즈
  split 이 체인 상대 순서를 보존). **golden r.0.0.mca 1024청크 전
  컴파운드의 키 순서를 재현함을 전수 실측** (루트 15키/섹션 4변형/팔레트
  엔트리/Heightmaps/structures/틱 엔트리 + Properties 89/89 블록 —
  Properties 삽입은 이름 내림차순 = PairMapCodec 좌중첩 접힘의 역방출,
  R-E §4). 단위 게이트: ctest `nbt`.
- **core/src/chunk_nbt.c + hc_chunk_nbt.h** — 26.2 청크 직렬화기.
  방출 시퀀스 = SerializableChunkData.write put 순서 (R-C §2), 섹션
  루프 = 라이트 섹션 -5..20 (블록 범위는 block_states+biomes 무조건,
  경계 섹션은 라이트 있을 때만), 팔레트 저장-시-재팩 (인덱스 스캔
  첫-등장 순, 블록 bits=max(4,ceillog2 n)/바이옴 무클램프, n==1 data
  생략, LSB-first 무스팬 — **재팩 규칙은 golden 24,576 섹션 x 블록+바이옴
  전수 재패킹 항등으로 사전 검증**), 하이트맵 9bit/7퍼롱/37롱
  (raw = 최고 블로킹 y+1+64), 라이트 방출 술어 = "등록(비공기 섹션의
  26-이웃 팽창) && 최종값>0 존재" (R-E §7f; DataLayer.isEmpty =
  data==null && defv==0 — 월드젠 increase-only 이력에선 materialized ⇔
  값>0 존재).
- **틱 레코더** (hc_feat_region_t.ticks, features.c) — ProtoChunkTicks
  등가: first-wins 중복 제거 키 (블록타입, pos), 저장 t=0 고정
  (ProtoChunkTicks.schedule @12 iconst_0 — delay 1 스케줄도 t=0 저장),
  청크별 시간순 방출 (LevelChunkTicks.pack 은 subTickOrder 재정렬이라
  save/load/FULL 경유 불변, R-D §4). 스케줄 지점: LeavesBlock.updateShape
  (wl → 물틱 @0-29; getDistanceAt(ns)+1 != 1 || 자기 distance != 1 →
  블록틱 @34-72), LiquidBlock.updateShape (소스 → getType 틱),
  spring/lake(cave_air)/geode crack. 기존 게이트 무영향 (NULL = off).
- **core/src/region.c + hc_region.h** — .mca 이미지 조립: stored-block
  DEFLATE + adler32 수제 zlib 컨테이너 (ADR-003 D1 무의존), 4096 섹터,
  length=압축+1, type 2.
- **게이트**: ctest `region_out` (04..06 → primary manifest 81엔트리 풀
  재생 + 틱 기록 → 라이트 최종 고정점 → 4청크 직렬화, (0,0) 바이트
  일치 강제) + `region_out_roundtrip` (컨테이너 왕복 무손실 + (0,0)
  canonical 프래그먼트) + `region_out_residuals` (아래 stale-mca 잔차
  봉투 강제). `HC_REGION_STRICT=1` = 4/4 바이트 게이트 (골든 재캡처 후).
- 참조 페이로드: tools/golden/extract_region_ref.py →
  golden/region-ref/c.{x}.{z}.nbt (LastUpdate 마스킹본, 로컬 전용 —
  .gitignore; 해시는 golden/SHA256SUMS 트래킹).

### 게이트 결과 (2026-07-31)

- **c.0.0: 100081 바이트 완전 일치** — 15키 스키마, 24 섹션 팔레트/데이터,
  하이트맵 4종, SkyLight 4 + BlockLight 4 레이어, block_ticks 681
  (시간순), PostProcessing/structures/빈 리스트 프레이밍 전부. 직렬화
  스택 전 요소가 한 청크에서 end-to-end 검증됨.
- c.1.0 / c.0.1 / c.1.1: 문서화된 stale-mca 잔차 (아래) 이내.

## 발견: golden .mca 는 별개 런의 산물이다 (stale-mca)

**git 증거**: `golden/SHA256SUMS` 의 mca 라인 (`ea3fd9…#canonical-payload`)
은 커밋 4fc5d30 (**2026-07-28**, make_golden.sh 캡처) 이후 불변.
스테이지 번들은 2f02df8/9529ea5 (**2026-07-31**) 재기록. 즉 .mca 런의
features 순서는 **manifest 이전 시대 — 미기록**이다.

**역학 증거** (순서-교차 불가능성):

1. c.1.1 의 block_ticks 549..557: 골든은 (1,2)-트리 틱이 (2,1)-트리
   틱보다 앞 — 기록된 manifest 는 (2,1)@32 < (1,2)@46 이라 재생으로
   불가능한 순서.
2. c.1.1 의 화강암 22셀 (x=31 평면): 인접 데코는 전부 seq<49 인데
   11_full@49 덤프에 없음 + 49 이후 인접 데코 부재 → 기록 세션 내
   불가능.
3. 링 청크 최종 상태 vs mca: 그리드 거리별 증가 (c.2.0 49셀 → c.2.2
   1434셀) — 순서-민감 피처 (나무/덩굴/패치, discard_on_air 광맥) +
   .mca 런의 postProcessGeneration 산물 (bubble_column 60셀 등).

(0,0) 이 그래도 일치하는 이유: NOTES.md 실측 "1-스레드 런들의 그리드
데코 프리픽스는 순서-고착 (old 번들 포함)" — (0,0) 의 영향 원뿔 전체가
고착 프리픽스 안이다. (1,0) 도 postProcess 물 1건 외 전부 고착.

### 3청크 잔차 전수 (실측, check_region_residuals.py ENVELOPE)

- **c.1.0**: 골든에만 fluid_ticks 8건 (t=5 — .mca 런의 게임타임-8
  postProcessGeneration 라이브 패스, R-D §3) + 물 스프레드 1셀
  (26,-21,12 air→water[level=1], 섹션 Y=-2 팔레트+데이터). 이 중
  (25,-21,12)→(26) 스프레드와 그 2틱은 R-D 역학으로 유도 가능하지만,
  (31,-32..-27,0) 컬럼 6틱은 **주변 상태 바이트 동일 조건에서 핀 다운된
  바닐라 역학 어느 경로로도 유도 불가** — .mca 런의 과도 상태 산물로
  판정 (구현으로 못 닫는다).
- **c.0.1**: 초목 3셀 (fern, 섹션 Y=4 데이터 3롱 + WORLD_SURFACE 1롱;
  block_ticks 419건은 완전 일치 — 링 패치의 상태-조건부 드로우 연쇄가
  런마다 다름) + **우리에게만 SkyLight Y=7 레이어** (전부 0xFF): 우리
  번들-순서 재생의 (1,2) 정글나무 y=97 (섹션 6) 이 26-이웃 등록 팽창으로
  (0,1) 컬럼 topSections=8 을 만들고 채움 패스가 materialize — .mca 런의
  (1,2) 는 캐노피가 낮았다 (jungle_leaves→air 179셀). 방출 술어 자체는
  바닐라 규칙 그대로.
- **c.1.1**: 화강암 22셀 (섹션 Y=3) + short_grass 1셀 (Y=4) +
  WORLD_SURFACE 1롱 + block_ticks 9엔트리 창 순서 전위 (멀티셋 동일).

## Task 13 핸드오프 — 풀 리전으로 가는 길

**전제 경고: 현 .mca 를 상대로 1024청크 canonical 해시 (ea3fd9…) 는
도달 불가능하다.** 위 잔차가 리전 전체로는 수천 셀 스케일이고 (링
전수조사), .mca 런의 순서는 기록이 없다. **Task 13 은 골든 재캡처가
선행 조건이다**: 한 서버 세션에서 (a) r.0.0 풀 커버 .mca, (b) 그
세션의 order.manifest/snapshots, (c) 스테이지 덤프, (d) autosave 무효화
(광 스테이지 mid-carve 레이스 제거 — tools/golden/NOTES.md 권고와 동일
캡처에서 해결) 를 함께 뽑아야 한다. 그러면 HC_REGION_STRICT=1 로 현
게이트가 그대로 4/4 바이트 게이트가 된다.

재캡처와 독립적으로 필요한 신규 메커니즘 (이번 런에서 바이트코드 핀
완료, R-C/R-D/R-E):

1. **postProcessGeneration** (틱킹 승격 시, R-D §3): 마킹 위치별로
   유체 → FlowingFluid.tick (스프레드 + neighborChanged/onPlace 의
   t=5 스케줄; getSpreadDelay=getTickDelay=5), LiquidBlock →
   bubble_column 갱신, 그 외 → updateFromNeighbourShapes+setBlock(276)
   (모래/자갈 t=2 스케줄 3,076건의 출처). 처리 후 ShortList clear —
   골든 전 청크 PostProcessing 이 빈 이유. 마킹 생산자: aquifer
   shouldScheduleFluidUpdate (doFill), WorldCarver, LakeFeature,
   MultifaceGrowth 등 — 우리 스테이지에 마킹 기록 추가 필요. 승격
   순서는 리전 단위 재캡처 시 기록 대상에 포함시킬 것 (fluid_ticks
   리스트 순서가 여기 걸린다).
2. **구조물 파이프라인**: 리전 전체 195 블록엔티티/49청크 (전부 구조물
   — trial_chambers/dungeon/shipwreck), starts 1020청크 빈 / 4청크
   비어있지 않음, References LongArray. structures.starts 의 컴파운드
   키는 HashMap identity-order 문제 (R-C §8: Structure 키 HashMap —
   JVM 런 의존!) — 재캡처 후 실측 순서 고정 필요.
3. **비그리드 청크 생성**: 04..06 은 순수 (전 청크 재생 가능); 07+ 는
   재캡처 manifest 재생. block_entities 직렬화 (getBlockEntityNbtForSaving
   — 스포너/컨테이너 NBT), UpgradeData 부재 확인 완료.
4. **water[level=1..] 상태**: postProcess 스프레드 산물 — blocks.c 에
   미등재 (현재 water[level=0] 뿐). 등재 시 마스크 워드 수 (448→449+)
   와 light_engine.c 의 g_damp/g_emit 테이블 갱신 동반.
5. 컨테이너: 26.2 라이터의 섹터 배치/deflate 는 canonical 게이트 무관
   (자유) — 우리 stored-block 프레이밍으로 충분함을 roundtrip 게이트가
   증명. 스테일 가비지 6청크 (idx 32/33/65/456/457/481) 도 무관.

### 이번 런이 확정한 직렬화 불변식 (재캡처 후에도 그대로 유효)

- 키 방출 = HashMap 에뮬레이션 (nbt.c 헤더 주석; 삽입 순서 충돌쌍:
  zPos<block_entities, block_states<SkyLight, BlockLight<Y, i<y).
- 팔레트 재팩 = 첫-등장 스캔 (24,576/24,576 섹션 항등).
- 라이트 방출 = 등록 && 값>0 (경계: 감소-이력 all-zero 레이어는 리전
  전체 2건 (1,6)Y4/(4,10)Y4 — 우리 배치 솔버로 재현 불가, 재캡처 시
  자동 소멸 예상 아님 — Task 13 에서 재실측).
- 틱: t=0(월드젠)/t=5(포스트프로세스 물)/t=2(포스트프로세스 모래),
  엔트리 키 바이트 순서 p,t,x,i,y,z, 방출 = 청크별 스케줄 시간순.
- LastUpdate 8 = 게임타임 동결 (전 청크 동일; 레코딩 하네스 gamerule).

## 커버리지 경계 (이 게이트가 못 보는 것)

- postProcessGeneration 전 클래스 (미구현 — 위 1).
- 감소-이력 라이트 레이어 (all-zero materialized) 방출.
- 비그리드 청크의 직렬화 (라이트/틱 상태가 게이트 창 밖).
- .mca 런 고유 순서 산물 전부 (잔차 봉투로만 상한 고정).
