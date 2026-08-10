# P2-9 — B-3 갭 기반 GO 3건: structures 스캔 인덱스 + surface 룰 트리 선형화 + features TLS 다이어트 (2026-08-10)

B-3 (§7 GO 재정렬) 상위 3건의 이행. **절대치는 이 VM(claw) 참고치,
유효한 것은 비중·배율·연산 카운트** (P2-0 §0). 전/후는 전부 **같은 날
같은 트리 쌍** — bench-o3 FREE 20스레드 3런 중앙값 + REPLAY 스팟,
매 런 own-v1 canonical 판정 PASS. 연산 카운트는 HC_BENCH_COUNTERS
(HOT 사이트는 /tmp 별도 `-DHC_CTR_HOT=ON` 빌드 — 커밋 빌드 무오염,
B-3 §9 함정 준수). l09 절대치는 판정에 쓰지 않는다 (B-2 §5).

## TL;DR

- **FREE gen wall 1894.5 → 1658.3 ms (-12.5%)**, **REPLAY 6158.6 →
  4271.4 ms (-30.6%)** (같은 날 쌍; pre 3런 1866/1894/2095, 최종 HEAD
  after 1589/1658/1709). after 표본 2회째 (TTAS 수정 전 fbcac6a 시점)
  는 1586/1604/1749 중앙값 1604.1·REPLAY 4091.1 — after 자체가 VM
  대역 ±3% 를 갖는다 (둘 다 유효 표본, 보수 쪽을 헤드라인으로).
  P2-0 (198.0s) 대비 누적 **FREE ~119-123x**.
- GO-1 structures 스캔 인덱스 (커밋 2b6c0d1): **스캔 스텝
  2,553,398,232 → 398,013 /런 (-99.98%)**, deco CPU-sum **5430.9 →
  3373.6 ms (-37.9%)**, 단독 wall -12.8%·REPLAY -31.4%. B-3 의 "바닐라
  강제가 아닌 구현 스캔" 판정 그대로 적중 — 이번 라운드 지배 항목.
- GO-2 surface 룰 트리 선형화 (커밋 0a25ee5): 방문 **625.2M → op
  315.2M (-49.6%)**, surface CPU-sum **5247.1 → 4716.9 ms (-10.1%)**.
  **surf_cond 265,091,736 전/후 비트 동일** — 평가 순서 보존의 실측
  증거 (아래 §2 논거).
- GO-3 features TLS 다이어트 (커밋 fbcac6a): **PT_TLS 8,174,752 →
  981,864 B (-88%)**, 체인 스폰 램프 skew **86 → 12.0 ms** (워터폴),
  체인 가동률 95.8 → **99.0%**, maxrss 805.8 → **661.1 MB**, 스레드
  스택 가용 마진 ~0.38 → ~7 MB. 헬퍼-스포너는 **유지** (§3 판단).
- 갭 테이블 갱신 (§5): **K_struct (자기-일관, 이 VM, steal 포함) =
  1.10-1.16 대역** (after 표본 2회 — l09 가 분모를 흔든다), steal
  (~8%, 베어메탈 소멸 항) 제외 시 **~1.01-1.07** — B-3 기준선 1.223
  에서 명확히 하강, 기준 1.10 충족 판정은 P2-6 베어메탈에서 확정
  (B-3 §6.2 판정 무대 조항). DAG CP 313.7 → **262.4 ms** (GO-1 의
  s0 체인 수혜 109 → 90 ms).
- **부수 수리 (커밋 9a696ab)**: GO-1 이 deco 를 조밀화하자 TSan
  free_region_own 게이트가 틱-레코더 스핀락 컨보이로 2분 → 75분+
  폭주 (gdb 실진단) — 스핀락을 TTAS 로 전환해 해소 (§4.1). 값 경로
  무접촉 (HB 구조 동일).
- 게이트 전부 green (최종 HEAD 재실행): ctest 36/36 (GO-2 등가 assert
  활성), parity_gate (a5963205…3c24 불변), own-v1 (2eb7485b…9b84d6)
  매 런, no_fma / isa_equiv (scalar+avx2) / sha_equiv (sw+ni) / TSan
  풀 스위트 + TSan 벤치 직접 런 / ASan+UBSan.

