# VIZ-2: 타임라인 이벤트 재정의 — serialize 몰림 왜곡 수정 + 무대 통일

2026-08-13. 커밋: 83135b1(계측) · 3672c8d(VIZ-1 트리 반입) · 84f3ad5(변환기) ·
70ba512(데모+캡션) · 880ed33(캡처 절차). 로컬만, push 안 함.

## 결론 요약

- serialize 몰림(마지막 37ms = wall 1.7%, GIF 1프레임 미만)은 **해소** —
  새 기본 이벤트 `complete` 는 hc-e6 실측에서 wall 의 69~90% 구간에 걸친
  ~190ms(≈5프레임) 점진 리빌.
- 단, **p50 30~70% 성공 기준은 불달성** (complete p50 = 84.9%). §1 로
  되돌아가 재확정한 결과 이벤트 선택의 문제가 아니라 **FREE 페이즈 구조의
  사실**이다 (아래 §구조적 한계). 30~70% 에 드는 유일한 이벤트는
  chain(32.7%)인데, 이는 데코 전 지형만 최종이라 리빌 픽셀(최종 콘텐츠)을
  선반영하므로 데모 기본으로 기각.
- 무대 통일 완료: 데모 hyperchunk 패널 = hc-e6 실측 (claw 리스케일 폐기,
  파일은 대조용 유지).

## 1. 이벤트 정의 (소스 확정, 적대 검증 통과)

정의: **청크 i 의 블록 내용이 더 이상 변경되지 않는 시점** =
`max(자기 chain w5, ±1 창 데코 E.t1 [, ±1 창 pp P.t1])`.

근거 (전 블록-쓰기 경로 전수 + 반례 탐색 실패):

- 체인(노이즈/서피스/카버스)은 자기 청크만 쓴다 — surface.c:79-87,
  carvers.c:123-129 (setter 가 단일 chunk 포인터), gen_noise_stage.c:106.
  → 체인 항 = 자기 C.w5.
- 데코 쓰기 창은 **소스 강제 ±1**: hc_features.h:67
  (blockStateWriteRadius=1, ensureCanWrite soft-fail), 클립 지점
  features.c:143-146 / 181-184 (set_block/_ks) / 738-748 (ore bulk 우회도
  동일 클립). center 는 데코 이벤트의 자기 셀 (gen_features_stage.c:32-33).
  구조물 피스·struct_step 도 같은 setter 경유 (structures_mineshaft.c:719,
  1168; structures_template.c:447-448) — ±1 창 탈출 불가.
  D 라인(±2 박스+피스 가상 셀)은 **충돌 신고 집합**(hc_sched.h:12-16,
  test_free_region.c:7-15 — 블록 ±1 + 라이트 헤일로 ±1 + BE 래치)이지 쓰기
  집합이 아니다 — D 로 계산하면 거리-2 읽기 이웃을 과대 귀속.
- 라이트 이벤트(EV_L08/L09/PREPARE)는 states[] 를 읽기만 한다
  (light_engine.c:135, 370, 416, 614 전수 — 전부 read).
- **pp 는 블록을 쓴다**: hc_postprocess_chunk = 바닐라
  LevelChunk.postProcessGeneration 등가 (hc_postprocess.h 헤더),
  updateFromNeighbourShapes 폴드가 다르면 setBlock(276)
  (postprocess.c:1234-1235), 쓰기 창은 드레인 청크 ±1
  (postprocess.c:803-804 die). 유체는 배치가 아니라 스케줄
  (rg->ticks, t=지연값). pp 집합(golden postprocess.manifest, 1165청크
  [-2,32])은 **전 1024 타깃을 덮는다** — FREE 는 행우선으로 메인 스레드
  직렬 드레인 (bench 1756-1796).
- pp1 이후 블록 변경 없음: lfinal 은 라이트 flush/prepare + memcpy 뿐,
  직렬화는 `hc_chunk_to_nbt(const hc_chunk_t*, …)` (hc_chunk_nbt.h:37).

계측: pp 만 청크별 기록이 없어 **P 레코드 추가** (83135b1) —
`P <m> <cx> <cz> <t0> <t1>`, 드레인 루프가 B_pp 귀속용으로 이미 읽는
t1/t2 재사용 (on 상태 추가 클록 읽기 0), off 상태 = 드레인당 포인터 검사
1회, 게이트 비연결, waterfall.py 는 미지 레코드 무시라 구판 분석기 호환.

