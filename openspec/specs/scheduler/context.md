# scheduler — Context

## Purpose & scope

[spec.md](spec.md)가 이중 모드 스케줄러의 normative SSOT다. 이 문서는 "왜 두
모드인가"(ADR-008)의 결정 역사와 이후 구현 사실을 담는다.

## Current state (2026-08 기준)

- FREE는 셀-FIFO DAG 스케줄러(데코 ±2 이웃 의존 + 스테이지 배리어, P2-3)로
  구현됐고, 충돌쌍을 직렬화한 하나의 유효한 총순서를 고정해 own-v1 해시로
  결정론을 판정한다. 바닐라 features가 단일-스레드 실행기라 비결정 축이
  순서뿐이라는 바닐라-계급 논거는 P2-3 노트 §1.4
  ([changes/archive/](../../changes/archive/) phase2).
- FREE own-v1 상수 재고정 규칙·골든 manifest는 크롤(게이트 전용) 등 운영
  함정도 P2-3 노트에 기록. TSan 게이트는 `setarch -R`로 돌린다.
- B-6 실측: REPLAY 6/6 == 골든 `a5963205…`, FREE 6/6 == own-v1 `2eb7485b…`
  (20T/32T 각 3런) — 공개 수치·클레임 규칙은
  [benchmarks-and-viz](../benchmarks-and-viz/spec.md).
- 벤치 숫자는 항상 FREE, 패리티 배지는 항상 REPLAY로 표기한다 (ADR-008 P2).
  normative 클레임 규칙은 benchmarks-and-viz spec.md.

## Decision history

> append-only. 기존 항목은 수정하지 않는다. 결정이 바뀌면 새 항목을 추가하고
> 이전 항목을 `Superseded by`로 표시한다.

배치: ADR-008은 본 문서가 primary home. 충돌 회피 스케줄링의 출처인 ADR-002
P4는 [generation-pipeline/context.md](../generation-pipeline/context.md),
스케줄러 소유(ADR-003 D3)는 [core-abi/context.md](../core-abi/context.md).

### ADR-008 — 코어 스케줄러는 벤치 모드(자유 순서)와 검증 모드(order-replay) 이중 모드로 동작한다 (Phase 1, 2026-07-28)

**Status:** Decided
**Type:** Architecture / Contract
**Resolves:** ADR-007의 게이트 정의를 실행 모드로 구체화

#### Context

ADR-007이 패리티 게이트를 2단으로 재정의한 뒤, 사용자가 실행 모드 분리를 확정했다. 원문:

> "우리도 멀티스레딩은 해야하니까 속도 비교에서는 가장 최적화된 (바닐라처럼 약간 비결정론적)방식을 보여주고, TC, 일치성 테스트에서는 순서 따라가기를 통해 일치하는지 (여기서 속도는 후 운선순위니까) 보면 되겠다"

#### Decision (3 핵심 결정)

| # | 결정 | 핵심 |
|---|---|---|
| D1 | **벤치/시연 모드: 자유 스케줄링** | 충돌 없는 최대 병렬(체스판 등). features 적용 순서는 성능 최적으로 자유. 바닐라와 같은 수준의 순서 비결정성 허용 *(이후 P2-3에서 FREE도 총순서를 고정해 출력 결정론을 획득 — Current state 참조)* |
| D2 | **검증 모드: order-replay** | golden order manifest를 재생. 속도는 후순위, 07~11 스테이지 바이트 일치가 목표 (ADR-007 Tier 2) |
| D3 | **두 모드는 같은 스테이지 코드를 공유, 스케줄러 정책만 교체** | 검증된 코드가 벤치에서 돌아야 증명이 성립. 모드별 별도 구현 금지 |

#### 시연 서사 (ADR-001 연계)

"검증 모드에서 바닐라와 비트 일치를 증명한 그 코드가, 벤치 모드에서 N배로 돈다." 두 모드가 코드를 공유하므로(D3) 속도 주장과 정확성 주장이 하나의 구현에 대한 주장이 된다.

#### Anti-goals

- 검증 모드 전용 느린 참조 구현과 벤치 전용 구현의 분리 (D3 위반 — 증명이 무효화됨)
- 벤치 모드 결과의 스테이지 덤프 비교 (순서 비고정이므로 정의상 무의미, Tier 1 구간 제외)

#### Pitfalls

1. 스케줄러 정책 주입점이 코어 API에 있어야 한다: `hc_schedule_policy { FREE, REPLAY(manifest) }` 형태. CLI 플래그가 아니라 라이브러리 계약.
2. 벤치 숫자는 항상 FREE 모드로, 패리티 배지는 항상 REPLAY 모드로 표기. 혼용 보고는 신뢰도 자살. *(normative: [benchmarks-and-viz/spec.md](../benchmarks-and-viz/spec.md))*

#### References

- ADR-007 (2단 게이트 — [worldgen-parity/context.md](../worldgen-parity/context.md)), ADR-003 (스케줄러 소유 — [core-abi/context.md](../core-abi/context.md)), ADR-001 (시연 목적 — [project.md](../../project.md))