## 1. GO-1 — structures references/beard/step 스캔 인덱스화 (커밋 2b6c0d1)

### 설계

종전: `hc_structures_references`/`_beard`/`_step` 이 (cx,cz) 마다
17×17 창 셀 × n_starts(=21) 선형 스캔 — step 은 idx 0..25 별로
재스캔이라 호출당 26×289×21 = 157,794 트립, 16,071 호출 (1461 시드
× 11 스텝) = **2,535,907,374 트립/런** (카운터 실측, 산술과 정확 일치).

대체: init 말미에 **(scx asc, scz asc, i asc) 사전식 안정 정렬 순열
`ord[]`** 를 1회 구축 (삽입 정렬, n≤64). 각 사이트는 ord 순회 + 창
`[cx±8]×[cz±8]` 필터로 스캔을 대체 (references/beard). step 은 **단일
패스 + step_index 버킷**: 창·step·bb 필터 통과분을 버킷에 append 후
idx 0..25 순 처리 — 호출당 트립이 26×289×21 → **21** 로.

### 패리티 논증 (순서 의존 — 태스크 지문의 요구 사항)

순서가 값을 운반하는 경로는 두 개다: (a) `hc_longset_to_array`
(LongOpenHashSet 에뮬) 는 **삽입 순서**가 충돌쌍의 슬롯 배치를 통해
방출 순서에 영향, (b) step 처리 루프의 스타트 순서가 **피스 배치 RNG
스트림 소비 순서**를 결정. 따라서 수집 시퀀스 자체를 보존해야 한다:

- **보조정리 (순회 순서 동일)**: 창 안의 두 스타트 a, b 에 대해 종전
  삼중 스캔은 셀을 (sx asc, sz asc) 로, 셀 안에서 i asc 로 방문하므로
  a 가 b 보다 먼저 방문 ⇔ (scx,scz,i)_a <lex (scx,scz,i)_b. ord 는
  정확히 이 사전식 순서이고 창 필터는 부분열 추출이므로 **모든 (cx,cz)
  에서 방문 시퀀스가 비트 동일**. (안정 정렬이라 동일 (scx,scz) 셀
  내 i 순서 보존 — mineshaft 다중 스타트 케이스.)
- **step 버킷**: 고정 (step, idx) 에 대해 버킷 append 순서 = 위
  시퀀스에서 (step,idx) 매치만 남긴 부분열 = 종전 그 idx 스캔의 수집
  순서. 빈 버킷은 종전 `n_hits==0 continue` 와 동일하게 **RNG 미시드
  스킵** (set_feature_seed 는 (deco_seed, idx, step) 완전 재시드라
  캐리오버 없음). `step_index > 25` 는 종전 루프 범위 밖 (매치 불가)
  그대로 스킵. bb/step 필터는 부수효과 없는 순수 술어 — 평가 위치
  이동은 집합에 무영향.
- **불변량 성립 조건**: starts[] 는 init (단일 스레드, 워커 스폰 전)
  이후 불변 (n_starts/scx/scz/step/step_index/bb 재기록 없음 — 전수
  grep; 피스 런타임 필드 spider_placed 등은 스캔이 안 읽음). ord 는
  init 말미 구축 후 읽기 전용.
- **게이트-실검증**: ord 구축을 init 의 references 골든 교차검증
  (references.txt 전수, 순서까지 대조, fail-loud) **앞**에 넣었다 —
  새 순회의 산출 순서가 init 마다 골든과 전수 대조된다.

### 실측

| 지표 | 전 (p29-pre) | 후 (p29-go1) | 델타 |
|---|---|---|---|
| struct_scan (스캔 스텝) | **2,553,398,232** (step 2,535,907,374 = 157,794×16,071 정확 + beard/init 17.5M) | **398,013** (step 337,491 = 21×16,071 정확) | **-99.98%** |
| deco CPU-sum | 5430.9 ms | 3373.6 ms | **-37.9%** (B-3 견적 -30~45% 적중) |
| beard CPU-sum | 31.3 ms | 15.4 ms | -51% |
| FREE gen wall (3런 중앙값) | 1894.5 ms | 1651.9 ms | -12.8% |
| REPLAY (스팟) | 6158.6 ms | 4222.1 ms | **-31.4%** (features 직렬 — B-3 "REPLAY 대형" 그대로) |

