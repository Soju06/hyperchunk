# VIZ-4: 바닐라/C2ME 청크별 실측 캡처 (hc-e6) + 데모 실측 교체

2026-08-19. VIZ-2 §5에서 확정한 절차(`tools/viz/capture/vanilla-c2me-probe.md`) 실행.
커밋: (아래 커밋 절 참조). 로컬만, push 안 함. 공개 수치(11.9/3.7/0.894/13.3x) 불변 —
이 태스크는 청크별 형상만 추가.

## 결론 요약

- 바닐라/C2ME 데모 패널이 **hc-e6 실측 청크별 타임라인**으로 교체됨
  (`timelines/vanilla-hce6.json`, `c2me-hce6.json`). 합성 scan/wave 파일은
  대조용으로 디스크 유지 (데모 미사용).
- **실측 형상은 두 계열 모두 끝 계단(step-function)**: FULL 승격이 마지막
  폴 사이클에 몰림. 바닐라 = 11.64s까지 0 → 3사이클(1003/6/15 @ 11.640/11.753/11.863s),
  C2ME = 3.39s까지 0 → **단일 사이클에 1024 전부** (3런 전부 동일 패턴).
  합성 scan/wave의 점진 리빌 서사와 정반대 — 그리고 그것이 정직한 그림
  (sub-full 진행은 모드 훅 없이 관측 불가; VIZ-2 §5 예상 그대로, C2ME까지
  계단인 것은 예상을 넘는 실측 결과).
- 관측자-부하 게이트 **양 계열 PASS at POLL_S=0.1** (아래 표) — 백오프 불필요.
- hyperchunk 패널 픽셀 **완전 불변** (VIZ-3 산출물과 스틸 3장 픽셀-diff 0).

## 1. 게이트 표 (같은 날 인터리브, hc-e6, 2026-08-19 11:34–11:43Z)

러너: `viz_run.sh` = b6_run.sh 사본 + {POLL_S 기본 0.1, 청크별 TSV(.done_prev diff),
%.3f 정밀도, ART 경로} — diff 4항목뿐. 실행 순서 p3-1 → p01-1 → p3-2 → p01-2 → p3-3 → p01-3 (계열별).

| 계열 | POLL_S=3 (레퍼런스) | POLL_S=0.1 (캡처) | 0.1 중앙값 | B-6 대역 | 판정 |
|---|---|---|---|---|---|
| vanilla | 15.127 / 15.128 / 15.131 | 11.889 / 11.866 / 11.715 | **11.866** | 11.9×3 (0.5s-폴) | **PASS** |
| c2me | 6.075 / 9.098 / 9.091 | 3.394 / 3.686 / 3.348 | **3.394** | 3.7/3.6/3.7 (0.5s-폴) + 3.5 (0.25s-폴 감도런) | **PASS** |

- 판정 논리: 프로브 오차는 단방향 과대(≤ POLL_S + 커맨드 레이턴시)이므로 더 촘촘한
  폴은 더 **낮게** 읽어야 정상. 0.1 중앙값이 B-6 관측치 아래·(B-6 − 0.5s) 위 = 부하
  비팽창. C2ME 0.1-폴(3.35–3.69)이 B-6 자체 0.25-폴 감도런(3.5)과 정합 — B-6이 측정한
  폴-간격 효과(−0.1~−0.2s)의 연장선. 관측자 부하가 벽시계를 밀었다면 중앙값이 B-6
  위로 떴을 것.
- POLL_S=3 레퍼런스는 3s 그리드 양자화가 커서(±1사이클 = ±3s) 수치 자체가 아니라
  "함의된 진짜 벽 구간이 B-6과 겹침"으로 판독: vanilla (9.2, 12.2] ∋ 11.4–11.9 ✓,
  c2me 6.075/9.09는 진값 ~3.3이 3.03 그리드점을 스트래들한 소산 ✓.
- 사용 POLL_S = **0.1** (양 계열, 백오프 없음). `meta.probe_interval_ms = 100`.
- 무대 규율: 캠페인 창 steal **2틱** (stage-census.txt pre/post 스냅샷), 전 런
  predone 0/1024·pre-t0 디스크 census 144청크(B-6 §3 지문 동일)·full 1024/1024·
  Worker-Main 31·c2me-worker 12·c2me.toml B-6 대비 diff 0.

