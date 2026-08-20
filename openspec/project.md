# hyperchunk — Project

바닐라 Minecraft Java Edition 26.2 오버월드 월드젠을 순수 C11로 비트단위 동일하게
재구현하는 프로젝트. 이 문서는 프로젝트의 방향 결정(제품 목적·스코프 경계)과
capability 인덱스를 담는다. 테스트 가능한 요구사항은 `specs/<capability>/spec.md`,
각 결정의 근거·수치·역사는 같은 폴더 `context.md`의 "Decision history"에 있다.

> NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG
> OR MICROSOFT.

## Capabilities

| Capability | 무엇의 SSOT인가 |
|---|---|
| [worldgen-parity](specs/worldgen-parity/spec.md) | 비트정확 패리티 정의, 2단 게이트(비트정확 + order-replay), canonical hash 정의, golden capture 체계 |
| [generation-pipeline](specs/generation-pipeline/spec.md) | 11스테이지 파이프라인, 26.2 버전 핀, 데이터팩 스키마 호환 + 바닐라 fallback |
| [simd-backends](specs/simd-backends/spec.md) | 스칼라/AVX2/AVX-512 런타임 디스패치, 백엔드 간 바이트동일, FMA 금지 게이트 |
| [core-abi](specs/core-abi/spec.md) | 리전 단위 C ABI, 순수 계산 라이브러리 경계, FFI 소비자(CLI/FFM/Rust) |
| [scheduler](specs/scheduler/spec.md) | FREE/REPLAY 이중 모드, 결정론 보장, 스테이지 코드 공유 |
| [engineering-safety](specs/engineering-safety/spec.md) | C11 유지, sanitizer 게이트, zero-warning, 게이트 체인 |
| [benchmarks-and-viz](specs/benchmarks-and-viz/spec.md) | 공개 벤치 수치와 클레임 규칙, 데모/viz 캡처·타임라인 |

## 워크플로 (요약)

specs가 현재 동작의 normative SSOT다. 동작·계약·스키마를 바꾸는 작업은
`openspec/changes/`에 change를 먼저 만들고, 구현 후 spec을 동기화하고,
`openspec validate --specs`를 통과시킨 뒤 `openspec/changes/archive/`로 보관한다.
자세한 절차는 리포 루트 [AGENTS.md](../AGENTS.md), 기여 규칙은
[.github/CONTRIBUTING.md](../.github/CONTRIBUTING.md).

Decision history 규칙 (구 append-only ADR 로그의 계승): 각 capability
`context.md`의 "Decision history" 섹션은 **append-only**다. 기존 ADR 항목은
수정하지 않는다. 결정이 바뀌면 새 항목을 추가하고 이전 항목을 `Superseded by`로
표시하며, spec.md 반영은 openspec change를 통해서만 한다.

---

## Decision history — 프로젝트 방향

요구사항이라기보다 방향 결정인 ADR 두 건을 여기 보존한다. ADR 번호·날짜는
원본(구 append-only ADR 로그, 2026-07-27~28 작성)을 그대로 유지한다.

### ADR-001 — 프로젝트 목적은 ROI가 아니라 기술 시연이며, 산출물은 3-way 탑뷰 비교 GIF다 (Phase 0, 2026-07-27)

**Status:** Decided
**Type:** Product direction
**Plan:** [changes/archive/2026-07-28-phase1-vertical-slice/](changes/archive/2026-07-28-phase1-vertical-slice/) (구 .hermes/plans/2026-07-27_phase1-vertical-slice.md)

#### Context

프로젝트는 "청크 생성 SaaS"로 출발했다. 초기 조사에서 SaaS 형태를 죽이는 벽 세 개가 확인됐다.

1. **법적 벽.** Minecraft Usage Guidelines는 열거주의다. 원문: "Do **not** make commercial use or commercially exploit anything that we have made unless these guidelines say it's okay" / "If something isn't covered by these guidelines and we haven't otherwise said it's okay, that probably means we don't want you to do it". 허용 목록은 영상·스트림, 서버 운영, 출판물, 핸드크래프트(연 $5,000 상한)뿐이고 "월드젠 출력물 판매"는 열거되지 않았다.
2. **공짜의 벽.** Chunky가 무료로 pregen을 수행한다. 고객은 이미 유휴 CPU를 임대 중이고, pregen은 일회성 비용이라 구독이 붙지 않는다. MC 호스팅 시장가는 $1/GB 수준으로 가격 민감도가 극단적이다.
3. **커모디티 벽.** 시드맵 레이어(Chunkbase, seeds.gg, seedlander, mcseedmap, seedmap.app, cubiomes.com)는 전부 무료 광고 모델이며 SaaS가 아니라 SEO/트래픽 시장이다.