기각/불채택: 셀→스타트 밀집 그리드 인덱스 (289 셀 프로브/호출) —
n_starts=21 이라 정렬 순열 순회 (21 트립) 가 더 작고 순서 논증이
자명해 그리드는 불필요.

## 2. GO-2 — surface 룰 트리 선형화 (커밋 0a25ee5)

### 설계

트리 (468 노드: cond 149·block 111·seq 53) 는 컴파일 후 불변인데
실행은 룰 노드 재귀 (`rule_apply` 360M 진입/런 — 호출 프레임 + seq
루프 북키핑 + children[] 간접) 였다. **컴파일 시 fail-연속 인코딩으로
선형화**: `hc_sop_t {kind, cond, fail, block}` (8B) 배열을 방출 —

- SEQUENCE → 자식 구간 연접 (자식 i 의 fail = 자식 i+1 의 물리 시작,
  마지막 자식 fail = 부모 fail). sequence 노드 자체는 **op 0개**.
- CONDITION → `[TEST cond fail]` ++ then 구간 (then 의 fail = 부모 fail).
- BLOCK/BANDLANDS → 반환 op 1개. 최상위 fail = 말단 NULL op (-1).

크기 선계산으로 시작 주소를 하향식 확정 (백패치 불요). 실행은 평탄
루프 `prog_run`: TEST 통과 → pc+1, 실패 → pc=fail.

### 패리티 논증 (평가 순서·단락 의미 보존 — 태스크 지문의 요구 사항)

**값 경로 무접촉**: cond_test/hc_surface_band/memo 기계는 한 줄도 안
바뀌었다. 변환은 순수 제어-흐름. 구조 귀납 (prog_run 주석 원문):
R(r, fail) 진입 = rule_apply(r) 가 non-null 이면 그 값 반환, null 이면
pc=fail 이탈, **cond_test 호출 시퀀스 동일** — BLOCK/BAND 자명,
CONDITION 은 TEST 1회 = 같은 cond 1회 평가 후 then/fail 분기가 재귀
정의와 1:1, SEQUENCE 는 "첫 non-null 승리" 단락이 fail-체인과 1:1
(빈 sequence = 0 op = 상시 null). 따라서 조건 평가의 순서·횟수·단락이
보존되고 → memo 래치 순서 (유일 순서-민감 memo 인 steep 포함), vgrad
positional RNG 소비, bandlands 도달 조건이 전부 보존된다.

**실측 증거 + 상시 게이트 2중**:
- `surf_cond` **265,091,736 전/후 비트 동일** (카운터 결정론 하에서
  "같은 조건을 같은 횟수 평가" 의 직접 증거). samp/sec/msl/steep
  미스 카운터도 전부 동일 — memo 동작 불변.
- assert 빌드 (`ctest`/ASan/TSan 트리 전부) 는 **매 루트 적용마다**
  선형화 결과 == 재귀 참조 구현 (`PROG_RUN_CHECKED`) 을 실검증 —
  36/36 + full/free region 골든 이력 전체가 이 assert 아래서 통과.
  두 번째 순회는 첫 순회가 래치한 memo 를 읽지만 memo 값은 위치
  상태의 순수 함수라 관측 등가 (steep 은 루트 적용 안에서 하이트맵
  불변이라 래치 시점 무관). release (NDEBUG) 비용 0.

### 실측

| 지표 | 전 (p29-go1) | 후 (p29-go2) | 델타 |
|---|---|---|---|
| 방문 (cond+apply) | 265.1M + 360.1M = **625.2M** | op 디스패치 **315.2M** (= cond 265,091,736 + root 50,069,420 + topmat 7,547 **정확**) | **-49.6%** |
| surf_cond | 265,091,736 | **265,091,736 (비트 동일)** | 0 |
| surface CPU-sum | 5247.1 ms | 4716.9 ms | **-10.1%** |
| FREE gen wall | 1651.9 ms | 1604.7 ms | -2.9% (B-3 견적 -2~3% 적중) |

op 합산이 정확히 닫히는 부수 실증: **NOT-내부 재귀 도달 0** (cond
진입수 = TEST op 수) — 26.2 오버월드 트리에서 NOT 래퍼는 실행 경로에
없다.

