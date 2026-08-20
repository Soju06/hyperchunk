# simd-backends: Context

## Purpose & scope

[spec.md](spec.md) is the normative SSOT for the backend set, byte-identical
output, and the FMA prohibition. This document holds the decision history of
"why AVX2 is the baseline and AVX-512 is dispatch-selected, and why AVX-512's
value is registers, not width" (ADR-004), plus the measurements taken since.

## Current state (as of 2026-08)

- The AVX2 lazy kernel (P2-4) gave noise 2.97x, the AVX-512 backend (P2-10,
  measured on hc-e6) a further 1.176x (noise wall), and 2-way interleaving
  (P2-11) 1.29-1.40x on the kernels; details in the phase2 notes under
  [changes/archive/](../../changes/archive/). The 6.5x kernel multiplier that
  ADR-004 conservatively re-estimated came out at only 1.55x in measurement
  (root-cause analysis in the P2-10 note: FP chains dominate, not the
  lookup).
- The public benchmark (hc-e6, Zen5) ran on the AVX-512 backend; the rule
  that the multipliers are properties of this stage is in
  [benchmarks-and-viz/spec.md](../benchmarks-and-viz/spec.md).

## Decision history

> append-only. Existing entries are never edited. When a decision changes,
> add a new entry and mark the previous one `Superseded by`.

Placement: this document is the primary home of ADR-004. ADR-002 P1, the
origin of the FMA prohibition, lives in
[generation-pipeline/context.md](../generation-pipeline/context.md) and is
reflected as a requirement in this capability's spec.md.

### ADR-004: Make AVX2 the baseline kernel and add AVX-512 via runtime dispatch (Phase 2, 2026-07-27)

**Status:** Decided
**Type:** Architecture / Performance strategy
**Phase:** 2 (Phase 1 implemented only the scalar reference kernel)

#### Context

We were asked what adopting AVX-512 would buy and analyzed it
quantitatively. The conclusion was the opposite of intuition.

**Hardware facts (verified):**

- **Zen4's AVX-512 double-pumps through 256-bit units.** A 512-bit instruction occupies the unit for 2 cycles, so the FP throughput gain is effectively 0; the only gain is the frontend savings from a reduced instruction count. There is no throttling.
- **From Zen5 on, the datapath widens to 512 bits and the 2x is real.**
- **Intel has fused off AVX-512 at the silicon level on consumer chips since Alder Lake.** Only Sapphire Rapids-class server chips support it.

Per-core double peak with FMA prohibited (ADR-002 Pitfall 1):

| CPU | Nominal | Effective correction | Actual |
|---|---|---|---|
| Zen3 AVX2 (local) | 8 flops/cyc | 1.00 | 8.0 |
| Zen4 AVX-512 | 16 | **0.50** | **8.0** |
| Zen5 AVX-512 | 16 | 1.00 | **16.0** |

**So the value of AVX-512 is not "width."** Decomposed (noise stage only, optimized AVX2 kernel = 1.0):

| Factor | Zen5 | Zen4 | What it is |
|---|---|---|---|
| Lane width 4 -> 8 | 2.00x | **0.50x** | Zen4 gives it back to double-pump |
| **32 zmm registers** | **3.00x** | **3.00x** | Main contributor |
| `vpermt2pd` gradient lookup | 1.35x | 1.35x | 20 uop -> 3 uop |
| Mask-based branch elimination | 1.10x | 1.10x | branchless |
| Cumulative | 8.9x | 2.2x | |
| Conservative re-estimate | **6.5x** | 1.6x | |

Why registers matter more than width ties directly to the FMA prohibition:

```
FP add latency 3 cyc x 2 ports -> 6 add chains needed
FP mul latency 3 cyc x 2 ports -> 6 mul chains needed
Independent chains needed to saturate the ports: 12
```

With FMA available, mul+add is one instruction, so the chain demand halves. Bit-exact parity forbids FMA, so **the demand doubles.**

| ISA | Table residency | Available registers | Chains accommodated | Port utilization |
|---|---|---|---|---|
| AVX2 (16 ymm) | 4 | 8 | 4/12 | **33%** (L1 spills occur) |
| AVX-512 (32 zmm) | 2 | 26 | 13/12 | **100%** |

**AVX-512 is the tool that exactly compensates the loss from giving up FMA.** This is the project-specific argument that differs from generic SIMD advice.

