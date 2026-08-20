# hyperchunk: Project

A project that reimplements the vanilla Minecraft Java Edition 26.2 overworld
worldgen bit-exactly in pure C11. This document holds the project's direction
decisions (product purpose, scope boundaries) and the capability index.
Testable requirements live in `specs/<capability>/spec.md`; each decision's
rationale, numbers, and history live in the "Decision history" section of the
same folder's `context.md`.

> NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG
> OR MICROSOFT.

## Capabilities

| Capability | SSOT of what |
|---|---|
| [worldgen-parity](specs/worldgen-parity/spec.md) | Bit-exact parity definition, two-tier gate (bit-exact + order-replay), canonical hash definition, golden capture scheme |
| [generation-pipeline](specs/generation-pipeline/spec.md) | 11-stage pipeline, 26.2 version pin, datapack schema compatibility + vanilla fallback |
| [simd-backends](specs/simd-backends/spec.md) | Scalar/AVX2/AVX-512 runtime dispatch, byte-identical output across backends, FMA prohibition gate |
| [core-abi](specs/core-abi/spec.md) | Region-granularity C ABI, pure compute library boundary, FFI consumers (CLI/FFM/Rust) |
| [scheduler](specs/scheduler/spec.md) | FREE/REPLAY dual mode, determinism guarantee, shared stage code |
| [engineering-safety](specs/engineering-safety/spec.md) | Staying on C11, sanitizer gates, zero-warning, gate chain |
| [benchmarks-and-viz](specs/benchmarks-and-viz/spec.md) | Public bench numbers and claim rules, demo/viz capture and timeline |

## Workflow (summary)

The specs are the normative SSOT for current behavior. Work that changes
behavior, contracts, or schemas first creates a change in `openspec/changes/`,
syncs the spec after implementation, passes `openspec validate --specs`, and
then archives to `openspec/changes/archive/`. Detailed procedure: repo-root
[AGENTS.md](../AGENTS.md); contribution rules:
[.github/CONTRIBUTING.md](../.github/CONTRIBUTING.md).

Decision history rule (successor to the former append-only ADR log): the
"Decision history" section of each capability's `context.md` is
**append-only**. Existing ADR entries are not modified. When a decision
changes, add a new entry and mark the previous one with `Superseded by`;
spec.md updates happen only through an openspec change.

---

## Decision history: project direction

Two ADRs that are direction decisions rather than requirements are preserved
here. ADR numbers and dates are kept exactly as in the original (the former
append-only ADR log, written 2026-07-27 to 2026-07-28).

### ADR-001: The project goal is technical demonstration, not ROI, and the deliverable is a 3-way top-view comparison GIF (Phase 0, 2026-07-27)

**Status:** Decided
**Type:** Product direction
**Plan:** [changes/archive/2026-07-28-phase1-vertical-slice/](changes/archive/2026-07-28-phase1-vertical-slice/) (formerly .hermes/plans/2026-07-27_phase1-vertical-slice.md)

#### Context

The project started as a "chunk generation SaaS". Initial research identified three walls that kill the SaaS form.

1. **The legal wall.** The Minecraft Usage Guidelines work by enumeration: "Do **not** make commercial use or commercially exploit anything that we have made unless these guidelines say it's okay" / "If something isn't covered by these guidelines and we haven't otherwise said it's okay, that probably means we don't want you to do it". The allowlist covers only videos/streams, server hosting, publications, and handcrafts (with a $5,000/year cap); "selling worldgen output" is not enumerated.
2. **The free-alternative wall.** Chunky performs pregen for free. Customers are already renting idle CPUs, and pregen is a one-time cost, so a subscription does not attach. The MC hosting market price is around $1/GB, making price sensitivity extreme.
3. **The commodity wall.** The seed-map layer (Chunkbase, seeds.gg, seedlander, mcseedmap, seedmap.app, cubiomes.com) is entirely free ad-supported models; it is an SEO/traffic market, not a SaaS market.

At this point the user stated the goal explicitly:

> "Honestly, this is just a monstrous show of technical skill that completely ignores ROI, to give people FOMO." (owner directive, translated from the Korean original)

