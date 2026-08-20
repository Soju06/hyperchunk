# generation-pipeline — Context

## Purpose & scope

[spec.md](spec.md)가 파이프라인 범위·버전 핀·데이터팩 호환의 normative SSOT다.
이 문서는 "왜 밑바닥부터 순수 C로 풀 파이프라인인가"(ADR-002), "왜 데이터팩
스키마가 호환 표면이고 fallback이 청크 단위인가"(ADR-003 호환 절반), "왜
26.2인가"(ADR-006)의 결정 역사를 담는다. ADR-003의 ABI 절반(리전 경계, 순수
계산 코어)은 [core-abi/context.md](../core-abi/context.md)에 있다.

## Current state (2026-08 기준)

- 11스테이지 전부 C로 구현 완료 (Phase 1), 풀 리전 canonical 일치. 스테이지별
  구현·검증 기록은 [changes/archive/](../../changes/archive/)의 task7(surface)
  ~task14(full region) 노트에 있다.
- 데이터팩 파서는 26.2 스키마(107.1) 기준 + 바닐라 fallback. Terralith급 외부
  데이터팩의 golden 대조는 아직 수행하지 않았다 (스펙의 Pure-JSON datapack
  시나리오는 계약 서술).

## Decision history

> append-only. 기존 항목은 수정하지 않는다. 결정이 바뀌면 새 항목을 추가하고
> 이전 항목을 `Superseded by`로 표시한다.

배치: ADR-002는 본 문서가 primary home (D3 패리티는
[worldgen-parity/spec.md](../worldgen-parity/spec.md), D1 언어 정체성은
[engineering-safety](../engineering-safety/spec.md), P1 FMA는
[simd-backends](../simd-backends/spec.md), P4 충돌 스케줄링은
[scheduler](../scheduler/spec.md)의 spec으로 반영). ADR-003은 호환 절반(D4·D5)만
여기, ABI 절반(D1~D3)은 core-abi. ADR-006은 본 문서가 primary home (D2 golden
환경은 worldgen-parity, D4 FFM은 core-abi의 spec으로 반영).

### ADR-002 — 밑바닥부터 순수 C로 재작성하고, 풀 파이프라인 비트 패리티를 유지한다 (Phase 1, 2026-07-27)

**Status:** Decided
**Type:** Architecture

#### Context

두 갈래가 있었다. (a) C2ME를 포크해 GPU density function 백엔드를 추가하는 안, (b) 밑바닥부터 C로 재작성하는 안.

(a)의 논거는 강했다. C2ME가 이미 해놓은 CPU 스레드 병렬화를 공짜로 얻고, 같은 CPU 코드 위에서 백엔드만 교체하므로 3-way 비교가 단일 변수 실험이 되며, GPL-3.0이라 포크 공개에 문제가 없다.

사용자가 (b)를 선택했다. 원문:

> "밑바닥부터 CPI 를 치밀하게 계산해서 짜는게 본질이라고 생각해"

이 선택을 정량 검증했다. 로컬 실측(AMD Ryzen 9 5900X, Zen 3, AVX2+FMA, AVX-512 없음) 기준:

- 코어당 이론 상한 16 flops/cycle (AVX2 double 4레인 × FMA 2유닛 × 2)
- 바닐라 density function 인터프리터 실효 0.03~0.08 flops/cycle (노드당 명령어 ~12개, CPI 3~8)
- **격차 96~256배**

원인은 언어가 아니라 자료구조다. 1.18부터 density function이 인터프리터 트리이며 노드마다 메가모픽 간접 호출이 발생해 JIT이 인라인하지 못한다. 대부분의 사이클을 분기 예측 실패와 간접 호출 대기로 소비한다.

로슈라인 분석으로 이 헤드룸이 실제로 회수 가능한지 확인했다:

- 청크당 노이즈 4,200,000 flops / 출력 196,608 bytes → **arithmetic intensity 21.4 flops/byte**
- 12코어 동시 기준 릿지포인트 7.0 flops/byte
- 21.4 ≫ 7.0 → **COMPUTE bound**

memory bound였다면 CPI 최적화가 헛수고였겠지만, compute bound이므로 IPC 개선이 벽시계 시간에 직접 반영된다. 사용자 접근의 전제조건이 실측으로 통과했다.

