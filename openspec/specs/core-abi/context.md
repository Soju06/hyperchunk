# core-abi Context

## Purpose & scope

[spec.md](spec.md) is the normative SSOT for the core boundary and ABI. This
document holds the decision history for "why a region-granularity boundary,
and why the core is a pure compute library" (the ABI half of ADR-003). The
compatibility half of ADR-003 (datapack D4, fallback D5) is in
[generation-pipeline/context.md](../generation-pipeline/context.md), and the
P4 isolation boundary (ADR-005) is in [project.md](../../project.md).

## Current state (as of 2026-08)

- The core is `libhyperchunk.a` (C11, libc/libm/pthreads only). Consumers are
  the CLI (`hyperchunk-verify`, region output) and the bench driver
  (`hyperchunk-bench`). The Fabric FFM bridge (Phase 3) is not started; Rust
  FFI is at the planning stage.
- The JNI choice made at ADR-003 time was replaced with FFM by ADR-006 D4
  (JEP 454 is final in Java 25); the full text is ADR-006 in
  generation-pipeline/context.md.

## Decision history

> Append-only. Existing entries are never edited. When a decision changes,
> add a new entry and mark the old one `Superseded by`.

### ADR-003 (ABI half): the core is a pure compute library exposing a region-granularity C ABI (Phase 1, 2026-07-27)

**Status:** Decided
**Type:** Architecture / Contract
**Full-text split:** the compatibility half (D4 datapack N-API, D5
chunk-granularity fallback) and its interface analysis are in
[generation-pipeline/context.md](../generation-pipeline/context.md).
Below is the part covering the ABI and module boundary (D1~D3).

#### Context

During the ADR-002 discussion the author concluded that "from-scratch C = a
standalone binary outside the JVM". **This was wrong, and the user corrected
it.** Original text:

> "On the usability side, if we can't build a compatibility layer for
> existing Bukkit or plugins, usability drops and portability takes a pretty
> big hit, which seems like the downside. Is there a way around it?
> I keep thinking of the experience I had before, where just switching
> node -> bun made API latency go up 30%; I don't want to create that kind
> of experience."

Shipping C code as a `.so` inside a Fabric mod is entirely possible. A
standalone binary is an option, not a requirement.

bun's actual strategy (implementing the N-API ABI: implement only the
interface and swap the engine) is recorded in the compatibility half. The key
point on the ABI side is quantifying the boundary cost (baseline: an empty
JNI call ~22ns, chunk generation ~10ms):

| Boundary granularity | Calls per chunk | Cost | Share of chunk time | Verdict |
|---|---|---|---|---|
| 1 per chunk | 1 | 0.00002ms | 0.00% | Free |
| 1 per cell column | 64 | 0.001ms | 0.01% | Free |
| 1 per noise sample | 1,400 | 0.031ms | 0.31% | Acceptable |
| **1 per density node** | **84,000** | **1.85ms** | **18.5%** | **Fatal** |

The user's "switched to bun and got 30% slower" experience corresponds to the
last row. With the wrong boundary granularity, going native is a net loss.

The user then expanded the scope:

> "Rather than delegating only chunk generation, Java seems weak at batch
> processing too, so if we manage the entire worldgen side ourselves as
> modules, I think that would cut down a lot of the bottlenecks we would hit
> later."

This diagnosis was verified quantitatively. Java allocates about 40,808
objects per chunk (DensityFunction intermediates 11,200 / BlockState/Palette
references 24,576 / BlockPos 5,000, etc.), roughly 1.31 MB per chunk.

| Throughput | Allocation rate | |
|---|---|---|
| 50 chunk/s | 0.07 GB/s | Comfortable |
| 500 chunk/s | 0.65 GB/s | Acceptable |
| **5000 chunk/s** | **6.53 GB/s** | **Exceeds G1's sustainable limit (1~2GB/s)** |

In other words, the root cause of slow batching is not the language but
**tens of thousands of object allocations per chunk**. In Java, growing the
batch increases GC pressure linearly, a net loss. Switching to arena/SoA in C
drops allocations to zero, so **the bigger the batch, the bigger the win. The
sign is reversed.**

Raising the boundary granularity to a region takes the per-chunk boundary
cost from 22ns to 0.02ns. The user's proposal is not scope creep but a design
improvement.

Finally, a decisive competitive landscape was confirmed:

