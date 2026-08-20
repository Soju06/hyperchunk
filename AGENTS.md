# AGENTS

hyperchunk: a bit-exact reimplementation of vanilla Minecraft Java Edition
26.2 overworld worldgen (pure C11, libc/libm/pthreads only, C ABI). This
file is the entry point for how human/AI contributors work in this repo.

## Workflow (OpenSpec-first)

In this repo, **OpenSpec is the primary workflow and the SSOT**.

1) Find the relevant spec in `openspec/specs/<capability>/spec.md` and treat
   it as the source of truth. The capability index is
   [openspec/project.md](openspec/project.md).
2) If the work changes behavior, requirements, contracts, or schemas: create
   a change in `openspec/changes/<change>/` before implementing
   (proposal -> tasks).
3) Keep code and spec in sync while implementing (including updating
   `spec.md`).
4) Local validation: `openspec validate --specs --strict`
5) On completion: after validation, archive to
   `openspec/changes/archive/YYYY-MM-DD-<change>/`
   (never archive an unvalidated change).

### Source of truth

- **Specs (normative, MUST/SHALL + scenarios)**: `openspec/specs/<capability>/spec.md`
- **Rationale, numbers, and decision history**: the "Decision history" in the
  same folder's `context.md`, which is **append-only**. Never edit an
  existing ADR entry. When a decision changes, add a new entry, mark the
  previous one `Superseded by`, and reflect it into spec.md only through an
  openspec change. (This inherits the rules of the old append-only ADR log;
  ADR-001 to ADR-009 were migrated into project.md and each capability's
  context.md.)
- **Direction decisions (product purpose and scope)**: `openspec/project.md`
- **Archived changes**: `openspec/changes/archive/YYYY-MM-DD-<slug>/`.
  All task notes from Phase 1-2, bench, and viz live here. Look here first
  for the facts of past work.

## Build & gate chain

```bash
cmake -S . -B build && cmake --build build -j"$(nproc)"   # zero-warning: -Werror, no exceptions
ctest --test-dir build --output-on-failure                # 37 tests (12 GOLDEN/SETUP ERRORs are normal on a fresh clone)
./scripts/check_no_fma.sh        # release archive (build/) only. never sanitizer builds
./scripts/check_sanitizers.sh    # ASan+UBSan full suite
./scripts/check_tsan.sh          # required for scheduler/threading changes
./scripts/parity_gate.sh         # required for core-affecting changes: canonical hash vs golden/SHA256SUMS
./scripts/check_isa_equiv.sh     # required for SIMD kernel changes (local 2-way minimum)
```

- CI is a 25-test subset that runs on tracked data only. The 12
  golden-dependent suites are local-only; if you touch core, the local full
  suite is a merge condition.
- The operational doc for the gate list and merge conditions is
  [.github/CONTRIBUTING.md](.github/CONTRIBUTING.md) section Merge gates;
  the normative definition is [openspec/specs/engineering-safety/spec.md](openspec/specs/engineering-safety/spec.md).

## Commit convention

`type(scope): subject`, with a closed set of 13 types
(`feat fix perf test bench viz brand docs notes chore golden build ci`).
Details in [.github/CONTRIBUTING.md](.github/CONTRIBUTING.md) section Commit
convention. Local pre-check:

```bash
git log --no-merges --format=%s origin/main..HEAD | python3 scripts/ci/check_commit_msgs.py --stdin
```

## Claim fence (immutable: do not change numbers or wording when summarizing)

Normative full text: [openspec/specs/benchmarks-and-viz/spec.md](openspec/specs/benchmarks-and-viz/spec.md)

- Public numbers (hc-e6, Zen5 32 vCPU, r.0.0 1024 chunks, median of 3 runs):
  vanilla **11.9 s** / C2ME **3.7 s** / REPLAY **3.155 s** / FREE
  **0.894 s** = **13.3x** vs vanilla, **4.14x** vs C2ME.
- Headline multipliers must state the conservative lower bounds alongside:
  **>=12.5x** / **>=3.3x**. The multipliers are properties of this stage;
  do not generalize to arbitrary hardware.
- For REPLAY, only "**canonical-identical output at C2ME-class speed**".
  No "faster than C2ME" superiority claims (the 1.17x is inside the error
  band).
- Canonical hash: golden `a5963205...` (raw `.mca` bit-equality is
  impossible in principle; the definition is in
  [openspec/specs/worldgen-parity/spec.md](openspec/specs/worldgen-parity/spec.md)).
- Bench numbers are always FREE; parity claims always name REPLAY mode.

## Hard fences

- `golden/` is off-limits in feature PRs; regeneration goes through the
  separate `golden:` process.
- FMA prohibition (`-ffp-contract=off`, no `-flto`), RNG consumption order,
  region-granularity ABI, pure compute core, and the rest of the
  load-bearing invariant list:
  [.github/CONTRIBUTING.md](.github/CONTRIBUTING.md) section Load-bearing
  invariants (normative in each capability's spec.md).
