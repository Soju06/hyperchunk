# Task 13-close 완료 노트 (2026-08-04)

Phase 1 종결: unified 재캡처 골든 확정 + strict 기본 승격 + postProcessGeneration.
이 노트는 중단-재개 런의 산출까지 포함한 최종 판정 기록이다 (원 브리프:
/tmp/hyperchunk-task13-close-brief.md; 선행 SoT: task12-region/A-task12-completion).

## 결과 요약 (완료 기준 대비)

1. **골든 교차검증 PASS 증거** — ✅ (아래 §1; 로그 리포 보존)
2. **strict 기본 활성 풀 스위트 green** — ✅ 26/26 (HC_LIGHT_STRICT/HC_REGION_STRICT
   env 는 제거됨 — strict 가 코드 기본값)
3. **region 게이트 4/4 byte-exact** — ✅ c.0.0/c.1.0/c.0.1/c.1.1 페이로드 완전 일치
4. **postProcessGeneration 물/모래 패스 + 게이트 편입** — ✅ (아래 §4)
5. **check_no_fma.sh PASS** — ✅ (postprocess.c 포함 재확인)

라이트 09 미드 스냅샷: 36/36 중 **35 덤프 0-diff**, 1건 (primary 09 c.-1.1
sky 36셀) 은 실측 소진으로 **골든 아티팩트 판정** — 캡 고정 문서화 잔차 (§3b).

## §1 골든 교차검증 (재캡처 계보와 증거)

Task-13 재캡처는 세 번 있었다 (모두 2026-08-04, make_golden_unified.sh):

| 캡처 | gameTime | 처리 | 증거 |
|---|---|---|---|
| recapture-1 | 37 | 폐기 (설치 전 검증만) | /tmp/coherence-primary.log (03:07 PASS) |
| recapture-2 | 34 | 커밋 926611a | tools/golden/logs/coherence-primary2.log (03:23 PASS) |
| recapture-3 | 28 | **현행 골든** (이 커밋들) | tools/golden/logs/coherence-primary3.log (05:56 PASS, 0 warnings) |

recapture-3 가 존재하는 이유: stage-dump 모드에 stages.log **v2 제출(submission)
라인** (`s II name cx cz featuresSeq nanos`, ChunkStep.apply RETURN 훅) 이 추가돼
09 라이트 스케줄 재생의 입력이 됐기 때문. 골든 텍스트 번들과 모드 코드는 같은
커밋에 있어야 재현 가능하다 — 이 커밋이 그 원자 단위다.

coherence-primary3.log 요지 (check_capture_coherence.py, 워킹트리 번들 재실행):
LastUpdate 균일 28 (1024청크) == postprocess.manifest gameTime {28:1165};
block_ticks {0:94250, 2:3086, 5:82, 1:2} / fluid_ticks {5:10366, 0:1228};
PostProcessing 전 청크 빈 리스트; 재생 창 |c|<=4 는 1461 manifest 엔트리에
정확; 11_full-vs-mca 그리드 diff = c.1.0 (26,-21,12) air→water[level=1] 1셀
(postProcess-클래스, §4 구현으로 닫힘 — region 게이트가 증명). SHA256SUMS 전
324 엔트리 디스크 일치 (gitignored .mca 포함: a877f8f8…a18f4b).

주의: `tools/golden/logs/` 는 .gitignore(`logs/`) 대상이라 증거 로그 4건
(coherence-primary2/3, unified-primary3, unified-alt3) 은 `git add -f` 로 강제
트래킹했다. alt 번들은 mca 미캡처 (HYPERCHUNK_CAPTURE_MCA=0) — mca 교차검증은
구조적으로 primary 전용이고, alt 는 캡처 스크립트 자체검증 (unified-alt3.log:
81 applications, 99 snapshots 0 torn, 25 promotions each-once) + C 게이트가 담보.

## §2 strict 전환 결과

