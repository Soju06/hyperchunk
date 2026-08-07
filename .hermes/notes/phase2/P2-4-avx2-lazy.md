# P2-4 — lazy 브랜치 평가 + AVX2 SoA 노이즈 배치 커널 완료 노트 (2026-08-07)

P2-0 §8 P2-4 행 (ADR-004 D1): AVX2 SoA 배치 커널 + 런타임 디스패치 +
백엔드 바이트 동일 게이트. P2-2 §7 / P2-3 §6 권고대로 **lazy 브랜치
평가를 선행**하고 두 기여를 분리 실측했다. **절대치는 이 VM(claw)
참고치, 유효한 것은 비중·배율** (P2-0 §0 동일 주의).

## TL;DR

- **noise CPU-sum 39.72s → 13.38s (2.97x)** = lazy 1.216x × AVX2 2.442x.
  **FREE gen wall 3731.1 → 2200.7 ms (1.696x, 청크당 2.15 ms)**, REPLAY
  8822.8 → 7033.1 ms (1.254x). 체인 wall 2851.8 → 1276.0 ms (2.24x).
  P2-0 베이스라인(198.0s) 대비 누적: REPLAY 28.2x / FREE **90.0x**.
- **lazy (커밋 3d18e34)**: 콘 선형 워크가 range_choice/interval_select 의
  전 브랜치를 eager 평가하고 있었다 — 실측으로 슬라이스 콘 펄린 124
  샘플/점 중 **~50 (40%) 이 죽은-브랜치 값**. 콘 프로그램 (브랜치-배타
  세그먼트) 이 바닐라의 선택-브랜치-만 시맨틱을 재현한다. 값-불변.
- **AVX2 (커밋 192d1d1)**: fill_slice (y 4점) / select_cell_yz (iz 4점)
  를 4-레인 SoA 스트림 평가기로. 레인 = 독립 평가점이라 수평 리덕션이
  없고, 레인별 연산 시퀀스가 스칼라와 동일 — **비트 동일이 설계로
  성립**하고 게이트가 증명한다. FMA 인트린식 0, 나눗셈은 vdivpd 만,
  ±0/NaN 은 cmp+blend 로 Java 시맨틱 재현. cpuid 디스패치 + 스칼라
  폴백 (ADR-004 D2), AVX-512 는 enum 슬롯만 (P2-6).
- 신규 게이트 2종: **df_x4** (ctest #20 — 두 백엔드 이중 실행 9,376
  비트 대조, AVX2 부재 호스트 SKIP 77), **check_isa_equiv.sh** (풀 리전
  --isa scalar/avx2 각각 골든 canonical 판정). 기존 게이트 전부 green.

## 1. 설계

### 1.1 lazy 브랜치 평가 — 콘 프로그램 (df_eval.c, hc_df.h 가 SoT)

사전 정량화 (스크래치 도구, 26.2 오버월드 n=262 그래프 실좌표 탤리):

| 콘 | choice | 발견 |
|---|---|---|
| slice_var (142노드, 펄린 124/점) | RC 9 + IS 3 | n172 out-브랜치 **배타 56노드/38펄린** — 69% 평가에서 사장. IS 3개 (spaghetti 계열) 는 비선택 함수 노이즈 ~20샘플/점 상시 사장. 합산 스킵 가능 ~50펄린/점 (40%) |
| cell (14노드, 펄린 0) | RC 1 | n208 브랜치 배타 8/14노드 (INTERP lerp3 포함), 선택 빈도 52:48 — 평균 33% 노드 스킵 |
| SP/BLOCK/FTS 콘 | 0 | choice 없음 — lazy 해당 없음 (FTS 사다리의 노이즈는 전부 y-불변부라 P2-2 가 이미 컬럼당 1회로 승격) |

