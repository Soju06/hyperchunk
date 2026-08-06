# Task 14 완료 노트 — 풀 리전 canonical 게이트 (2026-08-06, RESUME-8)

## 최종 상태

- **풀 리전 canonical 게이트 PASS**: r.0.0 전체 1024청크 페이로드
  byte-exact, canonical sha256 ==
  `a59632059bab42d808a24c27091d4b8786b5615405263a84ec1ba9a193223c24`
  (golden/SHA256SUMS `#canonical-payload`).
- ctest **30/30 green** (strict `full_region` 포함, 319.8s).
- `scripts/check_no_fma.sh` PASS (libhyperchunk.a 67,129 insn).
- `scripts/parity_gate.sh` / `cli/hyperchunk-verify` PASS:
  "PASS: bit-exact parity" — CLI 는 test_full_region.c 의 현행
  파이프라인 (라이브-창 라이트 + 배치 인터리브) 으로 동기화 (a868006).
- 블록 잔차 0셀 / PPG 마크 교차검증 1165/1165 (순서까지) / BE·틱·라이트
  전 성분 0 diff.

측정 궤적 (HC_SURVEY): 시작 8,934셀·455청크 → RESUME-5 말 131셀·211청크
→ RESUME-7 (라이트 sky 전소) 후 75셀·44청크 → **RESUME-8: 0셀·0청크**.

## 잔차 소진 이력 (RESUME-8 구간; 커밋 = 1차 기록)

각 클래스는 [셀 좌표 실측 → 원인 판정 → 26.2 디컴파일 핀 → 수정 →
풀-리전 재측정] 사이클로 닫았다. 아티팩트 캡·envelope 없음 — 전 클래스
실측 소진.

1. **버섯/초지/PPG 마크 (54셀 + 마크 3청크)** — 7f99794.
   MushroomBlock.canSurvive 의 getRawBrightness 는 라이트 엔진 visible
   스냅샷 (3×3 이웃 08 완료 전 = 스토리지-위 규칙 sky 15). "월드젠 중
   0" 가정 폐기, rg->raw_brightness 콜백 도입. updateShape 의
   canSurvive 폴드 (grass/fern/버섯 4종 매핑 정확 평가) 로 이웃 disk
   sand/gravel 치환 마크 셀의 초지 42+9셀 소진.
2. **마인샤프트 행잉/기둥 (3셀)** — c85a5bb. canHangChainBelow =
   canSupportCenter(DOWN) = isFaceSturdy(DOWN, SupportType.**CENTER**)
   — FULL 이 아니다. 펜스 포스트 (6..10px)/체인이 CENTER 통과 → 판자
   위 지지 펜스에서 dist=1 행잉 즉시 종료 (체인 구간 공집합 no-op).
   실측: c.31.1 (508,21..23,23) ours 오판 wood 기둥 vs 골든 cave_air.
3. **마인샤프트 펜스 연결 (28셀)** — c85a5bb + 89fc08f. 펜스는
   west/east=true 구운 상태로 배치되고 placeBlock 이 SHAPE_CHECK_BLOCKS
   마킹 → 승격 폴드 (updateFromNeighbourShapes) 가 이웃 기준 재계산.
   update_shape 에 FenceBlock.updateShape 추가 (connectsTo = 같은
   wooden fence || (!isExceptionForConnection && sturdy)). 폴드가
   임의 연결 조합을 산출 → oak_fence 32조합 폐포 등록 (tsv4 append,
   테이블 914→940, 기존 id 불변).
4. **wall_torch 소실 (6셀) + cave_vines/vine/dripleaf 소실 (22셀)** —
   622f8a3. is_plantish 지지-상실 근사 ("아래 공기/유체 → 파괴") 가
   행잉/부착 식물을 오파괴 (PPVEGKILL 진단 23건 = 소실 셀 전수 대응).
   정확 분기: wall_torch (부착 방향 && !canSurvive → AIR), cave_vines
   (지지=위, dir UP && !canSurvive → scheduleTick(1); head↔body 변환은
   이력 부재 fail-loud), vine (DOWN 불변), big_dripleaf(_stem)
   (canSurvive — 줄기-위-줄기 wl 포함 정당 지지).
5. **스포너-인접 펜스 (2셀)** — 622f8a3. isFaceSturdy 지지 형상은 충돌
   기반: noOcclusion 풀-충돌 (spawner) 도 sturdy — pp_face_sturdy 공용화
   (폐색 풀큐브 + collision_full − 잎).
