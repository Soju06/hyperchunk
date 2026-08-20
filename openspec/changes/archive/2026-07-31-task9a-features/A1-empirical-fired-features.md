# A1 — Empirical enumeration of what the features stage DID (06_carvers → 07_features), golden 9-chunk grid

MC **26.2** unobfuscated server, seed `1234567890`, grid center (0,0) radius 1.
Inputs (local, gitignored):

- `golden/stages/seed1234567890/c.<x>.<z>/{06_carvers,07_features}.{blocks,heightmaps}.txt` (primary bundle)
- `golden/stages-alt/seed1234567890/...` (alt bundle, different features execution order)
- `golden/rng/surface_seed1234567890.txt` section `quart_biomes` (5×5-chunk quart grid)
- `golden/stages{,-alt}/seed1234567890/order.manifest`, `order.snapshots`
- datapack JSON: `tools/golden/work/server/data/minecraft/worldgen/`, tags at `tools/golden/work/server/data/minecraft/tags/block/`
- bytecode: `javap -p -c` against `tools/golden/work/server` (spot checks cited inline)

Method: full-state block-by-block diff of `07_features.blocks.txt` vs `06_carvers.blocks.txt`
(linear index `((y+64)*16+z)*16+x`), per chunk, both bundles; positional/neighbor analysis of
ambiguous transitions; biome quart analysis; JSON/bytecode attribution. Analysis scripts were
throwaway (`/tmp/t9a_*.py`, not committed); everything needed to re-derive is described here.

Labels: **[VERIFIED-bytecode]** = checked in 26.2 `javap` output; **[VERIFIED-data]** = read from
the extracted 26.2 datapack JSON/tags; **[EMPIRICAL]** = measured from the golden dumps;
**[UNVERIFIED]** = plausible attribution not proven against bytecode.

---

## 1. Headline numbers

Total changed blocks 06→07, per chunk:

| chunk | primary | alt |
|---|---|---|
| c.-1.-1 | 5711 | 5711 (dump byte-identical) |
| c.-1.0  | 6058 | 6978 |
| c.-1.1  | 6412 | 6134 |
| c.0.-1  | 6044 | 6041 |
| c.0.0   | 7307 | 7281 |
| c.0.1   | 5394 | 5694 |
| c.1.-1  | 5090 | 5091 |
| c.1.0   | 6489 | 6484 |
| c.1.1   | 7154 | 6909 |
| **grid total** | **55659** | **56323** |

Only decoration steps **6 (UNDERGROUND_ORES)** and **9 (VEGETAL_DECORATION)** produced any block
in any of the 9 chunks, in both bundles. Steps 1 (lakes), 2 (geode), 3 (monster rooms),
8 (springs), 10 (freeze) all fired (they are in every relevant biome's feature lists) but placed
**zero** blocks — see §4 negatives. [EMPIRICAL]

Step naming (harness log `tools/golden/logs/feature_order_golden.log`, printed by the vanilla
enum): 0 RAW_GENERATION, 1 LAKES, 2 LOCAL_MODIFICATIONS, 3 UNDERGROUND_STRUCTURES,
4 SURFACE_STRUCTURES, 5 STRONGHOLDS, 6 UNDERGROUND_ORES, 7 UNDERGROUND_DECORATION,
8 FLUID_SPRINGS, 9 VEGETAL_DECORATION, 10 TOP_LAYER_MODIFICATION. The biome JSON `features`
arrays index exactly these steps. [VERIFIED-data]

## 2. Bundle sanity (task item 7)

- `06_carvers.blocks.txt` **byte-identical** between bundles for all 9 chunks (sha256). [EMPIRICAL]
- `07_features.blocks.txt` **differs for 8/9 chunks**; the identical one is **c.-1.-1**. [EMPIRICAL]
- Explanation from the order manifests: in both bundles the 9 grid chunks are decorated first
  (seq 0–8) before any ring chunk. Primary order starts `(-1,-1),(-1,0),(-1,1),(0,-1),...`;
  alt starts `(-1,1),(-1,-1),(-1,0),(0,-1),...`. A `07_features` dump is taken immediately after
  the chunk's own application (`order.snapshots`: seqBegin == own seq + 1, confirmed for all 9 in
  both bundles). c.-1.-1's dump therefore contains: primary = only its own application (seq 0);
  alt = its own (seq 1) plus alt-seq-0 = c.-1.1 — but c.-1.1 is 2 chunks away in z, outside its
  3×3 write window, so it contributed nothing. Same 06 input + same decoration seed ⇒ identical
  dump. Every other chunk saw a different *set/order* of prior neighbor applications ⇒ differs.
  [EMPIRICAL + manifest]
- `decorationSeedHex` per chunk is identical across both bundles for all 81 recorded chunks
  (pure function of seed+pos, as designed). [EMPIRICAL]

## 3. Bedrock, heightmaps (task items 3, 4)

**Bedrock:** counts identical 06 vs 07 in every chunk and every bedrock position identical
(per-cell comparison, both bundles): 785/790/718/786/777/720/767/778/772 for
c.-1.-1 … c.1.1. Bedrock is NOT features-placed; it is already final at 06. [EMPIRICAL]

**Heightmaps:**

- 06 files contain exactly `OCEAN_FLOOR_WG`, `WORLD_SURFACE_WG`.
- 07 files contain those two **plus** `MOTION_BLOCKING`, `MOTION_BLOCKING_NO_LEAVES`,
  `OCEAN_FLOOR`, `WORLD_SURFACE` (final maps first appear at the features dump).
- `OCEAN_FLOOR_WG` and `WORLD_SURFACE_WG` changed columns 06→07: **0 in all 9 chunks, both
  bundles** — the WG maps are frozen through features, exactly as the A4 note predicts
  (ProtoChunk.setBlockState primes/updates only the 4 FINAL maps during features). [EMPIRICAL]

Consequence for the C replay: features-stage placement modifiers that read heightmaps read
either a frozen WG map (`disk_*`, `underwater_magma`, `glow_lichen` use `OCEAN_FLOOR_WG`) or a
LIVE final map (`trees_jungle` uses `OCEAN_FLOOR`, `bamboo_light` uses `MOTION_BLOCKING`) that
mutates as features place blocks. [VERIFIED-data placement JSONs + A4]

---

## 4. Transition tables (task item 1)

Full-state diffs were computed; tables below are collapsed to block *kind* (properties
stripped). Property-level facts that matter are listed after the tables. The per-chunk
full-state tables can be re-derived mechanically from the dumps (diff, group by
`(state06,state07)`).

#### primary bundle — per-chunk (block-kind level, properties stripped)