## 2. 선택 런 + 변환

- 게이트-통과 0.1 런 중 벽 중앙값 런: **vanilla p01-2 (t1 11.866s)**, **c2me p01-1 (t1 3.394s)**.
- 신규 `hcviz convert-probe` (chunks-TSV → timeline.json): wall_s = 그 런의 t1
  (프로브 과대오차 포함, 단방향), t_done_ms = TREL×1000, 1024청크 r.0.0 전수·중복
  검증, `meta.synthetic` **부재**, `meta.probe = "forceload+if-loaded full-status"`,
  `meta.probe_interval_ms = 100`, stage `hc-e6/zen5`, 단일-시리즈 (t_stage1_ms 없음 —
  hyperchunk 타임라인 아님). `hcviz validate` 3/3 OK (스키마+시맨틱).
- **패널 시계는 공개 벽에 핀**: race-b6.yaml `normalize_wall_s` 11.9 / 3.7
  (리스케일 ×1.0029 / ×1.0902, hyperchunk 패널의 0.894 핀과 동일 규약,
  `time_scaled_from_wall_s`로 로드-메타에 기록). 공개 벽은 0.5s-폴 계단의 수치라
  실측 t1(0.1s-폴)보다 크고, 화면에 공개 수치 외의 벽이 노출되지 않게 하는 조치.
  타임라인 파일 자체는 실측 t1을 그대로 보존.

## 3. 계단 vs 합성 대조 (완료 노트 의무 항목)

- 구 합성: vanilla=scan(행 스캔 점진), c2me=wave(파면 점진) — **경쟁자에게 유리한
  이상화**였음이 실측으로 확정 (VIZ-2 §6의 보수성 예상 그대로).
- 실측: 두 계열 모두 리빌이 wall의 98%+에서 스냅. 아티팩트 아님 —
  (a) B-6 자체 0.5s-폴 progress TSV가 동일 붕괴 (10× 적은 프로브 부하에서),
  (b) 0.1s 폴 사이클이 ~115ms 케이던스로 연속 기록됨 = 콘솔 큐 적체 없음,
  (c) 게이트가 벽 비팽창 증명. 즉 "서버가 청크를 쓸 수 있는 시점"(FULL 승격)은
  두 자바 계열 모두 실제로 끝에 몰린다.
- GIF에서 스무딩/재해석 안 함. gif 456KB → 264KB (계단 패널은 레이스 대부분
  구간에서 정지 상태라 프레임 델타 축소 — 크기 감소 자체가 형상의 증거).

## 4. 캡션 일반화 (절차 문서 §오차 표기)

- `_build_base()` 마이크로 캡션 일반화: 세그먼트 = [synthetic 패널 목록: 기존 카피] +
  [probe 패널 그룹(간격별): `probe ±0.Xs`]. 여전히 **한 줄**, v5 픽셀 스펙 불변
  (빈 밴드 y≈572, 폰트/좌표 동일). 현 데모 출력: `vanilla · c2me: probe ±0.1s`.
- synthetic 경로는 코드 유지 (합성 타임라인 렌더 시 자동 복귀). 혼합 시 " — " 조인.

## 5. 검증 (self-verification 전 항목)

- 게이트 표: §1. 사용 POLL_S 0.1 명기.
- `hcviz validate` 신규 2파일 OK; 청크수 1024/1024; 정렬-단조 확인; max t_done ≤ wall
  (11.863 ≤ 11.866 / 3.385 ≤ 3.394).
- 스틸 육안: t=3.7 바닐라 공백·C2ME 완성 "done"·캡션 한 줄 ✓; t=0.89 바닐라/C2ME
  공백 ✓; t=11.9 3패널 완성 ✓. 계단 가시성: t=11.5 바닐라 패널 무리빌(단색) →
  t=11.75 대량 리빌 (프레임 실측).
- hyperchunk 패널 IDENTICAL: 사전 백업 스틸 3장 vs 재렌더 — hyperchunk 칼럼
  (x≥796, 캡션 밴드 제외) **max|diff|=0** 3/3.
