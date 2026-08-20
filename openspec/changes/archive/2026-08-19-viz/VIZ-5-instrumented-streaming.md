# VIZ-5: 바닐라/C2ME 스트리밍 리빌 — 서버측 계측(Fabric 훅) 실측 교체

2026-08-19. 오너 결정(VIZ-4의 끝-계단은 정직하나 시각적으로 죽어 있음 → 서버측
계측으로 세 패널 모두 스트리밍) 실행. 로컬만, push 안 함. 공개 수치
(11.9/3.7/0.894/13.3x) 불변 — 이 태스크도 청크별 형상만 교체.

## 결론 요약

- **세 패널 전부 점진 리빌.** 바닐라/C2ME 패널이 서버측 계측 타임라인
  (`timelines/vanilla-hce6-instr.json`, `c2me-hce6-instr.json`)의 2-시리즈
  (t_stage1/t_done)로 교체됨 — hyperchunk 패널과 동일한 2-스테이지 시맨틱.
  VIZ-4 프로브 타임라인은 디스크 유지 (대조용, 데모 미사용).
- **핵심 실측: 두 자바 계열 모두 내부적으로는 점진 생성이다.** features 완료
  분포 p10/p50/p90 = 바닐라 17/46/87% wall (데실 거의 균일), C2ME 32/58/87%
  (S-커브). VIZ-4의 끝-계단은 **FULL 승격의 몰림**이었을 뿐, 생성 자체의
  형상이 아니었다. honest-failure 조항(내부도 끝-몰림 가능성)은 기각됨 —
  스트리밍 그림이 정직하게 성립.
- 오버헤드 게이트 **양 계열 PASS** (아래 §3), hyperchunk 패널 픽셀 **불변**
  (스틸 3장 x≥796 캡션 밴드 제외 max|diff|=0), `hcviz validate` 8/8 OK.
- 캡션: `vanilla · c2me: instrumented chunk timing · measured walls`
  (meta.instrumented 구동, 한 줄; probe/synthetic 캡션 경로는 대조 파일용 유지).

## 1. 모드 선택 — 별도 미니 모드 (stage-dump-mod 확장 아님)

`tools/viz/capture/chunk-timeline-mod/` 신규 (2클래스: TimelineLog +
ChunkStepTimelineMixin). stage-dump-mod은 **바이트 단위로 미접촉** (재빌드도
안 함) — 골든 경로 무결성이 구조적으로 보장된다. 확장안을 기각한 이유:

1. 골든 StageLog는 **의도적으로** 라인당 eager flush + 단일 락 (order manifest가
   내구 순서를 요구) — VIZ-5 오버헤드 규칙(인메모리 append + 종료시 단일 flush)과
   정면 충돌. 로거를 갈아끼우는 순간 골든 경로 변경 리스크.
2. 훅 자체는 골든 ChunkStepMixin 패턴 재사용 (`ChunkStep.apply` RETURN에
   thenApply 연속 부착 — 검증된 초크포인트). 빌드도 같은 방식 (plain java,
   tools/golden/libs 재사용, loom 없음, ADR-006 D3).

핫패스 비용: 이벤트당 nanoTime + 소형 레코드 할당 + ConcurrentLinkedQueue
append (lock-free). 런당 이벤트 20,578(바닐라)/19,256(C2ME) — 카운트가 런 간
**결정론적** (무결성 크로스체크로 유용). 플러시는 JVM shutdown hook에서 TSV
단일 쓰기. inert 게이트: `-Dhyperchunk.timeline.file` 부재 시 완전 무동작.

## 2. 스테이지 바인딩 (완료 노트 의무 항목)

| 시리즈 | ChunkStatus | 근거 |
|---|---|---|
| t_stage1_ms | **SURFACE 완료** (ChunkStep.apply future 완료 시점) | "지형 확정·데코 미실행"의 자바측 최근접 정직점: surface가 상단 블록(잔디/모래)을 확정 — 탑뷰 톤의 정보가 이 시점 이후 불변. noise는 shape만(상단 블록 미정), carvers는 탑뷰 비가시(동굴). hyperchunk chain(noise~carvers) 스팬 안에 위치 — 세 패널이 같은 의미("터 잡힘 스케치") |
| t_done_ms | **FEATURES 완료** | 마지막 블록 콘텐츠 쓰기 — hyperchunk complete(마지막 substantive write)의 자바측 대응. FULL은 서빙 가능 시점이지 콘텐츠 시점이 아님 (그리고 C2ME에서 관측 불가, 아래) |

**C2ME는 FULL 스텝이 ChunkStep.apply를 안 탄다** (`c2me-rewrites-chunk-system`이
full 변환을 자체 처리 — 로컬 스모크 실측: g01~g10만 발생, g11/l11 부재).
surface/features는 C2ME에서도 ChunkStep.apply 경유 — 바인딩이 양 계열에서 동일
훅으로 성립. FULL에 타임라인 시맨틱을 걸었다면 C2ME에서 붕괴했을 것.

