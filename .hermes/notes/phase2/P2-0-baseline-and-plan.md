# P2-0 — 로컬 베이스라인 프로파일링 + Phase 2 최적화 플랜 초안 (2026-08-06)

Phase 1 완료 상태(r.0.0 1024청크 canonical byte-exact, ctest 30/30)에서
Phase 2(성능) 진입을 위한 베이스라인 실측과 플랜. **이 VM(claw)은 CPU
토폴로지 오보고(22c/1t 표기, hypervisor) — 절대치는 전부 "이 VM 참고치"
이고 유효한 것은 비중·순위·전/후 배율뿐이다** (ADR-002 Pitfall 5).

## TL;DR

- 하네스 `bench/run_bench.sh` 커밋 (f807974). 매 실행이 canonical 해시를
  판정하므로 **수치가 찍혔다 = 그 빌드가 비트 패리티를 유지한다**.
- 벤치 빌드 parity_gate PASS (bench-o2 / bench-o3 둘 다) + check_no_fma
  PASS (o2 66,991 / o3 82,174 insn). 릴리즈 O-레벨 **-O3 확정** (커밋
  0b7fa13; 근거 §3.3).
- 풀 리전(1024청크) gen wall 중앙값 **198.0s, 청크당 193ms** (-O3,
  20스레드, 3런, wall 편차 5.6% / CPU-sum 편차 1.2%).
- 스테이지 분해: **체인(04+07+08)이 gen wall의 96.7%, 체인 CPU의 99.5%가
  04 노이즈**. 직렬 페이즈는 features 3.8s + light09 2.1s + 직렬화 0.3s
  + sha 0.13s 수준.
- 핫스팟: **cycles의 ~99.2%가 DF 인터프리터 스택** — `hc_perlin_sample_scaled`
  62.8%, `hc_octaves_value` 22.7%, `eval_node` 7.0%.
- "노이즈(density function) 지배" 가설은 확인 — 단 ADR-002가 예측한 45%가
  아니라 **~97%**. 초과분의 원인은 커널 폭이 아니라 **`hc_df_eval_ex`의
  전-프리픽스 평가 구조**(§5) — 코드 유도상 바닐라 동등 작업량의 ~100배
  펄린 샘플을 계산 후 폐기한다.
- 1순위 태스크는 SIMD가 아니라 **라이브-콘 평가(P2-1, 패리티 리스크
  없음, 노이즈 스테이지 5-20x 추정)**. SIMD/병렬화 우선순위는 P2-1 후
  재측정으로 재결정 (ADR-004 anti-goal과 정합).

## 1. 하네스와 방법론

재현 (1줄):

```
bench/run_bench.sh                  # bench-o2, 20스레드, 3회
bench/run_bench.sh -p bench-o3      # -O3 비교
bench/run_bench.sh -t 1 -n 1        # 단일 스레드 (프로파일 귀속용)
```

- `bench/hyperchunk_bench.c` = `cli/hyperchunk_verify.c`와 동일 파이프라인
  시맨틱(소스 오브 트루스 `tests/parity/test_full_region.c`) + 계측.
  코어 무손대. 매 실행 canonical 해시 판정 (불일치 = exit 1 = 수치 무효).
- 체인 스테이지(04/07/08 + nc_init/beard)는 워커 스레드별
  `CLOCK_THREAD_CPUTIME_ID` 누적 합("CPU-sum") — VM steal에 강건한 비중
  지표. 처리량은 체인 페이즈 wall. 직렬 페이즈(09 이후)는 wall 누적.
- 리플레이 입력 파싱(`replay_load`)과 postprocess 마크 대조(`pp_verify`)는
  하네스 오버헤드 버킷으로 분리 — `gen_wall_ns`에서 제외 (~55ms/런).
- 반복은 프로세스 단위 셸 루프 (in-process 반복은 BE/틱 레코더 상태 누적
  때문에 금지).
- 원자료: `bench/results/20260806T100235Z-bench-o2-t20.jsonl`,
  `…T101323Z-bench-o3-t20.jsonl`, perf 상위 함수
  `bench/results/20260806-perf-o3-top-functions.txt` (전부 로컬-only,
  gitignore).