- `git diff --stat` 스코프: tools/viz/** + 본 노트만.
- 원격 정리: `pgrep -x java` 없음, run-*/world 디렉토리 0, 디스크 12% (40G 여유).
  raw 회수: `/mnt/scratch/bench/viz-capture/2026-08-19/` (101MB — chunks/progress
  TSV·서버 로그·threads·config·r.0.0.mca 12런 전량·results.jsonl·stage-census),
  러너+요약: `~/benchmarks/viz-capture/2026-08-19/` (viz_run.sh·results.jsonl·
  stage-census.txt). 원격 ART는 hc-e6 `~/benchmarks/viz-capture/2026-08-19/` 유지.
- 적대 리뷰 워크플로: 러너가 노트 작성 후 리뷰 단계에서 SIGTERM(rc=143, 12:02Z)으로
  중단 — 산출물은 전부 완성 상태였고 컨트롤러가 재검증 후 수습 커밋
  (hcviz validate 3/3 OK, 스틸 육안 검증, 게이트 표 대조). 리뷰 미완 항목 없음.

## 6. 스테이징 복원 (B-6 자산 소실 → 핀-검증 복원)

hc-e6이 B-6 이후 재부팅됨(커널 1018→1019) → `/tmp/b6-3way` 스테이징(서버 jar·
fabric-template·c2me jar) 증발, ART에는 러너도 없었음. "verbatim 재사용"을 해시-핀
복원으로 충족:

- `server-26.2.jar`: claw `tools/golden/libs/` 원본 rsync — **sha1 823e2250… = B-6 핀
  일치** (B-6도 같은 claw 원본에서 rsync했음).
- `c2me-fabric-mc26.2-0.4.2-alpha.0.35.jar`: Modrinth 핀 버전(HBLtzvqv) 재다운로드 —
  **sha512 전문 대조 일치** (078618f4…a504; B-1/B-6 prefix 핀과 동일).
- fabric-template: 리포 핀 `fabric-installer-1.1.2.jar`(tools/golden/libs/fabric)로
  hc-e6에서 재조립 (loader 0.19.3) — 부팅 로그 "Loading Minecraft 26.2 with Fabric
  Loader 0.19.3" + 템플릿 내장 server.jar sha1 일치.
- JDK: hc-e6 재부팅 후에도 **25.0.3+9 그대로** (B-6 동일).
- 기능적 verbatim 증명 = §1 게이트 자체 (같은-날 3s-폴 레퍼런스가 B-6 대역 재현).

## 함정 (다음 세션용)

- **hc-e6 /tmp는 재부팅에 증발** — B-6류 스테이징은 언제든 사라질 수 있음. 복원
  프로토콜은 §6 (핀 3종 + 게이트). **fabric-server-launch.jar의 sha1 핀은 무용** —
  이 jar는 2파일(매니페스트+properties) 부트스트랩이고 zip 타임스탬프 때문에 설치마다
  해시가 다름 (claw 골든 work 디렉토리 6개 설치 전부 상이 실측). 핀은 installer/loader
  버전 + server.jar sha1 + c2me sha512로.
- **ssh 원라이너에서 `pgrep -f java`는 자기 자신 매치** (원격 bash -c 커맨드라인에
  패턴 문자열 포함) — `pgrep -x java` 또는 `grep [j]ava`로.
- **C2ME FULL 승격은 0.1s 해상도에서 원자적** — 청크별 순서 정보가 0 (단일 폴
  사이클). 바닐라도 3사이클뿐. 자바 계열의 "점진 리빌"은 모드 훅 없이는 어떤 폴
  간격으로도 안 나온다 — 더 촘촘한 폴을 시도할 근거 없음 (부하만 증가).
- **POLL_S=3 레퍼런스 수치는 그리드 양자화 지배** (c2me 6.1/9.1처럼 2배 차이도
  정상) — 대역 판정은 0.1 런으로만, 3s 런은 "함의 구간 겹침"으로만 판독.
- 청크별 TREL은 사이클-말 스탬프 (send → sleep → grep → stamp) — 단방향 과대
  ≤ POLL_S + 틱 정렬. t1(wall)과 같은 방향이라 타임라인 내부 일관.
- 렌더 재현: `./bin/hcviz render demo/race-b6.yaml --out demo/out/race-b6.gif
  --out demo/out/race-b6.mp4` + still 3장 (README §usage 그대로).
