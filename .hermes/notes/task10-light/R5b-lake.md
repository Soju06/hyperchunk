# R5b — LakeFeature (minecraft:lake) recon: lake_lava / lake_lava_underground

Source of truth: `javap -p -c -constants -cp tools/golden/libs/extracted/server-26.2.jar
net.minecraft.world.level.levelgen.feature.LakeFeature` (+ `$Configuration`,
`stateproviders.SimpleStateProvider`, `blockpredicates.{NotPredicate,MatchingBlockTagPredicate,StateTestingPredicate}`,
`Feature.markAboveForPostProcessing`). All offsets below are `place@NN` unless another
method is named. `place()` is the ONLY nontrivial method on LakeFeature (ctor + `<clinit>` aside).

## 0. Concrete config (reference/configured_feature/lake_lava.json)

| Configuration field (Configuration ctor puts fields in this codec order: fluid, barrier, can_place_feature, can_replace_with_air_or_fluid, can_replace_with_barrier — Configuration.lambda$static$0@0-103) | value |
|---|---|
| `fluid` | simple_state_provider → `minecraft:lava[level=0]` |
| `barrier` | simple_state_provider → `minecraft:stone` |
| `canPlaceFeature` | `minecraft:true` (always passes) |
| `canReplaceWithAirOrFluid` | `not(matching_block_tag #minecraft:features_cannot_replace)` |
| `canReplaceWithBarrier` | `not(matching_block_tag #minecraft:lava_pool_stone_cannot_replace)` |

`static AIR = Blocks.CAVE_AIR.defaultBlockState()` (`<clinit>`@0-6). Air placed by lakes is
**cave_air**, not air.

## 1. place() — C-ready pseudocode

Locals: slot2=pos (origin, then re-based), 3=level, 4=random (=ctx.random(); the per-feature
WorldgenRandom), 5=config, 6=`boolean grid[2048]`, 7=blobCount, 8=blob index then reused for
fluidState.

