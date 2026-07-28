# DECISIONS — hyperchunk

바닐라 Minecraft Java Edition 월드젠을 순수 C로 비트단위 동일하게 재구현하는 프로젝트의 결정 기록.

Append-only. 기존 ADR은 수정하지 않고, 결정이 바뀌면 새 ADR을 추가하고 이전 것을 `Superseded by`로 표시한다.

---

## ADR-001 — 프로젝트 목적은 ROI가 아니라 기술 시연이며, 산출물은 3-way 탑뷰 비교 GIF다 (Phase 0, 2026-07-27)

**Status:** Decided
**Type:** Product direction
**Plan:** [.hermes/plans/2026-07-27_phase1-vertical-slice.md](.hermes/plans/2026-07-27_phase1-vertical-slice.md)

### Context

프로젝트는 "청크 생성 SaaS"로 출발했다. 초기 조사에서 SaaS 형태를 죽이는 벽 세 개가 확인됐다.

1. **법적 벽.** Minecraft Usage Guidelines는 열거주의다. 원문: "Do **not** make commercial use or commercially exploit anything that we have made unless these guidelines say it's okay" / "If something isn't covered by these guidelines and we haven't otherwise said it's okay, that probably means we don't want you to do it". 허용 목록은 영상·스트림, 서버 운영, 출판물, 핸드크래프트(연 $5,000 상한)뿐이고 "월드젠 출력물 판매"는 열거되지 않았다.
2. **공짜의 벽.** Chunky가 무료로 pregen을 수행한다. 고객은 이미 유휴 CPU를 임대 중이고, pregen은 일회성 비용이라 구독이 붙지 않는다. MC 호스팅 시장가는 $1/GB 수준으로 가격 민감도가 극단적이다.
3. **커모디티 벽.** 시드맵 레이어(Chunkbase, seeds.gg, seedlander, mcseedmap, seedmap.app, cubiomes.com)는 전부 무료 광고 모델이며 SaaS가 아니라 SEO/트래픽 시장이다.

이 시점에 사용자가 목적을 명시했다. 원문 그대로:

> "솔직히 그냥 ROI 개무시한 괴물같은 기술력 자랑이야, 사람들 FOMO오게"

이 directive가 벽 2와 벽 3을 무효화했다(무료 배포이므로 공짜 대안 및 광고 시장과 경쟁하지 않는다). 벽 1은 남는다. 같은 문서가 commercial use를 이렇게 정의하기 때문이다: "commercial use means any uses of our name, brand, or assets that you use and share with others (**regardless of whether you receive payment or provide it for free**)".

산출물 형태 역시 사용자가 지정했다:

> "java 기본 vs 가장 최적화 잘됬다는 버킷/플러그인 vs 우리 / 이거 청크 탑뷰로 실시간 생성이 얼마나 빠른지 gif같은 숏폼으로 보여주기만 해도 충분할 듯"

### Decision (5 핵심 결정)

| # | 결정 | 핵심 |
|---|---|---|
| D1 | **ROI 최적화를 명시적 anti-goal로 선언** | 수익 모델·과금·고객 획득은 설계 입력이 아니다 |
| D2 | **산출물은 3-way 탑뷰 실시간 생성 비교 숏폼** | `바닐라 Java` vs `Paper + C2ME + Chunky` vs `hyperchunk` |
| D3 | **무료 공개 (OSS)** | 벽 2·3을 회피하고 재현성을 확보 |
| D4 | **비트단위 패리티를 제품의 본질로 승격** | 3-way 화면에서 "지형은 동일, 속도만 다름"이 눈으로 증명되어야 GIF가 성립 |
| D5 | **법적 가드레일 준수** | 이름에 Minecraft를 지배적 요소로 쓰지 않고 부제로만, "NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR MICROSOFT" 명시 |

### Why 3-way GIF over 벤치 대시보드?

초기에는 별도 패리티 검증 하네스를 데모의 주인공으로 삼는 안을 검토했다. 3-way 탑뷰가 우월하다. 같은 시드로 세 화면을 나란히 놓으면 패리티 증명이 화면에 내장되어 별도 하네스가 불필요해지고, 기술 관객과 일반 관객을 동시에 커버한다. 대시보드는 기술 관객만 읽는다.

단 이것이 D4를 강제한다. 패리티가 화면에 내장된다는 것은 패리티가 깨지면 그것도 화면에 내장된다는 뜻이다. 우리 쪽이 지형만 생성하고 features를 빼면 관객이 즉시 알아채고 flex가 사기로 뒤집힌다.

### Why 시드 탐색(seed search)을 버렸나

ROI 기준으로는 시드 탐색이 최적해였다. 시드 간 의존성이 0이라 Amdahl 지분이 없고, 바이옴/구조물 레이어만 평가하므로 decoration RNG 순서 재현이 불필요하며, cubiomes라는 검증된 C 레퍼런스가 있고, 개인 노트북으로 10^12 시드를 스캔할 수 없어 컴퓨트 판매 지점이 실제로 존재한다.

그러나 D1이 확정되자 탈락했다. 시드 탐색은 **화면에 아무것도 보이지 않는다.** "10^12개 스캔함"은 숫자일 뿐이고 관객이 감탄할 그림이 없다. FOMO 지표로는 최악이다.

