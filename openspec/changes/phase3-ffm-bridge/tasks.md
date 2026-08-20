# Phase 3 tasks

Ordering: group 1 unblocks everything; groups 2 and 3 can proceed in
parallel after 1.1; group 4 needs 1.x, 2.4, 3.x; group 5 needs 4; group 6
needs 5; group 7 closes. Task 2.3 (golden capture) gates the second-region
acceptance criteria in 2.2 and the assembled-structure claim in 5.1. Per the
Phase 1 discipline, L-sized tasks are re-subdivided at start, after their
inputs (audits, captures) exist — subdividing them further now would be
guessing.

Sizes: S under a day, M one PR-sized unit of a few days, L needs its own
subdivision at start. Gates named per task are from the engineering-safety
merge chain; "full local suite" means ctest 37/37 with local golden data.

## 1. Public batch facade and region generalization (core)

- [ ] 1.1 Define and implement the public batch facade in `hyperchunk.h` (M)
  - Files: `core/include/hyperchunk.h`, new `core/src/abi_batch.c` (name
    final at implementation), `tests/` new facade-driver suite.
  - Scope: lifecycle init from caller-provided buffers (worldgen JSON
    closures, structure starts + template NBT, REPLAY order manifest) with
    `const char **err` reporting; region batch execution
    (seed, region coords, `hc_schedule_policy { FREE, REPLAY(manifest) }`,
    nthreads, caller arena backing); output retrieval (per-chunk serialized
    payloads and the in-memory region image); teardown. Bump
    `HC_ABI_VERSION` to 2. Facade declares region/batch-level entry points
    only (core-abi header-audit scenario).
  - Acceptance: a new ctest suite drives r.0.0 end to end through facade
    includes only and reproduces the canonical constant; public header audit
    shows no per-node/per-sample declarations.
  - Gates: zero-warning build, full local suite, check_sanitizers, no-fma,
    parity_gate.
- [ ] 1.2 Region-addressing generalization (L; audit first, subdivide at start)
  - Files: `core/src/*` (grid origin plumbing), `cli/hyperchunk_verify.c`
    (drop the `--region 0 0` hard-reject), manifest handling.
  - Scope: parameterize the 41x41 padded-grid origin end to end; written
    inventory of every (0,0) assumption found (audit note in this change
    folder).
  - Acceptance: all existing r.0.0 gates stay green (full local suite +
    parity_gate); a non-(0,0) region generates structurally (all chunks
    reach full promotion) — bit parity for a second region is gated on the
    2.3 capture, not this task.
  - Gates: zero-warning build, full local suite, check_sanitizers, no-fma,
    parity_gate; check_tsan if scheduler event/cell code is touched.
- [ ] 1.3 CLI and bench consume the facade only (M)
  - Files: `cli/hyperchunk_verify.c`, `bench/hyperchunk_bench.c`,
    `core/` (move FREE event-DAG construction behind the facade — today
    bench builds its own DAG, which violates the scheduler-ownership
    contract for consumers).
  - Acceptance: grep audit shows zero `../core/src` includes in `cli/` and
    `bench/`; parity_gate green; bench wall numbers within run-to-run noise
    (no public claim changes).
  - Gates: zero-warning build, full local suite, check_sanitizers, no-fma,
    parity_gate, check_tsan (scheduler ownership moves).

## 2. Core standalone inputs

- [ ] 2.1 Standalone-input audit (S)
  - Files: audit note in this change folder only.
  - Scope: document exactly which generation inputs come from golden
    directories today (quart biome grids/overlays, structure starts,
    template NBT, order/postprocess manifests, stage logs) vs computed
    in-core; the per-structure-type split between the core's placement scan
    and golden-start inputs; what a live server can and cannot supply.
  - Acceptance: reviewed note; 2.2/2.4 scoped from it.
  - Gates: none (documentation), but 2.2/2.4 must cite it.
- [ ] 2.2 Multi-noise biome assignment in core (L; subdivide after 2.1)
  - Files: `core/src/` new biome parameter-search TU(s), `tests/` biome
    stage suite.
  - Scope: bit-exact 26.2 multi-noise biome assignment (climate sampling
    already exists via the compiled noise router; the parameter-list search
    and tie-breaking must match 26.2 bytecode evidence exactly, per the
    primary-evidence rule).
  - Acceptance: `03_biomes` stage dumps byte-equal for r.0.0 with the golden
    biome inputs removed; second-region/second-seed probe once 2.3 lands.
  - Gates: zero-warning build, full local suite, check_sanitizers, no-fma,
    parity_gate; check_isa_equiv only if SIMD kernels are added.