## 2. 게이트 (벤치 빌드 패리티)

| 게이트 | bench-o2 | bench-o3 |
|---|---|---|
| `BUILD=build-bench-oX scripts/parity_gate.sh` | PASS (canonical a596…3c24) | PASS (동일 해시) |
| `scripts/check_no_fma.sh build-bench-oX/core/libhyperchunk.a` | PASS 66,991 insn | PASS 82,174 insn |
| 벤치 런 자체 해시 판정 (3런 × 2구성 + perf 런) | 7/7 PASS | — |

최적화 플래그(-O3 포함)가 패리티를 깨지 않음을 확인. FP 플래그
(`-ffp-contract=off -fno-fast-math`)는 루트 CMakeLists 고정 —
`HC_OPT_LEVEL` 캐시 변수는 O-레벨만 노출.

## 3. 베이스라인 측정 (이 VM 참고치)

시드 1234567890, r.0.0, 20스레드, 3회 연속. 게이트 전 런 PASS.

### 3.1 런별 (gen wall = 생성 페이즈 wall 합, 하네스 오버헤드 제외)

| 구성 | run1 | run2 | run3 | 중앙값 | wall 편차 | noise CPU-sum 편차 |
|---|---|---|---|---|---|---|
| -O2 | 206.9s | 218.2s | 218.7s | **218.2s** | 5.4% | 1.28% |
| -O3 | 195.3s | 198.0s | 206.3s | **198.0s** | 5.6% | 1.25% |

체인 가동률(CPU-sum ÷ wall×20): O2 76.9~80.4%, O3 81.5~85.1% — 미달분은
VM steal/선점. **wall 편차 5%대는 대부분 가동률 흔들림**이고, 같은 작업량
지표인 CPU-sum은 1.3% 이내로 재현된다. 결정론 판단은 CPU-sum 기준.

### 3.2 스테이지 분해 (-O3 중앙값 기준)

체인 페이즈 (wall 191.3s, CPU-sum 3220.5s — 20스레드 병렬):

| 스테이지 | CPU-sum | 체인 CPU 비중 |
|---|---|---|
| 04 noise (aquifer/ore vein 포함) | 3205.7s | **99.54%** |
| 07 surface | 8.3s | 0.26% |
| nc_init (DF 셀 구조 준비) | 4.9s | 0.15% |
| 08 carvers | 1.7s | 0.05% |
| beard | 0.03s | 0.00% |

직렬 페이즈 (wall):

| 페이즈 | -O2 | -O3 | 비고 |
|---|---|---|---|
| 09 features (구조물 step 포함) | 3.78s | 3.75s | 동등 |
| light 09 배치 (BFS 시딩+드레인) | 3.32s | **2.11s** | O3 1.6x, 3런 일관 |
| light 08 init | 0.109s | 0.089s | |
| light flush | 0.031s | 0.027s | |
| light final freeze | 0.056s | **0.027s** | O3 ~2x |
| postprocess | 0.041s | 0.037s | |
| serialize (nbt 1024청크) | 0.342s | 0.307s | |
| sha256 (~48MB) | 0.138s | 0.133s | 스칼라 구현 |

gen wall 비중 (-O3): 체인 96.7% / features 1.9% / light09 1.1% / 나머지
합 ~0.3%. 청크 수 참고: 체인 1681(마진 포함 41×41), 데코 1461, pp 1165,
방출 1024. maxrss 0.63GiB.

### 3.3 릴리즈 플래그 판정: -O3

- 지배 비용(노이즈 인터프리터)은 **CPU-sum 기준 O2/O3 동등** (3227.9 vs
  3205.7s, 차 0.7% < 런 편차 1.3%) — 의존-체인 바운드 인터프리터라
  자동벡터화 무효. gen wall의 "1.10x"는 가동률 차이가 섞인 수치라 코드젠
  효과로 **주장하지 않는다**.
- 실이득은 국소적·일관적: light09 1.6x, light_final ~2x, serialize 소폭
  (정수 BFS/패킹 루프가 벡터화 수혜; 배율은 3런 중앙값 기준).
- 손해 없음 + 게이트 전부 PASS → release 프리셋 `HC_OPT_LEVEL=-O3` 반영.
  베어메탈에서 재검증 항목으로 유지.
