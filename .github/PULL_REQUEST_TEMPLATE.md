<!--
Thanks for contributing to hyperchunk.
Fill in the sections below; delete sections that genuinely don't apply.
Read .github/CONTRIBUTING.md first — especially the load-bearing invariants.
-->

## Summary

<!-- One or two sentences: what does this PR change and why? -->

## ADR relevance

<!-- hyperchunk records every design decision as append-only decision history
     in openspec/ (project.md + specs/<capability>/context.md).
     If this PR implements, is constrained by, or changes a decision, link it. -->

- [ ] Not decision-relevant (mechanical fix, docs, tooling)
- [ ] Implements / constrained by an existing ADR entry: <!-- e.g. ADR-007, openspec/specs/worldgen-parity/ -->
- [ ] Changes a decision → this PR **adds a new superseding decision-history entry via an `openspec/changes/` change** (never edits an existing one)

## Gates

<!-- Check every gate you ran; CI can only cover the tracked-data subset.
     "As applicable": docs-only PRs may check none of these; core-affecting
     PRs are expected to check all of them. See CONTRIBUTING.md § Merge gates. -->

- [ ] `cmake --build build -j` — zero warnings (`-Werror` is unconditional)
- [ ] `ctest --test-dir build` — full suite green locally (37/37; requires local golden data)
- [ ] `scripts/check_no_fma.sh` PASS (release archive `build/` — never sanitizer builds)
- [ ] `scripts/check_sanitizers.sh` PASS (ASan+UBSan)
- [ ] `scripts/check_tsan.sh` PASS (required for scheduler/threading changes)
- [ ] `scripts/parity_gate.sh` PASS (required for any core-affecting change)
- [ ] `scripts/check_isa_equiv.sh` PASS (required when touching SIMD kernels; state 2-way or 3-way)
- [ ] N/A — no gate applies (docs/meta only; say why below)

## Scope audit

<!-- These are hard fences; the PR is rejected if they're crossed. -->

- [ ] This PR does **not** touch `golden/` (golden regeneration is its own reviewed `golden:` process)
- [ ] This PR does **not** edit existing decision-history entries in `openspec/` (append-only; new superseding entries are fine)
- [ ] Commit subjects follow the convention (`scripts/ci/check_commit_msgs.py`; CI-enforced)

## Perf claims

<!-- Delete this section if the PR makes no performance claim.
     Numbers without stage + machine + mode labels are not accepted:
     bench numbers are FREE mode, parity is REPLAY mode (ADR-008). -->

| Stage | Machine | Mode | Before | After |
|---|---|---|---|---|
|  |  | FREE |  |  |

## Test plan

<!-- What you ran and the relevant output. For parity-touching changes,
     paste the gate verdict lines (e.g. "PASS: bit-exact parity"). -->

```
```
