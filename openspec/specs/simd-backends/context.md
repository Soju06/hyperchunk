# simd-backends — Context

## Purpose & scope

[spec.md](spec.md)가 백엔드 구성·바이트동일·FMA 금지의 normative SSOT다. 이
문서는 "왜 AVX2가 기본이고 AVX-512가 디스패치인가, 왜 AVX-512의 가치가 폭이
아니라 레지스터인가"(ADR-004)의 결정 역사와 이후 실측을 담는다.

## Current state (2026-08 기준)

- AVX2 lazy 커널(P2-4)로 noise 2.97x, AVX-512 백엔드(P2-10, hc-e6 실측)로
  추가 1.176x(noise wall), 2-웨이 인터리브(P2-11)로 커널 1.29–1.40x — 상세는
  [changes/archive/](../../changes/archive/)의 phase2 노트. ADR-004가 보수
  재추정한 6.5x 커널 배수는 실측에서 1.55x에 그쳤다 (P2-10 노트에 원인 분석:
  룩업이 아니라 FP 체인이 지배).
- 공개 벤치(hc-e6, Zen5)는 AVX-512 백엔드로 돌았다 — 배율의 무대귀속 규칙은
  [benchmarks-and-viz/spec.md](../benchmarks-and-viz/spec.md).

## Decision history

> append-only. 기존 항목은 수정하지 않는다. 결정이 바뀌면 새 항목을 추가하고
> 이전 항목을 `Superseded by`로 표시한다.

배치: ADR-004는 본 문서가 primary home. FMA 금지의 출처인 ADR-002 P1은
[generation-pipeline/context.md](../generation-pipeline/context.md)에 있고,
본 capability spec.md의 요구사항으로 반영되어 있다.

### ADR-004 — AVX2를 기본 커널로 삼고 AVX-512는 런타임 디스패치로 추가한다 (Phase 2, 2026-07-27)

**Status:** Decided
**Type:** Architecture / Performance strategy
**Phase:** 2 (Phase 1에서는 스칼라 참조 커널만 구현)

#### Context

AVX-512 도입 효과를 질의받아 정량 분석했다. 결론은 직관과 반대였다.

**하드웨어 사실(확인됨):**

- **Zen4의 AVX-512는 256-bit 유닛으로 double-pump한다.** 512-bit 명령이 유닛을 2사이클 점유하므로 FP 처리량 이득이 사실상 0이고, 이득은 명령어 수 절감에서 오는 프론트엔드 절약뿐이다. 스로틀링은 없다.
- **Zen5부터 데이터패스가 512-bit로 확장되어 진짜 2배가 된다.**
- **Intel은 Alder Lake 이후 컨슈머 칩에서 AVX-512를 실리콘 단계에서 fuse off했다.** Sapphire Rapids급 서버 칩만 지원한다.

FMA 금지(ADR-002 Pitfall 1) 상태 코어당 double peak:

| CPU | 명목 | 실효 보정 | 실제 |
|---|---|---|---|
| Zen3 AVX2 (로컬) | 8 flops/cyc | 1.00 | 8.0 |
| Zen4 AVX-512 | 16 | **0.50** | **8.0** |
| Zen5 AVX-512 | 16 | 1.00 | **16.0** |

**즉 AVX-512의 가치는 "폭"이 아니다.** 분해하면(noise 스테이지 한정, AVX2 최적화 커널 = 1.0):

| 요소 | Zen5 | Zen4 | 정체 |
|---|---|---|---|
| 레인 폭 4→8 | 2.00x | **0.50x** | Zen4는 double-pump로 반납 |
| **zmm 레지스터 32개** | **3.00x** | **3.00x** | 주 기여 요소 |
| `vpermt2pd` gradient 룩업 | 1.35x | 1.35x | 20 uop → 3 uop |
| mask 기반 분기 제거 | 1.10x | 1.10x | branchless |
| 누적 | 8.9x | 2.2x | |
| 보수적 재추정 | **6.5x** | 1.6x | |

레지스터가 폭보다 중요한 이유가 FMA 금지와 직결된다:

```
FP add 지연 3cyc × 포트 2개 → add 체인 6개 필요
FP mul 지연 3cyc × 포트 2개 → mul 체인 6개 필요
포트 포화에 필요한 독립 체인: 12개
```

FMA를 쓸 수 있으면 mul+add가 한 명령이라 체인 수요가 절반이다. 패리티 때문에 FMA를 금지하므로 **수요가 2배로 증가한다.**

| ISA | 테이블 상주 | 가용 레지스터 | 수용 체인 | 포트 가동률 |
|---|---|---|---|---|
| AVX2 (ymm 16) | 4 | 8 | 4/12 | **33%** (L1 스필 발생) |
| AVX-512 (zmm 32) | 2 | 26 | 13/12 | **100%** |

**AVX-512는 FMA 포기로 생긴 손실을 정확히 보상하는 도구다.** 이것이 일반적인 SIMD 조언과 다른 이 프로젝트 고유의 논거다.

