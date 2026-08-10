# B-2: FREE 워터폴/프로파일 심층 분석 — 남은 최적화 헤드룸 확정

2026-08-10, 이 VM (claw). 분석 기준 HEAD 2a15ee6 (+계측 커밋 6e89d16 —
bench/ 한정, 기본 off). **분석 전용 — 코어 무손대.** 절대치는 VM 참고치,
유효한 것은 비중·배율 (P2-0 §0). 원자료: `bench/results/20260810T*-b2-*`
(baseline jsonl + waterfall 덤프 3런, 로컬-only), perf 데이터는 /tmp/b2
(janitor 대상, 요지 본 노트 흡수).

## TL;DR

- **베이스라인 정정**: B-1 의 "FREE 2.74s" 는 **bench-o2** 수치였다
  (run_bench.sh 기본 프리셋; o2 3런 2576/2742/2933). 릴리즈 대표
  **bench-o3 는 2228.3 ms** (2162/2228/2345, 3런 전부 own-v1 PASS,
  청크당 2.18 ms). 이 노트의 % 는 o3 2228 기준.
- **워터폴: 페이즈는 완전 순차** (chain → DAG → pp → light_final →
  serialize → sha, 겹침 0). 크리티컬 패스 판정: **chain 은 워크-바운드**
  (가동률 96.5% — 버블 없음, CPU 총량/20 이 바닥), **DAG 는 배리어-바운드**
  (가동률 60.5%, 유휴의 66%가 prepare 배리어), **꼬리는 오버헤드-바운드**
  (serialize 95.5 ms 중 실작업 43 ms, 나머지 = 스폰 지연 + 47MB concat).
- **surface 내부 분해 (신규 발견)**: surface CPU 8.6 s 의 **62.8%가
  바이옴 줌 스택** (hc_biome_zoom + fiddle/fiddled_distance/lcg_next +
  view_get) — 룰 트리(cond_test/rule_apply)는 24.1%뿐. gen-창 perf 에서도
  hc_biome_zoom(+view_get) 17.1%로 **단일 1위** (x4_run 9.7%보다 크다).
  콘너별 fiddle 오프셋은 (seed, 콘너 정수좌표)만의 순수 함수라 캐시 가능
  — 최대 후보, 예상 gen wall **−7~10%**.
- **체인-DAG 융합 재판정 (P2-3 §5-4)**: 실측-dur 재스케줄 시뮬로
  2002 → 1879 ms = **1.065x (gen wall −5.5%)**. P2-3 의 상한 ~1.2x 는
  σ\*/Λ\* 공간-국소 재설계까지 가야 나오는 워크바운드 플로어(1686 ms)다.
  현행 이력 융합은 **기각**.
- **noise 스칼라 폴백은 소멸 확인** (noise CPU 의 2.3%), **nc_init 은
  콘 산출이 아니라 flat 테이블 평가가 90.4%** — "리전당 1회 호이스트"
  가설 기각 (콘 산출은 4.3% ≈ 65 CPU-ms ≈ wall 3 ms).
- GO 판정 3건 합산 시 이 VM 추정 gen wall 2228 → **~1.8 s (−19%)**.
  그 너머는 P2-6 (베어메탈/AVX-512) 축.

## 0. 측정 조건

- bench-o3 (-O3 -g, falign-64), 20스레드, seed 1234567890 r.0.0, 매 런
  canonical 판정 (FREE=own-v1, REPLAY=골든). load 1분 0.8~3 (런별 병기
  생략 — 편차로 흡수). perf 는 `sudo sysctl kernel.perf_event_paranoid=1`
  후 **원복 완료** (P2-5 §5 함정 그대로).
- 워터폴 계측 (커밋 6e89d16): `HC_BENCH_TIMELINE=<path>` 로만 활성.
  체인은 기존에 읽던 타임스탬프의 저장뿐, 추가 클록은 DAG 이벤트당 2회 +
  직렬화 청크당 2회. **오버헤드 실측: on 3런 중앙값 2221 vs off 2228 /
  2597 ms — 런 편차(±10%) 안에 완전히 묻힌다** (계측 on 런도 전부 PASS).
- 분석기 bench/waterfall.py: 점유 타임라인, 유휴 분해, 셀-FIFO 의존
  재구성 크리티컬 패스, 융합 리스트-스케줄 시뮬.

## 1. 워터폴 — 타임라인과 크리티컬 패스

계측 run1 (gen 2182.3 ms) 기준, proc0 상대 ms:

