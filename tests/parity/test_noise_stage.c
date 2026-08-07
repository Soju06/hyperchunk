/* 04_noise 스테이지 청크 패리티 (Plan Task 6c 게이트).
 *
 * reference/ worldgen JSON 클로저 → 평탄 IR → NoiseChunk 셀 상태 기계 +
 * aquifer + ore vein + doFill 재현으로 청크를 생성하고, FORMAT.md 레이아웃
 * 텍스트로 렌더해 golden/stages/seed<seed>/c.<x>.<z>/04_noise.{blocks,
 * heightmaps}.txt 와 '라인 단위' 대조한다. 목표는 0 diff — 헤더/팔레트
 * 순서(첫 등장)까지 포함한 전문 일치다.
 *
 * false-PASS 방어:
 *  - golden 파일이 없거나 헤더 형식이 다르면 즉사 (exit 2)
 *  - 라인 수 불일치도 diff 로 계상 (꼬리 절단 방어)
 *  - 검사한 총 라인 수를 인쇄해 육안 대조 가능 (청크당 블록 ~6154 + 하이트맵 40) */

#undef NDEBUG

#include "../../core/src/hc_df_compile.h"
#include "../../core/src/hc_gen_noise.h"

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

/* JSON DOM + IR + 청크별 상태가 전부 들어간다 */
static unsigned char g_backing[96u << 20];
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

/* --- reference/ 로더 (test_router_slots.c 와 동일 규약) --- */

#define MAX_SOURCES 64

static hc_df_source_t g_dfs[MAX_SOURCES];
static int32_t        g_n_dfs = 0;
static hc_df_source_t g_noises[MAX_SOURCES];
static int32_t        g_n_noises = 0;

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
        if ((size_t)snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name) >=
            sizeof sub)
            die("reference path too long", dir);
        DIR *probe = opendir(sub);
        if (probe) {
            closedir(probe);
            if (depth >= 2)
                die("reference tree deeper than expected", sub);
            char pref[512];
            if ((size_t)snprintf(pref, sizeof pref, "%s%s/", rel_prefix,
                                 e->d_name) >= sizeof pref)
                die("reference prefix too long", rel_prefix);
            load_tree(tab, n, sub, pref, depth + 1);
        } else if (has_suffix(e->d_name, ".json")) {
            add_source(tab, n, dir, rel_prefix, e->d_name);
        } else {
            die("unexpected file in reference tree", sub);
        }
    }
    closedir(d);
}

/* --- 블록 id → 캐노니컬 직렬화 --- */

static const char *block_name(uint16_t b) {
    switch (b) {
    case HC_B_AIR:
        return "minecraft:air";
    case HC_B_STONE:
        return "minecraft:stone";
    case HC_B_WATER:
        return "minecraft:water[level=0]";
    case HC_B_LAVA:
        return "minecraft:lava[level=0]";
    case HC_B_COPPER_ORE:
        return "minecraft:copper_ore";
    case HC_B_RAW_COPPER_BLOCK:
        return "minecraft:raw_copper_block";
    case HC_B_GRANITE:
        return "minecraft:granite";
    case HC_B_DEEPSLATE_IRON_ORE:
        return "minecraft:deepslate_iron_ore";
    case HC_B_RAW_IRON_BLOCK:
        return "minecraft:raw_iron_block";
    case HC_B_TUFF:
        return "minecraft:tuff";
    default:
        die("unknown block id", NULL);
        return NULL;
    }
}

/* --- 렌더러: FORMAT.md 레이아웃 그대로 텍스트 생성 --- */

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

static void render_blocks(sbuf_t *sb, const hc_chunk_t *c) {
    sb_printf(sb, "# hyperchunk golden stage dump v1\n");
    sb_printf(sb, "# kind blocks\n");
    sb_printf(sb, "# chunk %d %d\n", c->cx, c->cz);
    sb_printf(sb, "# stage noise\n");
    sb_printf(sb, "# minY %d maxY %d height %d\n", HC_MIN_Y, HC_MAX_Y,
              HC_HEIGHT);
    sb_printf(sb, "# order y in [minY..maxY] asc, then z in [0..15], one row "
                  "of 16 x-asc palette indices\n");
    /* 팔레트: 스캔 순서 (y,z,x) 첫 등장 순 */
    int16_t pal_of[HC_B_COUNT];
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
        sb_printf(sb, "palette %d %s\n", i, block_name(pal[i]));
    sb_printf(sb, "data\n");
    for (int y = HC_MIN_Y; y <= HC_MAX_Y; y++)
        for (int z = 0; z < 16; z++) {
            for (int x = 0; x < 16; x++)
                sb_printf(sb, x ? " %d" : "%d",
                          pal_of[c->states[hc_idx(x, y, z)]]);
            sb_printf(sb, "\n");
        }
}