### Anti-goals (explicit rejections)

- 과금·구독·컴퓨트 판매 (D1)
- pregen 대행 서비스 (벽 2)
- 시드맵 웹사이트 (벽 3)
- 시드 탐색 서비스 (위 절)
- MC를 벗어난 범용 절차적 지형 생성 API — 시장은 크지만 기술 재사용도가 낮고 이 프로젝트의 flex와 무관

### Pitfalls

1. **"빠름"은 눈에 보이지 않고 "끊김"만 눈에 보인다.** 일반 유저 관객만 노리면 최적화 성과가 "좋은 서버네"로 소비되고 끝난다. 3-way 나란히 비교가 이 저주를 회피하는 유일한 형태다.
2. **비교 기준선을 낡은 버전으로 잡으면 즉시 털린다.** C2ME는 이미 density function compiler를 도입했다. GIF의 2번 화면은 반드시 C2ME 최신 릴리스여야 한다.
3. **풀 파이프라인 숫자를 숨기면 첫 댓글에서 무너진다.** features가 순차 의존이라 GPU/병렬화가 제한된다는 사실을 먼저 공개하는 편이 신뢰를 얻는다.

### Verification

- 3-way GIF의 세 화면이 동일 시드에서 육안으로 동일한 지형을 그린다
- 세 구성의 region 파일 `sha256`이 일치한다 (ADR-002 D3)
- 배포물 README와 영상 설명에 D5 디스클레이머가 포함되어 있다

### When this might break

- Mojang이 Usage Guidelines를 개정해 서드파티 월드젠 재구현을 명시적으로 금지하는 경우
- ROI가 목적으로 재진입하는 경우 — 그때는 본 ADR을 supersede하고 시드 탐색안을 재검토해야 한다

### References

- https://www.minecraft.net/en-us/usage-guidelines
- https://modrinth.com/project/VSNURh3q (C2ME)
- https://github.com/Cubitect/cubiomes-viewer

---

## ADR-002 — 밑바닥부터 순수 C로 재작성하고, 풀 파이프라인 비트 패리티를 유지한다 (Phase 1, 2026-07-27)

**Status:** Decided
**Type:** Architecture

### Context

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

### Decision (4 핵심 결정)

| # | 결정 | 핵심 |
|---|---|---|
| D1 | **밑바닥부터 순수 C로 재작성** | C2ME 포크 아님. 인터프리터 트리를 그대로 옮기면 2배로 끝나므로 자료구조부터 재설계 |
| D2 | **풀 파이프라인 (noise, surface, carvers, features, lighting 전부)** | terrain-only로 자르지 않는다. GIF의 정직성이 여기 달려 있다 |
| D3 | **바닐라와 비트단위 동일 출력 필수** | region 파일 `sha256` 일치가 수용 기준 |
| D4 | **범위 3분할로 작업량 통제** | 1.21.x 단일 패치 / 오버월드만 / 구조물은 배치까지 (직소 조립 제외) |

### Why 밑바닥 재작성 over C2ME 포크?

(a)가 ROI 최적해였음을 기록해 둔다. (b)를 택한 근거는 ADR-001 D1이다. 목적이 기술 시연이므로 "SoTA 위에 얹어 3배"보다 "전부 다시 써서 47배"가 목적에 부합한다. 또한 D1은 C2ME의 GPL-3.0 전파를 피해 라이선스를 자유롭게 선택할 수 있게 한다.

대가는 정직하게 기록한다. features/carvers/structures/lighting 전부를 C로 재구현해야 한다. cubiomes는 바이옴과 구조물 위치만 다뤘고 그것도 큰 프로젝트였다. **풀 바닐라 패리티 C 재구현은 선례가 없다.** 그것이 flex의 크기이자 리스크의 크기다.

### Why 비트 패리티 필수?

1. 그것이 flex의 전부다. "빠른 지형 생성기"는 흔하고 "너희 월드를 비트단위 동일하게 47배 빠르게 재생성"은 선례가 없다.
2. 검증이 공짜로 딸려온다. 바닐라 region과 우리 출력의 `sha256` 비교로 끝난다. 그 결과가 어떤 대시보드보다 강력하다.
3. 패리티가 없으면 GIF 자체가 무의미하다. 서로 다른 알고리즘의 속도 비교는 벤치마크가 아니다.

### Anti-goals

- terrain-only 벤치마크 (D2가 배제)
- 버전 매트릭스 지원 (D4)
- 네더/엔드 (D4)
- 직소 구조물 조립 — 마을 내부 방 배치는 별개 난이도이며 Phase 1 범위 밖 (D4)
- float으로 다운캐스트한 근사 생성 — 성능은 오르지만 D3와 양립 불가

### Pitfalls

