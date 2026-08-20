# R2 — Tree machinery for trees_jungle (MC 26.2, bytecode)

Disassembled inline in the 9b session (`javap -p -c` vs tools/golden/work/server;
the parallel recon agent for this slice died to API rate limits — this note is
the main-session reconstruction, same rigor). RNG shorthand per task9a A2.
Everything below is [VERIFIED-bytecode] unless noted; configs [VERIFIED-data]
quoted from the datapack in-session.

## 0. Configs (all `minecraft:tree` unless noted)

| cf | trunk placer | foliage placer | decorators | ignore_vines | minimum_size |
|---|---|---|---|---|---|
| jungle_tree | straight(4, 8, 0) | blob(h3, o0, r2) jungle_leaves | cocoa(0.2), trunk_vine, leave_vine(0.25) | true | two_layers (defaults) |
| jungle_bush | straight(1, 0, 0) | bush(h2, o1, r2) oak_leaves | — | false | two_layers(limit 0, upper 0) |
| mega_jungle_tree | mega_jungle(10, 2, 19) | jungle=MegaJungleFoliagePlacer(h2, o0, r2) | trunk_vine, leave_vine(0.25) | false | two_layers(lower 1, upper 2) |
| fancy_oak | fancy(3, 11, 0) | fancy(h4, o4, r2) oak_leaves | — | true | two_layers(limit 0, min_clipped 4, upper 0) |
| fallen_jungle_tree (`fallen_tree`) | — | — | stump: [trunk_vine]; log: [attached_to_logs(up, 0.1, weighted{red_mushroom 2, brown_mushroom 1})] | — | log_length uniform[4,11] |