**c.-1.-1** — 5711 changed blocks
```
  deepslate              -> tuff                         1064
  stone                  -> diorite                      1000
  stone                  -> granite                      886
  stone                  -> andesite                     653
  air                    -> oak_leaves                   520
  stone                  -> gravel                       220
  air                    -> jungle_leaves                198
  stone                  -> coal_ore                     190
  air                    -> vine                         189
  deepslate              -> clay                         167
  stone                  -> copper_ore                   92
  stone                  -> dirt                         91
  deepslate              -> gravel                       66
  stone                  -> clay                         49
  air                    -> jungle_log                   48
  stone                  -> iron_ore                     41
  air                    -> oak_log                      31
  deepslate              -> deepslate_diamond_ore        29
  air                    -> short_grass                  28
  deepslate              -> deepslate_redstone_ore       25
  deepslate              -> deepslate_iron_ore           23
  deepslate              -> deepslate_gold_ore           19
  grass_block            -> dirt                         17
  deepslate              -> deepslate_lapis_ore          14
  stone                  -> lapis_ore                    14
  air                    -> fern                         9
  deepslate              -> deepslate_copper_ore         9
  stone                  -> diamond_ore                  6
  air                    -> poppy                        5
  stone                  -> redstone_ore                 4
  stone                  -> gold_ore                     2
  air                    -> dandelion                    1
  air                    -> glow_lichen                  1
```
**c.-1.0** — 6058 changed blocks
```
  stone                  -> diorite                      1124
  deepslate              -> tuff                         907
  stone                  -> andesite                     831
  stone                  -> granite                      615
  stone                  -> dirt                         347
  stone                  -> gravel                       319
  air                    -> jungle_leaves                297
  air                    -> vine                         235
  air                    -> oak_leaves                   234
  stone                  -> clay                         149
  deepslate              -> clay                         143
  deepslate              -> moss_block                   141
  deepslate              -> andesite                     117
  air                    -> jungle_log                   92
  stone                  -> copper_ore                   73
  air                    -> short_grass                  60
  stone                  -> coal_ore                     54
  stone                  -> iron_ore                     42
  deepslate              -> deepslate_iron_ore           36
  deepslate              -> deepslate_diamond_ore        33
  deepslate              -> deepslate_redstone_ore       29
  deepslate              -> deepslate_gold_ore           25
  deepslate              -> deepslate_copper_ore         22
  air                    -> tall_grass                   16
  grass_block            -> dirt                         15
  air                    -> moss_carpet                  14
  air                    -> bamboo                       13
  air                    -> fern                         12
  deepslate              -> deepslate_lapis_ore          12
  air                    -> cave_vines_plant             9
  stone                  -> lapis_ore                    9
  air                    -> cave_vines                   7
  air                    -> glow_lichen                  7
  stone                  -> redstone_ore                 5
  air                    -> oak_log                      4
  air                    -> azalea                       3
  air                    -> cocoa                        3
  air                    -> flowering_azalea             2
  deepslate              -> copper_ore                   1
  deepslate              -> gold_ore                     1
```
**c.-1.1** — 6412 changed blocks
```
  deepslate              -> tuff                         850
  stone                  -> andesite                     787
  stone                  -> granite                      638
  stone                  -> dirt                         619
  stone                  -> diorite                      601
  deepslate              -> andesite                     479
  air                    -> jungle_leaves                392
  air                    -> vine                         371
  stone                  -> clay                         323
  stone                  -> gravel                       271
  air                    -> oak_leaves                   265
  deepslate              -> clay                         152
  air                    -> jungle_log                   136
  stone                  -> coal_ore                     72
  stone                  -> copper_ore                   66
  air                    -> short_grass                  63
  stone                  -> iron_ore                     51
  deepslate              -> gravel                       45
  deepslate              -> deepslate_redstone_ore       37
  deepslate              -> deepslate_diamond_ore        31
  air                    -> fern                         22
  deepslate              -> deepslate_gold_ore           18
  deepslate              -> deepslate_iron_ore           18
  air                    -> oak_log                      15
  deepslate              -> deepslate_lapis_ore          15
  stone                  -> lapis_ore                    15
  deepslate              -> moss_block                   14
  stone                  -> redstone_ore                 11
  grass_block            -> dirt                         10
  deepslate              -> deepslate_copper_ore         8
  deepslate              -> magma_block                  8
  stone                  -> gold_ore                     4
  air                    -> cave_vines_plant             2
  air                    -> cave_vines                   1
  air                    -> moss_carpet                  1
  stone                  -> magma_block                  1
```
**c.0.-1** — 6044 changed blocks
```
  stone                  -> granite                      1010
  stone                  -> diorite                      938
  deepslate              -> tuff                         637
  stone                  -> andesite                     482
  air                    -> oak_leaves                   447
  air                    -> jungle_leaves                426
  air                    -> vine                         367
  deepslate              -> andesite                     255
  deepslate              -> clay                         223
  stone                  -> dirt                         206
  deepslate              -> gravel                       152
  air                    -> jungle_log                   150
  stone                  -> clay                         105
  deepslate              -> diorite                      97
  deepslate              -> moss_block                   96
  stone                  -> copper_ore                   65
  air                    -> short_grass                  39
  stone                  -> iron_ore                     37
  deepslate              -> deepslate_gold_ore           36
  deepslate              -> deepslate_redstone_ore       34
  air                    -> oak_log                      33
  deepslate              -> deepslate_iron_ore           33
  stone                  -> coal_ore                     27
  deepslate              -> deepslate_diamond_ore        24
  air                    -> cave_vines_plant             22
  air                    -> fern                         17
  air                    -> cave_vines                   14
  grass_block            -> dirt                         14
  deepslate              -> deepslate_lapis_ore          11
  deepslate              -> dirt                         11
  stone                  -> redstone_ore                 10
  stone                  -> lapis_ore                    9
  air                    -> bamboo                       8
  air                    -> glow_lichen                  3
  air                    -> cocoa                        2
  air                    -> dirt                         1
  air                    -> spore_blossom                1
  deepslate              -> iron_ore                     1
  stone                  -> moss_block                   1
```
**c.0.0** — 7307 changed blocks
```
  deepslate              -> tuff                         1412
  stone                  -> granite                      1024
  stone                  -> andesite                     902
  stone                  -> diorite                      863
  air                    -> oak_leaves                   523
  deepslate              -> gravel                       293
  air                    -> jungle_leaves                285
  deepslate              -> clay                         284
  stone                  -> gravel                       229
  deepslate              -> moss_block                   227
  air                    -> vine                         203
  stone                  -> dirt                         150
  stone                  -> clay                         141
  air                    -> short_grass                  97
  stone                  -> coal_ore                     84
  stone                  -> iron_ore                     76
  air                    -> jungle_log                   54
  deepslate              -> andesite                     50
  stone                  -> copper_ore                   48
  deepslate              -> deepslate_gold_ore           38
  stone                  -> moss_block                   38
  air                    -> oak_log                      36
  deepslate              -> deepslate_redstone_ore       35
  air                    -> tall_grass                   28
  air                    -> cave_vines_plant             27
  air                    -> moss_carpet                  26
  deepslate              -> deepslate_diamond_ore        22
  air                    -> fern                         20
  air                    -> cave_vines                   14
  deepslate              -> deepslate_lapis_ore          14
  grass_block            -> dirt                         14
  stone                  -> lapis_ore                    14
  air                    -> azalea                       9
  deepslate              -> deepslate_iron_ore           6
  deepslate              -> diorite                      6
  air                    -> flowering_azalea             5
  stone                  -> redstone_ore                 4
  air                    -> cocoa                        3
  air                    -> glow_lichen                  3
```
**c.0.1** — 5394 changed blocks
```
  stone                  -> andesite                     763
  deepslate              -> tuff                         678
  stone                  -> granite                      441
  deepslate              -> diorite                      406
  stone                  -> clay                         382
  deepslate              -> clay                         367
  deepslate              -> moss_block                   272
  air                    -> vine                         260
  stone                  -> gravel                       248
  air                    -> jungle_leaves                233
  air                    -> oak_leaves                   203
  deepslate              -> gravel                       177
  stone                  -> diorite                      131
  stone                  -> coal_ore                     118
  stone                  -> dirt                         110
  air                    -> short_grass                  103
  stone                  -> copper_ore                   60
  stone                  -> iron_ore                     52
  deepslate              -> granite                      51
  air                    -> jungle_log                   39
  deepslate              -> deepslate_diamond_ore        36
  air                    -> moss_carpet                  35
  deepslate              -> dirt                         27
  air                    -> tall_grass                   22
  deepslate              -> deepslate_redstone_ore       17
  air                    -> fern                         16
  stone                  -> moss_block                   15
  deepslate              -> deepslate_lapis_ore          14
  grass_block            -> dirt                         14
  deepslate              -> deepslate_gold_ore           11
  stone                  -> lapis_ore                    10
  air                    -> azalea                       9
  air                    -> cave_vines                   9
  air                    -> cave_vines_plant             9
  deepslate              -> deepslate_copper_ore         9
  deepslate              -> redstone_ore                 7
  deepslate              -> deepslate_iron_ore           6
  deepslate              -> copper_ore                   5
  deepslate              -> gold_ore                     5
  stone                  -> redstone_ore                 5
  stone                  -> gold_ore                     4
  air                    -> flowering_azalea             3
  air                    -> glow_lichen                  3
  air                    -> big_dripleaf                 2
  air                    -> big_dripleaf_stem            2
  air                    -> small_dripleaf               2
  deepslate              -> iron_ore                     2
  air                    -> spore_blossom                1
```
**c.1.-1** — 5090 changed blocks
```
  stone                  -> granite                      997
  deepslate              -> tuff                         721
  stone                  -> diorite                      571
  stone                  -> andesite                     473
  deepslate              -> andesite                     304
  deepslate              -> clay                         272
  air                    -> oak_leaves                   258
  stone                  -> dirt                         245
  deepslate              -> gravel                       202
  air                    -> jungle_leaves                179
  deepslate              -> moss_block                   156
  air                    -> vine                         125
  stone                  -> coal_ore                     101
  air                    -> short_grass                  72
  stone                  -> copper_ore                   66
  stone                  -> iron_ore                     47
  deepslate              -> deepslate_redstone_ore       46
  air                    -> jungle_log                   28
  stone                  -> moss_block                   28
  deepslate              -> deepslate_diamond_ore        21
  deepslate              -> deepslate_iron_ore           21
  stone                  -> clay                         18
  air                    -> cave_vines_plant             17
  air                    -> moss_carpet                  17
  deepslate              -> deepslate_gold_ore           16
  grass_block            -> dirt                         13
  deepslate              -> deepslate_lapis_ore          12
  air                    -> cave_vines                   11
  air                    -> tall_grass                   10
  stone                  -> gold_ore                     7
  stone                  -> lapis_ore                    7
  air                    -> bamboo                       6
  air                    -> oak_log                      5
  air                    -> fern                         4
  air                    -> azalea                       3
  air                    -> flowering_azalea             2
  air                    -> glow_lichen                  2
  deepslate              -> iron_ore                     2
  deepslate              -> magma_block                  2
  air                    -> cocoa                        1
  air                    -> spore_blossom                1
  deepslate              -> diorite                      1
```
**c.1.0** — 6489 changed blocks
```
  stone                  -> diorite                      984
  air                    -> oak_leaves                   871
  stone                  -> andesite                     860
  deepslate              -> tuff                         855
  deepslate              -> clay                         678
  stone                  -> granite                      559
  stone                  -> dirt                         443
  air                    -> vine                         147
  stone                  -> gravel                       142
  stone                  -> clay                         122
  air                    -> jungle_leaves                108
  stone                  -> moss_block                   99
  air                    -> short_grass                  84
  deepslate              -> moss_block                   75
  stone                  -> iron_ore                     55
  air                    -> oak_log                      51
  stone                  -> copper_ore                   45
  stone                  -> coal_ore                     44
  deepslate              -> deepslate_redstone_ore       37
  air                    -> jungle_log                   34
  deepslate              -> deepslate_gold_ore           19
  deepslate              -> deepslate_lapis_ore          17
  deepslate              -> deepslate_iron_ore           16
  deepslate              -> deepslate_diamond_ore        15
  stone                  -> lapis_ore                    15
  grass_block            -> dirt                         13
  air                    -> cave_vines                   12
  air                    -> moss_carpet                  12
  air                    -> tall_grass                   10
  deepslate              -> deepslate_copper_ore         8
  air                    -> bamboo                       7
  air                    -> fern                         7
  stone                  -> redstone_ore                 7
  air                    -> cave_vines_plant             5
  air                    -> glow_lichen                  5
  deepslate              -> water                        5
  air                    -> flowering_azalea             4
  air                    -> small_dripleaf               4
  air                    -> big_dripleaf                 3
  air                    -> cocoa                        3
  air                    -> big_dripleaf_stem            2
  air                    -> spore_blossom                2
  deepslate              -> magma_block                  2
  air                    -> azalea                       1
  deepslate              -> andesite                     1
  deepslate              -> big_dripleaf                 1
```
**c.1.1** — 7154 changed blocks
```
  deepslate              -> tuff                         924
  stone                  -> granite                      795
  deepslate              -> clay                         766
  stone                  -> diorite                      707
  stone                  -> andesite                     676
  air                    -> oak_leaves                   420
  stone                  -> dirt                         361
  deepslate              -> moss_block                   354
  stone                  -> clay                         340
  air                    -> vine                         317
  air                    -> jungle_leaves                269
  deepslate              -> gravel                       211
  air                    -> short_grass                  97
  stone                  -> moss_block                   92
  stone                  -> copper_ore                   89
  deepslate              -> andesite                     85
  stone                  -> coal_ore                     76
  air                    -> cave_vines_plant             57
  deepslate              -> water                        57
  air                    -> jungle_log                   53
  deepslate              -> deepslate_redstone_ore       45
  stone                  -> gravel                       45
  stone                  -> iron_ore                     39
  air                    -> cave_vines                   36
  air                    -> moss_carpet                  31
  deepslate              -> granite                      27
  deepslate              -> deepslate_diamond_ore        26
  air                    -> oak_log                      24
  stone                  -> lapis_ore                    20
  deepslate              -> deepslate_gold_ore           19
  grass_block            -> dirt                         14
  air                    -> fern                         13
  air                    -> tall_grass                   12
  deepslate              -> deepslate_iron_ore           10
  air                    -> clay                         8
  air                    -> azalea                       6
  air                    -> big_dripleaf                 6
  stone                  -> gold_ore                     6
  air                    -> big_dripleaf_stem            4
  air                    -> glow_lichen                  4
  deepslate              -> diorite                      4
  air                    -> flowering_azalea             3
  air                    -> small_dripleaf               3
  deepslate              -> small_dripleaf               3
```

