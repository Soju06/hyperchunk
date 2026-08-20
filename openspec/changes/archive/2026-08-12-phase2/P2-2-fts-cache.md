# P2-2 — FTS y-불변 분할 + slice 콘 컷/분할 완료 노트 (2026-08-06)

P2-0 §8 P2-2 행: FTS y-불변 분할, cache_2d 실메모화, psl 메모 정리,
beard 박스 프리필터. 앞의 것은 구현, 뒤의 셋은 **실측 후 "안 함" 판정**
(§4 — P2-1 보류 기록 스타일). 전부 값-불변 최적화 — 매 단계 게이트
green, canonical 해시는 P2-0 과 동일 (a5963205…3c24). **절대치는 이
VM(claw) 참고치, 유효한 것은 비중·배율** (P2-0 §0 동일 주의).

## TL;DR

- **노이즈 스테이지 CPU-sum 51.92s → 41.42s (1.254x), nc_init 1.363x,
  체인 wall 1.234x, gen wall 9.468s → 8.984s (1.054x)** (-O3, 20스레드,
  3런 중앙값, 정렬-고정 빌드 쌍 §5, 전 런 canonical PASS). wall 배율이
  작은 것은 체인 비중이 이미 31%대로 내려와 있기 때문 — 남은 지형은 §7.
- 신규 게이트: **df_cones** (콘 기계 == 프리픽스 워크 비트-등가, 6,226
  검사) 추가 + **mutation-probe 6종** (커밋 후) — 실변조 5종 전부 게이트
  ≥1개 검출, 게이트 간 상보성 실증 (§6. 특히 P4: 등가-비교 게이트 단독
  으로는 못 잡고 동결 골든이 잡는 클래스 확인).
- 부수 사건: df 오브젝트 크기 변화가 **링크 레이아웃 룰렛**으로 무손대
  직렬 페이즈(-O3 light09 +61%)를 튀게 했다 — `-falign-functions=64`
  전 빌드 고정으로 해소 (§5, 커밋 985b7b0). 이후 Phase 2 태스크의 wall
  회귀 판독 시 "오브젝트 디스어셈 diff 먼저" 를 교훈으로 남긴다.

## 1. 구현 (커밋 fd3007e)

### y-분산 분류기 (`hc_df_mark_y_variant`, df_eval.c / 규칙 SoT hc_df.h)

- 무조건 y-가변: `Y`, `Y_CLAMPED_GRADIENT`, `BLENDED_NOISE`, `FTS`,
  **맨 `NOISE`** — y_scale==0 이어도 y\*0.0 = ±0.0 의 부호가 y 를 따라
  노이즈 인자로 들어가므로 비트-불변을 증명할 수 없다 (보수 분류).
- `SHIFTED_NOISE` 는 y_scale==0 && shift_y ≡ CONST(+0.0, 비트 0) && x/z
  시프트가 y-불변일 때만 불변: y\*±0.0 ∈ {+0.0, −0.0} 에 +0.0 을 더하면
  IEEE-754 round-to-nearest 에서 항상 +0.0 — y 인자가 상수로 접힌다.
  26.2 오버월드의 continents/erosion/ridges/temperature/vegetation 이
  전부 이 꼴이라 2D 스택이 불변으로 떨어진다.
- 마커는 pass-through 로 자식 전파 (창-안 테이블 히트는 y-불변이지만
  폴백 기준의 보수 분류가 cc==NULL 문맥 포함 전 경로에서 안전).
  스플라인은 coord/중첩 val 전파, interval_select 는 input+전 함수 전파.
- 전파가 단조(가변 피연산자를 읽으면 가변)라 **불변부 선평가가 위상
  순서를 보존**한다 — 분할의 값-불변 근거.

### FTS ipool 콘 분할 (df_compile.c build_cone + df_eval.c FTS 케이스)

- ipool 레이아웃을 `[nI][y-불변 nI개][y-가변]` (각각 오름차순) 으로 변경.
  사다리는 진입 시 불변부 1회 + y 마다 가변부만 재평가.
- 실측 (26.2 오버월드, n=262): FTS 콘 59 → **불변 43 / 가변 16**.
  가변 16개는 사다리 스파인의 ycg+산술 (전부 노이즈 없음) — offset/factor
  클로저(shift_a/b, shifted_noise×3, 스플라인)가 통째로 컬럼당 1회로
  승격됐다. P2-0 §6.1 의 "FTS 탐색 ~10-25x" 는 콘 노드 기준으로 달성
  (스텝당 노이즈 평가 0), wall 기여는 psl 버킷 자체가 ~3%라 제한적.

### WINDOW_SAFE 컷 + fill_slice 분할 (noise_chunk.c)

