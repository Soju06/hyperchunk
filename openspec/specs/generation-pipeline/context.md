# generation-pipeline: Context

## Purpose & scope

[spec.md](spec.md) is the normative SSOT for pipeline scope, the version pin,
and datapack compatibility. This document holds the decision history for "why
a full pipeline in pure C from scratch" (ADR-002), "why the datapack schema is
the compatibility surface and fallback is per chunk" (ADR-003, compatibility
half), and "why 26.2" (ADR-006). The ABI half of ADR-003 (region boundary,
pure compute core) is in [core-abi/context.md](../core-abi/context.md).

## Current state (as of 2026-08)

- All 11 stages implemented in C (Phase 1), full-region canonical match.
  Per-stage implementation and verification records are in the task7
  (surface) through task14 (full region) notes under
  [changes/archive/](../../changes/archive/).
- The datapack parser targets the 26.2 schema (107.1) plus vanilla fallback.
  Golden comparison of a Terralith-class external datapack has not been
  performed yet (the spec's Pure-JSON datapack scenario states the contract).

## Decision history

> append-only. Existing entries are never edited. When a decision changes,
> add a new entry and mark the previous one `Superseded by`.

Placement: this document is the primary home for ADR-002 (D3 parity is
reflected in the spec of [worldgen-parity/spec.md](../worldgen-parity/spec.md),
D1 language identity in [engineering-safety](../engineering-safety/spec.md),
P1 FMA in [simd-backends](../simd-backends/spec.md), P4 conflict scheduling in
[scheduler](../scheduler/spec.md)). Of ADR-003, only the compatibility half
(D4 and D5) is here; the ABI half (D1-D3) is in core-abi. This document is the
primary home for ADR-006 (D2 golden environment is reflected in the
worldgen-parity spec, D4 FFM in the core-abi spec).

### ADR-002: Rewrite from scratch in pure C, and maintain full-pipeline bit parity (Phase 1, 2026-07-27)

**Status:** Decided
**Type:** Architecture

#### Context

There were two paths. (a) Fork C2ME and add a GPU density function backend; (b) rewrite in C from scratch.

The case for (a) was strong. It gets C2ME's already-done CPU thread parallelization for free, swapping only the backend on top of the same CPU code makes the 3-way comparison a single-variable experiment, and GPL-3.0 poses no problem for publishing a fork.

The user chose (b). The directive:

> "I think the essence is writing it from the ground up with the CPI calculated meticulously." (owner directive, translated from the Korean original)

This choice was verified quantitatively. Based on local measurements (AMD Ryzen 9 5900X, Zen 3, AVX2+FMA, no AVX-512):

- Theoretical per-core ceiling 16 flops/cycle (AVX2 double 4 lanes x 2 FMA units x 2)
- Vanilla density function interpreter effective 0.03-0.08 flops/cycle (~12 instructions per node, CPI 3-8)
- **A 96-256x gap**

The cause is the data structure, not the language. Since 1.18 the density function is an interpreter tree, and every node makes a megamorphic indirect call the JIT cannot inline. Most cycles are spent on branch mispredictions and waiting on indirect calls.

A roofline analysis confirmed this headroom is actually recoverable:

- 4,200,000 flops of noise per chunk / 196,608 bytes of output -> **arithmetic intensity 21.4 flops/byte**
- Ridge point at 7.0 flops/byte with 12 cores concurrent
- 21.4 >> 7.0 -> **COMPUTE bound**

Had it been memory bound, CPI optimization would have been wasted effort; because it is compute bound, IPC improvements translate directly into wall clock. The precondition of the user's approach passed on measured data.

We also computed the total multiplier per attack axis to confirm that a CPI rewrite does not run into Amdahl. Based on the estimated stage shares `noise 45% / surface 8% / carvers 10% / features 22% / lighting 15%`:

| Attack axis | Total multiplier | Amdahl |
|---|---|---|
| GPU offload only (noise+surface+lighting) | 2.9x | Capped by the untouched 32-55% |
| Thread parallelism only (C2ME-class) | 9.8x | Partial due to features conflicts |
| **CPI rewrite from scratch (single core)** | **5.6x** | **Does not apply (`1-p` = 0)** |
| **CPI rewrite + 12 cores** | **47.5x** | Does not apply |

Amdahl only holds when an untouched portion remains. GPU offload touches only noise and merely moves the bottleneck to features/carvers (residual shares after offload: features 63% / carvers 29%). A CPI rewrite touches every stage, so no unacceleratable portion exists.

The full-pipeline scope was also specified by the user ("I want to do the full pipeline", owner directive, translated from the Korean original). In the course of validating this decision, **two errors in the preceding analysis were corrected**:

1. The Amdahl 3.13x cap applies only to the "us vs C2ME" leg. The left panel of the GIF is vanilla, and vanilla barely uses its cores for chunk gen, so the multiplier we get is the product `(CPU parallel gain) x (GPU/CPI gain)`. At 12 cores, 47-75x vs vanilla.
2. Amdahl is a latency law, and what the GIF shows is throughput. With batch pipelining the total time becomes `max(GPU, CPU)`, so the ceiling is the hardware, not Amdahl.

#### Decision (4 core decisions)

| # | Decision | Key point |
|---|---|---|
| D1 | **Rewrite from scratch in pure C** | Not a C2ME fork. Porting the interpreter tree as-is tops out at 2x, so the data structures are redesigned first |
| D2 | **Full pipeline (noise, surface, carvers, features, lighting, all of it)** | No cutting down to terrain-only. The honesty of the GIF depends on this |
| D3 | **Bit-exact output identical to vanilla is mandatory** | Region file `sha256` match is the acceptance criterion *(ADR-007 refined this into a canonical hash + two-tier gate)* |
| D4 | **Control the workload by splitting scope three ways** | Single 1.21.x patch / overworld only / structures through placement (jigsaw assembly excluded) *(ADR-006 replaced the version value with 26.2)* |

#### Why rewrite from scratch over a C2ME fork?

For the record: (a) was the ROI-optimal choice. The rationale for choosing (b) is ADR-001 D1. Since the goal is a technical demonstration, "rewrite everything for 47x" fits the goal better than "3x stacked on top of SoTA". D1 also avoids C2ME's GPL-3.0 propagation, leaving the license choice free.

The cost is recorded honestly. All of features/carvers/structures/lighting must be reimplemented in C. cubiomes covered only biomes and structure positions, and even that was a large project. **A full vanilla-parity C reimplementation has no precedent.** That is the size of the flex and the size of the risk.

#### Why is bit parity mandatory?

1. It is the entire flex. "Fast terrain generators" are common; "regenerates your world bit-exactly, 47x faster" has no precedent.
2. Verification comes for free. It ends with a `sha256` comparison of the vanilla region and our output. That result is stronger than any dashboard.
3. Without parity the GIF itself is meaningless. A speed comparison between different algorithms is not a benchmark.

#### Anti-goals

- A terrain-only benchmark (excluded by D2)
- Version matrix support (D4)
- Nether/End (D4)
- Jigsaw structure assembly: village interior room layout is a separate difficulty class and outside Phase 1 scope (D4)
- Approximate generation downcast to float: faster, but incompatible with D3

#### Pitfalls

1. **FMA contraction silently changes terrain.** Folding `a*b+c` into a single FMA instruction drops the intermediate rounding and changes the result bits. Vanilla is mul -> round -> add. FMA is banned in CPU kernels; if GPU code ever appears, `-fmad=false` is mandatory. This bug's only symptom is "terrain is subtly different", so the debugging cost is extreme. *(normative: [simd-backends/spec.md](../simd-backends/spec.md))*
2. **RNG consumption order must be reproduced too.** If the call order of the LCG and Xoroshiro128++ changes, features placement changes. *(normative: [worldgen-parity/spec.md](../worldgen-parity/spec.md))*
3. **Vanilla noise is double.** Lowering it to float as an optimization violates D3.
4. **features write across chunk boundaries into neighboring chunks.** Processing adjacent chunks concurrently conflicts, and changing the processing order changes RNG consumption order. Only conflict-free chunk sets may go into a batch, via checkerboard/stripe scheduling. This is harder than the GPU kernels and is what C2ME wrestled with for years. *(normative: [scheduler/spec.md](../scheduler/spec.md))*
5. **Bench host reliability.** The current dev box reports `Core(s) per socket: 22`, `Thread(s) per core: 1`, `L3 cache: 352 MiB (22 instances)` in `lscpu`. A real 5900X is 12 cores/24 threads with 64MB L3, and the `hypervisor` flag is present. That is, this is a VM that misreports SMT and cache topology. **Cycle counts from this box cannot be used as evidence.** Public benchmarks must be remeasured on bare metal or a core-pinned dedicated instance. *(Later: B-4 measured hc-e6's eligibility as the stage, B-6 remeasured for publication; [benchmarks-and-viz/context.md](../benchmarks-and-viz/context.md))*

#### Verification

- `hyperchunk-verify --seed <s> --region <x> <z>` reports `sha256` equality with the vanilla golden region
- Replace the estimated stage shares (`noise 45%` etc.) with a measured profile and recheck this ADR's calculations
- The FMA ban is enforced by compile flags and CI catches regressions

#### When this might break

- If Mojang changes the worldgen algorithm at scale (D4's single-patch pin is the line of defense)
- If the measured profile differs greatly from the estimated shares: in particular, if the features share is much larger than 22%, priorities need rebalancing

#### References

- ADR-001 (purpose; [project.md](../../project.md))
- https://github.com/RelativityMC/C2ME-fabric
- https://github.com/Cubitect/cubiomes

### ADR-003 (compatibility half): Compatibility comes from the datapack schema plus vanilla fallback (Phase 1, 2026-07-27)

**Status:** Decided
**Type:** Architecture / Contract
**Full-text split:** The ABI half (D1-D3: pure compute core, region boundary,
arena/scheduler ownership) and the Context's boundary-cost and batching
analysis are in [core-abi/context.md](../core-abi/context.md). Below is the
part covering the compatibility contract (D4 and D5).

#### Context (compatibility interface analysis)

bun did not reimplement the npm ecosystem; it implemented Node's *interface*: the `node:` built-in module API surface and the **N-API ABI** (native addons load without recompilation). It leaves packages untouched and swaps only the engine.

MC worldgen has three corresponding interfaces:

| Interface | What it is | Native handling feasibility |
|---|---|---|
| Datapack worldgen JSON (`noise_settings`, `density_function`, `placed_feature`) | Data | Feasible. Implementing the schema makes Terralith/Tectonic compatible automatically |
| Feature/StructurePiece registered in Java code | JVM bytecode | Impossible in principle |
| Bukkit/Paper `ChunkGenerator`, `BlockPopulator` | JVM interface | Bridgeable |

MC having made worldgen data-driven since 1.18 plays the role of N-API for us.

#### Decision (2 compatibility decisions; D1-D3 are in core-abi)

| # | Decision | Key point |
|---|---|---|
| D4 | **Treat datapack worldgen JSON as our N-API** | Implementing the schema makes Terralith/Tectonic-class worldgen compatible automatically |
| D5 | **On an unknown extension, fall back to the vanilla path for the whole chunk** | No guessing. Correctness outranks speed |

#### Why fallback? The definition of drop-in

Drop-in does not mean "always fast"; it means **"always works + mostly fast"**. bun also has incompatible packages, but they merely run slower; they still work.

Effective multiplier by native features coverage (12 cores):

| Scenario | features coverage | x12 cores |
|---|---|---|
| Vanilla only | 100% | 75x |
| Datapack worldgen (Terralith etc.) | 100% | 75x |
| Light mods (20% Java) | 80% | 62x |
| Mid-size modpack (50% Java) | 50% | 49x |
| Heavy modpack (80% Java) | 20% | **41x** |

Even with features remaining almost entirely in Java, 41x holds. features is only 22% of the total, and **the 78% that is noise/surface/carvers/lighting is the vanilla algorithm in any modpack**, so it is always handled natively. Compatibility loss thus shows up as graceful degradation, not as "does not work".

#### Anti-goals (compatibility)

- Attempting to emulate Java Features natively: impossible in principle; D5 is the answer

#### Pitfalls (compatibility)

- **The temptation to decide fallback at a granularity smaller than a chunk.** Partial fallback breaks RNG consumption order. The unit of decision is the whole chunk.

#### Verification (compatibility)

- With the Terralith datapack as input, `sha256` match against the vanilla comparison run
- With an artificially registered unknown Feature, the affected chunk is handled by the vanilla path and the result still satisfies parity

#### When this might break

- If Mojang makes an incompatible change to the datapack worldgen schema (ADR-002 D4's single-patch pin is the line of defense)
- If the mod ecosystem regresses toward Java code extensions over datapacks: the fallback share grows and the multiplier drops below 41x

#### References

- ADR-002 (rewrite policy; above in this document)
- https://minecraft.wiki/w/Data_pack

### ADR-006: Change the target version to 26.2, and leverage the unobfuscated source and FFM (Phase 1, 2026-07-28)

**Status:** Decided
**Type:** Scope / Contract
**Resolves:** the version pin of ADR-002 D4; partially supersedes ADR-003's JNI choice

#### Context

User directive: "For now, let's make the first version target 26.2" (owner directive, translated from the Korean original)

ADR-002 D4 assumed a "single 1.21.x patch", but Mojang switched to year-based versioning (year.drop.hotfix) starting in 2026. Release lineage: 1.21.11 -> 26.1 "Tiny Takeover" (2026-03-24) -> 26.1.1/26.1.2 -> **26.2 "Chaos Cubed" (2026-06-16, the current latest release)**. Existence confirmed in the version manifest (protocol 776, data version 4903, data pack format 107.1).

Three ripple effects confirmed by the investigation:

1. **Java 25 required.** From 26.1, minimum Java version = Java SE 25. The plan's "install JDK 21" instruction is void.
2. **Fully unobfuscated from 26.1.** 26.1 is "the first to be fully unobfuscated without an accompanying obfuscated variant". Fabric accordingly dropped Yarn mapping support and switched to official Mojang names. With the mapping layer gone, the stage dump harness (mixin) is written directly against the real class names.
3. **26.2 worldgen differs from 1.21.x.** The sulfur caves cave biome and sulfur/cinnabar blocks were added, enlarging the surface to reproduce. Existing reference implementations such as cubiomes most likely do not support 26.x. In exchange, the unobfuscated source greatly lowers the cost of confirming algorithms, offsetting this.

#### Decision (4 core decisions)

| # | Decision | Key point |
|---|---|---|
| D1 | **Target version = 26.2, pinned** | TARGET_VERSION=26.2. ADR-002 D4's "single-patch pin" principle stays; only the value is replaced |
| D2 | **Golden generation environment is JDK 25** | Requirement to run a 26.x server *(normative: [worldgen-parity/spec.md](../worldgen-parity/spec.md))* |
| D3 | **Drop the mapping layer, use real Mojang names directly** | Yarn is dead. Write mixins/reflection against the unobfuscated jar |
| D4 | **Phase 3 bridge uses FFM instead of JNI** | FFM (JEP 454) is final in Java 25. ADR-003's "JNI because Java 21 preview" rationale is gone. The boundary-granularity invariant (per region) stands *(normative: [core-abi/spec.md](../core-abi/spec.md))* |

#### Why 26.2 over 1.21.x?

The latest stable release is 26.2, so comparing against "current vanilla" fits the demonstration goal (ADR-001). A benchmark against 1.21.x would already be an old-version comparison at release time.

#### Anti-goals

- Simultaneous 26.1/26.3-snapshot support (single-patch principle)
- 1.21.x backward compatibility
- Copying decompiled/unobfuscated source code: name references and algorithm understanding are now unencumbered, but copying code is still a copyright problem (ADR-002 R4 principle stands)

#### Pitfalls

1. **sulfur caves are inside the carvers/features/biomes reimplementation scope.** 1.21-based material (most wiki articles) can differ from 26.2 reality, so the unobfuscated source and the 26.2-extracted JSON are always the primary evidence.
2. **Mind the version when consulting cubiomes.** Trusting an algorithm that lacks 26.x support silently breaks parity.
3. **data pack format 107.1.** The Task 12 datapack schema parser targets the 26.2 schema.
4. **Whether the overworld y range (-64..319) is retained is unconfirmed.** Settle it by measurement in the Task 2 golden dump and align the hyperchunk.h constants to it.

#### Verification

- TARGET_VERSION file content = 26.2
- The golden server runs on JDK 25 and region generation is confirmed
- The stage dump harness builds without mapping tools

#### References

- ADR-002 (above in this document), ADR-003 ([core-abi/context.md](../core-abi/context.md))
- https://minecraft.wiki/w/Java_Edition_26.2
- https://minecraft.wiki/w/Java_Edition_26.1 (unobfuscation, Java 25 requirement)
- https://docs.fabricmc.net/develop/porting/ (Yarn discontinued, switch to Mojang mappings)