This directive nullified walls 2 and 3 (as a free release it does not compete with free alternatives or the ad market). Wall 1 remains, because the same document defines commercial use like this: "commercial use means any uses of our name, brand, or assets that you use and share with others (**regardless of whether you receive payment or provide it for free**)".

The user also specified the deliverable format:

> "Default Java vs the bukkit/plugin that's supposedly best optimized vs us / just showing how fast real-time generation is in a chunk top view, as a short-form like a GIF, would probably be enough." (owner directive, translated from the Korean original)

#### Decision (5 core decisions)

| # | Decision | Key point |
|---|---|---|
| D1 | **Declare ROI optimization an explicit anti-goal** | Revenue model, billing, and customer acquisition are not design inputs |
| D2 | **The deliverable is a 3-way top-view real-time generation comparison short-form** | `vanilla Java` vs `Paper + C2ME + Chunky` vs `hyperchunk` |
| D3 | **Free release (OSS)** | Sidesteps walls 2 and 3 and secures reproducibility |
| D4 | **Promote bit-exact parity to the essence of the product** | The GIF only works if "same terrain, only speed differs" is proven to the eye on the 3-way screen |
| D5 | **Comply with the legal guardrails** | Do not use Minecraft as a dominant element of the name, only as a subtitle; state "NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR MICROSOFT" |

The normative reflection of D4 is in [worldgen-parity](specs/worldgen-parity/spec.md), and of D5 in
[benchmarks-and-viz](specs/benchmarks-and-viz/spec.md).

#### Why a 3-way GIF over a bench dashboard?

Early on we considered making a separate parity verification harness the star of the demo. The 3-way top view is superior. Putting three panels side by side on the same seed embeds the parity proof in the screen itself, making a separate harness unnecessary, and it covers the technical and general audiences at once. A dashboard is read only by the technical audience.

But this forces D4. If parity is embedded in the screen, then a parity break is embedded in the screen too. If our side generated only terrain and dropped features, the audience would notice immediately and the flex would flip into fraud.

#### Why seed search was abandoned

By ROI criteria, seed search was the optimal solution. Inter-seed dependency is zero, so there is no Amdahl share; only the biome/structure layers are evaluated, so reproducing the decoration RNG order is unnecessary; cubiomes exists as a proven C reference; and a personal laptop cannot scan 10^12 seeds, so a compute-sales point actually exists.

But once D1 was settled, it was eliminated. Seed search **shows nothing on screen.** "Scanned 10^12 seeds" is just a number; there is no picture for the audience to marvel at. By the FOMO metric it is the worst option.

#### Anti-goals (explicit rejections)

- Billing, subscriptions, compute sales (D1)
- A pregen service (wall 2)
- A seed-map website (wall 3)
- A seed search service (the section above)
- A general-purpose procedural terrain generation API beyond MC: the market is large, but the technology reuse is low and it is unrelated to this project's flex

#### Pitfalls

1. **"Fast" is invisible; only "stutter" is visible.** If you target only the general-user audience, the optimization work gets consumed as "nice server" and that is the end of it. The 3-way side-by-side comparison is the only format that escapes this curse.
2. **Picking an outdated version as the comparison baseline gets you torn apart immediately.** C2ME has already introduced a density function compiler. Panel 2 of the GIF must be the latest C2ME release.
3. **Hiding the full-pipeline numbers collapses at the first comment.** Disclosing up front that features are sequentially dependent, which limits GPU use and parallelization, is what earns trust.

#### Verification

- The three panels of the 3-way GIF draw visually identical terrain from the same seed
- The region file `sha256` of the three configurations match (ADR-002 D3; refined by ADR-007 into the canonical hash)
- The release README and video descriptions include the D5 disclaimer

#### When this might break

- If Mojang revises the Usage Guidelines to explicitly prohibit third-party worldgen reimplementations
- If ROI re-enters as a goal: then this ADR must be superseded and the seed search option re-examined

#### References

- https://www.minecraft.net/en-us/usage-guidelines
- https://modrinth.com/project/VSNURh3q (C2ME)
- https://github.com/Cubitect/cubiomes-viewer

### ADR-005: Isolate the network/packet layer (P4) as a separate product; it does not affect Phase 1 design (Phase 4, 2026-07-27)

**Status:** Decided
**Type:** Scope boundary
**Phase:** 4 (paper-only, re-evaluate after P1-P3 are complete)

