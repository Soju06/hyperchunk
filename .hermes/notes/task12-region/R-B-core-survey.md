# R-B — Core C codebase survey for the Task-12 region serializer

Scope: everything the chunk-NBT / Anvil writer needs from the existing core + harness.
All line numbers verified against working tree @ commit 4bebe8e (main, clean).

---

## 1. Chunk representation — `hc_chunk_t`

`core/include/hc_chunk.h:36-60`:

```c
typedef struct {
    int32_t   cx, cz;
    uint16_t *states;                     /* HC_BLOCKS 개 팔레트 인덱스 */
    int32_t   heightmap_ws[256];          /* WORLD_SURFACE(_WG), 컬럼당 1 */
    int32_t   heightmap_ocean_floor[256]; /* OCEAN_FLOOR(_WG) */
    int32_t   heightmap_final[HC_HMF_COUNT][256];
    uint8_t   hm_final_primed; /* 타입별 비트 (1<<HC_HMF_*) */
    int32_t   heightmap_wg_reprimed[2][256];
    uint8_t   wg_reprimed; /* 타입별 비트 (1<<0 OF_WG, 1<<1 WS_WG) */
    uint16_t  biomes[HC_QUARTS];
    uint8_t   promoted;    /* 0 pre-spawn, 10 spawn, 11 full */
} hc_chunk_t;
```

Constants (`hc_chunk.h:12-21`): `HC_MIN_Y -64`, `HC_MAX_Y 319`, `HC_HEIGHT 384`,
`HC_BLOCKS = 16*16*384 = 98304`, `HC_QUARTS_XZ 4`, `HC_QUARTS_Y 96`, `HC_QUARTS = 96*4*4 = 1536`.

Index helpers (all `static inline`, hc_chunk.h):
- blocks `hc_idx(x,y,z) = ((y - (-64))*16 + z)*16 + x` — x,z chunk-local [0,16), y world (hc_chunk.h:72-77). So per 16-block section `sec` (world section y, -4..19) the 4096 states are `c->states[hc_idx(0, sec*16, 0) .. +4095]` in y,z,x order — **exactly vanilla block_states data order** (YZX).
- heightmap column `hc_col_idx(x,z) = z*16 + x` (hc_chunk.h:80-84).
- biome quart `hc_quart_idx(qx,qy,qz) = ((qy - (-16))*4 + qz)*4 + qx`, qy world quart [-16..79] (hc_chunk.h:88-95). Per section `sec`, the 64 quarts are qy = sec*4 .. sec*4+3, each a 4x4 (qz,qx) plane — again YZX order matching vanilla biome PalettedContainer data order.

`hc_chunk_init(hc_chunk_t*, hc_arena_t*, cx, cz)` (hc_chunk.h:65) — allocates `states` from arena, zero-fills everything; returns -1 on arena exhaustion (never aborts).

## 2. Block states — id -> canonical string only (NO structured property table)

- `core/src/hc_blocks.h:13-159` — enum of internal ids, `HC_B_AIR = 0` … `HC_B_COUNT`.
  Verified by compilation: **HC_B_COUNT = 448** (also: CAVE_AIR=431, SHORT_GRASS=57,
  OAK_LEAVES_BASE=74, AZALEA_LEAVES_BASE=355, KELP_PLANT=447).
- `core/src/blocks.c:6-460` — `static const char *const NAMES[HC_B_COUNT]`, 1:1 with the enum.
  Entries are **fully serialized canonical strings**, `BlockStateParser.serialize` shape:
  `"minecraft:oak_leaves[distance=1,persistent=false,waterlogged=false]"` — properties in
  **alphabetical key order, ALL properties printed, no spaces**. Property-less blocks are bare
  (`"minecraft:stone"`).