1. **FMA contraction이 조용히 지형을 바꾼다.** `a*b+c`를 FMA 한 명령으로 접으면 중간 반올림이 사라져 결과 비트가 달라진다. 바닐라는 mul → round → add 순서다. CPU 커널은 FMA 금지, GPU 코드가 생기면 `-fmad=false` 필수. 이 버그는 증상이 "지형이 미묘하게 다름"뿐이라 디버깅 비용이 극단적으로 높다.
2. **RNG 소비 순서까지 재현해야 한다.** LCG와 Xoroshiro128++의 호출 순서가 바뀌면 features 배치가 달라진다.
3. **바닐라 노이즈는 double이다.** float으로 낮추는 최적화는 D3 위반이다.
4. **features는 청크 경계를 넘어 이웃 청크에 write한다.** 인접 청크를 동시 처리하면 충돌하고, 처리 순서가 바뀌면 RNG 소비 순서가 달라진다. 체스판/스트라이프 스케줄링으로 충돌 없는 청크 집합만 배치에 넣어야 한다. 이것이 GPU 커널보다 어렵고 C2ME가 수년간 씨름한 부분이다.
5. **벤치 호스트 신뢰성.** 현재 개발 박스는 `lscpu`가 `Core(s) per socket: 22`, `Thread(s) per core: 1`, `L3 cache: 352 MiB (22 instances)`를 보고한다. 실제 5900X는 12코어/24스레드, L3 64MB이며 `hypervisor` 플래그가 있다. 즉 SMT·캐시 토폴로지가 오보고되는 VM이다. **이 박스의 사이클 카운트는 근거로 쓸 수 없다.** 공개 벤치는 베어메탈 또는 코어 핀닝된 전용 인스턴스에서 재측정해야 한다.

### Verification

- `hyperchunk-verify --seed <s> --region <x> <z>` 가 바닐라 golden region과 `sha256` 일치를 출력
- 스테이지 비중 추정치(`noise 45%` 등)를 실측 프로파일로 교체하고 본 ADR의 계산을 재확인
- FMA 금지가 컴파일 플래그로 강제되고 CI가 회귀를 잡는다

### When this might break

- Mojang이 월드젠 알고리즘을 대규모 변경하는 경우 (D4의 단일 패치 고정이 방어선)
- 실측 프로파일이 추정 비중과 크게 다를 경우 — 특히 features 비중이 22%보다 훨씬 크면 우선순위 재조정 필요

### References

- ADR-001 (목적)
- https://github.com/RelativityMC/C2ME-fabric
- https://github.com/Cubitect/cubiomes

---

## ADR-003 — 코어는 리전 단위 C ABI를 노출하는 순수 계산 라이브러리이고, 호환은 데이터팩 스키마 + 바닐라 fallback으로 얻는다 (Phase 1, 2026-07-27)

**Status:** Decided
**Type:** Architecture / Contract

### Context

ADR-002 논의 중 저자가 "밑바닥부터 C = JVM 밖 독립 바이너리"라고 결론지었다. **이는 오류였고 사용자가 정정했다.** 원문:

> "이게 사용성 측면에서 기존 버킷 호환 또는 플러그인 호환 레이어를 못만들면 사용성이 떨어지고 이식성이 좀 많이 깎이는게 단점일것 같은데 방법있을까?
> 예전에 내가 느꼈던 node -> bun으로만 바꿨는데 API 레이턴시가 30% 늘어나는 그런 경험을 주게 만들고싶어"

C로 작성한 코드를 Fabric 모드 내부에 `.so`로 탑재하는 것은 완전히 가능하다. 독립 바이너리는 선택지이지 강제사항이 아니다.

bun의 실제 전략을 분석했다. bun은 npm 생태계를 재구현하지 않고 Node의 *인터페이스*를 구현했다: `node:` 내장 모듈 API 표면과 **N-API ABI**(네이티브 애드온이 재컴파일 없이 로드된다). 패키지는 손대지 않고 엔진만 교체한다.

MC 월드젠의 대응 인터페이스는 셋이다:

| 인터페이스 | 정체 | 네이티브 처리 가능성 |
|---|---|---|
| 데이터팩 worldgen JSON (`noise_settings`, `density_function`, `placed_feature`) | 데이터 | 가능. 스키마 구현으로 Terralith/Tectonic이 자동 호환 |
| Java 코드로 등록된 Feature/StructurePiece | JVM 바이트코드 | 원리적으로 불가 |
| Bukkit/Paper `ChunkGenerator`, `BlockPopulator` | JVM 인터페이스 | 브릿지 가능 |

MC가 1.18부터 월드젠을 데이터 주도로 만들어 둔 것이 우리에게 N-API 역할을 한다.

경계 비용을 정량화했다(JNI 빈 호출 ~22ns, 청크 생성 ~10ms 기준):

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

### Decision (5 핵심 결정)

| # | 결정 | 핵심 |
|---|---|---|
| D1 | **코어는 순수 계산 라이브러리. 파일 I/O도 네트워크도 모른다** | 4개 소비자가 붙을 수 있고, 벤치가 깨끗하고, 상태 관리가 코어를 오염시키지 못한다 |
| D2 | **경계는 리전 단위로만 노출. 노드 단위 함수를 외부로 내보내지 않는다** | `hyperchunk_batch(seed, region_list, out)`. 노드 단위 노출은 18.5% 손실 |
| D3 | **arena/SoA 할당자와 배치 스케줄러를 코어가 소유** | 배치를 자바에 남기면 GC 벽에 걸려 성과가 드러나지 않는다 |
| D4 | **데이터팩 worldgen JSON을 우리의 N-API로 취급** | 스키마 구현으로 Terralith/Tectonic급 월드젠이 자동 호환 |
| D5 | **알 수 없는 확장을 만나면 해당 청크를 통째로 바닐라 경로로 fallback** | 추측하지 않는다. 정확성이 속도보다 우선 |