#### alt bundle — per-chunk (block-kind level, properties stripped)

**c.-1.-1** — 5711 changed blocks
```
  deepslate              -> tuff                         1064
  stone                  -> diorite                      1000
  stone                  -> granite                      886
  stone                  -> andesite                     653
  air                    -> oak_leaves                   520
  stone                  -> gravel                       220
  air                    -> jungle_leaves                198
  stone                  -> coal_ore                     190
  air                    -> vine                         189
  deepslate              -> clay                         167
  stone                  -> copper_ore                   92
  stone                  -> dirt                         91
  deepslate              -> gravel                       66
  stone                  -> clay                         49
  air                    -> jungle_log                   48
  stone                  -> iron_ore                     41
  air                    -> oak_log                      31
  deepslate              -> deepslate_diamond_ore        29
  air                    -> short_grass                  28
  deepslate              -> deepslate_redstone_ore       25
  deepslate              -> deepslate_iron_ore           23
  deepslate              -> deepslate_gold_ore           19
  grass_block            -> dirt                         17
  deepslate              -> deepslate_lapis_ore          14
  stone                  -> lapis_ore                    14
  air                    -> fern                         9
  deepslate              -> deepslate_copper_ore         9
  stone                  -> diamond_ore                  6
  air                    -> poppy                        5
  stone                  -> redstone_ore                 4
  stone                  -> gold_ore                     2
  air                    -> dandelion                    1
  air                    -> glow_lichen                  1
```
**c.-1.0** — 6978 changed blocks
```
  stone                  -> diorite                      1110
  deepslate              -> tuff                         1085
  stone                  -> granite                      984
  stone                  -> andesite                     831
  air                    -> oak_leaves                   518
  stone                  -> dirt                         346
  stone                  -> gravel                       329
  air                    -> jungle_leaves                290
  air                    -> vine                         251
  deepslate              -> moss_block                   171
  stone                  -> clay                         150
  deepslate              -> clay                         143
  deepslate              -> andesite                     135
  air                    -> short_grass                  82
  stone                  -> copper_ore                   73
  stone                  -> coal_ore                     70
  air                    -> jungle_log                   43
  stone                  -> iron_ore                     42
  deepslate              -> deepslate_iron_ore           38
  deepslate              -> deepslate_diamond_ore        33
  deepslate              -> deepslate_redstone_ore       32
  air                    -> oak_log                      28
  deepslate              -> deepslate_gold_ore           25
  air                    -> tall_grass                   24
  deepslate              -> deepslate_copper_ore         22
  air                    -> fern                         18
  air                    -> moss_carpet                  17
  air                    -> bamboo                       13
  deepslate              -> deepslate_lapis_ore          12
  grass_block            -> dirt                         11
  air                    -> cave_vines_plant             9
  stone                  -> lapis_ore                    9
  air                    -> azalea                       7
  air                    -> cave_vines                   7
  air                    -> glow_lichen                  7
  stone                  -> redstone_ore                 5
  air                    -> flowering_azalea             3
  stone                  -> gold_ore                     3
  deepslate              -> copper_ore                   1
  deepslate              -> gold_ore                     1
```
**c.-1.1** — 6134 changed blocks
```
  deepslate              -> tuff                         850
  stone                  -> granite                      638
  stone                  -> dirt                         599
  stone                  -> diorite                      596
  stone                  -> andesite                     494
  deepslate              -> andesite                     479
  air                    -> jungle_leaves                412
  air                    -> vine                         381
  stone                  -> clay                         319
  air                    -> oak_leaves                   283
  stone                  -> gravel                       271
  air                    -> jungle_log                   153
  deepslate              -> clay                         152
  stone                  -> coal_ore                     72
  stone                  -> copper_ore                   66
  stone                  -> iron_ore                     51
  air                    -> short_grass                  46
  deepslate              -> gravel                       45
  deepslate              -> deepslate_redstone_ore       37
  deepslate              -> deepslate_diamond_ore        31
  air                    -> fern                         22
  deepslate              -> deepslate_gold_ore           18
  deepslate              -> deepslate_iron_ore           18
  air                    -> oak_log                      15
  deepslate              -> deepslate_lapis_ore          15
  stone                  -> lapis_ore                    15
  grass_block            -> dirt                         13
  stone                  -> redstone_ore                 11
  deepslate              -> deepslate_copper_ore         8
  deepslate              -> magma_block                  8
  deepslate              -> moss_block                   7
  stone                  -> gold_ore                     4
  air                    -> cave_vines_plant             2
  air                    -> cave_vines                   1
  air                    -> moss_carpet                  1
  stone                  -> magma_block                  1
```
**c.0.-1** — 6041 changed blocks
```
  stone                  -> granite                      1010
  stone                  -> diorite                      938
  deepslate              -> tuff                         637
  stone                  -> andesite                     482
  air                    -> oak_leaves                   442
  air                    -> jungle_leaves                426
  air                    -> vine                         367
  deepslate              -> andesite                     255
  deepslate              -> clay                         223
  stone                  -> dirt                         206
  deepslate              -> gravel                       152
  air                    -> jungle_log                   150
  stone                  -> clay                         105
  deepslate              -> diorite                      97
  deepslate              -> moss_block                   96
  stone                  -> copper_ore                   65
  air                    -> short_grass                  40
  stone                  -> iron_ore                     37
  deepslate              -> deepslate_gold_ore           36
  deepslate              -> deepslate_redstone_ore       34
  air                    -> oak_log                      33
  deepslate              -> deepslate_iron_ore           33
  stone                  -> coal_ore                     27
  deepslate              -> deepslate_diamond_ore        24
  air                    -> cave_vines_plant             22
  air                    -> fern                         18
  air                    -> cave_vines                   14
  grass_block            -> dirt                         14
  deepslate              -> deepslate_lapis_ore          11
  deepslate              -> dirt                         11
  stone                  -> redstone_ore                 10
  stone                  -> lapis_ore                    9
  air                    -> bamboo                       8
  air                    -> glow_lichen                  3
  air                    -> cocoa                        2
  air                    -> dirt                         1
  air                    -> spore_blossom                1
  deepslate              -> iron_ore                     1
  stone                  -> moss_block                   1
```
**c.0.0** — 7281 changed blocks
```
  deepslate              -> tuff                         1412
  stone                  -> granite                      1024
  stone                  -> andesite                     902
  stone                  -> diorite                      863
  air                    -> oak_leaves                   531
  deepslate              -> gravel                       293
  deepslate              -> clay                         284
  air                    -> jungle_leaves                264
  stone                  -> gravel                       229
  deepslate              -> moss_block                   227
  air                    -> vine                         188
  stone                  -> dirt                         150
  stone                  -> clay                         141
  air                    -> short_grass                  97
  stone                  -> coal_ore                     84
  stone                  -> iron_ore                     76
  air                    -> jungle_log                   55
  deepslate              -> andesite                     50
  stone                  -> copper_ore                   48
  deepslate              -> deepslate_gold_ore           38
  stone                  -> moss_block                   38
  air                    -> oak_log                      36
  deepslate              -> deepslate_redstone_ore       35
  air                    -> tall_grass                   28
  air                    -> cave_vines_plant             27
  air                    -> moss_carpet                  26
  deepslate              -> deepslate_diamond_ore        22
  air                    -> fern                         20
  grass_block            -> dirt                         15
  air                    -> cave_vines                   14
  deepslate              -> deepslate_lapis_ore          14
  stone                  -> lapis_ore                    14
  air                    -> azalea                       9
  deepslate              -> deepslate_iron_ore           6
  deepslate              -> diorite                      6
  air                    -> flowering_azalea             5
  stone                  -> redstone_ore                 4
  air                    -> cocoa                        3
  air                    -> glow_lichen                  3
```
**c.0.1** — 5694 changed blocks
```
  stone                  -> andesite                     763
  deepslate              -> tuff                         678
  stone                  -> granite                      441
  deepslate              -> diorite                      406
  stone                  -> clay                         382
  deepslate              -> clay                         367
  air                    -> jungle_leaves                356
  air                    -> vine                         332
  air                    -> oak_leaves                   286
  deepslate              -> moss_block                   272
  stone                  -> gravel                       248
  deepslate              -> gravel                       177
  stone                  -> diorite                      131
  stone                  -> coal_ore                     118
  stone                  -> dirt                         110
  air                    -> short_grass                  108
  stone                  -> copper_ore                   60
  air                    -> jungle_log                   57
  stone                  -> iron_ore                     52
  deepslate              -> granite                      51
  deepslate              -> deepslate_diamond_ore        36
  air                    -> moss_carpet                  35
  deepslate              -> dirt                         27
  air                    -> tall_grass                   22
  deepslate              -> deepslate_redstone_ore       17
  grass_block            -> dirt                         16
  stone                  -> moss_block                   15
  deepslate              -> deepslate_lapis_ore          14
  deepslate              -> deepslate_gold_ore           11
  stone                  -> lapis_ore                    10
  air                    -> azalea                       9
  air                    -> cave_vines                   9
  air                    -> cave_vines_plant             9
  deepslate              -> deepslate_copper_ore         9
  air                    -> oak_log                      8
  deepslate              -> redstone_ore                 7
  deepslate              -> deepslate_iron_ore           6
  air                    -> fern                         5
  deepslate              -> copper_ore                   5
  deepslate              -> gold_ore                     5
  stone                  -> redstone_ore                 5
  stone                  -> gold_ore                     4
  air                    -> flowering_azalea             3
  air                    -> glow_lichen                  3
  air                    -> big_dripleaf                 2
  air                    -> big_dripleaf_stem            2
  air                    -> small_dripleaf               2
  deepslate              -> iron_ore                     2
  air                    -> spore_blossom                1
```
**c.1.-1** — 5091 changed blocks
```
  stone                  -> granite                      997
  deepslate              -> tuff                         721
  stone                  -> diorite                      571
  stone                  -> andesite                     473
  deepslate              -> andesite                     304
  deepslate              -> clay                         272
  air                    -> oak_leaves                   258
  stone                  -> dirt                         245
  deepslate              -> gravel                       202
  air                    -> jungle_leaves                179
  deepslate              -> moss_block                   156
  air                    -> vine                         125
  stone                  -> coal_ore                     101
  air                    -> short_grass                  72
  stone                  -> copper_ore                   66
  stone                  -> iron_ore                     47
  deepslate              -> deepslate_redstone_ore       46
  air                    -> jungle_log                   28
  stone                  -> moss_block                   28
  deepslate              -> deepslate_diamond_ore        21
  deepslate              -> deepslate_iron_ore           21
  stone                  -> clay                         18
  air                    -> cave_vines_plant             17
  air                    -> moss_carpet                  17
  deepslate              -> deepslate_gold_ore           16
  grass_block            -> dirt                         13
  deepslate              -> deepslate_lapis_ore          12
  air                    -> cave_vines                   11
  air                    -> tall_grass                   10
  stone                  -> gold_ore                     7
  stone                  -> lapis_ore                    7
  air                    -> bamboo                       6
  air                    -> fern                         5
  air                    -> oak_log                      5
  air                    -> azalea                       3
  air                    -> flowering_azalea             2
  air                    -> glow_lichen                  2
  deepslate              -> iron_ore                     2
  deepslate              -> magma_block                  2
  air                    -> cocoa                        1
  air                    -> spore_blossom                1
  deepslate              -> diorite                      1
```
**c.1.0** — 6484 changed blocks
```
  stone                  -> diorite                      984
  air                    -> oak_leaves                   871
  stone                  -> andesite                     860
  deepslate              -> tuff                         855
  deepslate              -> clay                         678
  stone                  -> granite                      559
  stone                  -> dirt                         443
  air                    -> vine                         147
  stone                  -> gravel                       142
  stone                  -> clay                         122
  air                    -> jungle_leaves                108
  stone                  -> moss_block                   99
  air                    -> short_grass                  77
  deepslate              -> moss_block                   75
  stone                  -> iron_ore                     55
  air                    -> oak_log                      51
  stone                  -> copper_ore                   45
  stone                  -> coal_ore                     44
  deepslate              -> deepslate_redstone_ore       37
  air                    -> jungle_log                   34
  deepslate              -> deepslate_gold_ore           19
  deepslate              -> deepslate_lapis_ore          17
  deepslate              -> deepslate_iron_ore           16
  deepslate              -> deepslate_diamond_ore        15
  stone                  -> lapis_ore                    15
  grass_block            -> dirt                         13
  air                    -> cave_vines                   12
  air                    -> moss_carpet                  12
  air                    -> tall_grass                   10
  air                    -> fern                         9
  deepslate              -> deepslate_copper_ore         8
  air                    -> bamboo                       7
  stone                  -> redstone_ore                 7
  air                    -> cave_vines_plant             5
  air                    -> glow_lichen                  5
  deepslate              -> water                        5
  air                    -> flowering_azalea             4
  air                    -> small_dripleaf               4
  air                    -> big_dripleaf                 3
  air                    -> cocoa                        3
  air                    -> big_dripleaf_stem            2
  air                    -> spore_blossom                2
  deepslate              -> magma_block                  2
  air                    -> azalea                       1
  deepslate              -> andesite                     1
  deepslate              -> big_dripleaf                 1
```
**c.1.1** — 6909 changed blocks
```
  deepslate              -> tuff                         924
  stone                  -> granite                      795
  deepslate              -> clay                         766
  stone                  -> diorite                      707
  stone                  -> andesite                     676
  stone                  -> dirt                         361
  deepslate              -> moss_block                   354
  stone                  -> clay                         340
  air                    -> oak_leaves                   332
  air                    -> vine                         249
  deepslate              -> gravel                       211
  air                    -> jungle_leaves                193
  air                    -> short_grass                  99
  stone                  -> moss_block                   92
  stone                  -> copper_ore                   89
  deepslate              -> andesite                     85
  stone                  -> coal_ore                     76
  air                    -> cave_vines_plant             57
  deepslate              -> water                        57
  air                    -> jungle_log                   49
  deepslate              -> deepslate_redstone_ore       45
  stone                  -> gravel                       45
  stone                  -> iron_ore                     39
  air                    -> cave_vines                   36
  air                    -> moss_carpet                  31
  deepslate              -> granite                      27
  deepslate              -> deepslate_diamond_ore        26
  stone                  -> lapis_ore                    20
  deepslate              -> deepslate_gold_ore           19
  air                    -> oak_log                      15
  grass_block            -> dirt                         13
  air                    -> fern                         12
  air                    -> tall_grass                   12
  deepslate              -> deepslate_iron_ore           10
  air                    -> clay                         8
  air                    -> azalea                       6
  air                    -> big_dripleaf                 6
  stone                  -> gold_ore                     6
  air                    -> big_dripleaf_stem            4
  air                    -> glow_lichen                  4
  deepslate              -> diorite                      4
  air                    -> flowering_azalea             3
  air                    -> small_dripleaf               3
  deepslate              -> small_dripleaf               3
```

