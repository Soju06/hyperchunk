/* 06_carvers 스테이지 청크 패리티 (Plan Task 8 게이트).
 *
 * 스테이지 체이닝 증명: 04_noise 와 05_surface 를 '우리' 구현으로 생성한
 * 상태에서 출발해 (골든 재생이 아니라), 카버를 적용하고
 * golden/stages/seed<seed>/c.<x>.<z>/06_carvers.{blocks,heightmaps}.txt
 * 와 라인 단위 0 diff 를 요구한다.
 *
 * 유일한 골든 유래 입력 = 바이옴 (test_surface_stage.c 와 동일 규약:
 * 3x3 은 03_biomes 덤프, 5x5 링은 surface golden 의 quart_biomes).
 * 소스 청크 17x17 의 carverBiome 게이트는 값-중립이라 (A1 §5.1/§11 —
 * 오버월드 전 바이옴이 같은 3-카버 리스트) 바이옴 입력이 필요 없다.
 *
 * false-PASS 방어: 골든 형식 검증, 라인 수 계상, 총 라인 인쇄, 그리고
 * 05→06 에서 실제로 바뀐 블록 수가 0 이 아님을 확인한다 (카버가 아무
 * 것도 안 파면 05 와 동일해져 게이트가 무의미해진다). */

#undef NDEBUG

#include "../../core/src/hc_carvers.h"
#include "../../core/src/hc_sha256.h"

#include <assert.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdarg.h>
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

static unsigned char g_backing[112u << 20];
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

/* --- reference/ 로더 (test_surface_stage.c 와 동일 규약) --- */

#define MAX_SOURCES 64

static hc_df_source_t g_dfs[MAX_SOURCES];
static int32_t        g_n_dfs = 0;
static hc_df_source_t g_noises[MAX_SOURCES];
static int32_t        g_n_noises = 0;
static hc_df_source_t g_tags[MAX_SOURCES];
static int32_t        g_n_tags = 0;

static int has_suffix(const char *s, const char *suf) {
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

static void add_source(hc_df_source_t *tab, int32_t *n, const char *dir,
                       const char *rel_prefix, const char *fname) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, fname);
    char *name = hc_arena_alloc(
        &g_arena, 10 + strlen(rel_prefix) + strlen(fname) + 1, 1);
    if (!name)
        die("arena exhausted (source name)", fname);
    size_t stem = strlen(fname) - 5;
    sprintf(name, "minecraft:%s%.*s", rel_prefix, (int)stem, fname);
    if (*n >= MAX_SOURCES)
        die("too many reference sources", name);
    tab[*n].name = name;
    tab[*n].json = parse_file(path);
    (*n)++;
}

static void load_tree(hc_df_source_t *tab, int32_t *n, const char *dir,
                      const char *rel_prefix, int depth) {
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
            load_tree(tab, n, sub, pref, depth + 1);
        } else if (has_suffix(e->d_name, ".json")) {
            add_source(tab, n, dir, rel_prefix, e->d_name);
        } else {
            die("unexpected file in reference tree", sub);
        }
    }
    closedir(d);
}

/* --- 5x5 쿼트 바이옴 그리드 (test_surface_stage.c 와 동일) --- */

enum { QG_MIN_XZ = -8, QG_NXZ = 20, QG_MIN_Y = -16, QG_NY = 96 };

static uint16_t       g_grid[QG_NY][QG_NXZ][QG_NXZ];
static hc_biome_reg_t g_reg;

static void load_quart_grid(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        die("cannot open surface golden", path);
    char    line[512];
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
                die("quart_biomes palette out of order", path);
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
                    die("bad quart_biomes row", path);
                g_grid[cur_qy - QG_MIN_Y][cur_qz][qx] = (uint16_t)pal[v];
                p = end;
            }
            cur_qz++;
            rows++;
        }
    }
    fclose(f);
    if (rows != QG_NY * QG_NXZ)
        die("quart_biomes section incomplete", path);
}

