# P2-3 — FREE 모드 병렬 스케줄러 완료 노트 (2026-08-06)

P2-0 §8 P2-3 행: `hc_schedule_policy` 주입점 (ADR-008), features 웨이브
프런트, light09 병렬화, serialize 병렬화, TSan 게이트 신설, FREE-vs-REPLAY
해시 동일성 게이트. **절대치는 이 VM(claw) 참고치, 유효한 것은 비중·배율**
(P2-0 §0 동일 주의).

## TL;DR

- **gen wall 8203.2 ms (REPLAY) → 3455.7 ms (FREE) = 2.37x** (-O3, 20스레드,
  3런 중앙값, 정렬-고정 빌드 쌍, 매 런 canonical PASS). 직렬이던 데코+
  라이트 구간(5196 ms 상당)이 이벤트-DAG 597 ms 로 (8.7x, DAG 내부 병렬도
  11.6x), serialize 289→91 ms (3.2x). REPLAY 회귀 없음 (8203.2→8223.5,
  +0.25% < 런 편차 0.7~0.9%).
- 스케줄러는 코어 라이브러리 계약 (`hc_sched.h`, ADR-008 Pitfall 1):
  이벤트 = base 선형화 + 접촉 셀 신고, FREE = **셀별 FIFO 로 충돌쌍의
  상대순서만 base 로 고정** — Phase 1 A0 §1.3 "disjoint ⇒ commute" 의
  스케줄러화. 순서-민감 클래스(블록/하이트맵/틱/BE/ppg/피스 래치/라이트
  이력)는 회피가 아니라 **이 최소 동기화로 순서를 고정**해서 처리한다.
- 신규 게이트 4종 전부 green: `sched`(속성), `free_region_golden`(골든
  이력 FREE == 골든 canonical), `free_region_own`(자체 이력 REPLAY==FREE),
  **TSan 풀 스위트** (34/34 클린 — ADR-009 Pitfall 3 이행). 기존 게이트
  무손대 (ctest 31→34, parity_gate 3빌드, check_no_fma, check_sanitizers).
- 핵심 실측 발견: **골든 manifest 는 공간 크롤** (연속 엔트리의 58%가
  체비셰프 거리 1) — 골든 순서를 base 로 한 FREE 는 폭이 없다 (실측
  ~1.0x; 게이트 전용). 성능은 **자체 순서 σ\* (5×5 잔차 체스판 —
  ADR-003/008 이 명시한 그 체스판)** 가 담당하고, 이건 바닐라 규칙상
  똑같이 유효한 이력이다 (§2.4).

## 1. 설계

### 1.1 이벤트 모델 (base 선형화 + 셀 신고)

REPLAY 루프(소스 오브 트루스 test_full_region.c:1198-1244)가 실행하는
정확한 순서를 이벤트 열로 편다:

```
pos 마다: [L08 (P08<=pos)] [PREPARE + 멤버 L09 (bat_P<=pos)] [DECO pos]
DECO = hc_gen_features_chunk + set_featured + accum_flush(이벤트-로컬 ctx)
```

셀 신고 (= 읽기/쓰기 풋프린트 커버, hc_sched.h 계약):

| 이벤트 | 셀 | 근거 (코드 유도) |
|---|---|---|
| DECO | center±2 + 피스 그룹 | 블록 쓰기 ±1 (ensureCanWrite 클램프) + flush 라이트 헤일로 ±1 (BFS ≤14블록, run_decreases 포함) = ±2; 읽기(블록/hm/biome ±1, 라이트 ±2) ⊆ 동일 |
| L08 | ±1 | derive_geometry 마스크 스필 ±1 + fill_src_y 자기 블록 |
| L09 | ±1 | flood 쓰기 ⊆ ±1, nb_src/블록 읽기 ±1 |
| PREPARE | 전역 배리어 | §3.2 — in_r 전 청크 재유도가 관측면 |
| 피스 그룹 | 가상 셀 | 같은 구조물 피스 BB 에 닿는 데코들 (spider_placed/shipwreck 래치, BE 순서 — 피스가 반경 4 를 넘을 수 있어 반경 규칙 밖 결합) |