- `ctest` 풀 스위트 **26/26 green** — strict 가 기본값 (env 스위치 삭제):
  - test_region: 4/4 그리드 청크 canonical 페이로드 byte-exact 무조건 게이트.
    stale-mca 잔차 규율/봉투 (ec7e23d 시대) 폐기 — 코히런트 재캡처로 소멸.
  - test_light_stages: 두 번들 36 덤프 중 35 full 0-diff (blocks/hm/light 4계량
    전부) + primary 09 c.-1.1 sky<=36 문서화 아티팩트 캡 (초과 = FAIL).
    구 RESID 테이블 (9 덤프 잔차 캡, 2026-07-31 fallback) 삭제.
  - OF_WG 재프라임 잔차 캡 (<=4 컬럼, strict 와 무관한 별도 문서화 항목) 은 유지.
- ./scripts/check_no_fma.sh PASS (postprocess.c 포함 44,9xx 명령).

## §3 light 09 모델 v3 — stages.log v2 배치 재생

**모델**: 09 덤프 C 의 라이트 = lfp(블록@P(C), R, S). 라이트 워커(단일 스레드)의
큐-드레인 배치를 09 태스크의 제출/완료 나노로 시뮬레이션한다:

- 배치 멤버십: busy-드레인 (직전 배치 마지막 완료 나노, δ=0) / idle-기상
  (기상 제출 나노 + δ_wake 2ms). δ_wake 실측 제약: primary 09 burst 1.07ms 창
  + c.-2.-1 (+1.00ms) 포함 필요 (골든 enable 증거), alt c.0.-2 (+169µs after
  busy-드레인) 제외 필요 — busy/idle 구분이 두 요구를 동시에 만족시킨다.
- POST-실행가능성 병합: δ=0 이 쪼갠 인접 배치의 첫 09 POST 간격 < 2ms (청크
  flood PRE 물리 하한) 이면 PRE 페이즈가 낄 수 없으므로 병합 (실측 마진:
  primary 60µs vs alt 12.7ms).
- **08 은 배치 밖**: 08 완료가 09 POST 사이에 끼는 인터리빙이 관측된다 (예:
  09 POST 816.43/825.81ms 사이·직후 08 완료 825.85-831ms) — 08 은 별도 경로.
  R = {08 제출 < C 의 09 완료} (등록은 제출 즉시 유효 클래스), 단 P 프리픽스에
  deco 가 없는 청크의 08 은 블록과 일관되게 제외.
- S = {09 : 배치 <= batch(C)}.
- **P(C) = counter_at(drain(batch(C)))** — 전 스테이지 이벤트의 (nanos,
  featuresSeq) 타임라인 이분탐색. 데코는 배치 실행과 병행 진행되므로 P 가
  seq_begin 보다 이를 수 있다 (primary burst: P=49, seq_begin=56 — PRE 페이즈
  316ms 동안 데코 49..55 (전부 cz=3) 완료). 블록/하이트맵 비교는 seq_begin.
- stages.log nanos 는 파일 순서와 미세 역전 가능 (nanoTime 취득 후 락) —
  로드 후 정렬.

이 모델로 두 번들 35/36 full 0-diff (구모델 34/36; alt c.0.-1 의 lb 170셀 —
c.0.-2 lava emission 과포함 — 이 배치 제외로 닫혔고, c.-1.-1 도 (-2,-1)
포함으로 0-diff).

### §3b 문서화 아티팩트: primary 09 c.-1.1 sky 36셀

증상: (-16..-15, 64-65, 29-31) want 13-14 vs 수렴해 11-13 (want 가 밝음).
want 형상 = z-무구배, x-구배 15→14→13, y64(잎)/y65(공기) 동일값 — 서쪽 이웃
c.-2.1 동단 컬럼 (x=-17, z=29-31) 이 y64-65 에서 15 여야만 성립하는 기하.