- There is **no** {name, prop-key/value[]} struct anywhere. To emit an NBT
  `{Name:..., Properties:{k:v,...}}` compound the serializer must split the string:
  name = up to `'['`; then comma-separated `key=value` pairs (already alphabetically ordered).
  If vanilla's Properties compound iteration order (Task-A finding, empirical from golden mca)
  differs from alphabetical, reorder at emission time — the parse gives you the pairs either way.

Accessors (`hc_blocks.h:175-204`, impl blocks.c:462-697):
```c
const char *hc_block_name(uint16_t id);          /* never NULL; OOB = caller bug */
int32_t     hc_block_by_name(const char *name, int32_t len); /* -1 unknown */
int hc_block_is_air(uint16_t);        /* air | cave_air */
int hc_block_is_fluid(uint16_t);      /* source water/lava only */
int hc_block_blocks_motion/is_solid/is_full_cube(uint16_t);
int hc_block_fluid_nonempty/fluid_is_water(uint16_t);  /* incl waterlogged=true states */
int hc_block_is_leaves(uint16_t);
int hc_block_is_replaceable(uint16_t);
```
Flags come from `static const uint8_t FLAGS[HC_B_COUNT]` (blocks.c:492-660),
bits `F_MOTION=1,F_SOLID=2,F_FULL=4,F_LEAVES=8,F_REPL=16,F_WLOG=32` (blocks.c:483-490).
`F_WLOG` = the state string contains `waterlogged=true` (useful for fluid_ticks of waterlogged
blocks if ever needed).

Enumerating a state's properties "in some order": parse `NAMES[id]` — the only order the repo
knows is the alphabetical order baked into the strings.

## 3. Biomes

- Stored per chunk as `uint16_t biomes[1536]` of **interned registry ids** at quart (4x4x4)
  resolution, layout above. Zero-fill == id 0 (whatever was interned first!).
- Registry `hc_biome_reg_t` (`core/src/hc_biome.h:36-42`):
```c
typedef struct {
    hc_arena_t *arena;
    const char *names[HC_BIOME_MAX];   /* "minecraft:jungle" (arena copy) */
    float       temperature[HC_BIOME_MAX];
    uint8_t     temp_modifier[HC_BIOME_MAX];
    int32_t     count;
} hc_biome_reg_t;    /* HC_BIOME_MAX = 128 */
```
  Name for id: `reg.names[id]`. Intern/find: `hc_biome_intern` / `hc_biome_find` (hc_biome.h:47-49).
  In the harness the registry (`g_reg`) is populated from golden dump palettes + band golden
  (test_spawn_full.c:160,186,242).

## 4. Heightmaps

- FINAL 4 kinds, enum `hc_chunk.h:28-34`:
  `HC_HMF_OCEAN_FLOOR=0, HC_HMF_WORLD_SURFACE=1, HC_HMF_MOTION_BLOCKING=2, HC_HMF_MOTION_BLOCKING_NO_LEAVES=3`.
- Type: `int32_t heightmap_final[4][256]`, column index `hc_col_idx(x,z)=z*16+x`
  (i.e. row-major by z: col 0..15 = z=0 row). **Stored value = vanilla getFirstAvailable =
  highest opaque block y + 1 (world coords); empty column = HC_MIN_Y (-64)** (hc_chunk.h:25-27,
  features.c:58-99). So NBT 9-bit packed raw value = stored − (−64) (empty → 0) — pack rule
  itself is Task-A territory.
- Opaque predicates per kind: `hm_opaque` features.c:68-82 (OF: blocksMotion; WS: !isAir;
  MB: blocksMotion||fluidNonempty; MBNL: MB && !leaves).
- The `*_WG` maps (`heightmap_ws`, `heightmap_ocean_floor`, `heightmap_wg_reprimed`) are **not
  serialized** — FULL keeps client-4 only (gen_spawn_full_stages.c:10-17; gate asserts golden
  11_full has exactly the 4 kinds, test_spawn_full.c:865-874, `HM_CLIENT4` bitset :371).
