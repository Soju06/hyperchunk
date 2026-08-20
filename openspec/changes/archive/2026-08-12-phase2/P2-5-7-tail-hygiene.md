# P2-5+7 — 직렬 꼬리 (SHA-NI) + 하이진 완료 노트 (2026-08-07)

P2-0 §8 P2-5/P2-7 행. P2-3 §6 "안 함" 판단 (pp/sha 스레딩 대신 SHA-NI)
의 이행 + P2-0 이관 하이진 3항목. **절대치는 이 VM(claw) 참고치, 유효한
것은 비중·배율** (P2-0 §0 동일 주의). 이 런은 529 overload 중단 후
리줌으로 완료 — 미커밋 8파일 감사 → 빌드 → 게이트 → 커밋 순.

## TL;DR

- **P2-5 §1 SHA-NI (커밋 215e4c9)**: sha256 페이즈 wall **129.3 → 21.5
  ms (6.0x)**. FREE gen wall **2202.8 → 2088.9 ms (1.055x, 청크당 2.04
  ms)**, REPLAY 7116.2 → 6925.1 ms (1.028x). 같은-바이너리 A/B
  (`--sha sw`/`ni`) 로 백엔드 효과 격리: sha 136.1 → 21.6 ms (6.3x).
  P2-0 베이스라인(198.0s) 대비 누적: REPLAY **28.6x** / FREE **94.8x**.
- **P2-7 하이진 (커밋 4c7e339, 83aa478)**: Release -Werror 완화 원복
  (snprintf fail-loud + `(void)cap`), HC_DIAG_DENSITY 통삭, 핫패스
  디버그 env 4종 1회 캐시. 성능 기여 없음이 실측 (FREE 2202.5 →
  2202.8 — 변화 < 런 편차) — 하이진 목적 그대로.
- **P2-5 §2 데코 단가 / §3 light09·serialize: 전부 "안 함"** — 단일
  스레드 perf 실측 (§4): 최량 항목(하이트맵 4-스캔 융합)조차 FREE gen
  wall 기준 ~1.9% < 3%, 문자열-테이블 후보군은 전부 ≤0.09% 로 소멸.
- 신규 게이트 2종: **test_sha256** (ctest #36 — KAT×양백엔드 + 이중
  배터리 199 + obfuscateSeed 회귀), **check_sha_equiv.sh** (풀 리전
  `--sha sw`/`ni` 각각 canonical 판정). 기존 게이트 전부 green.
- **P2-6 베어메탈 진행 가능 상태** (§6 판단).

## 1. 구현 — SHA-NI (P2-5 §1)

용도 재확인: hc_sha256 은 (a) BiomeManager.obfuscateSeed 재현 (파티
불변 경로), (b) 벤치/게이트 canonical 판정 해시 (~47MB/런 — 이번
최적화 대상). 판정 해시가 자기 자신이므로 **백엔드가 갈리면 판정면이
갈린다** — 양 백엔드 상시 검증이 신규 게이트의 존재 이유.

- `sha256_ni.c` (신규 TU): sha256rnds2/msg1/msg2 커널, Intel SHA
  Extensions 레퍼런스 구조. K 상수 벡터는 sha256.c `K[64]` 사본
  (GRAD4 규약 — 불일치는 KAT 가 즉시 잡는다). `-msha -msse4.1` TU
  격리 (df_simd_avx2.c 와 동일 정책), 비-x86 은 abort 스텁 — 진입은
  cpuid 디스패치 뒤에만 (P2-4 SIGILL 함정 규약).
- `sha256.c`: 블록 압축을 `hc_sha256_blocks_sw/ni` 로 분리, 디스패치는
  df_isa.c 패턴 그대로 — `__builtin_cpu_supports("sha")+("sse4.1")` +
  `_Atomic` relaxed 1회 캐시 (TSan 클린), `hc_sha256_force` (미지원
  호스트 강등, 생성 스레드 기동 전 호출 전제).
- bench: `--sha sw|ni|auto` + 요약/JSON `sha` 백엔드 표기 (`--isa` 규약
  동일 — ni 강제는 cpuid 통과 시에만).
- FMA 게이트와의 관계: sha256rnds2 는 vfmadd 패턴 비대상 — no_fma
  3아카이브 재확인 (74,645/74,435/91,584 insn).

## 2. 전/후 실측 (bench-o3, 20스레드, 3런 중앙값, seed 1234567890 r.0.0)

전 = 8d47e07 (P2-4 HEAD), hyg = 83aa478 (P2-7 2커밋), sha = 215e4c9
작업트리 (SHA-NI). **매 런 canonical PASS** (REPLAY = 골든 상수, FREE =
own-v1 상수). 중단 전 런의 실측 재사용 — 파일 mtime (≤14:59) <
벤치 시각 (15:00) 으로 현 트리와의 일치 검증. confirm = 커밋 후 재실측.