static void render_heightmaps(sbuf_t *sb, const hc_chunk_t *c) {
    sb_printf(sb, "# hyperchunk golden stage dump v1\n");
    sb_printf(sb, "# kind heightmaps\n");
    sb_printf(sb, "# chunk %d %d\n", c->cx, c->cz);
    sb_printf(sb, "# stage noise\n");
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

/* --- 라인 단위 대조 --- */

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
    if (argc != 4) {
        fprintf(stderr,
                "usage: test_noise_stage <ref_dir> <golden_stage_seed_dir> "
                "<seed>\n");
        return 2;
    }
    const char *ref_dir = argv[1];
    const char *golden_dir = argv[2];
    int64_t     seed = strtoll(argv[3], NULL, 10);

    hc_arena_init(&g_arena, g_backing, sizeof g_backing);

    char sub[1024];
    snprintf(sub, sizeof sub, "%s/density_function", ref_dir);
    load_tree(g_dfs, &g_n_dfs, sub, "", 0);
    snprintf(sub, sizeof sub, "%s/noise", ref_dir);
    load_tree(g_noises, &g_n_noises, sub, "", 0);
    if (g_n_dfs < 19 || g_n_noises < 35)
        die("reference closure incomplete", ref_dir);

    snprintf(sub, sizeof sub, "%s/overworld-26.2.json", ref_dir);
    const hc_json_t *settings = parse_file(sub);
    const hc_json_t *router = hc_json_get(settings, "noise_router");
    if (!router || router->kind != HC_JSON_OBJ || router->count != 15)
        die("noise_router missing or not 15 slots", sub);
    const hc_json_t *sea = hc_json_get(settings, "sea_level");
    if (!sea || sea->kind != HC_JSON_NUM)
        die("sea_level missing", sub);
    const hc_json_t *aq_en = hc_json_get(settings, "aquifers_enabled");
    const hc_json_t *ore_en = hc_json_get(settings, "ore_veins_enabled");
    if (!aq_en || aq_en->kind != HC_JSON_BOOL || !aq_en->boolean || !ore_en ||
        ore_en->kind != HC_JSON_BOOL || !ore_en->boolean)
        die("aquifers/ore_veins not both enabled — stage assumes 26.2 "
            "overworld defaults",
            sub);

    /* 라우터 15슬롯 전부를 하나의 공유 그래프로 컴파일 (바닐라 mapAll 이
     * 전 슬롯을 한 visitor 로 감싸는 것과 동형 — 마커 조사가 일치한다) */
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

    /* 렌더 버퍼 (blocks ~300KB) */
    sbuf_t sb;
    sb.cap = 1u << 20;
    sb.buf = hc_arena_alloc(&g_arena, sb.cap, 1);
    if (!sb.buf)
        die("arena exhausted (render buffer)", NULL);

    /* golden 9청크 전부 — c.1.-1 이 유일하게 용암(y<-54 글로벌 픽커)을,
     * c.0.-1/c.0.0/c.-1.-1 이 aquifer 물을 커버한다 */
    static const int32_t CHUNKS[][2] = {{0, 0},  {1, 0},   {-1, -1},
                                        {0, -1}, {1, -1},  {-1, 0},
                                        {-1, 1}, {0, 1},   {1, 1}};
    int total_lines = 0;
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

        hc_gen_noise_stage(&chunk, nc);

        for (int kind = 0; kind < 2; kind++) {
            sb.len = 0;
            if (kind == 0)
                render_blocks(&sb, &chunk);
            else
                render_heightmaps(&sb, &chunk);
            sb.buf[sb.len] = '\0';

            char path[1024], what[64];
            snprintf(path, sizeof path, "%s/c.%d.%d/04_noise.%s.txt",
                     golden_dir, cx, cz, kind == 0 ? "blocks" : "heightmaps");
            snprintf(what, sizeof what, "c.%d.%d %s", cx, cz,
                     kind == 0 ? "blocks" : "heightmaps");
            size_t glen = 0;
            char  *golden = read_file(path, &glen);
            /* 최소 형식 검증 — 빈/절단 golden 으로 인한 false-PASS 방어 */
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

    printf("test_noise_stage: %d nodes, %d noises; 9 chunks, %d golden "
           "lines compared, %d diffs\n",
           graph.n, graph.n_noises, total_lines, g_fails);
    return g_fails == 0 ? 0 : 1;
}