메커니즘: `hc_df_cone_program` 이 콘 산출 시 1회, choice→branch 엣지
하나를 지운 도달성(위상 역스윕)으로 **브랜치-배타 집합**을 계산하고,
중첩 (배타 집합 안의 choice) 은 집합 포함 관계이므로 각 노드를 가장
작은 배타 집합(innermost)에 귀속시켜 세그먼트 트리를 만든다. 스트림
인코딩: `[-1, ch, wt, we][then][else]` (RC) / `[-2, ch, nf, w0..][seg0..]`
(IS) — 플레인 노드 인덱스 리스트의 상위집합이라 같은 워커가 소비한다.
`hc_df_eval_prog` 는 선택 세그먼트만 워크 후 ch 를 평가한다 (선택식은
eval_node 의 해당 케이스와 동일 식 — 같은 d, 같은 경계, 같은 선택).

값-불변 근거: eval_node 순수 (평가 경로 RNG 0회) + 스킵 노드는 도달성
정의상 이 점에서 아무도 읽지 않는다 (읽는 노드가 있으면 배타가 아니라
평가된다). 도달성 엣지는 eval 이 읽는 엣지의 상위집합 (마커 pass-through
포함) — 과대 연결은 배타를 줄일 뿐 (보수 방향). 엣지-삭제 모델이 성립
하지 않는 케이스는 eager 유지: b==c range_choice, 중복 함수표 IS.

적용: cone_build_ex 전 콘 + slice_var 분할부. 26.2 에서 실제 프로그램이
붙는 콘은 slice_var/cell 둘 (나머지는 choice 없음 → NULL). y-분할과의
합성 근거: y-분산 전파가 단조라 var→var 읽기 경로가 inv 를 경유하지
않는다 — var-내부 도달성이 var 안에 닫힌다.

### 1.2 AVX2 4-레인 SoA 백엔드 (df_simd_avx2.c, hc_df_simd.h 가 SoT)

**배치 축 = 평가점** (옥타브나 노드가 아니라): fill_slice 는 컬럼당 49개
y-점 (12×4 + 스칼라 꼬리 1), select_cell_yz 는 최내측 iz 루프가 정확히
4 (cell_width==4 가드). 레인이 서로 독립인 점이므로 리덕션이 존재하지
않고, 벡터 연산은 전부 레인별 — **스칼라와 같은 피연산자에 같은 순서로
같은 IEEE 연산**이라 비트 동일이 설계로 성립한다.

- 커널: perlin_x4 (해시 체인은 레인별 스칼라 정수, gradient 는
  GRAD4[16][4] 로드+4×4 전치, smoothstep/lerp 트리 벡터), octaves_x4
  (누산 `d += (amp*g)*f` 좌결합 유지 — 레인별 순차), normal_x4,
  blended_x4 (over/under 생략 누산은 마스크로 +0.0 강제 — 스칼라의
  lo=0.0 잔류와 동형; 전-레인 over/under 시 그 측 통째 스킵), wrap_x4.
- Java 시맨틱 프리미티브: v_jmin/v_jmax (NaN-a 우선, ±0 규칙 — blendv
  가 마스크 부호비트만 보는 성질로 `and(both0, b)` 한 방), v_clamp,
  v_clamped_lerp (NaN delta → lerp 경로), MUL 단락 (±0 → +0.0).
- 포화 가드: Mth.floor/lfloor 는 포화+NaN→0 — 벡터 fast path 는
  |좌표|<2^30 (wrap 은 2^62) 를 movemask 로 검사하고 위반 레인이 있으면
  **그 노드 전 레인 스칼라 폴백** (26.2 도달 불가, 데이터팩 방어).
- lazy 와 합성: 컨트롤 세그먼트는 "필요한 레인이 하나라도 있으면" 4레인
  실행 — 죽은 레인 값은 순수 계산이고 blend/회수가 선택하지 않는다.
  전 레인이 같은 브랜치를 고르면 (지형상 대부분) lazy 이득이 그대로
  유지된다.
