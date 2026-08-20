# worldgen-parity — Context

## Purpose & scope

[spec.md](spec.md)가 패리티 정의·게이트의 normative SSOT다. 이 문서는 그 결정의
역사와 근거, 현재 게이트 상태의 배경을 담는다.

## Current state (2026-08 기준)

- 풀 리전 r.0.0이 바닐라 golden 캡처와 canonical 해시 일치
  (`golden/SHA256SUMS`에 핀, `scripts/parity_gate.sh`가 판정). 37개 ctest 전부
  green, sanitizer-clean. CI는 추적 데이터만으로 도는 25개 서브셋을 실행하고,
  golden 의존 12개 스위트는 로컬 전용 (.github/CONTRIBUTING.md § Tests and
  golden data).
- strict 대조(스케줄-틱 포함 canonical 정규화)가 코드 기본값. 재캡처-3 unified
  골든 + `postProcessGeneration` 드레인으로 region 게이트 4/4 byte-exact
  (Task 13-close, [archive](../../changes/archive/)).
- 각 스테이지 게이트가 무엇을 커버하고 무엇에 blind한지는 mutation probe로
  실측해 태스크별 완료 노트에 기록했다
  ([changes/archive/](../../changes/archive/)의 task7~task14 폴더).

## Canonical hash의 유래

"canonical-일치" 정의(스펙의 Canonical payload hash 요구사항)는 B-6 공개 벤치
노트 §3에서 공개 문서용으로 확정된 것이다: 저장-시각 필드(루트 `LastUpdate`,
mca 헤더 타임스탬프 테이블)와 섹터 배치·압축 프레이밍을 제외한 청크 페이로드
전 바이트의 정규화 해시 (`tools/golden/compare_regions.py`). 골든의 헤더
타임스탬프는 캡처 당시 벽시계라 raw-비트 일치는 어떤 시스템도 원리적으로
불가하다. 골든 canonical `a5963205…3c24`, FREE own-v1 `2eb7485b…84d6`
(golden/SHA256SUMS). 전문: [benchmarks-and-viz/context.md](../benchmarks-and-viz/context.md).

## Decision history

> append-only. 기존 항목은 수정하지 않는다. 결정이 바뀌면 새 항목을 추가하고
> 이전 항목을 `Superseded by`로 표시한다.

관련 ADR 배치: ADR-002(전문: [generation-pipeline/context.md](../generation-pipeline/context.md))의
D3(비트 패리티 필수)·P2(RNG 소비 순서)·P3(double 유지)가 본 capability
spec.md의 요구사항으로 반영되어 있다. ADR-006(전문: 같은 문서) D2(golden 생성
환경 JDK 25)도 여기 spec.md로 반영. 아래는 본 capability가 primary home인
ADR-007 전문이다.

### ADR-007 — 패리티 게이트를 2단(비트정확 + 순서재생)으로 재정의한다 (Phase 1, 2026-07-28)

**Status:** Decided
**Type:** Quality strategy / Contract
**Resolves:** ADR-002 D3의 "region sha256 일치" 게이트를 정밀화 (원칙 유지, 정의 교체)

#### Context

Task 2 golden 작업에서 실측으로 확인된 사실: **바닐라 26.2 월드젠은 같은 시드·같은 머신에서도 run-to-run 비결정적이다.** 증거는 `tools/golden/NOTES.md`에 기록했다. 요약:

- `01_structure_starts`~`06_carvers` 스테이지 덤프는 두 독립 실행에서 바이트 단위 일치
- `07_features` 이후는 3x3 기본 런에서 4/9 청크, 5x5 순차 probe에서 16/25 청크가 실제 내용 차이 (glow_lichen/vine/광맥 배치)
- raw `.mca`는 물론 타임스탬프를 마스킹한 canonical payload 해시도 불일치
- 순차 forceload로도 안정화 실패: **full 승격 순서 자체가 두 probe 런에서 달랐다**