```c
/* ctx unpack: origin @0-4, level @5-9, random @10-14, config @16-23 */

/* -- guard -------------------------------------------------------- */
if (origin.y <= level.getMinY() + 4)      /* @25-37 if_icmpgt 42; overworld minY=-64 → abort y<=-60 */
    return false;                          /* @40-41 */

pos = origin.offset(-8, -4, -8);           /* @42-52: bipush -8,-4,-8 */
bool grid[2048] = {false};                 /* @53-58 newarray boolean 2048 = 16*16*8 */

/* -- RNG draw #1: blob count -------------------------------------- */
int blobs = random.nextInt(4) + 4;         /* @60-70 → 4..7 */

/* -- blob loop: 6 nextDouble per blob, exact order/expressions ----- */
for (int i = 0; i < blobs; i++) {          /* @72-362 */
    double a  = random.nextDouble() * 6.0 + 3.0;                       /* @82-97   x-diameter */
    double b  = random.nextDouble() * 4.0 + 2.0;                       /* @99-114  y-diameter */
    double c  = random.nextDouble() * 6.0 + 3.0;                       /* @116-131 z-diameter */
    /* center coords — evaluation order EXACTLY as written (left-to-right adds): */
    double cx = random.nextDouble() * (16.0 - a - 2.0) + 1.0 + a / 2.0; /* @133-160: r*((16.0-a)-2.0), +1.0, +(a/2.0) */
    double cy = random.nextDouble() * (8.0  - b - 4.0) + 2.0 + b / 2.0; /* @162-191: r*((8.0-b)-4.0),  +2.0, +(b/2.0) */
    double cz = random.nextDouble() * (16.0 - c - 2.0) + 1.0 + c / 2.0; /* @193-220 */

    /* grid fill: x outer 1..14, z middle 1..14, y inner 1..6 (@222-356) */
    for (int x = 1; x < 15; x++)
      for (int z = 1; z < 15; z++)
        for (int y = 1; y < 7; y++) {
            double dx = ((double)x - cx) / (a / 2.0);   /* @252-265: i2d, dsub, then a/2.0, ddiv */
            double dy = ((double)y - cy) / (b / 2.0);   /* @267-280 (inner var y) */
            double dz = ((double)z - cz) / (c / 2.0);   /* @282-295 (middle var z) */
            double s  = dx*dx + dy*dy + dz*dz;          /* @297-314: (dx*dx + dy*dy) + dz*dz */
            if (s < 1.0)                                 /* @316-320 dcmpg ifge → set iff s < 1.0 strictly */
                grid[(x*16 + z)*8 + y] = true;           /* @323-340 index (x*16+z)*8+y */
        }
}

/* -- fluid state (0 RNG draws for simple provider) ----------------- */
BlockState fluid = config.fluid.getState(level, random, pos);  /* @365-377; SimpleStateProvider.getState@0-4 returns field, NO RNG */

/* -- PASS 1: environment check (all reads, no writes, may abort) ---- */
/* x outer 0..15, z middle 0..15, y inner 0..7 (@379-703) */
for (int x = 0; x < 16; x++)
  for (int z = 0; z < 16; z++)
    for (int y = 0; y < 8; y++) {
        /* border := !grid[self] && any set 6-neighbor (in-bounds checks): @409-602
           order of neighbor tests: x+1 (@429-455), x-1 (@458-482), z+1 (@485-511),
           z-1 (@514-538), y+1 (@541-567), y-1 (@570-594). Boundary cells are never
           set by the fill loop (fill is 1..14/1..6), so borders exist at edges. */
        bool border = !grid[(x*16+z)*8+y] &&
            ((x < 15 && grid[((x+1)*16+z)*8+y]) || (x > 0 && grid[((x-1)*16+z)*8+y]) ||
             (z < 15 && grid[(x*16+z+1)*8+y])   || (z > 0 && grid[(x*16+z-1)*8+y])   ||
             (y < 7  && grid[(x*16+z)*8+y+1])   || (y > 0 && grid[(x*16+z)*8+y-1]));
        if (!border) continue;                          /* @604-606 */
        BlockPos p = pos.offset(x, y, z);               /* @609-616 offset(outer, inner, middle) */
        BlockState st = level.getBlockState(p);         /* @621-629 (read once) */
        if (y >= 4 && st.liquid())                      /* @631-642 */
            return false;                                /* @645-646 — liquid touching air-part border */
        if (y < 4 && !st.isSolid() && st != fluid)      /* @647-665; REFERENCE equality vs fluid state */
            return false;                                /* @668-669 — wall at fluid level must be solid or same fluid */
        if (!config.canPlaceFeature.test(level, p))     /* @670-683 — tested on EVERY border cell (both y ranges) */
            return false;                                /* @686-687; lake_lava: always true → never aborts */
    }

/* -- PASS 2: carve (fluid below y=4, cave_air at y>=4) -------------- */
/* x outer 0..15, z middle 0..15, y inner 0..7 (@706-865) */
for (int x = 0; x < 16; x++)
  for (int z = 0; z < 16; z++)
    for (int y = 0; y < 8; y++) {
        if (!grid[(x*16+z)*8+y]) continue;              /* @736-753 */
        BlockPos p = pos.offset(x, y, z);               /* @756-766 */
        if (!config.canReplaceWithAirOrFluid.test(level, p))  /* @768-781 */
            continue;                                    /* @784 */
        bool above = (y >= 4);                          /* @787-798 */
        level.setBlock(p, above ? CAVE_AIR : fluid, 2); /* @800-822, flag 2 */
        if (above) {
            level.scheduleTick(p, Blocks.CAVE_AIR /*block*/, 0);  /* @828-842 delay 0 */
            markAboveForPostProcessing(level, p);       /* @843-847 */
        }
    }

/* -- PASS 3: barrier shell (stone), 50% roll above fluid line ------- */
BlockState barrier = config.barrier.getState(level, random, pos);  /* @868-877; simple provider, NO RNG */
if (!barrier.isAir()) {                                  /* @882-887; stone → loop runs */
    /* x outer 0..15, z middle 0..15, y inner 0..7 (@890-1229) */
    for (int x = 0; x < 16; x++)
      for (int z = 0; z < 16; z++)
        for (int y = 0; y < 8; y++) {
            bool border = /* identical structure to pass 1: @920-1113 */;
            if (!border) continue;                       /* @1115-1117 */
            /* RNG DRAW: for every border cell with y>=4, BEFORE any block read: */
            if (y >= 4 && random.nextInt(2) == 0)        /* @1120-1134: y<4 skips roll (if_icmplt 1137); roll==0 → skip cell */
                continue;
            BlockPos p = pos.offset(x, y, z);            /* @1137-1147 */
            BlockState st = level.getBlockState(p);      /* @1149-1157 */
            if (st.isSolid()                             /* @1159-1164 */
                && config.canReplaceWithBarrier.test(level, p)) {  /* @1167-1180 */
                BlockPos p2 = pos.offset(x, y, z);       /* @1183-1193 recomputed, same value */
                level.setBlock(p2, barrier, 2);          /* @1195-1206 */
                markAboveForPostProcessing(level, p2);   /* @1207-1211 — unconditional on placement, any y */
            }
        }
}

/* -- PASS 4: freeze surface — WATER LAKES ONLY --------------------- */
if (fluid.getFluidState().is(FluidTags.WATER)) {         /* @1232-1243; lava → false, whole pass skipped */
    for (int x = 0; x < 16; x++)                         /* outer @1246-1347 */
      for (int z = 0; z < 16; z++) {                     /* middle */
          /* local 12 = 4 stored @1266 but literal iconst_4 used in the call @1272 */
          BlockPos p = pos.offset(x, 4, z);              /* @1269-1278 */
          if (level.getBiome(p).value().shouldFreeze(level, p, false)  /* @1280-1300 */
              && config.canReplaceWithAirOrFluid.test(level, p)) {     /* @1306-1319 */
              level.setBlock(p, Blocks.ICE.defaultBlockState(), 2);    /* @1322-1332 */
          }
      }
}

return true;                                             /* @1350-1351 */
```

