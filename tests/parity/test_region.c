/* 리전 출력 게이트 (Task 12 도입, Task 13-close 에서 4/4 strict 승격).
 *
 * 04..06 체인 → primary 번들 order.manifest 재생 (리전 전체 manifest,
 * |c|<=WR-1 창 적용; 틱 레코더 켬) → postProcessGeneration 승격 드레인
 * (golden postprocess.manifest 순서; 유도 마킹 교차검증 fail-loud) →
 * 라이트 최종 고정점 → r.0.0 ∩ 그리드 4청크 ((0,0),(1,0),(0,1),(1,1))
 * 를 26.2 청크 NBT 로 직렬화, golden .mca 에서 추출한 canonical 참조
 * 페이로드(LastUpdate 마스킹, tools/golden/extract_region_ref.py)와
 * 바이트 대조한다. 4/4 바이트 일치가 무조건 게이트다.
 *
 * 스펙/근거: .hermes/notes/task12-region/R-A..R-E, task13-close 완료
 * 노트. 골든 = Task-13 unified 단일-세션 재캡처 (mca+manifest+dumps
 * 코히런트, noSave — 교차검증 tools/golden/logs/coherence-primary3.log).
 * 구 stale-mca 잔차 규율 (07-28 mca vs 07-31 번들) 은 재캡처로 소멸 —
 * git 이력 ec7e23d 의 헤더 참조.
 *
 * 마지막에 4청크 .mca 이미지를 hc_region_write 로 조립해 argv 경로에
 * 쓴다 — ctest region_out_roundtrip/residuals 가 소비. */

#undef NDEBUG

#include "../../core/src/hc_carvers.h"
#include "../../core/src/hc_chunk_nbt.h"
#include "../../core/src/hc_features.h"
#include "../../core/src/hc_light.h"
#include "../../core/src/hc_postprocess.h"
#include "../../core/src/hc_region.h"
#include "../../core/src/hc_sha256.h" /* hc_biome_obfuscate_seed */

#include <assert.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails = 0;

static void die(const char *msg, const char *detail) {
    fprintf(stderr, "GOLDEN/SETUP ERROR: %s%s%s\n", msg, detail ? ": " : "",
            detail ? detail : "");
    exit(2);
}

static unsigned char g_backing[768u << 20];
static hc_arena_t    g_arena;

static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot open", path);
    if (fseek(f, 0, SEEK_END) != 0)
        die("seek failed", path);
    long sz = ftell(f);
    if (sz < 0)
        die("tell failed", path);
    rewind(f);
    char *buf = hc_arena_alloc(&g_arena, (size_t)sz + 1, 1);
    if (!buf)
        die("arena exhausted reading", path);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz)
        die("short read", path);
    fclose(f);
    buf[sz] = '\0';
    if (len_out)
        *len_out = (size_t)sz;
    return buf;
}

static const hc_json_t *parse_file(const char *path) {
    char       *buf = read_file(path, NULL);
    const char *err = NULL;
    size_t      pos = 0;
    hc_json_t  *v = hc_json_parse(buf, &g_arena, &err, &pos);
    if (!v) {
        fprintf(stderr, "JSON parse error in %s at %zu: %s\n", path, pos,
                err ? err : "?");
        exit(2);
    }
    return v;
}

/* --- reference/ 로더 (test_spawn_full.c 와 동일 규약; 복사본) --- */

#define MAX_SOURCES 64
#define MAX_FEAT_SOURCES 256

static hc_df_source_t g_dfs[MAX_SOURCES];
static int32_t        g_n_dfs = 0;
static hc_df_source_t g_noises[MAX_SOURCES];
static int32_t        g_n_noises = 0;
static hc_df_source_t g_tags[MAX_SOURCES];
static int32_t        g_n_tags = 0;
static hc_df_source_t g_placed[MAX_FEAT_SOURCES];
static int32_t        g_n_placed = 0;
static hc_df_source_t g_configured[MAX_FEAT_SOURCES];
static int32_t        g_n_configured = 0;

static int has_suffix(const char *s, const char *suf) {
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

static void add_source(hc_df_source_t *tab, int32_t *n, int32_t cap,
                       const char *dir, const char *rel_prefix,
                       const char *fname) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, fname);
    char *name = hc_arena_alloc(
        &g_arena, 10 + strlen(rel_prefix) + strlen(fname) + 1, 1);
    if (!name)
        die("arena exhausted (source name)", fname);
    size_t stem = strlen(fname) - 5;
    sprintf(name, "minecraft:%s%.*s", rel_prefix, (int)stem, fname);
    if (*n >= cap)
        die("too many reference sources", name);
    tab[*n].name = name;
    tab[*n].json = parse_file(path);
    (*n)++;
}