#### grid-wide (block-kind level), primary vs alt

```
  06-kind                -> 07-kind                       primary      alt
  deepslate              -> tuff                             8048     8226
  stone                  -> granite                          6965     7334
  stone                  -> diorite                          6919     6900
  stone                  -> andesite                         6427     6134
  air                    -> oak_leaves                       3741     4041
  deepslate              -> clay                             3052     3052
  stone                  -> dirt                             2572     2551
  air                    -> jungle_leaves                    2387     2426
  air                    -> vine                             2214     2229
  stone                  -> clay                             1629     1626
  stone                  -> gravel                           1474     1484
  deepslate              -> moss_block                       1335     1358
  deepslate              -> andesite                         1291     1309
  deepslate              -> gravel                           1146     1146
  stone                  -> coal_ore                          766      782
  air                    -> short_grass                       643      649
  air                    -> jungle_log                        634      617
  stone                  -> copper_ore                        604      604
  deepslate              -> diorite                           514      514
  stone                  -> iron_ore                          440      440
  deepslate              -> deepslate_redstone_ore            305      308
  stone                  -> moss_block                        273      273
  deepslate              -> deepslate_diamond_ore             237      237
  air                    -> oak_log                           199      222
  deepslate              -> deepslate_gold_ore                201      201
  deepslate              -> deepslate_iron_ore                169      171
  air                    -> cave_vines_plant                  148      148
  air                    -> moss_carpet                       136      139
  grass_block            -> dirt                              124      125
  air                    -> fern                              120      118
  stone                  -> lapis_ore                         113      113
  deepslate              -> deepslate_lapis_ore               109      109
  air                    -> cave_vines                        104      104
  air                    -> tall_grass                         98      106
  deepslate              -> granite                            78       78
  deepslate              -> water                              62       62
  deepslate              -> deepslate_copper_ore               56       56
  stone                  -> redstone_ore                       46       46
  deepslate              -> dirt                               38       38
  air                    -> bamboo                             34       34
  air                    -> azalea                             31       35
  air                    -> glow_lichen                        28       28
  stone                  -> gold_ore                           23       26
  air                    -> flowering_azalea                   19       20
  deepslate              -> magma_block                        12       12
  air                    -> big_dripleaf                       11       11
  air                    -> cocoa                              12        9
  air                    -> small_dripleaf                      9        9
  air                    -> big_dripleaf_stem                   8        8
  air                    -> clay                                8        8
  deepslate              -> redstone_ore                        7        7
  deepslate              -> copper_ore                          6        6
  stone                  -> diamond_ore                         6        6
  deepslate              -> gold_ore                            6        6
  deepslate              -> iron_ore                            5        5
  air                    -> poppy                               5        5
  air                    -> spore_blossom                       5        5
  deepslate              -> small_dripleaf                      3        3
  air                    -> dirt                                1        1
  deepslate              -> big_dripleaf                        1        1
  air                    -> dandelion                           1        1
  stone                  -> magma_block                         1        1
  TOTAL                                                     55659    56323
```

