# R6 — Lane E1: Golden dump forensics for Task 10 (08_initialize_light + 09_light)

Scope: pure shell/python over `golden/stages/seed1234567890` (PRIM) and
`golden/stages-alt/seed1234567890` (ALT). No Java. All coordinates WORLD coords
unless "local". Dump geometry: light rows = y ∈ [−64..319] asc, then z 0..15,
16 hex nibbles x-asc (6144 rows). Analysis scripts kept at `/tmp/lightscan/*.py`
(common.py / diff09.py / inflow3.py / stageevo2.py / anchors.py).

## 0. Anchors re-verified

- 08 `light_block` all zeros for all 9 chunks, both bundles (spot- and full-checked).
- 08 `light_sky` rows are ONLY `0…0` or `f…f`; the f-region starts at **y=112**
  uniformly for all 9 chunks in both bundles, zero mixed rows. y=112 = bottom of
  section index 7 (sections y∈[112..127]); max WORLD_SURFACE over the grid is 92
  (top non-air section 5, y∈[80..95]). So the 15-region starts exactly TWO
  sections above the grid-max top-filled section — i.e. one all-zero data layer
  exists for section 6 (y 96..111) and everything above data layers reads 15.
  Uniformity across chunks whose own top section is 4 suggests the storage
  keeps data layers up to (grid/neighborhood max top section + 1), not per-chunk
  top+1 — bytecode lane should confirm against
  `LayerLightSectionStorage`/`SkyLightSectionStorage` (topSectionY handling).
- 09 differs cross-bundle: sky in 9/9 chunks, block in 7/9 (c.0.0 and c.−1.1
  have 0 block-light diffs).

## 1. Prefix chunk sets and 09 dump order

Manifest = 81 entries in both bundles = full 9×9 (Chebyshev r≤4) runs features.
By radius: r0:1, r1:8, r2:16, r3:24, r4:32.

**PRIM prefix seq<49** (all non-center 09/10/11 dumps): 49 chunks =
r≤1 complete (9) + **r=2 complete (16/16)** + r=3 partial 11/24 + r=4 partial 13/32.
Missing r=3: entire x=3 column and z=3 row (SE side): (−3..2, 3) and (3, −3..3).
Missing r=4: (±4 SE side): (−4..2, 4), (3..4, −4..4) etc. — the prefix is strongly
NW-biased (Worker-Main-1 walked −x/−z first). seq 45,46,48 are (0,2),(1,2),(2,2);
seq 49 (first not seen) = c.−2.3.

