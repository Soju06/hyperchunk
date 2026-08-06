# P2-1 — df_eval 라이브-콘 평가 완료 노트 (2026-08-06)

P2-0 §8 P2-1 행 그대로: (root×mode) 라이브-콘 평가 + fill_slice 단일-워크
+ ore y-가드 + 인터프리터 미세화(그래프 사본 제거). 전부 값-불변 최적화 —
매 단계 게이트 green, canonical 해시는 P2-0 과 동일 (a5963205…3c24).
**절대치는 이 VM(claw) 참고치, 유효한 것은 비중·배율** (P2-0 §0 동일 주의).

## TL;DR

- **gen wall 198.0s → 9.38s (21.1x), 노이즈 스테이지 CPU-sum 3205.7s →
  49.8s (64.4x)** (-O3, 20스레드, 3런 중앙값, 전 런 canonical PASS).
  P2-0 추정 5-20x 를 상회 — vein 경로의 전-그래프 프리픽스가 추정보다
  컸다 (아래 콘 실측).
- 병목 이동 확인: gen wall 비중 **features 38.0% / 체인 35.0% / light09
  20.7%** (P2-0: 체인 96.7%). ADR-004 의 "재작성 후 features 지배" 예측
  방향과 일치. → P2-3~5 순서 판단은 §5.
- 부수: `structures.c` lcg_large_feature_seed 의 signed 곱 UB 수정
  (사전 존재, Task 14 유래) — **check_sanitizers.sh 가 이제 PASS** 한다
  (그전엔 mineshaft 스캔 첫 청크부터 UBSan 발화로 항상 FAIL).

## 1. 구현 (커밋 55150ab, 936b02d)

### 라이브 콘 (df_eval.c / hc_df.h)

- `hc_df_cone_mark/collect`: 위상 정렬 하향 스윕 (재귀 없음) 으로
  root×mode 의존 콘을 오름차순 리스트로 산출. `hc_df_eval_cone` 은 콘만
  방문 — 평가 순서는 프리픽스 워크와 동일한 인덱스 오름차순이라 남는
  노드의 FP 연산 순서 불변. 평가 경로 RNG 0회 + eval_node 순수 →
  죽은 값 생략은 관측 불가.
- 컷 규칙 (hc_df.h 주석이 SoT):
  - `INTERPOLATED`/`FLAT_CACHE`: CELL/BLOCK 은 셀 상태/테이블만 읽으므로
    자식 컷. **SP 는 자식 보수 포함** — flat_cache 창-밖 폴백(aquifer 의
    청크 밖 컬럼 조회)이 실경로다 (P2-0 명시 사항).
  - **assert**: eval 의 마커 폴백 경로가 콘 멤버십 mask 를 검사한다 —
    CELL/BLOCK 콘에서 창-밖 폴백이 발생하면 (콘 계산 버그) debug 에서
    즉시 발화. SP 는 보수 포함이라 항상 통과.
  - `FIND_TOP_SURFACE`: density(a) 는 기존 ipool 콘이 sc2 로 자족 평가
    — b(upper_bound)만 포함. FTS 는 바닐라가 fresh SinglePointContext
    로 평가하므로 SP 콘 전용 (비-SP 도달 시 -1 fail-loud). 실측상
    비-SP 콘에 FTS 는 등장하지 않는다 (전 게이트 통과가 증거).
- 콘 산출 시점: `hc_nc_init` (청크당 1회, 첫 평가 전). "그래프 컴파일
  직후 1회" 가 아니라 여기 둔 이유: 콘은 (graph, roots) 의 순수 함수라
  청크 간 동일하지만, 그래프에 붙이려면 컴파일 호출부 10곳(테스트 8 +
  CLI + bench)의 시그니처가 전부 바뀐다. 산출 비용은 스윕 O(n=262)
  ×15콘 ≈ µs 급 — 실측 nc_init CPU 는 오히려 2.5x 내려갔다 (flat
  테이블/psl 도 콘으로 빨라져서). 디스패치 미스(미등록 root)는 종전
  프리픽스 워크 폴백 — 항상 옳고 느릴 뿐.

### 콘 실측 (seed 1234567890 오버월드 라우터, g->n=262)

| root×mode | 프리픽스 노드 수 | 콘 |
|---|---|---|
| final_density × CELL | 210 | **14** |
| vein_toggle × BLOCK (root 261) | 262 | **1** |
| vein_ridged × BLOCK / barrier × BLOCK | 246 / 1 | **1** / **1** |
| vein_gap × BLOCK | 258 | 6 |
| preliminary_surface_level × SP | 241 | 51 (FTS 사다리 포함 — P2-2 대상) |
| floodedness/spread/lava × SP (root 210-212) | 211-213 | **1** |
| interp 자식 union × SP (fill_slice) | ~5×210 | 204 (점당 1워크) |

P2-0 §5 의 진단 그대로: solid 판정마다 걷던 전-그래프 262노드 + 기생
FTS y-사다리가 마커 상태 읽기 1회로 줄었다.

### fill_slice 단일-워크 + ore y-가드 + 미세화

- fill_slice: 점당 interp 자식 5개를 각각 프리픽스 워크하던 것을 union
  콘 1워크 + scratch 다중-루트 회수로 (hc_df.h:99 의 의도된 사용법).
- ore vein y-가드: 두 VeinType 합집합 [-60,-8]∪[0,50] 밖은 toggle 부호
  무관 항상 -1 (증명 주석 ore_veins.c) — P2-0 제안 [-60,50] 의 상위집합
  (사이 갭 -7..-1 도 항상 -1).