All 4 tree cfs: below_trunk_provider = rule_based{rules:[if not(#cannot_replace_below_tree_trunk) → dirt]}
(no fallback → getOptionalState returns null when the rule fails → NO write);
trunk/foliage providers simple (0 draws). trees_jungle = random_selector
{0.1 fancy_oak_checked, 0.5 jungle_bush, 0.33333334 mega_jungle_tree_checked,
0.0125 fallen_jungle_tree; default jungle_tree}; *_checked placement =
[block_predicate_filter would_survive(oak_sapling[stage=0] / jungle_sapling[stage=0])];
jungle_bush/jungle_tree/fallen_jungle_tree placed features have placement [].

## 1. Selector features

- RandomSelectorFeature.place: for entry in list order: `nextFloat() < chance`
  (strict) → return entry.placedFeature.place(...) (nested `place()` path — no
  biome allowed). All miss → default.place. Draw per tested entry, early exit.
- RandomBooleanSelectorFeature: `nextBoolean()` (= next(1)!=0); true→featureTrue.
- SimpleRandomSelectorFeature: `nextInt(size)` pick.

## 2. would_survive(sapling)

SaplingBlock does NOT override VegetationBlock.canSurvive/mayPlaceOn ⇒
canSurvive = `below.is(#supports_vegetation)` — identical to grass/flowers.

## 3. TreeFeature.place

4 × `Sets.newHashSet()`: roots(set), logs(set2), leaves(set3), deco(set4).
Setters: root/trunk BiConsumers add pos.immutable() to their set + setBlock
flag **19**; FoliageSetter (TreeFeature$1) same into leaves + `isSet(pos)` =
set.contains. doPlace:

1. height = trunkPlacer.getTreeHeight(random) = base + nextInt(randA+1) +
   nextInt(randB+1) — 2 draws ALWAYS (nextInt(1) burns).
2. fh = foliagePlacer.foliageHeight(random, height, cfg) — blob/bush/fancy/
   megaJungle: returns height field, 0 draws.
3. rad = foliagePlacer.foliageRadius(random, height−fh) = radius.sample —
   ConstantInt in all 5 configs ⇒ 0 draws.
4. rootPos = origin (no root placer). Bounds: `min(y,y) >= minY+1 &&
   max(y,y)+height+1 <= maxY+1` else false (no draws yet — the 2+0 draws of
   step 1 already burned).
5. maxFree = getMaxFreeTreeHeight: for y in 0..height (INCLUSIVE), size =
   minimumSize.getSizeAtHeight(height, y) (two_layers: y < limit? — see §9),
   for dx,dz in [-size,size]²: pos=root+(dx,y,dz); fail iff !isFree ||
   (!ignoreVines && isVine); on fail return y−2. isFree = validTreePos ||
   is(#logs); validTreePos = isAir || is(#replaceable_by_trees); isVine =
   is(vine block).
6. if (maxFree < height && (minClipped empty || maxFree < minClipped)) → false.
7. attachments = trunkPlacer.placeTrunk(level, trunkSetter, random, maxFree,
   rootPos, cfg).
8. attachments.forEach(att → foliagePlacer.createFoliage(level, foliageSetter,
   random, cfg, maxFree, att, fh, rad)) — public entry samples
   offset.sample(random) per attachment (ConstantInt ⇒ 0 draws) then protected
   createFoliage.

place() then: if !doPlace || (logs.isEmpty() && leaves.isEmpty()) → false.
If decorators nonempty: Context(level, decoSetter(set4+flag19), random,
logs, leaves, roots) — Context copies each Set into fastutil ObjectArrayList
(HashSet ITERATION ORDER!) then **stable-sorts by getY** (fastutil sort() =
stable mergesort; same-Y ties keep HashSet order). decorators run in list
order. Then box = encapsulating(roots ∪ logs ∪ leaves ∪ deco);
updateLeaves(level, box, logs, deco, roots); StructureTemplate.updateShapeAtEdge
(level, 3, shape, box.min) [world-read-only + block updates via
markPosForPostProcessing? — no: flag-3 neighbor-shape updates; see §8];
return true (orElse false when no positions at all).

## 4. updateLeaves(level, box, logs, deco, roots) — leaf distance recompute

```java
shape = BitSetDiscreteVoxelShape(box spans);
lists = [7 × newHashSet()];
for (pos : newArrayList(Sets.union(deco, roots))) if (box.isInside) shape.fill(rel);
lists.get(0).addAll(logs);
int i = 0;
while (true) {
    while (i < 7 && lists.get(i).isEmpty()) i++;
    if (i >= 7) return shape;
    Iterator it = lists.get(i).iterator(); pos = it.next(); it.remove();   // FIRST elem in HashSet order
    if (!box.isInside(pos)) continue;
    if (i != 0) setBlockKnownShape(level, pos, getBlockState(pos).setValue(DISTANCE, i)); // flag 19
    shape.fill(rel(pos));
    for (Direction d : values()) {                       // DOWN,UP,N,S,W,E
        m = pos+d; if (!box.isInside(m)) continue;
        if (shape.isFull(rel(m))) continue;
        opt = LeavesBlock.getOptionalDistanceAt(state(m));
        //   = is(#prevents_nearby_leaf_decay) ? 0 : hasProperty(DISTANCE) ? value : empty
        if (opt.isEmpty()) continue;
        k = min(opt.getAsInt(), i + 1);
        if (k < 7) { lists.get(k).add(m.immutable()); i = min(i, k); }
    }
}
```

Quirks the C port MUST keep: a pos can sit in several distance sets and be
REWRITTEN at a higher distance after being processed at a lower one (no
isFull guard on the polled pos); the poll order is HashSet-iteration first
element; `i` drops back via `min(i,k)`. Freshly placed leaves have
DISTANCE=7 so first-tree BFS is clean; neighbor trees' already-rewritten
leaves (distance <7) re-enter via `min(opt, i+1)`.

## 5. Trunk placers

- placeLog(pos[, fn]): validTreePos → replacer.accept(pos, fn(trunkProvider
  .getState)) (0 draws), true; else false. placeLogIfFree = isFree → placeLog.
- placeBelowTrunkBlock(level, replacer, random, pos.below(), cfg): rule-based
  provider (§0); non-null → replacer.accept — **the dirt below the trunk joins
  the LOG set** (decorators + BFS seeds see it).
- Straight: placeBelowTrunkBlock(below); for i in 0..free−1 placeLog(above(i));
  return [Attachment(pos.above(free), 0, false)].
- Giant (base of MegaJungle): placeBelowTrunkBlock at below, below.east,
  below.south, below.south.east (order); for i in 0..free−1:
  logIfFree(+0,i,+0); if i<free−1: (+1,i,+0), (+1,i,+1), (+0,i,+1);
  attachment (above(free), 0, doubleTrunk=TRUE).
- MegaJungle: giant.placeTrunk first; then
  `y = free − 2 − nextInt(4)`; while (y > free/2 [int div]):
  `f = nextFloat()*(float)π*2` — bytecode: (double)(nextFloat()*2.0f)*π;
  for l in 0..4: dx=(int)(1.5f + Mth.cos((double)f)*(float)l),
  dz=(int)(1.5f + Mth.sin((double)f)*(float)l)   [Mth TABLE sin/cos, f2i
  truncation]; placeLog(pos.offset(dx, y−3+l/2, dz)); after loop add
  Attachment(pos.offset(dx_last, y, dz_last), −2, false); y −= 2 + nextInt(4).
- Fancy: see §7.

## 6. Foliage placers (protected createFoliage(level,setter,random,cfg,
   maxFree,att,fh,rad,offset))

placeLeavesRow(pos, range, yOff, large): bound = range + (large?1:0); for dx
in [−range, bound], dz in [−range, bound] (dx outer): if
shouldSkipLocationSigned continue else tryPlaceLeaf(pos+(dx,yOff,dz)).
shouldSkipLocationSigned: large ? lx=min(|dx|,|dx−1|), lz=min(|dz|,|dz−1|)
: |dx|,|dz| → shouldSkipLocation(random, lx, yOff, lz, range, large).

tryPlaceLeaf: if state at pos has PERSISTENT==true → false; else if
!validTreePos → false; else state = foliageProvider.getState (0 draws); if
has WATERLOGGED → set from isFluidAtPosition(pos, isSourceOfType(WATER));
setter.set(pos, state) → adds to leaves set + setBlock 19; true.

- Blob (jungle_tree, h3 o0 r2): for l = offset..offset−fh (INCLUSIVE, desc):
  range = max(rad + att.radiusOffset − 1 − l/2, 0) [Java idiv]; row(l).
  shouldSkip: `dx==range && dz==range && (nextInt(2)==0 || y==0)` — draw
  only at corners, BEFORE the y test.
- Bush (jungle_bush, h2 o1 r2): range = rad + att.radiusOffset − 1 − l (no /2);
  shouldSkip: corners && nextInt(2)==0.
- MegaJungle (h2 o0 r2): rows = att.doubleTrunk ? fh : 1 + nextInt(2) [DRAW
  when single trunk]; for l = offset..offset−rows (INCLUSIVE desc): range =
  rad + att.radiusOffset + 1 − l; row(l, doubleTrunk). shouldSkip (0 draws):
  `dx + dz >= 7 || dx*dx + dz*dz > range*range`.
- Fancy (fancy_oak, h4 o4 r2): for l = offset..offset−fh (INCLUSIVE desc):
  range = rad + (l==offset || l==offset−fh ? 0 : 1); row(l).
  shouldSkip (0 draws): `Mth.square(dx+0.5f) + Mth.square(dz+0.5f) > range²`
  (float; Mth.square(f)=f*f).

## 7. FancyTrunkPlacer.placeTrunk

```java
j = free + 2; k = Mth.floor(j * 0.618);           // Mth.floor(D)I
placeBelowTrunkBlock(pos.below());
clusters = Math.min(1, Mth.floor(1.382 + (1.0*j/13.0)^2));   // vanilla oddity: always <=1
i1 = pos.getY() + k;                               // branch-base cap
list = [FoliageCoords(pos.above(j-5), i1)];
for (j1 = j-5; j1 >= 0; j1--) {
    f = treeShape(j, j1); if (f < 0) continue;
    for (k1 = 0; k1 < clusters; k1++) {
        d0 = 1.0 * (double)f * ((double)nextFloat() + 0.328);   // DRAW A
        d1 = (double)(nextFloat() * 2.0f) * Math.PI;            // DRAW B
        d2 = d0 * Math.sin(d1) + 0.5;  d3 = d0 * Math.cos(d1) + 0.5;   // JDK sin/cos (hc_jdk_*)
        pos1 = pos.offset(Mth.floor(d2), j1 - 1, Mth.floor(d3)); pos2 = pos1.above(5);
        if (makeLimb(pos1, pos2, false)) {          // CHECK-only pass, world reads, 0 draws
            dx = pos.x-pos1.x; dz = pos.z-pos1.z;
            d4 = (double)pos1.y - Math.sqrt((double)(dx*dx+dz*dz)) * 0.381;
            bb = d4 > (double)i1 ? i1 : (int)d4;
            if (makeLimb(new BlockPos(pos.x, bb, pos.z), pos1, false))
                list.add(FoliageCoords(pos1, bb));
        }
    }
}
makeLimb(pos, pos.above(k), true);                 // trunk logs (PLACE)
makeBranches: for fc in list: bb=fc.branchBase; pos3=(pos.x, bb, pos.z);
    if (!pos3.equals(fc.att.pos()) && trimBranches(j, bb - pos.y))
        makeLimb(pos3, fc.att.pos(), true);
return [fc.attachment for fc in list if trimBranches(j, fc.branchBase - pos.y)];
```
- FoliageCoords(p, bb): attachment = Attachment(p, 0, false).
- makeLimb(from, to, place): if (!place && from.equals(to)) return true;
  delta = to − from; steps = max(|dx|,|dy|,|dz|); fx,fy,fz = comp/steps
  (float); for i in 0..steps INCLUSIVE: p = from.offset(Mth.floor(0.5f+i*fx),
  …fy, …fz)  [Mth.floor(F)I]; place ? placeLog(p, axis fn) :
  (!isFree(p) → return false). axis fn: AXIS = getLogAxis(from, p):
  dx=|p.x−from.x|, dz=|p.z−from.z|, m=max; m>0 ? (dx==m ? X : Z) : Y.
- treeShape(j, y): (float)y < (float)j*0.3f → −1; f=j/2.0f; f1=f−y;
  f2=Mth.sqrt(f*f−f1*f1) [(float)Math.sqrt]; if f1==0 f2=f; else if
  |f1| >= f return 0; return f2*0.5f.
- trimBranches(j, y) = (double)y >= (double)j*0.2.
- Draw stream: exactly 2 nextFloat per y-level with treeShape >= 0 —
  world-INDEPENDENT (makeLimb checks draw nothing).

## 8. Write flags

All tree writes (trunk/foliage/deco/dirt/distance rewrite) = setBlock flag
**19** (16|2|1): ProtoChunk path ignores flags for heightmaps ⇒ maps update;
flag 16 suppresses post-processing marks (dump-irrelevant).
StructureTemplate.updateShapeAtEdge(level, 3, shape, …): neighbor-shape
updates (updateFromNeighbourShapes) — block-state effects possible in
principle (e.g. floating vines pruned)…; 26.2 path operates on positions on
the shape boundary; for our palette the only shape-updatable states are
vines/cocoa which sit INSIDE the filled shape (deco set) — treated as
no-op; [UNVERIFIED — trace/07 gates will catch if wrong].

## 9. two_layers_feature_size

TwoLayersFeatureSize(limit=1, lowerSize=0, upperSize=1 codec defaults;
min_clipped_height optional-empty). getSizeAtHeight(height, y) =
y < limit ? lowerSize : upperSize.
- jungle_tree: defaults → limit 1, lower 0, upper 1.
- jungle_bush: limit 0, upper 0 → size 0 everywhere.
- mega_jungle: defaults limit 1 + lower 1, upper 2.
- fancy_oak: limit 0, upper 0, min_clipped 4.
[VERIFIED-bytecode for getSizeAtHeight + codec defaults via
TwoLayersFeatureSize <init>/codec — defaults re-checked: limit 1, lowerSize 0,
upperSize 1.]

## 10. FallenTreeFeature (fallen_jungle_tree)

place → placeFallenTree, RETURNS TRUE ALWAYS.
1. placeStump: placeLogBlock(origin) = setBlock(origin, trunkProvider state,
   flag **3**) + Feature.markAboveForPostProcessing (dump-inert),
   UNCONDITIONAL write; decorateLogs(level, random, Set.of(stump),
   stumpDecorators=[trunk_vine]) — Context(setter=flag-19, logs={stump},
   leaves/roots empty; sort trivial) → trunk_vine draws 4 × nextInt(3) on the
   stump with conditional vine writes.
2. dir = HORIZONTAL.getRandomDirection = [N,E,S,W][nextInt(4)] — DRAW.
3. len = logLength.sample − 2 (uniform[4,11] → 1 draw) — DRAW.
4. start = origin.relative(dir, 2 + nextInt(2)) — DRAW; mutable.
5. setGroundHeightForFallenLogStartPos: m.move(UP); 6×: if
   (validTreePos(m) && isOverSolidGround(m)) stop; else m.move(DOWN).
   isOverSolidGround = state(below).isFaceSturdy(level, below, UP) — support
   shape (leaves NOT sturdy).
6. canPlaceEntireFallenLog: gap=0; len×: !validTreePos → false;
   !isOverSolidGround → ++gap > 2 → false; else gap=0; m.move(dir). Then
   m.move(dir.opposite, len); true.
7. placeFallenLog (only if 6 passed): HashSet logs; len×:
   placeLogBlock(m, AXIS=dir.getAxis()) (flag 3, unconditional), add
   immutable, m.move(dir); decorateLogs(logs, logDecorators=
   [attached_to_logs]).

## 11. Decorators

Context lists: logs/leaves sorted by Y stable (§3). Context.placeVine(pos,
faceProp) = setBlock(pos, vine[face=true], via decoSetter flag 19).
Context.isAir = state.isAir.

- CocoaDecorator(p=0.2): nextFloat DRAW; !(f<p) → return. logs empty →
  return. minY = logs.getFirst().y. For each log with y−minY <= 2 (list
  order): for dir in HORIZONTAL [N,E,S,W]: nextFloat DRAW ALWAYS; if
  f <= 0.25f (hardcoded, INCLUSIVE): pos2 = log + dir.opposite (x/z only);
  if isAir(pos2): setBlock(pos2, cocoa[AGE=nextInt(3) DRAW][FACING=dir]).
- TrunkVineDecorator: for each log (list order): nextInt(3) DRAW; >0 && west
  isAir → vine[east]; nextInt(3) >0 && east isAir → vine[west]; nextInt(3)
  >0 && north isAir → vine[south]; nextInt(3) >0 && south isAir →
  vine[north]. 4 draws per log ALWAYS.
- LeaveVineDecorator(p=0.25): for each leaf (list order): 4 × {nextFloat
  DRAW; if f < p && side isAir: addHangingVine(side, faceProp)}: placeVine at
  side, then extend down while isAir && count(4) > 0 (max 4 extra), same face.
  Order west/EAST, east/WEST, north/SOUTH, south/NORTH.
- AttachedToLogsDecorator(prob 0.1, directions=[up], weighted provider):
  shuffled = Util.shuffledCopy(logs, random): copy then Util.shuffle: for
  j = size; j > 1; j--: swap(j−1, nextInt(j)) — size−1 draws. For each pos in
  shuffled order: dir = Util.getRandom(directions) = nextInt(1) DRAW (burns);
  pos2 = pos.relative(dir); nextFloat DRAW; if f <= prob (fcmpg ifgt —
  INCLUSIVE) && isAir(pos2): setBlock(pos2, provider.getState —
  weighted{red 2, brown 1} nextInt(3) DRAW).

## 12. Java HashSet<BlockPos> iteration — REQUIRED for parity

Vec3i.hashCode = (y + z*31)*31 + x. Sets.newHashSet() = HashMap(16, 0.75).
JDK8+ HashMap semantics the port must copy: index = (cap−1) & (h ^ h>>>16);
append at bucket tail; resize when ++size > threshold (cap*0.75) AFTER
insert; resize splits each bucket into lo/hi lists preserving relative order
(bit oldCap of hash); ALSO resize when a bucket reaches 8 nodes and
cap < 64 (treeifyBin path); if cap >= 64 a bucket reaching 8 TREEIFIES —
NOT ported, die loudly (tree bins reorder iteration; unreachable for tree-
sized sets in practice). Iteration = buckets ascending, chain head→tail.
Consumers: Context list construction (stable sort by Y keeps HashSet order
within same Y — decorator RNG order!), updateLeaves seed set + per-level
poll order, place() union fill (order-inert), FallenTree log set (all same
Y → sort keeps HashSet order for attached_to_logs shuffle input).

## 13. Palette additions needed

red_mushroom, brown_mushroom (attached_to_logs). Mushrooms never fired in
the golden grid (no mushroom blocks in 07 diffs) but the code path must
exist; MushroomBlock canSurvive is NOT consulted (decorator writes blind
after isAir).