**ALT prefix seq<50** (c.1.1's window, the max): 50 chunks =
r≤1 (9) + r=2 complete (16/16) + r=3 21/24 (missing (2,3),(3,2),(3,3)) + r=4 only
(−4,−2..1). ALT interleaves radii differently (4 workers).

**Invariant worth keeping: at every 09 dump with seqBegin≥49 (PRIM non-center)
or ≥50 (ALT c.1.1) the entire r=2 ring's features have started; at the early
dumps (seq 9/14/21/24/31/33) they have not.**

09 dump ORDER of the 9 grid chunks (from `order.snapshots`, nanos ascending):

| # | PRIM chunk | seqB/E | nanos (10800…) | ALT chunk | seqB/E | nanos (10800…) | ALT thread |
|---|-----------|--------|----------------|-----------|--------|----------------|------|
| 1 | 0.0   | 9/9   | 45838202953 | 0.0   | 9/9    | 82498249798 | WM-2 |
| 2 | −1.−1 | 49/49 | 49286571620 | −1.−1 | 14/14  | 85093158061 | WM-4 |
| 3 | −1.0  | 49/49 | 49334162015 | −1.0  | 21/22 T| 85296714331 | WM-3 |
| 4 | 0.−1  | 49/49 | 49417654103 | 0.−1  | 24/24  | 85349186080 | WM-3 |
| 5 | 1.−1  | 49/49 | 49427773602 | 0.1   | 31/32 T| 85699579937 | WM-2 |
| 6 | 1.0   | 49/49 | 49434921757 | −1.1  | 33/33  | 85739724076 | WM-2 |
| 7 | −1.1  | 49/49 | 49442279159 | 1.−1  | 48/49 T| 86098298344 | WM-4 |
| 8 | 0.1   | 49/49 | 49450869125 | 1.0   | 49/50 T| 86126642319 | WM-1 |
| 9 | 1.1   | 49/49 | 49458895330 | 1.1   | 50/50  | 86153072632 | WM-3 |

Dumped-strictly-before sets follow directly (prefix of the column). Note PRIM
order ≠ ALT order in the middle: PRIM …0.−1, **1.−1, 1.0, −1.1, 0.1**, 1.1;
ALT …0.−1, **0.1, −1.1, 1.−1, 1.0**, 1.1.

ALT torn 09 windows and the single in-flight manifest entry each:
c.−1.0 [21,22): (1,−3); c.0.1 [31,32): (−2,2); c.1.−1 [48,49): (−4,1);
c.1.0 [49,50): (2,2). ALT per-chunk prefix radius profiles (entries<seqBegin):
c.−1.−1 n=14 {r2:5 = (−2,−2),(−2,−1),(−2,0),(−1,−2),(0,−2)}; c.−1.0 n=21
{r2:6,r3:6}; c.0.−1 n=24 {r2:7,r3:8}; c.0.1 n=31 {r2:10,r3:12}; c.−1.1 n=33
{r2:11,r3:13}; c.1.−1 n=48 {r2:15,r3:21,r4:3}; c.1.0 n=49; c.1.1 n=50 (r2 full).

Caveat on window semantics: seq counts `setDecorationSeed@RETURN` = feature-stage
**starts**, not completions. A dump at seqBegin==seqEnd can still see partially
applied in-flight neighbor features in ALT (4 threads). Empirically this shows
up at c.0.0: both bundles dump 09 at 9/9, yet c.0.0's 09 blocks differ by 116
nibbles cross-bundle (vegetation, y 63..80) — inherited from the features-stage
races already visible at 07 (07 cross-bundle block diffs: c.0.0=99; per chunk:
−1.−1:0, −1.0:2191, −1.1:723, 0.−1:5, 0.0:99, 0.1:522, 1.−1:1, 1.0:14, 1.1:711).

## 2. 09 light cross-bundle diff localization

Cross-bundle 09 **blocks** diffs (same dump files, names compared, grid only):
3833 total = {−1.−1:6, −1.0:1573, −1.1:599, 0.−1:20, 0.0:116, 0.1:602, 1.−1:96,
1.0:51, 1.1:770}.

### 2a. SKY (9/9 chunks differ; 5720 nibbles total)

| chunk | #diffs | y-range | 16-bin y histogram | border share (x/z==0/15) |
|-------|-------:|---------|--------------------|--------------------------|
| −1.−1 | 144 | 67..79 | {64:144} | W35 N52 int66 |
| −1.0 | 1966 | 64..79 | {64:1966} | mostly interior (1599) |
| −1.1 | 973 | 63..78 | {48:12, 64:961} | int679 |
| 0.−1 | 198 | 63..86 | {48:25, 64:147, 80:26} | E135 (faces 1.−1) |
| 0.0 | 149 | 64..72 | {64:149} | W53 int92 |
| 0.1 | 825 | 63..80 | {48:46, 64:770, 80:9} | int665 |
| 1.−1 | 198 | 55..73 | {48:61, 64:137} | N92 int91 |
| 1.0 | 123 | 71..82 | {64:104, 80:19} | S43 int62 |
| 1.1 | 1144 | 69..83 | {64:1045, 80:99} | int878 |

- ALL sky diffs live in the surface band y 55..86 (vegetation/canopy heights).
  No sky diffs in caves, none above y=86.
- Not border-concentrated: distance-to-nearest-edge histograms are broad
  (interior dominates for the big chunks).
- **Co-location: 5720/5720 (100%) of sky diffs have a cross-bundle 09 BLOCK
  diff within Chebyshev ≤15 in the same bundle pair; 3697 at d≤5, 0 at d>15.**
- Magnitudes: |Δ| hist {1:2743, 2:1662, 3:699, 4:264, 5:88, 6:5, 9..11:4,
  12:18, 13:56, 14:67, 15:114}; sign prim>alt 3220 vs alt>prim 2500. The
  |Δ|≥12 tail = full occlusion flips (leaf/trunk placed vs absent directly in
  a column: 15→0..3).

**Conclusion (sky): cross-bundle 09 sky diffs are fully explainable by
block-state diffs (features races + torn windows). No evidence of different
sky-enabled (S-set) chunk sets is REQUIRED for sky at the observed positions —
but see §4: inflow differences exist and are themselves co-located with the
block-diff explanation only because ring light also moved between windows.
Strictly: 100% co-location means block diffs suffice as a hypothesis; §4 shows
S-set timing additionally differs and is visible at borders.**

### 2b. BLOCK (7/9 differ; 1047 nibbles; c.0.0 and c.−1.1 identical)

| chunk | #diffs | y-range | 16-bin y hist | border share | ≤15 of a grid block diff |
|-------|-------:|---------|---------------|--------------|--------------------------|
| −1.−1 | 11 | 14..16 | {0:5,16:6} | N7 | **0/11** |
| −1.0 | 19 | −12..−10 | {−16:19} | W6 | **0/19** |
| 0.−1 | 192 | −8..20 | {−16:170,16:22} | N52 E39 | **0/192** |
| 0.1 | 368 | −12..9 | {−16:268,0:100} | E63 S61 W46 | 90/368 (all at d 6-15) |
| 1.−1 | 119 | −8..3 | {−16:78,0:41} | S36 E42 | **0/119** |
| 1.0 | 304 | −20..20 | {−32:26,−16:100,0:159,16:19} | S96 | **0/304** |
| 1.1 | 34 | −13..−5 | {−16:34} | W23 S18 | **0/34** |

- Opposite of sky: block-light diffs live at CAVE y-levels (−32..20), cluster
  hard at borders (dist-to-edge 0-1 dominates), and are **essentially never**
  co-located with a visible (grid-dumped) block diff.
- Magnitudes: |Δ| hist {1:202, 2:211, 3:171, 4:104, 5:114, 6:52, 7:68, 8:33,
  9:39, 10:23, 11:20, 12:8, 13:2}; sign prim>alt 742 vs 305 (PRIM dumps are
  later in the feature/light progression, hence brighter).
- Worked example (c.−1.−1, N border facing ring chunk c.−1.−2): PRIM has a
  block-light plume peaking at world (−8,16,−16)=3, decaying inward
  (y=16 z=−16: `…0 3 2 1 0…`, z=−15: `…0 2 1 0…`, z=−14: `…0 1 0…`); ALT has
  all-zero there. The block at (−8,16,−16) is water in BOTH bundles (no block
  diff); the gradient points beyond z=−16 → the emitter sits in undumped ring
  chunk c.−1.−2 (border value 3 ⇒ e.g. glow_lichen(7) 4 blocks beyond, or
  magma(3) adjacent). ALT's dump (seq 14) predates that emitter's light; PRIM's
  (seq 49) includes it.

**Conclusion (block): cross-bundle block-light diffs are NOT explainable by
grid block diffs. They are inflow differences from emitters outside the dumped
volume (r=2 ring chunks) and/or from grid-neighbor emitters whose light had
not yet propagated at one bundle's dump time. They directly encode the "which
chunks' light was already in the engine" (S-set/timing) difference.**

## 3. Within-bundle evolution 08 → 09 (PRIM unless noted)

08→09 total sky nibble changes per chunk: 10.5k–12.5k (e.g. c.0.0: 12159).
09 min-y with nonzero sky per chunk: −1.−1:64, −1.0:64, −1.1:63, 0.−1:63,
0.0:63, 0.1:63, 1.−1:57, 1.0:57, 1.1:69. **No sky light below y=57 anywhere;
below y=60 only c.1.−1 (23 nibbles) and c.1.0 (86) via a water-filled surface
dip at y=57-59.** So 09 sky = full-15 column down to occlusion + a thin
partial penumbra; caves at this seed/grid receive no skylight.

Unit anchors (verified bundle-STABLE: same value AND same block both bundles):

sky:
- (7,63,0) c.0.0: sky=7, block `vine[west=true]` (under jungle canopy overhang).
- (27,57,1) c.1.0: sky=5, air (in the y=57 dip, deepest sky at this grid).
- (−2,64,−16) c.−1.−1: sky=14, `oak_leaves[distance=2]` (canopy attenuation).

block (emitter peaks; light value at emitter pos = emission):
- (−3,44,−1) c.−1.−1: block=7, `glow_lichen[south=true]` (emission 7).
- (−3,−15,15) c.−1.0: block=14, `cave_vines[age=24,berries=true]` (emission 14).
- (0,−13,11) c.0.0: block=14, `cave_vines_plant[berries=true]`.
- (15,−30,0) c.0.0: block=3, `magma_block` (emission 3).
- (27,−58,−10) c.1.−1: block=15, `lava[level=0]` (a large lava lake floor
  y≈−58..−55 spanning c.1.−1; 187 local maxima in that chunk).

Emitter census sanity: the interior "unexplainable-high" block-light cells
(fixed-point residues, §4 method) are exactly the emitters: cave_vines(14),
glow_lichen(7), magma(3), lava(15) — counts per chunk 0..54 (54 = lava lake
c.1.−1), and every one cross-references to an emitting block in the 09 palette.

## 4. Border-inflow probe (S-set discriminator)

Method: flag cell v>0 (y<300) where v > max over the 5-6 IN-CHUNK neighbors of
(neighbor−1, or 15 if straight-above==15). Assuming opacity≥1 this is
conservative: every flagged cell PROVES an out-of-chunk source (for sky there
are no in-grid emitters; for block light I exclude cells that are themselves
emitters). Top-of-world rows excluded (y=319 artifact). Crucially, **interior
flags = 0 for sky in all 18 chunk-dumps** → both bundles' sky fields are at a
local fixed point; every anomaly is exactly at a border = genuine inflow.

Sky inflow counts per side (`W/E/N/S`, `*`=outer/ring-facing side):

| chunk | PRIM | ALT |
|-------|------|-----|
| −1.−1 | **11\*/0/27\*/0** | **0\*/0/0\*/0** |
| −1.0 | 2\*/2/0/0 | 0\*/59/0/0 |
| −1.1 | 0\*/66/0/11\* | 0\*/68/1/10\* |
| 0.−1 | 2/72/0\*/3 | 2/0/0\*/0 |
| 0.0 | **0/0/0/0** | **0/0/0/0** |
| 0.1 | 0/0/0/0\* | 0/0/0/5\* |
| 1.−1 | 0/1\*/0\*/0 | 0/1\*/0\*/0 |
| 1.0 | 11/0\*/37/1 | 10/3\*/37/0 |
| 1.1 | 13/0\*/26/0\* | 0/0\*/25/0\* |

Readings:
- **c.0.0 (dumped first, seq 9, both bundles): ZERO inflow on all four sides.**
  At its dump, no neighbor's propagateLightSources output had reached it — its
  sky field is self-contained. This is the clean "no ring, no grid neighbors
  yet" reference state.
- **c.−1.−1 is the sharpest discriminator**: PRIM (dump at seq 49) shows OUTER
  inflow W(from ring c.−2.−1)=11, N(from ring c.−1.−2)=27, while its INNER
  sides (E→c.0.−1, S→c.−1.0, grid chunks whose light stages ran later on the
  same worker) show 0. ALT (dump at seq 14) shows 0 on ALL sides. So between
  ALT-seq14 and PRIM-seq49 dump times, RING chunks' sky sources entered the
  engine — i.e. r=2 chunks' propagateLightSources ran before PRIM's grid 09
  dumps but after ALT's early ones. (Ring chunks must already have
  INITIALIZE_LIGHT before any grid LIGHT — storage alone gives no 15s; seeded
  sources do.)
