# A8 — Task 9b completion record + Task 10 (light) handoff

Written at 9b completion (2026-07-31, resume run 2). Companions: R1–R5 (this
dir), task9a A1–A7, golden/features-trace/FORMAT.md.

## 1. Verdict

**Features stage COMPLETE and gated.** tests/parity/test_features_walk.c is
the committed acceptance (ADR-007 D2/D3), CI-enabled, strict:

- (a) decoration seeds: 81/81 primary + 81/81 alt manifest lines recomputed.
- (b) vanilla trace line-exact for all 9 grid chunks, steps 0..10:
  positions + npos + placed bits, **0 placed-bit drifts** (cap 0). The 9a
  moss-spill drift class disappeared exactly as predicted (A7 §3) once
  step-9 spills became real.
- (c) 07_features **blocks + 6 heightmaps 0 diff on BOTH bundles**
  (55,658 / 56,322 blocks placed per replay). Two different recorded orders,
  one code path — the Tier-2/D3 evidence.
- 20/20 ctest; clean build zero warnings; check_no_fma.sh PASS (33,083
  insns); check_sanitizers.sh PASS (ASan+UBSan full suite); goldens
  untouched.

## 2. Per-family verdicts (all bit-exact at the gate)

| family | verdict | notes |
|---|---|---|
| ore/blob/magma/springs (9a) | bit-exact | unchanged |
| trees (jungle/oak: straight/mega/fancy + cocoa/trunk-vine/leave-vine, fallen) | bit-exact | R2 implementation was correct except the two cross-cutting fixes below |
| bamboo, simple_block (grass/fern/flowers/tall_grass/dripleaf), vines | bit-exact | R4 |
| glow_lichen multiface_growth (+ spreader) | bit-exact | R3 §1 — stationary-cursor search, shuffled dirs, spread ignores can_be_placed_on |
| cave_vines / dripleaf block_column | bit-exact | R3 §2 — up-front heights, offset-by-one scan, prioritize_tip truncate |
| moss/clay vegetation_patch (+waterlogged pool) | bit-exact | R3 §3 — inclusive edge keep, invisible accept-then-abort columns |
| freeze_top_layer | bit-exact | zero-draw walk item (grid is warm) |
| rooted_azalea_tree / trees_water / seagrass_river / patch_* / mushrooms / melon / pumpkin / sugar_cane / firefly bush | pipeline-exact, body never fires in grid (npos 0) | bodies remain die-if-executed or UNIMPLEMENTED placed=-1 markers |

## 3. The three bugs that separated "runs" from "0 diff"

1. **jset treeify off-by-one (9a latent).** JDK putVal treeifies when a
   chain becomes 9, not 8 (binCount excludes the first node). Then the real
   finding: vegetation_patch ground sets DO treeify at cap ≥ 64 —
   **java.util.HashMap TreeNode machinery is now fully ported** (jset.c:
   treeify, putTreeVal, split+untreeify, removeTreeNode with the JDK-9+
   movable gate, RB balance, moveRootToFront; verified against THIS
   machine's JDK 25 javap). Equal-hash tie-breaks (identityHashCode — not
   reproducible) die loudly; set geometry keeps them unreachable (R3 note).
2. **updateShapeAtEdge is not a no-op (R2 §8 refuted → R5).** TreeFeature's
   final pass shape-updates both cells of every boundary face of the tree
   shape, in Z→Y→X axis-pass order, writes visible to later faces. It
   deletes decorator vines whose face support was overwritten by a LATER
   tree's foliage (vine ∈ #replaceable_by_trees) and whose vine-above rule
   fails — 13/17 phantom vines in the grid, plus every downstream
   world-read desync in c.1.1. Ported in features_tree.c with per-block
   updateShape dispatch; cave_vines_plant→head conversion (the only
   RNG-drawing override — worldgen_region_random) is die-if-reached.
3. **isFaceSturdy(FULL) is face-dependent for azalea** (R1 §7 open item,
   confirmed): SHAPE = or(column(16, 8..16), column(4, 0..8)) — the UP face
   is a full 16×16 slab top. vegetation_patch floor columns whose surface
   is an azalea bush are ACCEPTED by vanilla (then placeGround aborts at
   i=0 — azalea not replaceable — leaving no visible write), burning one
   extra_bottom_block_chance draw. One such invisible draw desynced a
   c.1.1 clay-pool patch; found by dumping the body-entry world + float
   stream and realigning in a Python differential sim
   (hc_featx_face_sturdy_full in features_internal.h).

The 9a placed-bit drift (A7 §3) needed no fix: implementing step 9 made the
neighbor moss spill real and the ore rule-test now sees it, as diagnosed.

## 4. Residual approximations (documented, gate-clean)

- `isFaceSturdy` beyond {full cube, azalea-UP}: CENTER/RIGID collapse to the
  same table. Azalea's DOWN face would pass CENTER (4×4 trunk ⊇ 2×2 center)
  — only spore_blossom-under-azalea would care; not in any golden region.
  If a future gate diff points at a sturdiness call, extend
  hc_featx_face_sturdy_full first.
- Small mushrooms in updateShape (light-dependent canSurvive) and
  cave_vines_plant→head (region random): die-if-reached.
- Cross-chunk spill halves are gated only via later-decorated chunks' dumps
  (a chunk's 07 dump snapshots before later neighbors decorate) — the x<16
  half of a c.1.1-origin patch is structurally ungated. Both-bundle
  replay covers most of this blind spot with a second order.
- Ring-chunk decoration (all 81 manifest entries) still out of scope: grid
  gates don't need it; 08+ gates will (A7 §5 item 7).

## 5. Task 10 (light) handoff

- **Light emitters in the features palette** (26.2 Blocks.<clinit> priors —
  re-verify constants in the Task 10 recon before use): lava 15,
  magma_block 3, glow_lichen 7 (emission gated on ≥1 face — all placed
  states qualify), cave_vines/cave_vines_plant with berries=true 14,
  redstone_ore only when lit=true (our states are lit=false → 0). Nothing
  else in hc_blocks.h emits.
- **Heightmaps**: chunks now carry the 4 FINAL maps (chunk->heightmap_final,
  lazily primed + incrementally updated by every feature write; gate-proven
  against the 07 dumps) plus the 2 frozen WG maps. The 08 stage inherits
  them as-is; vanilla does not re-prime at INITIALIZE_LIGHT
  (heightmapsAfter stays FINAL_HEIGHTMAPS from carvers on — R1 §1.7).
- **Light-relevant per-state flags to recon for 10**: lightEmission (above),
  lightDampening/propagatesSkylightDown (leaves noOcclusion → dampening 1?,
  water 1, lichen/plants 0 dampening... needs its own R-note — blocks.c
  FLAGS has occlusion/fluid bits but NOT light opacity yet).
- **08_initialize_light dumps**: present in golden bundles for ring chunks
  (features-trace tree) — check which sections they carry before designing
  the gate; the 07 heightmap dump semantics (raw = firstFree − minY,
  x-minor rows) are confirmed by this gate.
- Debug tooling that will transfer: HC_WALK_DUMP_DIR (per-chunk our-07
  dumps), HC_TREE_DEBUG / HC_TREE_DEBUG_CELL, HC_VPATCH_DEBUG (draw-stream
  logs; the vpatch one + a body-entry world dump pinned the azalea bug in
  ~30 minutes — reuse the pattern for light diffs).
