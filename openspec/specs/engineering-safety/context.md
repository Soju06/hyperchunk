# engineering-safety — Context

## Purpose & scope

[spec.md](spec.md)가 언어 정체성·안전망·게이트 체인의 normative SSOT다. 이
문서는 "왜 Rust가 아니라 C11 + sanitizer인가"(ADR-009)의 결정 역사를 담는다.

## Current state (2026-08 기준)

- sanitizer 게이트는 상시 운용 (`check_sanitizers.sh` ASan+UBSan,
  `check_tsan.sh` TSan — TSan은 `setarch -R`로 ASLR을 꺼야 안정). P2-1에서
  게이트 자체가 항상-FAIL로 방치돼 있던 것을 복구한 이력이 있다
  ([changes/archive/](../../changes/archive/) phase2 P2-1 노트) — 게이트는
  "존재"가 아니라 "green 이력"으로 신뢰한다.
- ASan fake-stack 32B 정렬 함정(P2-10), PT_TLS 8.6MB vs 8MB 스택 예산으로 큰
  `_Thread_local`이 스폰 즉사를 만든 사례(P2-8~9)도 phase2 노트에 기록.
- CI(25-테스트 서브셋)는 GH-1에서 실측 기반으로 구성 — `-E` 정규식은 신규
  golden 테스트를 트래킹하지 않으므로 테스트 추가 시 수동 갱신 필요
  (gh-setup 노트).

## Decision history

> append-only. 기존 항목은 수정하지 않는다. 결정이 바뀌면 새 항목을 추가하고
> 이전 항목을 `Superseded by`로 표시한다.

배치: ADR-009는 본 문서가 primary home. 재확인 대상인 ADR-002 D1(순수 C
재작성)의 전문은
[generation-pipeline/context.md](../generation-pipeline/context.md).

### ADR-009 — 구현 언어는 순수 C11을 유지하고, Rust의 안전망은 sanitizer 게이트로 대체 수입한다 (Phase 1, 2026-07-28)

**Status:** Decided
**Type:** Architecture / Identity
**Resolves:** ADR-002 D1 재확인 (Rust 전환 검토 후 기각)

#### Context

Task 4(arena/SoA) 착수 직전, 사용자가 언어 재검토를 제기했다: "c로 메모리 관리까지 빡세게해서 최적화 할지, rust로 약간 물을 탈지 결정해보자". Task 4 이전이 전환 가능한 마지막 저비용 지점(기존 C 코드 475줄)이었다.

성능 축 조사 결과는 무승부: 코드젠 상한 동일(core::arch에 AVX2/AVX-512 intrinsics, AVX-512는 1.89 안정화), bounds check 실무 오버헤드 ~0, arena/SoA는 Rust에서도 인덱스 기반이 정석. FP 패리티는 rustc가 FMA contraction을 기본 비활성(RFC 3514)이라 소폭 우위지만 C도 -ffp-contract=off로 동률(이미 적용).

실질 결정 축은 넷이었다:
1. **위임 개발 리스크 (Rust 우위)**: LLM이 짜는 C의 silent OOB는 크래시 없이 지형을 오염시켜 "가짜 패리티 버그"를 만든다. 패리티 디버깅이 이 프로젝트의 최고 비용 자원.
2. **features 병렬화 (표면상 Rust, 실질 애매)**: 3x3 이웃 동시 mutation은 borrow checker 최악 상성이라 결국 unsafe. 충돌은 체스판 스케줄링으로 구조적 회피(ADR-003)하므로 언어 보증 가치 낮음.
3. **flex 서사 (C 우위)**: ADR-001의 제품 정의가 기술 시연이고, ADR-002에 사용자 원문 "밑바닥부터 CPI를 치밀하게 계산해서 짜는게 본질"이 있다. "pure C11, zero dependencies"가 FOMO를 만든다.
4. **소비자 생태계 (무승부)**: C ABI는 어느 쪽이든 노출 가능.

사용자 확정 원문:

> "a 레츠고 아주 그냥 클럭단위로 예술을 만들어보자"

((a) = "pure C11, zero dependencies, hand-tuned to the cycle" 정체성)

#### Decision (3 핵심 결정)

| # | 결정 | 핵심 |
|---|---|---|
| D1 | **순수 C11 유지** | ADR-002 D1 재확인. 프로젝트 정체성 = hand-tuned C |
| D2 | **sanitizer 게이트 도입** | ASan+UBSan 프리셋으로 전체 ctest 추가 실행을 표준 게이트화. Phase 2(멀티스레드)부터 TSan 추가. Rust 안전망(축 1)의 실효 대체 |
| D3 | **debug 빌드 bounds assert** | hc_idx 등 접근자에 debug 한정 assert (release zero-cost) |

#### Anti-goals

- Rust 전환 (축 1 우위가 D2/D3 + 스테이지별 golden 게이트로 상쇄됨)
- C 커널 + Rust 셸 하이브리드 (툴체인 2개, 내부 FFI, 서사 희석 — 최악 조합)
- unsafe 회피 목적의 성능 타협

#### Pitfalls

1. sanitizer는 실행된 경로만 검사한다. 골든 테스트 커버리지가 sanitizer 실효성의 상한.
2. ASan 빌드는 FMA 게이트 대상이 아니다 (계측 코드가 섞임). FMA 게이트는 release 아카이브에만.
3. TSan은 Phase 2 진입 시점에 반드시 추가 — 잊으면 features 병렬화 데이터레이스가 무방비. *(P2-3에서 추가 완료.)*

#### Verification

- CMake preset asan-ubsan 존재, ctest 전체가 sanitizer 하에서 PASS
- release 빌드 assert 비활성 확인 (objdump에 abort 경로 없음)

#### References

- ADR-001 (FOMO 목적 — [project.md](../../project.md)), ADR-002 (C 재작성 + CPI 본질 — [generation-pipeline/context.md](../generation-pipeline/context.md)), ADR-003 (스케줄링 구조 회피 — [core-abi/context.md](../core-abi/context.md))
- https://rust-lang.github.io/rfcs/3514-float-semantics.html
- https://github.com/rust-lang/rust/issues/111137 (AVX-512 intrinsics)