#### Property-level facts worth pinning [EMPIRICAL]

- Water always appears as `minecraft:water[level=0]` in the dump palettes (the dumper prints the
  `level` property; both stages serialize identically so diffs are exact). Lava appears as
  `minecraft:lava[level=0]` in 06 palettes (carver lava) — **no new lava in 07**.
- `redstone_ore`/`deepslate_redstone_ore` place as `[lit=false]`.
- Leaves place with real `distance=1..6` values (`persistent=false, waterlogged=false`); vines
  with single-face booleans; glow_lichen with 1–2 face booleans (`waterlogged=false`); cave
  vines as body `cave_vines_plant[berries=…]` + head `cave_vines[age=23..25,berries=…]`;
  bamboo as `[age=1,leaves=none|small|large,stage=0|1]`; cocoa `[age=0..2,facing=…]`;
  tall_grass as matched `half=lower` + `half=upper` pairs (49/49 primary, 53/53 alt);
  dripleaf small/big including `waterlogged=true` variants standing inside pool water.
- No transition changes only a property on the same block kind (no e.g. `grass_block[snowy]`
  flips) in either bundle.

#### y-ranges by transition (primary bundle, grid aggregate) [EMPIRICAL]

```
  deepslate->tuff                    n=8048 y[-63..-9]
  stone->granite                     n=6965 y[1..45]
  stone->diorite                     n=6919 y[1..61]
  stone->andesite                    n=6427 y[1..54]
  air->oak_leaves                    n=3741 y[63..86]
  deepslate->clay                    n=3052 y[-20..7]
  stone->dirt                        n=2572 y[3..59]
  air->jungle_leaves                 n=2387 y[64..91]
  air->vine                          n=2214 y[-12..91]
  stone->clay                        n=1629 y[1..20]
  stone->gravel                      n=1474 y[1..58]
  deepslate->moss_block              n=1335 y[-21..6]
  deepslate->andesite                n=1291 y[-5..7]
  deepslate->gravel                  n=1146 y[-63..7]
  stone->coal_ore                    n=766  y[9..68]
  air->short_grass                   n=643  y[-20..72]   (two populations: caves + surface)
  air->jungle_log                    n=634  y[63..90]
  stone->copper_ore                  n=604  y[3..67]
  deepslate->diorite                 n=514  y[-3..7]
  stone->iron_ore                    n=440  y[3..67]
  deepslate->deepslate_redstone_ore  n=305  y[-62..4]
  stone->moss_block                  n=273  y[1..26]
  deepslate->deepslate_diamond_ore   n=237  y[-63..1]
  deepslate->deepslate_gold_ore      n=201  y[-63..7]
  air->oak_log                       n=199  y[63..82]
  deepslate->deepslate_iron_ore      n=169  y[-56..7]
  air->cave_vines_plant              n=148  y[-14..19]
  air->moss_carpet                   n=136  y[-20..23]
  grass_block->dirt                  n=124  y[62..71]
  air->fern                          n=120  y[63..72]
  stone->lapis_ore                   n=113  y[1..56]
  deepslate->deepslate_lapis_ore     n=109  y[-60..7]
  air->cave_vines                    n=104  y[-15..18]
  air->tall_grass                    n=98   y[-19..23]
  deepslate->granite                 n=78   y[-4..2]
  deepslate->water                   n=62   y[-5..-1]
  deepslate->deepslate_copper_ore    n=56   y[-6..7]
  stone->redstone_ore                n=46   y[4..15]
  deepslate->dirt                    n=38   y[2..7]
  air->bamboo                        n=34   y[63..77]
  air->azalea                        n=31   y[-16..19]
  air->glow_lichen                   n=28   y[-11..48]
  stone->gold_ore                    n=23   y[2..22]
  air->flowering_azalea              n=19   y[-16..23]
  air->cocoa                         n=12   y[63..72]
  deepslate->magma_block             n=12   y[-30..1]
  air->big_dripleaf                  n=11   y[-13..-4]
  air->small_dripleaf                n=9    y[-8..1]
  air->big_dripleaf_stem             n=8    y[-12..-5]
  air->clay                          n=8    y[-5..-2]
  deepslate->redstone_ore            n=7    y[1..3]
  stone->diamond_ore                 n=6    y[1..2]
  deepslate->gold_ore                n=6    y[1..4]
  deepslate->copper_ore              n=6    y[-3..3]
  air->poppy                         n=5    y[67..70]
  deepslate->iron_ore                n=5    y[4..6]
  air->spore_blossom                 n=5    y[-3..13]
  deepslate->small_dripleaf          n=3    y[-2..-1]
  air->dandelion                     n=1    y[68]
  stone->magma_block                 n=1    y[1]
  air->dirt                          n=1    y[63]
  deepslate->big_dripleaf            n=1    y[-1]
```

