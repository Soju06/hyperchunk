# Contributing to hyperchunk

Thanks for considering a contribution. This document covers everything from
"clone" to "merged PR" without re-discovering the conventions yourself.

hyperchunk is a bit-exact reimplementation of Minecraft Java Edition 26.2
worldgen in pure C11. That premise makes this repo stricter than most C
projects: a change that is *almost* correct produces terrain that is *almost*
identical, which is worthless here. Read the
[invariants](#load-bearing-invariants) before writing code — violating any of
them is an automatic PR rejection, no matter how good the numbers are.

---

## Table of contents

1. [Development setup](#development-setup)
2. [Project layout](#project-layout)
3. [Load-bearing invariants](#load-bearing-invariants)
4. [Commit convention](#commit-convention)
5. [Merge gates](#merge-gates)
6. [Tests and golden data](#tests-and-golden-data)
7. [Pull request flow](#pull-request-flow)
8. [Security issues](#security-issues)

---

## Development setup

Requirements: CMake ≥ 3.28, GCC with C11 and AVX-512 flag support (any
GCC ≥ 9; the AVX2/AVX-512/SHA-NI translation units are always *compiled* on
x86-64 regardless of host CPU — execution is behind runtime `cpuid`
dispatch), `python3` (stdlib only), `bash`. There are zero third-party
library dependencies ([ADR-003](../openspec/specs/core-abi/context.md) D1):
the core links against
`libm` and pthreads only.

```bash
git clone https://github.com/Soju06/hyperchunk.git
cd hyperchunk
cmake -S . -B build                 # default preset: -O2, asserts on
cmake --build build -j"$(nproc)"    # must be zero-warning: -Wall -Wextra -Werror is unconditional
ctest --test-dir build --output-on-failure
```

On a fresh clone, expect 12 of the 37 tests to fail with `GOLDEN/SETUP
ERROR` — they need local-only golden captures that are deliberately not
tracked. See [Tests and golden data](#tests-and-golden-data) for which tests
run from tracked data alone (that subset is what CI runs) and how to obtain
the rest.

Build presets (`CMakePresets.json`):

| Preset | Build dir | Purpose |
|---|---|---|
| `default` | `build/` | -O2, asserts on. The FMA-gated release archive — `scripts/check_no_fma.sh` judges `build/core/libhyperchunk.a` |
| `asan-ubsan` | `build-asan/` | ASan+UBSan gate build (ADR-009 D2). Never FMA-gated — instrumentation pollutes the disassembly |
| `tsan` | `build-tsan/` | ThreadSanitizer gate build for the FREE scheduler |
| `release` | `build-release/` | NDEBUG + -O3 |
| `bench-o2` / `bench-o3` | `build-bench-*/` | Release + `-g` for perf symbolization |

The floating-point flags `-ffp-contract=off` and `-fno-fast-math` are not
cache variables and not negotiable — see the invariants table.

## Project layout

```
core/         pure C11 compute library, C ABI, zero dependencies — the product
cli/          hyperchunk-verify and friends (parity verification, region output)
bench/        hyperchunk-bench (FREE/REPLAY benchmark driver, ISA selection)
tests/        unit/ and parity/ suites (37 ctest cases)
tools/golden/ vanilla-side capture harness (stage-dump mod, extractors) — regenerates golden data
tools/viz/    demo renderer and capture tooling
golden/       parity anchors: tracked hashes + order manifests; heavy payloads are local-only
reference/    extracted 26.2 datapack worldgen JSON + structure NBT (tracked, read by tests)
scripts/      gate scripts (check_no_fma, check_sanitizers, check_tsan, parity_gate, check_isa_equiv)
openspec/     spec-driven SSOT — capability specs (spec.md), decision history (context.md), archived change notes
```

The decision history in `openspec/` (`project.md` and each capability's
`context.md`) is **append-only**: existing ADR entries are never edited. If
a decision changes, a new entry supersedes the old one and says so, and the
`spec.md` update goes through an `openspec/changes/` change. PRs that
rewrite decision history are rejected regardless of content.

## Load-bearing invariants

These come from the ADRs preserved as decision history in
[openspec/](../openspec/project.md) (normative form in each capability's
`spec.md`). They are not
style preferences; each one is load-bearing for the product claim ("your
world, bit-identical, N× faster"). **Violating any of them is an automatic
PR rejection.**

| # | Invariant | Rule | ADR |
|---|---|---|---|
| 1 | Bit-exact parity | Output must byte-match vanilla 26.2 within the gate scope. "Statistically similar" terrain is a bug, not a feature. No float downcasts of double paths | ADR-002 D3, ADR-007 |
| 2 | FMA prohibition | No FMA contraction on any value path: `-ffp-contract=off` stays, no `vfmadd*` in the release archive, no `-flto` (it moves contraction to link time and blinds the disassembly gate) | ADR-002 P1, ADR-004 D3 |
| 3 | RNG consumption order | LCG/Xoroshiro128++ call order is a contract. Any reordering — loop restructuring, batching, early-exit — that changes consumption order changes worldgen | ADR-002 P2 |
| 4 | Region-level ABI | The public API surface is region-granularity only. Node-level functions are never exported (18.5%/chunk boundary-cost cliff) | ADR-003 D2 |
| 5 | Pure compute core | `core/` knows no file I/O, no network, no player/entity/tick state. Consumers pass buffers in and get buffers out | ADR-003 D1 |
| 6 | Whole-chunk fallback | An unknown datapack/mod extension sends the *whole chunk* to the vanilla path. Sub-chunk fallback breaks RNG order (see #3) | ADR-003 D5 |
| 7 | Two-tier parity gate | Stages 01–06 must be byte-equal to golden dumps unconditionally (order-independent); stages 07+ must be byte-equal under order-replay of a golden manifest | ADR-007 |
| 8 | Dual-mode scheduler | FREE (bench) and REPLAY (verification) share the same stage code; only the scheduling policy differs. A verification-only slow path invalidates the proof | ADR-008 |
| 9 | SIMD backends byte-identical | scalar/AVX2/AVX-512 must produce byte-identical output, CI/gate-enforced (`scripts/check_isa_equiv.sh`) | ADR-004 D4 |

Known traps that follow from these (each has burned a real debugging session):

- JVM `Math.sin`/`Math.cos` are HotSpot intrinsics, **not** libm — they can
  differ from glibc by 1 ulp. Any vanilla `Math.*` value path must go through
  `hc_jdk_sin`/`hc_jdk_cos`, never `sin`/`cos`.
- Vanilla parses float literals with `Float.parseFloat(raw)`. Parsing the
  same literal as double and narrowing (`strtod` → `(float)`) double-rounds:
  1-ulp datapack gap. Keep the raw literal and `strtof` it.
- Sanitizer builds (`build-asan/`, `build-tsan/`) are **never** the target of
  `check_no_fma.sh` — instrumentation makes the disassembly check
  meaningless. The FMA gate judges the release archive (`build/`) only.

## Commit convention

Formalized from this repo's actual history (187 commits, EN/KR mixed).
Conventional-commits style:

```
type(scope): subject
```

- **Types** (closed set): `feat`, `fix`, `perf`, `test`, `bench`, `viz`,
  `brand`, `docs`, `notes`, `chore`, `golden`, `build`, `ci`.
  - `notes` = completion/analysis notes; `bench` = benchmark tooling or
    measurements; `viz` = demo/renderer/capture; `golden` = golden-data
    process changes (its own reviewed flow — see below); `brand` = visual
    identity assets.
  - History also contains `wip`, `hygiene`, `refactor`, `diag`, `ref` —
    these are legacy and not accepted in new commits (fold refactors into
    `perf`, `fix`, or `chore`; `wip` never lands on `main`).
- **Scope** is a lowercase free-form token (`[a-z0-9][a-z0-9_./+-]*`),
  optional. Recommended scopes: the core stage names (`noise`, `surface`,
  `carvers`, `features`, `light`, `region`), plus `simd`, `sched`, `cli`,
  `viz`, `capture`, `git`, `readme`.
- **Subject language**: English only. (History up to 2026-08-20 is EN/KR
  mixed; the convention is forward-only and old commits are not rewritten.)
- **Compound commits**: `bench(micro)+notes(bench): ...` is legal when one
  logic unit genuinely spans two areas. Rare by design.
- **Body** optional. When the commit implements or is constrained by a
  decision, reference the ADR (`ADR-007`) in subject or body.

CI enforces this on every PR via `scripts/ci/check_commit_msgs.py` (stdlib
Python, no Node toolchain). Check locally:

```bash
git log --no-merges --format=%s origin/main..HEAD | python3 scripts/ci/check_commit_msgs.py --stdin
```

(`--no-merges` matters: real merge commits are filtered by git, and the
validator deliberately has no textual "Merge ..." exemption to evade.)

## Merge gates

Every PR must clear all of these before merge, regardless of author. CI
covers the subset it can (see [Tests and golden data](#tests-and-golden-data));
the rest are run locally and the PR template asks you to attest to them.

- [ ] **Build is zero-warning.** `-Wall -Wextra -Werror` is unconditional;
      there is no warning-tolerant configuration.
- [ ] **Full ctest suite green locally** (37/37 on a machine with local
      golden data). CI green alone is *not* sufficient for core-affecting
      changes — CI cannot run the 12 golden-dependent parity suites.
- [ ] **`scripts/check_no_fma.sh` PASS** — release archive (`build/`) only,
      never sanitizer builds.
- [ ] **`scripts/check_sanitizers.sh` PASS** (full suite under ASan+UBSan).
      For scheduler/threading changes, `scripts/check_tsan.sh` too.
- [ ] **`scripts/parity_gate.sh` PASS for any core-affecting change** —
      full-region canonical payload hash against `golden/SHA256SUMS`.
- [ ] **`scripts/check_isa_equiv.sh` PASS when touching SIMD kernels**
      (3-way needs an AVX-512 host; 2-way minimum locally).
- [ ] **No `golden/` edits in feature PRs.** Golden regeneration is its own
      reviewed process via `tools/golden/` and ships as a dedicated
      `golden:` PR with capture provenance.
- [ ] **Decision history is append-only.** New superseding entries only
      (via an `openspec/changes/` change); never edit or delete
      existing ones.
- [ ] **Commit subjects conform** to the convention above (CI-enforced).
- [ ] **Perf claims carry evidence**: bench numbers labeled with stage and
      machine (e.g. `noise, hc-e6`), FREE vs REPLAY mode stated. Bench
      numbers are always FREE mode; parity claims are always REPLAY mode —
      mixing them up in a PR description is an instant re-request (ADR-008).

## Tests and golden data

The 37-test ctest suite splits by data dependency:

- **25 tests run from tracked data alone** — unit suites plus the parity
  tests whose anchors (`golden/rng/*.txt`, `golden/SHA256SUMS`, order
  manifests, `reference/` JSON) are tracked in git. This is exactly the
  subset CI runs (the exclusion list lives in
  [`.github/workflows/ci.yml`](workflows/ci.yml) with per-test justification).
  One honest footnote: `nbt_read` *passes* on a fresh clone but vacuously —
  it skips its gitignored `golden/structures/*.starts.nbt` inputs by design
  and only does real work on a machine that has them.
- **12 tests need local-only golden captures** and are NOT runnable in CI —
  contributors touching core stages must run them locally:

| Local-only test | Missing data (gitignored) |
|---|---|
| `noise_stage`, `surface_stage`, `carvers_stage` | `golden/stages/**` stage dumps |
| `features_walk`, `light_stages`, `spawn_full` | `golden/stages/**` (+ `stages-alt/**`) dumps **and** `golden/features-trace/**` payloads |
| `region_out` (+ `region_out_roundtrip`, `region_out_residuals`, which consume its ctest fixture) | `golden/features-trace/**` payloads, `golden/region-ref/`, `golden/*.mca` |
| `full_region`, `free_region_golden`, `free_region_own` | `golden/region-ref/`, `golden/region-ref-margin/`, `golden/structures/` |

Why not track them? The heavy payloads are hundreds of MB, and the 07+
bundles are *coherent pairs* (dumps + the order manifest of the run that
produced them) that cannot be regenerated as "the same bundle" — vanilla's
feature-decoration order is nondeterministic run-to-run (ADR-007). Tracked
hashes (`golden/SHA256SUMS`, per-bundle `SHA256SUMS`) pin the local data.

Regenerating golden data is a separate, reviewed process: an instrumented
vanilla 26.2 server (JDK 25) run through `tools/golden/`, landed as a
dedicated `golden:` commit with capture provenance. Feature PRs never touch
`golden/`.

## Pull request flow

1. Branch from `main`. Keep the PR one logic unit — split unrelated changes.
2. Make commits following the [convention](#commit-convention).
3. Run the [merge gates](#merge-gates) that apply to your change; record the
   results in the PR template.
4. Open the PR using the template. Link the owning openspec capability
   (`spec.md` requirement or `context.md` decision-history entry) if the
   change implements or affects a recorded decision; if it *changes* a
   decision or behavior, the PR must carry an `openspec/changes/` change and
   append a new superseding decision-history entry (append-only).
5. CI (build + tracked-data test subset + FMA gate + sanitizers + commitlint)
   must be green. The maintainer reviews and merges; for core-affecting
   changes expect to be asked for local parity-gate evidence.

## Security issues

Do not open public issues for security vulnerabilities. Report privately via
[GitHub Security Advisories](https://github.com/Soju06/hyperchunk/security/advisories/new)
— see [SECURITY.md](SECURITY.md).
