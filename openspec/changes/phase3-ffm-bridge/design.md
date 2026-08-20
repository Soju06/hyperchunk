# Phase 3 design: Fabric FFM bridge

## Context

See proposal.md for motivation. Constraints that shape everything below:

- **ABI**: the public C ABI is region-granularity batch only; node-level entry
  points are never declared (core-abi "Region-granularity ABI only", ADR-003
  D2: 84,000 crossings/chunk = 18.5% of chunk time = fatal; region granularity
  = 0.02 ns/chunk). The core owns the arena allocator and the batch scheduler
  (ADR-003 D3); scheduling policy is a library contract,
  `hc_schedule_policy { FREE, REPLAY(manifest) }` (scheduler spec, ADR-008 P1).
- **Facade reality**: `core/include/hyperchunk.h` today declares only
  `hc_version`/`hc_abi_version` (`HC_ABI_VERSION 1`). The ADR-recorded target
  shape `hyperchunk_batch(seed, region_list, out)` does not exist yet; the CLI
  and bench drivers compose region batches from internal `core/src` headers,
  and bench builds its own FREE event DAG. A real FFI consumer cannot exist
  until the facade does.
- **Standalone-input reality**: the core cannot yet generate an arbitrary
  region from a seed alone. Verified gaps: quart biomes are supplied by the
  golden `03_biomes` loader ("the only supplier", `core/src/hc_chunk.h`);
  structure starts and template NBT are read from golden dump directories via
  `fopen` inside `core/src/structures.c` / `structures_template.c`; REPLAY
  needs golden bundle artifacts (order manifest, stage logs) that exist only
  for captured regions; `hyperchunk-verify` hard-rejects any region other than
  (0,0). FREE mode's self-built event DAG is the model for server use.
- **Region shape**: one batch = region r.X.Z = 32x32 = 1024 output chunks,
  computed on a 41x41 padded grid (`HC_FEAT_REGION_N = 41`, chunk margin -5..35
  for r.0.0, decorable window -4..34).
- **26.2 server facts** (verified against local unobfuscated sources under
  `tools/golden/work/` — official Mojang names, no mappings, per ADR-006 D3):
  stage bodies are static methods on
  `net.minecraft.world.level.chunk.status.ChunkStatusTasks`, wired per status
  in `ChunkPyramid.GENERATION_PYRAMID`; each builds a `WorldGenRegion` and
  calls the `ChunkGenerator` held by the per-`ChunkMap` `WorldGenContext`
  record (`createStructures`, `createReferences`, `createBiomes`,
  `fillFromNoise`, `buildSurface`, `applyCarvers`, `applyBiomeDecoration`,
  `spawnOriginalMobs`); light goes to
  `ThreadedLevelLightEngine.initializeLight`/`lightChunk`. The per-step choke
  point `ChunkStep.apply(WorldGenContext, StaticCache2D<GenerationChunkHolder>,
  ChunkAccess)` is already mixin-hooked by both existing repo mods. The load
  path: `ChunkMap.applyStep` (EMPTY) -> `scheduleChunkLoad` ->
  `SerializableChunkData.parse`/`read`; a chunk saved with `Status=full`
  returns as an `ImposterProtoChunk` wrapping a live `LevelChunk`, every
  worldgen step becomes a passthrough via `ChunkPyramid.LOADING_PYRAMID`,
  saved per-section light attaches via `lightEngine.queueSectionData` +
  `retainData`, and `isLightOn` short-circuits `propagateLightSources`. The
  repo's region gate already relies on exactly this load path (golden
  `11_full` promotions of loaded regions). Custom `ChunkGenerator` types are
  registered in code (`BuiltInRegistries.CHUNK_GENERATOR`; vanilla registers
  `noise`/`flat`/`debug`); datapacks select registered types by name through
  `LevelStem`/`WorldPreset`.
- **Mod precedent**: `tools/golden/stage-dump-mod` and
  `tools/viz/capture/chunk-timeline-mod` — plain Gradle `java` plugin
  (deliberately no fabric-loom: 26.2 is unobfuscated, intermediary is a stub),
  Java 25 toolchain, compileOnly against the extracted server jar +
  sponge-mixin, fabric.mod.json depends `{"minecraft": "26.2"}`, loader 0.19.3
  pinned, inert unless a `-Dhyperchunk.*` property is set, hooks never break
  vanilla generation. There is zero FFM/native precedent in the repo; the
  bridge is the first native-access Java artifact.