FREE = 셀별 FIFO: 이벤트는 자신이 접촉하는 모든 셀의 머리가 됐을 때만
실행. 충돌쌍(셀 공유)은 시간상 겹치지 않고 base 순서로 직렬화되며
pthread mutex/cond 가 happens-before 를 부여한다 → **셀 신고가 풋프린트를
덮는 한 결과는 REPLAY 와 비트 동일** (레이스도 순서 재량도 없음).
REPLAY 정책 = base 순서 순차 실행 (기존 루프와 정의상 동일).

### 1.2 순서-민감 클래스별 "최소 동기화" (과제 §패리티 불변 요구 항목)

ADR-007 이 실측한 순서-민감 채널 전부의 처리:

| 클래스 (Phase 1 A4 §7.1) | 처리 |
|---|---|
| 블록/FINAL 하이트맵 (스필오버 + lazy prime) | 같은 청크의 접근자는 전부 거리 ≤2 = 충돌쌍 → 셀 FIFO 가 base 순서 직렬화 |
| 틱 레코더 (전역 append + first-wins dedup) | 직렬화가 읽는 관측면은 **청크별 프로젝션**뿐 (chunk_nbt.c:481-489 필터) — 같은 청크에 틱을 쓰는 데코는 ±1 이웃 = 충돌쌍이라 순서 고정; 같은 키(pos) dedup 도 같은 논리. 물리 append 만 스핀락 (hc_sync.h) |
| BE 레코더 (dead-mark + append, 저장 순서 재구성) | bes_for_chunk 는 청크 필터 후 상대순서만 사용 (structures.c:1276-1284) — 동일 논리 + 스핀락. shipwreck 의 mid-walk 레코더 읽기(loot 배정+RNG 소비)도 같은 pos = 같은 컬럼 = 충돌쌍 |
| ppg 레코더 (교차-청크 append) | 같은 recorder 의 writer 들은 서로 ≤2 → DAG 가 이미 직렬화, 락 불요. golden-free 게이트가 마킹 교차검증으로 직접 증명 |
| 구조물 피스 래치 (spider_placed, shipwreck height) | 피스-그룹 가상 셀 (base 순서 체인) |
| 라이트 이력 (08 src_y 동결·배치 카운터·flush 감소 기계) | PREPARE 전역 배리어 + L08/L09 ±1 셀 + 이벤트-로컬 hc_light_ctx_t + 라이브-창 게이트를 이벤트 데이터(cur_pos)로 |

### 1.3 FREE-vs-REPLAY 게이트의 두 구성

바닐라가 자기 자신과도 순서-비결정 (ADR-007) 이므로 "FREE == REPLAY" 는
**같은 이력에 대한** 등식으로 정의된다:

- **(A) free_region_golden**: 골든 이력 (σ_g, Λ_g) 을 FREE 로 실행 →
  canonical == 골든 상수. REPLAY==상수는 full_region 이 상시 증명하므로
  FREE==REPLAY 가 전이 성립. pp 마킹 교차검증 포함 (FREE 의 마킹
  프로젝션도 골든 기록과 좌표·순서·중복까지 일치).
- **(B) free_region_own**: 자체 이력 (σ\*, Λ\*, π\*) 을 REPLAY 와 FREE 로
  두 번 실행해 canonical 동일 판정 — "**병렬로 돌려도 비트가 같다**"
  (이 프로젝트의 flex 포인트). 벤치 FREE 도 이 이력을 돌리고, 그 상수를
  `#canonical-own-v1` 로 SHA256SUMS 에 고정해 매 벤치 런이 패리티를
  판정한다 (ADR-008 Pitfall 2 준수; 자체 이력은 (manifest, seed) 의 순수
  함수라 결정론).

주: ADR-008 anti-goal 대로 FREE 산출물의 스테이지 덤프 비교는 하지
않는다 — 판정면은 canonical 최종 상태뿐.

### 1.4 자체 이력 (σ\*, Λ\*, π\*) — 바닐라 규칙 준수 근거