- Vanilla NBT key names as used by the harness dump parser (`HM_NAMES`, test_spawn_full.c:366-369):
  `WORLD_SURFACE`, `OCEAN_FLOOR`, `MOTION_BLOCKING`, `MOTION_BLOCKING_NO_LEAVES` (+_WG variants).
- Primedness: after `hc_gen_features_chunk` on a chunk, all 4 are primed
  (`hc_feat_prime_final_maps`, gen_features_stage.c:38); gate asserts `(hm_final_primed & 0xF)==0xF`
  at 11_full (test_spawn_full.c:870-873). Helper `ensure_final_primed(rg,c)`
  (test_spawn_full.c:570-580) lazily primes via `hc_feat_height`.

## 5. Light — storage, nullability, "final fixed point"

`core/src/hc_light.h:30-54`:
```c
enum { HC_LIGHT_SEC_MIN = -5, HC_LIGHT_SEC_MAX = 20, HC_LIGHT_NSEC = 26 };
enum { HC_LIGHT_BLOCK = 0, HC_LIGHT_SKY = 1 };
typedef struct {
    hc_chunk_t *chunk;      /* NULL = 슬롯 비어 있음 */
    uint8_t    *light[2];   /* [ (sec-SEC_MIN)*4096 + ((y&15)<<8|(z&15)<<4|(x&15)) ] */
    uint32_t    registered; /* bit (sec - SEC_MIN): DataLayer 존재 (LIGHT_ONLY 포함) */
    int32_t     top;        /* topSections: max 등록 섹션 y + 1; HC_LIGHT_NO_TOP */
    uint8_t     feat_done, in_r, enabled;
    int32_t     src_y[256]; /* ChunkSkyLightSources.getLowestSourceY; hc_col_idx */
} hc_light_chunk_t;
typedef struct {
    hc_light_chunk_t *slots; /* [ (cz-cz0)*n + (cx-cx0) ] */
    int32_t cx0, cz0, n;  uint64_t *queue;  uint32_t qlog;
} hc_light_world_t;
```
Key facts for the serializer:
- Light lives **outside hc_chunk_t**, in a harness-owned `hc_light_world_t`. There is no light
  state persisted per chunk after `gen_light_stages.c` runs — the model is *re-solve from
  scratch per snapshot*: `hc_light_reset` → replay `hc_light_set_featured` +
  `hc_gen_initialize_light_stage` (= register) for every manifest entry so far, then
  `hc_gen_light_stage` (= enable) for eligible chunks, then **one** `hc_light_solve(w)`
  (see test_light_stages.c:1174-1205 for the exact eligibility replay).
- Per-cell storage is **one byte per block (0..15), not nibble-packed**:
  `s->light[layer][(sec+5)*4096 + ((y&15)<<8 | (z&15)<<4 | (x&15))]` (light_engine.c:125-135).
  The in-section linear index is identical to vanilla DataLayer index (y<<8|z<<4|x), so
  nibble-packing for NBT is `byte[j] = cell[2j] | cell[2j+1]<<4` (vanilla DataLayer: even index
  = low nibble — UNVERIFIED against this repo, repo has no nibble code; confirm in Task-A/golden).
- **Per-section presence (DataLayer null-ness)** = `registered` bitmask, bit `sec - (-5)`,
  covering sections -5..20 (26 bits). Set in `hc_light_solve` phase A by spilling ±1 section /
  ±1 chunk around every non-air section of every registered chunk (light_engine.c:358-387).
  `top` = max registered section + 1 (`HC_LIGHT_NO_TOP = INT32_MIN` if none).
- `hc_light_get` (light_engine.c:490-514) implements *visible snapshot* rules (sky above `top`
  → 15; unregistered gap → bottom slice of next layer above; block unregistered → 0). That is
  dump semantics, **not** raw serialization semantics — for NBT DataLayers read
  `s->light[layer]` + `registered` directly.