- **Parity**: two-tier gate (worldgen-parity, ADR-007). REPLAY output is
  pinned to the golden canonical hash `a5963205...`; FREE output is pinned to
  its own own-v1 hash `2eb7485b...` and must never be bit-compared to vanilla
  golden 07+ dumps. Vanilla does not sha256-match even itself; parity claims
  are scoped "bit-identical at the same seed and the same decoration order".

## Goals / Non-Goals

**Goals:**

- A Fabric 26.2 mod that pre-generates whole regions through libhyperchunk
  over FFM and lets the stock server serve them, with a machine-checkable
  parity proof on the served path.
- The core work that makes a real consumer possible: a sufficient public
  facade, arbitrary region addressing, and self-computed or caller-supplied
  standalone inputs.
- Honest fallback: the bridge activates only on configurations the core fully
  supports and yields cleanly to vanilla otherwise.
- A fresh, reviewed bench note for bridge pregen vs vanilla vs Fabric+C2ME on
  the same machine (claim fence untouched).

**Non-Goals:**

- Low-latency on-demand chunk serving for interactive frontier play (the MVP
  serves pregenerated regions; players crossing into uncovered territory get
  vanilla generation — that is the fallback design, not a bug).
- Jigsaw assembly in C (stays excluded per ADR-002 D4).
- Per-chunk mixed native/vanilla generation inside one region (deferred; see
  Decision 4).
- P4 network/packet work or any player/entity/tick state in core (ADR-005).
- Windows/macOS/aarch64 packaging (linux-x86-64 first; see Decision 7).
- Paper bridge, Rust FFI (sequenced after Fabric validation, core-abi context).

## Decisions

### Decision 1 (axis A) — Interception point: pregen-then-load through the vanilla load path

**Alternatives considered:**

1. **Custom `ChunkGenerator`** registered in `BuiltInRegistries.CHUNK_GENERATOR`
   and selected via world preset / `LevelStem`. Idiomatic, but the scheduler
   invokes generator methods per chunk per stage, and the core deliberately
   has no per-chunk incremental entry points (core-abi "Region-granularity ABI
   only") — every stage call would have to be answered from an
   internally-completed region batch cache anyway, while vanilla still runs
   its own light engine, `WorldGenRegion` bookkeeping, and proto-chunk state
   around each result. That is a large per-stage Java marshaling surface
   (11 stages x 1024 chunks) for zero parity benefit. It also requires worlds
   to be created with a custom preset; behavior for existing worlds
   (level.dat / LevelStem reload semantics) is UNVERIFIED from local sources.
   Rejected as the serving mechanism.
2. **Per-stage substitution at `ChunkStep.apply` / `ChunkStatusTasks` mixins.**
   The hook itself is proven (both repo mods use it), but substituting stage
   *results* means translating the core's SoA chunk state into vanilla
   `ProtoChunk` at every stage boundary, and the substitution semantics do not
   survive chunk-system rewrites: under C2ME the FULL step does not pass
   through `ChunkStep.apply` at all (VIZ-5 archive note; benchmarks-and-viz
   context). Rejected.
3. **Pregen-then-load (recommended).** The bridge crosses FFM once per region
   batch, receives the core's canonical serialized output (the same
   `hc_chunk_to_nbt`/`hc_region_write` bytes the parity gate hashes), writes
   it as region data, and the stock server serves it through the verified
   `Status=full` load path (`SerializableChunkData.read` ->
   `ImposterProtoChunk` -> `LOADING_PYRAMID` passthrough, saved light attached
   via `queueSectionData`). No worldgen-stage mixin sits on the serving path;
   the mod's hooks are orchestration and detection only.

**Why 3:** it is the only option where the FFI granularity is per region batch
*by construction* (core-abi requirement + ADR-003 D2), where the served bytes
are the *gated* bytes (worldgen-parity canonical hash — Decision 3 turns this
into a gate), and where "bridge did nothing" degrades to stock vanilla
generation, which is exactly the whole-chunk fallback contract
(generation-pipeline, ADR-003 D5). The load path's fidelity for fully
generated regions is already exercised daily by the region gate
(recapture-3 golden, 4/4 strict; Task 14 archive notes).