- 부수 발견: release(NDEBUG) 프리셋은 지금껏 빌드된 적이 없었고 -Werror
  잠복 이슈 2건이 드러남 — Release 한정 완화로 우회, 본 수정은 P2-7
  (structures.c:41 `cap` (void) 처리, :659 snprintf).

## 4. 핫스팟 상위 20 (perf cycles:u, -O3, 20스레드, 336,595 샘플)

perf 하드웨어 카운터는 이 VM에서 **가용** (PMU 가상화 확인, callgrind
폴백 불요). `kernel.perf_event_paranoid` 1로 임시 하향 후 원복.

| # | % | 함수 | 스테이지 귀속 |
|---|---|---|---|
| 1 | 62.78 | hc_perlin_sample_scaled | 04 noise (DF 커널) |
| 2 | 22.69 | hc_octaves_value | 04 noise (DF 커널) |
| 3 | 7.02 | eval_node | 04 noise (DF 인터프리터) |
| 4 | 2.15 | hc_octaves_wrap | 04 noise (DF 커널) |
| 5 | 1.25 | hc_blended_compute | 04 noise (BlendedNoise) |
| 6 | 1.22 | hc_df_eval_ex | 04 noise (인터프리터 루프) |
| 7 | 1.18 | spline_sample.isra.0 | 04 noise (스플라인) |
| 8 | 0.88 | hc_normal_noise_value | 04 noise (DF 커널) |
| 9 | 0.16 | hc_biome_zoom | 07 surface / 09 features |
| 10 | 0.07 | hc_gen_noise_stage | 04 noise (doFill 루프) |
| 11 | 0.07 | hc_nc_select_cell_yz | 04 noise (셀 채움) |
| 12 | 0.07 | hc_aquifer_substance | 04/08 (aquifer) |
| 13 | 0.05 | derive_geometry_chunk | light (배치 재유도) |
| 14 | 0.05 | hc_structures_step | 09 features (스텝 스캔) |
| 15 | 0.04 | cond_test | 07 surface (룰 조건) |
| 16 | 0.03 | rule_apply | 07 surface (룰 트리) |
| 17 | 0.03 | hc_nc_update_for_z | 04 noise (증분 lerp) |
| 18 | 0.03 | hm_prime_one | 09 features (하이트맵 프라임) |
| 19 | 0.02 | hc_beard_compute | 04 noise (beard) |
| 20 | 0.02 | hc_nbt_list_at | setup/serialize (NBT) |

상위 8개(99.2%)가 전부 DF 평가 스택. 9위 이하는 전부 0.2% 미만 — 현
시점에서 노이즈 외 최적화는 전체 배율에 거의 기여하지 못한다.

## 5. 사전 예측 대비 + 지배 비용의 구조적 원인

| 예측 (출처) | 실측 | 판정 |
|---|---|---|
| 스테이지 비중 noise 45% / surface 8% / carvers 10% / features 22% / light 15% (ADR-002, 추정) | noise ~97% / surface 0.3% / carvers 0.05% / features ~2% / light ~1.3% | **noise가 예측을 2배 이상 상회.** ADR-004 "When this might break" 조건("noise ≫ 45%면 우선순위 재조정") 발동 |
| 바닐라 인터프리터 실효 0.03~0.08 flops/cyc, 격차 96~256x (ADR-002) | 이 구현도 같은 병목 계급 — cycles 99%가 인터프리터+커널 | 방향 일치 |
| CPI 재작성 후 noise 21% / features 41% 이동 (ADR-004) | 미검증 — P2-1 후 재측정 대상 | 보류 |

**원인 진단 (코드 유도, wf_17b5f645 코어 맵 + 실측 프로파일 합치):**
`hc_df_eval_ex`(core/src/df_eval.c:368)가 루트의 실제 의존 콘이 아니라
**IR 배열 프리픽스 `[0..root]` 전체를 매 호출 평가**한다. 결과:

- final_density CELL 평가 98,304회/청크 × 210노드 프리픽스 — 라이브는
  ~24노드뿐 (interpolated 마커 5개 하위의 노이즈 서브트리는 계산 후 폐기).