- Emission/opacity tables `g_damp[HC_B_COUNT]` / `g_emit[HC_B_COUNT]` are file-static in
  light_engine.c:36-79 (not exported).
- Attach allocates the two 26*4096-byte arrays from the arena (light_engine.c:241-257);
  `hc_light_world_init(w, arena, cx0, cz0, n)` light_engine.c:224-238.
- test_spawn_full.c builds **no** light world; a region gate that serializes light must clone
  the light replay from test_light_stages.c (world init :999, attach :1002, replay :1174-1205).

## 6. Features stage — where block writes & shape updates happen; tick hook sites

Region struct `core/src/hc_features.h:60-74`:
```c
enum { HC_FEAT_REGION_N = 11 };
typedef struct {
    hc_chunk_t *chunks[HC_FEAT_REGION_N * HC_FEAT_REGION_N]; /* [dz*n+dx] */
    int32_t     cx0, cz0, n;
    int32_t     center_cx, center_cz;
    int         wg_dropped;
} hc_feat_region_t;
```
Accessors (features.c):
- `hc_feat_region_chunk(rg,cx,cz)` :29-36 (asserts in-range).
- `hc_feat_get_block(rg,x,y,z)` :42-50 — y OOB = AIR.
- `hc_feat_set_block(rg,x,y,z,id)` :126-148 — **the single funnel for every feature write**
  (write window center±1 soft-fail; writes `c->states[hc_idx(...)]`; lazily primes + incrementally
  updates all 4 FINAL heightmaps). Ore bodies bypass it (BulkSectionAccess equivalence).
- `hc_feat_height(rg,hm_type,x,z)` :170-198 — hm_type enum `hc_features.h:99-107`
  (`HC_HM_OCEAN_FLOOR_WG=0, WORLD_SURFACE_WG=1, OCEAN_FLOOR=2, WORLD_SURFACE=3,
  MOTION_BLOCKING=4, MOTION_BLOCKING_NO_LEAVES=5`; FINAL index = type-2).
- `feat_env_t` (features_internal.h:8-23) carries `rg` to every feature body — all tick-relevant
  sites below can reach `e->rg` (and thus a recorder pointer added to `hc_feat_region_t`).

### Existing `tick` mentions (complete list, `grep -ri tick core/src`)
1. **features.c:580** (`spring_place`, :556-584): after `hc_feat_set_block(..., cfg->fluid_block)`
   at :579 — vanilla `level.scheduleTick(pos, fluid, 0)` → **fluid_ticks**. Comment: "scheduleTick
   → fluid_ticks NBT — 07 blocks/heightmaps 덤프 밖".
2. **features_ring.c:181-183** (lake PASS 2, :169-184): for grid cells `y>=4` after writing
   CAVE_AIR — vanilla `scheduleTick(pos, CAVE_AIR-block?, 0)` + `markAboveForPostProcessing`
   → **block_ticks + PostProcessing** ("@828-847, R5b §5").
   Also features_ring.c:202 (PASS 3 barrier): `markAboveForPostProcessing` only ("@1207-1211").
3. **features_ring.c:480-482** (geode crack branch): literal AIR write + *adjacent fluid
   scheduleTick* → **fluid_ticks** ("place@898-1011").