검토 후 보류 (다음 라운드 후보): cond kind 를 op 에 스플랫해 cond_test
switch 를 prog_run switch 로 융합 (디스패치 1단 제거, ~265M×수 사이클).
cond 본문 이동/중복 대비 이득 불확실 — 선형화 단독 -10% 를 취하고
중단. **방문 수 자체 축소는 시도 금지 유지** (B-3 §7-2: memo 는 이미
바닐라-최적, 방문 수는 데이터-강제 플로어).

## 3. GO-3 — features_tree/lush TLS 다이어트 (커밋 fbcac6a)

### 설계

P2-8 실사: PT_TLS 8,174,752B 중 features_tree.c 6,373,672 (levels[7]
jset 2.87M + tree_ctx×2 3.28M + log/leaf_arr 196K + box 33K) +
features_lush.c 819,232 (ground/water jset). `pthread_create` 가 부모
측에서 정적 TLS 블록을 제로화하므로 스폰이 ~2.5-3ms/스레드 — 3웨이브
61 생성/런의 "스폰 계단" + 스택 가용 마진 ~380KB 의 원인.

biome_zoom.c (P2-8 GO-1) 패턴 그대로 **bss 풀 + 스레드당 1회 relaxed
원자 핸드아웃**: TU 별 스크래치 구조체 (`tree_scratch_t` 6.37MB ×80,
`lush_scratch_t` 0.82MB ×80), TLS 는 포인터만. 슬롯은 소유 스레드
전용 (TSan 관측면 = 핸드아웃 카운터뿐), 스레드당 단일 슬롯이라 재사용
/중첩-재진입 특성이 종전 TLS 와 동형.

- **값-불변 논거**: 전 필드가 사용 전 전체 재초기화된다 (jset_init 은
  헤더+bucket[0..15] 를 쓰고 성장분은 resize 가 초기화; box 는 memset;
  log/leaf_arr 는 ctx_list 전량 재기록) — **제로-초기화 의존 없음**
  (P2-9 전수 실사; biome_zoom 과 달리 valid-태그류 없음). 풀 이관은
  값 경로 무접촉.
- **풀 상한 80 + 소진 die**: 스크래치를 쓰는 스레드는 배치 실행자뿐
  — DAG 워커 (스케줄러 캡 MAX_W 64) + 메인 (REPLAY/verify/pp). 소진은
  compute-폴백이 불가능한 자원이라 (zoom 캐시와 다름) fail-loud die —
  캡 die 하우스 스타일.
- .bss 683.7MB (가상) — 미사용 슬롯은 제로 페이지, **물리는 오히려
  감소** (maxrss 805.8 → 661.1MB: 스폰마다 8.2MB TLS 제로화가 사라진
  효과).

### 실측

| 지표 | 전 | 후 |
|---|---|---|
| PT_TLS p_memsz | 8,174,752 B | **981,864 B (-88.0%)** |
| 체인 스폰 램프 (워터폴 skew) | 86 ms (B-3 §3) | **12.03 ms** |
| 체인 가동률 | 95.8% | **99.0%** (tail idle 0.5%) |
| maxrss | 805.8 MB | **661.1 MB** |
| 스레드 스택 가용 마진 | ~380 KB | **~7 MB** (안정성 항목) |
| FREE gen wall (go2→go3 쌍) | 1604.7 | 1684.8 (중앙값; 런 1651/1685/1838) |

**정직 캐벳**: go3 쌍의 wall 중앙값은 VM 변동 대역 (당일 런 전폭
1586~2095) 안이라 wall 단독으로는 판정 불가 — GO-3 의 판정 지표는
**워터폴 스폰 skew (86→12ms) 와 PT_TLS/가동률/maxrss** 다. after
3런 (1586/1604/1749, 전 게이트 후) 은 go2 대비 동등-이상.

**헬퍼-스포너 판단 (태스크 요구)**: per-create 잔존 ~0.6ms (12.03/19
— 잔여 TLS 0.98MB + 커널 비용). 직접 메인 20-스폰으로 되돌리면 ser
창 (33.6ms) 에 ~12ms 재유입 → **헬퍼-스포너 유지가 여전히 유효**.
제거는 채택하지 않음. 잔여 PT_TLS 의 95% 는 structures_template.c
930,304B (processed/fillv/locked 등) — 다음 다이어트 후보로 기록만
(P2-9 범위 밖).