또한 공격축별 총 배수를 계산해 CPI 재작성이 Amdahl에 걸리지 않음을 확인했다. 스테이지 비중 추정 `noise 45% / surface 8% / carvers 10% / features 22% / lighting 15%` 기준:

| 공격축 | 총 배수 | Amdahl |
|---|---|---|
| GPU 오프로드만 (noise+surface+lighting) | 2.9x | 손 안 댄 32~55%에 갇힘 |
| 스레드 병렬만 (C2ME급) | 9.8x | features 충돌로 부분적 |
| **CPI 밑바닥 재작성 (단일코어)** | **5.6x** | **적용 안 됨 (`1-p` = 0)** |
| **CPI 재작성 + 12코어** | **47.5x** | 적용 안 됨 |

Amdahl은 손대지 않은 부분이 남을 때만 성립한다. GPU 오프로드는 noise만 건드려 병목을 features/carvers로 이동시킬 뿐이다(오프로드 후 잔여 비중 features 63% / carvers 29%). CPI 재작성은 전 스테이지를 건드리므로 가속 불가 구간이 존재하지 않는다.

풀 파이프라인 여부도 사용자가 지정했다("풀 파이프라인으로 하고싶은데"). 이 결정을 검증하는 과정에서 **직전 분석의 오류 두 개를 정정했다**:

1. Amdahl 3.13배 상한은 "우리 vs C2ME" 구간에만 걸린다. GIF의 좌측 화면은 바닐라이고 바닐라는 청크젠에 코어를 거의 쓰지 않으므로, 우리가 얻는 배수는 `(CPU 병렬 이득) × (GPU/CPI 이득)`의 곱이다. 12코어 기준 vs 바닐라 47~75배.
2. Amdahl은 레이턴시 법칙이고 GIF가 보여주는 것은 스루풋이다. 배치 파이프라이닝으로 총 시간이 `max(GPU, CPU)`가 되어 천장이 Amdahl이 아니라 하드웨어가 된다.

#### Decision (4 핵심 결정)

| # | 결정 | 핵심 |
|---|---|---|
| D1 | **밑바닥부터 순수 C로 재작성** | C2ME 포크 아님. 인터프리터 트리를 그대로 옮기면 2배로 끝나므로 자료구조부터 재설계 |
| D2 | **풀 파이프라인 (noise, surface, carvers, features, lighting 전부)** | terrain-only로 자르지 않는다. GIF의 정직성이 여기 달려 있다 |
| D3 | **바닐라와 비트단위 동일 출력 필수** | region 파일 `sha256` 일치가 수용 기준 *(ADR-007이 canonical hash + 2단 게이트로 정밀화)* |
| D4 | **범위 3분할로 작업량 통제** | 1.21.x 단일 패치 / 오버월드만 / 구조물은 배치까지 (직소 조립 제외) *(버전 값은 ADR-006이 26.2로 교체)* |

#### Why 밑바닥 재작성 over C2ME 포크?

(a)가 ROI 최적해였음을 기록해 둔다. (b)를 택한 근거는 ADR-001 D1이다. 목적이 기술 시연이므로 "SoTA 위에 얹어 3배"보다 "전부 다시 써서 47배"가 목적에 부합한다. 또한 D1은 C2ME의 GPL-3.0 전파를 피해 라이선스를 자유롭게 선택할 수 있게 한다.

대가는 정직하게 기록한다. features/carvers/structures/lighting 전부를 C로 재구현해야 한다. cubiomes는 바이옴과 구조물 위치만 다뤘고 그것도 큰 프로젝트였다. **풀 바닐라 패리티 C 재구현은 선례가 없다.** 그것이 flex의 크기이자 리스크의 크기다.

#### Why 비트 패리티 필수?

1. 그것이 flex의 전부다. "빠른 지형 생성기"는 흔하고 "너희 월드를 비트단위 동일하게 47배 빠르게 재생성"은 선례가 없다.
2. 검증이 공짜로 딸려온다. 바닐라 region과 우리 출력의 `sha256` 비교로 끝난다. 그 결과가 어떤 대시보드보다 강력하다.
3. 패리티가 없으면 GIF 자체가 무의미하다. 서로 다른 알고리즘의 속도 비교는 벤치마크가 아니다.