| 지표 | 전 | hyg | sha | confirm (HEAD) |
|---|---|---|---|---|
| sha256 wall (FREE) | 133.5 ms | 129.3 ms | **21.5 ms** | 21.6 ms |
| **FREE gen wall** | 2202.5 ms (편차 3.0%) | 2202.8 ms (1.2%) | **2088.9 ms** (5.2%) | 2057.9 ms (0.7%) |
| **REPLAY gen wall** | 7173.1 ms (4.3%) | 7116.2 ms (1.0%) | **6925.1 ms** (0.2%) | 6963.5 ms (0.3%) |
| FREE 청크당 | 2.15 ms | 2.15 ms | 2.04 ms | 2.01 ms |

- 같은-바이너리 A/B (HEAD, `--sha sw` vs `ni`, 3런 중앙값): sha 페이즈
  FREE 136.1 → 21.6 ms / REPLAY 131.8 → 21.2 ms (**6.2~6.3x**). gen
  wall 델타는 sha 델타 (~110 ms) 와 정합.
- 체인/DAG/serialize 는 전 구간 동등 (체인 1283~1305, DAG 611~639,
  serialize 91~97 — 무손대 방증).
- 하이진 2커밋의 성능 기여 없음 (전↔hyg 변화 < 편차) — 예상대로.

## 3. 게이트

