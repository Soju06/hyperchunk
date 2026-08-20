# core-abi — Context

## Purpose & scope

[spec.md](spec.md)가 코어 경계·ABI의 normative SSOT다. 이 문서는 "왜 리전
단위 경계이고 왜 코어가 순수 계산 라이브러리인가"(ADR-003 ABI 절반)의 결정
역사를 담는다. ADR-003의 호환 절반(데이터팩 D4·fallback D5)은
[generation-pipeline/context.md](../generation-pipeline/context.md), P4 격리
경계(ADR-005)는 [project.md](../../project.md)에 있다.

## Current state (2026-08 기준)

- 코어는 `libhyperchunk.a` (C11, libc/libm/pthreads만). 소비자는 CLI
  (`hyperchunk-verify`, region 출력)와 벤치 드라이버(`hyperchunk-bench`).
  Fabric FFM 브릿지(Phase 3)는 미착수, Rust FFI는 계획 단계.
- ADR-003 당시의 JNI 선택은 ADR-006 D4가 FFM으로 교체했다 (Java 25에서 JEP
  454 정식) — 전문은 generation-pipeline/context.md의 ADR-006.

## Decision history

> append-only. 기존 항목은 수정하지 않는다. 결정이 바뀌면 새 항목을 추가하고
> 이전 항목을 `Superseded by`로 표시한다.

### ADR-003 (ABI 절반) — 코어는 리전 단위 C ABI를 노출하는 순수 계산 라이브러리다 (Phase 1, 2026-07-27)

**Status:** Decided
**Type:** Architecture / Contract
**전문 분할:** 호환 절반(D4 데이터팩 N-API, D5 청크 단위 fallback)과 그
인터페이스 분석은 [generation-pipeline/context.md](../generation-pipeline/context.md).
아래는 ABI·모듈 경계(D1~D3)에 해당하는 부분이다.

#### Context

ADR-002 논의 중 저자가 "밑바닥부터 C = JVM 밖 독립 바이너리"라고 결론지었다. **이는 오류였고 사용자가 정정했다.** 원문:

> "이게 사용성 측면에서 기존 버킷 호환 또는 플러그인 호환 레이어를 못만들면 사용성이 떨어지고 이식성이 좀 많이 깎이는게 단점일것 같은데 방법있을까?
> 예전에 내가 느꼈던 node -> bun으로만 바꿨는데 API 레이턴시가 30% 늘어나는 그런 경험을 주게 만들고싶어"

C로 작성한 코드를 Fabric 모드 내부에 `.so`로 탑재하는 것은 완전히 가능하다. 독립 바이너리는 선택지이지 강제사항이 아니다.

bun의 실제 전략(N-API ABI 구현 — 인터페이스만 구현하고 엔진을 교체)은 호환 절반에 기록했다. ABI 쪽 핵심은 경계 비용의 정량화다(JNI 빈 호출 ~22ns, 청크 생성 ~10ms 기준):

| 경계 입도 | 청크당 호출 | 비용 | 청크시간 대비 | 판정 |
|---|---|---|---|---|
| 청크 1개당 1회 | 1 | 0.00002ms | 0.00% | 무료 |
| 셀 컬럼당 1회 | 64 | 0.001ms | 0.01% | 무료 |
| 노이즈 샘플당 1회 | 1,400 | 0.031ms | 0.31% | 허용 |
| **density 노드당 1회** | **84,000** | **1.85ms** | **18.5%** | **치명적** |

사용자의 "bun으로 바꿨는데 30% 느려짐" 경험은 마지막 행에 해당한다. 경계 입도가 잘못되면 네이티브 전환이 오히려 손해다.

이후 사용자가 스코프를 확장했다:

> "청크 생성만 위임하는게 아니라, 배치 처리도 좀 java가 삐리한 것 같아서, 아예 월드 생성 관련 부분을 아예 우리가 모듈식으로 전부 관리하는게 나중에 있을 병목 요소를 많이 줄일 수 있을 것 같아"

이 진단을 정량 검증했다. 자바가 청크 하나당 생성하는 객체는 약 40,808개(DensityFunction 중간객체 11,200 / BlockState·Palette 참조 24,576 / BlockPos 5,000 등) ≈ 1.31 MB/청크다.

| 처리량 | 할당율 | |
|---|---|---|
| 50 chunk/s | 0.07 GB/s | 여유 |
| 500 chunk/s | 0.65 GB/s | 허용 |
| **5000 chunk/s** | **6.53 GB/s** | **G1 처리한계(1~2GB/s) 초과** |

즉 배치가 느린 근본 원인은 언어가 아니라 **청크당 수만 객체 할당**이다. 자바에서는 배치를 키우면 GC 압력이 선형 증가해 오히려 손해다. C에서 arena/SoA로 전환하면 할당이 0이 되어 **배치가 커질수록 이득이 커진다. 부호가 반대다.**

경계 입도를 리전으로 올리면 청크당 경계비용이 22ns → 0.02ns가 된다. 사용자 제안은 스코프 확대가 아니라 설계 개선이다.

마지막으로 결정적인 경쟁 지형이 확인됐다:

| 프로젝트 | 정체 | 약점 |
|---|---|---|
| Pumpkin (Rust) | 서버 전체 재구현 | **worldgen/saving 미완** |
| Valence (Rust) | Bevy ECS 프레임워크 | 바닐라 worldgen 없음 |
| Minestom (Java) | NMS 제거 프레임워크 | 바닐라 worldgen 없음(의도적) |
| Cuberite (C++) | 독자 서버 | 바닐라 패리티 없음 |
| C2ME | 청크젠 최적화 모드 | JVM 한계 내부 |