**양 피라미드 기록 (g=GENERATION, l=LOADING)**: 부트 스폰-프렙이 r.0.0에 144청크를
t0 이전 디스크 저장 (census 지문 B-6 §3와 동일: structure_starts 128 / biomes 7 /
carvers 5 / initialize_light 3 / full 1). 이 중 features 이상으로 저장된 **4청크**
(il 3 + full 1 = 스폰 2×2)는 창 내 g05/g07이 없음 → 변환기가 청크·스테이지별
**첫 창-내 이벤트(피라미드 무관)**를 취해 l05/l07(로드 순간)로 폴백,
t_stage1≈t_done 동시 등장 (µs 차) — 콘텐츠가 부트에 결정됐으므로 정직.
`meta.disk_loaded_chunks=4` 기록. 나머지 140청크는 창 내에서 이어서 생성(g 이벤트).

## 3. 오버헤드 게이트 (같은 날 인터리브, hc-e6, 12:36–12:47Z, 커널 1020)

러너: 클린 = viz_run.sh 그대로 (POLL_S 0.1), 계측 = viz5_run.sh (viz_run.sh 사본 +
{instr 모드 케이스, -Dhyperchunk.timeline.file, 부트/트레일러 assert, TSV 아카이브,
결과 필드 4개} — **프로브 폴 루프 불변**이라 벽 측정이 클린과 동일 조건). 실행
순서 클린-1 → 계측-1 → … 계열별 교대. (캠페인에 실제 쓰인 러너 사본은
HC_GRID_FIRST 블록이 빠져 있었고 `# flush` 게이트였다 — §8 리뷰 확정 1·2번으로
repo 판은 수정됨; 두 결함 모두 이번 캡처 결과에는 무영향이며 아카이브 사본은
실행 당시 그대로 보존.)

| 계열 | 클린 3런 | 계측 3런 | 계측 중앙값 | VIZ-4 대역 | 판정 |
|---|---|---|---|---|---|
| vanilla | 11.803 / 11.729 / 11.739 (med 11.739) | 11.708 / 11.562 / 11.820 | **11.708** | 11.7–11.9 | **PASS** |
| c2me | 3.336 / 3.315 / 3.317 (med 3.317) | 3.321 / 3.355 / 3.457 | **3.355** | 3.35–3.69 | **PASS** |

- 바닐라 계측 중앙값 11.708 = 태스크 대역(11.7–11.9) 안. VIZ-4 raw 최저치
  11.715 대비 −7ms는 런 노이즈 수준이고, **같은 날 클린 중앙값(11.739)보다
  낮음** — 게이트의 목적(로깅이 벽을 안 민다)에 대한 직접 증거.
- c2me 같은 날 클린 중앙값 3.317은 VIZ-4 대역 하한(3.348)보다 소폭 낮음 —
  재부팅(커널 1019→1020) 후 박스가 미세하게 빠른 날. 계측 중앙값은 대역 안.
- 무대 규율: census pre/post `stage-census.txt` 추가 기록, predone 0/1024·
  디스크 census 144 지문 전 런 동일, Worker-Main 31 / c2me-worker 12,
  c2me config **VIZ-4와 diff 0** (config-c2me-instr-* 대조).

## 4. 실측 형상 (내부 진행 — 완료 노트 의무 항목)

채택 런 (게이트-통과 계측 3런 중 벽 중앙값): **vanilla v5i-1 (t1 11.708s)**,
**c2me v5i-2 (t1 3.355s)**.

| 계열 | 시리즈 | p10 | p25 | p50 | p75 | p90 | max (%wall) |
|---|---|---|---|---|---|---|---|
| vanilla | s1(surface) | 15.4 | 27.0 | 43.7 | 72.7 | 84.6 | 92.0 |
| vanilla | done(features) | 17.4 | 29.4 | 46.4 | 75.7 | 86.5 | 92.8 |
| c2me | s1(surface) | 30.5 | 39.8 | 54.8 | 77.3 | 85.7 | 91.0 |
| c2me | done(features) | 32.2 | 42.1 | 57.6 | 79.2 | 87.0 | 91.7 |

- 바닐라: 데실당 완료 수 [36,96,135,140,158,69,66,124,154,46] — 전 구간 분산,
  NW 코너에서 스윕하는 생성 파면이 화면에 그대로 보임.
- C2ME: 첫 10%는 조용(병렬 스케줄 워밍업+의존성 래더)하다가 30%부터 대량
  병렬 완료 — 진짜 S-커브. **C2ME는 내부적으로 확실히 점진적.**