static void load_tree(hc_df_source_t *tab, int32_t *n, int32_t cap,
                      const char *dir, const char *rel_prefix, int depth) {
    DIR *d = opendir(dir);
    if (!d)
        die("cannot open reference dir", dir);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char sub[1024];
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        DIR *probe = opendir(sub);
        if (probe) {
            closedir(probe);
            if (depth >= 2)
                die("reference tree deeper than expected", sub);
            char pref[512];
            snprintf(pref, sizeof pref, "%s%s/", rel_prefix, e->d_name);
            load_tree(tab, n, cap, sub, pref, depth + 1);
        } else if (has_suffix(e->d_name, ".json")) {
            add_source(tab, n, cap, dir, rel_prefix, e->d_name);
        } else {
            die("unexpected file in reference tree", sub);
        }
    }
    closedir(d);
}

/* --- 쿼트 바이옴 그리드 (52x52, 청크 -6..6) --- */

enum { QG_MIN_XZ = -24, QG_NXZ = 52, QG_MIN_Y = -16, QG_NY = 96 };

static uint16_t       g_grid[QG_NY][QG_NXZ][QG_NXZ];
static hc_biome_reg_t g_reg;

static void load_band_grid(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        die("cannot open biome band golden", path);
    char    line[1024];
    int     in_section = 0, cur_qy = -9999, cur_qz = 0;
    int32_t pal[64];
    int     n_pal = 0, rows = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "section quart_biomes", 20) == 0) {
            in_section = 1;
            continue;
        }
        if (strncmp(line, "section", 7) == 0) {
            in_section = 0;
            continue;
        }
        if (!in_section)
            continue;
        int  idx;
        char name[64];
        if (sscanf(line, "palette %d %63s", &idx, name) == 2) {
            if (idx != n_pal || n_pal >= 64)
                die("band palette out of order", path);
            pal[n_pal] = hc_biome_intern(&g_reg, name, (int32_t)strlen(name));
            if (pal[n_pal] < 0)
                die("biome registry full", name);
            n_pal++;
        } else if (sscanf(line, "qy %d", &cur_qy) == 1) {
            cur_qz = 0;
        } else if (cur_qy != -9999) {
            const char *p = line;
            for (int qx = 0; qx < QG_NXZ; qx++) {
                char *end;
                long  v = strtol(p, &end, 10);
                if (end == p || v < 0 || v >= n_pal)
                    die("bad band quart row", path);
                g_grid[cur_qy - QG_MIN_Y][cur_qz][qx] = (uint16_t)pal[v];
                p = end;
            }
            cur_qz++;
            rows++;
        }
    }
    fclose(f);
    if (rows != QG_NY * QG_NXZ)
        die("band quart_biomes incomplete", path);
}

static int g_band_quart_diffs = 0;

static void store_biomes_dump(hc_chunk_t *chunk, const char *path) {
    size_t len = 0;
    char  *buf = read_file(path, &len);
    if (len < 100 || strncmp(buf, "# hyperchunk golden stage dump v1\n", 34))
        die("golden biomes malformed", path);
    int32_t pal[64];
    int     n_pal = 0;
    char   *p = buf;
    int     in_data = 0;
    int     qy = HC_MIN_Y >> 2, qz = 0;
    int     quads = 0;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (!nl)
            break;
        *nl = '\0';
        if (p[0] == '#') {
        } else if (strncmp(p, "palette ", 8) == 0) {
            int  idx;
            char name[64];
            if (sscanf(p, "palette %d %63s", &idx, name) != 2 ||
                idx != n_pal || n_pal >= 64)
                die("bad biome palette line", path);
            pal[n_pal] = hc_biome_intern(&g_reg, name, (int32_t)strlen(name));
            if (pal[n_pal] < 0)
                die("biome registry full", name);
            n_pal++;
        } else if (strcmp(p, "data") == 0) {
            in_data = 1;
        } else if (in_data) {
            const char *q = p;
            for (int qx = 0; qx < 4; qx++) {
                char *end;
                long  v = strtol(q, &end, 10);
                if (end == q || v < 0 || v >= n_pal)
                    die("bad biome data row", path);
                uint16_t id = (uint16_t)pal[v];
                int gx = chunk->cx * 4 + qx - QG_MIN_XZ;
                int gz = chunk->cz * 4 + qz - QG_MIN_XZ;
                if (g_grid[qy - QG_MIN_Y][gz][gx] != id) {
                    g_band_quart_diffs++;
                    g_grid[qy - QG_MIN_Y][gz][gx] = id;
                }
                chunk->biomes[hc_quart_idx(qx, qy, qz)] = id;
                q = end;
                quads++;
            }
            if (++qz == 4) {
                qz = 0;
                qy++;
            }
        }
        p = nl + 1;
    }
    if (quads != HC_QUARTS)
        die("biome dump incomplete", path);
}

static void fill_chunk_biomes_from_grid(hc_chunk_t *chunk) {
    for (int qy = HC_MIN_Y >> 2; qy <= HC_MAX_Y >> 2; qy++)
        for (int qz = 0; qz < 4; qz++)
            for (int qx = 0; qx < 4; qx++) {
                int gx = chunk->cx * 4 + qx - QG_MIN_XZ;
                int gz = chunk->cz * 4 + qz - QG_MIN_XZ;
                chunk->biomes[hc_quart_idx(qx, qy, qz)] =
                    g_grid[qy - QG_MIN_Y][gz][gx];
            }
}

