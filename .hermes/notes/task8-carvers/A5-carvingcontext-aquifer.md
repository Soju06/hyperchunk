# A5 — CarvingContext + carver↔aquifer interplay (MC 26.2, unobfuscated server bytecode)

Source of truth: `javap -p -c -constants` against
`/home/ubuntu/projects/hyperchunk/tools/golden/work/server`, plus the 26.2 datapack JSON under
`tools/golden/work/server/data/minecraft/worldgen/` (mirrored in `reference/`). All pseudocode is a
1:1 reconstruction from bytecode. No vanilla-source guessing.

Companion notes: A1 (applyCarvers builds CarvingContext/fetches aquifer), A2 (WorldCarver.getCarveState
is the only carve-path aquifer caller; topMaterial is the only surface-rule caller), task7 A1 §11
(SurfaceSystem.topMaterial — complete reconstruction, reused here). This note owns the CARVE-TIME
evaluation-mode question (the dead run's open flag) and what the C port must do about it.

---

## 1. `CarvingContext` — thin holder, no logic

```java
public class CarvingContext extends WorldGenerationContext {
    private final RegistryAccess registryAccess;
    private final NoiseChunk noiseChunk;
    private final RandomState randomState;
    private final SurfaceRules$RuleSource surfaceRule;

    public CarvingContext(NoiseBasedChunkGenerator gen, RegistryAccess ra,
                          LevelHeightAccessor height, NoiseChunk nc,
                          RandomState rs, SurfaceRules.RuleSource rule) {
        super(gen, height);        // WorldGenerationContext: minY/height clamp (A7 §6.3)
        this.registryAccess = ra; this.noiseChunk = nc; this.randomState = rs; this.surfaceRule = rule;
    }
    public Optional<BlockState> topMaterial(Function<BlockPos,Holder<Biome>> biomeGetter,
                                            ChunkAccess chunk, BlockPos pos, boolean hasFluid) {
        return randomState.surfaceSystem().topMaterial(surfaceRule, this, biomeGetter,
                                                       chunk, noiseChunk, pos, hasFluid);
    }
    // + registryAccess() / randomState() accessors. NOTHING else.
}
```

- `getMinGenY()/getGenDepth()` used by carveEllipsoid bounds (A2 §2) and `VerticalAnchor.resolveY`
  (A7 §6.2) come from the `WorldGenerationContext` super — overworld: −64 / 384.
- `topMaterial` delegates 1:1 to `SurfaceSystem.topMaterial` — fully reconstructed in
  **task7 A1 §11**: builds a FRESH `SurfaceRules.Context` per call (possibleBiomes = null),
  `ruleSource.apply(ctx)`, `ctx.updateXZ(x, z)`, `ctx.updateY(1, 1, hasFluid ? y+1 : Integer.MIN_VALUE, y)`,
  `Optional.ofNullable(rule.tryApply(x, y, z))`.
  C consequence: all per-context lazy memos (surface depth, secondary noise, steep, per-rule
  condition state) START CLEAN on every topMaterial call — reuse surface.c's rule engine but with a
  context reset per invocation. `updateXZ` always burns `surfaceNoise.getValue(x,0,z)` +
  `noiseRandom.at(x,0,z).nextDouble()` (positional, stateless — no cross-call RNG stream).

## 2. Which aquifer? — the NOISE-stage NoiseChunk's, same instance

A1 §7.1: `chunk.getOrCreateNoiseChunk(...)` returns the ProtoChunk-cached NoiseChunk built by the
NOISE stage; the creation lambda does NOT run during normal generation. `noiseChunk.aquifer()` is
that same `Aquifer$NoiseBasedAquifer` with its accumulated `aquiferLocationCache`/`aquiferCache`
state. Because every cache entry is a pure function of position (positional RNG + pointwise DF
evals + psl memo), a re-initialized aquifer produces bit-identical results — the C port may reuse
the live `hc_noise_chunk_t` (vanilla-faithful, cheaper) or re-init; values are identical either way.

NoiseChunk ctor bytecode (offsets 388–483): the router handed to `Aquifer.create` is
**slot 11 = `randomState.router().mapAll(this::wrap)` — the WRAPPED router**, chunk pos =
`SectionPos.blockToSectionCoord(minBlockX/Z)`, RNG = `randomState.aquiferRandom()`, bounds =
`noiseSettings.minY()/height()`. (So barrier/floodedness/spread/lava/erosion/depth fields are the
NoiseChunk-wrapped DFs — the wrap only matters where the subtree contains marker nodes, §4.)

## 3. Carve-time call sites (the ONLY two aquifer/surface-rule touchpoints)

From A2 (bytecode-verified there):
1. `WorldCarver.getCarveState`: `aquifer.computeSubstance(new DensityFunction.SinglePointContext(
   pos.getX(), pos.getY(), pos.getZ()), 0.0)` — density argument is the literal `dconst_0`.
   Result: null → block preserved (carveBlock returns false); else the returned state is placed
   (lava floor `y <= lavaLevel.resolveY` bypasses the aquifer entirely, A2 §4).
2. `carveBlock` step 7: `aquifer.shouldScheduleFluidUpdate()` read AFTER computeSubstance, gates
   `markPosForPostProcessing` only (not block choice) — golden 06 dumps blocks+heightmaps, so the
   flag is parity-neutral for Task 8 but its STATE MUTATION order is not observable anyway
   (every computeSubstance path rewrites it; A6/Task-9 ground for the postprocess list itself).

## 4. THE OPEN FLAG RESOLVED — barrier noise mode at carve time

Mechanism (Aquifer$NoiseBasedAquifer bytecode):
- `calculatePressure(ctx, MutableDouble barrier, f1, f2)` evaluates `barrierNoise.compute(ctx)`
  with the CALLER-PASSED ctx and memoizes it in the per-call MutableDouble(NaN).
  - doFill: ctx = the NoiseChunk itself → BLOCK mode.
  - carve: ctx = a fresh `SinglePointContext` → **SP mode**. ⟵ answer to the dead run's question.
- Every OTHER noise read (`fluidLevelFloodednessNoise`, `fluidLevelSpreadNoise`, `lavaNoise`,
  `erosion`, `depth`) happens inside `computeFluid`/`computeSurfaceLevel`/`computeFluidType`,
  which construct their OWN `SinglePointContext` (bytecode lines: `new #333 SinglePointContext`
  ×3 sites) — i.e. those are SP in BOTH stages. Only barrier's mode differs between stages.

Value-neutrality proof for 26.2 shipped data:
- barrier = `{"type":"minecraft:noise","noise":"minecraft:aquifer_barrier",xz=1.0,y=0.5}` — a plain
  noise node, NO marker wrappers (`reference/overworld-26.2.json` noise_router; same for
  floodedness/spread/lava). `mapAll(wrap)` only binds the NoiseHolder; there is no
  interpolated/flat_cache/cache_once node whose result depends on ctx identity. Hence
  **BLOCK ≡ SP bit-exactly for the barrier root**, and hyperchunk's existing
  `hc_aquifer_substance` (barrier via `hc_nc_eval_block`, aquifer.c:292) is ALREADY correct for
  carve-time calls — no fill-cursor state is touched because the subtree contains no
  INTERPOLATED/FLAT_CACHE ops (df_eval.c dispatch reads cursor state only for those ops).
- erosion (`flat_cache(shifted_noise)`) and depth (`add(y_clamped_gradient, overworld/offset)`)
  DO contain flat_cache — but they are SP in both stages (above), and our compute_fluid already
  evaluates them with `hc_nc_eval_sp` (aquifer.c:180/182). FlatCache.compute is ctx-INSENSITIVE
  (quart-window test only, no identity check) — window semantics already reproduced in df_eval.c.
- Guard for datapack futures: if a datapack puts marker nodes inside `barrier`, BLOCK vs SP could
  diverge; ADR-003 D5 fallback territory, not a Task 8 concern. Documented in carvers.c header.

Callability audit of `hc_aquifer_substance` outside doFill (all pass):
- 12-cell scan bounds: carve positions are center-chunk columns (writeRadius 0, A1 §2.1),
  y ∈ [minGenY+1, minGenY+genDepth−8] (A2 §2) — same grid coverage as fill; `cell_index` asserts hold.
- `get_status` → `compute_fluid` → `hc_nc_psl` (pure memo) + SP evals: no fill-cursor dependency.
- `should_schedule_fluid_update`: rewritten on every path; stale state from the noise stage is
  unobservable.

## 5. computeSubstance carve-path semantics (density = 0.0)

With `density = 0.0` (contrast doFill where density is the real final_density value ≤ 0):
- `density > 0.0` fast-return(null) — NOT taken (0.0 is not > 0.0).
- `y > skipSamplingAboveY` → `globalPicker.at(y)` result (air above sea level / water below —
  the "cheap path"; sets shouldScheduleFluidUpdate=false).
- global lava floor (y < min(−54, seaLevel) → level −54) → LAVA short-circuit.
- pressure tests: `0.0 + pressure > 0.0` ⇒ any positive mixed pressure preserves the block
  (returns null → carveBlock false). Since density=0.0 (vs negative fill densities), carve-time
  preservation triggers EASIER — this is the "barrier" wall effect around carver-pierced aquifers.
- Otherwise `fs1.at(y)`: water / lava / air by fluid status — this is how carved caves flood.
  (fluid level=0 water/lava sightings in the golden 06 palettes come from this path.)

## 6. C port directives (summary)

1. Reuse `hc_aquifer_substance(aq, x, y, z, 0.0)` verbatim from the carve loop; do not fork a
   carver-local aquifer. Keep the noise-stage `hc_noise_chunk_t` alive across 04→05→06 (already the
   pattern for 05's noiseChunk conditions).
2. `getCarveState`: `y <= resolveY(lava_level)` (above_bottom 8 → y ≤ −56) → lava, bypass aquifer.
   NOTE the asymmetry with doFill's global picker (y < −54 strict, level −54): carvers stamp lava
   at y ≤ −56 only; −55/−54 carve outcomes come from the aquifer path.
3. `topMaterial`: fresh surface-rule context per call; stoneDepthAbove=1, stoneDepthBelow=1,
   waterHeight = hasFluid ? y+1 : INT32_MIN, blockY = y; possibleBiomes absent (task7 A1 §11).
4. `shouldScheduleFluidUpdate` + `markPosForPostProcessing`: parity-neutral for the 06 gate
   (blocks/heightmaps only); keep the flag maintained (already is), leave markPos a no-op, hand the
   decision to Task 9 (features may consume the postprocess list — A6/Task-9 handoff).

## 7. OPEN items

- OPEN: `Aquifer$FluidStatus`/`FluidPicker` bytecode re-verified only to signature level here;
  algorithm bodies were verified during Task 6 (aquifer.c golden-passed) — not re-derived.
- OPEN (Task 9): exact `markPosForPostProcessing` packing + promotion semantics (A6 ground).
- Presumed (data-audited, not exhaustively proven for all dimensions): no marker nodes inside
  barrier/floodedness/spread/lava subtrees in ANY shipped 26.2 noise_settings — checked overworld
  only; nether/end out of scope (ADR-002 D4).

## 8. sulfur scan

`strings` over `CarvingContext.class`, `Aquifer*.class`: no "sulfur" hits. As established (A1 §11,
A2 §10): the 26.2 sulfur delta is data-only (replaceables tag + sulfur_caves biome surface rules).