## 4. 게이트

### 4.1 부수 수리 — TSan 게이트 틱-레코더 스핀락 컨보이 (커밋 9a696ab)

GO-3 까지의 HEAD 에서 check_tsan 의 **free_region_own 이 2분 → 75분+
로 폭주**했다 (같은 스위트의 free_region_golden 은 2분 정상 통과).
gdb 실진단 (본체는 ptrace_scope=1 이라 자식 재현 런에 attach): **20
워커 중 17 이 `hc_feat_schedule_tick` 의 `hc_spin_lock(&g_recorder.mu)`
— 전부 `__tsan` AtomicRMW 경로** (SlotLock/MetaMap/VectorClock). 종전
스핀락은 경합 시 atomic_exchange 를 반복하는데, TSan 은 RMW 마다 전역
런타임 락을 잡으므로 17-스레드 RMW 폭풍이 컨보이가 된다.

발화 조건 분해: (a) GO-1 이 deco 이벤트에서 스캔 (이벤트 시간의
지배항이던 사적 작업) 을 제거 → 틱-스케줄 호출 도착률이 조밀해짐,
(b) own-σ* 는 산개 순서라 (골든 크롤과 달리) 20 deco 가 무충돌로 상시
동시 실행 — 전역 락 도착률 최대화. **네이티브는 무영향** (일반 ctest
29s, 벤치 full-speed — 경합 창이 ns 대역).

수리: `hc_sync.h` 스핀락을 TTAS 로 — 획득 실패 시 relaxed 로드로만
관망, 풀림 관측 후에만 acquire 교환 재시도. HB 는 여전히 acquire
교환만이 부여 (릴랙스드 로드는 HB 를 만들지 않음 — 시맨틱·값 경로
불변; atomic_flag → atomic_uchar 는 관망 로드 제공용, TAS 와 동일
기계어). 수리 후 free_region_own 정상 대역 복귀 (§4 표), 전 게이트
재실행 green. **게이트 약화 없음 — 코드를 고쳤다** (가드레일 조항
그대로).

### 4.2 결과 (전부 P2-9 최종 HEAD 재실행; GO-1/2 커밋 시점에도 각각 통과)

| 게이트 | 결과 |
|---|---|
| ctest (build, -O2 debug) | **36/36** — GO-2 등가 assert (매 루트 적용 flat==재귀) + GO-1 init references 교차검증 활성 |
| parity_gate.sh | PASS — REPLAY canonical **a5963205…3c24 불변** |
| FREE own-v1 (2eb7485b…9b84d6) | 매 런 내장 판정 **불변** (pre/go1/go2/go3/after 3런씩 + 스팟 전부 PASS) |
| check_no_fma.sh | PASS (75,730 insn, 최종 HEAD) |
| check_isa_equiv.sh | PASS (scalar/avx2 둘 다 골든 canonical) |
| check_sha_equiv.sh | PASS (sw/ni) |
| check_tsan.sh (풀 스위트) | PASS 클린 (438.6s — TTAS 수정 후 정상 대역 복귀, §4.1) — 신규 관측면: g_ts_next/g_ls_next relaxed 핸드아웃 + TTAS 관망 로드 |
| TSan 벤치 FREE 직접 실행 | PASS 클린, own-v1 PASS (풀 핸드아웃·TTAS 를 벤치 동시성 그대로 판정) |
| check_sanitizers.sh (ASan+UBSan) | PASS |

## 5. 전/후 종합 + B-3 갭 테이블 갱신

### 5.1 전/후 (같은 날 쌍, bench-o3 FREE 20T 3런 중앙값; 후 = 최종 HEAD 9a696ab)