```
setup 936.7 (생성비용 아님) → [chain 1348.7] → [DAG 653.7] →
(pp-parse 32.3, 하네스) → [pp 58.7] → [lfinal 26.1] → [ser 95.5] → [sha 21.9]
```

겹침 가능 구간은 구조상 chain↔DAG 뿐 (§3-1 융합 시뮬) — serialize 는
최종 라이트 동결 상태를 읽으므로 DAG/pp 와 겹칠 수 없다 (값-등가 증명
없이는).

### 1.1 chain — 워크-바운드, 버블 없음

- 가동률 **96.5%** (busy 25.81 CPU-s / 예산 26.76). tail idle 0.6%,
  램프 skew 77 ms (pthread 스폰 ~3.9 ms/스레드 — VM 특성, §5).
- 청크 지속시간 p50 15.5 / p90 18.4 / p99 35.7 / max 64.4 ms — 롱테일이
  없어서 아토믹-카운터 스틸링으로 충분히 평탄.
- 결론: **chain wall 은 CPU 총량 나누기 20 이 바닥** — 여기서 더 얻는
  길은 CPU 자체 감소 (§2 surface/noise) 뿐, 스케줄 개선 여지 0.

### 1.2 DAG — 배리어-바운드 (워터폴이 드러낸 것)

ASCII 점유 (run1, 100컬럼×6.54 ms — 세로 흰 골 = prepare 배리어):

```
w00 |+#########################+.  +###   +############+   .##   .##   .#######+    +##+  ...
w12 |     .############################   .##.  .######+   .########.  .#############...
w19 |           ################.  +###.  +##   +######.   .##.  .#+   .########.   +####...
```

- 가동률 **60.5%** (busy 7.91 / 예산 13.07 CPU-s). **유휴 5.17 CPU-s 의
  66% = 배리어** (prepare 9회 × 13.3→26.4 ms 단독 실행 = wall-sum 179 ms
  가 매번 20-폭 정지), 잔여 34% = 드레인 꼬리 + 스폰 램프 (~70 ms).
- 이벤트 wall-sum: deco 5592 (n=1461) / l09 2016 (n=1309) / l08 121
  (n=1461) / prepare 179 (n=9).
- **구조적 크리티컬 패스 442.8 ms = wall 의 68%** (셀-FIFO 의존 재구성,
  워커 무한 가정): 세그먼트별 최장 체인 합 263.8 + prepare 179. 즉 워커를
  더 부어도 443 ms 가 바닥 — DAG 의 병목은 폭이 아니라 **배리어 사슬**.
- 세그먼트 구조: s0 이 185.8 ms/1442이벤트 (첫 09 적격이 버킷 ~12에야
  발생 — 잔차 체스판이 공간 전체에 퍼져 3×3 완주가 늦다), 이후 s1~s8 은
  span 18~61 ms 의 짧은 세그먼트가 배리어로 끊긴다. **prepare 지속시간이
  13→26 ms 로 단조 증가** — in_r 전 청크(최대 1461) 재유도 스캔이라
  누적 비례 (light_engine.c hc_light_accum_prepare).
- P2-3 §5 는 prepare 를 "dag cpu 의 2.5%"로 소읽음했지만, **배리어로서의
  wall 비용은 DAG wall 의 27% + 그에 걸린 유휴 3.4 CPU-s** — 워터폴
  없이는 안 보이던 것.

### 1.3 꼬리 — serialize 의 정체 (P2-5 §4-3 "오버헤드 플로어" 해부)

```
w00 |.############################################.        ← 43ms 에 일감 소진
w13 |                                            ..        ← 스폰이 늦어 도착 즉시 종료
w19 |                                                      ← 아예 무작업
```

- serialize 95.5 ms = 선두 0.5 + **워커 구간 43.1 + 꼬리(join+47MB
  concat) 51.8**. busy 360 CPU-ms → 유효 폭 ~8.4/20. 스폰 지연
  ~3.3 ms/스레드라 w13 이후는 일감 소진 뒤 도착.
- P2-5 가 "~75 ms 플로어"로 뭉뚱그린 것의 실체: **절반은 concat memcpy,
  절반은 스폰 계단** — 스레드 생성 자체가 아니라 생성의 직렬 지연.
- sha 21.9 ms 는 SHA-NI 로 이미 바닥 (47MB ≈ 2.1 GB/s).

### 1.4 전체 예산

gen-창 (setup_end→sha1) 2244 ms × 20 = 44.9 CPU-s 예산, 계측 busy 34.2
CPU-s → **전체 가동률 76.2%**. 유휴 ~10.7 CPU-s 귀속: DAG 5.2 + 단일
스레드 구간(pp/lfinal/sha 107 ms × 19) 2.0 + serialize 1.55 + chain 0.9
+ 램프/갭 잔여.