static void check_and_fill_chunk_biomes(hc_chunk_t *chunk,
                                        const char *golden_dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/c.%d.%d/03_biomes.biomes.txt", golden_dir,
             chunk->cx, chunk->cz);
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
            /* 헤더 스킵 */
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
                int      gx = chunk->cx * 4 + qx - QG_MIN_XZ;
                int      gz = chunk->cz * 4 + qz - QG_MIN_XZ;
                if (g_grid[qy - QG_MIN_Y][gz][gx] != id)
                    die("quart_biomes vs 03_biomes mismatch", path);
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

/* --- 렌더러 (FORMAT.md — test_surface_stage.c 와 동일) --- */

typedef struct {
    char  *buf;
    size_t len, cap;
} sbuf_t;

static void sb_printf(sbuf_t *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sb->cap - sb->len)
        die("render buffer overflow", NULL);
    sb->len += (size_t)n;
}

static void render_blocks(sbuf_t *sb, const hc_chunk_t *c, const char *stage) {
    sb_printf(sb, "# hyperchunk golden stage dump v1\n");
    sb_printf(sb, "# kind blocks\n");
    sb_printf(sb, "# chunk %d %d\n", c->cx, c->cz);
    sb_printf(sb, "# stage %s\n", stage);
    sb_printf(sb, "# minY %d maxY %d height %d\n", HC_MIN_Y, HC_MAX_Y,
              HC_HEIGHT);
    sb_printf(sb, "# order y in [minY..maxY] asc, then z in [0..15], one row "
                  "of 16 x-asc palette indices\n");
    int16_t  pal_of[HC_B_COUNT];
    uint16_t pal[HC_B_COUNT];
    int      n_pal = 0;
    for (int i = 0; i < HC_B_COUNT; i++)
        pal_of[i] = -1;
    for (size_t i = 0; i < (size_t)HC_BLOCKS; i++) {
        uint16_t b = c->states[i];
        assert(b < HC_B_COUNT);
        if (pal_of[b] < 0) {
            pal_of[b] = (int16_t)n_pal;
            pal[n_pal++] = b;
        }
    }
    for (int i = 0; i < n_pal; i++)
        sb_printf(sb, "palette %d %s\n", i, hc_block_name(pal[i]));
    sb_printf(sb, "data\n");
    for (int y = HC_MIN_Y; y <= HC_MAX_Y; y++)
        for (int z = 0; z < 16; z++) {
            for (int x = 0; x < 16; x++)
                sb_printf(sb, x ? " %d" : "%d",
                          pal_of[c->states[hc_idx(x, y, z)]]);
            sb_printf(sb, "\n");
        }
}

static void render_heightmaps(sbuf_t *sb, const hc_chunk_t *c,
                              const char *stage) {
    sb_printf(sb, "# hyperchunk golden stage dump v1\n");
    sb_printf(sb, "# kind heightmaps\n");
    sb_printf(sb, "# chunk %d %d\n", c->cx, c->cz);
    sb_printf(sb, "# stage %s\n", stage);
    sb_printf(sb, "# minY %d maxY %d height %d\n", HC_MIN_Y, HC_MAX_Y,
              HC_HEIGHT);
    sb_printf(sb, "# order per heightmap type: 16 rows (z asc) of 16 x-asc "
                  "values; value = highest blocking y + 1\n");
    const int32_t *maps[2] = {c->heightmap_ws, c->heightmap_ocean_floor};
    const char    *names[2] = {"WORLD_SURFACE_WG", "OCEAN_FLOOR_WG"};
    for (int m = 0; m < 2; m++) {
        sb_printf(sb, "heightmap %s\n", names[m]);
        for (int z = 0; z < 16; z++) {
            for (int x = 0; x < 16; x++)
                sb_printf(sb, x ? " %d" : "%d", maps[m][hc_col_idx(x, z)]);
            sb_printf(sb, "\n");
        }
    }
}

