# simd-backends Specification

## Purpose

SIMD 백엔드 계약의 normative SSOT: 스칼라/AVX2/AVX-512 백엔드 구성과 런타임
디스패치, 백엔드 간 바이트동일 출력, FMA 금지와 그 게이트. 근거·수치·결정
역사는 [context.md](context.md).

## Requirements

### Requirement: Backend set and runtime dispatch

The core SHALL provide a scalar reference kernel, an AVX2 kernel (the
baseline; ADR-004 D1), and an AVX-512 kernel selected by `cpuid` runtime
dispatch (ADR-004 D2). An AVX-512-only build MUST NOT exist; on hosts
without AVX-512 the dispatcher MUST fall back to AVX2 transparently. SIMD
translation units are always compiled on x86-64 regardless of host CPU;
execution is gated by dispatch only.

#### Scenario: Host without AVX-512

- **GIVEN** a machine whose `cpuid` reports no AVX-512
- **WHEN** generation runs
- **THEN** the AVX2 kernel executes and output is identical to the AVX-512
  path's output on capable hardware

#### Scenario: ISA selection surface

- **WHEN** `hyperchunk-bench` runs with an explicit ISA selection
  (scalar / avx2 / avx512)
- **THEN** the requested backend executes, enabling per-backend comparison

### Requirement: Byte-identical output across backends

All backends MUST produce byte-identical output for identical inputs
(ADR-004 D4). This is gate-enforced by `scripts/check_isa_equiv.sh` (3-way on
an AVX-512 host; 2-way minimum locally) and is a merge requirement for any
change touching SIMD kernels.

#### Scenario: Cross-backend equivalence gate

- **GIVEN** a change touching any SIMD kernel
- **WHEN** `scripts/check_isa_equiv.sh` runs
- **THEN** scalar, AVX2, and (where hardware allows) AVX-512 outputs hash
  identically; any mismatch rejects the change

### Requirement: FMA prohibition on all value paths

FMA contraction MUST be disabled on every backend and every value path
(ADR-002 P1, ADR-004 D3): `-ffp-contract=off` and `-fno-fast-math` are
unconditional and not cache variables; the release archive MUST contain no
`vfmadd*`/`vfmsub*` instructions; `-flto` MUST NOT be used (it moves
contraction to link time and blinds the disassembly gate). Contraction
removes an intermediate rounding and silently changes terrain bits.

#### Scenario: Disassembly gate

- **WHEN** `scripts/check_no_fma.sh` runs against the release archive
  (`build/core/libhyperchunk.a`)
- **THEN** it finds zero FMA instructions, and the gate fails the build
  otherwise

#### Scenario: Sanitizer builds excluded from the FMA gate

- **GIVEN** the `asan-ubsan` or `tsan` build directories
- **WHEN** the FMA gate is configured
- **THEN** those builds are never its target — instrumentation pollutes the
  disassembly; the gate judges the release archive only