### Why 리전 단위 경계?

D2가 사용자의 bun 열화 경험을 구조적으로 회피한다. "C 함수를 Java에서 호출한다"가 아니라 "리전을 통째로 넘기고 완성해서 받는다"는 구조여야 한다. 노드 단위로 노출하면 naive 구현이 자연스럽게 18.5% 손실 경로로 빠진다. 따라서 이것은 성능 팁이 아니라 **API 표면에서 강제하는 불변식**이다.

FFM(Panama)은 Java 22에서 확정됐고(JEP 454) MC 1.20.5+는 Java 21을 요구하므로 21에서는 preview다. 따라서 JNI로 간다. 리전 단위 입도에서는 FFM과 JNI의 성능 차이가 무의미하다.

### Why fallback? — drop-in의 정의

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

### 모듈 경계

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
- **JNI/Fabric**은 실제 서버 사용성
- **Rust FFI**는 Phase 3 이후. 위 경쟁 지형이 수요를 보여준다
- Paper 플러그인 브릿지는 API가 더 얽혀 있어 Fabric 검증 후로 미룬다

CLI와 모드는 같은 정적 라이브러리를 두 번 링크하므로 추가 비용이 거의 없다.

### Anti-goals

- 코어에 파일 I/O·네트워크·플레이어/엔티티 상태 넣기 (D1)
- density function 노드 단위 FFI 노출 (D2)
- 배치 스케줄링을 자바에 남기기 (D3)
- Java Feature를 네이티브에서 에뮬레이션하려는 시도 — 원리적으로 불가하며 D5가 정답
- Paper 브릿지를 Phase 1에 포함

### Pitfalls

1. **노드 단위 FFI 유혹.** 이식 초기에 "Java의 density function 노드를 하나씩 네이티브 호출"하는 것이 가장 쉬운 경로처럼 보인다. 그것이 18.5% 손실 경로다. 코어 헤더에 노드 단위 함수를 아예 선언하지 않아 컴파일 단계에서 막는다.
2. **fallback 판정을 청크보다 작은 단위로 하려는 유혹.** 부분 fallback은 RNG 소비 순서를 깨뜨린다. 판정 단위는 청크 전체다.
3. **arena 재사용 시 stale 데이터.** SoA 버퍼를 배치 간 재사용할 때 초기화 누락이 패리티 버그로 나타나고, 증상이 산발적이라 추적이 어렵다.
4. **Java 21 vs 22 런타임 혼선.** FFM 예제 코드를 그대로 쓰면 `--enable-preview` 없이 동작하지 않는다. JNI를 쓰기로 한 이유가 여기 있다.

### Verification

- `hyperchunk` 헤더에 리전 단위 진입점만 존재하고 노드 단위 함수가 없다
- 코어가 `libc` 외 의존성 없이 빌드된다 (`ldd` 확인)
- Terralith 데이터팩을 입력했을 때 바닐라 대조와 `sha256` 일치
- 인위적으로 알 수 없는 Feature를 등록했을 때 해당 청크가 바닐라 경로로 처리되고 결과가 여전히 패리티를 만족

### When this might break

- Mojang이 데이터팩 worldgen 스키마를 비호환 변경하는 경우 (ADR-002 D4의 단일 패치 고정이 방어선)
- 모드 생태계가 데이터팩보다 Java 코드 확장으로 회귀하는 경우 — fallback 비중이 커져 배수가 41배 아래로 내려간다

### References

- ADR-002 (재작성 방침)
- https://openjdk.org/jeps/454 (FFM, Java 22)
- https://github.com/pumpkin-mc/pumpkin
- https://github.com/valence-rs/valence
- https://minestom.net/
- https://minecraft.wiki/w/Data_pack

---

## ADR-004 — AVX2를 기본 커널로 삼고 AVX-512는 런타임 디스패치로 추가한다 (Phase 2, 2026-07-27)

**Status:** Decided
**Type:** Architecture / Performance strategy
**Phase:** 2 (Phase 1에서는 스칼라 참조 커널만 구현)

### Context

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

### Decision (4 핵심 결정)

| # | 결정 | 핵심 |
|---|---|---|
| D1 | **AVX2를 기본 커널로 삼는다** | Haswell(2013) 이후 보편적. AVX-512 전용은 관객 대부분이 실행조차 못 한다 |
| D2 | **AVX-512는 `cpuid` 런타임 디스패치로 추가** | 버리지 않는다. 두 백엔드 공존 |
| D3 | **FMA는 전 백엔드에서 금지** | 컴파일 플래그로 강제. 패리티 불변식 |
| D4 | **모든 백엔드의 출력 바이트 동일성을 CI 게이트로 강제** | 선택이 아니라 필수 |