변환기 이벤트 (84f3ad5): `complete`(기본) = w5 ∨ 데코±1 —
**마지막 실질 블록 쓰기**. `strict` = complete ∨ pp±1 — 비트-최종, P 필수.
pp 를 기본에서 뺀 이유: (a) pp 쓰기는 희소 형상-보정(덩굴 프룬·울타리
연결 — MOTION_BLOCKING 하이트맵 무영향 = 타일 픽셀 불변, 유체는 스케줄만),
(b) 행우선 직렬 스윕이 전 타깃을 덮어 **리빌이 pp 창(hc-e6 25ms, GIF
1프레임 미만)으로 붕괴** — 이 태스크가 고치려는 왜곡의 재생산.
`serialize`/`deco`/`chain` 은 --event 로 유지.

## 2. 전/후 분포 대조 (min/p25/p50/p90/max, t0=setup_end)

**전 (VIZ-1 데모: claw 무대, event=serialize, wall 2201.1ms):**
2159.7 / 2168.4 / 2177.9 / 2192.7 / 2196.7 — p50 = wall 의 98.9%,
스팬 98~100% (37ms).

**후 (VIZ-2 데모: hc-e6/zen5, cap-3, event=complete, wall 912.2ms):**
633.8 / 722.5 / 774.6 / 816.5 / 822.7 — p50 = wall 의 84.9%,
스팬 69~90% (189ms). 3캡처 전부 p50 84.9~85.0% (극안정).

hc-e6 cap-3 이벤트별 (p50/wall · 스팬):

| 이벤트 | p50/wall | 스팬 | 비고 |
|---|---|---|---|
| chain | 32.9% | 9~56% | 유일한 30~70% 충족 — 지형만 최종 (데코 선반영이라 기각) |
| **complete** | **84.9%** | **69~90%** | 데모 채택 |
| strict | 95.7% | 94~96% | pp 스윕에 붕괴 — 감사용 |
| serialize | 98.2% | 97~99% | 기존 왜곡 |

리빌 형상: complete 는 FREE 데코 셀-FIFO 의 σ* 5-스트라이드 버킷이 그대로
보이는 **세로 스트라이프 웨이브** — 실측 스케줄 구조라 그대로 둠 (오히려
차별점).

### 구조적 한계 (§1 재확정 결과)

FREE 는 페이즈-배리어 구조다: chain 0~63% → DAG(데코/라이트) 63~92% →
pp → lfinal → serialize (hc-e6 마크 실측; claw 도 비율 유사 0~69/70~93%).
**모든 데코-포함 완료 이벤트는 정의상 dag0(=wall 63%) 이후에만 존재**하므로
p50 30~70% 는 "블록 최종" 계열로는 원리적으로 불달성. 30~70% 기대는
체인·데코가 셀 단위 인터리브된다는 가정인데 현 구현은 chain_end 배리어 후
DAG 다 (마크·소스 양쪽 확정). 즉 잔여 후반-집중은 측정 아티팩트가 아니라
스케줄 실제 — serialize 의 "기록-시각" 아티팩트와 성격이 다르다.

## 3. hc-e6 재캡처 (무대: hc-e6/zen5, E6.Flex 16 OCPU Zen5)

- FREE 20T, seed 1234567890, r.0.0 1024청크, bench-o2, 3런 전부
  canonical PASS (own-v1 2eb7485b…), exit 0.
- gen_wall: 900.7 / 888.3 / 889.4ms — 중앙값 889.4, **B-6 대역
  (887.8~900.5, 공개 0.894s) 정합**. 어긋남 없음.
- 타임라인 wall(proc_end−setup_end) = 912.2ms = gen_wall + 인터리브된
  replay-input 파스 ~2.5% (하네스 오버헤드; 공개 수치는 3-way 대칭으로
  제외). 데모는 normalize_wall_s 0.894 로 공개 규약에 핀 (리스케일 0.980,
  meta.time_scaled_from_wall_s 기록).
- 채택 런 = cap-3 (중앙값이자 공개 수치 최근접).
- **계측 off 무회귀**: before(4cb17e9)/after(83135b1) 같은 날 인터리브
  8런 페어 — 중앙값 889.4 vs 893.5 (+0.46%), 바이너리 내 런간 스프레드
  ~20ms 대역 내. ON 3런 중앙값(889.4)이 OFF-after(893.5)보다 낮음 —
  계측 비용은 소음 아래.