이 시점에 사용자가 목적을 명시했다. 원문 그대로:

> "솔직히 그냥 ROI 개무시한 괴물같은 기술력 자랑이야, 사람들 FOMO오게"

이 directive가 벽 2와 벽 3을 무효화했다(무료 배포이므로 공짜 대안 및 광고 시장과 경쟁하지 않는다). 벽 1은 남는다. 같은 문서가 commercial use를 이렇게 정의하기 때문이다: "commercial use means any uses of our name, brand, or assets that you use and share with others (**regardless of whether you receive payment or provide it for free**)".

산출물 형태 역시 사용자가 지정했다:

> "java 기본 vs 가장 최적화 잘됬다는 버킷/플러그인 vs 우리 / 이거 청크 탑뷰로 실시간 생성이 얼마나 빠른지 gif같은 숏폼으로 보여주기만 해도 충분할 듯"

#### Decision (5 핵심 결정)

| # | 결정 | 핵심 |
|---|---|---|
| D1 | **ROI 최적화를 명시적 anti-goal로 선언** | 수익 모델·과금·고객 획득은 설계 입력이 아니다 |
| D2 | **산출물은 3-way 탑뷰 실시간 생성 비교 숏폼** | `바닐라 Java` vs `Paper + C2ME + Chunky` vs `hyperchunk` |
| D3 | **무료 공개 (OSS)** | 벽 2·3을 회피하고 재현성을 확보 |
| D4 | **비트단위 패리티를 제품의 본질로 승격** | 3-way 화면에서 "지형은 동일, 속도만 다름"이 눈으로 증명되어야 GIF가 성립 |
| D5 | **법적 가드레일 준수** | 이름에 Minecraft를 지배적 요소로 쓰지 않고 부제로만, "NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR MICROSOFT" 명시 |

D4의 normative 반영은 [worldgen-parity](specs/worldgen-parity/spec.md), D5는
[benchmarks-and-viz](specs/benchmarks-and-viz/spec.md)에 있다.

#### Why 3-way GIF over 벤치 대시보드?

초기에는 별도 패리티 검증 하네스를 데모의 주인공으로 삼는 안을 검토했다. 3-way 탑뷰가 우월하다. 같은 시드로 세 화면을 나란히 놓으면 패리티 증명이 화면에 내장되어 별도 하네스가 불필요해지고, 기술 관객과 일반 관객을 동시에 커버한다. 대시보드는 기술 관객만 읽는다.

단 이것이 D4를 강제한다. 패리티가 화면에 내장된다는 것은 패리티가 깨지면 그것도 화면에 내장된다는 뜻이다. 우리 쪽이 지형만 생성하고 features를 빼면 관객이 즉시 알아채고 flex가 사기로 뒤집힌다.

#### Why 시드 탐색(seed search)을 버렸나

ROI 기준으로는 시드 탐색이 최적해였다. 시드 간 의존성이 0이라 Amdahl 지분이 없고, 바이옴/구조물 레이어만 평가하므로 decoration RNG 순서 재현이 불필요하며, cubiomes라는 검증된 C 레퍼런스가 있고, 개인 노트북으로 10^12 시드를 스캔할 수 없어 컴퓨트 판매 지점이 실제로 존재한다.

그러나 D1이 확정되자 탈락했다. 시드 탐색은 **화면에 아무것도 보이지 않는다.** "10^12개 스캔함"은 숫자일 뿐이고 관객이 감탄할 그림이 없다. FOMO 지표로는 최악이다.

#### Anti-goals (explicit rejections)

- 과금·구독·컴퓨트 판매 (D1)
- pregen 대행 서비스 (벽 2)
- 시드맵 웹사이트 (벽 3)
- 시드 탐색 서비스 (위 절)
- MC를 벗어난 범용 절차적 지형 생성 API — 시장은 크지만 기술 재사용도가 낮고 이 프로젝트의 flex와 무관

#### Pitfalls

1. **"빠름"은 눈에 보이지 않고 "끊김"만 눈에 보인다.** 일반 유저 관객만 노리면 최적화 성과가 "좋은 서버네"로 소비되고 끝난다. 3-way 나란히 비교가 이 저주를 회피하는 유일한 형태다.
2. **비교 기준선을 낡은 버전으로 잡으면 즉시 털린다.** C2ME는 이미 density function compiler를 도입했다. GIF의 2번 화면은 반드시 C2ME 최신 릴리스여야 한다.
3. **풀 파이프라인 숫자를 숨기면 첫 댓글에서 무너진다.** features가 순차 의존이라 GPU/병렬화가 제한된다는 사실을 먼저 공개하는 편이 신뢰를 얻는다.

#### Verification