### Why AVX2 먼저?

재현 불가능한 벤치는 기술 관객에게 즉사 요인이다. AVX-512는 Zen5 또는 Intel 서버 칩에만 있어 대부분의 관객이 검증할 수 없다. 반면 두 백엔드를 함께 두면 flex가 하나 늘어난다: **"AVX2/AVX-512 두 백엔드, 출력 해시 동일"** 이 패리티 엄밀성의 증거로 기능한다.

### Anti-goals

- AVX-512 전용 빌드 (D1)
- FMA 활성화로 2배 얻기 (D3, ADR-002 D3 위반)
- noise 스테이지 추가 최적화를 features보다 우선하기 — 위 표가 features 우선을 지시한다
- GPU(CUDA) 백엔드 — noise 오프로드만으로는 총 2.9배이고 병목이 features로 이동할 뿐. 배치 pregen 서비스가 아니면 ROI가 없으며 ADR-001이 그 방향을 배제했다

### Pitfalls

1. **SIMD 레벨 간 부동소수점 결과 불일치.** FastNoise2 FAQ도 지적하는 알려진 문제다. 우리는 비트 패리티가 제품의 본질이므로 D4의 CI 게이트가 필수다.
2. **Zen4를 Zen5로 오인한 하드웨어 구매.** double-pump 때문에 Zen4의 AVX-512 FP 이득은 0에 가깝다.
3. **AVX-512 다운클럭 가정.** Skylake-X 시절 통념이며 Zen4/Zen5에는 고정 주파수 오프셋이 없다. 오히려 512-bit가 256-bit보다 약간 높은 클럭으로 관측된 사례가 있다.
4. **로컬 개발 박스에 AVX-512가 없다.** 5900X는 Zen3다. AVX-512 경로는 로컬에서 실측 불가이며 별도 하드웨어가 필요하다.

### Verification

- `hyperchunk-bench --isa=scalar|avx2|avx512` 세 경로의 출력 `sha256`이 모두 동일
- `objdump`로 FMA 명령(`vfmadd*`)이 산출물에 존재하지 않음을 확인
- `cpuid` 디스패치가 AVX-512 없는 호스트에서 AVX2로 정상 폴백

### When this might break

- Zen5/AVX-512 하드웨어가 보편화되어 D1의 이식성 논거가 약해지는 경우
- 실측 프로파일에서 noise 비중이 45%보다 훨씬 크게 나오는 경우 — 그때는 AVX-512 우선순위를 올려야 한다

### References

- ADR-002 (FMA 금지의 출처)
- https://www.numberworld.org/blogs/2024_8_7_zen5_avx512_teardown/
- https://chipsandcheese.com/p/zen-5s-avx-512-frequency-behavior
- https://github.com/Auburn/FastNoise2/wiki/FAQ
- https://www.felixcloutier.com/x86/vpermt2w:vpermt2d:vpermt2q:vpermt2ps:vpermt2pd

---

## ADR-005 — 네트워크/패킷 레이어(P4)는 별개 제품으로 격리하고 Phase 1 설계에 영향을 주지 않는다 (Phase 4, 2026-07-27)

**Status:** Decided
**Type:** Scope boundary
**Phase:** 4 (paper-only, P1~P3 완료 후 재평가)

### Context

사용자가 장기 로드맵을 제시했다:

> "최종 로드맵은 c레벨 자바 없는 네트워트/패킷레벨 최적화도 해보고싶어서"

이후 성격을 명확히 했다:

> "P4는 그냥 꺼내본 장기 로드맵이였어"

로슈라인을 재적용해 P4가 P1~P3과 같은 물리법칙 위에 있는지 확인했다:

| 워크로드 | arithmetic intensity | 판정 |
|---|---|---|
| 청크 생성 | **21.36 flops/byte** | COMPUTE bound |
| 청크 zlib 압축 | 2.03 | MEMORY/IO bound |
| **패킷 직렬화** | **0.00** | **MEMORY/IO bound** |

**축적한 무기가 전이되지 않는다.** AVX-512, zmm 32 레지스터, `vpermt2pd`, FMA 금지 대응, CPI 튜닝은 모두 compute bound 워크로드용이다. 패킷 계층에 필요한 것은 `io_uring`, zero-copy, `sendmmsg`, syscall 배칭이며 완전히 다른 스킬셋이다.

압축은 이미 해결된 문제다. libdeflate / ISA-L / zlib-ng가 zlib보다 2배 이상 빠르고 검증되어 있다. 직접 구현은 NIH이며 조립이 정답이다.

더 중요한 것은 **P4가 ADR-003 D5를 무효화한다**는 점이다. 패킷 계층을 대체하려면 플레이어/엔티티/인벤토리/틱루프 상태를 우리가 소유해야 하고, 그 순간 fallback 대상인 JVM 서버가 사라져 fallback이 원리적으로 불가능해진다. 즉 P4는 "모드"가 아니라 "서버 전체 대체"이며 Pumpkin과 같은 카테고리다.

### Decision (3 핵심 결정)