| Project | What it is | Weakness |
|---|---|---|
| Pumpkin (Rust) | Full server reimplementation | **worldgen/saving incomplete** |
| Valence (Rust) | Bevy ECS framework | No vanilla worldgen |
| Minestom (Java) | NMS-free framework | No vanilla worldgen (intentional) |
| Cuberite (C++) | Independent server | No vanilla parity |
| C2ME | Chunkgen optimization mod | Stays within JVM limits |

**Every team building a native server is stuck on worldgen.** Standing alone
as a modular C ABI makes them potential consumers, and our core takes the
position that N-API holds for bun.

#### Decision (3 ABI decisions; D4 and D5 are in generation-pipeline)

| # | Decision | Key point |
|---|---|---|
| D1 | **The core is a pure compute library. It knows nothing of file I/O or networking** | Four consumers can attach, benchmarks stay clean, and state management cannot contaminate the core |
| D2 | **The boundary is exposed at region granularity only. No node-level functions are exported** | `hyperchunk_batch(seed, region_list, out)`. Node-level exposure is an 18.5% loss |
| D3 | **The core owns the arena/SoA allocator and the batch scheduler** | Leaving batching in Java hits the GC wall and the gains never show up |

#### Why a region-granularity boundary?

D2 structurally avoids the user's bun-regression experience. The shape must
be "hand over a whole region and receive it completed", not "call C functions
from Java". With node-level exposure, a naive implementation naturally falls
into the 18.5%-loss path. This is therefore not a performance tip but **an
invariant enforced at the API surface**.

FFM (Panama) was finalized in Java 22 (JEP 454), and MC 1.20.5+ requires
Java 21, where it is still preview. So we go with JNI. At region granularity
the performance difference between FFM and JNI is meaningless. *(Superseded
by ADR-006 D4: with the target moving to 26.2/Java 25, FFM is final, and the
bridge uses FFM.)*

#### Module boundary

```
┌─ hyperchunk (pure C, zero dependencies, C ABI) ─────┐
│  arena allocator / SoA block storage                        │
│  density function compiler (AVX2 + AVX-512 runtime dispatch) │
│  5-stage pipeline                                            │
│  batch scheduler (checkerboard scheduling, features conflict avoidance) │
│  API: hyperchunk_batch(seed, region_list, out)  ← region granularity │
└────────────────────────────────────────────────────────┘
      ↑              ↑               ↑
  CLI (bench/GIF)   JNI (Fabric)   Rust FFI (Pumpkin/Valence)
```

- **CLI** is for bench, GIF, and parity verification only. It shuts down
  JVM-warmup disputes and guarantees benchmark purity
- **JNI/Fabric** is real-server usability *(FFM since ADR-006 D4)*
- **Rust FFI** comes after Phase 3. The competitive landscape above shows the
  demand
- A Paper plugin bridge is deferred until after Fabric validation because its
  API is more entangled

The CLI and the mod link the same static library twice, so the extra cost is
negligible.

#### Anti-goals (ABI-related)

- Putting file I/O, networking, or player/entity state in the core (D1)
- Node-level FFI exposure of density functions (D2)
- Leaving batch scheduling in Java (D3)
- Including a Paper bridge in Phase 1

#### Pitfalls (ABI-related)

1. **The node-level FFI temptation.** Early in the port, "natively call
   Java's density function nodes one at a time" looks like the easiest path.
   That is the 18.5%-loss path. Node-level functions are simply never
   declared in the core header, so this is blocked at compile time.
2. **Stale data on arena reuse.** When SoA buffers are reused across batches,
   a missed initialization shows up as a parity bug, and the sporadic
   symptoms make it hard to track down.
3. **Java 21 vs 22 runtime confusion.** FFM sample code used as-is does not
   run without `--enable-preview`. This is why JNI was chosen. *(No longer
   applicable since ADR-006 moved the target to Java 25; preserved as
   historical record.)*

#### Verification (ABI-related)

- The `hyperchunk` header contains only region-granularity entry points and
  no node-level functions
- The core builds with no dependency beyond `libc` (verified with `ldd`)

#### When this might break

- If the mod ecosystem regresses toward Java code extensions over datapacks:
  the fallback share grows and the multiplier drops below 41x (see the
  compatibility half)

#### References

- ADR-002 (rewrite policy, in [generation-pipeline/context.md](../generation-pipeline/context.md))
- https://openjdk.org/jeps/454 (FFM, Java 22)
- https://github.com/pumpkin-mc/pumpkin
- https://github.com/valence-rs/valence
- https://minestom.net/