- Worked example: PRIM c.−1.−1 (−14,74,−16) sky=14 flagged: own column above is
  ≤14 (leaves at y=78..80 cut 15→14 at y≤80; jungle_leaves at 72/73), in-chunk
  laterals 13/14/13, below 14 — the only possible v=15 donor is the ring column
  at (−14,74,−17) in c.−1.−2, i.e. a fully skylit ring column. Sustained 14 down
  y=74..77 = continuous feed across the border. ALT value at the same cell: 9
  (ring dark, light arrives around the canopy instead).
- Later-dumped chunks show INNER-side inflow from earlier-lit grid neighbors:
  c.−1.0 E-side (facing c.0.0): PRIM 2, ALT 59; c.−1.1 E (facing c.0.1): 66/68;
  c.1.0 W (facing c.0.0): 11/10 and N (facing c.1.−1): 37/37; c.1.1 W/N: 13/26
  (PRIM), 0/25 (ALT). So yes — **c.0.0's borders show strictly less (zero)
  inflow than every later-dumped chunk's interior-adjacent borders.**
- Block-light inflow examples at outer borders (non-emitter cells, no nearby
  block diff): c.−1.−1 N\* (−8,16,−16)=3 (PRIM only, §2b); c.1.0 E\*
  (31,−32,0)=1 (both bundles; emitter in c.2.0); c.0.−1 N (11,−5,−16)=12 PRIM
  (strong emitter just beyond z=−17 in c.0.−2).