| # | 결정 | 핵심 |
|---|---|---|
| D1 | **P4는 별개 제품으로 격리. Phase 1~3 설계 입력이 아니다** | P4를 고려한 상태 관리 레이어를 코어에 미리 넣지 않는다 |
| D2 | **P3(region I/O)까지는 P1에 포함** | 패리티 검증에 region 비교가 필요하고, 압축은 라이브러리 조립이라 저비용 |
| D3 | **격리가 P4를 막는 것이 아니라 가능하게 한다** | 코어가 순수 계산 라이브러리면 P4에서 그대로 재사용된다 |

### 로드맵 축별 평가

| 축 | 성격 | 헤드룸 | 필요 기술 | 선행 | 판정 |
|---|---|---|---|---|---|
| P1 worldgen 코어 | compute | 높음 (AI 21.4) | SIMD/CPI 직결 | 없음 | 최우선 |
| P2 배치/스케줄러 | compute | GC 제거가 본질 | arena/SoA | P1 | **P1에 흡수** |
| P3 region I/O | IO | libdeflate 등 | 라이브러리 조립 | P1 | 포함 |
| P4 네트워크/패킷 | IO | AI 0.00 | io_uring 등 | 서버 전체 | **격리** |

P2를 P1에 흡수한 근거는 ADR-003 D3에 기록했다. 배치를 자바에 남기면 GC 벽에 걸려 P1 성과가 드러나지 않는다. 이것은 스코프 확대가 아니라 P1이 원래 포함해야 했던 부분이며, 초기 설계에서 누락된 항목이다.

### Why 격리가 P4를 가능하게 하나

Pumpkin/Valence가 우리 코어를 쓸 수 있다는 것은, 우리가 나중에 서버를 만들 때도 코어가 그대로 쓰인다는 뜻이다. 반대로 지금 P4를 고려해 코어에 상태 관리를 넣으면, 코어가 4개 소비자 어디에도 붙지 않는 어중간한 물건이 된다. grill-me 스킬이 경고하는 speculative implementation 패턴이다.

### Anti-goals

- 코어에 플레이어/엔티티/인벤토리/틱루프 상태 넣기 (D1)
- 압축 알고리즘 직접 구현 (libdeflate 조립)
- P4를 Phase 1 플랜의 태스크로 편성

### Pitfalls

1. **"나중에 필요할 테니 미리" 논리.** P4를 근거로 코어에 상태 훅을 추가하는 것이 가장 흔한 오염 경로다. ADR-003 D1이 방어선이다.
2. **P3와 P4 혼동.** region 파일 쓰기(P3)는 코어 밖 CLI/모드 계층의 일이며 코어는 버퍼만 채운다. 이 경계가 흐려지면 D1이 무너진다.

### Verification

- Phase 1~3 코드에 소켓·플레이어·틱루프 관련 심볼이 존재하지 않는다
- 코어 헤더에 상태 수명주기 API가 없다

### When this might break

- P1~P3가 완료되고 사용자가 P4를 실제 목표로 승격하는 경우 — 그때 본 ADR을 supersede하고 ADR-003 D5(fallback)의 대체 전략을 새로 설계해야 한다

### References

- ADR-003 (fallback 전략, 모듈 경계)
- https://github.com/zlib-ng/zlib-ng/issues/1486 (압축 라이브러리 벤치)
- https://minecraft.wiki/w/Java_Edition_protocol/Packets

---

## ADR-006 — 타겟 버전을 26.2로 변경하고, 비난독화 소스와 FFM을 활용한다 (Phase 1, 2026-07-28)

**Status:** Decided
**Type:** Scope / Contract
**Resolves:** ADR-002 D4의 버전 핀, ADR-003의 JNI 선택을 부분 supersede

### Context

사용자 directive: "일단 최초 버전 타겟은 26.2로 하자"

ADR-002 D4는 "1.21.x 단일 패치"를 가정했으나, Mojang이 2026년부터 연도 기반 버저닝(year.drop.hotfix)으로 전환했다. 릴리스 계보: 1.21.11 → 26.1 "Tiny Takeover" (2026-03-24) → 26.1.1/26.1.2 → **26.2 "Chaos Cubed" (2026-06-16, 현재 latest release)**. 버전 매니페스트에서 실존 확인함 (protocol 776, data version 4903, data pack format 107.1).

조사에서 확인된 파급효과 세 가지:

1. **Java 25 필수.** 26.1부터 Minimum Java version = Java SE 25. 플랜의 "JDK 21 설치" 지시는 무효.
2. **26.1부터 완전 비난독화.** 26.1은 "the first to be fully unobfuscated without an accompanying obfuscated variant". Fabric도 이에 따라 Yarn 매핑 지원을 중단하고 Mojang 공식 이름으로 전환했다. 매핑 레이어가 사라져 스테이지 덤프 하네스(mixin)를 실명 클래스로 직접 작성한다.
3. **26.2 worldgen은 1.21.x와 다르다.** sulfur caves 케이브 바이옴과 sulfur/cinnabar 블록이 추가되어 재현해야 할 표면이 늘었다. cubiomes 등 기존 레퍼런스 구현은 26.x를 지원하지 않을 가능성이 높다. 대신 비난독화 소스가 알고리즘 확인 비용을 크게 낮춰 이를 상쇄한다.