`vpermt2pd`는 구조적으로 잘 맞는다. 바닐라 `ImprovedNoise`는 `hash & 15`로 16방향 gradient를 선택하며, 16 double = zmm 정확히 2개다. 테이블 전체가 레지스터에 상주해 룩업이 3 uop로 끝난다(AVX2 blend 트리 20 uop, Zen3 `vgatherqpd`는 마이크로코드라 ~36 cyc로 최악).

레인 채우기는 GPU와 달리 문제가 없다. 청크당 노이즈 샘플 1400개면 8-wide로 175회 반복, 낭비 레인 0이다. GPU는 SM을 채우려 수천 청크 배칭이 필요했다.

**그러나 풀 파이프라인에 얹으면 효과가 축소된다:**

| 구성 | 단일코어 | AVX2 대비 |
|---|---|---|
| AVX2 밑바닥 재작성 | 5.58x | — |
| Zen4 AVX-512 | 6.27x | +12% |
| Zen5 AVX-512 | 7.11x | **+27%** |
| *noise+surface 무한 가속* | *7.79x* | *천장* |

CPI 재작성 후 병목 비중이 `noise 21% / features 41%`로 이동하므로 noise를 더 파도 총합 기여가 작다. 12코어 기준 `59x → 66x → 75x`이며, **3-way GIF에서 59배와 75배는 시각적으로 구분되지 않는다.**

#### Decision (4 핵심 결정)

| # | 결정 | 핵심 |
|---|---|---|
| D1 | **AVX2를 기본 커널로 삼는다** | Haswell(2013) 이후 보편적. AVX-512 전용은 관객 대부분이 실행조차 못 한다 |
| D2 | **AVX-512는 `cpuid` 런타임 디스패치로 추가** | 버리지 않는다. 두 백엔드 공존 |
| D3 | **FMA는 전 백엔드에서 금지** | 컴파일 플래그로 강제. 패리티 불변식 |
| D4 | **모든 백엔드의 출력 바이트 동일성을 CI 게이트로 강제** | 선택이 아니라 필수 |

#### Why AVX2 먼저?

재현 불가능한 벤치는 기술 관객에게 즉사 요인이다. AVX-512는 Zen5 또는 Intel 서버 칩에만 있어 대부분의 관객이 검증할 수 없다. 반면 두 백엔드를 함께 두면 flex가 하나 늘어난다: **"AVX2/AVX-512 두 백엔드, 출력 해시 동일"** 이 패리티 엄밀성의 증거로 기능한다.

#### Anti-goals

- AVX-512 전용 빌드 (D1)
- FMA 활성화로 2배 얻기 (D3, ADR-002 D3 위반)
- noise 스테이지 추가 최적화를 features보다 우선하기 — 위 표가 features 우선을 지시한다
- GPU(CUDA) 백엔드 — noise 오프로드만으로는 총 2.9배이고 병목이 features로 이동할 뿐. 배치 pregen 서비스가 아니면 ROI가 없으며 ADR-001이 그 방향을 배제했다

#### Pitfalls

1. **SIMD 레벨 간 부동소수점 결과 불일치.** FastNoise2 FAQ도 지적하는 알려진 문제다. 우리는 비트 패리티가 제품의 본질이므로 D4의 CI 게이트가 필수다.
2. **Zen4를 Zen5로 오인한 하드웨어 구매.** double-pump 때문에 Zen4의 AVX-512 FP 이득은 0에 가깝다.
3. **AVX-512 다운클럭 가정.** Skylake-X 시절 통념이며 Zen4/Zen5에는 고정 주파수 오프셋이 없다. 오히려 512-bit가 256-bit보다 약간 높은 클럭으로 관측된 사례가 있다.
4. **로컬 개발 박스에 AVX-512가 없다.** 5900X는 Zen3다. AVX-512 경로는 로컬에서 실측 불가이며 별도 하드웨어가 필요하다. *(이후 hc-e6(Zen5)에서 실측 — P2-10 노트.)*

#### Verification

- `hyperchunk-bench --isa=scalar|avx2|avx512` 세 경로의 출력 `sha256`이 모두 동일
- `objdump`로 FMA 명령(`vfmadd*`)이 산출물에 존재하지 않음을 확인
- `cpuid` 디스패치가 AVX-512 없는 호스트에서 AVX2로 정상 폴백

#### When this might break

- Zen5/AVX-512 하드웨어가 보편화되어 D1의 이식성 논거가 약해지는 경우
- 실측 프로파일에서 noise 비중이 45%보다 훨씬 크게 나오는 경우 — 그때는 AVX-512 우선순위를 올려야 한다

#### References

- ADR-002 (FMA 금지의 출처 — [generation-pipeline/context.md](../generation-pipeline/context.md))
- https://www.numberworld.org/blogs/2024_8_7_zen5_avx512_teardown/
- https://chipsandcheese.com/p/zen-5s-avx-512-frequency-behavior
- https://github.com/Auburn/FastNoise2/wiki/FAQ
- https://www.felixcloutier.com/x86/vpermt2w:vpermt2d:vpermt2q:vpermt2ps:vpermt2pd
