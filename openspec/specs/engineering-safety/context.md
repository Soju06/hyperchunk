# engineering-safety: Context

## Purpose & scope

[spec.md](spec.md) is the normative SSOT for language identity, the safety
net, and the gate chain. This document holds the decision history of "why
C11 + sanitizers instead of Rust" (ADR-009).

## Current state (as of 2026-08)

- The sanitizer gates are in continuous operation (`check_sanitizers.sh`
  ASan+UBSan, `check_tsan.sh` TSan; TSan is only stable with ASLR disabled
  via `setarch -R`). There is a P2-1 history of restoring the gate itself
  after it had been left in an always-FAIL state
  ([changes/archive/](../../changes/archive/) phase2 P2-1 notes); a gate is
  trusted by its record of green runs, not by its existence.
- The ASan fake-stack 32B alignment trap (P2-10), and the case where PT_TLS
  8.6MB against the 8MB stack budget made large `_Thread_local` variables die
  instantly at spawn (P2-8 through P2-9), are also recorded in the phase2
  notes.
- CI (the 25-test subset) was configured in GH-1 from measurements; the `-E`
  regex does not track new golden tests, so it needs a manual update whenever
  a test is added (gh-setup notes).

## Decision history

> append-only. Existing entries are never edited. When a decision changes,
> add a new entry and mark the previous one as `Superseded by`.

Placement: this document is the primary home of ADR-009. The full text of
ADR-002 D1 (pure C rewrite), which it reaffirms, lives in
[generation-pipeline/context.md](../generation-pipeline/context.md).

### ADR-009: Keep the implementation language pure C11, and import Rust's safety net as sanitizer gates instead (Phase 1, 2026-07-28)

**Status:** Decided
**Type:** Architecture / Identity
**Resolves:** Reaffirmation of ADR-002 D1 (Rust migration evaluated and rejected)

#### Context

Just before starting Task 4 (arena/SoA), the user raised a language
re-evaluation: "let's decide whether to go hard in C, optimizing all the way
down to memory management, or to water it down a bit with Rust" (owner
directive, translated from the Korean original). Before Task 4 was the last
low-cost point where a switch was possible (475 lines of existing C code).

The performance-axis investigation came out a draw: identical codegen ceiling
(AVX2/AVX-512 intrinsics in core::arch, AVX-512 stabilized in 1.89),
bounds-check overhead ~0 in practice, and index-based arena/SoA is the
standard idiom in Rust as well. On FP parity, rustc has a slight edge because
it disables FMA contraction by default (RFC 3514), but C ties with
-ffp-contract=off (already applied).

The real decision axes were four:
1. **Delegated-development risk (advantage Rust)**: silent OOB in LLM-written
   C corrupts terrain without crashing, producing "fake parity bugs". Parity
   debugging is this project's most expensive resource.
2. **features parallelization (Rust on the surface, ambiguous in practice)**:
   concurrent 3x3-neighbor mutation is the borrow checker's worst match, so
   it ends up unsafe anyway. Conflicts are avoided structurally via
   checkerboard scheduling (ADR-003), so the language guarantee is worth
   little.
3. **flex narrative (advantage C)**: ADR-001's product definition is a
   technical showcase, and ADR-002 records the user's words, translated from
   the Korean original: "the essence is writing it from the ground up,
   calculating CPI meticulously". "pure C11, zero dependencies" creates the
   FOMO.
4. **Consumer ecosystem (draw)**: either language can expose a C ABI.

The user's confirmation:

> "a let's go, let's just make art at the clock-cycle level" (owner
> directive, translated from the Korean original)

((a) = the "pure C11, zero dependencies, hand-tuned to the cycle" identity)

#### Decision (3 core decisions)

| # | Decision | Key point |
|---|---|---|
| D1 | **Stay pure C11** | Reaffirms ADR-002 D1. Project identity = hand-tuned C |
| D2 | **Introduce sanitizer gates** | Make an additional full-ctest run under the ASan+UBSan preset a standard gate. Add TSan starting with Phase 2 (multithreading). The effective substitute for Rust's safety net (axis 1) |
| D3 | **Debug-build bounds asserts** | Debug-only asserts on accessors such as hc_idx (release zero-cost) |

#### Anti-goals

- Rust migration (the axis-1 advantage is offset by D2/D3 plus per-stage
  golden gates)
- A C-kernel + Rust-shell hybrid (2 toolchains, internal FFI, diluted
  narrative: the worst combination)
- Performance compromises made to avoid unsafe

#### Pitfalls

1. Sanitizers only check executed paths. Golden test coverage is the ceiling
   on sanitizer effectiveness.
2. ASan builds are not targets of the FMA gate (instrumentation code is mixed
   in). The FMA gate applies to the release archive only.
3. TSan must be added at the point of entering Phase 2; if forgotten, data
   races in features parallelization go unguarded. *(Added in P2-3.)*

#### Verification

- The CMake preset asan-ubsan exists; the full ctest suite PASSes under the
  sanitizers
- Confirm asserts are disabled in the release build (no abort paths in
  objdump)

#### References

- ADR-001 (FOMO purpose: [project.md](../../project.md)), ADR-002 (C rewrite
  + CPI essence:
  [generation-pipeline/context.md](../generation-pipeline/context.md)),
  ADR-003 (structural avoidance via scheduling:
  [core-abi/context.md](../core-abi/context.md))
- https://rust-lang.github.io/rfcs/3514-float-semantics.html
- https://github.com/rust-lang/rust/issues/111137 (AVX-512 intrinsics)