### Decision (4 핵심 결정)

| # | 결정 | 핵심 |
|---|---|---|
| D1 | **타겟 버전 = 26.2 고정** | TARGET_VERSION=26.2. ADR-002 D4의 "단일 패치 고정" 원칙은 유지, 값만 교체 |
| D2 | **golden 생성 환경은 JDK 25** | 26.x 서버 구동 요건 |
| D3 | **매핑 레이어 폐기, Mojang 실명 직접 사용** | Yarn 사망. 비난독화 jar 기준으로 mixin/리플렉션 작성 |
| D4 | **Phase 3 브릿지는 JNI 대신 FFM** | Java 25에서 FFM(JEP 454)이 정식. ADR-003의 "Java 21 preview라서 JNI" 논거 소멸. 경계 입도 불변식(리전 단위)은 그대로 |

### Why 26.2 over 1.21.x?

최신 안정판이 26.2이므로 "현재의 바닐라"와 비교하는 것이 시연 목적(ADR-001)에 부합한다. 1.21.x 대비 벤치는 출시 시점에 이미 구버전 비교가 된다.

### Anti-goals

- 26.1/26.3-snapshot 동시 지원 (단일 패치 원칙)
- 1.21.x 하위 호환
- 디컴파일/비난독화 소스 코드의 복사 — 이름 참조와 알고리즘 이해는 자유로워졌지만 코드 복사는 여전히 저작권 문제 (ADR-002 R4 원칙 유지)

### Pitfalls

1. **sulfur caves는 carvers/features/biomes 재구현 범위에 포함된다.** 1.21 기준 자료(위키 문서 다수)가 26.2 현실과 다를 수 있으므로, 항상 비난독화 소스와 26.2 추출 JSON을 1차 근거로 삼는다.
2. **cubiomes 참조 시 버전 주의.** 26.x 미지원 알고리즘을 그대로 믿으면 패리티가 조용히 깨진다.
3. **data pack format 107.1.** Task 12 데이터팩 스키마 파서는 26.2 스키마 기준이다.
4. **오버월드 y 범위(-64..319) 유지 여부 미확인.** Task 2 golden 덤프에서 실측으로 확정하고 hyperchunk.h 상수를 그에 맞춘다.

### Verification

- TARGET_VERSION 파일 내용 = 26.2
- golden 서버가 JDK 25에서 구동되고 region 생성 확인
- 스테이지 덤프 하네스가 매핑 도구 없이 빌드됨

### References

- ADR-002, ADR-003
- https://minecraft.wiki/w/Java_Edition_26.2
- https://minecraft.wiki/w/Java_Edition_26.1 (비난독화, Java 25 요건)
- https://docs.fabricmc.net/develop/porting/ (Yarn 중단, Mojang mappings 전환)

---

## ADR-007 — 패리티 게이트를 2단(비트정확 + 순서재생)으로 재정의한다 (Phase 1, 2026-07-28)

**Status:** Decided
**Type:** Quality strategy / Contract
**Resolves:** ADR-002 D3의 "region sha256 일치" 게이트를 정밀화 (원칙 유지, 정의 교체)

### Context

Task 2 golden 작업에서 실측으로 확인된 사실: **바닐라 26.2 월드젠은 같은 시드·같은 머신에서도 run-to-run 비결정적이다.** 증거는 `tools/golden/NOTES.md`에 기록했다. 요약:

- `01_structure_starts`~`06_carvers` 스테이지 덤프는 두 독립 실행에서 바이트 단위 일치
- `07_features` 이후는 3x3 기본 런에서 4/9 청크, 5x5 순차 probe에서 16/25 청크가 실제 내용 차이 (glow_lichen/vine/광맥 배치)
- raw `.mca`는 물론 타임스탬프를 마스킹한 canonical payload 해시도 불일치
- 순차 forceload로도 안정화 실패: **full 승격 순서 자체가 두 probe 런에서 달랐다**

메커니즘: carvers까지는 (seed, chunk pos)의 위치시드 순수 함수라 순서 무관. features는 청크 경계를 넘어 읽고 쓰므로(나무/덩굴 스필오버, heightmap 갱신) 이웃 데코 순서가 결과를 바꾸고, 스케줄러는 그 순서를 고정하지 않는다.

따라서 ADR-002 D3의 "바닐라 region과 sha256 일치"는 그대로는 정의 불성립이다. **바닐라가 자기 자신과도 sha256이 일치하지 않는다.**

### Decision (3 핵심 결정)

| # | 결정 | 핵심 |
|---|---|---|
| D1 | **Tier 1 게이트: 01~06 스테이지는 바닐라 golden 덤프와 바이트 단위 일치** | 순서 무관 구간이므로 무조건 비트정확. noise/surface/carvers가 여기 속함 |
| D2 | **Tier 2 게이트: features 이후는 order-replay 검증** | golden 런의 실제 features 실행 순서를 order manifest로 기록하고, C 구현이 그 순서를 재생해 07~11 덤프와 바이트 단위 일치해야 함 |
| D3 | **비결정성을 노이즈가 아니라 입력으로 승격** | 서로 다른 순서의 golden 런 2개를 모두 재생·일치시키면 알고리즘 패리티의 강한 증거 |