#### Anti-goals

- terrain-only 벤치마크 (D2가 배제)
- 버전 매트릭스 지원 (D4)
- 네더/엔드 (D4)
- 직소 구조물 조립 — 마을 내부 방 배치는 별개 난이도이며 Phase 1 범위 밖 (D4)
- float으로 다운캐스트한 근사 생성 — 성능은 오르지만 D3와 양립 불가

#### Pitfalls

1. **FMA contraction이 조용히 지형을 바꾼다.** `a*b+c`를 FMA 한 명령으로 접으면 중간 반올림이 사라져 결과 비트가 달라진다. 바닐라는 mul → round → add 순서다. CPU 커널은 FMA 금지, GPU 코드가 생기면 `-fmad=false` 필수. 이 버그는 증상이 "지형이 미묘하게 다름"뿐이라 디버깅 비용이 극단적으로 높다. *(normative: [simd-backends/spec.md](../simd-backends/spec.md))*
2. **RNG 소비 순서까지 재현해야 한다.** LCG와 Xoroshiro128++의 호출 순서가 바뀌면 features 배치가 달라진다. *(normative: [worldgen-parity/spec.md](../worldgen-parity/spec.md))*
3. **바닐라 노이즈는 double이다.** float으로 낮추는 최적화는 D3 위반이다.
4. **features는 청크 경계를 넘어 이웃 청크에 write한다.** 인접 청크를 동시 처리하면 충돌하고, 처리 순서가 바뀌면 RNG 소비 순서가 달라진다. 체스판/스트라이프 스케줄링으로 충돌 없는 청크 집합만 배치에 넣어야 한다. 이것이 GPU 커널보다 어렵고 C2ME가 수년간 씨름한 부분이다. *(normative: [scheduler/spec.md](../scheduler/spec.md))*
5. **벤치 호스트 신뢰성.** 현재 개발 박스는 `lscpu`가 `Core(s) per socket: 22`, `Thread(s) per core: 1`, `L3 cache: 352 MiB (22 instances)`를 보고한다. 실제 5900X는 12코어/24스레드, L3 64MB이며 `hypervisor` 플래그가 있다. 즉 SMT·캐시 토폴로지가 오보고되는 VM이다. **이 박스의 사이클 카운트는 근거로 쓸 수 없다.** 공개 벤치는 베어메탈 또는 코어 핀닝된 전용 인스턴스에서 재측정해야 한다. *(이후 B-4에서 hc-e6 무대 적격성 실측, B-6에서 공개 재실측 — [benchmarks-and-viz/context.md](../benchmarks-and-viz/context.md))*

#### Verification

- `hyperchunk-verify --seed <s> --region <x> <z>` 가 바닐라 golden region과 `sha256` 일치를 출력
- 스테이지 비중 추정치(`noise 45%` 등)를 실측 프로파일로 교체하고 본 ADR의 계산을 재확인
- FMA 금지가 컴파일 플래그로 강제되고 CI가 회귀를 잡는다

#### When this might break

- Mojang이 월드젠 알고리즘을 대규모 변경하는 경우 (D4의 단일 패치 고정이 방어선)
- 실측 프로파일이 추정 비중과 크게 다를 경우 — 특히 features 비중이 22%보다 훨씬 크면 우선순위 재조정 필요

#### References

- ADR-001 (목적 — [project.md](../../project.md))
- https://github.com/RelativityMC/C2ME-fabric
- https://github.com/Cubitect/cubiomes

### ADR-003 (호환 절반) — 호환은 데이터팩 스키마 + 바닐라 fallback으로 얻는다 (Phase 1, 2026-07-27)

**Status:** Decided
**Type:** Architecture / Contract
**전문 분할:** ABI 절반(D1~D3: 순수 계산 코어, 리전 경계, arena/스케줄러 소유)과
Context의 경계 비용·배치 분석은 [core-abi/context.md](../core-abi/context.md).
아래는 호환 계약(D4·D5)에 해당하는 부분이다.

#### Context (호환 인터페이스 분석)

bun은 npm 생태계를 재구현하지 않고 Node의 *인터페이스*를 구현했다: `node:` 내장 모듈 API 표면과 **N-API ABI**(네이티브 애드온이 재컴파일 없이 로드된다). 패키지는 손대지 않고 엔진만 교체한다.