static void load_climate(const char *ref_dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/biome_climate-26.2.json", ref_dir);
    const hc_json_t *tbl = parse_file(path);
    if (tbl->kind != HC_JSON_OBJ || tbl->count < 60)
        die("biome climate table malformed", path);
    for (int32_t id = 0; id < g_reg.count; id++) {
        const hc_json_t *e = hc_json_get(tbl, g_reg.names[id]);
        if (!e || e->kind != HC_JSON_OBJ)
            die("biome missing from climate table", g_reg.names[id]);
        const hc_json_t *t = hc_json_get(e, "temperature");
        const hc_json_t *m = hc_json_get(e, "temperature_modifier");
        if (!t || t->kind != HC_JSON_NUM)
            die("biome climate missing temperature", g_reg.names[id]);
        uint8_t mod = HC_BIOME_TEMP_MOD_NONE;
        if (m && m->kind == HC_JSON_STR && hc_json_streq(m, "frozen"))
            mod = HC_BIOME_TEMP_MOD_FROZEN;
        hc_biome_set_climate(&g_reg, id, (float)t->num, mod);
    }
}

/* --- order.manifest --- */

typedef struct {
    int32_t  cx, cz;
    uint64_t seed_hex;
} manifest_line_t;

static int32_t load_manifest(const char *path, int64_t level_seed,
                             manifest_line_t *out, int32_t cap) {
    size_t len = 0;
    char  *buf = read_file(path, &len);
    char  *p = buf;
    int32_t n = 0;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (!nl)
            break;
        *nl = '\0';
        if (p[0] != '#' && p[0] != '\0') {
            long long          seq;
            int32_t            cx, cz;
            unsigned long long hex;
            char               thread[64];
            long long          nanos;
            if (sscanf(p, "%lld %d %d %llx %63s %lld", &seq, &cx, &cz, &hex,
                       thread, &nanos) != 6)
                die("bad manifest line", p);
            if (seq != n)
                die("manifest seq gap", path);
            if (n >= cap)
                die("manifest longer than expected", path);
            out[n].cx = cx;
            out[n].cz = cz;
            out[n].seed_hex = (uint64_t)hex;
            if ((uint64_t)hc_features_decoration_seed(level_seed, cx, cz) !=
                out[n].seed_hex)
                die("decoration seed mismatch", p);
            n++;
        }
        p = nl + 1;
    }
    return n;
}

/* --- fail-loud 트레이스 싱크 --- */

static void sink_pos_nop(void *ud, int32_t step, int32_t index, int32_t x,
                         int32_t y, int32_t z, int32_t placed) {
    (void)ud;
    (void)step;
    (void)index;
    (void)x;
    (void)y;
    (void)z;
    (void)placed;
}

static void sink_feature_guard(void *ud, int32_t step, int32_t index,
                               const char *name, int32_t npos,
                               int32_t placed) {
    (void)ud;
    if (npos > 0 && placed < 0) {
        char buf[192];
        snprintf(buf, sizeof buf, "%s (step %d index %d, npos %d)", name,
                 step, index, npos);
        die("UNIMPLEMENTED feature body fired in replay", buf);
    }
}

static const hc_feat_trace_t g_guard_sink = {sink_pos_nop,
                                             sink_feature_guard, NULL};

/* --- 월드 --- */

enum { WR = 5, WN = 2 * WR + 1, WORLD_CHUNKS = WN * WN };

typedef struct {
    hc_chunk_t chunks[WORLD_CHUNKS];
} world_t;

static world_t g_world;

static int widx(int32_t cx, int32_t cz) {
    assert(cx >= -WR && cx <= WR && cz >= -WR && cz <= WR);
    return (cz + WR) * WN + (cx + WR);
}

static int is_grid(int32_t cx, int32_t cz) {
    return cx >= -1 && cx <= 1 && cz >= -1 && cz <= 1;
}
static int is_ring2(int32_t cx, int32_t cz) {
    int32_t r = abs(cx) > abs(cz) ? abs(cx) : abs(cz);
    return r == 2;
}

/* --- 게이트 대상: r.0.0 ∩ 그리드 --- */
static const int32_t GATE[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};

