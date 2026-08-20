# benchmarks-and-viz — Context

## Purpose & scope

[spec.md](spec.md)가 공개 수치·클레임 규칙의 normative SSOT다. 이 문서는 그
수치의 출처(B-6 캠페인)와 측정 프로토콜 요약, 캐벳, viz 파이프라인의 사실
기록을 담는다. 원본 노트 전량은
[changes/archive/](../../changes/archive/)의 bench(B-1~B-6)·viz(VIZ-1~5)
폴더에 있다.

## B-6 공개 재실측 (2026-08-12, hc-e6) — 수치의 출처

- **무대**: OCI `VM.Standard.E6.Flex`, 16 OCPU = AMD EPYC 9J45 (Zen5) SMT 32
  vCPU, 64GB, Ubuntu 24.04, OpenJDK 25.0.3 (자바 두 계열 `-Xms2G -Xmx8G`).
  hyperchunk는 HEAD 7aaf6c7, preset `bench-o2`, 이 무대에서 noise 커널은
  AVX-512 백엔드(런타임 디스패치). 무대 적격성은 B-4에서 실측 (벤치-중 steal
  0틱, instructions 결정론 1.4e-5).
- **작업**: 시드 1234567890, 오버월드 r.0.0 풀 커버 1024청크, 3런 중앙값.
  바닐라/C2ME는 0.5s 정밀-폴 (`forceload` + `execute if loaded` 프로브),
  hyperchunk는 내부 계측 `gen_wall` (생성+serialize+sha256 포함).
- **결과**: 바닐라 11.9s / C2ME 3.7s / REPLAY 20T 3.155s / FREE 20T 0.894s.
  32T 병기: REPLAY 3.091s, FREE 0.829s (14.4x — 단 20T가 공식). cps 표기:
  86 → 277 → 1,145 chunks/s (FREE 20T).
- **하한 유도**: 경쟁자 = 최소런 − 폴 간격 0.5s − 커맨드-레이턴시 2틱(0.1s,
  보수 가정치), hyperchunk = 최대런 → FREE vs 바닐라 **≥12.5x**, vs C2ME
  **≥3.3x**. 프로브 주입 부하는 추가 미공제 (방향은 경쟁자 과대 = 우리 유리).
- **REPLAY vs C2ME 1.17x는 오차 밴드 안** (같은 공제로 하한 0.95x) — 우열
  공개 클레임 금지의 근거. 올바른 표현: "REPLAY는 C2ME-급 속도로 골든과
  canonical-일치 재생" (spec의 REPLAY claim phrasing).
- **결정론**: 바닐라 3런 3해시 (semantic diff 581/587/591 /1024), C2ME 5런
  5해시 (758~804/1024), hyperchunk REPLAY 6/6 == 골든 `a5963205…3c24`, FREE
  6/6 == own-v1 `2eb7485b…84d6` (20T/32T 각 3런). 기전: 데코가 이웃 청크의
  현재 블록을 읽는 구조 → 이웃 간 완료 순서가 내용에 새겨짐.
- **측정창 대칭**: 자바 boot 6.0~7.0s 제외 ↔ hyperchunk setup ~0.77s(참조
  로드·DF 컴파일) 제외. 원자료 jsonl에 proc_wall_ns·setup_ns 보존 (FREE 20T
  proc_wall 중앙값 1.68s).
- **주요 캐벳** (전량은 B-6 노트 §5): 클라우드 VM(steal 2틱 관측), 폴링
  단측 오차 3성분 전부 우리에게 유리한 방향(하한에서 공제), 스폰 선생성
  144청크는 경쟁자에게만 유리, C2ME는 알파 채널(26.2 대응 최신), C2ME
  12워커는 힙-유래 기본값이며 24워커 강제 감도런 3.7s 무이득으로 봉합,
  무대-종속 배율(AVX-512/32vCPU) — 일반화 금지.

수치 계보: B-1 (claw, 2026-08-06): 바닐라 15.8s / C2ME 6.5s / FREE 2.74s
(5.76x) → B-6 (hc-e6): 위 표. 배율 확대에는 무대-종속 기여(AVX-512, 32코어,
P2-8~11 누적)가 섞여 있다.

## Viz 파이프라인 (VIZ-1~5, 2026-08-13~19)

- **hyperchunk 캡처**: `HC_BENCH_TIMELINE` 워터폴 v1 마크 (+ pp용 P 레코드
  `P <m> <cx> <cz> <t0> <t1>`) → `hcviz convert`. t0=setup_end 마크,
  wall=proc_end−setup_end.
- **이벤트 정의**: complete(기본) = max(자기 chain w5, ±1창 데코 E.t1) =
  마지막 실질 블록 쓰기. serialize는 p50 98.9%가 끝에 몰려 리빌이 무너져
  기각(VIZ-2). `--stage1 chain`이 own C.w5를 `t_stage1_ms`로 추가 방출 —
  2-스테이지 리빌(VIZ-3): chain 시점 옅은 지형 톤(stage1.cells=1 하드
  불변식) → complete 시점 최종 픽셀.
- **자바 계열 캡처**: Fabric loader 0.19.3 + `chunk-timeline-mod`
  (ChunkStep.apply RETURN에 thenApply 부착; `-Dhyperchunk.timeline.file`
  부재 시 완전 inert; shutdown-hook TSV flush, 트레일러 `# end events=N`
  으로 완결성 게이트). t_stage1_ms = SURFACE 완료, t_done_ms = FEATURES
  완료. **C2ME는 FULL 스텝이 ChunkStep.apply를 비경유하므로 FULL 기반
  시맨틱 금지** (VIZ-5).
- **시계 매핑**: TSV 헤더의 (epochMillis, nanoTime) ref/flush 쌍으로
  nano→epoch 사상; 드리프트 100ms 초과 시 `hcviz convert-instr` 거부 (실측
  −0.2~−0.6ms).
- **스키마**: `tools/viz/schema/timeline.schema.json` (draft 2020-12).
  chunks[]에 `t_done_ms` + 옵션 `t_stage1_ms` (t_stage1 ≤ t_done 불변식).
  meta: `synthetic` / `probe`·`probe_interval_ms` / `instrumented`·
  `stage1_event`·`done_event`·`disk_loaded_chunks`·`clock_drift_ms` /
  `time_scaled_from_wall_s` — 캡션 분기(synthetic/probe/instrumented)를
  구동.
- **실측 결과**: features 완료 p10/p50/p90 바닐라 17/46/87%, C2ME 32/58/87%
  — 세 패널 전부 점진 리빌 성립. 프리젠 폴백: 부트 스폰-프렙 144청크 중
  features 이상 4청크는 첫 창-내 이벤트로 폴백 (meta.disk_loaded_chunks=4).

## Decision history

> append-only. 기존 항목은 수정하지 않는다. 결정이 바뀌면 새 항목을 추가하고
> 이전 항목을 `Superseded by`로 표시한다.

이 capability의 클레임 규칙은 단일 ADR이 아니라 B-6 노트 §0(공개 수치)·
§6(GIF·공개 자막 권고)에서 이관됐다 (2026-08-12; 원문은
[changes/archive/](../../changes/archive/) bench 폴더의 B-6-3way-public.md).
공개 표기 규율의 뿌리는 ADR-008 P2(벤치=FREE, 패리티=REPLAY —
[scheduler/context.md](../scheduler/context.md))와 ADR-001 D5(디스클레이머 —
[project.md](../../project.md)). ADR-002 P5(벤치 호스트 신뢰성)는 B-4의
hc-e6 적격성 실측으로 해소됐다
([generation-pipeline/context.md](../generation-pipeline/context.md)).