### Why order-replay over 바닐라 순서 고정?

바닐라를 우리 순서에 맞추는 실험(sequential probe)은 실패했다. 스케줄러가 외부에서 고정되지 않는다. 반대로 **바닐라가 실제로 한 순서를 기록해 우리가 따라가는 것**은 항상 가능하고, 재현 가능한 golden 번들(덤프 + order manifest)을 만든다. C 코어는 어차피 배치 스케줄러를 소유하므로(ADR-003 D3) 임의 순서 재생이 자연스럽다.

### 3-way 시연에 대한 의미 (ADR-001 연계)

"바닐라와 비트단위 동일" 주장의 정확한 스코프는 "**같은 시드 + 같은 데코 순서에서** 비트단위 동일"이 된다. 시연 문구와 README는 이 스코프를 정직하게 명시한다. 이는 약점이 아니라 오히려 방어력이다: 기술 관객이 "바닐라도 런마다 다른데 뭐랑 같다는 거냐"고 공격하는 지점을 우리가 먼저 점유한다.

### Anti-goals

- 바닐라 스케줄러 자체를 패치해 순서를 고정하는 접근 (침습적, 유지 불가)
- features 이후를 "통계적 유사"로 완화 (패리티 원칙 포기)
- Tier 1 구간을 order-replay로 낮추기 (불필요한 약화)

### Pitfalls

1. **order manifest 기록이 아직 미구현.** stage-dump mod 확장 필요 (open item). manifest 없는 golden은 Tier 2 검증에 쓸 수 없다.
2. **probe의 순차 강제 실패 원인 미규명.** forceload 완료 대기 로직이 스케줄러를 못 고정했다. Tier 2 설계는 이 실패에 의존하지 않지만, 재도전은 시간 낭비다.
3. **spawn/full 스테이지 diff는 features의 하류 효과.** 별도 원인으로 오인하지 말 것.

### Verification

- golden 번들에 order.manifest 포함 확인
- C 구현이 manifest 재생 모드를 지원하고, 서로 다른 순서의 golden 2벌을 모두 통과
- Tier 1: `diff -rq` 01~06 전체 0 diff

### References

- ADR-002 (패리티 원칙), ADR-003 (배치 스케줄러 소유)
- tools/golden/NOTES.md (증거·환경·재현 절차)
- tools/golden/experiments/sequential_probe.sh

---

## ADR-008 — 코어 스케줄러는 벤치 모드(자유 순서)와 검증 모드(order-replay) 이중 모드로 동작한다 (Phase 1, 2026-07-28)

**Status:** Decided
**Type:** Architecture / Contract
**Resolves:** ADR-007의 게이트 정의를 실행 모드로 구체화

### Context

ADR-007이 패리티 게이트를 2단으로 재정의한 뒤, 사용자가 실행 모드 분리를 확정했다. 원문:

> "우리도 멀티스레딩은 해야하니까 속도 비교에서는 가장 최적화된 (바닐라처럼 약간 비결정론적)방식을 보여주고, TC, 일치성 테스트에서는 순서 따라가기를 통해 일치하는지 (여기서 속도는 후 운선순위니까) 보면 되겠다"

### Decision (3 핵심 결정)

| # | 결정 | 핵심 |
|---|---|---|
| D1 | **벤치/시연 모드: 자유 스케줄링** | 충돌 없는 최대 병렬(체스판 등). features 적용 순서는 성능 최적으로 자유. 바닐라와 같은 수준의 순서 비결정성 허용 |
| D2 | **검증 모드: order-replay** | golden order manifest를 재생. 속도는 후순위, 07~11 스테이지 바이트 일치가 목표 (ADR-007 Tier 2) |
| D3 | **두 모드는 같은 스테이지 코드를 공유, 스케줄러 정책만 교체** | 검증된 코드가 벤치에서 돌아야 증명이 성립. 모드별 별도 구현 금지 |

### 시연 서사 (ADR-001 연계)

"검증 모드에서 바닐라와 비트 일치를 증명한 그 코드가, 벤치 모드에서 N배로 돈다." 두 모드가 코드를 공유하므로(D3) 속도 주장과 정확성 주장이 하나의 구현에 대한 주장이 된다.

### Anti-goals

- 검증 모드 전용 느린 참조 구현과 벤치 전용 구현의 분리 (D3 위반 — 증명이 무효화됨)
- 벤치 모드 결과의 스테이지 덤프 비교 (순서 비고정이므로 정의상 무의미, Tier 1 구간 제외)

### Pitfalls

1. 스케줄러 정책 주입점이 코어 API에 있어야 한다: `hc_schedule_policy { FREE, REPLAY(manifest) }` 형태. CLI 플래그가 아니라 라이브러리 계약.
2. 벤치 숫자는 항상 FREE 모드로, 패리티 배지는 항상 REPLAY 모드로 표기. 혼용 보고는 신뢰도 자살.

### References

- ADR-007 (2단 게이트), ADR-003 (스케줄러 소유), ADR-001 (시연 목적)