- **바닐라는 차원 내 병렬이 구조적으로 불가** (A5 §5.3/§8 — 전 FEATURES
  바디가 단일 ConsecutiveExecutor): 비결정은 **순서뿐**. 따라서 임의의
  총순서는 바닐라가 만들 수 있었던 이력과 같은 계급이고, 충돌쌍을
  직렬화하는 우리 FREE 는 바닐라 보증보다 강하다.
- σ\* = 5×5 잔차 클래스 버킷 순회 (버킷 내 manifest 순). 세그먼트 내
  같은 버킷 원소는 거리 ≥5 > 충돌반경 4 → 세그먼트 내 충돌 0.
- Λ\* = 데코 직후 L08 (vanilla: initialize_light 는 features 직후) +
  버킷 경계마다 배치 (eligibility: 자신+±1 전부 08 — **09 피라미드
  radius-1 규칙 그대로**, gen_light_stages.c:26-40). 배치 25회 (골든
  Λ_g 109회; 바닐라 배치는 스케줄러 wake 산물이라 개수는 규칙이 아님).
- π\* = 골든 pp 집합(1165 청크 — FULL 승격 집합은 세션 기록)의 행우선.

## 2. 구현 (커밋 단위)

1. **1eb3694 스레드-안전 기반작업** — 전부 값-불변:
   - surface 샘플러 memo 를 공유 hc_ssampler_t → 청크별 ctx (바닐라
     Context 소속과 동형). **기존 체인 20스레드의 실제 레이스**였다
     (스탬프가 per-ctx 카운터라 스레드 간 상시 충돌 — hc_surface.h 가
     스스로 "Phase 2 스레딩 시 이동" 을 예고했던 그 항목).
   - per-call static 스크래치 12건 → `_Thread_local` (tree/lush/geode/
     template/o2omap slot_of), 지연-초기화 전역 소진 API
     (hc_features_prewarm, structures_init 의 template/mineshaft eager,
     ensure_palette 스핀 가드), 레코더 C11 스핀락 (hc_sync.h),
     hc_structures_references 의 `(void)scratch` 버그 수정 (전역 arena
     범프 → 호출자 scratch — 병렬 직렬화 선행 조건).
2. **a28a14b light ctx 분리** — BFS 큐/펜딩/dirty 를 hc_light_ctx_t 로;
   기존 API 는 내장 ctx0 래퍼 (ADR-008 D3: 스테이지 코드 단일).
   check_contiguous 를 재유도 영향 반경(±1)으로 국소화 (검출력 동일).