- GIF 프레임 실측 (painted %): vanilla 23/61/78% @ 25/50/75% wall,
  c2me 6/40/71% — 과제의 목적(스트리밍 look) 달성. gif 264KB → 462KB
  (패널이 레이스 내내 움직이므로 프레임 델타 증가 — VIZ-4 축소의 역방향).

## 5. 변환·데모 (`hcviz convert-instr` 신규)

- 시계 매핑: 모드가 TSV 헤더에 (epochMillis, nanoTime) 쌍 2개(ref/flush) 기록 →
  이벤트 nano를 epoch로 사상, 러너 t0_epoch(`date +%s.%N`, results.jsonl 신규
  필드)와의 차 = t_rel. ref↔flush 드리프트 실측 −0.21/−0.58ms (런 전장) —
  100ms 초과 시 변환기가 거부.
- 검증(변환기 내장): r.0.0 1024 전수(양 시리즈)·중복 불가·t_stage1≤t_done·
  wall 초과 거부. `hcviz validate` 스키마+시맨틱 8/8 OK.
- 스팟체크 3+3청크 (최조기/중앙/최후기, raw nano → 수동 재계산 vs JSON):
  **전부 MATCH** (µs 단위 일치). 디스크-로드 4청크의 s1≈done 동시성도 확인.
- race-b6.yaml: 바닐라/C2ME 타임라인 경로만 교체, `normalize_wall_s` 11.9/3.7
  유지 (리스케일 ×1.0164 / ×1.1028, `time_scaled_from_wall_s` 기록). 엔드카드
  13.3x 불변. 타임라인 파일은 실측 t1 보존.
- meta: `instrumented="fabric-loader+chunk-timeline-mod"`, `stage1_event=surface`,
  `done_event=features`, `disk_loaded_chunks`, `clock_drift_ms`;
  **probe_interval_ms 부재** (캡션 분기 충돌 방지). 스키마에 instrumented/
  done_event 선언 추가 (additionalProperties true라 게이트는 아니고 문서화).
- 캡션 일반화: `_build_base()`에 instrumented 세그먼트 추가 (synthetic 스타일
  카피), probe 분기는 `not instrumented` 가드. 현 데모 출력 한 줄:
  `vanilla · c2me: instrumented chunk timing · measured walls` (스틸 크롭 육안 확인).

## 6. 공정성 각주 (캡션·데모 헤더 주석에 반영)

> 바닐라 패널의 청크별 타이밍은 **Fabric loader 0.19.3 + chunk-timeline-mod**를
> 얹은 서버에서 계측했다 (바닐라 전용 서버는 모드를 로드할 수 없음). 로더/모드는
> 월드젠을 변경하지 않는다(골든 파이프라인 선례 — 훅은 관측 전용; 단 바닐라
> 26.2 자체가 런 간 내용-비결정적이라 — ADR-007 — 어떤 두 런도 비트 동일하지
> 않으며, 이번 캠페인 12런의 canonical 해시도 전부 상이). 벽시계는 게이트로
> 비팽창 증명 (계측 중앙값 11.708s가 무-모드 클린 대역 11.7–11.9 안, 같은 날
> 클린 중앙값보다 낮음). 패널 시계가 가리키는 공개 벽 11.9s는 **무-모드
> 바닐라**의 B-6 실측 그대로다. C2ME 패널은 Fabric + C2ME + 모드 (동일 훅).

## 7. 검증 (self-verification 전 항목)

1. 게이트 표 §3 (3+3 인터리브, 중앙값, 대역 판정 명기).
2. validate 8/8 (신규 2 포함), 1024/1024 양 파일, stage1≤done 전수(변환기 assert
   + validator), 스팟체크 3청크×2계열 MATCH (§5).