- `HC_DF_CONE_WINDOW_SAFE`: 전 평가점이 쿼트 창 안 보장인 문맥 전용
  flat_cache 자식 컷 (창 안 = 항상 테이블 히트 → 자식 값은 죽은 값).
  적용 지점 2곳: fill_slice 포인트 (기존 증명 주석), flat 테이블 구축
  포인트 (창 격자 자신). psl/erosion/depth SP 콘은 **무변경** — aquifer
  의 청크-밖 컬럼 조회 (SURF_OFFS x −3..+1 청크) 가 창-밖 폴백 실경로다.
- slice 유니온 콘 실측: 204 → 컷 후 152, y-분할 **불변 10 / 가변 142**.
  fill_slice 는 컬럼당 불변 10 + 포인트당 142 (전: 포인트당 204 — 2D
  스택 49×25 죽은 평가가 사라짐 = P2-0 의 "컬럼 2D 스택 49x→1x" 실체,
  실제로는 flat 테이블이 흡수해 0x). flat 구축 콘 합계 72 노드.
- 폴백 안전장치는 P2-1 의 eval mask assert 그대로 — WINDOW_SAFE 가
  잘못 적용되면 debug 에서 즉시 발화 (프로브 P5 로 검출 확인).

## 2. 전/후 실측 (-O3, 20스레드, 3런 중앙값, seed 1234567890 r.0.0)

정렬-고정 빌드 쌍 (§5): 전 = cdb1484 + `-falign-functions=64`, 후 =
985b7b0 (동일 플래그). 같은 세션에서 측정.

| 지표 | 전 | 후 | 배율 |
|---|---|---|---|
| gen wall | 9467.9 ms (편차 0.9%) | **8984.2 ms** (편차 2.4%) | **1.054x** |
| 체인 wall | 3482.7 ms | 2822.0 ms | **1.234x** |
| 체인 CPU-sum | 63.40 s | 52.35 s | 1.211x |
| — 04 noise CPU | 51.92 s | **41.42 s** | **1.254x** |
| — 07 surface CPU | 8.589 s | 8.589 s | 동등 (무손대 방증) |
| — nc_init CPU | 2.038 s | 1.495 s | 1.363x (flat 구축 콘 컷) |
| — 08 carvers CPU | 0.817 s | 0.821 s | 동등 |
| — beard CPU | 31.2 ms | 31.3 ms | 동등 (무손대) |
| light09 wall | 1914.8 ms | 1970.5 ms | 동등 (±3%, 편차 내) |
| features wall | 3463.4 ms | 3589.8 ms | 동등 (±3.6%, 편차 내) |

-O2 (후, 정렬): gen wall 8853.9 ms (편차 19.4% — 1런 steal 오염,
min 8822.9), noise CPU 41.58 s ≈ O3 와 동등 — "잔여도 의존-체인 바운드"
(P2-0 §3.3) 유지.

단일-스레드 perf 귀속 (전→후, 총 cycles 대비): fill_slice 39.8%→27.6%,
select_cell_yz(final_density CELL — 무손대) 22.5%→25.1%, psl(초기화+
런타임) ~3.2%→~3.4% (상대 상승, 절대 감소), aquifer SP ~3.5%.

원자료 (로컬-only, gitignore): `bench/results/20260806T132202Z-…-p22-
before-align.jsonl`, `…T132250Z-…-p22-after-align.jsonl`, `…T132328Z-
bench-o2-…-p22-after-align.jsonl` (+ 정렬 전 참고런 p22-before/mid/after).

## 3. 게이트

| 게이트 | 결과 |
|---|---|
| ctest (build, -O2 debug) | **31/31** PASS (df_cones 신규 포함) |
| `scripts/parity_gate.sh` (build / bench-o2 / bench-o3) | 3/3 PASS, canonical a5963205…3c24 (P2-0 동일) |
| `scripts/check_no_fma.sh` (3빌드) | PASS (69,688 / 69,476 / 84,951 insn) |
| `scripts/check_sanitizers.sh` (ASan+UBSan 풀 스위트, 정렬 반영 후 재실행) | PASS |
| 벤치 런 자체 해시 판정 (정렬쌍 3+3+3 + 참고런) | 전부 PASS |

### 신규 게이트: df_cones (커밋 e335ea3, ctest #18)

콘/컷/분할 기계의 계약 "콘 평가 == 프리픽스 [0..root] 워크 비트 동일"
을 직접 게이트한다. 슬롯×모드×좌표 배터리: 창 안/경계/밖 (−3..+1 청크
— aquifer 실경로), 음수 좌표 청크 (−3,−5), y 8종, A-B-A 재방문 (상태
누출 클래스), fill_slice 전 슬라이스 (5컬럼×49y×interp 8), CELL 5셀×3점,
BLOCK 점진 상태. 6,226 검사, 배터리 크기 동결 (false-PASS 방어).
기존 골든과의 역할 분담: router_slots = cc==NULL 순수 그래프의 바닐라
동결값, noise_stage = 게이트 청크의 스테이지 출력, df_cones = 그 사이의
콘 경로 자체 (골든이 못 보는 창-밖·음수·재방문 입력 공간 포함).