## 2. 체인 내부 프로파일

gen-창 perf (cycles, F999, `-D 1000` 으로 setup 스킵) 상위 30:

| % | 함수 | 귀속 | | % | 함수 | 귀속 |
|---|---|---|---|---|---|---|
| 15.75 | hc_biome_zoom | 07 (+09 소량) | | 1.41 | hc_gen_surface_stage | 07 본체 |
| 9.71 | x4_run | 04 AVX2 | | 1.35 | hc_biome_view_get | 07 |
| 7.49 | hc_structures_step | 09 deco | | 1.14 | hc_ore_vein_block | 04 |
| 5.58 | hc_aquifer_substance | 04(+08) | | 0.99 | hc_nc_update_for_x | 04 interp |
| 5.41 | perlin_x4 | 04 AVX2 | | 0.87 | derive_geometry_chunk | light(08/prep) |
| 5.04 | hc_perlin_sample_scaled | 04 스칼라(aquifer/vein/flat)+08 | | 0.79 | hc_df_eval_cone | 04 스칼라 |
| 4.88 | hc_gen_noise_stage | 04 본체 | | 0.74 | octaves_x4 | 04 AVX2 |
| 3.43 | cond_test | 07 룰 | | 0.59 | hm_update | 09 |
| 2.98 | hc_nc_update_for_z | 04 interp | | 0.47 | hc_feat_schedule_tick | 09 |
| 2.91 | rule_apply | 07 룰 | | 0.42 | hc_chunk_to_nbt | ser |
| 2.69 | hm_prime_one | 09 deco | | 0.40 | hc_df_eval_stream_x4_avx2 | 04 AVX2 |
| 2.15 | hc_nc_select_cell_yz | 04 interp | | 0.38 | run_mods | 07 룰 |
| 2.12 | [kernel] | — | | 0.38 | seed_block_chunk | l09 |
| 2.09 | ore_do_place | 09 deco | | 0.36 | hc_nc_update_for_y | 04 interp |
| 2.06 | hc_octaves_value | 04 스칼라 | | 0.36 | packed_container | ser |
| 1.82 | bfs_run | l09 | | 0.36 | hc_beard_compute | 04 |
| 1.77 | hc_beard_compute | 04 | | (1.55 eval_node — 04 스칼라) | | |

(플랫 -O3 에선 fiddle/lcg_next 가 hc_biome_zoom 에 인라인. dwarf 콜그래프
런으로 분리하면 아래와 같다. dwarf 런은 언와인딩 오버헤드가 메모리-바운드
스테이지(l09)를 부풀리므로 **귀속 비율 전용**, 절대치는 bench JSON.)

### 2.1 스테이지-앵커 컨테인먼트 분해 (dwarf cg, 스테이지 CPU = bench 실측)

**surface (8.6 CPU-s = 체인 CPU 34.8%):**

| 슬라이스 | 비중 | 내용 |
|---|---|---|
| **바이옴 줌 스택** | **62.8%** | hc_biome_zoom 19.6 + fiddle 22.2 + fiddled_distance 6.8 + lcg_next 8.3 + view_get 3.6 (+기타) |
| 룰 트리 | 24.1% | cond_test 12.5 + rule_apply 8.9 + run_mods 등 |
| 본체 (col_get/상태기계) | 9.9% | |
| surface_depth/secondary | 2.7% | |
| psl/min_surface | 0.4% | |

줌 호출원은 컬럼당 biome_col(x, h1, z) + 룰의 ctx_biome (y 마다 메모,
surface.c:198-204). 핵심 관찰: **fiddled_distance 의 콘너별 오프셋
(fx,fy,fz)는 (zoom_seed, 콘너 정수좌표)만의 함수** (biome_zoom.c:21-37)
— 쿼리 블록은 (dx,dy,dz) 분수만 기여한다. 즉 쿼트-격자 콘너 캐시로
쿼리당 lcg ~72회(8콘너×9) → 콘너당 3-double 조회 + 8×(3add+3mul+strict
비교, 순서 보존)로 붕괴 가능. **값-불변**: 같은 double, 같은 비교 순서.
교차 검증: 줌 슬라이스 5.4 CPU-s ≈ gen-창 CPU 16% ≈ perf 17.1% 일치.

**noise (13.2 CPU-s = 체인 CPU 55.4%):**