- [ ] 2.3 Second-region and second-seed golden capture (M; `golden:` process,
      owner-approved)
  - Files: `golden/SHA256SUMS` + bundle pins, `tools/golden/` harness reuse;
    lands as a dedicated `golden:` commit with capture provenance — never in
    a feature PR.
  - Scope: capture at least one region containing a village (assembled
    jigsaw structure) and one non-(0,0) region; JDK 25 instrumented vanilla
    server per the worldgen-parity capture requirement.
  - Acceptance: bundles with provenance and tracked sha256 pins; any new
    golden-dependent ctest suites added to `CTEST_EXCLUDE` (engineering-
    safety: the -E regex does not track new golden tests).
  - Gates: golden-process review; CI untouched otherwise.
- [ ] 2.4 Buffer-fed structure starts and templates (M)
  - Files: `core/src/structures.c`, `core/src/structures_template.c`,
    facade input plumbing from 1.1, `cli/` file-loading shim.
  - Scope: replace the in-core `fopen` of golden starts/template
    directories with caller-provided buffers through the facade; the CLI
    loads the same files and feeds buffers (behavior-neutral for the gates).
  - Acceptance: full-region gate green with buffer-fed inputs; the core
    file-I/O deviation from the pure-compute-core requirement is retired
    (state/file-I/O audit clean).
  - Gates: zero-warning build, full local suite, check_sanitizers, no-fma,
    parity_gate.

## 3. Bridge native artifact and mod skeleton

- [ ] 3.1 Shared-library target and native gates (M)
  - Files: `core/CMakeLists.txt` (SHARED target, `-fPIC`, same
    parity-critical flags), `scripts/check_no_fma.sh` (extend to the `.so`),
    new ldd-audit helper if needed.
  - Scope: build `libhyperchunk.so` from the same sources/flags with
    TU-isolated AVX2/AVX-512/SHA-NI and cpuid runtime dispatch; measure
    PT_TLS of the `.so` and record it.
  - Acceptance: check_no_fma green on archive AND `.so`; ldd shows
    libc/libm/pthreads only; PT_TLS measured with a written budget — if it
    exceeds the budget, a TLS-diet follow-up task is filed before 4.x
    proceeds (dlopen static-TLS risk, design Decision 6).
  - Gates: zero-warning build, no-fma (extended), full local suite,
    check_sanitizers.
- [ ] 3.2 Fabric mod skeleton with FFM binding (M)
  - Files: new `bridge/fabric-mod/` Gradle project (plain `java` plugin,
    Java 25 toolchain, loader 0.19.3 pin via `tools/golden/fetch_fabric.sh`,
    compileOnly extracted server jar + sponge-mixin, per the stage-dump-mod
    template), `fabric.mod.json`, jar packaging of
    `natives/linux-x86-64/libhyperchunk.so`, extraction + `System.load`,
    hand-written FFM downcall handles for the facade, enable flag
    (system property), `--enable-native-access` runtime check.
  - Acceptance: fresh Fabric 26.2 server boots with the mod; ABI version
    handshake (`hc_abi_version`) logged when enabled; with the flag absent,
    boot log and behavior match a stock server (inertness smoke); on a
    platform/access failure the mod logs one line and stays inert.
  - Gates: Java side joins no C gate; the facade call sequence used by the
    mod is mirrored by the 1.1 ctest driver so sanitizers cover it.

## 4. Pregen-then-load serving

- [ ] 4.1 Vanilla structure-starts capture in Java (L; subdivide at start)
  - Files: `bridge/fabric-mod/` starts-capture module.
  - Scope: at pregen time, compute structure starts and references for the
    batch's padded chunk set by driving vanilla's own
    `createStructures`/`createReferences` logic, then serialize
    starts + templates into the facade input format (same shape as the
    golden dumps). Fallback path if direct driving proves too entangled
    (UNVERIFIED beyond signatures): advance real proto chunks to
    `structure_references` through the server's own scheduler and capture
    from them.
  - Acceptance: for r.0.0 seed 1234567890, produced starts byte-equal the
    golden starts dumps; for the 2.3 village region, starts byte-equal that
    capture.
  - Gates: bridge-side self-check harness; core untouched.