### Decision 2 (axis B) — Batch adapter: explicit region pregen for the MVP; prefetch as stretch; no sub-region ABI

**Alternatives considered:**

1. **Pre-generation only (recommended MVP).** A `/hyperchunk pregen` command
   (and/or config) generates a rectangle of regions; the server load path
   serves them afterward. No serving-path latency at all for covered regions.
   Players crossing into uncovered territory are served by vanilla generation
   (fallback semantics, Decision 4).
2. **On-demand region batching with a cache**: first chunk request in an
   uncovered region triggers the native batch and the chunk's load future
   waits for it. Latency honesty: FREE mode generates one full region (1024
   chunks) in 0.894 s at 20 threads on hc-e6 (Zen5, 32 vCPU) — that number is
   a property of that stage and MUST NOT be generalized (benchmarks-and-viz
   "Stage attribution"). B-2/B-3 established the workload is compute-bound,
   so on a typical 8-core desktop the same batch plausibly lands in the
   several-second range — a several-second world-gen stall at every region
   boundary during play, before measuring. That is not an acceptable
   interactive experience, and integrating a blocking region future into
   `ChunkGenerationTask` scheduling is new, risky surface. Deferred (not in
   MVP); a softened variant — **ahead-of-player region prefetch** (pregen the
   next region ring in the background as players approach a boundary) — gets
   most of the value with no stall and is listed as a stretch task.
3. **Sub-region batch extension to the ABI** (e.g. 8x8-chunk batches to cut
   latency). Rejected for Phase 3: (a) the +/-5-chunk generation apron stops
   amortizing — r-region batches compute 41x41 for 1024 outputs (1.64x
   overhead) while an 8x8 batch with the same apron computes 18x18 for 64
   outputs (5.1x); (b) changing batch decomposition changes FREE's fixed
   total order, forcing an own-v1 re-pin with coherence-log gating (scheduler
   spec "Deterministic output in both modes"); (c) it cuts against the ADR-003
   D2 shape — "hand over a whole region and receive it completed" (core-abi
   context).

**Hard fence restated:** per-chunk or per-node FFI entry points are forbidden
at the API surface (core-abi "Region-granularity ABI only"; the header simply
never declares them). All alternatives above respect this; the differences
are purely about *when* region batches run.

### Decision 3 (axis C) — Parity on the served path: bridge REPLAY round-trip gate (third gated consumer)

The two-tier gate today covers one generator under test: the C implementation
driven by the CLI (worldgen-parity context). The bridge becomes the third
gated consumer (after CLI REPLAY and CLI FREE/own-v1) via a new local gate,
`scripts/bridge_parity_gate.sh`:

1. Bridge pregen of r.0.0, seed 1234567890, in **REPLAY(manifest)** mode using
   the golden bundle's order manifest.
2. A quiet server (tick freeze + gamerules, the `make_stage_dumps.sh`
   protocol) loads all 1024 chunks via forceload and saves
   (`save-all flush`).
3. `tools/golden/compare_regions.py` canonical payload hash of the
   server-emitted `r.0.0.mca` MUST equal the pinned golden hash
   `a5963205...` (verbatim from `golden/SHA256SUMS`).

This proves the full loop — native generation -> serialized bytes -> server
parse -> live `LevelChunk` -> server save — preserves canonical content, not
just that the CLI can hash its own output. Precedent that the loop can be
bit-stable: the Task 14 region gate already achieves strict canonical
equality across vanilla save-time semantics (scheduled ticks, postProcess
drain, per-chunk light batch state; see task14 archive notes and the
recapture-3 golden).

FREE-mode serving is verified the only way it can be: the same round trip
compared against the FREE own-v1 hash `2eb7485b...`, never against vanilla
golden 07+ dumps (worldgen-parity "Vanilla nondeterminism is an input";
scheduler spec). Claim discipline is inherited unchanged: parity claims name
REPLAY and are scoped "bit-identical at the same seed and the same decoration
order"; bench numbers name FREE (benchmarks-and-viz; ADR-008 P2).