| 슬라이스 | 비중 | 판정 근거 |
|---|---|---|
| x4 AVX2 커널 | 41.8% | 추가 인하는 P2-6 축 (gradient 전치/스필 — P2-4 §6-4) |
| aquifer (스칼라 BLOCK 콘 포함) | 16.3% | 단일점 온디맨드 — 배치 축 없음 (P2-4 §4-2 재확인) |
| interp/셀 기계 (update/select/lerp) | 14.2% | 값 그 자체 (보간 시맨틱) |
| stage 본체/블록쓰기 | 13.8% | |
| ore_vein (스칼라 포함) | 6.4% | 동상 |
| beard | 5.1% | |
| **스칼라 DF 폴백 잔여** | **2.3%** | fill_slice 꼬리(49점 중 1)/SP 콘 — **소멸 확인** |

**nc_init (1.5 CPU-s = 체인 CPU 6.3%):** flat 테이블 **평가** 90.4%
(펄린/eval_node — 청크 종속 값), 콘 산출(mark/collect/program/x4_ok)
**4.3% ≈ 65 CPU-ms ≈ wall 3 ms**, 할당/제로화 1.2%. 태스크 지문의
"리전당 1회 비용" 가설은 **콘 산출에만 성립하고 그 몫이 없다.**

**deco (5.6 CPU-s, DAG 내부):** structures_step 47.3% / hm_prime_one
18.3% (P2-5 §4-1 의 3.47%-of-total 과 정합) / ore 12.8% / tree·lush 7.1%.

### 2.2 l09 CPU 요동 (관찰 기록)

l09 cpu-sum 이 런마다 1775~4906 ms (2.8x). 같은 런에서 **deco p50 은
3242→3235 us 로 불변인데 l09 만 p50 1018→2258 us** — 전역 감속(주파수/
스틸)이 아니라 **메모리-레이턴시 바운드 스테이지(BFS flood)만 때리는
공유-VM 간섭**. DAG wall 654→823 ms 로 전달. noise/surface/carvers 도
불변. → 최적화 후보 아님, **베어메탈 재실측 항목** (P2-6 에서 DAG wall
편차가 크게 줄 것으로 예상).

## 3. 후보 판정표

기준: bench-o3 FREE gen wall 2228 ms. "이 VM에서 X ms = Y%" 표기.