---

## 5. Category attribution (task item 2)

Candidate lists come from the 4 biomes present in the 5×5 area (`jungle`, `lush_caves`,
`beach`, `river` — §7): each biome's `features` array was read from
`data/minecraft/worldgen/biome/*.json` [VERIFIED-data]. All four share identical step-1/2/3/6/8/10
lists (lush_caves additionally has `ore_clay` in step 6); they differ only in step 9.

### (a) Ore/blob family (`minecraft:ore` feature, step 6) — the Task 9a gate core

Target tags [VERIFIED-data]:
- `#base_stone_overworld` = stone, granite, diorite, andesite, tuff, deepslate
- `#stone_ore_replaceables` = stone, granite, diorite, andesite
- `#deepslate_ore_replaceables` = deepslate, tuff

| transition(s) | feature(s) | config evidence |
|---|---|---|
| `deepslate->tuff` (8048/8226) | `ore_tuff` | size 64, count 2, y uniform(-64..0), target #base_stone_overworld |
| `stone->granite` + `deepslate->granite` | `ore_granite_lower` (count 2, y 0..60); `ore_granite_upper` (1/6, y 64..128) | size 64; deepslate hits are the y∈[-4..2] blend zone (deepslate ∈ target tag) |
| `stone->diorite` + `deepslate->diorite` | `ore_diorite_lower/upper` | ditto |
| `stone->andesite` + `deepslate->andesite` | `ore_andesite_lower/upper` | ditto (deepslate->andesite up to y7) |
| `stone->dirt` + `deepslate->dirt` | `ore_dirt` | size 33, count 7, y 0..160 |
| `stone->gravel` + `deepslate->gravel` | `ore_gravel` | size 33, count 14, full height (hence gravel down to y −63) |
| `stone->clay` + `deepslate->clay` (part) | `ore_clay` (**lush_caves-only**) | size 33, count 46, y bottom..256; blended with vegetation-patch clay, see (d) |
| `stone->coal_ore` | `ore_coal_upper/lower` (size 17) | no `deepslate_coal_ore` observed anywhere (coal y-ranges ≥0 barely overlap deepslate/tuff) |
| `stone->iron_ore` / `deepslate->deepslate_iron_ore` | `ore_iron_upper/middle/small` (size 9/9/4) | |
| `stone->copper_ore` / `deepslate->deepslate_copper_ore` | `ore_copper` → cf `ore_copper_small` (size 10) | |
| `stone->gold_ore` / `deepslate->deepslate_gold_ore` | `ore_gold`, `ore_gold_lower` (size 9) | |
| `stone->redstone_ore` / `deepslate->deepslate_redstone_ore` | `ore_redstone`, `ore_redstone_lower` (size 8) | |
| `stone->diamond_ore` / `deepslate->deepslate_diamond_ore` | `ore_diamond{,_medium,_large,_buried}` (sizes 4/8/12/8, discard 0.5/0.5/0.7/1.0) | placed `ore_diamond` references cf `ore_diamond_small` |
| `stone->lapis_ore` / `deepslate->deepslate_lapis_ore` | `ore_lapis` (triangle) + `ore_lapis_buried` (discard 1.0) | size 7 |

**Cross-feature chaining (important for gate exactness)** [EMPIRICAL + tags]: the 18 blocks of
`deepslate -> {iron,gold,copper,redstone}_ore` (STONE ore variants on a 06-deepslate cell, all at
y∈[-6..6]) are two features stacking within step 6: an igneous blob (granite/diorite/andesite ∈
biome list indices before the metal ores) first replaced deepslate, then a metal ore replaced
that igneous block via `#stone_ore_replaceables`. The net 06→07 diff hides the intermediate
state. Any per-transition validator must diff against the *live* world, not against 06.

### (b) Spring/fluid (step 8) + underwater_magma (step 6)

**Springs: ZERO successful placements grid-wide, both bundles.** Evidence:

- `spring_water` config places fluid state `minecraft:water[falling=true]`, `spring_lava` →
  `lava[falling=true]` [VERIFIED-data]; `SpringFeature.place` [VERIFIED-bytecode,
  `net/minecraft/world/level/levelgen/feature/SpringFeature.class`]:
  1. `getBlockState(origin.above()).is(validBlocks)` else fail;
  2. if `requiresBlockBelow` (codec default **true** [VERIFIED-bytecode: `ldc "requires_block_below", iconst_1`]):
     `getBlockState(origin.below()).is(validBlocks)` else fail;
  3. state at origin must be air **or** in validBlocks;
  4. count rocks = validBlocks among {W,E,N,S,below}, count holes = `isEmptyBlock` among the same 5;
  5. place iff rocks == `rock_count` (default **4**, `iconst_4`) and holes == `hole_count`
     (default **1**, `iconst_1`); then `setBlock(origin, state.createLegacyBlock(), 2)` +
     `scheduleTick`. **No RNG draws inside the feature body** — count/in_square/height_range
     placement draws happen in the placement pipeline, not here.