`vpermt2pd` is a structural fit. Vanilla `ImprovedNoise` selects among 16 gradient directions with `hash & 15`, and 16 doubles are exactly 2 zmm registers. The whole table stays resident in registers and the lookup finishes in 3 uops (an AVX2 blend tree takes 20 uops; Zen3 `vgatherqpd` is microcoded and worst at ~36 cyc).

Lane occupancy, unlike on a GPU, is a non-issue. At 1400 noise samples per chunk, 8-wide means 175 iterations with 0 wasted lanes. The GPU needed thousands of chunks batched to fill its SMs.

**On the full pipeline, however, the effect shrinks:**

| Configuration | Single-core | vs AVX2 |
|---|---|---|
| AVX2 ground-up rewrite | 5.58x | - |
| Zen4 AVX-512 | 6.27x | +12% |
| Zen5 AVX-512 | 7.11x | **+27%** |
| *noise+surface infinitely accelerated* | *7.79x* | *ceiling* |

After the CPI rewrite the bottleneck shares shift to `noise 21% / features 41%`, so digging further into noise contributes little to the total. At 12 cores it is `59x -> 66x -> 75x`, and **59x and 75x are visually indistinguishable in the 3-way GIF.**

#### Decision (4 core decisions)

| # | Decision | Key point |
|---|---|---|
| D1 | **Make AVX2 the baseline kernel** | Universal since Haswell (2013). An AVX-512-only build is one most of the audience cannot even run |
| D2 | **Add AVX-512 via `cpuid` runtime dispatch** | Not discarded. Both backends coexist |
| D3 | **FMA prohibited on all backends** | Enforced by compile flags. A parity invariant |
| D4 | **Enforce byte-identical output across all backends as a CI gate** | Mandatory, not optional |

#### Why AVX2 first?

An unreproducible benchmark is instant death with a technical audience. AVX-512 exists only on Zen5 or Intel server chips, so most of the audience cannot verify it. Keeping both backends, on the other hand, adds one more flex: **"two backends, AVX2/AVX-512, identical output hashes"** serves as evidence of parity rigor.

#### Anti-goals

- An AVX-512-only build (D1)
- Gaining 2x by enabling FMA (D3; violates ADR-002 D3)
- Prioritizing further noise-stage optimization over features: the table above says features come first
- A GPU (CUDA) backend: offloading noise alone yields 2.9x total and merely moves the bottleneck to features. There is no ROI outside a batch pregen service, and ADR-001 ruled that direction out

#### Pitfalls

1. **Floating-point result mismatch across SIMD levels.** A known problem the FastNoise2 FAQ also points out. Since bit parity is the essence of the product for us, the D4 CI gate is mandatory.
2. **Buying hardware while mistaking Zen4 for Zen5.** Because of double-pump, Zen4's AVX-512 FP gain is near 0.
3. **Assuming AVX-512 downclocking.** That is Skylake-X-era folklore; Zen4/Zen5 have no fixed frequency offset. If anything, 512-bit has been observed running at slightly higher clocks than 256-bit.
4. **The local dev box has no AVX-512.** The 5900X is Zen3. The AVX-512 path cannot be measured locally and needs separate hardware. *(Later measured on hc-e6 (Zen5); P2-10 note.)*

#### Verification

- `hyperchunk-bench --isa=scalar|avx2|avx512`: output `sha256` identical across all three paths
- `objdump` confirms no FMA instructions (`vfmadd*`) exist in the artifact
- `cpuid` dispatch falls back correctly to AVX2 on hosts without AVX-512

#### When this might break

- If Zen5/AVX-512 hardware becomes ubiquitous and weakens D1's portability argument
- If measured profiles show the noise share far above 45%: then AVX-512 must move up in priority

#### References

- ADR-002 (origin of the FMA prohibition: [generation-pipeline/context.md](../generation-pipeline/context.md))
- https://www.numberworld.org/blogs/2024_8_7_zen5_avx512_teardown/
- https://chipsandcheese.com/p/zen-5s-avx-512-frequency-behavior
- https://github.com/Auburn/FastNoise2/wiki/FAQ
- https://www.felixcloutier.com/x86/vpermt2w:vpermt2d:vpermt2q:vpermt2ps:vpermt2pd