실측 체인:
1. 그 컬럼들 위에는 오크 캐노피 (y69-74; 트렁크 (-16,68-69,30-31)) 가 있고,
   프로브 실측으로 **manifest 엔트리 <=44 산물** (pos 45 에 이미 존재) —
   모든 후보 09 flood (907ms+) 보다 >=130ms 앞선다. 어떤 수렴해도 캐노피를
   지나 12-14 를 넘지 못한다 (XRAY 실측).
2. 재생 공간 소진: 프리픽스 {제출 seq, 드레인 seq, seq_begin} × S {완료
   컷오프, δ=0 배치, 병합 배치} × R {완료 컷오프, 배치} 5회 게이트 런에서
   이 36셀은 단 한 셀도 움직이지 않았다 (다른 실패는 전부 닫힘).
3. 최종 라이트는 region 게이트 4/4 byte-exact (SkyLight 레이어 포함) — 즉
   이 상태는 미드-배치 과도상태에 국한된다.

판정: **레코딩 엔진의 크로스보더 스카이 유입 과도상태 (골든 아티팩트)** —
스테이지-이벤트 단위 기록으로는 재생 불가. 게이트는 정확히 이 덤프의
sky<=36 만 허용하고 그 외 어떤 잔차도 FAIL (test_light_stages.c 계상부 주석).

참고: 구모델의 "감소-이력 all-zero 라이트 레이어 2건 ((1,6)Y4/(4,10)Y4)" 은
재캡처-3 기준 재실측에서 소멸 확인 (region/light 게이트 green 에 포섭).

## §4 postProcessGeneration 구현 (R-D §3)

**core/src/postprocess.c + hc_postprocess.h** — LevelChunk.postProcessGeneration
등가 드레인:

- 드레인 순서: 섹션 오름차순 × ShortList append 순 (중복 유지). per pos:
  FluidState.tick (fall-through) → LiquidBlock 이면 state.tick (버블 컬럼) →
  아니면 updateFromNeighbourShapes 접기 (W,E,N,S,D,U) 후 참조-부등 시
  setBlock(276).
- 라이브 setBlock 체인: onPlace → updateNeighborsAt (flags&1; W,E,D,U,N,S;
  CollectingNeighborUpdater LIFO 스택 + 레이어 내 FIFO, MultiNeighborUpdate
  는 runNext 당 한 방향) → updateNeighbourShapes (flags&16==0; W,E,N,S,D,U;
  중첩 flags & -34).
- FlowingFluid 물 전체: tick/spread/getNewLiquid/spreadToSides (평가 N,E,S,W /
  적용 EnumMap ordinal N,S,W,E), 경사 DFS (slopeFindDistance 4, 조기 return),
  canPassThroughWall (충돌 형상 full/empty; 부분 형상 die), waterlog 컨테이너
  placeLiquid (NAMES 스캔 wl=false→true 치환 테이블), WATER_SOURCE_CONVERSION.
- 스케줄: hc_feat_schedule_tick 재사용 (first-wins dedup = 바닐라 청크 컨테이너
  동일 키) — t=5 물 / t=2 모래·자갈 / t=1 잎 / t=20 버블 예약.
- 게이트 밖 리스크는 전부 fail-loud die: 용암 상태 변화/스프레드, 부분 충돌
  형상, EMPTY getNewLiquid 경로 (canBeReplacedWith(EMPTY) 물기둥-소거),
  wl=true 미등재 placeLiquid, 언프로즌 레코더 드레인.

**마킹 생산자**: aquifer shouldScheduleFluidUpdate/doFill (gen_noise_stage.c),
WorldCarver (carvers.c), LakeFeature/spring/MultifaceGrowth 등 (features*.c,
surface.c) — hc_ppg_recorder_t (hc_chunk.h; 지역 공유 시간순 로그, 청크별
복원 = 청크 필터 → 섹션 stable sort). 바닐라 flag-19 (KNOWN_SHAPE) 쓰기는
마킹하지 않는다 — features_tree.c 5개 사이트를 hc_feat_set_block_ks 로 분리
(실측 반례: attached_to_logs 머시룸 (-24,69,-10), c.-2.-1 ours 28 vs golden 27).