4. **features_tree.c:877 + 1058** (`edge_update_state`): dispatch implements only the
   *immediate* updateShape state changes; the default branch :1058-1059
   ("그 외 (잎/통나무/물/돌/흙/이끼 등): tick 스케줄만 — 불변") is exactly where vanilla
   `LeavesBlock.updateShape` returns `this` after `scheduleTick(pos, this, 1)` → **block_ticks
   (leaf distance recalc)**. Water reached here likewise schedules **fluid_ticks** (vanilla
   `BlockBehaviour.updateShape` → `FlowingFluid` tick) — UNVERIFIED which palette states other
   than leaves/water actually schedule; pin from golden mca inventory (Task-A/#2).

### updateShape execution order (needed for tick insertion order)
- Trigger: end of `hc_featx_tree_place` (features_tree.c:1212-1215) → `update_shape_at_edge(e,box)`.
- `update_shape_at_edge` (features_tree.c:1078-1133): forAllFaces = Z pass (rising NORTH=2 /
  falling SOUTH=3), then Y pass (DOWN=0/UP=1), then X pass (WEST=4/EAST=5) over the tree shape
  bitset box.
- Each face event `edge_face` (features_tree.c:1063-1075): reads s1(inside)/s2(neighbor), computes
  s3 = `edge_update_state(s1, dir)` (conditional write via `hc_feat_set_block`), then
  s4 = `edge_update_state(s2, OPP(dir), s3)` (conditional write). Vanilla schedules ticks inside
  these two updateShape calls — a recorder hooked in `edge_update_state` (or at its call sites in
  `edge_face`) reproduces vanilla scheduling order exactly.
- The `updateLeaves` distance relaxation itself (`update_leaves`, features_tree.c:754-833;
  leaf rewrite at :812 `hc_feat_set_block(e->rg, x, y, z, leaf_with_distance(cur, i))`) writes
  distances **directly** (vanilla setBlock flag 19 — no updateShape, no ticks from this pass).
- Suggested hook shape: add `struct hc_tick_rec *ticks;` (or two arrays + counts) to
  `hc_feat_region_t` (hc_features.h:62-74) — visible at all 4 sites via `e->rg`;
  record `{x,y,z, block-or-fluid id, delay, order-index}`; chunk assignment at serialization
  time via `x>>4, z>>4`. NULL pointer = recording off (keeps all existing gates unchanged).

## 7. 10_spawn / 11_full

`core/src/gen_spawn_full_stages.c:26-32`: `hc_gen_spawn_stage(c){c->promoted=10;}`,
`hc_gen_full_stage(c){c->promoted=11;}` — observationally pure; FULL semantics = *_WG maps cease
to exist, FINAL 4 survive bit-identically (header comment :10-22).

## 8. test_spawn_full.c replay harness (template for test_region.c)

File: `tests/parity/test_spawn_full.c` (1000 lines). Anatomy:
- **argv (7)**: `ref_dir stages_seed_dir stages_alt_seed_dir trace_seed_dir surface_golden band_golden seed`
  (:582-596; argv[5] unused, kept for CMake consistency).
- Static arena: `static unsigned char g_backing[640u << 20]; hc_arena_t g_arena` (:49-50);
  `read_file`/`parse_file` allocate from it (:52-85).
- reference/ loading: `load_tree` recursion over `density_function`, `noise`, `tags/block`,
  `placed_feature`, `configured_feature` into `hc_df_source_t` tables (:127-153, :601-614);
  `overworld-26.2.json` router (15 slots) + `sea_level` (:616-676); carver JSONs (:625-635);
  `hc_surface_init` (:678-682); band biome grid 52x52 quarts, chunks −6..6
  (`QG_MIN_XZ=-24, QG_NXZ=52, QG_MIN_Y=-16, QG_NY=96`, :157, `load_band_grid` :162-209);
  `features_order-26.2.txt` + `biome_features-26.2.json` → `hc_feat_reg_init(walk_max_step=10)`
  (:686-697); `load_climate` (:294-313).
- **World**: `enum { WR=5, WN=11, WORLD_CHUNKS=121 }` chunks −5..5 (:546);
  `world_t { hc_chunk_t chunks[121]; uint16_t *pristine[121]; }` (:548-552); `widx(cx,cz)`
  (:555-558). Every chunk is chained through our 04..06
  (`hc_gen_noise_stage` → `hc_gen_surface_stage` → `hc_gen_carvers_stage`, :711-752), biomes from
  golden 03 dumps (grid + ring2) or band grid; post-06 states snapshotted into `pristine`.
- **Region**: one `hc_feat_region_t rg` over the whole 11x11 (:757-765).
- **Golden dir discovery**: none — fixed paths `"%s/c.%d.%d/<stage>.<kind>.txt"` under the bundle
  dir; manifest `"%s/order.manifest"`, snapshots `"%s/order.snapshots"` (:777-780).
- Manifest line: `seq cx cz seedhex thread nanos` (:439); loader cross-checks
  `hc_features_decoration_seed` (:449-452). Snapshot line: `stage cx cz seqBegin seqEnd thread
  nanos`, filtered to 10_spawn/11_full, sorted by nanos (:467-514).
- **Replay loop** (:807-983): for pos 0..max_prefix — first consume snapshots with
  `seq_begin == pos` (run stage marker, compare blocks/biomes/heightmaps dumps), then
  `if (pos == 9) rg.wg_dropped = 1;` and `if (pos < max_prefix) hc_gen_features_chunk(&rg,
  man[pos].cx, man[pos].cz, seed, freg, &view, &g_reg, sea, 10, &g_guard_sink);`.
- **State at END of a bundle**: features replayed through manifest entry `max_prefix-1`
  (max_prefix = largest snapshot seq_begin — the harness does NOT replay the whole manifest tail);
  all 18 snapshots consumed (`assert(next_snap == n_snap)` :984); the 9 grid chunks have
  promoted=11 and all FINAL maps primed; chunk states equal the 11_full goldens except the
  **inherited 09 residual chunks** (RESID table :939-949: primary (-1,-1) caps{1,1},
  (0,-1){78,39}, (1,-1){11,10}; alt similarly; `HC_LIGHT_STRICT=1` env forces strict 0-diff).
  Region r.0.0 covers cx,cz in [0,31] → of the dumped grid only **(0,0),(1,0),(0,1),(1,1)** —
  all four are 0-diff chunks (no RESID entry), so the byte-exact region gate is not polluted
  by the documented residuals. NOTE: chunks are compared *at their snapshot seq*, and the four
  region chunks' last observation is their 11_full snapshot; replay continues past some of them
  (later manifest entries can decorate neighbors and write into ring around center±1) — the mca
  was saved by the recording server at *its* save time. Use the golden mca inventory (Task #2) to
  decide whether "state at snapshot seq" or "state at end of replay" matches; the harness can
  serialize at the 11_full snapshot moment for each chunk if needed (hook inside the snapshot
  consumption block :808-973).
- Trace sink `g_guard_sink` (:518-542) fail-louds if an UNIMPLEMENTED body fires.

## 9. Build & compliance

- Top CMakeLists (`CMakeLists.txt:11-16`): global `add_compile_options(-O2 -ffp-contract=off
  -fno-fast-math -Wall -Wextra -Werror)`, C11, extensions OFF; `HC_SANITIZE` option adds
  ASan+UBSan+float-cast-overflow (:24-29). Presets: `build/` release, `build-asan/`.
- New core sources: append to `add_library(hyperchunk STATIC ...)` list in
  `core/CMakeLists.txt:1-38`. Only link dep is `m` (:42) — **ADR-003 D1: no third-party libs**;
  repo contains zero NBT/zlib/deflate/crc32 code today (grep verified) → Task-12 writes its own
  stored-block DEFLATE + zlib container (adler32) and CRC-less anvil bits in-tree.
- Tests: `tests/CMakeLists.txt` pattern — `add_executable(test_x parity/test_x.c)`;
  `target_link_libraries(test_x PRIVATE hyperchunk)`; `add_test(NAME x COMMAND test_x
  ${CMAKE_SOURCE_DIR}/reference ${CMAKE_SOURCE_DIR}/golden/... 1234567890)`; heavy replays get
  `set_tests_properties(x PROPERTIES TIMEOUT 1200)` (:124,:138). spawn_full registration
  :128-138 is the model for `test_region`.
- `scripts/check_no_fma.sh`: runs on `build/core/libhyperchunk.a` only (arg-overridable);
  requires elf64-x86-64 members, **no `.gnu.lto` sections** (no -flto), zero
  `vfmadd|vfmsub|vfnmadd|vfnmsub` mnemonics, and >0 disassembled instructions. Integer-only
  serializer code is trivially compliant; do not add LTO/IPO flags.
- `scripts/check_sanitizers.sh`: `cmake --preset asan-ubsan`, build, full
  `ctest --test-dir build-asan --no-tests=error` with `halt_on_error=1` — new test must pass
  under ASan/UBSan (mind the big static buffers pattern used by existing tests; arena backing is
  a file-scope static, fine).

## 10. sha256 + arena

- `core/src/hc_sha256.h:14`: `void hc_sha256(const void *data, size_t len, uint8_t out[32]);`
  plain FIPS 180-4 (sha256.c:47-74) — internal core/src header, but parity tests already include
  core/src headers relatively (test_spawn_full.c:29-31 includes `../../core/src/hc_sha256.h`), so
  test_region can hash decompressed chunk payloads with it directly.
- Arena (`core/include/hc_arena.h:19-28`): `hc_arena_t {base,cap,off}`;
  `hc_arena_init(a, backing, cap)` — **caller supplies backing** (core never mallocs, ADR-003 D3);
  `hc_arena_alloc(a, n, align)` returns align-multiple address or NULL on exhaustion (no abort);
  no free — `hc_arena_reset` rewinds wholesale and does NOT zero (consumers must clear, cf.
  hc_chunk_init). Convention for serializer output: caller passes `hc_arena_t*`; core function
  allocates the output buffer from it and returns pointer+len (or writes into caller buffer with
  cap and returns -1 on overflow — either matches house style; NULL/-1 on exhaustion, never abort
  for capacity).

## 11. Quick-reference: post-replay accessors for chunk (cx,cz)

```c
hc_chunk_t *c = hc_feat_region_chunk(&rg, cx, cz);      /* or &g_world.chunks[widx(cx,cz)] */
uint16_t id  = c->states[hc_idx(lx, y, lz)];            /* y world, lx/lz local */
const char *state_str = hc_block_name(id);              /* "ns:name[k=v,...]" alphabetical */
uint16_t bid = c->biomes[hc_quart_idx(qx, qy, qz)];     /* qy world quart */
const char *biome = g_reg.names[bid];
int32_t hm = c->heightmap_final[HC_HMF_MOTION_BLOCKING][hc_col_idx(lx, lz)]; /* y+1; empty -64 */
/* light (needs light world replay, test_light_stages.c:999-1205): */
hc_light_chunk_t *s = &lw.slots[(cz - lw.cz0)*lw.n + (cx - lw.cx0)];
int present = (s->registered >> (sec - HC_LIGHT_SEC_MIN)) & 1;   /* sec in [-5,20] */
uint8_t v = s->light[HC_LIGHT_SKY][(sec+5)*4096 + ((y&15)<<8 | (lz&15)<<4 | (lx&15))];
```

## Open items / UNVERIFIED

- Nibble order of vanilla DataLayer packing (even index = low nibble) — standard vanilla, but not
  represented anywhere in this repo; confirm against golden mca bytes.
- Whether vanilla serializes a *registered but all-zero* light section as a present DataLayer or
  skips it (SerializableChunkData omits null layers; ours distinguishes only registered/not) —
  empirically decide from golden r.0.0.mca.
- Which non-leaf palette states scheduled ticks in the golden run (water via updateShape at tree
  edges? lake CAVE_AIR block ticks?) — enumerate from the golden mca block_ticks/fluid_ticks lists
  (Task #2) before wiring the recorder, then assert recorder output == inventory.
- Properties compound key order in vanilla NBT output vs. our alphabetical strings — Task-A rule.
- `hc_light_get` visible-snapshot semantics vs raw arrays: serializer must use raw arrays;
  dumps used visible semantics — do not reuse `compare_light` logic for NBT.