3. GIF 25/50/75% 프레임 점진 충전 §4 (모노토닉), hyperchunk 스틸 3장 픽셀-diff 0.
4. git diff 스코프: tools/viz/** + 본 노트만 (stage-dump-mod 미접촉, core/golden/
   scripts/README/DECISIONS 미접촉, hyperchunk·VIZ-4 타임라인 파일 무변경).
5. 원격 정리: `pgrep -x java` 없음, run 디렉토리 0, 디스크 12%. raw 회수:
   `/mnt/scratch/bench/viz-capture/2026-08-19/` (+70파일: instrumented-timeline
   6종·클린/계측 12런 전 아티팩트·results-viz5.jsonl·stage-census-viz5.txt·
   캠페인 로그 — VIZ-4 raw 무접촉). 러너+요약: claw
   `~/benchmarks/viz-capture/2026-08-19/` (viz5_run.sh·viz5_campaign*.sh/log·
   results-viz5.jsonl). 원격 ART는 hc-e6 유지 (VIZ-4 관례).
6. 스테이징 복원 (또 재부팅: 커널 1019→**1020**, /tmp/b6-3way 증발): VIZ-4 §6
   프로토콜 그대로 — server jar sha1 `823e2250…` 전문 일치, c2me Modrinth
   HBLtzvqv 재다운 sha512 `078618f4…a504` 전문 일치, fabric-installer-1.1.2로
   템플릿 재조립 (loader 0.19.3 부팅 로그 확인). JDK 25.0.3+9 유지.

## 8. 적대 리뷰 (3렌즈 리뷰어 × 발견당 2스켑틱, 총 39 에이전트)

확정 5건 (전부 수습 커밋으로 수정, 캡처 데이터 자체는 전부 무영향 — 6개 계측
TSV는 1024 전수 + 결정론 이벤트 카운트로 완결성 독립 증명):

1. **플러시 게이트 불건전** (med): `# flush` 헤더가 이벤트 행 앞에 쓰임 —
   시작 증거일 뿐. mid-loop IOException(ENOSPC 등) 시 잘린 파일이 게이트를
   통과. → 모드가 트레일러 `# end events=N`을 행 뒤에 방출, 러너가 트레일러
   + 행수 일치를 게이트.
2. **viz5_run.sh가 HC_GRID_FIRST 블록을 무고지 삭제** (med): 헤더의 "나머지
   IDENTICAL" 계약 위반 — HC_GRID_FIRST=1이 조용한 no-op이 됐음. → 블록 복원
   (캠페인에 영향 없음: 캠페인은 HC_GRID_FIRST 미사용; 아카이브된 러너 사본은
   실행 당시 그대로 보존).
3. **.gradle/ 로컬 캐시 13파일 커밋** (low): 루트 .gitignore가
   stage-dump-mod/.gradle만 커버. → untrack + ignore 항목 추가.
4. **"월드젠 비트 동일" 문구 과장**: 바닐라 26.2는 런 간 내용-비결정적
   (ADR-007; 캠페인 12런 canonical 전부 상이) — 정확한 주장은 "로더/모드가
   월드젠을 변경하지 않음". → yaml 헤더·§6 각주 재서술.
5. **믹스인 javadoc 과장**: 144 프리젠 청크 "전부 l-이벤트만"이라 서술했으나
   실제 140개는 창 내 g-이벤트로 재개, l-전용은 4개뿐. → javadoc 정정.

주요 기각 (양 스켑틱 반박): normalize 리스케일 왜곡 주장(사전 존재 관례·공개벽
핀은 태스크 binding), 게이트 비대칭 주장(대역 기준이 사전 등록 기준), stage1
surface-vs-chain 불일치(carvers 델타 실측 p50 3ms — 렌더 효과 0, 프레임 양자화
이하), shutdown 훅 레이스/무한 큐(설계 봉투 내), 스키마 선언 미비(스펙대로).

## 함정 (다음 세션용)

- **shutdown hook의 System.out은 서버 로그에 안 남는다** — log4j가 stdout을
  가로채는데 hook 시점엔 이미 정지 → println 증발 (v5i-1 실측, 러너 1회 FATAL).
  플러시 완결성은 **TSV 트레일러 `# end events=N`으로** 게이트하라 (`# flush`
  헤더는 이벤트 행 **앞에** 쓰여서 시작 증거일 뿐 — 적대 리뷰 확정 건, 수정됨).
  로그 grep 금지.
- **bash: `[ cond ] && { …; }`를 while 본문 마지막에 두면 set -e 지뢰** — 조건
  거짓 시 리스트 상태 1이 루프 종료 상태로 새서, 성공적으로 끝난 함수가 1을
  반환 → set -e가 스크립트를 죽임 (스모크에서 실측; viz_run.sh가 전부 if/fi인
  이유). 파생 스크립트 작성 시 if/fi 유지.
- **C2ME에서 FULL 스텝은 ChunkStep.apply 비경유** (rewrites-chunk-system).
  g11/l11 이벤트 없음 — FULL 기반 시맨틱 설계 금지. surface/features는 경유.
- **`*v5*` 글롭은 `viz5_*`·`results-viz5`를 못 잡는다** (인접 "v5" 부재) —
  아카이브 rsync에서 명시 지정 필요.
- 계측 TSV 이벤트 수는 모드별 결정론 (vanilla 20578 / c2me 19256) — 회수본
  무결성 체크에 사용 가능.
- epoch↔nanoTime 드리프트는 이 지평(~30s)에서 sub-ms (−0.2~−0.6ms 실측) —
  ref 쌍 1개 매핑으로 충분. 변환기가 100ms 초과를 거부하니 NTP 스텝 등
  이상은 자동 검출.
- 부트 스폰-프렙 144청크 지문은 시드 고정 산물 (B-6/VIZ-4/VIZ-5 전 런 동일:
  128/7/5/3/1). predone(로드 census)은 0이어도 **디스크 census는 144** —
  둘을 혼동하지 말 것.