- [ ] 4.2 Region pregen orchestration and injection (M)
  - Files: `bridge/fabric-mod/` pregen command/config, dedicated batch
    thread, region-file write path into the dimension's region directory;
    guard so pregen only targets regions with no live chunk holders.
  - Acceptance: on a quiet server, pregen r.0.0 then forceload-sweep all
    1024 chunks — every chunk is served via the load path (loading-pyramid
    only; no generation-stage recomputation), reaches `minecraft:full`,
    zero unsafe-read/far-write log lines.
  - Gates: interactive smoke script (6.2 harness reused); no C gates
    touched.

## 5. Parity gate and fallback

- [ ] 5.1 `scripts/bridge_parity_gate.sh` (M)
  - Files: `scripts/bridge_parity_gate.sh`, harness reuse from
    `tools/golden/make_stage_dumps.sh` (quiet-server protocol, console
    FIFO), `.github/CONTRIBUTING.md` merge-gates row (docs, at landing).
  - Scope: REPLAY pregen (golden manifest) -> quiet-server load of all 1024
    chunks -> `save-all flush` -> canonical payload hash equals the pinned
    golden hash; FREE variant equals the own-v1 pin. Close vacuous-pass
    channels (missing goldens, zero chunks loaded, stale mod jar) the way
    existing gate scripts do.
  - Acceptance: two consecutive recorded green runs (gate-trust rule);
    deliberate mutation (e.g. corrupt one payload byte) turns the gate red.
  - Gates: this task creates a gate; local-only, required for
    bridge-affecting changes thereafter.
- [ ] 5.2 Unsupported-configuration detection and yield (M)
  - Files: `bridge/fabric-mod/` activation audit (dimension shape check,
    effective-worldgen export -> facade init, worldgen-registry namespace
    scan, chunk-system-rewrite mod detection), `tests/` synthetic fixtures.
  - Scope: implement the Decision 4 activation chain; build a synthetic
    test mod registering one Java feature; test a non-noise world preset.
  - Acceptance: both scenarios yield with a single clear log line, the
    bridge stays inert for the dimension, and the resulting worlds are
    vanilla-normal; the pure-JSON acceptance case (vanilla datapack set)
    activates.
  - Gates: bridge smoke harness; facade init error path covered by the 1.1
    sanitizer suite.

## 6. Demo and bench

- [ ] 6.1 Bridge pregen bench note (M)
  - Files: bench note in this change folder (archived with it), runner
    scripts under `bench/` or `tools/` as appropriate.
  - Scope: same machine, three configurations pregenerate the same
    fresh-region workload through a real server: vanilla (B-1
    forceload+poll protocol), Fabric+C2ME (same protocol), bridge pregen
    (own instrumentation + server-observable completion). Define the
    measurement windows against the B-1 trap list (poll resolution,
    streaming saves, spawn pre-generation, C2ME heap-derived workers,
    boot/setup symmetry) before publishing.
  - Acceptance: reviewed bench note with stage+machine+mode labels and
    error-deducted lower bounds; claim-fence numbers appear verbatim or not
    at all; REPLAY phrasing stays "canonical-identical output at C2ME-class
    speed"; README/public materials change only after review.
  - Gates: claim-fence review (benchmarks-and-viz).
- [ ] 6.2 Interactive smoke and demo capture (M)
  - Files: `bridge/` smoke script (console-FIFO harness), optional timeline
    capture reusing `chunk-timeline-mod`.
  - Scope: scripted join, stream across at least two region boundaries
    including the vanilla frontier; capture demo footage assets if the
    owner wants a bridge variant of the 3-way demo.
  - Acceptance: all served chunks `minecraft:full`; zero unsafe-read/
    far-write lines; checklist recorded; any demo asset carries measured
    provenance per benchmarks-and-viz.
  - Gates: smoke checklist; demo provenance rules.

## 7. Docs and archive

- [ ] 7.1 Spec sync, decision history, and archive (S)
  - Files: apply this change's deltas to `openspec/specs/` via the OpenSpec
    workflow; append decision-history entries (pregen-then-load,
    starts-as-inputs, facade) to the relevant `context.md` files
    (append-only); `.github/CONTRIBUTING.md` merge-gate and invariant rows;
    README bridge section (honest scope: pregen MVP, linux-x86-64, fallback
    behavior).
  - Acceptance: `openspec validate --specs --strict` green; archive to
    `openspec/changes/archive/YYYY-MM-DD-phase3-ffm-bridge/` per AGENTS.md;
    commit subjects pass the validator.
  - Gates: openspec validation; commit-subject validation.