| 게이트 | 결과 |
|---|---|
| ctest (build) | **36/36** (신규 #36 sha256) |
| **test_sha256 (신설)** | FIPS 180-4 KAT (빈/abc/2블록경계/1M-a) × sw+ni 강제, 이중 백엔드 배터리 199케이스 (길이 0..193 전 위상 + 대형 5, 동결), force(1) 강등 검사, obfuscateSeed 골든 회귀. SHA-NI 부재 호스트는 sw 파트만 (PASS — 공허 아님) |
| **check_sha_equiv.sh (신설)** | PASS — 풀 리전 `--sha sw` / `--sha ni` 둘 다 골든 canonical (전이적 다이제스트 동일; isa_equiv 와 동일 논리, 부재 호스트 SKIP) |
| parity_gate.sh (build / bench-o2 / bench-o3) | 3/3 PASS, canonical a5963205…3c24 (불변) — auto=ni 로 실행되므로 NI 경로의 obfuscateSeed 포함 판정 |
| check_no_fma.sh (3아카이브) | PASS (74,645 / 74,435 / 91,584 insn) |
| check_isa_equiv.sh | PASS (scalar / avx2) |
| check_sanitizers.sh (ASan+UBSan, 36종) | PASS (102.8 s) |
| check_tsan.sh (TSan 36종 직렬) | PASS (410.6 s) — sha 디스패치 원자 접근 포함 레이스 0 |
| 벤치 런 자체 해시 판정 (전 3+3 / hyg 3+3 / sha 3+3 / confirm 3+3 / sw-A/B 3+3) | 전부 PASS |

## 4. "안 함" 판정 (P2-5 §2·§3 — 실측 근거)

단일-스레드 FREE perf (cycles, 10,776 샘플 ≈ 21.6 s CPU; DAG cpu 실측:
deco 5129 / l09 1903 / l08 108 / prepare 179 ms):

1. **데코 단가 항목 (P2-0 §6.2 후보군) — 안 함.** 후보 전수 실측:
   `hm_prime_one` **3.47%** (≈0.75 s, 데코 CPU 의 ~15%) 가 유일한
   가시 항목. 나머지 전부 소멸: hm_update 0.46% / schedule_tick 0.28% /
   strlen 0.25% / jset_add 0.09% / jset_poll 0.05% / strcmp 0.06% /
   by_name·state_build·strncmp 0.01~0.02% / edge_* 0.05%. **문자열
   테이블화·jset 힌트·by_name 해시·rotate 메모 행은 현 비중에서 이득
   측정 불가** (P2-1~4 가 주변을 다 줄여 상대 소멸). 최량 항목인
   FINAL 하이트맵 4-스캔 융합 (직전 런 정찰이 값-불변 분석 완료:
   4종 술어 동시 판정 단일 하강, 리스크 none) 도 낙관 모델로 DAG wall
   −40 ms ≈ **FREE gen wall 1.9% < 3%**. 단 REPLAY 는 features 직렬이라
   같은 융합이 ~6% 상당 — **미래 features 태스크에 이관 권고** (surface
   태스크와 묶으면 적합).
2. **light09 slot-merge/fastpath + seed 이분화 — 안 함.** l09 는 P2-3
   부터 DAG 내부 (cpu 1903 ms, wall 기여 ~130 ms — P2-3 §6-2). 전체
   2x 가정 시 DAG wall −65~80 ms = 3.2~3.9% (스루풋 비례 낙관 모델) 이
   상한이고, 크리티컬 패스는 deco (5129 ms) 라 실효는 그 이하. P2-0
   추정 1.5~2.5x 는 드레인 한정이라 l09 전체 2x 자체가 낙관. BFS
   큐-순서 변경은 lfp 논증 의존 (P2-3 §5-2) — 경계선 이득 + 비영
   패리티 리스크 → 기각.
3. **serialize 번들 — 안 함.** CPU 항목 개별 ≤0.5% (직전 런 정찰
   마이크로벤치: pack_bits ~140 ms CPU-sum / 틱-스캔 ~123 ms / put_i64
   ~21 ms — 세 항목이 직렬 serialize 287 ms 의 사실상 전부). FREE
   serialize wall 97 ms 중 **~75 ms 가 병렬화 오버헤드 플로어** (스레드
   생성/first-touch 폴트/47MB 직렬 concat) — CPU 만 고쳐선 wall 이 안
   움직인다. 풀 번들 (CPU + 워커풀/프리폴트/병렬 concat) 이어야 2.5~3%
   로 기준선상인데, 오버헤드 플로어는 P2-6 베어메탈에서 구조가 달라진다
   → 거기서 재실측 후 판단.

## 5. 함정 기록 (다음 태스크용)

- **이 VM 은 perf_event_paranoid=4** — perf 는 `sudo sysctl -w
  kernel.perf_event_paranoid=1` 후 원복으로. P2-4 때와 달리 기본 차단.
- **run_bench.sh 는 --sha/--isa 패스스루 없음** — 같은-바이너리 A/B 는
  bench 직접 호출로 (결과는 수동으로 bench/results/ 규약 이름).
- **test_sha256 배터리 카운트 199 동결** (df_x4 규약) — 위상 케이스를
  바꾸면 카운트 검사도 같이.
- **check_sha_equiv 는 bench stderr `sha=` 표기에 의존** (isa_equiv 의
  `isa=` 와 동일 취약면) — bench 요약 포맷 변경 시 두 게이트 grep 확인.
- **529 리줌 검증 요령**: 중단 런의 벤치 결과는 소스 mtime < 벤치
  타임스탬프면 현 트리 실측으로 재사용 가능 — 이번에 그 검증으로
  before/hyg/sha 3세트를 살렸다 (fresh confirm 으로 이중 확인).

## 6. 갱신된 비중 + P2-6 판단

FREE gen wall 2088.9 ms (sha 런) 기준:

| 페이즈 | wall | 비중 |
|---|---|---|
| 체인(04+07+08, 20스레드) | 1283.2 ms | **61.4%** |
| 데코+라이트 DAG | 639.0 ms | 30.6% |
| serialize | 97.0 ms | 4.6% |
| sha256 | 21.5 ms | 1.0% |
| pp + light_final + setup | ~48 ms | 2.3% |

체인 내부 CPU: noise 55.4% / **surface 34.8%** / nc_init 6.3% /
carvers 3.3% (P2-4 와 동일 — 이번 런 무손대 방증).

**P2-6 베어메탈 진행 가능 판단 — 가능:**

1. **직렬 꼬리는 정리 완료**: sha 6.1% → 1.0%. 남은 직렬은 serialize
   4.6% (오버헤드 플로어 — 베어메탈에서 재실측이 곧 진단) 뿐.
2. **ISA 이식성 게이트 완비**: 백엔드 강제 게이트 2종 (isa_equiv,
   sha_equiv) + 유닛 이중 배터리 2종 (df_x4, sha256) + 3-빌드 parity +
   ASan/TSan — 새 호스트에서 cpuid 가 뭘 고르든 그대로 판정 가능.
   SHA-NI/AVX2 부재 호스트도 폴백+SKIP 정책으로 동작.
3. **AVX-512 진입점 준비됨** (P2-4): enum 슬롯 + 디스패치 구조 + x4
   화이트리스트. P2-6 1번 항목 (gradient vpermt2pd) 의 이득 지점 식별
   완료.
4. 로컬에서 더 짜낼 대형 항목은 **surface (체인 CPU 34.8%)** 하나 —
   신규 태스크 필요 (P2-4 §6 1순위 유지). 베어메탈 전 필수 아님;
   VM 절대치가 참고치인 이상 베어메탈 착수를 막을 이유가 없다.

## 7. 커밋

- 4c7e339 hygiene(build): -Werror 완화 원복 — snprintf fail-loud + (void)cap (P2-7 §1)
- 83aa478 hygiene(hotpath): HC_DIAG_DENSITY 통삭 + 디버그 env 4종 1회 캐시 (P2-7 §2-3)
- 215e4c9 feat(sha): SHA-NI sha256 블록 압축 백엔드 + cpuid 디스패치 (P2-5 §1)
- (본 노트 커밋)

원자료 (로컬-only, gitignore): `bench/results/20260807T14232*…-p257-before.jsonl`,
`…T14455*/T14461*-p257-hyg.jsonl`, `…T15005*/T15010*-p257-sha.jsonl`,
`…T15274*/T15275*-p257-sha-confirm.jsonl`, `…T152830Z-…-p257-shasw.jsonl`
(같은-바이너리 A/B), `/tmp/p257/perf_deco.data` (janitor 대상, 요지 §4
흡수). 직전 런 로그: `~/hyperchunk-runs/p2-57.jsonl`.