#### Context

The user laid out a long-term roadmap:

> "For the final roadmap, I also want to try C-level network/packet-level optimization with no Java." (owner directive, translated from the Korean original)

and later clarified its nature:

> "P4 was just a long-term roadmap idea I floated." (owner directive, translated from the Korean original)

We re-applied the roofline model to check whether P4 sits on the same physical laws as P1-P3:

| Workload | arithmetic intensity | Verdict |
|---|---|---|
| Chunk generation | **21.36 flops/byte** | COMPUTE bound |
| Chunk zlib compression | 2.03 | MEMORY/IO bound |
| **Packet serialization** | **0.00** | **MEMORY/IO bound** |

**The accumulated weapons do not transfer.** AVX-512, the 32 zmm registers, `vpermt2pd`, working around the FMA prohibition, and CPI tuning are all for compute bound workloads. What the packet layer needs is `io_uring`, zero-copy, `sendmmsg`, and syscall batching, a completely different skill set.

Compression is already a solved problem. libdeflate / ISA-L / zlib-ng are 2x or more faster than zlib and battle-tested. Implementing it ourselves is NIH; assembly from libraries is the right answer.

More importantly, **P4 invalidates ADR-003 D5**. Replacing the packet layer means we would have to own player/entity/inventory/tick-loop state, and at that moment the JVM server that is the fallback target disappears, making fallback impossible in principle. In other words P4 is not a "mod" but a "full server replacement", the same category as Pumpkin.

#### Decision (3 core decisions)

| # | Decision | Key point |
|---|---|---|
| D1 | **P4 is isolated as a separate product. It is not a design input for Phases 1-3** | Do not pre-install a P4-aware state management layer in the core |
| D2 | **P3 (region I/O) is included in P1** | Parity verification needs region comparison, and compression is library assembly, so the cost is low |
| D3 | **Isolation does not block P4; it enables it** | If the core is a pure compute library, it is reused as-is in P4 |

#### Roadmap evaluation by axis

| Axis | Nature | Headroom | Required tech | Prerequisite | Verdict |
|---|---|---|---|---|---|
| P1 worldgen core | compute | high (AI 21.4) | directly SIMD/CPI | none | top priority |
| P2 batching/scheduler | compute | GC removal is the essence | arena/SoA | P1 | **absorbed into P1** |
| P3 region I/O | IO | libdeflate etc. | library assembly | P1 | included |
| P4 network/packet | IO | AI 0.00 | io_uring etc. | entire server | **isolated** |

The rationale for absorbing P2 into P1 is recorded in ADR-003 D3. If batching stays in Java, it hits the GC wall and the P1 gains never show. This is not scope expansion but something P1 should have included from the start, an item missing from the initial design.

#### Why isolation enables P4

That Pumpkin/Valence can use our core means the core is also reused as-is if we later build a server. Conversely, if we put state management into the core now with P4 in mind, the core becomes a half-baked thing that fits none of the four consumers. This is the speculative implementation pattern the grill-me skill warns about.

#### Anti-goals

- Putting player/entity/inventory/tick-loop state into the core (D1)
- Implementing a compression algorithm ourselves (assemble libdeflate instead)
- Scheduling P4 as a task in the Phase 1 plan

#### Pitfalls

1. **The "we will need it later, so add it now" logic.** Adding state hooks to the core on the grounds of P4 is the most common contamination path. ADR-003 D1 is the line of defense.
2. **Confusing P3 with P4.** Writing region files (P3) is the job of the CLI/mod layer outside the core; the core only fills buffers. If this boundary blurs, D1 collapses.

#### Verification

- No socket, player, or tick-loop related symbols exist in Phase 1-3 code (normative: [core-abi](specs/core-abi/spec.md))
- The core header has no state lifecycle API

#### When this might break

- If P1-P3 are complete and the user promotes P4 to an actual goal: then this ADR must be superseded and a replacement strategy for ADR-003 D5 (fallback) designed anew

#### References

- ADR-003 (fallback strategy, module boundaries: [core-abi/context.md](specs/core-abi/context.md))
- https://github.com/zlib-ng/zlib-ng/issues/1486 (compression library bench)
- https://minecraft.wiki/w/Java_Edition_protocol/Packets