**Implication for the C port:** a dump-matching light stage cannot be computed
chunk-locally; it must reproduce the vanilla schedule: (a) per-chunk
propagateLightSources in the observed stage order, (b) live checkBlock updates
from later feature placements into already-lit chunks, and (c) ring-chunk
(r=2) light runs before late grid dumps. The PRIM bundle's fixed order
(c.0.0 at seq 9; all others after ALL 49 prefix features, in the §1 order) is
the tractable gate target.

## 5. Heightmaps in 08–11 dumps

Kinds present (`heightmap <key>` header lines), per chunk:

| stage | c.0.0 | other 8 chunks |
|-------|-------|----------------|
| 07, 08 | 6 kinds: WORLD_SURFACE_WG, WORLD_SURFACE, OCEAN_FLOOR_WG, OCEAN_FLOOR, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES | same 6 |
| 09, 10 | same 6 | **5: WORLD_SURFACE_WG absent** |
| 11 | 4 (both _WG gone) | 4 |

Identical pattern in BOTH bundles. c.0.0 is only "special" because its 09/10
dumps happen at seq 9; the other chunks' 09 dumps (ALT seq≥14) already show
WORLD_SURFACE_WG dropped ⇒ the drop event happens per-chunk somewhere in the
(chunk features .. ring-neighbor features) window, NOT in the light stage
itself (c.0.0 ran light+spawn and kept 6). At 11_full the surviving _WG kinds
are pruned (LevelChunk keeps the 4 client types).