| 지표 | 전 (8b10ac3) | 후 (P2-9 HEAD) | 델타 |
|---|---|---|---|
| **gen wall** | **1894.5 ms** | **1658.3 ms** | **-12.5%** |
| 청크당 | 1.850 ms | 1.619 ms | |
| chain wall | 1267.1 ms | 1179.2 ms | -6.9% |
| chain CPU-sum | 21.68 CPU-s | 21.30 CPU-s | -1.8% (GO-2 몫) |
| surface CPU-sum | 5248.8 ms | 5109.3 ms | 쌍별 판정은 §2 (-10.1%) — 런간 대역 |
| deco CPU-sum | 5430.9 ms | 4022.0 ms | **-25.9%** (쌍별 -37.9%) |
| DAG wall | 537.9 ms | 407.5 ms | 참고치 (l09 요동) |
| ser/pp/lfinal | 47.9/36.6/3.9 ms | 31.9/34.8/3.8 ms | |
| maxrss | 805.8 MB | 661.6 MB | -17.9% |
| **REPLAY (스팟)** | **6158.6 ms** | **4271.4 ms** | **-30.6%** |

after 표본 2 (fbcac6a, TTAS 전): gen 1604.1·chain 1102.9·surface
4912.3·deco 3890.7·REPLAY 4091.1 — after 자체의 VM 대역 (±3%). GO
별 판정은 각 절의 **인접 쌍** (pre↔go1↔go2↔go3) 이 SoT.

카운터 (결정론, HOT 수집): struct_scan 2,553,398,232 → 398,013;
룰 방문 625,210,969 → 315,168,703; surf_cond/root/topmat·zoom_q·
x4·aquifer·light 계수 전부 불변 (값 경로 무접촉의 카운트 면).

### 5.2 갭 테이블 갱신 (B-3 §6.1 대비 변동 행만)

| 구간 | B-3 (전) | P2-9 (후) | 판정 |
|---|---|---|---|
| 09 structures_step | ~2.8 CPU-s, 스캔 2.54G = 구현 오버헤드 — GO 1위 | 스캔 398K (값-운반 잔존: 창 필터 21트립 + 매칭·배치) | **갭 소멸 — 하한도달** (잔여 deco 는 setblk/hm_prime 항으로 흡수) |
| 07 룰 트리 | ~2.1 CPU-s, per-visit 갭 — GO 2위 | 방문 -49.6%, surface CPU -10.1%; 잔여 = cond_test 디스패치+본문 (데이터-강제) | **per-visit 갭 절반 회수**; 잔여 후보 = kind-스플랫 융합 (§2 보류) |
| wall 스폰/램프/드레인 ~162 ms | PT_TLS ≥110 회수 대상 | 체인 skew 86→12, ser 스폰 0.2, DAG 램프 축소 — 램프 성분 ~25ms 잔존 | **PT_TLS 항 회수 완료** (잔존은 드레인/웜업 + 잔여 TLS 0.98MB) |
| DAG 구조 CP | 313.7 ms | **262.4 ms** (s0 109→90 — GO-1 수혜; 클린 런) | 여전히 워크-바운드 (CPU/20 342.7 > CP) |
| 합성 워크 플로어 | 1486.0 ms | **~1426 ms** = chain 1064.9 + DAG max(296.4, 262.4) + pp 34.8 + lfinal 3.8 + ser busy/20 25.6 (최종 HEAD 쌍, l09 클린 런) | after 표본 2 (l09 간섭) 로는 1464 |
| **K_struct** | 1.223 (steal 포함) / 1.124 (제외) | **자기-일관 1.10-1.16 대역** (표본 2회: 1604.1/1464 = 1.095, 1658.3/1426 = 1.163 — l09 가 분모·분자를 함께 흔든다); B-3 실측 steal 비중 ~8% (베어메탈 소멸 항) 제외 시 **~1.01-1.07** | 1.223 에서 명확히 하강; **기준 1.10 충족 판정은 P2-6 베어메탈에서 확정** (B-3 §6.2 판정 무대 조항) |

남은 GO 지형 (B-3 §7 갱신): #1/#2/#3 완료 → 남는 상위는 **#7 AVX-512
커널 (베어메탈, +12% Zen4 추정)** 과 #4 σ*/Λ* 재견적 (베어메탈 후).
로컬 잔여 후보는 GO-2 kind-스플랫 융합 (¶§2) 과 structures_template
TLS 0.93MB (¶§3) — 둘 다 소형.

## 6. 시도했으나 기각/보류한 것

1. **GO-1 밀집 그리드 인덱스** (셀→스타트, 289 프로브/호출) —
   n_starts=21 에서 정렬 순열 순회가 더 작고 순서 논증이 자명. 기각.