## 4. "안 함" 판정 3건 (실측 근거)

1. **cache_2d 실메모화 (바닐라 Cache2D 동형)** — 안 함.
   - 컷 이후 cache_2d 가 실제로 반복 평가되는 핫 콘이 없다: slice 콘의
     cache_2d 5개 {n2,n6,n34,n44,n56} 는 전부 flat_cache 하위라
     WINDOW_SAFE 컷에 흡수됐고, CELL/BLOCK 콘에는 원래 0개 (실측).
   - psl 신규 컬럼당 2D 클로저 2회 평가 (psl 콘 sc1 + FTS 불변부 sc2)
     는 **바닐라도 2회다** — upper_bound 의 Cache2D 인스턴스(n234/n231)
     와 density 사다리의 인스턴스(n216/n218)가 별개라 각각 1미스.
     동형이므로 제거 대상이 아니다.
   - 바닐라 대비 유일한 초과 작업: aquifer deep-dark 판정의 erosion→
     depth 연속 평가에서 shift 스택(n2/n6) 재평가 — 단, depth 평가는
     erosion < −0.225 단락을 통과한 희귀 경로에서만 발생하고 규모가
     ~15 펄린 샘플/회. eager 콘 워크에서 메모 히트로 자식 평가를
     생략하려면 배타-도달 skip-set + per-nc 상태 + 서브콘 기계가 필요
     (공유 서브트리는 빼야 값-불변) — 이득 ≤~1% 에 리스크 불균형.
2. **psl in-window 변형 콘** ("psl 메모 정리" 잔여) — 안 함. psl 조회
   컬럼은 aquifer 121-프리로드 (±1청크) + SURF_OFFS (−3..+1청크) 인데
   flat 창은 자기 청크 5×5 쿼트뿐 — in-window 비율이 낮아 (121 중 25)
   추정 이득 ~0.1%. psl 해시 메모 자체 (Long2IntMap 동형) 는 건재.
   사다리 낭비 (P2-1 이관분) 는 FTS 분할로 해소.
3. **beard 조각별 박스 프리필터** — 안 함. 실측 beard CPU-sum 25-31 ms
   = 체인 CPU 의 0.04-0.05% (전/후 동일). union-AABB 프리필터 (기존,
   beard.c) 가 이미 비구조물 청크를 0-비용으로 거르고, 조각별 컷의
   대상인 "affected 박스 안 다조각 순회" 는 측정 한계 이하다. 참고로
   noise_stage/surface 등 스테이지 골든에는 beard 배선이 없어 (beard
   ==NULL 경로) 이 항목의 판정 게이트는 beard_math + full_region +
   parity_gate 였을 것 — 미구현이므로 해당 없음.

## 5. 부수 사건: 링크 레이아웃 룰렛 (커밋 985b7b0)

- 증상: P2-2 df 변경 후 -O3 벤치에서 **무손대** 직렬 페이즈가 일관 팽창
  (light09 1.85→2.9-3.1s +61%, features 3.4→4.0-4.3s; 3런×2회 + 교차
  실행으로 신구 바이너리 귀속 확정).
- 원인 격리: light_engine/features/gen_light_stages/gen_features_stage/
  biome_zoom 오브젝트의 디스어셈블리가 전후 **바이트 동일** — df
  오브젝트 크기 변화가 링크 배치를 밀어낸 순수 레이아웃 효과.
- 해소: `-falign-functions=64` 전 빌드 고정 → light09 1.88-1.96s,
  features 3.4-3.5s 복원 + before 런 편차도 0.9% 로 안정화. FP 시맨틱
  무관 (전 게이트 재판정 PASS). **전/후 wall 비교는 반드시 동일 정렬
  정책의 빌드 쌍으로** — 본 노트 §2 가 그 쌍이다.
- 교훈: 이 VM 에서 wall 회귀가 보이면 (1) 오브젝트 디스어셈 diff 로
  코드 동일성 먼저 확인, (2) CPU-sum (작업량) 과 wall (레이아웃+steal
  민감) 을 분리 판독.

## 6. mutation-probe (커밋 e335ea3 상태, main tree, 순차, build -O2 debug)

판정 게이트 3종: df_cones / router_slots / noise_stage. 변조 → 재빌드 →
게이트 → `git checkout` 복원.

