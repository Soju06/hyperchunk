# Phase 3: Fabric FFM bridge

## Why

Phases 1-2 proved the thesis at the CLI only: bit-exact 26.2 overworld worldgen in pure C11, canonical-gated, with the public bench numbers recorded in benchmarks-and-viz. No real server can consume libhyperchunk yet. The consumer matrix names a Fabric-side Java bridge over FFM as Phase 3 (core-abi spec, "Phase 3, not yet started"; ADR-006 D4), and both downstream consumer tracks (Rust FFI, Paper) are explicitly sequenced behind Fabric validation (core-abi context). The owner has approved starting Phase 3; this change is the plan of record: proposal, design, spec deltas, and ordered tasks. No implementation lands in this change.

## What Changes

- **New capability `fabric-bridge`**: the normative contract for a Fabric 26.2 server mod that consumes libhyperchunk over Java FFM at region-batch granularity, serves the core's canonical serialized output through the vanilla chunk load path (pregen-then-load), detects unsupported worldgen configurations and yields whole dimensions to vanilla, stays inert by default, joins the parity gate chain as the third gated consumer, and ships its native library inside the mod artifact (linux-x86-64 first).
- **core-abi**: ADDED requirement "Batch facade sufficiency" — today `core/include/hyperchunk.h` exports only `hc_version`/`hc_abi_version` and the existing consumers compose region batches out of internal `core/src` headers; a real FFI consumer needs a complete public facade (init from caller-provided reference data, region batch execution with policy selection, serialized-output retrieval, error reporting). MODIFIED requirement "FFI consumers and bridge technology" — consumer-matrix status update plus a facade-only binding rule for bridges.
- **generation-pipeline**: MODIFIED requirement "Full pipeline, eleven stages" — structure starts and structure templates are classified as caller-provided batch inputs (vanilla-computed, jigsaw-assembled in Java, per the existing ADR-002 D4 exclusion of jigsaw assembly from core scope). This is how served worlds get fully assembled villages without expanding core scope.
- **Implementation tasks** (tasks.md, to be executed as follow-up PRs): public batch facade and region-addressing generalization in core; core standalone-input work (multi-noise biome assignment, buffer-fed structure starts); the bridge mod (Gradle project, FFM bindings, `.so` packaging, PT_TLS budget); pregen-then-load orchestration; `bridge_parity_gate.sh`; fallback detection; a fresh, reviewed bridge bench note.
- **Not changing**: the claim fence (all public numbers stay verbatim); the region-granularity ABI invariant (reaffirmed, not relaxed); P4 network/packet scope (stays out per ADR-005); `golden/` content (any new capture rides the separate `golden:` process).

## Capabilities

### New Capabilities

- `fabric-bridge`: contract for the Fabric server bridge consumer — FFM region-batch consumption, canonical serving via the vanilla load path, unsupported-config detection and yield, inert-by-default safety, the bridge parity gate, and the runtime/packaging envelope.

### Modified Capabilities

- `core-abi`: the public facade must become sufficient for facade-only consumers (new requirement), and the consumer-matrix requirement is updated for Phase 3 with a facade-only binding rule for bridges.
- `generation-pipeline`: the eleven-stages requirement is amended to name structure starts/templates as caller-provided batch inputs; jigsaw assembly remains out of core scope.

## Impact

- **Specs**: `openspec/specs/core-abi/spec.md`, `openspec/specs/generation-pipeline/spec.md` (on archive), new `openspec/specs/fabric-bridge/`.
- **Code (follow-up tasks, not this change)**: `core/include/hyperchunk.h` + new facade TU(s); region-addressing generalization across `core/src` and `cli/`; multi-noise biome assignment; buffer-fed structure starts (retiring the `fopen` paths inside `core/src/structures.c` / `structures_template.c`); `cli/`+`bench/` facade-only include cleanup; new top-level `bridge/fabric-mod/` Gradle project modeled on `tools/golden/stage-dump-mod`; a CMake SHARED (`.so`) target; `scripts/bridge_parity_gate.sh`.
- **Gates**: `check_no_fma.sh` extended to the `.so`; a new local-only bridge parity gate joins the merge checklist for bridge-affecting changes; `check_tsan.sh` applies to the facade/scheduler-ownership moves; CI stays on the tracked-data subset (new golden-dependent suites require manual `CTEST_EXCLUDE` updates per engineering-safety).
- **Dependencies**: no new third-party dependencies. The mod builds like the two existing mods (plain Gradle, Java 25, compileOnly against the extracted unobfuscated server jar, sponge-mixin only); the core keeps libc/libm/pthreads only.
- **Docs**: `.github/CONTRIBUTING.md` merge-gates section and README gain bridge sections at implementation time; all public claims remain inside the claim fence until a new reviewed bench note exists.