메커니즘: carvers까지는 (seed, chunk pos)의 위치시드 순수 함수라 순서 무관. features는 청크 경계를 넘어 읽고 쓰므로(나무/덩굴 스필오버, heightmap 갱신) 이웃 데코 순서가 결과를 바꾸고, 스케줄러는 그 순서를 고정하지 않는다.

따라서 ADR-002 D3의 "바닐라 region과 sha256 일치"는 그대로는 정의 불성립이다. **바닐라가 자기 자신과도 sha256이 일치하지 않는다.**

#### Decision (3 핵심 결정)

| # | 결정 | 핵심 |
|---|---|---|
| D1 | **Tier 1 게이트: 01~06 스테이지는 바닐라 golden 덤프와 바이트 단위 일치** | 순서 무관 구간이므로 무조건 비트정확. noise/surface/carvers가 여기 속함 |
| D2 | **Tier 2 게이트: features 이후는 order-replay 검증** | golden 런의 실제 features 실행 순서를 order manifest로 기록하고, C 구현이 그 순서를 재생해 07~11 덤프와 바이트 단위 일치해야 함 |
| D3 | **비결정성을 노이즈가 아니라 입력으로 승격** | 서로 다른 순서의 golden 런 2개를 모두 재생·일치시키면 알고리즘 패리티의 강한 증거 |

#### Why order-replay over 바닐라 순서 고정?

바닐라를 우리 순서에 맞추는 실험(sequential probe)은 실패했다. 스케줄러가 외부에서 고정되지 않는다. 반대로 **바닐라가 실제로 한 순서를 기록해 우리가 따라가는 것**은 항상 가능하고, 재현 가능한 golden 번들(덤프 + order manifest)을 만든다. C 코어는 어차피 배치 스케줄러를 소유하므로(ADR-003 D3) 임의 순서 재생이 자연스럽다.

#### 3-way 시연에 대한 의미 (ADR-001 연계)

"바닐라와 비트단위 동일" 주장의 정확한 스코프는 "**같은 시드 + 같은 데코 순서에서** 비트단위 동일"이 된다. 시연 문구와 README는 이 스코프를 정직하게 명시한다. 이는 약점이 아니라 오히려 방어력이다: 기술 관객이 "바닐라도 런마다 다른데 뭐랑 같다는 거냐"고 공격하는 지점을 우리가 먼저 점유한다.

#### Anti-goals

- 바닐라 스케줄러 자체를 패치해 순서를 고정하는 접근 (침습적, 유지 불가)
- features 이후를 "통계적 유사"로 완화 (패리티 원칙 포기)
- Tier 1 구간을 order-replay로 낮추기 (불필요한 약화)

#### Pitfalls

1. **order manifest 기록이 아직 미구현.** stage-dump mod 확장 필요 (open item). manifest 없는 golden은 Tier 2 검증에 쓸 수 없다. *(이후 구현 완료 — Task 9-pre에서 훅 설계 확정, [archive](../../changes/archive/) task9pre-order 노트 참조.)*
2. **probe의 순차 강제 실패 원인 미규명.** forceload 완료 대기 로직이 스케줄러를 못 고정했다. Tier 2 설계는 이 실패에 의존하지 않지만, 재도전은 시간 낭비다.
3. **spawn/full 스테이지 diff는 features의 하류 효과.** 별도 원인으로 오인하지 말 것.

#### Verification

- golden 번들에 order.manifest 포함 확인
- C 구현이 manifest 재생 모드를 지원하고, 서로 다른 순서의 golden 2벌을 모두 통과
- Tier 1: `diff -rq` 01~06 전체 0 diff

#### References

- ADR-002 (패리티 원칙 — [generation-pipeline/context.md](../generation-pipeline/context.md)), ADR-003 (배치 스케줄러 소유 — [core-abi/context.md](../core-abi/context.md))
- tools/golden/NOTES.md (증거·환경·재현 절차)
- tools/golden/experiments/sequential_probe.sh