- 디스패치 (df_isa.c): `__builtin_cpu_supports("avx2")` 1회 + `_Atomic`
  캐시 (TSan 클린), `hc_isa_force` 오버라이드 (벤치 `--isa`, 게이트).
  nc_init 이 청크당 1회 `nc->x4` 로 캐시. op 화이트리스트
  (`hc_df_stream_x4_ok`) 밖 콘은 x4 부적격 → 스칼라 (SHIFTED_NOISE/
  SHIFT_A/B/SPLINE/FTS — 26.2 핫 콘에 없음). 비-x86 은 항상 스칼라
  (avx2 TU 는 #if 가드 + abort 스텁).
- -mavx2 는 df_simd_avx2.c 한 TU 만 (CMake per-source) — 나머지 TU 는
  기본 ISA 라 AVX2 부재 호스트에서 바이너리가 그대로 돈다. -mfma 는
  켜지 않는다.

## 2. 전/후 실측 (-O3, 20스레드, 3런 중앙값, seed 1234567890 r.0.0)

전 = ffd6751 (P2-3 HEAD), 중간 = 3d18e34 (lazy 만), 후 = 192d1d1
(lazy+AVX2). 동일 정렬 정책 (-falign-functions=64 전 빌드 고정, P2-2
§5). **매 런 canonical PASS** (REPLAY = 골든 상수, FREE = own-v1 상수).

| 지표 | 전 | lazy | lazy+AVX2 | 총배율 |
|---|---|---|---|---|
| noise CPU-sum | 39719.3 ms | 32667.8 ms (1.216x) | **13376.8 ms** (2.442x) | **2.97x** |
| 체인 wall (REPLAY) | 2851.8 ms | 2429.4 ms | 1276.0 ms | 2.24x |
| **REPLAY gen wall** | 8822.8 ms (편차 2.0%) | 8275.6 ms (1.8%) | **7033.1 ms** (0.7%) | **1.254x** |
| **FREE gen wall** | 3731.1 ms (편차 4.1%) | 3343.7 ms (2.0%) | **2200.7 ms** (2.6%) | **1.696x** |
| FREE 청크당 | 3.64 ms | 3.27 ms | **2.15 ms** | |

- surface/carvers/nc_init CPU 는 전 구간 동등 (surface 8.3~8.4s,
  carvers 0.79s — 무손대 방증). nc_init 1459→1515 ms (+3.8%) 는
  프로그램 산출 + x4 적격 검사 비용 — 체인 CPU 의 0.2%p.
- FREE DAG wall 664.0 → 606.1 ms — 데코 자체는 무손대지만 체인 완료가
  빨라져 웨이브 공급이 고르게 된 효과 (deco cpu-sum 5120→5159 동등).
- -O2 스팟 (후): REPLAY gen wall 7275.7 ms (편차 1.2%) / FREE 2409.5 ms
  (편차 5.6%), noise CPU 14.75s — **O3 가 노이즈 CPU 에서 1.10x 우위**.
  종전 "O2/O3 동급" (P2-0 §3.3) 에서 소폭 벌어졌다 — SIMD 커널/전치
  코드젠이 O3 수혜 구간. 릴리즈 -O3 유지 근거 강화.
- P2-0 베이스라인 gen wall 198.0s 대비 누적: REPLAY 28.2x, FREE 90.0x.

단일-스레드 perf 귀속 (총 cycles, 전→후 — 후는 avx2 백엔드):

| 함수 | 전 (P2-4 직전) | 후 |
|---|---|---|
| eval_node (스칼라 인터프리터) | 22.2% | 1.1% |
| hc_perlin_sample_scaled (스칼라 잔여: SP 콘·flat 구축·y-꼬리·07) | 22.1% | 5.9% |
| x4 커널 (x4_run + perlin_x4) | — | 16.5% |
| hc_octaves_value | 6.4% | 1.9% |
| hc_df_eval_cone | 4.4% | <0.3% |
| hc_biome_zoom (07/09 측 — 무손대, 상대 부상) | 8.0% | **14.3%** |
| hc_aquifer_substance | 3.3% | 5.9% |

DF 평가 스택 합계 ~57% → **~26%** (그중 x4 커널 16.5%p). 차상위는
전부 07/09/light 측 (biome_zoom, derive_geometry 6.2%, structures_step
5.5%, cond_test/rule_apply 5.1%) — §6 판단의 근거.

## 3. 게이트

| 게이트 | 결과 |
|---|---|
| ctest (build, -O2 debug, auto=avx2 로 전 스위트 실행) | **35/35** (신규 #20 df_x4 — 백엔드 이중 실행 9,376 비트 대조) |
| **check_isa_equiv.sh (신설)** | PASS — 풀 리전 --isa scalar / --isa avx2 둘 다 골든 canonical (전이적 바이트 동일, ADR-004 D4) |
| parity_gate.sh (build / bench-o2 / bench-o3) | 3/3 PASS, canonical a5963205…3c24 (불변) |
| check_no_fma.sh (3빌드) | PASS (74,380 / 74,161 / 91,493 insn — AVX2 TU 포함 vfmadd 0) |
| check_sanitizers.sh (ASan+UBSan, 35종) | PASS (101.6 s) |
| check_tsan.sh (TSan 35종 직렬) | PASS 클린 (412.4 s) — 레이스 0 |
| 벤치 런 자체 해시 판정 (전 3+3 / lazy 3+3 / avx2 3+3 + o2 3+3) | 전부 PASS |

- df_cones (기존) 는 fill_slice 경유로 **x4 경로 3,920 값을 스칼라
  프리픽스와 상시 대조**하게 됐고, cell 배터리에 프로그램 경로 검사
  (WANT_CELL_PROG 30) 를 추가했다 — 검출력 강화 방향만.
- `#canonical-own-v1` 재고정 불필요: P2-4 는 노이즈 값-불변이라
  features 시맨틱 무손대 — free 벤치가 기존 상수로 PASS 한 것이 증명
  (P2-3 §7 함정 항목의 반례 아님).

## 4. "안 함" 판정 (근거)

1. **AVX-512 (vpermt2pd gradient, zmm 32)** — 안 함, P2-6 그대로.
   로컬(5900X=Zen3) 실측 불가 (ADR-004 Pitfall 4). enum 슬롯과 디스패치
   구조만 마련. AVX2 의 gradient 로드+전치 (코너당 4로드+6셔플) 가
   정확히 vpermt2pd 가 갚아줄 비용이다 — P2-6 의 이득 지점 재확인.
2. **SP/BLOCK 콘의 x4 화** (psl/erosion/depth/aquifer/vein) — 안 함.
   전부 단일점 온디맨드 평가라 배치 축이 없다 (aquifer 그리드는 지연
   계산, vein 은 y-가드 뒤 산재 블록, psl 은 메모 뒤 희귀). 콘 자체는
   x4_ok 로 판정해 두어 미래 배치 지점이 생기면 그대로 붙는다.
3. **옥타브-수직 SIMD** (한 점의 옥타브 4개 레인화) — 안 함. 누산
   `d += amp*g*f` 순서가 값이라 수평 리덕션이 필요해지고, 순서-보존
   추출-합산으로는 이득이 상쇄된다. 점-배치가 자연스러운 곳에서는
   불필요한 우회.
4. **vgatherqpd gradient** — 안 함. Zen3 마이크로코드 ~36cyc (ADR-004
   가 명시한 최악 경로) — 로드+전치가 우월.
5. **fill_slice inv 콘 / flat 테이블 구축의 x4 화** — 안 함. 컬럼당
   1회 / 청크당 25점 — nc_init 2.9% 안의 소수부, 측정 한계 이하.
6. **BLENDED 의 mixed-lane over/under 세분화** (레인별 스킵) — 안 함.
   26.2 지형에서 q∈(0,1) 이 지배적이라 양측 모두 계산하는 경우가
   대부분이고, 전-레인 일치 스킵은 이미 있다.

## 5. 함정 기록 (다음 태스크용)

- **parity_gate.sh 스테일-바이너리 false-PASS (실발화)**: 종전 스크립트
  는 verify 바이너리가 '존재하면' 빌드를 건너뛰었다 — bench-o2/o3 의
  verify 가 P2-3 산출물인 채 PASS 를 찍었다 (P2-4 코드를 전혀 판정하지
  않음). 항상-재빌드로 수정 (커밋 cd2238a). 교훈: run_bench.sh
  는 타깃을 매번 빌드하지만 parity_gate 는 아니었다 — **게이트 스크립트
  의 빌드 정책을 게이트 추가 때마다 확인할 것**.
- **x4 콜사이트 좌표는 정수값 계약**: BLENDED 의 (int32_t) 캐스트 동치가
  이 계약에 걸려 있다 (debug assert 가 지킨다). 새 x4 콜사이트를 추가할
  때 비정수 좌표를 넘기면 안 된다.
- **GRAD4 는 noise_perlin.c GRAD 의 사본**: 값 SoT 는 바닐라 <clinit>.
  불일치는 df_x4/df_cones 비트 대조가 즉시 잡지만, 편집 시 두 곳을
  같이 볼 것.
- **콘 프로그램의 스테일 scratch 는 정상 상태**: lazy 이후 비선택
  브랜치 노드의 scratch 는 이전 점 값이다. scratch 를 읽는 새 코드를
  추가할 때 "콘/프로그램이 이 점에서 그 노드를 평가했는가" 를 물을 것
  (mask assert 가 마커 폴백만 지킨다).
- **-mavx2 는 TU 격리**: 새 SIMD 코드는 반드시 df_simd_avx2.c (또는
  동일 정책의 새 TU) 에 — 다른 TU 에 인트린식을 넣으면 AVX2 부재
  호스트에서 SIGILL.

## 6. 갱신된 비중 + 다음 순서 판단

FREE gen wall 2200.7 ms 기준:

| 페이즈 | wall | 비중 |
|---|---|---|
| 체인(04+07+08, 20스레드) | 1289.1 ms | **58.6%** |
| 데코+라이트 DAG | 606.1 ms | 27.5% |
| sha256 | 133.3 ms | 6.1% |
| serialize | 93.8 ms | 4.3% |
| pp + light_final + setup | ~62 ms | 2.8% |

체인 내부 CPU: noise 55.5% / **surface 34.8%** / nc_init 6.3% /
carvers 3.3%.

1. **07 surface 가 처음으로 주요 병목으로 부상** (체인 CPU 8.4s —
   P2-0 이후 무손대). 다음 태스크 1순위 후보: surface 룰 트리
   (cond_test/rule_apply, perf 상위) 의 콘/캐시/배치화 — P2-5 의 기존
   항목이 아니라 신규 검토 필요.
2. **체인-DAG 융합 재평가 발동** (P2-3 §5-4 조건 충족): 체인 wall
   1289 vs DAG 606 — 융합 상한 견적 gen wall ≈ 1.6s (1.37x 추가).
   P2-4 로 체인이 짧아져 이득 폭도 줄었다 — surface 개선 후 재견적.
3. sha256 6.1% — SHA-NI (P2-5) 실효 상승.
4. 노이즈 잔여 13.4s CPU 의 추가 인하는 P2-6 (AVX-512, 베어메탈) 축 —
   gradient 전치 비용과 ymm 16개 스필이 AVX2 의 구조적 한계.

## 7. 커밋

- 3d18e34 perf(df): lazy 브랜치 평가 — 콘 프로그램 (P2-4 §1)
- 192d1d1 feat(simd): AVX2 4-레인 콘 스트림 평가기 + cpuid 디스패치 (P2-4 §2)
- cd2238a fix(gate): parity_gate.sh 항상-재빌드 (스테일 false-PASS 채널 봉쇄)
- (본 노트 커밋)

원자료 (로컬-only, gitignore): `bench/results/20260807T025606Z-…-replay-p24-before.jsonl`,
`…T025636Z-…-free-p24-before.jsonl`, `…-p24-lazy.jsonl` (replay/free),
`…T040126Z-…-replay-p24-avx2.jsonl`, `…T040150Z-…-free-p24-avx2.jsonl`,
bench-o2 쌍, `/tmp/p24/perf_{before,after}.data` (janitor 대상, 요지 §2 흡수).