6. **glow_lichen 스프레드 발산 (4셀 + c.16.11 마크 순서)** — 4f78f55.
   MultifaceBlock.canAttachTo = 지지면 완전 **||** 충돌면 완전 —
   waxed_copper_grate (트라이얼) 위 down 부착을 occlusion 판정이 거부해
   스프레드가 다른 면으로 발산. mf_attach_ok/can_attach_to/lichen 폴드
   3곳 collision-full 일반화.
7. **정글 캐노피 연쇄 (7셀: 잎 distance 4 + 버섯 3)** — 4f78f55.
   features_tree.c 버섯 엣지-업데이트의 "rawBrightness 0 (생존)" 가정을
   hc_featx_can_survive (visible 스냅샷) 접기로 교체 — 실제 밝기 ≥13 →
   바닐라 버섯 사멸 → 잎이 그 자리를 차지하는 연쇄까지 일치.
8. **tall_seagrass (1셀 + fluid_ticks)** — 4f78f55 + f7d5541.
   (a) DoublePlant 폴드 정확 구현: 마크된 lower 만 magma 위에서 소거
   (드레인 276 플래그 → upper 는 부유 잔존 — 골든과 동일).
   (b) 엣지 SimpleWaterlogged 폴백을 waterlogged=true **프로퍼티** 보유
   상태로 좁힘 — F_WLOG (고유 유체 포함) 기반이 tall_seagrass 에 바닐라에
   없는 물 틱을 걸었다.
9. **BE 저장 순서 (4청크, 인접쌍 스왑)** — 7d65fbb. LevelChunk 생성자의
   proto→level 복사는 fastutil 순회 순서 재삽입 — 오픈 어드레싱 충돌쌍
   (같은 홈 슬롯) 의 상대 순서가 복사마다 **반전**된다 (이전 "재삽입
   보존" 가정 오류). 순열 역산 (720/40320 전수) 으로 이중-패스만 골든
   재현 확인. o2omap 2회 (proto 순회 → 복사 → level 순회) + jset.
10. **SkyLight 상단 all-15 섹션 (3청크)** — 7195f30. 바닐라
    createDataLayer 의 isAboveData: 기존 데이터 위 신규 레이어는 15 로
    채워 생성 — 늦은 트리/이웃 스필의 일시적 checkBlock 쓰기가 레이어를
    실체화하고 값 복원 후 all-15 로 저장에 남는다. 시딩-후(seeded)
    상단-확장 등록에만 채움 (시딩 전 채우면 캐노피 음영을 덮는다 —
    c.21.27 실측 회귀로 조건 확정).
11. **brown_mushroom 발광 + block 레이어 실체화 (2청크)** — e4e9de2.
    (a) 발광 테이블에 brown_mushroom=1 누락 (26.2 Blocks.java:820
    lightLevel(s->1) 핀; c.4.9 골든 니블 1×2 = 버섯 자기-셀).
    (b) block DataLayer 는 >0 쓰기 시 생성되고 all-0 감쇠 후에도 저장에
    남는다 (c.4.10: 버섯 배치-발광 후 엣지-사멸 잔재) — blk_written
    실체화 추적 도입, block 방출 규칙을 "등록 && 최종값>0" 에서 교체.
12. **placeLiquid 틱 (1청크)** — e4e9de2. FlowingFluid.spreadTo →
    placeLiquid = set wl=true + scheduleTick(WATER, 5). PP 물 스프레드가
    dripleaf 줄기를 워터로깅하며 거는 틱 (c.4.21 (64,-18,350) t=5).

### 기각된 가설 (실측 근거 보존)

- **틱 딜레이 바닐라 값 (물 5/용암 30/버블 20)**: 월드젠(proto) 경로는
  ProtoChunkTicks 가 SavedTick delay 를 0 으로 정규화 — 딜레이를
  바닐라 값으로 바꾸면 잔차 14→57 회귀 (survey7). 리코더 규약 "월드젠
  경로 = 0" 이 골든과 일치. t=5 저장 틱은 전부 레벨-시점 (PP 드레인
  캐스케이드/placeLiquid) 스케줄.
- **BE live 맵 = java HashMap**: 4청크 우연 일치였고 실제는 fastutil
  (ChunkAccess 디컴파일 핀) — 복사 반전 이중-패스가 정답.

## 구조물 starts 순서 근거