MC 월드젠의 대응 인터페이스는 셋이다:

| 인터페이스 | 정체 | 네이티브 처리 가능성 |
|---|---|---|
| 데이터팩 worldgen JSON (`noise_settings`, `density_function`, `placed_feature`) | 데이터 | 가능. 스키마 구현으로 Terralith/Tectonic이 자동 호환 |
| Java 코드로 등록된 Feature/StructurePiece | JVM 바이트코드 | 원리적으로 불가 |
| Bukkit/Paper `ChunkGenerator`, `BlockPopulator` | JVM 인터페이스 | 브릿지 가능 |

MC가 1.18부터 월드젠을 데이터 주도로 만들어 둔 것이 우리에게 N-API 역할을 한다.

#### Decision (호환 관련 2 결정; D1~D3은 core-abi)

| # | 결정 | 핵심 |
|---|---|---|
| D4 | **데이터팩 worldgen JSON을 우리의 N-API로 취급** | 스키마 구현으로 Terralith/Tectonic급 월드젠이 자동 호환 |
| D5 | **알 수 없는 확장을 만나면 해당 청크를 통째로 바닐라 경로로 fallback** | 추측하지 않는다. 정확성이 속도보다 우선 |

#### Why fallback? — drop-in의 정의

drop-in은 "항상 빠름"이 아니라 **"항상 동작함 + 대부분 빠름"**이다. bun도 호환되지 않는 패키지가 있지만 느려질 뿐 동작한다.

features 네이티브 커버리지에 따른 실효 배수(12코어):

| 시나리오 | features 커버 | ×12코어 |
|---|---|---|
| 바닐라만 | 100% | 75x |
| 데이터팩 월드젠 (Terralith 등) | 100% | 75x |
| 경량 모드 (20% 자바) | 80% | 62x |
| 중형 모드팩 (50% 자바) | 50% | 49x |
| 헤비 모드팩 (80% 자바) | 20% | **41x** |

features가 거의 전부 자바로 남아도 41배를 유지한다. features는 전체의 22%뿐이고 **noise/surface/carvers/lighting 78%는 어떤 모드팩에서도 바닐라 알고리즘**이므로 항상 네이티브로 처리된다. 즉 호환성 손실이 "동작 불가"가 아니라 우아한 열화로 나타난다.

#### Anti-goals (호환 관련)

- Java Feature를 네이티브에서 에뮬레이션하려는 시도 — 원리적으로 불가하며 D5가 정답

#### Pitfalls (호환 관련)

- **fallback 판정을 청크보다 작은 단위로 하려는 유혹.** 부분 fallback은 RNG 소비 순서를 깨뜨린다. 판정 단위는 청크 전체다.

#### Verification (호환 관련)

- Terralith 데이터팩을 입력했을 때 바닐라 대조와 `sha256` 일치
- 인위적으로 알 수 없는 Feature를 등록했을 때 해당 청크가 바닐라 경로로 처리되고 결과가 여전히 패리티를 만족

#### When this might break

- Mojang이 데이터팩 worldgen 스키마를 비호환 변경하는 경우 (ADR-002 D4의 단일 패치 고정이 방어선)
- 모드 생태계가 데이터팩보다 Java 코드 확장으로 회귀하는 경우 — fallback 비중이 커져 배수가 41배 아래로 내려간다

#### References

- ADR-002 (재작성 방침 — 본 문서 위)
- https://minecraft.wiki/w/Data_pack

### ADR-006 — 타겟 버전을 26.2로 변경하고, 비난독화 소스와 FFM을 활용한다 (Phase 1, 2026-07-28)

**Status:** Decided
**Type:** Scope / Contract
**Resolves:** ADR-002 D4의 버전 핀, ADR-003의 JNI 선택을 부분 supersede

#### Context

사용자 directive: "일단 최초 버전 타겟은 26.2로 하자"

ADR-002 D4는 "1.21.x 단일 패치"를 가정했으나, Mojang이 2026년부터 연도 기반 버저닝(year.drop.hotfix)으로 전환했다. 릴리스 계보: 1.21.11 → 26.1 "Tiny Takeover" (2026-03-24) → 26.1.1/26.1.2 → **26.2 "Chaos Cubed" (2026-06-16, 현재 latest release)**. 버전 매니페스트에서 실존 확인함 (protocol 776, data version 4903, data pack format 107.1).

