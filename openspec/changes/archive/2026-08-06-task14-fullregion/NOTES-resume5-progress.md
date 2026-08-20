# Task 14 RESUME-5 진행 노트 (2026-08-05, 커밋 d8d8e13..HEAD)

측정 궤적 (HC_SURVEY 풀 리전): 블록 잔차 8,934셀 → **131셀/53청크**,
페이로드 불일치 455 → **211청크**. 커밋 로그가 원인·수정 체인의 1차 기록.

## 세션 종료 상태 (RESUME-6 시작점)
- ctest 29/29 green (full_region 제외) + check_no_fma PASS (64,105
  insn) — 코어 변경 (beard/noise/postprocess/structures/blocks/BE)
  전부 기존 게이트 무회귀.
- full_region strict 미통과 — 지배 잔차는 §라이트 (아래 1번). 원인은
  판정 완료, 구현이 남음 (995a5f1 메시지 + light-savetime 메모리).
- cli/hyperchunk-verify + scripts/parity_gate.sh 동작 (판정은 현재
  잔차만큼 정직하게 FAIL).
- BE 클래스 닫힘: monster_room chest/spawner 기록 + vault config
  loot_table 기본값 생략 → 페이로드 239→211.

## 닫힌 클래스 (실측 근거는 각 커밋 메시지)
- Beardifier 부재 (지배 클래스): beard.c 포트 + BeardProbe 29,375포인트
  비트일치. (13,35) 실측 starts 로드.
- fluid_ticks 3,013 누락: 드레인 updateShape 수생 물 틱 + (32,17)
  shipwreck 이웃 스타트 + 스프레드 flag-3 경유.
- 미등재 상태: StatePropsProbe 실측 → R-blockprops4.tsv (누적 261종,
  테이블 914). 교차검증: 기존 수기 TSV 8행 0 diff.
- 마진 링 바이옴: extract_margin_biomes.py (657청크 오버레이).
- c.32.17 PPG 마크.

## 최종 페이로드 불일치 성분 (211청크, 세션 말 실측)
- SkyLight 단독 145 (예 c.7.0) + 라이트 포함 복합 ~15 — §1 라이트 모델.
- 블록/하이트맵 복합 ~40 (마인샤프트 국경/식생/vines — §4).
- fluid_ticks#len 4 (c.19.0 등 — ours-only 7틱, §6).
- BlockLight 섹션 존재 ± 7 — 라이트 모델과 동근원 추정.

## 남은 클래스 (배치 순서 권장; 2·3번 BE 클래스는 닫힘)
1. **SkyLight 144 + 복합 ~17청크 (18,237셀, 델타 전부 음수)** — 원인
   판정 완료: 바닐라 저장 라이트 = 각 청크 09 배치 시점의 lfp (이후
   데코 미재조명; c.2.26 (47,y,417) 실측 — 나중 정글 캐노피 y95-97
   아래가 골든 15). test_full_region 의 "최종 블록 고정점" 모델을
   task13 배치 모델 (test_light_stages.c ltask_t: batch/P(C)/R/S) 로
   교체해야 함. 라이트 지역성: 광원 도달 ≤15블록 → 청크 C 의 lfp 는
   C±1 (3×3) 로컬 월드로 정확 (인접 리전만 필요, 증명 스케치는 경로
   길이 논증). 포스트프로세스 라이브 쓰기의 증분 재조명은 2차 이슈
   (드문 라이트-관련 쓰기; 첫 라운드는 무시하고 재측정 권장).
2. **BE 카운트 16청크 (ours 0 vs golden 2-4)** — 던전(monster_room)류
   피처-배치 BE 미기록 추정. (17,12)/(18,12) 는 ±1 개.
3. **BE config.loot_table 16청크 (전부 trial 영역)** — trial spawner/
   vault BE 의 config.loot_table 필드 직렬화 차이. 표본 덤프로 판정.
4. **블록 131셀**: (a) 마인샤프트 국경 청크 (c.31.1 등: 펜스 연결/
   지지 로그 vs cave_air/wall_torch 누락) — 조립은 3 스타트 전부 골든
   일치 (122/160/134 피스 0 diff, /tmp/ms_dump 하니스), 배치 스트림
   발산. 다중-스타트 공유 스트림 구간 의심. 계측 로그로 첫 발산 셀
   찾기. (b) 초지 패치 단방향 초과 (c.7.30 등), (c) cave_vines 소실
   (c.2.1, 마크 불일치 c.2.6/2.7 과 인접).
5. **PPG 마크 4청크**: c.2.6(9v7)/c.2.7(10v9)/c.5.9(54v53)/c.16.11(5v5
   순서만).
6. **ours-only fluid_ticks 7**: 스프레드가 tall_seagrass/air 로 스케줄
   — can_hold_any_fluid 가 LiquidBlockContainer canPlaceLiquid=false
   (kelp/seagrass/tall_seagrass) 를 제외하는지 점검.

## 인프라 (재사용)
- /tmp/ms_dump.c: mineshaft 조립 덤프 하니스 (libhyperchunk.a 링크).
- 이웃 스타트 추출: tools/golden/extract_neighbor_start.py (coherence
  가드). 마진: extract_margin_biomes.py.
- 상태 실측: StatePropsProbe.java (families 폐포는 blocks.c NAMES 대비
  차집합 생성 후 일괄).
- 잔차 분류: 이 노트와 같은 디렉터리의 커밋 로그 + NBT 경로 분류
  스니펫 (대화 로그; semantic_compare.py 의 walk 변형).

## 미포함 판정 (근거)
- trial (40,4): 피스 x≥560 (데코 창 x≤559 밖), 지하 밀도만 — 관측 불가.
- jungle_pyramid (5,42)/ocean_ruin(9,-10)/shipwreck(10,-12)/
  buried(14,-10)/(36,13): 데코 창 밖. buried(29,32): 배치되나 체스트가
  경계에서 9블록 (엣지/라이트 도달 없음, 마크 일치 실측).