**네이티브 서버를 만드는 팀들이 전부 worldgen에서 막혀 있다.** 모듈식 C ABI로 독립하면 이들이 잠재 소비자가 되며, 우리 코어가 bun의 N-API 위치를 차지한다.

#### Decision (ABI 관련 3 결정; D4·D5는 generation-pipeline)

| # | 결정 | 핵심 |
|---|---|---|
| D1 | **코어는 순수 계산 라이브러리. 파일 I/O도 네트워크도 모른다** | 4개 소비자가 붙을 수 있고, 벤치가 깨끗하고, 상태 관리가 코어를 오염시키지 못한다 |
| D2 | **경계는 리전 단위로만 노출. 노드 단위 함수를 외부로 내보내지 않는다** | `hyperchunk_batch(seed, region_list, out)`. 노드 단위 노출은 18.5% 손실 |
| D3 | **arena/SoA 할당자와 배치 스케줄러를 코어가 소유** | 배치를 자바에 남기면 GC 벽에 걸려 성과가 드러나지 않는다 |

#### Why 리전 단위 경계?

D2가 사용자의 bun 열화 경험을 구조적으로 회피한다. "C 함수를 Java에서 호출한다"가 아니라 "리전을 통째로 넘기고 완성해서 받는다"는 구조여야 한다. 노드 단위로 노출하면 naive 구현이 자연스럽게 18.5% 손실 경로로 빠진다. 따라서 이것은 성능 팁이 아니라 **API 표면에서 강제하는 불변식**이다.

FFM(Panama)은 Java 22에서 확정됐고(JEP 454) MC 1.20.5+는 Java 21을 요구하므로 21에서는 preview다. 따라서 JNI로 간다. 리전 단위 입도에서는 FFM과 JNI의 성능 차이가 무의미하다. *(ADR-006 D4가 supersede: 타겟이 26.2/Java 25가 되면서 FFM 정식 — 브릿지는 FFM으로.)*

#### 모듈 경계

```
┌─ hyperchunk (순수 C, 의존성 0, C ABI) ──────────────┐
│  arena 할당자 / SoA 블록 스토리지                          │
│  density function 컴파일러 (AVX2 + AVX-512 런타임 디스패치) │
│  5 스테이지 파이프라인                                      │
│  배치 스케줄러 (체스판 스케줄링, features 충돌 회피)          │
│  API: hyperchunk_batch(seed, region_list, out)  ← 리전 단위  │
└────────────────────────────────────────────────────────┘
      ↑              ↑               ↑
  CLI(벤치/GIF)   JNI(Fabric)   Rust FFI (Pumpkin/Valence)
```

- **CLI**는 벤치·GIF·패리티 검증 전용. JVM 워밍업 논란을 차단하고 벤치 순수성을 보장한다
- **JNI/Fabric**은 실제 서버 사용성 *(ADR-006 D4 이후 FFM)*
- **Rust FFI**는 Phase 3 이후. 위 경쟁 지형이 수요를 보여준다
- Paper 플러그인 브릿지는 API가 더 얽혀 있어 Fabric 검증 후로 미룬다

CLI와 모드는 같은 정적 라이브러리를 두 번 링크하므로 추가 비용이 거의 없다.

#### Anti-goals (ABI 관련)

- 코어에 파일 I/O·네트워크·플레이어/엔티티 상태 넣기 (D1)
- density function 노드 단위 FFI 노출 (D2)
- 배치 스케줄링을 자바에 남기기 (D3)
- Paper 브릿지를 Phase 1에 포함

#### Pitfalls (ABI 관련)

1. **노드 단위 FFI 유혹.** 이식 초기에 "Java의 density function 노드를 하나씩 네이티브 호출"하는 것이 가장 쉬운 경로처럼 보인다. 그것이 18.5% 손실 경로다. 코어 헤더에 노드 단위 함수를 아예 선언하지 않아 컴파일 단계에서 막는다.
2. **arena 재사용 시 stale 데이터.** SoA 버퍼를 배치 간 재사용할 때 초기화 누락이 패리티 버그로 나타나고, 증상이 산발적이라 추적이 어렵다.
3. **Java 21 vs 22 런타임 혼선.** FFM 예제 코드를 그대로 쓰면 `--enable-preview` 없이 동작하지 않는다. JNI를 쓰기로 한 이유가 여기 있다. *(ADR-006 이후 Java 25 타겟이라 해당 없음 — 역사 기록으로 보존.)*

#### Verification (ABI 관련)

- `hyperchunk` 헤더에 리전 단위 진입점만 존재하고 노드 단위 함수가 없다
- 코어가 `libc` 외 의존성 없이 빌드된다 (`ldd` 확인)

#### When this might break

- 모드 생태계가 데이터팩보다 Java 코드 확장으로 회귀하는 경우 — fallback 비중이 커져 배수가 41배 아래로 내려간다 (호환 절반 참조)

#### References

- ADR-002 (재작성 방침 — [generation-pipeline/context.md](../generation-pipeline/context.md))
- https://openjdk.org/jeps/454 (FFM, Java 22)
- https://github.com/pumpkin-mc/pumpkin
- https://github.com/valence-rs/valence
- https://minestom.net/
