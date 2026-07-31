# Feature-trace golden format (v1)

Produced by `tools/golden/make_feature_trace.sh` (FeatureTrace hooks in the
stage-dump mod, `-Dhyperchunk.dump.trace=true`) from vanilla Minecraft
**26.2**, seed 1234567890, `max.bg.threads=1`, dump radius 2 (5×5 chunks).
Trace/dump files are gitignored; sha256 hashes live in
`golden/features-trace/SHA256SUMS`. `order.manifest` / `order.snapshots` of
the trace run are tracked.

This bundle is the Task 9 (features stage) bisect ladder between "decoration
seed matches" and "07 blocks match" (ADR-007 Tier 2), plus the ring-chunk
inputs the C replay reads at the grid edge.

## Validity vs the primary bundle

The traces describe THE RUN THAT PRODUCED THEM. They are valid goldens for
the committed primary bundle `golden/stages/seed<seed>` because the harness
verifies, before writing SHA256SUMS:

- the run's 9 grid-chunk `01..07` dumps byte-match the committed
  `golden/SHA256SUMS` entries, and
- the run manifest's first 9 data lines `(seq chunkX chunkZ seedHex)` equal
  the committed manifest's

i.e. the 1-thread sticky grid order (tools/golden/NOTES.md) reproduced the
committed decoration order and state exactly. If that verification fails the
harness aborts and nothing is recorded. Ring-chunk `01..06` dumps are
order-free (Tier 1 stages) and valid regardless; ring `07` dumps and ring
traces are coherent with the primary bundle only if the harness reported
`full manifest identical` (see the run log / harness summary).

## Directory layout

```
golden/features-trace/seed<seed>/
  traces/c.<x>.<z>.trace.txt    one per features application of the run
  c.<x>.<z>/<II>_<stage>.*.txt  ring chunks only (chessboard distance 2),
                                stages 01..07, format per golden/stages/FORMAT.md
  order.manifest                this run's features order (format per NOTES.md)
  order.snapshots
```

Grid-chunk (3×3) dump dirs are pruned after verification — they are
byte-identical duplicates of the primary bundle. Ring `08..11` dumps are
pruned (async-timing snapshots, not features-work inputs).

## `traces/c.<x>.<z>.trace.txt`

`#` header lines, then events in execution order:

```
begin <cx> <cz> <decorationSeedHex>
s <step> <index> <structure_id>
p <step> <index> <x> <y> <z> <placed01>
f <step> <index> <npos> <placed01> <placed_feature_id>
end <cx> <cz> <f_line_count>
```

- `begin` — emitted at `WorldgenRandom#setDecorationSeed@RETURN` inside this
  chunk's `generateFeatures` (same instant as the order.manifest line; the
  seed hex must match the manifest — the harness checks).
- `(step, index)` — the exact `setFeatureSeed(decorationSeed, index, step)`
  salts (recon `.hermes/notes/task9pre-order/A2` §2.1): `index` is the
  position in the per-step `FeatureSorter` list for features
  (`reference/features_order-26.2.txt`), the per-step registry counter for
  structures.
- `s` — a `StructureStart#placeInChunk` call ran for the current salt.
  The harness fails if any grid-chunk trace contains one (Task 9a scope
  assumption: no structures in the grid).
- `p` — one TOP-LEVEL `ConfiguredFeature#place` call: `x y z` is a position
  that survived the placed feature's whole placement-modifier pipeline,
  `placed01` its return value. Nested placements inside composite features
  (trees, selectors) are depth-excluded. `p` lines appear in vanilla's
  evaluation order — the depth-first pipeline order the C replay must
  reproduce draw-for-draw.
- `f` — the `PlacedFeature#placeWithBiomeCheck` return for the item:
  `npos` = its `p`-line count, `placed01` = whether any position placed,
  then the placed-feature registry id.

A feature filtered out entirely (empty pipeline output) still yields its
`f` line with `npos 0 placed 0` — the C walk must reproduce those too
(exact RNG accounting for skipped-but-counted features).