- nc_eval 의 96B 그래프 얕은 사본은 콘 경로에서 소멸. **나머지 미세화
  2건은 보류**: computed goto 는 C11 strict(CMAKE_C_EXTENSIONS OFF)라
  불가, 노드 56B→32B 압축은 핫 콘이 14노드(L1 상주)로 줄어 캐시 논거가
  소멸 — 잔여 노이즈 CPU 는 펄린/옥타브 커널이 지배해 P2-4(SIMD) 영역.
  1.2-1.5x 추정은 프리-콘(노드 방문 ~10⁸/청크) 기준이었다.

## 2. 게이트

| 게이트 | 결과 |
|---|---|
| ctest (build, -O2 debug) | 30/30 PASS |
| `scripts/parity_gate.sh` (build / bench-o2 / bench-o3) | 3/3 PASS, canonical a5963205…3c24 (P2-0 동일) |
| `scripts/check_no_fma.sh` (build / bench-o2 / bench-o3) | PASS (67,624 / 67,400 / 82,751 insn) |
| `scripts/check_sanitizers.sh` (ASan+UBSan 풀 스위트) | **PASS** (structures UB 수정 후; 그전엔 사전 존재 UB 로 항상 FAIL) |
| 벤치 런 자체 해시 판정 (3런 × 2구성) | 6/6 PASS |

## 3. 전/후 실측 (-O3, 20스레드, 3런 중앙값, seed 1234567890 r.0.0)

| 지표 | P2-0 | P2-1 | 배율 |
|---|---|---|---|
| gen wall | 198.0s | **9.380s** (편차 3.5%) | **21.1x** |
| 청크당 (1024) | 193ms | 9.16ms | 21.1x |
| 체인 wall | 191.3s | 3.329s | 57.5x |
| 체인 CPU-sum | 3220.5s | 60.70s | 53.1x |
| — 04 noise CPU | 3205.7s | **49.76s** | **64.4x** |
| — 07 surface CPU | 8.3s | 8.15s | 동등 (무손대 — 정합성 방증) |
| — nc_init CPU | 4.9s | 1.97s | 2.5x (flat 테이블/psl 콘) |
| — 08 carvers CPU | 1.7s | 0.80s | 2.1x (barrier/psl 콘) |
| 09 features wall | 3.75s | 3.61s | 동등 (무손대) |
| light09 wall | 2.11s | 1.97s | 동등 |

-O2: gen wall 218.2s → 9.654s (**22.6x**), noise CPU 3227.9s → 50.87s
(63.5x). O2/O3 gen wall 이 이제 3% 이내 — 잔여도 의존-체인 바운드라는
P2-0 §3.3 판단 유지. (참고: P2-0 의 "light09 O3 1.6x" 는 이번 O2 런에서
재현 안 됨 — O2 1.98s ≈ O3 1.97s. 라이트 코드 무손대이므로 당시 VM
조건 차이로 보고, O-레벨 판정엔 영향 없음.)

원자료 (로컬-only, gitignore): `bench/results/20260806T113551Z-bench-o3-t20.jsonl`,
`…T113702Z-bench-o2-t20.jsonl`.

## 4. 갱신된 스테이지 비중 (-O3 중앙값)

gen wall 9.38s 기준:

| 페이즈 | wall | 비중 |
|---|---|---|
| 09 features (직렬) | 3.61s | **38.0%** |
| 체인(04+07+08, 20스레드) | 3.33s | **35.0%** |
| light09 배치 (직렬) | 1.97s | **20.7%** |
| serialize | 0.30s | 3.1% |
| sha256 | 0.14s | 1.4% |
| 나머지 (light08/pp/…) | ~0.17s | ~1.8% |

체인 내부 CPU: noise 82.0% / surface 13.4% / nc_init 3.2% / carvers 1.3%.

## 5. P2-3~5 순서 재확인 (본 태스크의 판단 деliverable)

ADR-004 예측(재작성 후 noise 21% / features 41%)과 실측(체인 35% /
features 38%)이 대체로 합치. 판단:

1. **P2-3 (FREE 스케줄러: features 웨이브프런트 + serialize 병렬화)
   최우선 유지·강화** — features 는 이제 최대 단일 버킷(38.0%)이고
   직렬. light 타임라인은 병렬화 불가(제품 시맨틱)이므로 병렬화 대상은
   features+serialize ≈ 41.1%.
2. **P2-5 의 light09 항목(BFS slot 합치기/마스크 증분, 추정 1.5-2.5x)
   비중 상승** — light09 가 20.7% 로 3위이고 P2-3 이후에도 직렬로
   남는다. P2-5 를 light09 중심으로 P2-4 보다 앞당기는 것을 권고.
3. **P2-4 (AVX2 SoA) 순위 하락** — 잔여 noise 는 wall 로 ~2.7s(체인의
   82%). 2-4x 커널 이득의 wall 기여는 ~14-20% 로 여전히 유의하나
   1-2위가 아니다. P2-2 (FTS y-불변/cache_2d — psl 콘 51노드의 사다리,
   surface 의 psl 소비, nc_init) 결과를 본 뒤 범위 확정이 경제적.
4. P2-2 는 계획 순서대로 다음 태스크로 타당 — noise 잔여 CPU 와 surface
   13.4% 양쪽을 건드리는 low-risk 구조 수정.

## 6. 커밋

- 55150ab perf(df): 라이브-콘 평가 + fill_slice 단일-워크 + ore y-가드
- 936b02d fix(structures): lcg_large_feature_seed signed 곱 UB (사전 존재)
- (본 노트 커밋)
