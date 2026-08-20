# GH-2 완료 노트 — OpenSpec 전환: .hermes 이관 + DECISIONS.md 폐기, openspec 단일 SSOT (2026-08-20)

Base: da709f0. 커밋: 93675a7(specs) → 28f3390(archive 이관) → 554f39e(DECISIONS 폐기·참조 치환) → 4ae54ad(AGENTS.md) → 본 노트 커밋.
도구: openspec CLI 1.10.0 (`~/.bun/bin/openspec`, node v24.14.1).

## 1. 산출물

- `openspec/specs/<capability>/` 7개: worldgen-parity(요구 8) / generation-pipeline(5) /
  simd-backends(3) / core-abi(4) / scheduler(4) / engineering-safety(5) /
  benchmarks-and-viz(8) — 각각 spec.md(MUST/SHALL + 시나리오) + context.md(Decision
  history, append-only).
- `openspec/project.md`: 방향 결정 ADR-001·ADR-005 전문 + capability 인덱스 +
  append-only 계승 규칙. `openspec/config.yaml`: 프로젝트 컨텍스트 + context_docs 규칙.
- `openspec/changes/archive/`: .hermes 노트·플랜 95파일 → 15개 날짜-슬러그 폴더로
  git mv (히스토리 보존 — `git log --follow` 원 커밋 추적 확인), 폴더별 README.md 신규.
- `AGENTS.md` (OpenSpec-first 워크플로 + 게이트 체인 + 클레임 펜스), DECISIONS.md git rm,
  README/CONTRIBUTING/PR 템플릿/feature_request.yml 참조 치환, `.gitignore`에 `.hermes/`.

## 2. ADR 이관 매핑 테이블 (9/9, 누락 0)

| ADR | 결정(normative) → spec.md | 근거·수치·역사 → context.md |
|---|---|---|
| ADR-001 (2026-07-27) | D4 패리티 본질→worldgen-parity; D5 디스클레이머→benchmarks-and-viz "Legal disclaimer" | **project.md** Decision history (전문: 3벽·인용·D1~D5·GIF/시드탐색 분석·anti-goals·pitfalls) |
| ADR-002 (2026-07-27) | D2 풀 파이프라인·D4 스코프→generation-pipeline; D3 비트패리티·P2 RNG 순서·P3 double→worldgen-parity; D1 순수 C→engineering-safety; P1 FMA→simd-backends; P4 충돌 스케줄링→scheduler | **generation-pipeline** (primary home, 전문: flops/roofline/Amdahl 표·정정 2건·pitfall 5건) |
| ADR-003 (2026-07-27) | D1 순수 계산 코어·D2 리전 ABI·D3 arena/스케줄러 소유→core-abi; D4 데이터팩 N-API·D5 청크 fallback→generation-pipeline | **분할**: ABI 절반(경계비용 표·40808 객체·경쟁 지형·모듈 경계도)→core-abi; 호환 절반(인터페이스 표·drop-in 정의·커버리지 표)→generation-pipeline |
| ADR-004 (2026-07-27) | D1 AVX2 기본·D2 cpuid 디스패치·D3 FMA 금지·D4 바이트동일 게이트→simd-backends | **simd-backends** (전문: Zen4 double-pump·zmm32·vpermt2pd·체인 분석 표 전량) |
| ADR-005 (2026-07-27) | 코어 무상태(소켓/플레이어/틱 심볼 부재)→core-abi "Pure compute core" | **project.md** Decision history (전문: AI 표·P1~P4 축 평가) |
| ADR-006 (2026-07-28) | D1 26.2 핀(4903/107.1)·비난독화 1차 근거→generation-pipeline; D2 JDK 25 golden→worldgen-parity; D4 FFM→core-abi | **generation-pipeline** (primary home, 전문: 릴리스 계보·파급 3건·pitfall 4건) |
| ADR-007 (2026-07-28) | 2단 게이트(Tier1/Tier2)·비결정성=입력·클레임 스코프→worldgen-parity | **worldgen-parity** (전문: Task 2 실측 증거·메커니즘·order-replay 논거) |
| ADR-008 (2026-07-28) | FREE/REPLAY 이중 모드·코드 공유·정책=라이브러리 계약→scheduler; P2 모드 표기 규율→benchmarks-and-viz | **scheduler** (전문: 인용·시연 서사·anti-goals) |
| ADR-009 (2026-07-28) | C11 유지·sanitizer 게이트·debug assert→engineering-safety | **engineering-safety** (전문: Rust 4축 평가·인용·pitfall 3건) |

추가 이관: B-6 노트 §0(공개 수치)·§3(canonical 정의)·§6(자막 권고) →
benchmarks-and-viz spec/context; VIZ-2/3/5의 캡처·타임라인 스키마 →
benchmarks-and-viz "Demo renders measured timing" + context.