**신규 블록 상태**: water[level=1..15] (blocks.c 등재, 마스크 워드 갱신) —
light_engine.c 테이블은 predicate-파생이라 자동 (hc_block_fluid_nonempty 확장).

**게이트 편입** (test_region.c): manifest 재생 → 유도 마킹 vs golden
postprocess.manifest 'p' 라인 **좌표·순서·중복 교차검증 (fail-loud, |c|<=3
창 34청크 3585 마킹)** → 기록 승격 순서로 hc_postprocess_chunk 드레인 →
라이트 lfp → 직렬화. 수용 기준이던 c.1.0 물 스프레드 1셀 (26,-21,12) +
t=5 fluid_ticks 8행이 strict 하에서 byte-exact 로 닫혔다.

미배선 진단: hc_postprocess_unmodeled_veg_evals() (지지-상실 근사 클래스
평가 횟수) — region 게이트 런에서 로그로 방출, 게이트 green 이면 커버.

## §5 이 런에서 잡은 이전-런 결함

- postprocess.c:536 `flags & ~34` → `& -34` (Java -34 == ~33; C ~34 는 비트 2
  를 지우는 오역). 게이트 창 내 바이트 무변화 실측 (해당 경로 미발화) —
  하지만 중첩 SHAPE 이벤트가 블록을 바꾸는 순간 MULTI 연쇄가 갈라지는 클래스.
- FL_NONE→FL_WATER 치환 2곳 → 도달-불가 증명 + fail-loud die 로 교체.
- test_region PPG 카운트 불일치 진단 덤프 무상한 → 40행 캡.
- make_golden_unified.sh: stages.log 검사를 v2 명시 (제출 라인 카운트 분리,
  그리드 제출 라인 1회 검증), 설치 게이트에 tee + PIPESTATUS 가이드 (이전
  런의 `| tail -9` 파이프라인이 coherence FAIL 을 삼킬 수 있었다).

## §6 커버리지 경계 (이 게이트들이 못 보는 것)

- **postProcess**: 용암 스프레드/변환 (t=30, 리전 내 0건 — die 가드),
  soul_sand 버블 (미등재), 부분 충돌 형상 canPassThroughWall, EMPTY
  getNewLiquid 물기둥-소거, 초목 측면 지지 규칙 (수련잎 등 — 근사 클래스,
  진단 카운터). 발화 시 즉사 (조용한 오염 없음).
- **light 09 모델**: δ_wake(2ms)/T_PRE_MIN(2ms) 은 이 두 캡처의 실측 제약으로
  핀 — 다른 하드웨어/부하의 재캡처에서 재측정 대상. 드레인 나노 자체는
  미기록 관측 한계 (§3b 아티팩트의 근원).
- **비그리드 청크 직렬화 / 1024청크 canonical / 구조물 파이프라인 /
  structures.starts HashMap identity-order**: Task 13 비목표 그대로 (다음
  태스크; 핸드오프 §Task 13 항목 2·3 참조).
- alt 번들은 mca-측 교차검증 구조적 불가 (캡처 설계).

## §7 커밋 구성

1. `golden+light`: recapture-3 재스테이징 (양 번들 텍스트+SUMS) + stage-dump
   모드 v2 + 캡처 스크립트 v2 검사 + coherence 로그 강제 트래킹 +
   test_light_stages v2 소비자/배치 모델/strict 기본 (원자 단위 — v1 파서는
   v2 로그에, v2 파서는 v1 로그에 각각 die).
2. `postprocess+region-strict`: core postProcess 구현 전체 + 마킹 생산자 +
   water[level] 등재 + test_region 드레인 페이즈/마킹 교차검증/4-4 strict.