| # | 후보 | 예상 이득 (gen wall) | 난이도 | 패리티 리스크 | 판정 |
|---|---|---|---|---|---|
| 1 | **surface 바이옴 줌 콘너-fiddle 캐시** | 줌 5.4 CPU-s (surface 62.8%, 체인 CPU ~22%); 2~5x 축소 시 chain wall −150~220 ms = **−7~10%** | 중 | 하 (순수 함수 캐시 — double 값·strict 비교 순서 보존; region/own 게이트 즉시 판정) | **GO — 차기 1순위** |
| 2 | **prepare dirty-skip 증분화** | 배리어 wall-sum 179 ms→~수 ms 시 DAG wall −~150 ms = **−5~7%** | 중상 | 중 (무변경 청크 재유도 = 멱등 논증 필요 — 래치 입력 불변 시 동일 출력 + dirty 전파 ±1. P2-3 §5-2 기각은 "시점 이동"이지 "무변경 스킵"이 아님) | **GO — 증명 선행 조건부** |
| 3 | **serialize 파이프 정리** (풀 재사용/조기 스폰 + concat 제거 + sha 스트리밍) | ser+sha 117 ms → 이론 플로어 ~35 ms (워커 busy 360/20=18 + 스트림 sha 꼬리) = **−75~85 ms = −3.4~3.8%** | 하~중 (sha update API 는 core 소형 추가; 다이제스트 동일 — 같은 바이트 스트림) | 무 | **GO — 소형 번들** |
| 4 | **체인-DAG 융합 (현행 σ\*)** | 실측-dur 시뮬: chain+dag 2002→1879 ms (naive 1895/demand 1879) = **−5.5%** | 상 (스케줄러에 체인 이벤트 통합 + 배리어 시맨틱 분리) | 중 | **기각** — P2-3 상한 ~1.2x 의 실측 재판정 = 1.065x. 원인: 잔차 체스판 버킷이 공간 전체에 퍼져 s0 적격이 체인 완주에 결박 |
| 5 | **σ\*/Λ\* 공간-국소 재설계 + 융합** | 워크바운드 플로어 (25.8+7.9)/20 = 1686 ms → 상한 **−14%** (+배리어 횟수 절감 여지) | 상 (이력 재설계 + own-v1 재고정 + 게이트 재검증 — P2-3 §7) | 중상 | **베어메탈 후** — P2-6 에서 스폰/메모리 특성 확정 후 #1·#2 반영 재견적 |
| 6 | noise 스칼라 폴백 축소 | 잔여 2.3% of noise CPU ≈ 0.3 CPU-s = **−15 ms = −0.7%** | — | — | **기각 (소멸)** |
| 7 | nc_init 콘 산출 리전-호이스트 | 4.3% × 1.5 CPU-s ≈ 65 CPU-ms = **wall −3 ms = −0.1%** | — | — | **기각** |
| 8 | nc_init flat 테이블 x4 화 | 90.4% × 1.5 = 1.35 CPU-s; 2.4x 가정 −0.8 CPU-s = **−40 ms = −1.8%** | 중 | 하 | **기각** (<3%; 25점 배치 축은 있으나 flat 콘 x4 적격 미확인 — #1 태스크에서 무료로 재확인 가능) |
| 9 | deco 단가 (structures_step 47%/hm_prime 18%) | DAG 가 배리어-바운드라 deco CPU −20% 해도 세그먼트 span 경유 **−~60 ms = −2.7%** (REPLAY 는 features 직렬이라 ~6% 상당) | 중 | 중 | **이관 유지** — P2-5 §4-1 의 "미래 features 태스크" 권고 그대로 (REPLAY 개선 가치가 더 큼) |
| 10 | pp 병렬화 | 34.9 ms = **1.6%** | 중 | 중 | **기각** (<3%) |
| 11 | DAG/ser 스레드 풀 재사용 (스폰 램프 제거) | DAG 램프 ~70 ms + ser 계단 ~40 ms 중 회수분 = **−1.5~3%** | 중 (core sched 에 풀 주입) | 하 | **베어메탈 후** — 스폰 3~4 ms/스레드는 VM 특성, 베어메탈에선 <0.1 ms 로 자체 소멸 가능성 |

**종합**: GO 3건 (#1+#2+#3) 적중 시 이 VM 추정 2228 → **~1.80 s
(−19%, 청크당 ~1.76 ms)**. 이후 대형 축은 P2-6 (베어메탈 절대치 +
AVX-512 gradient + #5 재견적).

## 4. 게이트/검증

| 항목 | 결과 |
|---|---|
| 계측 on 벤치 (o3 FREE ×3 + perf 하 ×3) | 전 런 own-v1 canonical **PASS** |
| 계측 off 벤치 (동일 바이너리 ×3) | PASS — off 경로 오염 없음 |
| REPLAY 스팟 (o3 ×1) | PASS (골든 상수) |
| parity_gate.sh (build) | PASS (a5963205…3c24 불변) |
| 코어 소스 | **무손대** (git diff 는 bench/ 2파일 뿐) |

## 5. 함정 기록 (다음 태스크용)

- **run_bench.sh 기본 프리셋은 bench-o2** — B-1 처럼 절대치를 인용할 땐
  프리셋 병기 필수. o2/o3 격차 ~1.15x (2742 vs 2388 동시점 비교 아님,
  같은 날 o3 2228).
- **summarize.py 가 free 모드 breakdown 에 dag_wall 를 빼고 출력**한다
  (합계가 gen wall 과 불일치) — 후속 하이진 1줄 감. 이번엔 JSONL 직접
  파싱으로 우회.
- **dwarf 콜그래프 perf 는 l09 를 부풀린다** (언와인딩이 메모리-바운드
  구간에 집중) — cg 런은 귀속 비율 전용, 절대치는 flat 런/bench JSON 으로.
- perf_event_paranoid=4 재확인 (P2-5 §5) — sysctl 1 로 낮췄다가 **4 로
  원복 완료**.
- 워터폴 덤프의 DAG 셀 리스트 (D 라인) 는 의존 재구성용 — waterfall.py
  의 배리어 모델 (n_cells==0 = 전역) 과 함께 써야 CP 가 맞는다.
- l09 cpu-sum 은 이 VM 에서 신뢰 구간이 넓다 (1.8~4.9 s) — DAG wall
  회귀 판정에 l09 절대치를 쓰지 말 것.

## 6. 커밋

- 6e89d16 bench(waterfall): B-2 워터폴 계측 (HC_BENCH_TIMELINE, 기본 off) + 분석기
- (본 노트 커밋)

원자료 (로컬-only, gitignore): `bench/results/20260810T045446Z-…-b2-baseline.jsonl`,
`…T0504*/T0505*-b2-waterfall-run{1,2,3}.txt`. perf: `/tmp/b2/perf_free20{,_cg,_gen}.data`
+ folded.txt (janitor 대상, 요지 §2 흡수).