static void diff_lines(const char *what, const char *got, const char *want,
                       size_t want_len) {
    int    line = 1;
    size_t gi = 0, wi = 0;
    size_t glen = strlen(got);
    int    local_fails = 0;
    while (gi < glen || wi < want_len) {
        const char *gs = got + gi, *ws = want + wi;
        size_t gl = 0, wl = 0;
        while (gi + gl < glen && got[gi + gl] != '\n')
            gl++;
        while (wi + wl < want_len && want[wi + wl] != '\n')
            wl++;
        int eof_g = gi >= glen, eof_w = wi >= want_len;
        if (eof_g != eof_w || gl != wl || memcmp(gs, ws, gl) != 0) {
            local_fails++;
            if (local_fails <= 10)
                fprintf(stderr,
                        "DIFF %s line %d:\n  ours:   %.*s\n  golden: %.*s\n",
                        what, line, eof_g ? 5 : (int)gl, eof_g ? "<EOF>" : gs,
                        eof_w ? 5 : (int)wl, eof_w ? "<EOF>" : ws);
        }
        gi += gl + (gi + gl < glen ? 1 : 0);
        wi += wl + (wi + wl < want_len ? 1 : 0);
        line++;
        if (eof_g && eof_w)
            break;
    }
    if (local_fails)
        fprintf(stderr, "DIFF %s: %d differing lines (of %d)\n", what,
                local_fails, line - 1);
    g_fails += local_fails;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: test_carvers_stage <ref_dir> "
                        "<golden_stage_seed_dir> <surface_seed_golden> "
                        "<seed>\n");
        return 2;
    }
    const char *ref_dir = argv[1];
    const char *golden_dir = argv[2];
    const char *surface_golden = argv[3];
    int64_t     seed = strtoll(argv[4], NULL, 10);

    hc_arena_init(&g_arena, g_backing, sizeof g_backing);
    hc_biome_reg_init(&g_reg, &g_arena);

    char sub[1024];
    snprintf(sub, sizeof sub, "%s/density_function", ref_dir);
    load_tree(g_dfs, &g_n_dfs, sub, "", 0);
    snprintf(sub, sizeof sub, "%s/noise", ref_dir);
    load_tree(g_noises, &g_n_noises, sub, "", 0);
    if (g_n_dfs < 19 || g_n_noises < 51)
        die("reference closure incomplete", ref_dir);

    /* 블록 태그 (replaceable 확장용) */
    snprintf(sub, sizeof sub, "%s/tags/block", ref_dir);
    load_tree(g_tags, &g_n_tags, sub, "", 0);
    if (g_n_tags < 12)
        die("block tags incomplete", ref_dir);

    snprintf(sub, sizeof sub, "%s/overworld-26.2.json", ref_dir);
    const hc_json_t *settings = parse_file(sub);
    const hc_json_t *router = hc_json_get(settings, "noise_router");
    if (!router || router->kind != HC_JSON_OBJ || router->count != 15)
        die("noise_router missing or not 15 slots", sub);
    const hc_json_t *sea = hc_json_get(settings, "sea_level");
    if (!sea || sea->kind != HC_JSON_NUM)
        die("sea_level missing", sub);

    /* 카버 설정 — carverIndex 순서 (오버월드 바이옴 JSON 의 "carvers"
     * 배열 순서): cave, cave_extra_underground, canyon (A1 §5.3) */
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
    /* 컴파일 결과 새니티 (fail-loud): 확률/y 범위가 데이터와 일치 */
    if (!(carvers[0].probability == 0.15f && carvers[0].y_max == 180 &&
          carvers[1].probability == 0.07f && carvers[1].y_max == 47 &&
          carvers[2].probability == 0.01f && carvers[2].y_min == 10 &&
          carvers[2].y_max == 67 && carvers[2].lava_level == -56))
        die("carver config sanity check failed", NULL);

    /* 라우터 컴파일 (04_noise 와 동일 경로) */
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
        if (root < 0) {
            fprintf(stderr, "compile failed for slot %s: %s\n", key,
                    comp.err ? comp.err : "?");
            return 2;
        }
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

    load_quart_grid(surface_golden);
    load_climate(ref_dir);

    hc_biome_view_t view;
    view.qx0 = QG_MIN_XZ;
    view.qz0 = QG_MIN_XZ;
    view.nxz = QG_NXZ;
    view.qy0 = QG_MIN_Y;
    view.ny = QG_NY;
    view.ids = &g_grid[0][0][0];
    view.zoom_seed = hc_biome_obfuscate_seed(seed);

    sbuf_t sb;
    sb.cap = 1u << 20;
    sb.buf = hc_arena_alloc(&g_arena, sb.cap, 1);
    if (!sb.buf)
        die("arena exhausted (render buffer)", NULL);

    static const int32_t CHUNKS[][2] = {{0, 0},  {1, 0},  {-1, -1},
                                        {0, -1}, {1, -1}, {-1, 0},
                                        {-1, 1}, {0, 1},  {1, 1}};
    int     total_lines = 0;
    int64_t total_carved = 0;
    for (size_t ci = 0; ci < sizeof CHUNKS / sizeof CHUNKS[0]; ci++) {
        int32_t cx = CHUNKS[ci][0], cz = CHUNKS[ci][1];

        hc_noise_chunk_t *nc =
            hc_arena_alloc(&g_arena, sizeof *nc, _Alignof(hc_noise_chunk_t));
        hc_chunk_t chunk;
        if (!nc || hc_chunk_init(&chunk, &g_arena, cx, cz) != 0)
            die("arena exhausted (chunk)", NULL);
        if (hc_nc_init(nc, &g_arena, &graph, &roots, seed, cx, cz,
                       (int32_t)sea->num) != 0)
            die("arena exhausted (noise chunk)", NULL);

        /* 체인: 우리 04 → (골든 바이옴) → 우리 05 → 우리 06 */
        hc_gen_noise_stage(&chunk, nc);
        check_and_fill_chunk_biomes(&chunk, golden_dir);
        hc_gen_surface_stage(&chunk, nc, surf, &view);

        uint16_t *pre = hc_arena_alloc(
            &g_arena, sizeof(uint16_t) * (size_t)HC_BLOCKS, 2);
        if (!pre)
            die("arena exhausted (pre snapshot)", NULL);
        memcpy(pre, chunk.states, sizeof(uint16_t) * (size_t)HC_BLOCKS);

        static uint64_t mask[HC_CARVING_MASK_WORDS];
        memset(mask, 0, sizeof mask);
        hc_gen_carvers_stage(&chunk, nc, surf, &view, seed, carvers, 3,
                             mask);

        int64_t changed = 0;
        for (size_t i = 0; i < (size_t)HC_BLOCKS; i++)
            if (pre[i] != chunk.states[i])
                changed++;
        total_carved += changed;

        for (int kind = 0; kind < 2; kind++) {
            sb.len = 0;
            if (kind == 0)
                render_blocks(&sb, &chunk, "carvers");
            else
                render_heightmaps(&sb, &chunk, "carvers");
            sb.buf[sb.len] = '\0';

            char path[1024], what[64];
            snprintf(path, sizeof path, "%s/c.%d.%d/06_carvers.%s.txt",
                     golden_dir, cx, cz, kind == 0 ? "blocks" : "heightmaps");
            snprintf(what, sizeof what, "c.%d.%d %s", cx, cz,
                     kind == 0 ? "blocks" : "heightmaps");
            const char *dump_dir = getenv("HC_DUMP_DIR");
            if (dump_dir) { /* 디버그: 우리 렌더를 파일로 (패리티 분석용) */
                char dpath[1024];
                snprintf(dpath, sizeof dpath, "%s/c.%d.%d.%s.txt", dump_dir,
                         cx, cz, kind == 0 ? "blocks" : "heightmaps");
                FILE *df = fopen(dpath, "w");
                if (df) {
                    fwrite(sb.buf, 1, sb.len, df);
                    fclose(df);
                }
            }
            size_t glen = 0;
            char  *golden = read_file(path, &glen);
            if (glen < 100 ||
                strncmp(golden, "# hyperchunk golden stage dump v1\n", 34) !=
                    0)
                die("golden file malformed", path);
            diff_lines(what, sb.buf, golden, glen);
            for (size_t i = 0; i < glen; i++)
                if (golden[i] == '\n')
                    total_lines++;
        }
    }

    if (total_carved == 0)
        die("carvers changed 0 blocks across 9 chunks — gate is vacuous",
            NULL);

    printf("test_carvers_stage: 9 chunks (own 04+05 chained), %" PRId64
           " blocks carved, %d golden lines compared, %d diffs\n",
           total_carved, total_lines, g_fails);
    return g_fails == 0 ? 0 : 1;
}