- raw: `/mnt/scratch/bench/viz2/2026-08-13/` (tl-free-hce6-{1,2,3}.txt,
  cap/off-*.json). 원격 /tmp/viz2 는 janitor 위임, 인스턴스는 idle
  watchdog 위임 (down 안 함).
- 게이트: ctest 37/37 PASS (build-release; df_x8 스킵은 평소대로 AVX-512
  부재) + parity_gate.sh PASS (canonical-payload a5963205…).

## 4. 데모 재렌더 (70ba512)

- `race-b6.yaml`: hyperchunk 패널 → `timelines/hyperchunk-free-hce6.json`
  (claw 파일은 유지). 엔드카드 13.3× 불변.
- 합성 라벨 정직성: 하단 빈 밴드(y≈572, v5 요소 최하단 progress bar
  y567 아래)에 마이크로 캡션 1줄
  `vanilla · c2me: synthetic chunk timing · measured walls` —
  meta.synthetic 구동 (실측 교체 시 자동 소멸), body 13px subink, 테마 키
  `caption{size,bottom}`. **v5 스펙 픽셀(헤드라인/라벨/타이머/트랙) 좌표·
  문자열 전부 불변.**
- 산출물 (gitignore, 재생성 ~10초):
  `tools/viz/demo/out/race-b6.gif` (456KB, 375프레임) / `race-b6.mp4` /
  `still-t{0.89,3.7,11.9}.png`.

## 5. 바닐라/C2ME 실측 캡처 절차 (880ed33)

`tools/viz/capture/vanilla-c2me-probe.md` 로 확정 — B-6 b6_run.sh 프로브
(이미 청크별 if-loaded say)를 POLL_S 0.1 고빈도화 + 청크별 TSV 초안.
FULL-승격 시맨틱 (바닐라는 실측도 끝 계단일 것 — 합성 scan 서사와 다를
수 있음, 캡처 후 대조 필요), 단방향 과대오차 ≤POLL_S+틱50ms,
관측자-부하 검증 게이트(3s vs 0.1s 인터리브 페어), meta.probe_interval_ms
표기, 캡션 probe-꼬리 일반화 방침.

## 6. 기각/보류

- **기각: strict 를 데모 기본으로** — pp 행우선 스윕이 전 타깃을 덮어 리빌이
  25ms 창으로 붕괴 (serialize 왜곡 재생산). 감사용 --event 로만.
- **기각: chain 을 데모 기본으로** — 30~70% 기준은 유일하게 충족하지만
  리빌 픽셀(트리 포함 최종 타일)이 데코 전에 등장 = 콘텐츠 선반영.
- **기각: on_block_write 훅 기반 '실제 마지막 쓰기'** — 쓰기마다 클록 읽기로
  ON 상태 타임라인 자체를 오염 + ore 는 훅 우회 (hc_features.h:141-145).
  이벤트-종료 보수 상한(E.t1)으로 충분.
- **보류(오너 승인 필요, 제안만): 2-스테이지 리빌** — 청크를 chain 시점에
  지형 톤으로 옅게, complete 시점에 최종 픽셀로. 오너 기대(완만한 S커브,
  9~56% 구간)와 블록-최종 정직성을 동시에 만족하는 유일한 길이지만 v5 픽셀
  스펙 변경이라 여기서 안 함.
- **보류: 합성 패널 실측 교체** — §5 절차 확정, 실행은 후속 태스크.
- 주의: 합성 scan/wave 는 균일 분포라 바닐라/C2ME 에 **유리한** 이상화
  (바닐라 실측 full-승격은 끝 계단). 현 대비 불공정 방향은 우리에게 불리
  = 보수적이므로 공개 안전.

## 함정 (다음 세션용)

- VIZ-1 트리가 언커밋 상태였다 — 3672c8d 로 반입하며 `.gitignore` 의
  비앵커 `tiles/` 가 커밋 대상인 `demo/tiles/` 까지 먹는 것 수정 (`/tiles/`).
- ivory.json 은 컴팩트 배열 포맷 — json.dump 재직렬화 금지 (텍스트 편집만).
- E.t1(데코)은 이벤트 후미 라이트 flush 포함 — t_done 은 보수 상한
  (증명-최종 시점이지 마지막 관측 쓰기가 아님).
- 타임라인 wall ≠ gen_wall (replay-load ~2.5% 포함) — 공개 수치와 섞을 땐
  normalize_wall_s 로 핀.