08 vs 09 value diffs within a bundle (differing columns of 256):
- c.0.0: none (both bundles) — 08 and 09 dumps bracket no feature activity.
- All other chunks: OCEAN_FLOOR_WG rewritten wholesale (202..238 of 256
  columns!), plus small diffs in WORLD_SURFACE/OCEAN_FLOOR/MOTION_BLOCKING
  (1..36 columns; ALT c.−1.1 also MOTION_BLOCKING_NO_LEAVES:3) from late
  feature placements between the two dumps.
- The rewritten OCEAN_FLOOR_WG(09) equals OCEAN_FLOOR(09) in 253..256/256
  columns (vs only ~30/256 equal to its own 08 values) — i.e. **OCEAN_FLOOR_WG
  is re-primed post-features with (near-)OCEAN_FLOOR semantics at the same
  event that deletes WORLD_SURFACE_WG**. Residual mismatches are 1-3 columns
  where predicates differ (e.g. c.1.1 local(15,2): OF_WG=72 vs OF=MB=75).
- **Open question for the bytecode lane (task #2):** find the 26.2 code that,
  after features, removes WORLD_SURFACE_WG and re-primes OCEAN_FLOOR_WG
  (grep ChunkStatusTasks/ProtoChunk/Heightmap for a `heightmaps.remove` /
  re-prime; also confirm the §0 y=112 storage-top rule). Not needed for light
  parity math, but needed if the 09 heightmap files are gated.

## 6. Task-11 pre-scan: 09 vs 10 vs 11

Within each bundle, per chunk (all 9, both bundles):
- **09→10 (spawn): blocks=0, heightmap values=0, kinds unchanged, biomes=0.**
  10_spawn is a pure no-op on dumped state.
- **10→11 (full): blocks=0, heightmap values=0, biomes=0; ONLY change =
  heightmap kinds pruned to the 4 non-WG (c.0.0 drops both _WG, others drop
  their remaining OCEAN_FLOOR_WG).**
- 10/11 light files DO NOT exist (only 08/09 have light_block/light_sky dumps).
- order.snapshots 10/11 rows: PRIM none torn (all 9/9 or 49/49). ALT: only
  `11_full −1 −1 17 18` is torn (in-flight entry seq 17 = c.−3.−3); all ALT
  10_spawn rows untorn. ALT 10/11 seq per chunk trails its 09 seq by 0..3
  (e.g. c.−1.−1: 09@14, 10@17, 11@17/18 — and still zero block diffs across
  those windows, so seq 14..18 ring features wrote nothing into c.−1.−1).
- 11_full dumps run on **Server_thread** (08/09/10 on workers) in both bundles.

**Empirical summary for Task 11: stages 10 and 11 change nothing but heightmap
kind pruning at 11. The Task-11 implementation is bookkeeping (status,
heightmap set, proto→full conversion), no worldgen math.**