int main(int argc, char **argv) {
    if (argc != 8) {
        fprintf(stderr,
                "usage: test_region <ref_dir> <stages_seed_dir> "
                "<trace_seed_dir> <band_golden> <seed> <region_ref_dir> "
                "<out_mca_path>\n");
        return 2;
    }
    const char *ref_dir = argv[1];
    const char *stages_dir = argv[2];
    const char *trace_dir = argv[3];
    const char *band_golden = argv[4];
    int64_t     seed = strtoll(argv[5], NULL, 10);
    const char *region_ref_dir = argv[6];
    const char *out_mca_path = argv[7];

    hc_arena_init(&g_arena, g_backing, sizeof g_backing);
    hc_biome_reg_init(&g_reg, &g_arena);

    char sub[1024];
    snprintf(sub, sizeof sub, "%s/density_function", ref_dir);
    load_tree(g_dfs, &g_n_dfs, MAX_SOURCES, sub, "", 0);
    snprintf(sub, sizeof sub, "%s/noise", ref_dir);
    load_tree(g_noises, &g_n_noises, MAX_SOURCES, sub, "", 0);
    snprintf(sub, sizeof sub, "%s/tags/block", ref_dir);
    load_tree(g_tags, &g_n_tags, MAX_SOURCES, sub, "", 0);
    snprintf(sub, sizeof sub, "%s/placed_feature", ref_dir);
    load_tree(g_placed, &g_n_placed, MAX_FEAT_SOURCES, sub, "", 0);
    snprintf(sub, sizeof sub, "%s/configured_feature", ref_dir);
    load_tree(g_configured, &g_n_configured, MAX_FEAT_SOURCES, sub, "", 0);
    if (g_n_dfs < 19 || g_n_noises < 51 || g_n_tags < 30 ||
        g_n_placed < 200 || g_n_configured < 160)
        die("reference closure incomplete", ref_dir);

    snprintf(sub, sizeof sub, "%s/overworld-26.2.json", ref_dir);
    const hc_json_t *settings = parse_file(sub);
    const hc_json_t *router = hc_json_get(settings, "noise_router");
    if (!router || router->kind != HC_JSON_OBJ || router->count != 15)
        die("noise_router missing", sub);
    const hc_json_t *sea = hc_json_get(settings, "sea_level");
    if (!sea || sea->kind != HC_JSON_NUM)
        die("sea_level missing", sub);

    static const char *CARVER_FILES[3] = {"cave", "cave_extra_underground",
                                          "canyon"};
    hc_carver_t        carvers[3];
    for (int i = 0; i < 3; i++) {
        snprintf(sub, sizeof sub, "%s/carver/%s.json", ref_dir,
                 CARVER_FILES[i]);
        const char *err = NULL;
        if (hc_carver_init(&carvers[i], parse_file(sub), g_tags, g_n_tags,
                           &err) != 0)
            die(err ? err : "carver compile failed", sub);
    }

    hc_df_compiler_t comp;
    hc_df_graph_t    graph;
    if (hc_df_compiler_init(&comp, &graph, &g_arena, seed, g_dfs, g_n_dfs,
                            g_noises, g_n_noises) != 0)
        die("compiler init failed (arena)", NULL);

    hc_noise_roots_t roots;
    memset(&roots, -1, sizeof roots);
    struct {
        const char *name;
        int32_t    *dst;
    } slot_map[] = {
        {"final_density", &roots.final_density},
        {"barrier", &roots.barrier},
        {"fluid_level_floodedness", &roots.fluid_level_floodedness},
        {"fluid_level_spread", &roots.fluid_level_spread},
        {"lava", &roots.lava},
        {"erosion", &roots.erosion},
        {"depth", &roots.depth},
        {"preliminary_surface_level", &roots.preliminary_surface_level},
        {"vein_toggle", &roots.vein_toggle},
        {"vein_ridged", &roots.vein_ridged},
        {"vein_gap", &roots.vein_gap},
    };
    for (const hc_json_t *m = router->child; m; m = m->next) {
        char key[128];
        if (m->klen >= (int32_t)sizeof key)
            die("slot name too long", NULL);
        memcpy(key, m->key, (size_t)m->klen);
        key[m->klen] = '\0';
        int32_t root = hc_df_compile_expr(&comp, m);
        if (root < 0)
            die("router slot compile failed", key);
        for (size_t i = 0; i < sizeof slot_map / sizeof slot_map[0]; i++)
            if (strcmp(slot_map[i].name, key) == 0)
                *slot_map[i].dst = root;
    }
    for (size_t i = 0; i < sizeof slot_map / sizeof slot_map[0]; i++)
        if (*slot_map[i].dst < 0)
            die("router slot missing from JSON", slot_map[i].name);

    hc_surface_t *surf =
        hc_arena_alloc(&g_arena, sizeof *surf, _Alignof(hc_surface_t));
    if (!surf || hc_surface_init(surf, &g_arena, seed, settings, g_noises,
                                 g_n_noises, &g_reg) != 0)
        die("surface init failed", surf ? surf->err : "arena");

    load_band_grid(band_golden);

    snprintf(sub, sizeof sub, "%s/features_order-26.2.txt", ref_dir);
    char *order_txt = read_file(sub, NULL);
    snprintf(sub, sizeof sub, "%s/biome_features-26.2.json", ref_dir);
    const hc_json_t *biome_features = parse_file(sub);

    hc_feat_reg_t *freg =
        hc_arena_alloc(&g_arena, sizeof *freg, _Alignof(hc_feat_reg_t));
    const char *ferr = NULL;
    if (!freg ||
        hc_feat_reg_init(freg, &g_arena, order_txt, biome_features, g_placed,
                         g_n_placed, g_configured, g_n_configured, g_tags,
                         g_n_tags, &g_reg, /*walk_max_step=*/10, &ferr) != 0)
        die(ferr ? ferr : "feature registry init failed", NULL);

    load_climate(ref_dir);

    /* --- 월드 구성: 11x11 전부 04..06 체인 --- */
    hc_biome_view_t view;
    view.qx0 = QG_MIN_XZ;
    view.qz0 = QG_MIN_XZ;
    view.nxz = QG_NXZ;
    view.qy0 = QG_MIN_Y;
    view.ny = QG_NY;
    view.ids = &g_grid[0][0][0];
    view.zoom_seed = hc_biome_obfuscate_seed(seed);

    static hc_ppg_recorder_t ppg;
    if (hc_ppg_recorder_init(&ppg, &g_arena, 1 << 16) != 0)
        die("arena exhausted (ppg recorder)", NULL);
    for (int32_t cz = -WR; cz <= WR; cz++)
        for (int32_t cx = -WR; cx <= WR; cx++) {
            hc_chunk_t *c = &g_world.chunks[widx(cx, cz)];
            if (hc_chunk_init(c, &g_arena, cx, cz) != 0)
                die("arena exhausted (chunk)", NULL);
            c->ppg = &ppg; /* Task 13: postProcess 마킹 수집 */
            char bpath[1024];
            if (is_grid(cx, cz)) {
                snprintf(bpath, sizeof bpath,
                         "%s/c.%d.%d/03_biomes.biomes.txt", stages_dir, cx,
                         cz);
                store_biomes_dump(c, bpath);
            } else if (is_ring2(cx, cz)) {
                snprintf(bpath, sizeof bpath,
                         "%s/c.%d.%d/03_biomes.biomes.txt", trace_dir, cx,
                         cz);
                store_biomes_dump(c, bpath);
            }
        }
    if (g_band_quart_diffs > 8)
        die("too many band-vs-stored quart diffs — probe drift?", NULL);
    for (int32_t cz = -WR; cz <= WR; cz++)
        for (int32_t cx = -WR; cx <= WR; cx++) {
            hc_chunk_t *c = &g_world.chunks[widx(cx, cz)];
            if (!is_grid(cx, cz) && !is_ring2(cx, cz))
                fill_chunk_biomes_from_grid(c);
            size_t            arena_mark = g_arena.off;
            hc_noise_chunk_t *nc = hc_arena_alloc(
                &g_arena, sizeof *nc, _Alignof(hc_noise_chunk_t));
            if (!nc || hc_nc_init(nc, &g_arena, &graph, &roots, seed, cx, cz,
                                  (int32_t)sea->num) != 0)
                die("arena exhausted (noise chunk)", NULL);
            hc_gen_noise_stage(c, nc);
            hc_gen_surface_stage(c, nc, surf, &view);
            static uint64_t mask[HC_CARVING_MASK_WORDS];
            memset(mask, 0, sizeof mask);
            hc_gen_carvers_stage(c, nc, surf, &view, seed, carvers, 3, mask);
            g_arena.off = arena_mark;
        }
    printf("world: %d chunks chained 04..06 (band quart overlay diffs: %d)\n",
           WORLD_CHUNKS, g_band_quart_diffs);

    /* features 리전 + 틱 레코더 */
    hc_feat_region_t rg;
    memset(&rg, 0, sizeof rg);
    rg.cx0 = -WR;
    rg.cz0 = -WR;
    rg.n = WN;
    for (int32_t cz = -WR; cz <= WR; cz++)
        for (int32_t cx = -WR; cx <= WR; cx++)
            rg.chunks[(cz + WR) * WN + (cx + WR)] =
                &g_world.chunks[widx(cx, cz)];
    static hc_tick_recorder_t recorder;
    if (hc_tick_recorder_init(&recorder, &g_arena, 1 << 17) != 0)
        die("arena exhausted (tick recorder)", NULL);
    rg.ticks = &recorder;

    /* --- primary 번들 manifest 전체 재생 --- */
    enum { MAX_MANIFEST = 4096 };
    static manifest_line_t man[MAX_MANIFEST];
    char mpath[1024];
    snprintf(mpath, sizeof mpath, "%s/order.manifest", stages_dir);
    int32_t n_man = load_manifest(mpath, seed, man, MAX_MANIFEST);
    if (n_man < 81)
        die("manifest unexpectedly short", mpath);

    /* Task 13 unified bundle: noSave 캡처라 저장/언로드 웨이브가 없다 —
     * wg_dropped 모델링 제거. manifest 는 리전 전체 (1461 엔트리) 를
     * 기록하므로 11x11 월드 밖 엔트리는 스킵한다. 이 필터가 게이트
     * 4청크에 대해 EXACT 함은 캡처 시 실측 — 게이트 영향원뿔 (-2..3)^2
     * 의 모든 엔트리보다 앞서 데코된 창밖 청크가 없다
     * (tools/golden/check_capture_coherence.py check 7). */
    for (int32_t pos = 0; pos < n_man; pos++) {
        if (man[pos].cx < -(WR - 1) || man[pos].cx > WR - 1 ||
            man[pos].cz < -(WR - 1) || man[pos].cz > WR - 1)
            continue;
        hc_gen_features_chunk(&rg, man[pos].cx, man[pos].cz, seed, freg,
                              &view, &g_reg, (int32_t)sea->num, 10,
                              &g_guard_sink);
    }
    printf("replayed %d manifest entries; recorded %d scheduled ticks, "
           "%d postprocess marks\n",
           n_man, recorder.n, ppg.n);

    /* --- Task 13: postProcessGeneration (승격 드레인) ---
     * golden postprocess.manifest 의 승격 순서를 |c|<=3 창으로 재생.
     * 게이트 4청크의 바이트에 닿을 수 있는 드레인은 (-1..2)^2 뿐이지만
     * (쓰기 ±1 / 스케줄 ±2), 마킹이 완전한 (라이터 |c|<=4 전부 재생됨)
     * |c|<=3 전체를 드레인해 커버리지를 넓힌다. 드레인 전에 우리가
     * 유도한 마킹을 기록된 마킹 (postprocess.manifest 'p' 라인) 과
     * 좌표·순서·중복까지 교차검증한다 — 마킹 생산자 버그 fail-loud. */
    ppg.frozen = 1; /* 라이브 단계: LevelChunk.markPos = no-op */
    enum { MAX_PPM = 2048, PP_WIN = 3 };
    static int32_t ppm_cx[MAX_PPM], ppm_cz[MAX_PPM];
    int32_t        n_ppm = 0;
    {
        char ppath[1024];
        snprintf(ppath, sizeof ppath, "%s/postprocess.manifest", stages_dir);
        size_t len = 0;
        char  *buf = read_file(ppath, &len);
        char  *p = buf;
        /* seq → 창 내 여부 (p 라인 매칭용) */
        static int8_t seq_in_win[MAX_PPM * 4];
        memset(seq_in_win, 0, sizeof seq_in_win);
        /* 기록 마킹 (창 내): 청크별 검증용 평면 리스트 */
        enum { MAX_PPP = 1 << 16 };
        static struct {
            int32_t seq, x, y, z;
        } rec_pos[MAX_PPP];
        int32_t n_rec = 0;
        static int32_t seq_cx[MAX_PPM * 4], seq_cz[MAX_PPM * 4];
        while (*p) {
            char *nl = strchr(p, '\n');
            if (!nl)
                break;
            *nl = '\0';
            if (p[0] == 'p' && p[1] == ' ') {
                long long seq, k, secy;
                int32_t   x, y, z;
                if (sscanf(p + 2, "%lld %lld %lld %d %d %d", &seq, &k, &secy,
                           &x, &y, &z) != 6)
                    die("bad postprocess position line", p);
                if (seq_in_win[seq]) {
                    if (n_rec >= MAX_PPP)
                        die("too many recorded marks", ppath);
                    rec_pos[n_rec].seq = (int32_t)seq;
                    rec_pos[n_rec].x = x;
                    rec_pos[n_rec].y = y;
                    rec_pos[n_rec].z = z;
                    n_rec++;
                }
            } else if (p[0] != '#' && p[0] != '\0') {
                long long seq, gt, nm;
                int32_t   cx, cz;
                char      thread[64];
                long long nanos;
                if (sscanf(p, "%lld %d %d %lld %lld %63s %lld", &seq, &cx,
                           &cz, &gt, &nm, thread, &nanos) != 7)
                    die("bad postprocess chunk line", p);
                if (seq >= MAX_PPM * 4)
                    die("postprocess manifest too long", ppath);
                seq_cx[seq] = cx;
                seq_cz[seq] = cz;
                if (cx >= -PP_WIN && cx <= PP_WIN && cz >= -PP_WIN &&
                    cz <= PP_WIN) {
                    seq_in_win[seq] = 1;
                    if (n_ppm >= MAX_PPM)
                        die("too many in-window promotions", ppath);
                    ppm_cx[n_ppm] = cx;
                    ppm_cz[n_ppm] = cz;
                    n_ppm++;
                }
            }
            p = nl + 1;
        }
        /* 교차검증: 청크별 (섹션 오름차순 × append 순) 유도 마킹 ==
         * 기록 마킹 (기록 순서 = 드레인 순서 그 자체) */
        int64_t bad = 0;
        for (int32_t m = 0; m < n_ppm; m++) {
            int32_t cx = ppm_cx[m], cz = ppm_cz[m];
            /* 기록측: rec_pos 에서 이 청크의 seq 를 파일 순서로 */
            int32_t ri = 0;
            static int32_t ours_x[8192], ours_y[8192], ours_z[8192];
            int32_t n_ours = 0;
            for (int sec = 0; sec < HC_HEIGHT / 16; sec++)
                for (int32_t i = 0; i < ppg.n; i++) {
                    const hc_ppg_rec_t *r = &ppg.recs[i];
                    if ((r->x >> 4) != cx || (r->z >> 4) != cz)
                        continue;
                    if ((r->y - HC_MIN_Y) >> 4 != sec)
                        continue;
                    if (n_ours < 8192) {
                        ours_x[n_ours] = r->x;
                        ours_y[n_ours] = r->y;
                        ours_z[n_ours] = r->z;
                    }
                    n_ours++;
                }
            int32_t n_theirs = 0;
            for (int32_t i = 0; i < n_rec; i++) {
                if (seq_cx[rec_pos[i].seq] != cx ||
                    seq_cz[rec_pos[i].seq] != cz)
                    continue;
                if (n_theirs < n_ours &&
                    (rec_pos[i].x != ours_x[n_theirs] ||
                     rec_pos[i].y != ours_y[n_theirs] ||
                     rec_pos[i].z != ours_z[n_theirs])) {
                    if (bad < 12)
                        fprintf(stderr,
                                "PPG MARK c.%d.%d[%d]: ours (%d,%d,%d) "
                                "golden (%d,%d,%d)\n",
                                cx, cz, n_theirs, ours_x[n_theirs],
                                ours_y[n_theirs], ours_z[n_theirs],
                                rec_pos[i].x, rec_pos[i].y, rec_pos[i].z);
                    bad++;
                }
                n_theirs++;
                (void)ri;
            }
            if (n_theirs != n_ours) {
                fprintf(stderr,
                        "PPG MARK c.%d.%d: count ours %d golden %d\n", cx, cz,
                        n_ours, n_theirs);
                /* die 경로 진단 — 앞쪽 일부만 (마킹 생산자 지목에 충분) */
                for (int32_t i = 0; i < n_ours && i < 40; i++)
                    fprintf(stderr, "  ours[%d] (%d,%d,%d) %s\n", i,
                            ours_x[i], ours_y[i], ours_z[i],
                            hc_block_name(g_world.chunks[widx(cx, cz)].states
                                              [hc_idx(ours_x[i] & 15,
                                                      ours_y[i],
                                                      ours_z[i] & 15)]));
                int32_t ti = 0;
                for (int32_t i = 0; i < n_rec && ti < 40; i++)
                    if (seq_cx[rec_pos[i].seq] == cx &&
                        seq_cz[rec_pos[i].seq] == cz)
                        fprintf(stderr, "  gold[%d] (%d,%d,%d)\n", ti++,
                                rec_pos[i].x, rec_pos[i].y, rec_pos[i].z);
                bad++;
            }
        }
        if (bad)
            die("derived postprocess marks diverge from recording", NULL);
        printf("postprocess marks verified for |c|<=%d (%d chunks, %d "
               "recorded positions); draining\n",
               PP_WIN, n_ppm, n_rec);
    }
    for (int32_t m = 0; m < n_ppm; m++)
        hc_postprocess_chunk(&rg, ppm_cx[m], ppm_cz[m], &ppg);
    printf("postprocess drained %d chunks (unmodeled-veg updateShape "
           "evals: %" PRId64 " — coverage diag, see hc_postprocess.h)\n",
           n_ppm, hc_postprocess_unmodeled_veg_evals());

    /* --- 라이트 최종 고정점 (postProcess 변이 포함 상태의 lfp — 바닐라
     * 증분 relight 의 수렴값과 동일, R2 §10) --- */
    hc_light_world_t lw;
    if (hc_light_world_init(&lw, &g_arena, -WR, -WR, WN) != 0)
        die("arena exhausted (light world)", NULL);
    for (int i = 0; i < WORLD_CHUNKS; i++)
        if (hc_light_attach(&lw, &g_arena, &g_world.chunks[i]) != 0)
            die("arena exhausted (light chunks)", NULL);
    for (int32_t pos = 0; pos < n_man; pos++) {
        if (man[pos].cx < -(WR - 1) || man[pos].cx > WR - 1 ||
            man[pos].cz < -(WR - 1) || man[pos].cz > WR - 1)
            continue; /* 리전 전체 manifest — 라이트 월드(±5) 밖 스킵 */
        hc_light_set_featured(&lw, man[pos].cx, man[pos].cz);
        hc_gen_initialize_light_stage(&lw, man[pos].cx, man[pos].cz);
    }
    /* enable: 3x3 이웃 전부 08 인 청크 (manifest 창 내부 -3..3) */
    for (int32_t cz = -WR + 2; cz <= WR - 2; cz++)
        for (int32_t cx = -WR + 2; cx <= WR - 2; cx++)
            hc_gen_light_stage(&lw, cx, cz);
    hc_light_solve(&lw);

    /* 진단: HC_REGION_DUMP=1 이면 코어 5x5 청크의 상태 배열을 바이너리로
     * 내려놓는다 (u16 호스트 순서, HC_BLOCKS 개) — 오프라인 mca 대조용 */
    if (getenv("HC_REGION_DUMP")) {
        for (int32_t cz = -2; cz <= 2; cz++)
            for (int32_t cx = -2; cx <= 2; cx++) {
                char dpath[1024];
                snprintf(dpath, sizeof dpath, "%s.state.%d.%d.bin",
                         out_mca_path, cx, cz);
                FILE *df = fopen(dpath, "wb");
                if (df) {
                    fwrite(g_world.chunks[widx(cx, cz)].states,
                           sizeof(uint16_t), (size_t)HC_BLOCKS, df);
                    fclose(df);
                }
            }
    }

    /* --- 4청크 직렬화 + 참조 대조 --- */
    static hc_region_chunk_t region_chunks[4];
    hc_arena_t               scratch;
    unsigned char           *scratch_mem = hc_arena_alloc(&g_arena, 32u << 20, 16);
    if (!scratch_mem)
        die("arena exhausted (scratch)", NULL);

    for (int gi = 0; gi < 4; gi++) {
        int32_t     cx = GATE[gi][0], cz = GATE[gi][1];
        hc_chunk_t *c = &g_world.chunks[widx(cx, cz)];
        hc_light_chunk_t *ls = &lw.slots[(cz - lw.cz0) * lw.n + (cx - lw.cx0)];
        uint8_t *payload = hc_arena_alloc(&g_arena, 256u << 10, 1);
        if (!payload)
            die("arena exhausted (payload)", NULL);
        hc_arena_init(&scratch, scratch_mem, 32u << 20);
        ptrdiff_t n = hc_chunk_to_nbt(c, &g_reg, ls, recorder.recs,
                                      recorder.n, /*last_update=*/0, NULL,
                                      &scratch, payload, 256u << 10);
        if (n < 0)
            die("chunk serialization failed", NULL);

        region_chunks[gi].x = cx;
        region_chunks[gi].z = cz;
        region_chunks[gi].payload = payload;
        region_chunks[gi].len = (size_t)n;

        /* 참조 페이로드 대조 */
        char refpath[1024];
        snprintf(refpath, sizeof refpath, "%s/c.%d.%d.nbt", region_ref_dir,
                 cx, cz);
        size_t ref_len = 0;
        char  *ref = read_file(refpath, &ref_len);

        /* 진단용으로 우리 페이로드를 항상 내려놓는다 */
        char ourpath[1024];
        snprintf(ourpath, sizeof ourpath, "%s.c.%d.%d.ours.nbt",
                 out_mca_path, cx, cz);
        FILE *of = fopen(ourpath, "wb");
        if (of) {
            fwrite(payload, 1, (size_t)n, of);
            fclose(of);
        }

        if ((size_t)n != ref_len ||
            memcmp(payload, ref, (size_t)n) != 0) {
            size_t common = (size_t)n < ref_len ? (size_t)n : ref_len;
            size_t off = 0;
            while (off < common && payload[off] == (uint8_t)ref[off])
                off++;
            /* Task 13-close: 코히런트 재캡처 + postProcess 드레인으로
             * 4/4 바이트 일치가 기본 게이트 (HC_REGION_STRICT 불필요). */
            fprintf(stderr,
                    "PAYLOAD c.%d.%d: MISMATCH ours=%td golden=%zu first "
                    "diff @%zu\n",
                    cx, cz, n, ref_len, off);
            size_t ctx0 = off >= 24 ? off - 24 : 0;
            fprintf(stderr, "  ours  @%zu:", ctx0);
            for (size_t i = ctx0; i < ctx0 + 48 && i < (size_t)n; i++)
                fprintf(stderr, " %02x", payload[i]);
            fprintf(stderr, "\n  gold  @%zu:", ctx0);
            for (size_t i = ctx0; i < ctx0 + 48 && i < ref_len; i++)
                fprintf(stderr, " %02x", (uint8_t)ref[i]);
            fprintf(stderr, "\n");
            g_fails++;
        } else {
            printf("PAYLOAD c.%d.%d: OK (%td bytes)\n", cx, cz, n);
        }
    }

    /* --- 미니 리전 이미지 (round-trip 게이트 소비) --- */
    {
        static uint8_t mca[8u << 20];
        ptrdiff_t      mn =
            hc_region_write(region_chunks, 4, 1u, mca, sizeof mca);
        if (mn < 0)
            die("region image assembly failed", NULL);
        FILE *f = fopen(out_mca_path, "wb");
        if (!f)
            die("cannot write out mca", out_mca_path);
        fwrite(mca, 1, (size_t)mn, f);
        fclose(f);
        printf("wrote %s (%td bytes)\n", out_mca_path, mn);
    }

    if (g_fails) {
        fprintf(stderr, "test_region: FAIL (%d payload mismatches)\n",
                g_fails);
        return 1;
    }
    printf("test_region: PASS (4/4 grid chunks byte-exact)\n");
    return 0;
}