- No `air->lava` / `*->lava` transitions exist at all ⇒ spring_lava fired 20×9 attempts, 0 placed.
- All 62 water blocks have **air directly above in both 06 and 07** ⇒ step 1 above can never
  have passed ⇒ none of them is a spring; they are clay-pool surfaces (see (d)). spring_water:
  25×9 attempts, 0 placed. [EMPIRICAL]

**`underwater_magma`: 13 blocks, 2 clusters, identical in both bundles.**
`deepslate->magma_block` 12 + `stone->magma_block` 1: cluster A at world (-10..-8, y 0..1,
z 19..21) in c.-1.1 under a carver aquifer water pocket (water directly above several cells in
06); cluster B at (16..17, y −30..−29, z −2..0) straddling the c.1.-1 / c.1.0 border (single
feature call writing across the chunk boundary through the 3×3 window). Config: count
uniform(44..52) per chunk, `surface_relative_threshold_filter` OCEAN_FLOOR_WG ≤ −2,
floor_search_range 5, prob 0.5 per valid position, radius 1. [EMPIRICAL + VERIFIED-data]

### (c) Disk family: ZERO output

`disk_sand`/`disk_clay`/`disk_gravel` are gated by
`block_predicate_filter(matching_fluids=water)` at the `heightmap OCEAN_FLOOR_WG` position
[VERIFIED-data]. No surface water exists in the 9 chunks. Positive confirmation from targets:
disks only convert dirt/grass_block/clay (`disk_gravel`: dirt+grass_block → gravel; `disk_clay`:
dirt+clay → clay; `disk_sand`: dirt+grass_block → sand/sandstone) — the diffs contain **no**
`dirt->gravel`, `grass_block->gravel`, `dirt->clay`, `*->sand`, `*->sandstone` transitions.
[EMPIRICAL]

### (d) Vegetal / Task-9b (step 9)

Attributions [VERIFIED-data configs; mechanism details for 9b]:

- **Trees** (`trees_jungle`: default `jungle_tree`, 0.1 `fancy_oak_checked`, 0.5 `jungle_bush`,
  1/3 `mega_jungle_tree_checked`, 0.0125 `fallen_jungle_tree`; count weighted 50 (w9) / 51 (w1),
  `heightmap OCEAN_FLOOR`, `surface_water_depth_filter 0`):
  `air->jungle_log[axis=y]`, `air->oak_log[axis=x|y|z]` (fancy_oak branches), `air->oak_leaves`
  (fancy_oak AND jungle_bush — jungle_bush foliage is oak_leaves), `air->jungle_leaves`,
  `air->cocoa` (jungle_tree decorator p=0.2), tree-attached `air->vine`
  (trunk_vine + leave_vine p=0.25 decorators), `grass_block->dirt` (below_trunk_provider forces
  dirt; 124/125 blocks) and the single `air->dirt` at (6,63,-1) — trunk base over a carver
  overhang, dirt forced into air below the trunk [EMPIRICAL: jungle_log column starts at y64
  directly above].
- **`vines`** (jungle, count 127, y 64..100) + **`classic_vines_cave_feature`** (lush_caves):
  wall `air->vine` from y −12 up to 91.
- **`bamboo_light`** (1/4 rarity, `heightmap MOTION_BLOCKING`) → `air->bamboo` (34 primary
  across age/leaves/stage variants).
- **`flower_warm`** → `air->poppy` (5), `air->dandelion` (1). **`patch_grass_jungle`** →
  `air->short_grass` (surface population, y 63..72), `air->fern`.
- **`glow_lichen`** (count uniform 104..157, OCEAN_FLOOR_WG ≤ −13) → 28 blocks, y −11..48.
- **lush_caves set** (all placed with `environment_scan down for solid, max 12` +
  `random_offset y +1`):
  - `lush_caves_vegetation` = cf `moss_patch` (vegetation_patch, ground moss_block, replaceable
    `#moss_replaceable` = base stones + #cave_vines + dirt/mud/moss/grass tags, xz radius 4..7,
    depth 1, veg chance 0.8) and `lush_caves_ceiling_vegetation` (= `moss_patch_ceiling`,
    surface=ceiling, veg `cave_vine_in_moss`): `stone->moss_block`, `deepslate->moss_block`,
    plus `moss_vegetation` on top (weighted: flowering_azalea 4 / azalea 7 / moss_carpet 25 /
    short_grass 50 / tall_grass[lower] 10) → the cave-y `air->{moss_carpet,azalea,
    flowering_azalea,short_grass,tall_grass}` populations.
  - `cave_vines` (count 188, ceiling `block_column` down) → `air->cave_vines_plant` +
    `air->cave_vines` head blocks.
  - `patch_tall_grass_2` → cave `air->tall_grass` pairs.
  - `lush_caves_clay` (count 62) = random_boolean_selector{`clay_with_dripleaves`,
    `clay_pool_with_dripleaves`} (vegetation_patch / **waterlogged**_vegetation_patch, ground
    clay, replaceable `#lush_ground_replaceable` = #moss_replaceable + clay + gravel + sand,
    depth 3, extra_bottom 0.8, extra_edge 0.7):
    - large `stone->clay` / `deepslate->clay` contribution (mixed with `ore_clay`, not separable
      from the net diff);
    - **all 62 `deepslate->water[level=0]` blocks** = pool fill of `clay_pool_with_dripleaves`
      (WaterloggedVegetationPatchFeature.placeGroundPatch: super-placed ground set, then
      non-`isExposed` positions overwritten with `Blocks.WATER.defaultBlockState()`
      [VERIFIED-bytecode]). One pool cluster in c.1.0 (x29..31, z6..9, y −2..−1), pools in
      c.1.1 (x16..27, z16..31, y −5..−1). Every pool cell has clay directly below in 07;
      1-block-deep sheets over an uneven dug floor.
    - `deepslate->{small,big}_dripleaf[waterlogged=true]` (4 blocks): dripleaf vegetation placed
      into freshly dug pool water — same-patch composition, net diff hides the water.
    - the 8 `air->clay` blocks (c.1.1, y −5..−2): clay patches replacing **earlier-placed
      cave_vines columns** — cave_vines runs before lush_caves_clay in the lush_caves step-9
      list, and `#lush_ground_replaceable` ⊇ `#cave_vines`. Evidence: truncated hanging vine
      remains at (23,−5,22) directly below an `air->clay` column. Net diff shows the 06 block
      (air). [EMPIRICAL; tag chain VERIFIED-data]
  - `spore_blossom` (5 blocks, cave ceilings).
  - `rooted_azalea_tree`: **0 visible output** (no rooted_dirt / hanging_roots / azalea_leaves
    anywhere in either bundle).
- Dripleaf states observed also unwaterlogged (`air->big_dripleaf*`, `air->small_dripleaf`) from
  `clay_with_dripleaves` (dry variant).

### (e) Unexplained: NONE

Every transition kind in both bundles is attributed above. The only initially-surprising rows —
`air->clay`, `deepslate->(stone-variant ores)`, `deepslate->water`, `deepslate->dripleaf`,
`air->dirt` — are all two-feature compositions whose intermediate state is invisible in a
06-vs-07 diff.

### Zero-output candidates (fired but placed nothing) [EMPIRICAL]

- step 1 `lake_lava_underground`, `lake_lava_surface` — no lava/obsidian anywhere.
- step 2 `amethyst_geode` — no amethyst/calcite/smooth_basalt/budding_amethyst (1/24 rarity).
- step 3 `monster_room`, `monster_room_deep` — no cobblestone/mossy_cobblestone/spawner/chest.
- step 6 `disk_*` — §(c). step 8 springs — §(b).
- step 9 (jungle) `brown_mushroom_normal`, `red_mushroom_normal`, `patch_pumpkin`,
  `patch_sugar_cane`, `patch_firefly_bush_near_water`, `patch_melon` — no such blocks.
- step 10 `freeze_top_layer` — no snow/snow_block/ice (all biomes too warm; also no
  `grass_block[snowy=true]`).

---

## 6. Biome sets and 3×3 unions (task item 5)