- **스타트 소스 3분류** (structures.c 상단 주석): in-region 골든 starts
  NBT 4건 + 이웃 스타트 프래그먼트 (extract_neighbor_start.py, coherence
  가드) + mineshaft 파생 15건 (스캔 창 -12..43).
- **references 산출/배치 순서 = LongOpenHashSet 순회 등가**
  (hc_longset_to_array): 기본 32슬롯, 전방 프로브, 방출 = [0(있으면)] +
  슬롯 내림차순. 리전-와이드 references 교차검증 (양방향, 순서 포함) 이
  매 런 게이트로 동작.
- **같은 (step, step_index) 의 다중 스타트** (mineshaft): 청크별 수집
  순서 = references 산식과 같은 스캔 (sx 외측, sz 내측) 후
  LongOpenHashSet 순회 순서로 재배열, 공유 RNG 스트림
  (setFeatureSeed(deco_seed, idx, step)) 을 그 순서로 소비.
- **BE 직렬화 순서** = 실체화(기록) 순서 → proto fastutil 맵 → 생성자
  복사 (충돌쌍 반전) → level fastutil 맵 (+ DUMMY 는 pending HashMap
  승격순 꼬리) → fresh HashSet 순회 (R-serialization §4.4/4.5 + 이번
  이중-패스 수정).

## 커버리지 경계 (이 골든이 실행하지 않는 경로)

기존 스테이지별 경계 (noise/surface/carvers/features/light/spawn 메모리
및 각 완료 노트) 에 더해, 이번 태스크에서 명시된 것:

- **repeatFirstLayer** (기존 등록 아래 in-fill 섹션 생성, 상단 슬라이스
  nonzero): 이 리전은 등록이 바닥-고정 연속이라 미도달 — fail-loud.
- **cave_vines head↔body 변환** (PP 폴드 dir DOWN 에서 아래가
  식물/비식물로 바뀌는 경우): 지역 랜덤 age 드로우 필요 — 이력 부재,
  fail-loud.
- **vine 면 재계산** (PP 폴드 비-DOWN 방향): getUpdatedState 미모델 —
  관측 이벤트 전부 아래-방향이라 불변 반환이 바닐라와 일치. 잔차가
  지목하면 면 단위 재계산 구현 필요.
- **kelp head/body 전환·지지 상실** (PP): fail-loud 유지.
- **CENTER 지지의 일반 형상** (ms_can_hang_chain_below): 풀큐브 +
  oak_fence/iron_chain 허용 목록 — 판/벽/모루 등 CENTER≠FULL 상태는 이
  리전 탐색 경로 부재. soul_sand 의 getBlockSupportShape FULL
  오버라이드도 미모델 (팔레트 부재).
- **hasSturdyNeighbours (거미줄) 의 noOcclusion 풀-충돌**: 기존
  FULL-기반 유지 (거미줄 잔차 부재) — 펜스 폴드/토치와 달리 미일반화.
- **PPG 마크 4096 초과 / 프로토 재해시** (o2omap 32슬롯 가정 n≤24):
  assert 로 고정.
- **트라이얼 (40,4) 및 데코 창 밖 구조물**: RESUME-5 노트 "미포함 판정"
  절 참조 (관측 불가 판정 근거 포함).
- **마진 링 (r.0.0 밖 657청크)**: 바이옴은 기록 오버레이, 블록/라이트는
  게이트 비교 대상 아님 (해시는 r.0.0 1024청크만).

## 재사용 인프라 (이번 세션 추가분)

- `HC_TRACE_NEAR="targets.txt out.txt"` (test_full_region): 잔차 셀 근방
  배치 이벤트 → 피처 소속 규명.
- `HC_PP_DEBUG_VEG=1`: plantish 폴백 파괴 로그 (PPVEGKILL).
- 서베이 워크플로: 덤프는 불일치 청크만 쓰이므로 **런 전에
  `tr.mca.c.*.ours.nbt` 를 반드시 삭제** — 스테일 덤프가 셀 분류를
  오염시킨 실사고 있음 (RESUME-8 초반).
- NBT 성분 분류: tools/golden/mca.py 의 parse_nbt+nbt_diff+
  mask_last_update 조합 (대화 로그의 스니펫).

## 남은 로컬-전용 자산

- golden/region-ref, *-margin, 골든 starts/구조물 덤프, mca 산출물은
  여전히 로컬-전용 (gitignore) — region-gate-stale-mca 메모리 참조.
- /tmp/t14r8/ (서베이 로그 survey1..13, 분석 스크립트) 은 휘발성.