- ore vein: solid 블록(~3-4만/청크)마다 `vein_toggle`(root=261, 알파벳순
  마지막 슬롯 = **전 그래프 262노드**) + 기생 find_top_surface y-사다리
  (~59노드 × 15-25회)를 평가하고 마커 값만 읽는다.
- fill_slice: 같은 점에서 interp 자식 8개를 각각 별도 프리픽스 워크로
  재평가 (점당 1,727노드; hc_df.h:99의 의도된 다중-루트 읽기 미사용).
- aquifer floodedness/spread/lava(root 210-212)도 final_density 콘 전체를
  프리픽스로 끌고 온다.

합계 **~3×10⁷ 펄린 샘플/청크 vs 바닐라 동등 콘 평가 ~2.5×10⁵ (~100배)**
— perlin 62.8%라는 프로파일과 정합. 즉 실측 노이즈 비중 ~97% 중 대부분은
"노이즈가 원래 비싸서"가 아니라 **버려지는 평가**다. 이것이 Phase 2
우선순위를 결정한다: 구조(콘) 수정 → 재측정 → 그다음 SIMD/병렬화 배분.

## 6. 최적화 후보 목록

이득은 전부 **측정 전 추정**(코드 유도 근거 병기), 패리티 리스크 등급은
FP 연산 순서·RNG 소비 순서·관측가능성 분석 기반 (코어 맵 wf_17b5f645,
모듈별 상세는 /tmp/p2map/*.json — 휘발성, 요지는 아래에 흡수).

### 6.1 노이즈 스테이지 (실측 지배 — 최우선)

| 후보 | 예상 이득 | 난이도 | 패리티 리스크 | 근거/비고 |
|---|---|---|---|---|
| **(root×mode)별 라이브-콘 평가** — 프리픽스 워크를 컴파일 시 계산한 콘 리스트 워크로 교체. CELL/BLOCK 콘은 interpolated·flat_cache 하위 제외, SP 콘은 flat_cache 창-밖 폴백 대비 자식 포함 | 노이즈 스테이지 **5-20x** (노드 방문 ~10⁸→~10⁷/청크; vein_toggle 262→~1, CELL 210→~24) | medium | **none** | 평가 경로 RNG 0회(시딩은 전부 컴파일 타임), eval_node 순수 — 죽은 값 생략은 관측 불가. 남는 노드의 연산은 재배열 금지. FTS build_cone(df_compile.c:287)과 동형 |
| fill_slice 다중-루트 단일-워크 (8 interp 자식을 한 워크의 scratch에서 회수) | 슬라이스 채움 ~8x (콘과 결합 시 중복) | low | none | hc_df.h:99가 명시한 의도된 사용법 |
| ore_veins y-범위 선가드 (y∉[-60,50]이면 toggle 평가 전 -1) | vein 평가 ~71% 제거 (범위 111/384) | low | none | RNG는 toggle 통과 후에만 소비 (ore_veins.c:45) |
| FTS 콘의 y-불변 분할 + cache_2d 실메모화 (바닐라 Cache2D 동형) | FTS 탐색 ~10-25x, 컬럼 2D 스택 49x→1x | medium | low | 동일 입력 재사용 = 비트 동일. 데이터팩 일반화(Task 12) 시 판정을 콘 빌더에 내장 |
| beard 조각별 영향 박스 프리필터 | 구조물 청크 한정 수십 배 | medium | low | 스킵 조건 "기여가 정확히 +0.0"일 때만; 합산 순서(i 오름차순) 보존 필수 |
| AVX2 SoA 배치 커널 (셀 128포인트/컬럼 49y 레인화; jmin/jmax는 cmp+blend로 Java 규칙 재현, minpd/maxpd 직접 사용 금지) | 콘 적용 후 잔여 워크 2-4x | high | low~medium | 레인별 연산 시퀀스 스칼라와 동일 유지, 수평 리덕션 금지, FMA 금지 유지. 백엔드 바이트 동일 게이트(ADR-004 D4) 필수 |
| vpermt2pd gradient 룩업 등 AVX-512 커널 | Zen5 +27% / Zen4 +12% (ADR-004 유도) | high | low~medium | 로컬(Zen3) 실측 불가 — 베어메탈 (§8 P2-6) |
| 인터프리터 미세화 (96B 그래프 복사 제거, 노드 56B→32B 압축, computed goto) | 잔여 1.2-1.5x | low | none | 콘/SIMD 후순위 |

### 6.2 직렬 페이즈 (현 비중 ~3% — P2-1 후 비중 상승 시)

| 후보 | 예상 이득 (구간 한정) | 난이도 | 패리티 리스크 |
|---|---|---|---|
| light BFS slot() 조회 합치기 + 내부-청크 패스트패스 | 드레인 1.5-2.5x | low | none |
| light 섹션 비-공기 마스크 증분 유지 (배치마다 전-청크 derive_geometry 재유도 제거) | prepare 10x+ | medium | low |
| seed_sky/block 스캔 이분화·섹션 스킵 | 시딩 1.5-3x | low~medium | none~low |
| features: FINAL 하이트맵 4종 프라임 단일 스캔 융합 | 프라임 ~4x | low | none |
| features: 문자열 술어 팔레트-id 테이블화 (strstr/strchr 제거) | 후처리 국소 1.3-2x | low | none |
| features: jset poll 버킷 힌트, set_block no-op 스킵, 핫패스 getenv 제거 | 한 자릿수 % | low | none~low |
| structures: 스텝 디스패치 역전 (26×289×n_starts 전수 스캔 → 버킷) | 스캔 ~10⁴x 소멸 (perf 0.05%) | low | none |
| structures: state rotate/mirror 메모, hc_block_by_name 해시화 | 배치 단가 수십 배 | low | none |
| serialize: pack_bits 특수화(나눗셈 제거), long 배열 bulk 방출, 틱 프리버킷팅, HashMap 순서 캐시 | 구간 2-5x | low | none |
| sha256 SHA-NI 디스패치 (호스트 sha_ni 확인) | 해시 5-8x | low | none |
| 직렬화 청크별 병렬화 (연접·해시만 idx 순차) | 코어 수 근접 | medium | none |

### 6.3 병렬화 (스테이지 스케줄러)

| 후보 | 예상 이득 | 난이도 | 패리티 리스크 |
|---|---|---|---|
| FREE 모드 스케줄러 (ADR-008 D1/D2: 같은 스테이지 코드, 정책만 교체) — 체인은 이미 청크-병렬, features를 체비셰프 거리 ≥3 웨이브프런트/컬러링으로 병렬화 | features 구간 코어 수 근접 | high | medium (충돌쌍 상대순서 보존이 조건) |
| light 타임라인 병렬화 | — | — | **불가** (accum 모드는 이벤트 순서가 제품 시맨틱) |

### 6.4 반(反)-기회 (하지 않는다)

- geode 셀 노이즈/역제곱근 벡터화 — FP 누적 순서가 계약 (features_ring.c:479).
- 구조물 배치·postprocess 병렬화 — manifest 총순서가 BE 기록·래치·지형
  읽기 의존을 고정.
- FMA 활성화, float 다운캐스트, GPU 오프로드 — ADR-002/004 anti-goals.
- 노이즈 커널의 libm/근사 대체 — JDK sin/cos 1-ulp 문제와 동류
  (memory: jdk-sincos-not-libm).

## 7. ADR 제약 반영 (플랜 전제)

- **FMA 전면 금지** 유지: `-ffp-contract=off` 루트 고정, release 아카이브
  check_no_fma 게이트, -flto 금지 (게이트 공허화).
- **AVX2 기본 + AVX-512 cpuid 런타임 디스패치** (ADR-004 D1/D2), 전 백엔드
  출력 **바이트 동일** CI 게이트 (D4): `--isa=scalar|avx2|avx512` 3경로
  sha256 동일 + AVX-512 부재 호스트 폴백 확인. AVX-512는 로컬(5900X=Zen3)
  실측 불가 — Hetzner Robot AX102(Zen4)급 필요 (bench/hosting.md; Cloud
  CCX는 AVX-512 없음 확인, 부적격).
- **벤치는 FREE 모드, 패리티 배지는 REPLAY 모드** (ADR-008 Pitfall 2) —
  현 하네스는 REPLAY 재생 기반이므로 P2-3 이후 FREE 벤치 경로 추가 시
  구분 표기 필수.
- **멀티스레드 확장 진입 시 TSan 게이트 의무** (ADR-009 Pitfall 3).
- 벤치 수치는 ASan 빌드에서 뽑지 않는다 (ADR-009 Pitfall 2 역방향).
- 스테이지 코드는 단일 구현 공유 — 검증용/벤치용 분리 금지 (ADR-008 D2).

## 8. Task 분해 제안 (P2-1..7)

각 태스크 완료 기준에 "ctest 30/30 + parity_gate + check_no_fma +
bench/run_bench.sh 전/후 배율 기록"을 포함한다. 순서 근거: 실측상 노이즈
~97%이므로 구조 수정(P2-1/2)이 선행하고, 그 후 재측정이 P2-3 이후의
배분을 결정한다 (ADR-004 "features 우선" anti-goal은 CPI 재작성 이후
비중 기준 — 지금 단계엔 미적용).

| # | 내용 | 예상 | 리스크 | 게이트 추가 |
|---|---|---|---|---|
| **P2-1** | df_eval 라이브-콘 평가 (root×mode 콘 리스트) + fill_slice 단일-워크 + ore y-가드 + 인터프리터 미세화. 완료 시 **재측정으로 비중 갱신** | 노이즈 스테이지 5-20x, 전체 wall ~수 배 | 패리티 none — 단 SP flat_cache 창-밖 폴백 케이스를 콘에 보수 포함 + assert | 기존 게이트로 충분 (콘 == 값-불변) |
| **P2-2** | FTS y-불변 분할, cache_2d 실메모화, psl 메모 정리, beard 박스 프리필터 | FTS/psl 경로 10x+ | low | noise_stage/router_slots 골든 + mutation-probe (커밋 후) |
| **P2-3** | FREE 모드 스케줄러 (ADR-008 정책 주입점 `hc_schedule_policy`) — features 웨이브프런트, 직렬화 병렬화. **TSan 게이트 신설** | 직렬 페이즈 코어 수 근접 | medium | TSan ctest, FREE-vs-REPLAY 해시 동일성 |
| **P2-4** | AVX2 SoA 노이즈 배치 커널 (ADR-004 D1) — P2-1 재측정 결과로 범위 확정 | 잔여 노이즈 2-4x | low~medium | 백엔드 바이트 동일 게이트 (scalar vs avx2) |
| **P2-5** | 직렬 페이즈 묶음: light slot/마스크, hm 프라임 융합, 문자열 테이블화, pack_bits/bulk 방출/틱 버킷팅, SHA-NI, structures 스텝 버킷·메모 | 구간별 2-10x (전체 기여는 P2-1 후 비중에 따름) | none~low | 기존 게이트 |
| **P2-6** | AVX-512 백엔드 + cpuid 디스패치 + 베어메탈(Hetzner Robot AX102) 실측·공개 수치 | Zen4 +12%/Zen5 +27% (ADR-004) | low~medium | 3-ISA 바이트 동일 + 폴백 게이트 |
| **P2-7** | 하이진: structures.c:41 `(void)cap`, :659 snprintf 정리 → Release -Werror 완화 원복; 핫패스 getenv 제거; gen_noise_stage.c diag 분기 제거 | — | none | 기존 게이트 |

P2-1이 끝나면 노이즈 비중이 급락하고 features/light가 새 병목이 된다는
것이 ADR-004의 예측(21%/41%) — **P2-1 직후 bench/run_bench.sh 재측정으로
이 표의 P2-3~5 순서를 재확인**한다.

## 9. 이 태스크의 커밋

- f807974 feat(bench): P2-0 측정 하네스 (+ HC_OPT_LEVEL, bench-o2/o3 프리셋)
- 0b7fa13 build: 릴리즈 O-레벨 -O3 확정 + Release 한정 -Werror 완화 2건
- (본 노트 커밋)

측정 원자료는 gitignore된 `bench/results/`와 `/tmp/hc_perf_o3.data`
(janitor 대상, 요지는 본 노트에 전량 흡수).
