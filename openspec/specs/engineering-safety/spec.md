# engineering-safety Specification

## Purpose

구현 언어 정체성과 안전망의 normative SSOT: 순수 C11 유지, sanitizer 게이트,
zero-warning 빌드, 머지 게이트 체인. 근거·수치·결정 역사는
[context.md](context.md).

## Requirements

### Requirement: Pure C11, zero dependencies

The implementation language SHALL remain pure C11 (ADR-002 D1, reaffirmed
against a Rust migration in ADR-009 D1). No Rust conversion, no C-kernel +
Rust-shell hybrid, no third-party libraries. The project identity is
"pure C11, zero dependencies, hand-tuned to the cycle".

#### Scenario: Language boundary audit

- **WHEN** the build is inspected
- **THEN** all core translation units are C11 compiled by a single C
  toolchain, linking libc/libm/pthreads only

### Requirement: Sanitizer gates substitute the Rust safety net

The full ctest suite MUST pass under ASan+UBSan
(`scripts/check_sanitizers.sh`, preset `asan-ubsan`; ADR-009 D2). Scheduler
and threading changes MUST additionally pass TSan (`scripts/check_tsan.sh`,
preset `tsan`). Sanitizer builds are never the target of the FMA gate
([simd-backends](../simd-backends/spec.md)).

#### Scenario: Sanitizer gate run

- **WHEN** `scripts/check_sanitizers.sh` runs
- **THEN** the full suite passes under ASan+UBSan with zero reports

#### Scenario: Threading change

- **GIVEN** a change touching the FREE scheduler or any threading code
- **WHEN** the merge gates run
- **THEN** `scripts/check_tsan.sh` passes before merge

### Requirement: Debug bounds asserts, release zero-cost

Accessor paths (e.g. `hc_idx`) SHALL carry debug-build bounds asserts
(ADR-009 D3) that compile to nothing in release builds (NDEBUG).

#### Scenario: Release build cost audit

- **WHEN** the release build is disassembled
- **THEN** no assert/abort paths remain on accessor hot paths

### Requirement: Zero-warning build

`-Wall -Wextra -Werror` are unconditional; a warning-tolerant configuration
MUST NOT exist.

#### Scenario: Warning introduced

- **WHEN** a change introduces any compiler warning
- **THEN** the build fails

### Requirement: Merge gate chain

Every merge MUST clear the applicable gates (.github/CONTRIBUTING.md § Merge
gates is the operational checklist): zero-warning build; full local ctest
(37/37 with local golden data — CI covers the 25-test tracked-data subset
only); `check_no_fma.sh`; `check_sanitizers.sh` (+ `check_tsan.sh` for
threading); `parity_gate.sh` for core-affecting changes; `check_isa_equiv.sh`
for SIMD changes; commit-subject validation
(`scripts/ci/check_commit_msgs.py`, CI-enforced).

#### Scenario: Core-affecting PR

- **GIVEN** a PR touching `core/`
- **WHEN** the merge gates are evaluated
- **THEN** CI green alone is insufficient — local full-suite and parity-gate
  evidence is required