## 3. 검증 결과

### openspec validate (1.10.0, 원문)

```
$ openspec validate --specs
✓ spec/benchmarks-and-viz
✓ spec/core-abi
✓ spec/engineering-safety
✓ spec/generation-pipeline
✓ spec/scheduler
✓ spec/simd-backends
✓ spec/worldgen-parity
Totals: 7 passed, 0 failed (7 items)
$ openspec validate --specs --strict
Totals: 7 passed, 0 failed (7 items)
```

### ADR 이관 완전성 — 적대 검증 (워크플로 11 에이전트)

ADR별 독립 에이전트가 원문을 요소 단위(수치·표 행·인용·항목)로 분해해 이관본과
전수 대조: **9/9 missing 0건**. altered 지적 2건은 이관 중 시제 교체("현재"→"당시",
ADR-002 P5·ADR-006 Context)로, append-only 원칙에 맞게 **원문 시제로 복원 후 종결**.
클레임 펜스 에이전트: PASS — 11.9/3.7/3.155/0.894/13.3x/4.14x·≥12.5x/≥3.3x(≥ 누락
0)·a5963205(golden/SHA256SUMS 풀 해시와 대조 일치)·2eb7485b·"canonical-identical
output at C2ME-class speed" verbatim, "faster than C2ME"는 금지 규칙 서술에만 존재,
변형 수치(13x/3.15s/0.89s 등) 0건. 링크 에이전트: PASS — 19파일 103개 상대 링크
전부 실재 경로로 해석, 깨진 링크 0.

### 나머지 게이트

- `grep -rn "DECISIONS"` (빌드 디렉토리·reference 제외): **라이브 문서 0건**.
  잔존은 `openspec/changes/archive/` 내 4파일(구 플랜 1·VIZ-5·GH-1)의 역사 인용뿐 —
  "이관 노트 내용 수정 금지" 규칙에 따라 유지 (판단 기록: 스펙·README·템플릿·코드
  기준 파일 참조 0 달성; ADR-00N 표기와 동급의 역사 인용으로 분류).
- `git ls-files .hermes` = **0**; `git ls-files openspec` = 128 (이관 95 + 아카이브
  README 15 + specs 14 + project.md/config.yaml 2 + 본 노트 폴더 2).
- cmake 빌드 + `ctest --test-dir build`: **37/37 PASS, 경고 0** (df_x8는 AVX-512 없는
  로컬에서 Skip — 정상). 코드 무변경 확인: core/cli/bench/tools/golden/scripts(ci 제외)
  diff 0.
- 커밋 제목 5건 `scripts/ci/check_commit_msgs.py --stdin` PASS. push 안 함 (컨트롤러 몫).

## 4. 판단·조정 기록 (CLI 실동작 기준 조정 포함)

1. **openspec init은 `openspec/config.yaml`만 생성** (`--tools none`) — specs/changes
   골격·project.md는 스키마(spec-driven) 문서 구조에 맞춰 수동 작성. validate가
   요구하는 spec.md 형식: `## Purpose`(50자 이상, strict) + `## Requirements` +
   `### Requirement:` + 요구마다 `#### Scenario:` 1개 이상.
2. **ADR 분할 방식**: ADR-003만 오너 지시대로 절반 분할(ABI/호환), ADR-002·006은
   과분할을 피해 primary home에 전문 보존 + 타 capability로 normative 포인터.
   이관 중 신규 주석은 전부 이탤릭 괄호("(이후 …)") 형태로 원문과 구분.
3. **코드 주석의 `.hermes/notes/*` 경로 참조**(core/tools/tests 41파일)는 코드 무변경
   펜스에 따라 유지. 대응: 경로의 디렉토리명이 archive 폴더 슬러그와 1:1
   (예: `.hermes/notes/bench/` → `openspec/changes/archive/2026-08-12-bench/`).
4. AGENTS.md·project.md의 "구 DECISIONS.md" 서술은 grep 기준(파일 참조 0)을 위해
   "구 append-only ADR 로그"로 표현 — 파일명 대응은 본 노트가 보존.
5. 아카이브 날짜는 각 그룹 완료 노트의 명시 일자 우선, 애매하면 마지막 커밋일
   (plans는 ADR-006 리타깃 반영 최종 커밋일 2026-07-28).

## 5. 남은 것 (스코프 밖)

- Terralith급 외부 데이터팩 golden 대조는 미수행 — generation-pipeline spec의
  Pure-JSON datapack 시나리오는 계약 서술 (context에 명시).
- GH-1 잔여(레이블·CI Required·private reporting 컨트롤러 설정)는 별도.