Source: `golden/rng/surface_seed1234567890.txt` section `quart_biomes` (qx −8..11,
qy −16..79, qz −8..11; rows are qz ascending, columns qx ascending — orientation
**cross-verified**: all 13824 quarts of the 9 chunks match the per-chunk
`07_features.biomes.txt` dumps exactly). Palette: jungle, beach, river, lush_caves. [EMPIRICAL]

Own set (distinct quart values in the chunk's 4×4×96 quart volume):

- **all 9 grid chunks**: `{jungle, lush_caves}` — no exceptions.
- lush_caves occupies quart-y −4..4 (block y −16..19) in all grid chunks (c.-1.-1: −3..4;
  c.1.1: −4..5 = block y up to 23).

Ring chunks (needed for unions): beach appears in (0,−2), (1,−2), (2,−2), (2,−1), (2,0), (2,1),
(2,2); river appears **only** in (2,2) (single quart column qx=11,qz≥18 area). All other ring
chunks are `{jungle, lush_caves}`.

3×3 neighborhood unions of the 9 grid chunks:

| chunk | union | beach? | river? |
|---|---|---|---|
| c.-1.-1 | jungle, lush_caves, **beach** (via (0,−2)) | YES | no |
| c.-1.0  | jungle, lush_caves | no | no |
| c.-1.1  | jungle, lush_caves | no | no |
| c.0.-1  | jungle, lush_caves, **beach** | YES | no |
| c.0.0   | jungle, lush_caves | no | no |
| c.0.1   | jungle, lush_caves | no | no |
| c.1.-1  | jungle, lush_caves, **beach** | YES | no |
| c.1.0   | jungle, lush_caves, **beach** | YES | no |
| c.1.1   | jungle, lush_caves, **beach**, **river** | YES | YES |

Relevance: `applyBiomeDecoration` builds its feature index set from the 3×3 neighborhood's
stored-biome palettes (A2 §2.2). Beach and river share the grid's step-1/2/3/6/8/10 lists and
their step-9 extras (`trees_water`, `patch_bush`, `flower_default`, `patch_grass_badlands`,
`seagrass_river`, …) are a superset check only — but their presence in 5 of 9 unions means the
per-step feature INDEX walk of those chunks includes indices contributed by beach/river even
though the `biome` placement filter then rejects every placement outside those biomes. The C
walk must reproduce the same union or the `setFeatureSeed(decoSeed, perStepListIndex, step)`
indices drift. [EMPIRICAL + A2]

Caveat: A2 says the union is over `PalettedContainer.getAll` (palette entries), while this
measurement is over stored quart values. If any section retains a stale palette entry (e.g. the
pre-fill `minecraft:plains`), vanilla's set could be larger than measured. Open question below.

---

## 7. Structure check (task item 6)

- **No structure-attributable blocks** in any 06→07 diff, either bundle: zero cobblestone,
  mossy_cobblestone, spawner, chest, rails, planks-not-from-trees, tripwire, suspicious sand,
  TNT, etc. Every transition is accounted for in §5. [EMPIRICAL]
- Logs: `tools/golden/logs/latest.log` is empty (0 lines); `feature_order_golden.log` contains
  only the step-enum/ordering dump (its "UNDERGROUND_STRUCTURES/SURFACE_STRUCTURES" lines are
  decoration-step *names*, not placements); `golden/rng/latest.log` has no structure mentions.
  [EMPIRICAL]
- Data: structures that CAN generate in these biomes exist in the datapack (e.g. jungle
  pyramid, mineshaft, etc.) — nothing here proves they can't appear at other coordinates; the
  evidence is only that **no structure start materialized blocks inside the 9-chunk grid for
  this seed**. Structure placement inside the features stage (per-step, before placed features)
  therefore contributes 0 block writes AND 0 RNG draws from the per-chunk `WorldgenRandom` in
  this grid — structures use their own forked seeds (A2), so even a non-zero structure would not
  disturb the feature RNG, but for THIS gate the replay can skip structures entirely.
  [EMPIRICAL + A2]

---

## 8. Implications for the C implementation (Task 9a gate)

1. **Scope of the 9a gate is ores + step-6 misc.** 06→07 parity for this grid = igneous/dirt/
   gravel/tuff/clay blobs, the 15 ore placed-features, `underwater_magma`, plus the entire
   vegetal step (9b). Springs, disks, lakes, geodes, monster rooms, freeze, structures all
   produce zero blocks here — they must still be *walked* (their placement-modifier RNG draws
   burn state!) but need no world-write correctness for this gate beyond "place nothing".
2. **RNG walk order is everything.** The per-step feature index walk must include beach/river
   contributions in the 5 chunks whose 3×3 union contains them (§6 table). A union computed
   from the center chunk only, or from block-resolution biomes, will desync
   `setFeatureSeed(decorationSeed, perStepListIndex, step)` indexing.
3. **Placement pipeline draws happen even for failing features.** 25 spring_water + 20
   spring_lava attempts per chunk each burn count/in_square/height_range draws (and the
   SpringFeature body itself draws nothing — verified). Same for disk (biome/predicate gates),
   monster_room, etc. Exact per-modifier draw sequences are covered by the pipeline recon notes,
   not here — but this note fixes the expected *outcome*: zero blocks.
4. **Blob/ore writes cross chunk borders** (magma cluster B straddles c.1.-1/c.1.0; ore blobs
   near borders continue into neighbors). The 3×3 write window (A4) is mandatory; per-chunk
   diffs cannot be validated in isolation against "features of that chunk only".
5. **Live-world reads, not 06 snapshots.** Step-6 features read the current world: metal ores
   replace igneous blobs placed moments earlier (deepslate→iron_ore etc.), clay patches replace
   cave_vines placed earlier in step 9. An implementation that tests replaceability against the
   carvers-stage state will diverge in exactly these ~26 blocks.
6. **Heightmaps:** WG maps must be frozen after 06 (0 changed columns observed); the 4 final
   maps must be primed lazily during features and updated per `setBlockState` (A4), because
   `trees_jungle` (OCEAN_FLOOR) and `bamboo_light` (MOTION_BLOCKING) read them mid-stage —
   trees placed by an earlier neighbor application change the heightmap a later application
   reads.
7. **Bedrock is carvers-stage-final**; features never touch it (counts and positions identical).
8. **Replay determinism:** decoration seed per chunk is order-independent (identical across
   bundles); a chunk's dump equals {its own application} ∪ {all manifest-earlier applications
   that overlap its 3×3}. c.-1.-1 is the regression baseline that is order-insensitive for
   these two bundles.
9. **Serialization details for the gate differ:** `water[level=0]`, `lava[level=0]`,
   `redstone_ore[lit=false]`, leaves `distance` values — the C dump writer must emit the same
   full property lists as the harness (it prints non-default-only per FORMAT.md, but water/lava
   level IS printed — match the harness's serializer exactly, not the FORMAT.md prose).

## 9. Open questions

- **Palette-vs-stored biome union**: does any chunk section's biome `PalettedContainer` retain a
  palette entry (e.g. pre-fill `plains`) that is not among stored quart values? If yes,
  vanilla's `getAll`-based union is a superset of §6 and the C walk must replicate the palette
  behavior, not the stored-value set. Needs a targeted harness probe or PalettedContainer
  bytecode analysis (Task 9 implementation risk; the golden replay gate would catch it as an
  index-walk desync).
- `stone->diorite` reaches y61 while granite/andesite stop at 45/54 — consistent with random
  blob spread from y≤60 origins (`*_lower` uniform 0..60) and/or `*_upper` (y 64..128) blobs
  spreading downward into terrain; per-blob attribution not derivable from net diffs. No action
  needed unless the gate fails in 40 ≤ y ≤ 64.
- Clay split between `ore_clay` and the clay patches (both lush-only) is not separable from net
  diffs; a per-feature trace (Task 4 of the plan) will give exact per-feature expected counts.
- `moss_vegetation`'s `tall_grass[half=lower]` weight-10 entry vs the matched lower/upper pair
  counts (49/49, 53/53): implies the simple_block feature places both halves of double plants
  (or that all tall_grass came from `patch_tall_grass_2`). Resolve in 9b (SimpleBlockFeature
  bytecode).