| # | 변조 (전부 df_eval.c/noise_chunk.c 1개소) | df_cones | router_slots | noise_stage | 판정 |
|---|---|---|---|---|---|
| P1 | 분류기: YCG 를 항상-가변에서 제거 (불변 오분류) | **FAIL** | PASS | **FAIL** | 검출 |
| P2 | 분류기: 맨 NOISE 를 항상-가변에서 제거 | **FAIL** | PASS | **FAIL** | 검출 |
| P3 | SHIFTED_NOISE 불변 조건 (y_scale/shift_y) 제거 | PASS | PASS | PASS | **등가-변이** |
| P4 | FTS 불변 프리패스 생략 (가변부가 스테일 sc2 읽음) | PASS | **FAIL** | **FAIL** | 검출 |
| P5 | WINDOW_SAFE 를 psl 등 SP 콘에 오적용 | **FAIL** | PASS | **FAIL** | 검출 |
| P6 | fill_slice 불변 호이스트를 컬럼(j) 루프 밖으로 | **FAIL** | PASS | **FAIL** | 검출 |

해석:
- 실변조 5종 전부 게이트 ≥1개 검출. **상보성이 실증됐다**:
  - **P4 에서 df_cones 가 맹**: 등가-비교 게이트는 참조 경로(프리픽스
    워크)도 같은 FTS 코드를 지나므로 양쪽 sc2 가 같은 방식으로 오염돼
    일치한다. **동결 골든 (router_slots) 이 이 클래스를 잡는다** —
    등가 게이트와 절대값 골든을 함께 두는 이유.
  - P1/P2 에서 router_slots 가 맹: 40개 psl 골든 벡터가 이 변조에
    결과-불변 (density 를 사다리 시작 y 로 동결해도 40 벡터의 FTS
    결과가 우연히 같음 — 첫 스텝 결정 케이스로 추정, 미확인). 콘
    경로를 직접 보는 df_cones / 스테이지를 보는 noise_stage 가 잡는다.
  - P5 는 정확히 noise-stage-gate-coverage 메모의 "flat_cache 창-밖
    폴백" 경계 클래스 — debug assert (P2-1 장치) + df_cones 창-밖
    배터리가 잡았고, cc 없는 router_slots 는 정의상 못 본다.
- P3 등가-변이: 26.2 오버월드 라우터의 shifted_noise 전 인스턴스가
  y_scale=0 && shift_y=리터럴 0.0 이라 조건 약화가 관측 불가. 조건은
  데이터팩 일반화 (Task 12) 방어용으로 유지 — 이 게이트 세트로는 맹
  이라는 사실을 기록한다.

## 7. 갱신된 스테이지 비중 + P2-3~5 순서 재확인

gen wall 8984 ms 기준 (-O3 중앙값):

| 페이즈 | wall | 비중 |
|---|---|---|
| 09 features (직렬) | 3590 ms | **40.0%** |
| 체인(04+07+08, 20스레드) | 2822 ms | **31.4%** |
| light09 배치 (직렬) | 1971 ms | **21.9%** |
| serialize | 300 ms | 3.3% |
| sha256 | 129 ms | 1.4% |

체인 내부 CPU: noise 79.1% / surface 16.4% / nc_init 2.9% / carvers 1.6%.

판단 (P2-1 §5 유지 + 강화):
1. **P2-3 (FREE 스케줄러) 최우선 불변** — features+serialize ≈ 43.3%
   가 직렬. 체인 개선이 진행될수록 이 비중은 커진다.
2. **P2-5 의 light09 항목 2순위 유지** — 21.9%, P2-3 후에도 직렬.
3. **P2-4 (AVX2 SoA) 3순위 유지, 대상 구체화**: 잔여 noise CPU 41.4s
   의 단일-스레드 귀속은 fill_slice 27.6% + select_cell_yz 25.1% —
   둘 다 SoA 배치화의 정석 대상 (셀/컬럼 레인화). 신규 관찰: 케이브
   스택의 `interval_select`/`range_choice` 를 우리는 eager 로 전 브랜치
   평가하는데 바닐라는 선택 브랜치만 평가한다 — **lazy 브랜치 평가는
   값-불변** (순수 노드 생략) 이고 fill_slice 가변부 142노드의 상당분이
   케이브 브랜치다. P2-4 범위 확정 전에 이 lazy 화를 P2-4 선행 항목
   으로 검토할 것을 제안 (콘을 브랜치별 세그먼트로 쪼개는 build_cone
   확장 — df_cones 게이트가 그대로 판정기).
4. P2-2 잔여 이관 없음 — cache_2d/psl/beard 는 §4 로 종결.

## 8. 커밋

- fd3007e perf(df): FTS y-불변 분할 + slice 콘 WINDOW_SAFE 컷/분할
- e335ea3 test(df): 라이브-콘 기계 등가 테스트 df_cones
- 985b7b0 build: -falign-functions=64 — 링크 레이아웃 룰렛 고정
- (본 노트 커밋)
