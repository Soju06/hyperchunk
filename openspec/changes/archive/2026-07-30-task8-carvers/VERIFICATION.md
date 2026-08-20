# Task 8 (06_carvers) — verification record, 2026-07-30

Final state: 2c29a44 (parity green) + review-fix commit on top. Gates: 18/18 ctest, 9/9 chunks 0 diff
(55,782 golden lines, blocks+heightmaps, chained from OUR 04+05 state),
check_no_fma PASS, ASan+UBSan full-suite PASS, tree clean.

## Root cause of the salvaged run's 13,023-diff state

NOT RNG. `overworld_carver_replaceables` lists property-less block names
(`minecraft:deepslate`, `water`, ...); the block table stores canonical
state names (`deepslate[axis=y]`, `water[level=0]`, ...). `tag_expand`'s
exact-match lookup silently skipped deepslate/water/grass_block/mycelium/
podzol → never replaceable → 8,137 one-directional missed cells (golden
carved, we didn't; zero extra, zero RNG drift). Vanilla `canReplaceBlock`
is `state.is(tag)` = block-level; fix = match the `[`-prefix of every
table state (`tag_mark_block`). One fix closed the entire divergence —
the run-2 RNG draw ladder was already bit-correct.

## Mutation probes (committed state 2c29a44, /tmp worktree, sequential)

Verdicts against the 9-chunk stage gate (unit = mth sin table gate):

| probe | mutation | stage | verdict |
|---|---|---|---|
| P2 | setLargeFeatureSeed cx/cz mix swap | FAIL 19,936 | covered |
| P5 | ellipsoid y desc→asc | FAIL 2 | covered (repair-order only) |
| P6 | dy −0.5→+0.5 | FAIL 4,018 | covered |
| P7 | nested cave-count draws flattened | FAIL 33,088 | covered |
| P10 | grass repair w/o dirt-below cond | FAIL 15 | covered |
| P14 | room extra-tunnel nextInt(4)→(3) | FAIL 6,325 | covered |
| P16 | Mth.cos offset 16384→16383 | FAIL 9 + unit FAIL | covered |
| P1 | seed+idx → seed^idx | PASS | blind — seed+1==seed^1 for this seed (low bit 0), so only the canyon stream (idx 2) mutated, and canyon contributes 0 cells (below) |
| P3 | isStartChunk <= → < | PASS | equivalent mutant — 0.15f/0.07f/0.01f are not k·2⁻²⁴; nextFloat()==p impossible on ANY seed with shipped data |
| P4 | source-chunk 17×17 transpose | PASS | blind — pure order permutation; per-(chunk,carver) reseed makes RNG order-free; only mask/repair order effects could show and none materialized |
| P8 | tunnel split > 1.0f → >= | PASS | blind — no tunnel drew thickness exactly 1.0f (possible but rare event) |
| P9 | lava floor <= → < | PASS | blind — carving never reached y ≤ −56 in these 9 chunks (min carved y ≈ −45); lava-floor branch golden-uncovered |
| P11 | can_reach width sum float→double | PASS | blind — boundary never decisive |
| P12 | trapezoid d1·h+d2·g → d1·g+d2·h | PASS | blind — trapezoid only feeds canyon thickness (canyon = 0 cells) |
| P13 | canyon width-factor yi==0 recalc dropped | PASS | blind — canyon 0 cells |
| P15 | carving-mask revisit check removed | PASS | blind — recarve is near-idempotent (air not replaceable; water recarve → same positional aquifer result) |

## Gate coverage boundaries (measured, not guessed)

- **Canyon path is 100% golden-blind on seed 1234567890**: disabling
  `hc_canyon_carve` entirely still gives 0 diff and the identical 14,468
  carve count. All canyon semantics (A4) rest on note-review + the
  adversarial review below. A canyon-intersecting golden seed would close
  this (canyon probability 0.01/source-chunk).
- Lava-floor substitution (y ≤ lava_level = −56) unreached.
- Trapezoid float provider unexercised (canyon-only).
- Covered strongly: cave + cave_extra_underground full draw ladder, room
  path, 8-neighborhood seeding/mix, ellipsoid geometry, aquifer water
  substitution (1,400+ water cells), grass repair via topMaterial, WG
  heightmap updates (c.1.0/c.1.-1 heightmap sections).

## Adversarial review (6 dimensions vs A1–A7, javap tiebreak)

24 agents (6 finders + adversarial verifiers per finding; first run lost
5 lanes to API 429, resumed with cache). **Zero shipped-data parity
deviations confirmed.** All confirmed-real findings are datapack-only or
UB-discipline items.

Fixed in-tree (commit after 2c29a44):

1. **`seed + idx` signed-overflow UB** (low): Java `ladd` wraps mod 2⁶⁴;
   C signed add is UB for seeds within 2 of INT64_MAX (legal user seeds) —
   our own UBSan gate would fire. Fixed with unsigned add (same discipline
   as the multiply/xor three lines below).
2. **NaN polarity inverted on the ellipsoid column disc gate** (low):
   bytecode is `dcmpl; iflt` — Java PROCESSES the column when
   dx²+dz² is NaN; our `!(sum < 1.0)` skipped it. Reachable only via
   datapack h_radius=0 + measure-zero center coincidence. Fixed to
   `sum >= 1.0` (NaN→false→process). A2 §2's gloss was the origin; the
   cave/canyon skip-checkers and the reach check were verified to already
   have correct NaN polarity.
3. **Canyon distance cast lacked JVM f2i saturation** (med): `f2i`
   saturates (NaN→0, overflow→INT_MAX/MIN); C raw cast is UB. Unreachable
   with shipped distance_factor ∈ [0.75,1.0) — fixed with jvm_f2i (same
   pattern as mth_floor/d2l).
4. **`debug_settings.debug_mode` silently ignored** (med): 26.2
   isDebugEnabled is config-driven and gates mask-revisit bypass,
   canReplaceBlock bypass, and debug-state substitution. Now fail-loud at
   config compile (shipped configs never set it).
5. **fprov_parse rejected `{"type":"minecraft:constant","value":v}`**
   (low, loud failure): vanilla FloatProviders.CODEC = either(FLOAT,
   dispatch). Object-form constant now accepted.
6. **Missing codec validations** (low/med): probability floatRange(0,1);
   UniformFloat validate max > min (strict); TrapezoidFloat validate
   max ≥ min and plateau ≤ span. We silently accepted configs vanilla
   refuses to load. All three added (exact bytecode semantics).
7. **hc_lcg_next_int(bound≤0)** (low): Java throws; ours silently hit the
   pow2 path returning 0 forever. Debug assert added (release zero-cost).

Documented, deferred (shipped-data-correct; needs deeper layers):

8. **Codec.FLOAT double-rounding** (med, systemic): vanilla parses float
   fields via GSON LazilyParsedNumber → Float.parseFloat(raw literal)
   (single rounding); we strtod→double→(float) (double rounding). 1-ulp
   divergence exists for adversarial literals (witness:
   "16777217.00000000000000001"); ALL shipped literals coincide (gates
   prove it). Affects every Codec.FLOAT consumer (carver probability,
   float providers; surface thresholds too). Fix = preserve raw literal
   slice + strtof — Task 12 datapack schema parser. json.c comment
   corrected to scope the "bit-match" claim to Double consumers.
9. **`interpolated` node under aquifer barrier** (low/med): carve-time
   barrier is SP-mode (fresh eval); our hc_aquifer_substance always
   evaluates BLOCK-mode, which for an `interpolated` node returns the
   stale fill cursor. Shipped barrier has no marker nodes (BLOCK ≡ SP,
   value-proven). flat_cache/cache_once are ctx-insensitive both sides
   (verified — the A5 §4 "any marker" claim narrowed to `interpolated`
   only). Guard belongs in the Task 12 schema validator (ADR-003 D5).
10. **Loud-failure coverage gaps** (low): y height provider only
    minecraft:uniform (vanilla also accepts constant-anchor shorthand +
    3 other provider types), replaceable only as single tag/name string
    (vanilla HolderSet also accepts inline lists), clamped_normal float
    provider (needs nextGaussian), un-namespaced type spellings
    ("uniform" vs "minecraft:uniform"). All fail loud at config compile —
    ADR-003 D5 fallback class, not silent divergence.
11. **Tag machinery** (low): unknown-block names in tags are silently
    skipped — harmless by construction (our pipeline cannot place a block
    outside the table, so membership is unobservable); recursion depth
    cap 4 vs vanilla unbounded (shipped max depth 2).

Clean areas (verified bit-equal against bytecode by the finders): full
cave draw ladder incl. tunnel recursion/room/thickness, canyon draw
ladder + width factors + vertical radius, 17×17 orchestration + reseed
recipe + carverBiome value-neutrality (datapack-audited across all
overworld biomes), mask packing/word math, WG heightmap update semantics,
carveBlock step order, topMaterial fresh-context recipe, aquifer
substitution paths (incl. y > skipSamplingAboveY fast path and pressure
preservation), Mth sin/cos table + d2l truncation, LCG draw primitives.

## 26.2 findings for the record

- **Seeding recipe (verified bytecode)**: one WorldgenRandom
  (LegacyRandomSource) per stage call; per (source chunk, carverIndex):
  `setLargeFeatureSeed(worldSeed + carverIndex, srcCx, srcCz)` =
  `setSeed(seed); a=nextLong(); b=nextLong(); setSeed(cx*a ^ cz*b ^ seed)`
  — NO `|1` (contrast setDecorationSeed). carverIndex increments whether
  or not isStartChunk passes. 17×17 (radius 8), x outer / z inner, both
  ascending.
- **Sulfur**: NO sulfur carver exists. sulfur_caves biome uses the
  standard `[cave, cave_extra_underground, canyon]` list; the 26.2 delta
  is data-only (sulfur/potent_sulfur/cinnabar in the replaceables tag).
  "Sulfur-cave carving" in FORMAT.md = ordinary carvers passing through
  sulfur-cave terrain.
- **Aquifer interplay**: carvers reuse the NOISE-stage NoiseChunk's
  aquifer (`chunk.getOrCreateNoiseChunk` cache hit) via
  `computeSubstance(SinglePointContext, 0.0)`. Barrier noise evaluates in
  SP mode at carve time vs BLOCK mode in doFill, but the 26.2 barrier
  subtree has no marker nodes → bit-identical; `hc_aquifer_substance`
  reused verbatim. Lava floor y ≤ −56 bypasses the aquifer.
- **26.2 orchestration deltas vs 1.21**: applyCarvers abstract, no
  GenerationStep.Carving arg; single carving mask (no AIR/LIQUID);
  flat `BiomeGenerationSettings.carvers` HolderSet; plain Iterator +
  manual carverIndex.
- **carverBiome gate skipped** (value-neutral): all overworld 26.2 biomes
  carry the identical 3-carver list (datapack-audited); documented as an
  ADR-003 D5 fallback trigger if a datapack diverges.

## What Task 9 (features) inherits

- Carving masks: per-target-chunk bitset (x | z<<4 | (y−minY)<<8) filled
  by stage 06 — vanilla keeps it on ProtoChunk; features (e.g. vegetation
  placement checks) may read it. Currently test-local; move to hc_chunk_t
  if Task 9 needs it.
- Heightmaps: WS_WG/OF_WG post-carve state is golden-verified (the
  FINAL_HEIGHTMAPS priming for features happens in generateFeatures, A6).
- markPosForPostProcessing is still a no-op — fluid-tick scheduling list
  is features/finalization ground (A5 §6, A6 §8).
- topMaterial (hc_surface_top_material) is implemented and carve-proven;
  features that re-apply surface rules can reuse it.
- The stage entry point is `hc_gen_carvers_stage(chunk, nc, surf, view,
  seed, carvers[3], mask)` chained after 05 on the same nc/surf/view.