Gate placement: local-only (it needs golden data and a server install), like
the 12 golden-dependent ctest suites; CI stays on the tracked-data subset and
any new golden-dependent ctest suite must be added to `CTEST_EXCLUDE`
manually (engineering-safety context). Per the gate-trust rule ("a gate is
trusted by its record of green runs"), the gate lands with recorded green
runs, and its failure modes (vacuous pass channels) are closed the same way
the existing scripts do.

### Decision 4 (axis D) — Fallback activation: detect at activation time, yield whole dimensions (MVP)

**Detection point alternatives:**

1. **Activation-time configuration audit (recommended).** Before the bridge
   activates for a dimension it verifies, in order: (a) the dimension is
   overworld-shaped — its `LevelStem` generator is the vanilla noise
   generator (`minecraft:noise` codec) with `minecraft:overworld` settings
   and the multi-noise overworld biome source; (b) the *effective* worldgen
   data (vanilla + enabled datapacks, pack format 107.1) exported from the
   live registries compiles cleanly through the core's init — the facade's
   `const char **err` init contract is the detection primitive: the core
   never guesses at unknown constructs (generation-pipeline "Whole-chunk
   vanilla fallback"), so init failure IS the unsupported-extension signal;
   (c) no Java-registered worldgen extensions are present — non-`minecraft`
   entries in the worldgen registries (features, structures, carvers, biomes,
   noise settings, density functions) mean JVM-bytecode content the core
   cannot interpret "in principle" (ADR-003 interface table); (d) no
   chunk-system-rewriting mod is present (C2ME-class mods change load/step
   semantics the bridge relies on; VIZ-5 evidence). Any check failing =>
   the bridge stays inert for that dimension, logs one clear line, and
   vanilla generates everything.
2. **Per-chunk fallback inside a region batch** (core reports chunks it
   refused; the server generates those chunks via vanilla). This matches the
   letter of ADR-003 D5 (whole-chunk granularity) but creates a mixed
   pipeline inside one region: vanilla decorating a fallback chunk reads and
   writes neighbor chunks whose content came from the core, and neighbor
   completion order is baked into block content (B-1 evidence: vanilla
   differs from itself in 581+/1024 chunks run-to-run). The combined output
   has no golden to compare against and no recording/replay story on the
   served path yet. The generation-pipeline scenario ("combined output still
   satisfies parity") states the contract, but its server-side verification
   design does not exist. Deferred to a follow-up change; the MVP MUST NOT
   partially activate.

**Yield mechanism:** with pregen-then-load, yield is structurally free — the
bridge simply does not pregen, and the untouched vanilla pipeline generates.
Yield granularity in the MVP is the whole dimension (activation gate) and, in
any later demand-driven mode, at minimum a whole region. Both are coarser
than the spec floor ("fallback granularity MUST NOT be smaller than a chunk",
generation-pipeline), which is deliberately conservative in the direction the
spec allows.

**Observability gap being closed:** the generation-pipeline spec defines no
mechanism for a caller to *learn* fallback happened. At the ABI, the facade's
init/error surface becomes that mechanism at configuration granularity
(core-abi ADDED requirement, scenario "Init failure is reported, not
guessed"). Runtime per-chunk fallback reporting is deliberately out of the
MVP together with alternative 2.

### Decision 5 (axis E) — Structures: vanilla-computed, jigsaw-assembled starts as batch inputs; core places pieces

**Facts first (honesty):** core scope is "structures through placement, not
jigsaw-assembled" (ADR-002 D4; generation-pipeline spec). In 26.2, jigsaw
piece resolution happens when the start is *created*
(`ChunkGenerator.createStructures` at `structure_starts`); pieces are
*placed* into chunks during the features stage
(`StructureStart.placeInChunk`; the stage-dump mod already mixin-hooks it).
Today the core reads structure starts + template NBT from golden dump
directories (`fopen` in `core/src/structures.c`) — starts are already
*inputs*, captured from vanilla, and the core's placement of the supplied
pieces is what the full-region gate verifies. The precise per-structure-type
split between the core's own placement scan (`structures_step`) and
golden-start inputs is not fully documented — tasks.md includes an audit
before anything depends on it.

**Alternatives considered:**

1. **Jigsaw assembly in C.** A separate difficulty class, explicitly an
   anti-goal (generation-pipeline context: "village interior room layout is
   a separate difficulty class"). Would turn Phase 3 into a second
   Phase-1-sized parity campaign. Rejected.
2. **Serve structure starts and let vanilla's FEATURES path place them on
   top.** Incoherent with the pipeline ownership: placement happens *inside*
   the features stage, which the core owns; splitting one chunk's features
   stage between vanilla and core breaks the RNG consumption-order contract
   (worldgen-parity "RNG consumption order is a contract"). Rejected.
3. **Documented MVP limitation (served worlds lack assembled villages).**
   Honest but crippling: it makes the bridge demo-only and contradicts the
   drop-in definition ("always works + mostly fast", ADR-003). Rejected
   because alternative 4 exists at bounded cost.
4. **(Recommended) Vanilla-computed starts as region-batch inputs.** At
   pregen time the bridge computes structure starts and references for the
   padded chunk set by driving vanilla's own logic
   (`createStructures`/`createReferences`) — a deterministic pure function of
   (seed, chunk pos, registries): stages 01-02 are Tier 1, byte-identical
   across independent vanilla runs (worldgen-parity/ADR-007 evidence). The
   bridge serializes starts + templates in the same shape as the golden
   dumps and passes them as caller-provided buffers in the batch call
   (facade input; the `fopen` path retires). The core places pieces exactly
   as it does today. Served worlds get fully assembled villages; jigsaw
   stays in Java, inside its ADR-002 D4 exclusion.

**Honesty limits attached to 4** (stated in the proposal, carried to tasks):
(i) r.0.0 contains a limited structure population — before claiming
assembled-structure parity generally, a second capture region containing a
village must pass the gate (a `golden:`-process capture, owner-reviewed);
(ii) driving `createStructures` outside `ChunkMap` requires constructing its
inputs (`ChunkGeneratorStructureState`, proto chunks, `StructureTemplateManager`)
— verified to the signature level only, marked UNVERIFIED beyond that; the
named fallback if it proves too entangled is to advance real proto chunks to
`structure_references` through the server's own scheduler before batching
(slower, but uses only stock behavior); (iii) this decision is why the
generation-pipeline delta reclassifies starts/templates as batch inputs — a
live JVM computing stage-01 content is a real amendment to "no JVM in the
generation path" and must be decided at spec level, not smuggled in.

### Decision 6 (axis F) — Memory and threading across FFM

**Buffer ownership: Java owns everything; ownership never crosses.** The
bridge allocates all native memory via an FFM `Arena` on the dedicated
bridge thread: (a) the core arena backing (the core never allocates on
generation paths — caller-provided backing, NULL-on-exhaustion means
grow-and-retry, per `hc_arena` contract); (b) input buffers (worldgen JSON
closures, structure starts/templates, REPLAY manifest); (c) output buffers
(per-chunk NBT payloads and/or the in-memory `.mca` image via the
`hc_region_write` shape). The core frees nothing and holds no pointers after
the call returns; the Java `Arena` is closed after outputs are persisted.
Observed budgets to be treated as measurement baselines, not designs:
`hyperchunk-verify` mallocs 6 GiB backing for a 41x41 job with 160 MiB
per-worker sub-arenas; a 6 GiB default is not acceptable on typical servers,
so measuring the true floor and making backing configurable is an explicit
task with an acceptance number.

**Call model: downcalls only.** No Java callbacks into the batch (fallback is
decided *before* submission, Decision 4), so no core-spawned pthread ever
attaches to the JVM. This keeps the FFM surface to
`Linker.nativeLinker()` downcall handles over a ~half-dozen facade functions.

**Thread coexistence.** `hc_sched_run(FREE, nthreads)` spawns and joins up to
64 pthreads per call — no persistent native pool exists, and the calling
thread blocks for the batch. The bridge therefore runs batches on one
dedicated Java thread; `nthreads` is a config knob defaulting to a
conservative `max(1, cores - reserve)` so the vanilla worker pool
(`Worker-Main`, cores-1 by default via `-Dmax.bg.threads`) is not starved
during live pregen. Oversubscription is a performance concern only, never
correctness: FREE output is deterministic across thread counts (scheduler
spec, observed 12/12 at 20T/32T).

**REPLAY runs on the calling thread** (`hc_sched.h`): in REPLAY mode the Java
bridge thread executes core stage code directly, so its stack size and the
process TLS budget matter. Known trap: the current binary carries ~8.6 MB of
PT_TLS against glibc's 8 MB default thread stack (memory:
pt-tls-stack-budget; engineering-safety context). For a **dlopen'd `.so`
inside a JVM** this is worse than for the CLI: large PT_TLS in a
runtime-loaded library risks "cannot allocate memory in static TLS block"
and taxes every JVM thread. Mitigation is a hard task: measure PT_TLS of the
`.so` build, set a small budget as an acceptance criterion, and extend the
P2-9 TLS-diet approach if it fails.

**Sanitizer story.** JVM-attached-thread interaction sits outside
`check_tsan.sh` coverage. Mitigation: a native smoke driver (a C test that
exercises the facade exactly the way the bridge does — same call sequence,
same buffer patterns) joins the ctest suite so ASan/UBSan/TSan cover the
bridge's entry sequence; sanitizers only check executed paths
(engineering-safety, ADR-009 pitfall).

### Decision 7 (axis G) — Packaging

- **Native library**: a new CMake SHARED target built from the same sources
  and parity-critical flags as the static core (`-fPIC`, `-ffp-contract=off`,
  `-fno-fast-math`, `-falign-functions=64`, TU-isolated AVX2/AVX-512/SHA-NI
  with cpuid runtime dispatch — the `.so` runs on non-AVX hosts). Gates
  extend, not fork: `check_no_fma.sh` grows a `.so` target alongside the
  archive; the ldd audit (libc/libm/pthreads only) applies to the `.so`
  (core-abi "Pure compute core" scenario).
- **Mod artifact**: plain Gradle `java` project at `bridge/fabric-mod/`
  following the stage-dump-mod template exactly — no fabric-loom (26.2 is
  unobfuscated; intermediary is a stub; ADR-006 D3), Java 25 toolchain,
  compileOnly against `tools/golden/libs/extracted/server-26.2.jar` + bundled
  libraries + sponge-mixin, `fabric.mod.json` depends `{"minecraft": "26.2"}`,
  loader 0.19.3 via the pinned `fetch_fabric.sh`. The `.so` ships inside the
  jar under `natives/linux-x86-64/`, is extracted to the server run dir at
  boot, and loaded with `System.load`.
- **FFM bindings**: hand-written `MethodHandle`s over `Linker.nativeLinker()`
  for the small facade surface. No jextract build dependency (nothing else in
  the repo needs it; the facade is a handful of functions; zero-dependency
  ethos).
- **Java 25 native access**: FFM is final (JEP 454) — no preview flags — but
  the JEP 472 line restricts native access, so the server launch should pass
  `--enable-native-access` for the mod's classpath. The exact enforcement
  level on Java 25 (warning vs denial) is UNVERIFIED from local sources and
  is settled at implementation time; the mod handles both modes — it logs
  one clear line and stays inert if native access is denied.
- **Platform scope**: linux-x86-64 only in Phase 3 (every dev, bench, golden,
  and demo environment is linux-x86-64; the multipliers are stage-attributed
  there). On any other platform the mod logs one line and stays inert —
  inertness is the compatibility story, not a crash.
- **Alternatives considered**: fabric-loom build (rejected — dead weight with
  no remapping to do, and both existing mods already prove the plain path);
  standalone `.so` distributed next to the jar (rejected for MVP — one
  artifact is the drop-in story; ADR-003 context explicitly blesses in-jar
  `.so`); jextract-generated bindings (rejected — build-time dependency for a
  half-dozen handles); multi-platform natives in one jar (deferred — no CI or
  golden coverage exists for other platforms; shipping untested natives
  contradicts the gate-trust rule).

### Decision 8 (axis H) — Phase 3 success criteria (ADR-001 style: measurable, honest)

1. **Bridge parity gate green** (the Phase 3 analogue of Phase 1's
   "parity is the only cause of death"): `bridge_parity_gate.sh` — REPLAY
   pregen -> server load + save round trip -> canonical payload hash equals
   golden `a5963205...`; FREE round trip equals own-v1 `2eb7485b...`
   (Decision 3). Recorded green runs, not a one-off.
2. **Pregen demo with fresh numbers**: same machine, three configurations
   pregenerate the same fresh-region workload through a real server —
   vanilla (B-1 forceload+poll protocol), Fabric+C2ME (same protocol), and
   bridge pregen (own instrumentation plus server-observable completion,
   with the measurement window and its symmetry to the polling windows
   documented per the B-1/B-6 conventions, including where serialization
   falls in each window). Results land as a new reviewed bench note with
   stage+machine+mode labels and error-deducted lower bounds. The existing
   claim-fence numbers are never restated as bridge numbers; REPLAY phrasing
   stays "canonical-identical output at C2ME-class speed".
3. **Interactive smoke**: a player joins a bridge-pregenerated world, streams
   chunks across at least two region boundaries including the
   vanilla-generated frontier; all served chunks reach `minecraft:full`;
   zero "Detected setBlock in a far chunk" / unsafe-read lines in the server
   log; no client-visible corruption. Scripted where possible (console-FIFO
   harness precedent).
4. **Fallback proof**: (a) a synthetic test mod registering a Java feature
   and (b) a non-noise world preset each cause a clean yield — one log line,
   bridge inert, vanilla plays normally (mirrors the ADR-003 verification
   scenarios).
5. **Audits**: ldd on the shipped `.so` (libc/libm/pthreads only);
   facade-only include audit for the bridge and the refactored CLI; bridge
   granularity review — every FFM crossing is per region batch (core-abi
   scenario).

## Risks / Trade-offs

- **[Risk] Region-addressing generalization touches unknown ground** — 0,0
  assumptions may be scattered through `core/src` and the manifests.
  -> Mitigation: audit-first task with a written inventory; Phase 1
  discipline applies ("do not start a task whose stage output cannot be
  golden-compared") — the second-region capture lands before dependent
  acceptance runs.
- **[Risk] Multi-noise biome assignment is unscoped** — it could be
  Task-6-of-Phase-1 class work. -> Mitigation: standalone-input audit first,
  subdivide only after reading the actual 26.2 evidence (Phase 1 rule:
  "subdividing now would be guessing").
- **[Risk] PT_TLS in a dlopen'd `.so`** — static TLS exhaustion inside the
  JVM, per-thread TLS tax. -> Mitigation: measured budget with a hard
  acceptance number in the `.so` task; TLS-diet precedent exists (P2-9).
- **[Risk] Arena budget vs real servers** — 6 GiB observed backing is a
  non-starter on an 8 GiB host. -> Mitigation: measure the true floor,
  make it configurable, publish honest minimum requirements.
- **[Risk] Save-time semantics drift through a live server** (scheduled
  ticks, postProcess, light batch state could mutate between load and save).
  -> Mitigation: quiet-server protocol in the gate; recapture-3/Task-14
  precedent says strict canonical equality through this loop is achievable;
  the gate exists precisely to catch drift.
- **[Risk] `createStructures` may not be cleanly drivable outside
  `ChunkMap`** (UNVERIFIED beyond signatures). -> Mitigation: named fallback
  (advance proto chunks to `structure_references` via the server's own
  scheduler), decided inside the task, not silently.
- **[Risk] Bench comparability** — the bridge moves where serialization and
  disk writes happen relative to the B-1 windows. -> Mitigation: the bench
  note defines its windows explicitly against the B-1 trap list before any
  number is published.
- **[Trade-off] Pregen-first MVP** buys parity-provable serving and zero
  serving-path latency at the cost of not accelerating the interactive
  frontier; that residual is served by vanilla (correct, slow) until the
  prefetch stretch or a future on-demand design.
- **[Trade-off] Dimension-level yield** is coarser than the spec's
  whole-chunk floor — it forfeits partial acceleration on lightly-modded
  servers in exchange for a provable no-mixed-pipeline guarantee; the
  graceful-degradation economics (down to ~41x at 20% coverage,
  generation-pipeline context) are deferred with it.

## Migration Plan

No server-side migration: the mod is optional, inert by default, and a world
generated with the bridge is a normal vanilla world (canonical bytes) that
opens fine without the mod afterward. Spec migration happens at archive time
(deltas apply to core-abi and generation-pipeline; fabric-bridge spec is
created). Decision-history (context.md) entries for the decisions above are
appended when the implementing changes land, per the append-only rule.

## Open Questions

Deferrable without changing specs, approach, or task breakdown:

- The measured minimal arena backing (Task 1/4 acceptance produces the
  number; the contract — configurable, caller-owned — does not move).
- Whether the prefetch stretch ships inside Phase 3 or slips to a follow-up
  change (pure scheduling; the MVP is complete without it).
- Hand-written FFM bindings could later be swapped for jextract output if the
  facade grows (internal detail).

Questions that need the owner (not deferrable in spirit — they gate specific
tasks) are listed separately in notes.md.
