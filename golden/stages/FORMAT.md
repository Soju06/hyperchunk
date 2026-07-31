# Golden stage dump format (v1)

Produced by `tools/golden/make_stage_dumps.sh` (Fabric mixin harness in
`tools/golden/stage-dump-mod/`) from vanilla Minecraft **26.2**,
`level-seed=1234567890`, default overworld settings. The dump files are
gitignored; their sha256 hashes live in `golden/SHA256SUMS`.

## Directory layout

```
golden/stages/seed<seed>/c.<chunkX>.<chunkZ>/<II>_<stage>.<kind>.txt
```

- `chunkX`, `chunkZ` — chunk coordinates (may be negative, e.g. `c.-1.0`)
- `II` — zero-padded `ChunkStatus.getIndex()`, so files sort in pipeline order
- `stage` — `ChunkStatus` name without the `minecraft:` prefix

The 26.2 generation pipeline (`net.minecraft.world.level.chunk.status.ChunkStatus`),
in order, with the indices used in filenames:

| index | stage | notes |
|---|---|---|
| 00 | empty | never dumped (nothing exists yet) |
| 01 | structure_starts | |
| 02 | structure_references | |
| 03 | biomes | biome palette becomes meaningful here |
| 04 | noise | first stage with terrain blocks |
| 05 | surface | |
| 06 | carvers | includes 26.2 sulfur-cave carving |
| 07 | features | |
| 08 | initialize_light | light files appear from here |
| 09 | light | |
| 10 | spawn | no block changes expected vs features |
| 11 | full | ProtoChunk → LevelChunk conversion |

Every stage that runs in vanilla is dumped unfiltered; a dump is taken in the
stage's own `CompletableFuture` continuation, i.e. after the stage finished
and before any dependent stage observes completion.

Dumps are taken only for steps of `ChunkPyramid.GENERATION_PYRAMID` — chunk
*loading* never overwrites a generation-time dump — and only once per
(chunk, stage).

## Common header

Every file starts with `#`-prefixed header lines:

```
# hyperchunk golden stage dump v1
# kind <blocks|biomes|heightmaps|light_block|light_sky>
# chunk <chunkX> <chunkZ>
# stage <stage>
# minY <minY> maxY <maxY> height <height>
# order <human-readable iteration order>
```

`minY`/`maxY` are **inclusive** world-Y block coordinates observed at runtime
(26.2 overworld: `minY -64 maxY 319 height 384`). All files are ASCII text,
LF line endings, values space-separated unless stated otherwise.

## `*.blocks.txt` — block states

After the header:

1. Palette lines: `palette <index> <blockstate>`, indices dense from 0 in
   order of first appearance. `<blockstate>` is the canonical
   `BlockStateParser.serialize` form, e.g. `minecraft:stone` or
   `minecraft:water[level=0]` (property list only when non-default properties
   exist; property order as serialized by vanilla).
2. A line containing exactly `data`.
3. `height × 16` data rows, each row = 16 space-separated palette indices:

```
for y in minY..maxY (ascending):      # 384 y-levels
  for z in 0..15:                     # one output row per (y, z)
    for x in 0..15:                   # 16 indices per row, x ascending
```

`x`/`z` are chunk-local block offsets (world = chunkX*16 + x). Linear index
of a block within the data section: `((y - minY) * 16 + z) * 16 + x`.

## `*.biomes.txt` — biomes (4×4×4 quart resolution)

Same palette/`data` structure as blocks, but the palette holds biome ids
(e.g. `minecraft:plains`) and the grid is in quart coordinates (block >> 2):

```
for quartY in (minY>>2)..(maxY>>2) (ascending):   # 96 layers
  for quartZ in 0..3:                             # one row per (quartY, quartZ)
    for quartX in 0..3:                           # 4 indices per row
```

Biome storage is only populated by the `biomes` stage; dumps of earlier
stages show the pre-fill default (`minecraft:plains`) and are kept only to
prove when population happens.

## `*.heightmaps.txt` — heightmaps

After the header, for each heightmap type present on the chunk at that stage:

```
heightmap <serialization key>     # e.g. WORLD_SURFACE_WG, OCEAN_FLOOR_WG
<16 rows (z ascending) of 16 space-separated values (x ascending)>
```

Values are `Heightmap.getFirstAvailable(x, z)` = world Y of the highest
blocking block **plus one** (vanilla convention). Which types exist depends
on the stage (worldgen types `*_WG` during generation; final types from
`initialize_light` on).

## `*.light_block.txt`, `*.light_sky.txt` — light (only for stages ≥ initialize_light)

After the header and a `data` line: same `y,z,x` iteration order as blocks,
but each row is 16 **hex digits** (no separators), one digit per block,
value 0–15 as returned by the corresponding
`LevelLightEngine.getLayerListener(<BLOCK|SKY>).getLightValue(pos)`.

## `order.manifest` — features-stage execution order (v1, tracked in git)

One line per features-stage chunk application, in the actual execution
order of the recording run (ADR-007 Tier-2 replay input):

```
# hyperchunk features order manifest v1
# target_version 26.2
# seed <level seed, decimal>
# dimension minecraft:overworld
# grid center=(0,0) radius=1
# hook net.minecraft.world.level.levelgen.WorldgenRandom#setDecorationSeed(JII)J@RETURN armed-by net.minecraft.world.level.chunk.status.ChunkStatusTasks#generateFeatures
# columns seq chunkX chunkZ decorationSeedHex thread nanos
0 -1 -1 4ffcca0c0308cf12 Worker-Main-1 1080045214459802
```

- `seq` — dense from 0, assigned and appended under one lock (file order ==
  seq order). Records ALL features applications in the dump dimension
  (spawn area + dependency ring), not only the grid.
- `decorationSeedHex` — the value `setDecorationSeed` actually returned,
  16 hex digits two's complement. A pure function of (seed, chunk pos):
  replay recomputes it and must match.
- `thread` — recording thread, whitespace replaced by `_`.
- `# ERROR` lines mean a capture miss; the harness fails on them.

## `order.snapshots` — dump positions in the features order (v1, tracked)

```
# columns stage chunkX chunkZ seqBegin seqEnd thread nanos
07_features 0 0 5 5 Worker-Main-1 1080045535659712
```

`seqBegin`/`seqEnd` are the manifest seq counter sampled before/after that
(chunk, stage) dump's file writes. Equal values mean the dump saw exactly
the first `seqBegin` features applications; unequal values flag a torn
snapshot (only async-completing stages: `09_light`, `11_full`).
`07_features` dumps always satisfy `seqBegin == seqEnd ==` (own manifest
seq + 1): they are taken synchronously inside the serialized features step.

## Reproducing

```
bash tools/golden/make_stage_dumps.sh
```

wipes `golden/stages/seed<seed>/`, regenerates a fresh world with the pinned
server + pinned Fabric loader, and rewrites the hashes in `golden/SHA256SUMS`
(`HYPERCHUNK_DUMP_DIR=.../golden/stages-alt/seed<seed>` writes an alt bundle
with its own `golden/stages-alt/SHA256SUMS`; `HYPERCHUNK_BG_THREADS` >1 gives
the scheduler more ordering freedom).

Stages `01..06` are order-free: regenerated dumps must hash identically, and
a mismatch there is a parity-relevant finding. `07_features`+ depend on the
features execution order, which vanilla does not fix run to run
(`tools/golden/NOTES.md`, ADR-007): a bundle is a coherent
(dumps + order.manifest + order.snapshots) triple that regeneration replaces
wholesale — 07+ dumps are only reproducible by REPLAYING the recorded order.