3. **4698ffe hc_sched** — core/include/hc_sched.h + sched.c (위 §1.1)
   + 속성 게이트 test_sched (ctest #32).
4. **3cdf690 test_free_region** — 드라이버 + 게이트 (A)(B) (#33/#34)
   + features free_read_guard (±2 읽기 반경 debug assert — 미래
   데이터팩의 원거리 읽기는 무음 오염이 아니라 즉사).
5. **bc49a20 TSan 게이트** — HC_TSAN 옵션 + tsan 프리셋 +
   scripts/check_tsan.sh (전체 스위트 직렬; 커널 6.x ASLR
   mmap_rnd_bits=32 와 TSan 새도우 충돌은 `setarch -R` 로 우회).
6. **bcd6f46 bench --policy free** — 자체 이력 경로 + dag_wall/dag_cpu
   계측 + 병렬 직렬화 + `#canonical-own-v1` 판정 + run_bench.sh `-m`.

## 3. 전/후 실측 (-O3, 20스레드, 3런 중앙값, seed 1234567890 r.0.0)

전 = d17dfc7 워크트리 빌드, 후 = 본 태스크 HEAD — 동일 정렬 정책
(-falign-functions=64, P2-2 §5 룰렛 교훈 준수). 매 런 canonical PASS.

| 지표 | 전 (REPLAY) | 후 (REPLAY) | 후 (FREE) |
|---|---|---|---|
| **gen wall** | **8203.2 ms** (편차 0.9%) | 8223.5 ms (편차 0.7%) | **3455.7 ms** (편차 0.9%) |
| features (직렬) | 3306.4 ms | 3294.2 ms | → DAG 로 통합 |
| light09+08+flush (직렬) | 1890.4 ms | ≈동등 | → DAG 로 통합 |
| **데코+라이트 DAG wall** | (직렬 합 5196.8) | — | **597.0 ms (8.7x)** |
| chain(04+07+08) | 2531.4 ms | ≈2545 | 2579.0 ms (무손대) |
| serialize | 289.4 ms | 287.5 ms | **90.8 ms (3.2x)** |
| sha256 | 127.3 ms | ≈ | 127.4 ms |

- REPLAY 회귀 없음: +0.25% < 런 편차. FREE 배율 **2.37x** (전 대비) /
  2.38x (후-REPLAY 대비).
- DAG 내부: cpu-sum 6920.8 ms ÷ wall 597.0 = **병렬도 11.6x** (20스레드;
  배리어 25회 + 세그먼트 경계 워밍업이 잔여 갭). 귀속: deco 5074.4 /
  l09 1542.3 / prepare 173.1 / l08 104.4 / flush 26.6 ms.
- prepare (전-청크 지오메트리 재유도): 109회(Λ_g) → 25회(Λ\*) 로 배치
  통합 자체가 라이트 비용을 줄였다 (golden light09 1795.6 ms 의 상당분이
  prepare 반복이었음 — P2-0 §6.2 진단과 정합).
- deco cpu-sum 5074 vs REPLAY features 3294 ms: 자체 이력의 작업량 차이
  (라이브 라이트 쓰기 84,282 vs 25,212 — 08 이 데코 직후라 창이 넓다)
  + 20스레드 메모리 대역 공유. 판정 지표는 wall.
- -O2 FREE: 3545.0 ms (편차 0.4%) — O3 와 동급 (잔여가 체인 인터프리터
  지배라 P2-0 §3.3 결론 유지).
- 참고 (debug -O2, asserts+guard on): own-이력 DAG 4.249 s → 0.694 s.

## 4. 게이트

| 게이트 | 결과 |
|---|---|
| ctest (build, -O2 debug) | **34/34** (신규: #32 sched, #33 free_region_golden, #34 free_region_own) |
| parity_gate.sh (build / bench-o2 / bench-o3) | 3/3 PASS, canonical a5963205…3c24 (불변) |
| check_no_fma.sh (3빌드) | PASS (70,950 / 70,892 / 87,004 insn) |
| check_sanitizers.sh (ASan+UBSan, 34종) | PASS |
| **check_tsan.sh (신설, TSan 34종 직렬)** | **PASS 클린 (729 s)** — 레이스 0 |
| 벤치 런 자체 해시 판정 (전 3 + 후 replay 3 + free o3 3 + free o2 3) | 전부 PASS |

TSan 클린은 §2-1 기반작업의 완결성 검증이기도 하다: 사전 식별한 공유
상태(공유 surf memo, static 스크래치, 레코더, 지연 초기화, 전역 arena)
외에 잔여 레이스가 없었다.

## 5. "안 함" 판정 (실측/유도 근거)

1. **골든-순서 FREE 를 벤치 수치로** — 안 함. 골든 manifest 는 공간
   크롤(연속 엔트리 846/1460 = 58%가 체비셰프 1)이라 충돌반경 4 에서
   임계경로가 곧 전체 — 사전 시뮬 폭 1.5~1.9x, 실측 debug 에서 ~1.0x.
   게이트 (A) 전용으로 유지. 성능 주장은 전부 자체-이력 (B) 로 표기
   (ADR-008 Pitfall 2 의 구분 표기).
2. **prepare 국소화/증분화 (배치를 전역 배리어에서 해방)** — 안 함.
   derive_geometry 는 단조 래치 (registered/top 은 자라기만) + pre_top
   스냅샷 조건 15-fill (light_engine.c:433-505) 이라 "언제 파생했는가"
   가 마스크·라이트에 영구 관측된다 — 동결-창(08 제출~완료 사이) 쓰기의
   지오메트리 가시화 시점이 전역 prepare 타이밍에 걸려 있어, 국소화는
   값-등가 증명이 없다. Λ\* 에서 prepare 는 25회 × ~7 ms = dag cpu 의
   2.5% — 이득도 없다. 증분 마스크 유지(P2-0 §6.2)는 값-불변 재설계와
   함께 P2-5 항목으로 유지.
3. **prepare 내부 병렬화** — 안 함. 루프가 이웃 슬롯 마스크를 쓰면서
   뒤 청크의 pre_top 스냅샷이 앞 청크의 등록을 관측한다 — 루프 순서가
   시맨틱. 비중 2.5%.
4. **체인의 DAG 융합** (04..06 을 이벤트로 넣어 데코와 파이프라인) —
   이번 범위 밖 (P2-0 §8 행에 없음). 상한 견적: gen wall ≈ chain-bound
   2.85 s (1.21x 추가). P2-4 가 체인 CPU 를 줄인 뒤 재평가가 순서.
5. **pp/sha 병렬화** — pp 34 ms, sha 127 ms. sha 는 SHA-NI (P2-5) 가
   정답이지 스레딩이 아니다.

## 6. 갱신된 비중 + 다음 순서 판단

FREE gen wall 3455.7 ms 기준:

| 페이즈 | wall | 비중 |
|---|---|---|
| 체인(04+07+08, 20스레드) | 2579 ms | **74.6%** |
| 데코+라이트 DAG | 597 ms | 17.3% |
| sha256 | 127 ms | 3.7% |
| serialize | 91 ms | 2.6% |
| pp + light_final + setup | 60 ms | 1.7% |

1. **P2-4 (AVX2 SoA 노이즈 배치) 최우선 확정** — 체인이 74.6%, 그중
   noise CPU 79%. P2-2 §7 이 제안한 lazy 브랜치 평가(interval_select/
   range_choice) 선행 검토 포함.
2. P2-5 의 light09 항목(slot/마스크)은 우선순위 하락 — l09 는 DAG 에서
   코어에 풀렸고 (cpu 1542 ms → wall 기여 ~130 ms), prepare 는 25회로
   줄었다. P2-5 잔여 중 실효는 SHA-NI (127 ms) 와 hm 프라임 융합/문자열
   테이블화 (deco cpu 5074 ms 의 내부 — DAG 폭이 아니라 단가를 줄인다).
3. 체인-DAG 융합은 P2-4 후 체인 wall 재측정과 함께 재평가 (§5-4).

## 7. 함정 기록 (다음 태스크용)

- **TSan + 커널 6.8**: mmap_rnd_bits=32 ASLR 이 TSan 새도우와 충돌
  ("FATAL: unexpected memory mapping") — check_tsan.sh 가 `setarch -R`
  로 프로세스 한정 우회. sysctl 건드리지 말 것.
- **블록 주석 안의 σ\*/Λ\***: `*/` 시퀀스가 주석을 조기 종료시킨다 —
  구분자는 `·` 를 쓴다 (bench 패치에서 실제 발화).
- `#canonical-own-v1` 은 파생 상수 — features 시맨틱이 바뀌는 태스크는
  골든 상수와 함께 재고정해야 한다 (free_region_own 게이트가 페어
  일치로 잡아준다).
- 워크트리 벤치 시 gitignore 로컬 골든 (region-ref/-margin, structures,
  band) 심링크 필요.

## 8. 커밋

- 1eb3694 perf(threads): 스테이지 코드 스레드-안전 기반작업
- a28a14b refactor(light): 이벤트-로컬 hc_light_ctx_t 분리
- 4698ffe feat(sched): hc_schedule_policy 주입점 + 이중-정책 스케줄러
- 3cdf690 test(free): FREE 드라이버 + FREE-vs-REPLAY 게이트 2종
- bc49a20 test(tsan): TSan 게이트 신설
- bcd6f46 feat(bench): --policy free 벤치 경로 + canonical-own-v1
- (본 노트 커밋)

원자료 (로컬-only, gitignore): `bench/results/20260806T173947Z-…-p23-before.jsonl`
(전, 워크트리에서 복사), `…T174033Z-…-replay-p23-after.jsonl` (후 REPLAY),
`…T174112Z-…-free-p23-free.jsonl` (후 FREE o3), `…-bench-o2-…-free-p23-free.jsonl` (o2).