2. **GO-2 cond kind 스플랫** (op 에 cond 종별 내장, cond_test switch
   융합) — 코드 중복 대비 이득 불확실, 선형화 단독으로 견적 달성.
   보류 (후보로만 기록).
3. **GO-2 방문 수 축소** (트리 구조 캐시/조건 호이스팅) — B-3 §7-2
   금지 유지 (memo 가 이미 바닐라-최적; 방문은 데이터-강제).
4. **GO-3 malloc 지연 할당** (스레드당 1회 malloc) — bss 풀 대비 장점
   없고 (물리 동급, 해제 시점 문제) 태스크가 P2-8 GO-1 패턴을 지정.
   기각.
5. **GO-3 헬퍼-스포너 제거** — per-create 0.6ms 잔존이라 직접 스폰은
   ser 창에 ~12ms 재유입. 유지 (§3).

## 7. 함정 기록 (다음 태스크용)

- **전/후 카운터 비교는 "전" 형상에 카운터를 심은 일회성 /tmp HOT
  빌드로** — 카운터 사이트가 최적화로 사라지는 경우 (이번 스캔 루프)
  전 수치를 먼저 실측해 두지 않으면 재구성해야 한다 (산술 유도와
  교차확인 가능했지만 실측이 SoT).
- 선형화류 변환의 등가 assert (flat vs 재귀) 는 **flat 먼저 실행**
  후 재귀 검증이 안전 — 첫 순회가 memo 를 래치하는 쪽이 제품 경로여야
  래치 순서가 제품과 동일하다 (이번 트리는 래치 시점 무관이지만
  일반 규칙으로).
- .bss 대형 풀 (683MB 가상) 은 size/readelf 를 놀라게 하지만 미사용
  슬롯은 제로 페이지 — 물리 판정은 maxrss 로 (이번엔 오히려 -145MB).
- GO-3 류 (램프/유휴 회수) 는 **wall 중앙값으로 판정 불가** (VM 변동
  대역 안) — 워터폴 skew·가동률·PT_TLS 같은 구조 지표로 판정할 것.
- **연산량형 최적화가 동기화 지점을 노출시킨다**: GO-1 이 deco 의
  사적 작업 (스캔) 을 제거하자 전역 틱-레코더 락 도착률이 조밀해져
  TSan 게이트가 컨보이로 폭주했다 (§4.1). 핫 이벤트 경로의 전역 락은
  다음 최적화 라운드의 잠재 병목/게이트 리스크 — 스핀락은 TTAS 가
  기본값이어야 하고, 근본 해소가 필요해지면 틱-레코더의 청크별 샤딩
  (같은 청크 append 는 셀-FIFO 가 이미 직렬화 — 락 자체가 불필요해짐)
  이 후보다.
- **TSan 게이트 폭주의 진단 경로**: ptrace_scope=1 에선 본체 attach
  불가 — 같은 커맨드를 gdb 자식으로 재현해 `timeout -s INT` 후
  `thread apply all bt`. 워커 전원이 100% CPU 인데 진행이 없으면
  스핀 컨보이부터 의심.

## 8. 커밋

- 2b6c0d1 perf(structures): 스캔 순열 인덱스 — references/beard/step
  삼중 스캔 제거 (P2-9 GO-1)
- 0a25ee5 perf(surface): 룰 트리 선형화 — 재귀 tryApply 를 fail-연속
  평탄 프로그램으로 (P2-9 GO-2)
- fbcac6a perf(features): 대형 스크래치 TLS 다이어트 — bss 풀 +
  스레드당 핸드아웃 (P2-9 GO-3)
- 9a696ab fix(sync): 스핀락 TTAS 전환 — TSan 게이트 틱-레코더 컨보이
  해소 (P2-9)
- (본 노트 커밋)

원자료 (로컬-only, gitignore/janitor 대상): `bench/results/20260810T*-p29-*.jsonl`
(pre/go1/go2/go3/after/after2), 카운터 `/tmp/p29_ctr_{pre,go1,go2,after}.txt`,
워터폴 `/tmp/p29_tl_go3.txt`, TSan 컨보이 백트레이스 `/tmp/p29_gdb_ownfree.txt`
(요지 본 노트 흡수).