- 3-way GIF의 세 화면이 동일 시드에서 육안으로 동일한 지형을 그린다
- 세 구성의 region 파일 `sha256`이 일치한다 (ADR-002 D3; ADR-007이 canonical hash로 정밀화)
- 배포물 README와 영상 설명에 D5 디스클레이머가 포함되어 있다

#### When this might break

- Mojang이 Usage Guidelines를 개정해 서드파티 월드젠 재구현을 명시적으로 금지하는 경우
- ROI가 목적으로 재진입하는 경우 — 그때는 본 ADR을 supersede하고 시드 탐색안을 재검토해야 한다

#### References

- https://www.minecraft.net/en-us/usage-guidelines
- https://modrinth.com/project/VSNURh3q (C2ME)
- https://github.com/Cubitect/cubiomes-viewer

### ADR-005 — 네트워크/패킷 레이어(P4)는 별개 제품으로 격리하고 Phase 1 설계에 영향을 주지 않는다 (Phase 4, 2026-07-27)

**Status:** Decided
**Type:** Scope boundary
**Phase:** 4 (paper-only, P1~P3 완료 후 재평가)

#### Context

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

#### Decision (3 핵심 결정)

| # | 결정 | 핵심 |
|---|---|---|
| D1 | **P4는 별개 제품으로 격리. Phase 1~3 설계 입력이 아니다** | P4를 고려한 상태 관리 레이어를 코어에 미리 넣지 않는다 |
| D2 | **P3(region I/O)까지는 P1에 포함** | 패리티 검증에 region 비교가 필요하고, 압축은 라이브러리 조립이라 저비용 |
| D3 | **격리가 P4를 막는 것이 아니라 가능하게 한다** | 코어가 순수 계산 라이브러리면 P4에서 그대로 재사용된다 |

#### 로드맵 축별 평가

| 축 | 성격 | 헤드룸 | 필요 기술 | 선행 | 판정 |
|---|---|---|---|---|---|
| P1 worldgen 코어 | compute | 높음 (AI 21.4) | SIMD/CPI 직결 | 없음 | 최우선 |
| P2 배치/스케줄러 | compute | GC 제거가 본질 | arena/SoA | P1 | **P1에 흡수** |
| P3 region I/O | IO | libdeflate 등 | 라이브러리 조립 | P1 | 포함 |
| P4 네트워크/패킷 | IO | AI 0.00 | io_uring 등 | 서버 전체 | **격리** |

P2를 P1에 흡수한 근거는 ADR-003 D3에 기록했다. 배치를 자바에 남기면 GC 벽에 걸려 P1 성과가 드러나지 않는다. 이것은 스코프 확대가 아니라 P1이 원래 포함해야 했던 부분이며, 초기 설계에서 누락된 항목이다.

#### Why 격리가 P4를 가능하게 하나

Pumpkin/Valence가 우리 코어를 쓸 수 있다는 것은, 우리가 나중에 서버를 만들 때도 코어가 그대로 쓰인다는 뜻이다. 반대로 지금 P4를 고려해 코어에 상태 관리를 넣으면, 코어가 4개 소비자 어디에도 붙지 않는 어중간한 물건이 된다. grill-me 스킬이 경고하는 speculative implementation 패턴이다.

#### Anti-goals

- 코어에 플레이어/엔티티/인벤토리/틱루프 상태 넣기 (D1)
- 압축 알고리즘 직접 구현 (libdeflate 조립)
- P4를 Phase 1 플랜의 태스크로 편성

#### Pitfalls

1. **"나중에 필요할 테니 미리" 논리.** P4를 근거로 코어에 상태 훅을 추가하는 것이 가장 흔한 오염 경로다. ADR-003 D1이 방어선이다.
2. **P3와 P4 혼동.** region 파일 쓰기(P3)는 코어 밖 CLI/모드 계층의 일이며 코어는 버퍼만 채운다. 이 경계가 흐려지면 D1이 무너진다.

#### Verification

- Phase 1~3 코드에 소켓·플레이어·틱루프 관련 심볼이 존재하지 않는다 (normative: [core-abi](specs/core-abi/spec.md))
- 코어 헤더에 상태 수명주기 API가 없다

#### When this might break

- P1~P3가 완료되고 사용자가 P4를 실제 목표로 승격하는 경우 — 그때 본 ADR을 supersede하고 ADR-003 D5(fallback)의 대체 전략을 새로 설계해야 한다

#### References

- ADR-003 (fallback 전략, 모듈 경계 — [core-abi/context.md](specs/core-abi/context.md))
- https://github.com/zlib-ng/zlib-ng/issues/1486 (압축 라이브러리 벤치)
- https://minecraft.wiki/w/Java_Edition_protocol/Packets