World extent: base = origin+(-8,-4,-8); writes cover origin.x-8..+7, origin.y-4..+3,
origin.z-8..+7 (plus post-processing marks up to origin.y+5). Grid y=4 plane == origin.y,
so the fluid surface (top fluid cell) is world y = origin.y-1.

## 2. RNG ledger (order-exact; confirms RNG = ctx.random() only)

Only RandomSource ops in the whole method (all on local slot 4 = ctx.random(), loaded @10-14;
there is NO `new` of any random class anywhere in place(), and SimpleStateProvider.getState
ignores its random arg — SimpleStateProvider.getState@0-4):

1. `nextInt(4)` @63 → blobCount = r+4.
2. Per blob, 6 × `nextDouble()` in order a, b, c, cx, cy, cz (@84, @101, @118, @135, @164, @195).
3. `nextInt(2)` @1129 — once per **barrier-pass border cell with y>=4**, in x→z→y visit
   order, drawn *before* getBlockState/isSolid/predicate checks. Roll 0 = skip, 1 = attempt.
4. Nothing else. Pass 1 (env check), pass 2 (carve), pass 4 (freeze) draw nothing.

Early exits: the y<=minY+4 guard returns before any draw; pass-1 aborts return AFTER
1 + 6×blobCount draws (harmless — feature RNG is re-seeded per placed-feature index, draws
don't leak to other features).

## 3. Block predicates — what/where

All three predicates are `test(level, blockPos)` with the exact loop position (offset field
= Vec3i.ZERO for our configs; StateTestingPredicate.test@0-18 does
`test(level.getBlockState(pos.offset(this.offset)))`; MatchingBlockTagPredicate.test@0-8 is
`state.is(tag)`; NotPredicate.test@0-19 inverts).

| predicate | tested at | semantics for lake_lava |
|---|---|---|
| `canPlaceFeature` (`true`) | pass 1, every border cell, after the liquid/solid aborts (@670-683) | constant true → C impl may omit, keep the two hard aborts |
| `canReplaceWithAirOrFluid` (`not #features_cannot_replace`) | pass 2, every set grid cell before setBlock (@768-781); also pass 4 freeze (@1306-1319, unreachable for lava) | replace unless block ∈ tag |
| `canReplaceWithBarrier` (`not #lava_pool_stone_cannot_replace`) | pass 3, after isSolid() passes (@1167-1180) | stone shell unless block ∈ tag |

Tag contents (reference/tags/block/*.json):
- `features_cannot_replace` = {bedrock, spawner, chest, end_portal_frame, reinforced_deepslate, trial_spawner, vault}.
- `lava_pool_stone_cannot_replace` = #features_cannot_replace ∪ #leaves ∪ #logs.
  - `#leaves` = {jungle,oak,spruce,pale_oak,dark_oak,acacia,birch,azalea,flowering_azalea,mangrove,cherry}_leaves.
  - `#logs` = #logs_that_burn ∪ #crimson_stems ∪ #warped_stems; logs_that_burn = {dark_oak,pale_oak,oak,acacia,birch,jungle,spruce,mangrove,cherry}_logs (each *_logs family = log/wood/stripped_log/stripped_wood).
  Ordering note: lakes run at GenerationStep.Decoration LAKES (index 1), before
  UNDERGROUND_STRUCTURES and VEGETAL_DECORATION — at lake time the world holds only
  noise/surface/carver output, so in vanilla overworld gen the only tag member that can
  actually be encountered is bedrock (via #features_cannot_replace). Implement the full
  tag checks anyway (cheap, and datapack-proof).

## 4. Block property semantics → hyperchunk mapping

- `BlockState.liquid()` @639: BlockStateBase cached `liquid` flag — true only for the two
  LiquidBlocks (water/lava, any `level` value); waterlogged blocks are NOT liquid().
  → `hc_block_is_fluid()` (core/src/blocks.c:624, id==WATER||id==LAVA) matches: worldgen
  palette only ever holds level=0 sources. Waterlogged seagrass etc. at a y>=4 border does
  NOT abort.
- `BlockState.isSolid()` @655/@1161: legacySolid → `hc_block_is_solid()` (core/src/blocks.c:632, F_SOLID).
- `st != fluid` @663-665 is Java reference equality on interned BlockStates → C: palette-id
  equality against `lava[level=0]` (the exact configured state).
- `barrier.isAir()` @884: stone → false; only datapack-air barriers skip pass 3.
- `setBlock(..., 2)` flag 2 everywhere (@817, @1201, @1331) — standard worldgen write, updates
  WG heightmaps in our set-block path as usual.

## 5. Helper methods

`Feature.markAboveForPostProcessing(level, pos)` (Feature.markAboveForPostProcessing@0-53):
```
m = pos.mutable();
loop 2 times: m.move(UP);
  if (level.getBlockState(m).isAir()) return;      /* air (incl. cave_air/void_air) stops */
  level.getChunk(m).markPosForPostProcessing(m);
```
Reads block states only, no RNG, writes chunk `PostProcessing` NBT — outside the 07
blocks/heightmaps gate (same class of no-op for us as scheduleTick, cf. core/src/features.c:539).
`scheduleTick(p, CAVE_AIR, 0)` @828-842 likewise only lands in `block_ticks` NBT — no-op for
the gate, but keep a comment marker in C.

## 6. Iteration orders (summary)

| loop | x | z | y | index/offset |
|---|---|---|---|---|
| grid fill | outer 1..14 | mid 1..14 | inner 1..6 | `grid[(x*16+z)*8+y]` @325-338 |
| env check (pass 1) | outer 0..15 | mid 0..15 | inner 0..7 | pos = base.offset(x,y,z) @609-616 |
| carve (pass 2) | outer 0..15 | mid 0..15 | inner 0..7 | @756-763 |
| barrier (pass 3) | outer 0..15 | mid 0..15 | inner 0..7 | @1137-1144 |
| freeze (pass 4) | outer 0..15 | mid 0..15 | y=4 fixed | @1269-1275 |

Neighbor-check order inside border detection (both passes): +x, −x, +z, −z, +y, −y with
short-circuit on first set neighbor (@429-594 / @940-1105).

## 7. FP notes (bit-exactness)

- Pure double +,−,×,÷; no transcendentals, no float. Safe with `-ffp-contract=off`
  (no FMA-able a*b+c may be contracted: `dx*dx + dy*dy + dz*dz` must be two separate adds
  in the bytecode order `(dx*dx + dy*dy) + dz*dz` @297-314).
- Center expressions: strict left-to-right — `r*((16.0 - a) - 2.0)` then `+ 1.0` then
  `+ (a/2.0)` (@133-160); y variant `r*((8.0 - b) - 4.0) + 2.0 + b/2.0` (@162-191).
- `a/2.0`, `b/2.0`, `c/2.0` recomputed inside the innermost loop (@258-264 etc.) — division
  by 2.0 is exact, hoisting is safe, but keep the division (not `*0.5`; also exact, but keep
  literal fidelity).
- `(double)x` etc. via i2d @254/@269/@284 — exact.
- Ellipsoid test `s < 1.0` strict, compiled dcmpg+ifge @318-320 (NaN → not set; NaN cannot
  occur: a∈[3,9) → a/2 ≥ 1.5 > 0).
- nextDouble/nextInt are the standard per-feature WorldgenRandom (Xoroshiro) draws — same
  helpers as all other features.

## 8. Placement pipelines (context only; feature body identical)

- `lake_lava_underground` (reference/placed_feature/lake_lava_underground.json):
  rarity_filter chance=9 → in_square → height_range uniform(absolute 0 .. below_top 0) →
  environment_scan down, max_steps=32, target = allOf(not(#minecraft:air tag match),
  inside_world_bounds offset (0,−5,0)) → surface_relative_threshold_filter
  heightmap=OCEAN_FLOOR_WG max_inclusive=−5 → biome.
- `lake_lava_surface` (reference/placed_feature/lake_lava_surface.json): rarity_filter
  chance=200 → in_square → heightmap WORLD_SURFACE_WG → biome. That is the ONLY difference —
  same configured feature `minecraft:lake_lava`.

## 9. Gotchas checklist for the C port

1. Guard is `<=` minY+4 (abort at y = −60 for overworld), BEFORE any RNG draw.
2. Base offset is (−8,−4,−8) — all three axes, not just y.
3. Grid index `(x*16+z)*8+y`; fill bounds exclusive 1..14 / 1..6 keep the outer shell empty.
4. Pass 1 runs to completion (or aborts) before ANY write; abort leaves the world untouched
   but the RNG advanced.
5. Pass-1 y<4 rule: border block must be `isSolid()` OR be exactly the configured fluid
   state (id equality). Pass-1 y>=4 rule: border block must not be `liquid()`.
6. Pass-3 `nextInt(2)` is drawn for every y>=4 border cell even when the subsequent
   isSolid/predicate checks fail — draw order is part of the stream.
7. cave_air (not air) for the carved top half; ICE pass unreachable for lava.
8. scheduleTick/markPosForPostProcessing: no block bytes — comment-mark only.
