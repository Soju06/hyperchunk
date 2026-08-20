# AGENTS

hyperchunk: 바닐라 Minecraft Java Edition 26.2 오버월드 월드젠의 비트정확
재구현 (pure C11, libc/libm/pthreads only, C ABI). 이 파일은 사람/AI
기여자가 이 리포에서 일하는 방식의 진입점이다.

## Workflow (OpenSpec-first)

이 리포는 **OpenSpec이 primary 워크플로이자 SSOT**다.

1) `openspec/specs/<capability>/spec.md`에서 관련 스펙을 찾고 그것을
   source-of-truth로 삼는다. capability 인덱스는
   [openspec/project.md](openspec/project.md).
2) 동작·요구사항·계약·스키마를 바꾸는 작업이면: 구현 전에
   `openspec/changes/<change>/`에 change를 먼저 만든다 (proposal → tasks).
3) 구현하면서 코드와 스펙을 동기화한다 (`spec.md` 갱신 포함).
4) 로컬 검증: `openspec validate --specs --strict`
5) 완료 시: 검증 후 `openspec/changes/archive/YYYY-MM-DD-<change>/`로
   보관한다 (미검증 change는 archive 금지).

### Source of truth

- **Specs (normative, MUST/SHALL + scenarios)**: `openspec/specs/<capability>/spec.md`
- **근거·수치·결정 역사**: 같은 폴더 `context.md`의 "Decision history" —
  **append-only**. 기존 ADR 항목은 절대 수정하지 않는다. 결정이 바뀌면 새
  항목을 추가하고 이전 항목을 `Superseded by`로 표시하며, spec.md 반영은
  openspec change를 통해서만 한다. (구 append-only ADR 로그의 규칙 계승 —
  ADR-001~009는 project.md와 각 capability context.md로 이관됨.)
- **방향 결정 (제품 목적·스코프)**: `openspec/project.md`
- **Archived changes**: `openspec/changes/archive/YYYY-MM-DD-<slug>/` —
  Phase 1~2·bench·viz의 태스크 노트 전량이 여기 있다. 과거 작업의 사실
  관계는 여기서 먼저 찾는다.

## Build & gate chain

```bash
cmake -S . -B build && cmake --build build -j"$(nproc)"   # zero-warning: -Werror 무조건
ctest --test-dir build --output-on-failure                # 37 tests (fresh clone은 12개 GOLDEN/SETUP ERROR가 정상)
./scripts/check_no_fma.sh        # release 아카이브(build/)만. sanitizer 빌드 금지
./scripts/check_sanitizers.sh    # ASan+UBSan 풀 스위트
./scripts/check_tsan.sh          # 스케줄러/스레딩 변경 시 필수
./scripts/parity_gate.sh         # core 영향 변경 시 필수 — canonical hash vs golden/SHA256SUMS
./scripts/check_isa_equiv.sh     # SIMD 커널 변경 시 필수 (로컬 2-way 최소)
```

- CI는 추적 데이터만으로 도는 25-테스트 서브셋. golden 의존 12개 스위트는
  로컬 전용 — core를 건드리면 로컬 풀 스위트가 머지 조건이다.
- 게이트 목록·머지 조건의 운영 문서는
  [.github/CONTRIBUTING.md](.github/CONTRIBUTING.md) § Merge gates,
  normative 정의는 [openspec/specs/engineering-safety/spec.md](openspec/specs/engineering-safety/spec.md).

## Commit convention

`type(scope): subject` — 13-타입 닫힌 집합
(`feat fix perf test bench viz brand docs notes chore golden build ci`).
상세는 [.github/CONTRIBUTING.md](.github/CONTRIBUTING.md) § Commit
convention. 로컬 사전 검증:

```bash
git log --no-merges --format=%s origin/main..HEAD | python3 scripts/ci/check_commit_msgs.py --stdin
```

## Claim fence (불변 — 요약하다 숫자·문구를 바꾸지 말 것)

normative 전문: [openspec/specs/benchmarks-and-viz/spec.md](openspec/specs/benchmarks-and-viz/spec.md)

- 공개 수치 (hc-e6, Zen5 32 vCPU, r.0.0 1024청크, 3런 중앙값):
  vanilla **11.9 s** / C2ME **3.7 s** / REPLAY **3.155 s** / FREE
  **0.894 s** = **13.3x** vs vanilla, **4.14x** vs C2ME.
- 헤드라인 배율에는 하한 병기: **≥12.5x** / **≥3.3x**. 배율은 무대 귀속 —
  임의 하드웨어로 일반화 금지.
- REPLAY는 "**canonical-identical output at C2ME-class speed**"만.
  "faster than C2ME" 우열 클레임 금지 (1.17x는 오차 밴드 안).
- canonical 해시: golden `a5963205…` (raw `.mca` 비트 일치는 원리적 불가 —
  정의는 [openspec/specs/worldgen-parity/spec.md](openspec/specs/worldgen-parity/spec.md)).
- 벤치 숫자는 항상 FREE, 패리티 클레임은 항상 REPLAY 모드 표기.

## Hard fences

- `golden/` 은 feature PR에서 불가침 — 재생성은 별도 `golden:` 프로세스.
- FMA 금지 (`-ffp-contract=off`, no `-flto`), RNG 소비 순서, 리전 단위
  ABI, 순수 계산 코어 등 load-bearing invariant 목록:
  [.github/CONTRIBUTING.md](.github/CONTRIBUTING.md) § Load-bearing
  invariants (normative는 각 capability spec.md).