조사에서 확인된 파급효과 세 가지:

1. **Java 25 필수.** 26.1부터 Minimum Java version = Java SE 25. 플랜의 "JDK 21 설치" 지시는 무효.
2. **26.1부터 완전 비난독화.** 26.1은 "the first to be fully unobfuscated without an accompanying obfuscated variant". Fabric도 이에 따라 Yarn 매핑 지원을 중단하고 Mojang 공식 이름으로 전환했다. 매핑 레이어가 사라져 스테이지 덤프 하네스(mixin)를 실명 클래스로 직접 작성한다.
3. **26.2 worldgen은 1.21.x와 다르다.** sulfur caves 케이브 바이옴과 sulfur/cinnabar 블록이 추가되어 재현해야 할 표면이 늘었다. cubiomes 등 기존 레퍼런스 구현은 26.x를 지원하지 않을 가능성이 높다. 대신 비난독화 소스가 알고리즘 확인 비용을 크게 낮춰 이를 상쇄한다.

#### Decision (4 핵심 결정)

| # | 결정 | 핵심 |
|---|---|---|
| D1 | **타겟 버전 = 26.2 고정** | TARGET_VERSION=26.2. ADR-002 D4의 "단일 패치 고정" 원칙은 유지, 값만 교체 |
| D2 | **golden 생성 환경은 JDK 25** | 26.x 서버 구동 요건 *(normative: [worldgen-parity/spec.md](../worldgen-parity/spec.md))* |
| D3 | **매핑 레이어 폐기, Mojang 실명 직접 사용** | Yarn 사망. 비난독화 jar 기준으로 mixin/리플렉션 작성 |
| D4 | **Phase 3 브릿지는 JNI 대신 FFM** | Java 25에서 FFM(JEP 454)이 정식. ADR-003의 "Java 21 preview라서 JNI" 논거 소멸. 경계 입도 불변식(리전 단위)은 그대로 *(normative: [core-abi/spec.md](../core-abi/spec.md))* |

#### Why 26.2 over 1.21.x?

최신 안정판이 26.2이므로 "현재의 바닐라"와 비교하는 것이 시연 목적(ADR-001)에 부합한다. 1.21.x 대비 벤치는 출시 시점에 이미 구버전 비교가 된다.

#### Anti-goals

- 26.1/26.3-snapshot 동시 지원 (단일 패치 원칙)
- 1.21.x 하위 호환
- 디컴파일/비난독화 소스 코드의 복사 — 이름 참조와 알고리즘 이해는 자유로워졌지만 코드 복사는 여전히 저작권 문제 (ADR-002 R4 원칙 유지)

#### Pitfalls

1. **sulfur caves는 carvers/features/biomes 재구현 범위에 포함된다.** 1.21 기준 자료(위키 문서 다수)가 26.2 현실과 다를 수 있으므로, 항상 비난독화 소스와 26.2 추출 JSON을 1차 근거로 삼는다.
2. **cubiomes 참조 시 버전 주의.** 26.x 미지원 알고리즘을 그대로 믿으면 패리티가 조용히 깨진다.
3. **data pack format 107.1.** Task 12 데이터팩 스키마 파서는 26.2 스키마 기준이다.
4. **오버월드 y 범위(-64..319) 유지 여부 미확인.** Task 2 golden 덤프에서 실측으로 확정하고 hyperchunk.h 상수를 그에 맞춘다.

#### Verification

- TARGET_VERSION 파일 내용 = 26.2
- golden 서버가 JDK 25에서 구동되고 region 생성 확인
- 스테이지 덤프 하네스가 매핑 도구 없이 빌드됨

#### References

- ADR-002 (본 문서 위), ADR-003 ([core-abi/context.md](../core-abi/context.md))
- https://minecraft.wiki/w/Java_Edition_26.2
- https://minecraft.wiki/w/Java_Edition_26.1 (비난독화, Java 25 요건)
- https://docs.fabricmc.net/develop/porting/ (Yarn 중단, Mojang mappings 전환)
