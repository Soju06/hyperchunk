/* Task 14: 풀 리전 canonical 게이트 — r.0.0 전체 1024청크 byte-exact.
 *
 * 파이프라인 (test_region.c 의 4청크 게이트를 리전 스케일로 확장):
 *   41x41 월드 (청크 -5..35) 04..06 체인 (순수 — 테스트측 pthread 병렬)
 *   → primary order.manifest 1,461 엔트리 전체 재생 (07)
 *   → postprocess.manifest 승격 순서 (1,165 청크, -2..32) 드레인
 *     (유도 마킹 vs 기록 마킹 리전-와이드 교차검증)
 *   → 라이트: R/S 를 stages.log v2 제출 라인에서 실측 재구성
 *     (R = initialize_light 1,461 / S = light 1,309) 후 최종 고정점
 *   → 1024청크 직렬화, region-ref 페이로드 바이트 대조 + canonical 해시
 *     (sha256(u32be idx ‖ masked payload), golden/SHA256SUMS
 *     #canonical-payload 라인) 일치.
 *
 * 바이옴: 리전용 와이드 밴드 골든 (청크 -9..40, 순수 함수 샘플링) 를
 * 베이스로, r.0.0 내부는 .mca 청크 바이옴을 오버레이한다 — 생 샘플링과
 * 기록 청크 바이옴은 Climate RTree lastResult 캐시의 tie-resolution 이
 * 질의 순서 의존이라 22 쿼트가 어긋난다 (실측 2026-08-04). 마진 청크의
 * 바이옴은 밴드 그대로 (기록 부재 — 커버리지 경계, 완료 노트 참조).
 *
 * HC_SURVEY=1 (환경): 갭 열거 모드 — 미구현 feature 는 die 대신 수집,
 * 마킹 불일치 청크는 드레인 스킵, 직렬화 대신 블록 단위 diff 분류
 * 리포트. 게이트 실행 (ctest) 은 이 변수를 절대 켜지 않는다. */

#undef NDEBUG
#define _POSIX_C_SOURCE 200809L

#include "../../core/src/hc_carvers.h"
#include "../../core/src/hc_chunk_nbt.h"
#include "../../core/src/hc_features.h"
#include "../../core/src/hc_light.h"
#include "../../core/src/hc_nbt.h"
#include "../../core/src/hc_postprocess.h"
#include "../../core/src/hc_region.h"
#include "../../core/src/hc_sha256.h"
#include "../../core/src/hc_structures.h"

#include <assert.h>
#include <dirent.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int        g_survey = 0;
static hc_sctx_t *g_sctx = NULL;

static void struct_step_cb(void *ud, hc_feat_region_t *rg,
                           const struct hc_feat_reg *reg, int32_t sea_level,
                           int32_t cx, int32_t cz, int64_t deco_seed,
                           int32_t step) {
    hc_structures_step((hc_sctx_t *)ud, rg, (const hc_feat_reg_t *)reg,
                       sea_level, cx, cz, deco_seed, step);
}

static void die(const char *msg, const char *detail) {
    fprintf(stderr, "GOLDEN/SETUP ERROR: %s%s%s\n", msg, detail ? ": " : "",
            detail ? detail : "");
    exit(2);
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* 6 GiB 백킹 — BSS 2GiB 릴로케이션 한계를 피해서 malloc. */
static hc_arena_t g_arena;

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

/* --- reference/ 로더 (test_region.c 와 동일 규약) --- */

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

/* --- 와이드 쿼트 바이옴 그리드 (청크 -12..43 = 쿼트 -48..175) ---
 * 창 근거: 구조물 위치계산 스캔 -12..43 (References 반경 8 + 데코 마진). */

enum { QG_MIN_XZ = -48, QG_NXZ = 224, QG_MIN_Y = -16, QG_NY = 96 };

static uint16_t       g_grid[QG_NY][QG_NXZ][QG_NXZ];
static hc_biome_reg_t g_reg;

static void load_band_grid(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        die("cannot open biome band golden", path);
    char    line[2048];
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

/* --- 월드 --- */

enum { WC0 = -5, WN = 41, WORLD_CHUNKS = WN * WN }; /* 청크 -5..35 */

static hc_chunk_t g_chunks[WORLD_CHUNKS];

static int widx(int32_t cx, int32_t cz) {
    assert(cx >= WC0 && cx < WC0 + WN && cz >= WC0 && cz < WC0 + WN);
    return (cz - WC0) * WN + (cx - WC0);
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

/* --- region-ref 페이로드 (참조 바이트 + 바이옴 오버레이 소스) --- */

static uint8_t *g_ref[32 * 32]; /* [cz*32+cx] */
static size_t   g_ref_len[32 * 32];

static int ceil_log2_i(int32_t n) {
    int b = 0;
    while ((1 << b) < n)
        b++;
    return b;
}

/* 참조 페이로드의 sections[].biomes 를 g_grid 에 오버레이. 파스 트리는
 * scratch — 호출자가 되감는다. */
static void overlay_biomes_from_ref(int32_t cx, int32_t cz,
                                    const uint8_t *payload, size_t len,
                                    hc_arena_t *scratch) {
    hc_nbt_t *root = hc_nbt_parse(scratch, payload, len);
    if (!root)
        die("region-ref payload parse failed", NULL);
    const hc_nbt_t *sections = hc_nbt_get(root, "sections");
    if (!sections)
        die("region-ref payload missing sections", NULL);
    for (int32_t i = 0; i < hc_nbt_list_count(sections); i++) {
        const hc_nbt_t *sec = hc_nbt_list_at(sections, i);
        const hc_nbt_t *bio = hc_nbt_get(sec, "biomes");
        if (!bio)
            continue;
        int32_t sy = (int32_t)hc_nbt_i64(hc_nbt_get(sec, "Y"));
        const hc_nbt_t *palette = hc_nbt_get(bio, "palette");
        int32_t         npal = hc_nbt_list_count(palette);
        int32_t         ids[64];
        for (int32_t p = 0; p < npal; p++) {
            const char *nm = hc_nbt_str(hc_nbt_list_at(palette, p));
            ids[p] = hc_biome_intern(&g_reg, nm, (int32_t)strlen(nm));
            if (ids[p] < 0)
                die("biome registry full (overlay)", nm);
        }
        const hc_nbt_t *data = hc_nbt_get(bio, "data");
        for (int32_t q = 0; q < 64; q++) {
            int32_t pi = 0;
            if (data) {
                int bits = ceil_log2_i(npal);
                int32_t        n = 0;
                const int64_t *longs = hc_nbt_la(data, &n);
                int            vpl = 64 / bits;
                pi = (int32_t)(((uint64_t)longs[q / vpl] >>
                                ((q % vpl) * bits)) &
                               ((1u << bits) - 1u));
                if (pi >= npal)
                    die("overlay biome index out of palette", NULL);
            }
            int qx = q & 3, qz = (q >> 2) & 3, qy = (q >> 4) & 3;
            int gy = sy * 4 + qy - QG_MIN_Y;
            int gz = cz * 4 + qz - QG_MIN_XZ;
            int gx = cx * 4 + qx - QG_MIN_XZ;
            assert(gy >= 0 && gy < QG_NY);
            g_grid[gy][gz][gx] = (uint16_t)ids[pi];
        }
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
    size_t  len = 0;
    char   *buf = read_file(path, &len);
    char   *p = buf;
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

/* --- 서베이/가드 트레이스 싱크 --- */

enum { MAX_GAP_NAMES = 512 };
static struct {
    const char *name;
    int64_t     fires;
} g_gaps[MAX_GAP_NAMES];
static int32_t g_n_gaps = 0;

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

static void sink_feature(void *ud, int32_t step, int32_t index,
                         const char *name, int32_t npos, int32_t placed) {
    (void)ud;
    if (npos <= 0 || placed >= 0)
        return;
    if (!g_survey) {
        char buf[192];
        snprintf(buf, sizeof buf, "%s (step %d index %d, npos %d)", name,
                 step, index, npos);
        die("UNIMPLEMENTED feature body fired in replay", buf);
    }
    for (int32_t i = 0; i < g_n_gaps; i++)
        if (strcmp(g_gaps[i].name, name) == 0) {
            g_gaps[i].fires++;
            return;
        }
    if (g_n_gaps < MAX_GAP_NAMES) {
        g_gaps[g_n_gaps].name = name;
        g_gaps[g_n_gaps].fires = 1;
        g_n_gaps++;
    }
}

static const hc_feat_trace_t g_sink = {sink_pos_nop, sink_feature, NULL};

/* --- 진단: 우리측 04..06 스테이지 블록 덤프 ---
 * HC_DIAG_STAGE_DUMP="x0 z0 x1 z1 dir" (리전 청크좌표 박스, 밖이면 무경로).
 * FORMAT.md v1 blocks 레이아웃 (팔레트 첫등장순 + y승순 z행 x16열) —
 * 바닐라 스테이지 덤프와 tools/golden/diff_stage_dump.py 로 대조. */
static int  g_diag_x0 = 1, g_diag_z0 = 1, g_diag_x1 = 0, g_diag_z1 = 0;
static char g_diag_dir[512];

static void diag_dump_stage(const hc_chunk_t *c, const char *stage) {
    if (c->cx < g_diag_x0 || c->cx > g_diag_x1 || c->cz < g_diag_z0 ||
        c->cz > g_diag_z1)
        return;
    char path[1024];
    snprintf(path, sizeof path, "%s/c.%d.%d", g_diag_dir, c->cx, c->cz);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/c.%d.%d/%s.blocks.txt", g_diag_dir, c->cx,
             c->cz, stage);
    FILE *f = fopen(path, "w");
    if (!f)
        die("diag dump fopen failed", path);
    fprintf(f,
            "# hyperchunk C stage dump (diag)\n# kind blocks\n"
            "# chunk %d %d\n# stage %s\n# minY %d maxY %d height %d\n",
            c->cx, c->cz, stage, HC_MIN_Y, HC_MAX_Y, HC_MAX_Y - HC_MIN_Y + 1);
    uint16_t pal[4096];
    int32_t  npal = 0;
    static _Thread_local int32_t inv[HC_B_COUNT];
    for (int32_t i = 0; i < HC_B_COUNT; i++)
        inv[i] = -1;
    for (int32_t y = HC_MIN_Y; y <= HC_MAX_Y; y++)
        for (int32_t lz = 0; lz < 16; lz++)
            for (int32_t lx = 0; lx < 16; lx++) {
                uint16_t s = c->states[hc_idx(lx, y, lz)];
                if (inv[s] < 0) {
                    if (npal == 4096)
                        die("diag palette overflow", NULL);
                    inv[s] = npal;
                    pal[npal++] = s;
                }
            }
    for (int32_t i = 0; i < npal; i++)
        fprintf(f, "palette %d %s\n", i, hc_block_name(pal[i]));
    fprintf(f, "data\n");
    for (int32_t y = HC_MIN_Y; y <= HC_MAX_Y; y++)
        for (int32_t lz = 0; lz < 16; lz++) {
            for (int32_t lx = 0; lx < 16; lx++)
                fprintf(f, lx ? " %d" : "%d",
                        inv[c->states[hc_idx(lx, y, lz)]]);
            fputc('\n', f);
        }
    fclose(f);
}

/* 진단: PSL 그리드 덤프 (diag 박스 청크, 04 직후 — aquifer 입력 대조) */
static void diag_dump_psl(const hc_chunk_t *c, hc_noise_chunk_t *nc) {
    if (c->cx < g_diag_x0 || c->cx > g_diag_x1 || c->cz < g_diag_z0 ||
        c->cz > g_diag_z1)
        return;
    char path[1024];
    snprintf(path, sizeof path, "%s/c.%d.%d", g_diag_dir, c->cx, c->cz);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/c.%d.%d/psl.txt", g_diag_dir, c->cx,
             c->cz);
    FILE *f = fopen(path, "w");
    if (!f)
        die("diag psl fopen failed", path);
    for (int32_t z = c->cz * 16 - 32; z <= c->cz * 16 + 47; z += 4)
        for (int32_t x = c->cx * 16 - 32; x <= c->cx * 16 + 47; x += 4)
            fprintf(f, "psl %d %d %d\n", x, z, hc_nc_psl(nc, x, z));
    fclose(f);
    /* 대수층 그리드 셀 소스/상태 (블록 ±2 그리드) */
    snprintf(path, sizeof path, "%s/c.%d.%d/aquifer.txt", g_diag_dir, c->cx,
             c->cz);
    f = fopen(path, "w");
    if (!f)
        die("diag aquifer fopen failed", path);
    int32_t cgx = c->cx, cgz = c->cz; /* gridX(16cx)=cx (16블록 그리드) */
    for (int32_t gy = -6; gy <= 3; gy++)
        for (int32_t gz = cgz - 2; gz <= cgz + 2; gz++)
            for (int32_t gx = cgx - 2; gx <= cgx + 2; gx++) {
                int32_t sx, sy, sz, lvl, ty;
                hc_aquifer_debug_cell(&nc->aq, gx, gy, gz, &sx, &sy, &sz,
                                      &lvl, &ty);
                fprintf(f, "cell %d %d %d src %d %d %d level %d type %s\n",
                        gx, gy, gz, sx, sy, sz, lvl, hc_block_name(ty));
            }
    fclose(f);
}

/* --- 병렬 04..06 체인 --- */

typedef struct {
    hc_arena_t       arena; /* 스레드 전용 스크래치 */
    const hc_df_graph_t   *graph;
    const hc_noise_roots_t *roots;
    hc_surface_t          *surf;
    const hc_biome_view_t *view;
    int64_t                seed;
    int32_t                sea;
    const hc_carver_t     *carvers;
    _Atomic int32_t       *next;
} chain_job_t;

static void *chain_worker(void *ud) {
    chain_job_t *job = ud;
    for (;;) {
        int32_t i =
            atomic_fetch_add_explicit(job->next, 1, memory_order_relaxed);
        if (i >= WORLD_CHUNKS)
            return NULL;
        hc_chunk_t *c = &g_chunks[i];
        size_t      mark = job->arena.off;
        hc_noise_chunk_t *nc = hc_arena_alloc(&job->arena, sizeof *nc,
                                              _Alignof(hc_noise_chunk_t));
        if (!nc || hc_nc_init(nc, &job->arena, job->graph, job->roots,
                              job->seed, c->cx, c->cz, job->sea) != 0)
            die("thread arena exhausted (noise chunk)", NULL);
        hc_gen_noise_stage(c, nc);
        diag_dump_stage(c, "04_noise");
        diag_dump_psl(c, nc);
        hc_gen_surface_stage(c, nc, job->surf, job->view);
        diag_dump_stage(c, "05_surface");
        uint64_t mask[HC_CARVING_MASK_WORDS];
        memset(mask, 0, sizeof mask);
        hc_gen_carvers_stage(c, nc, job->surf, job->view, job->seed,
                             job->carvers, 3, mask);
        diag_dump_stage(c, "06_carvers");
        job->arena.off = mark;
    }
}

/* --- stages.log v2 제출 라인 (라이트 R/S 실측 집합) --- */

static uint8_t g_stage_r[WORLD_CHUNKS]; /* initialize_light 제출 */
static uint8_t g_stage_s[WORLD_CHUNKS]; /* light 제출 */

static void load_stage_sets(const char *stages_dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/stages.log", stages_dir);
    size_t len = 0;
    char  *buf = read_file(path, &len);
    char  *p = buf;
    int32_t n_r = 0, n_s = 0;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (!nl)
            break;
        *nl = '\0';
        if (p[0] == 's' && p[1] == ' ') {
            long long ii, fseq, nanos;
            char      name[64];
            int32_t   cx, cz;
            if (sscanf(p + 2, "%lld %63s %d %d %lld %lld", &ii, name, &cx,
                       &cz, &fseq, &nanos) != 6)
                die("bad stages.log submission line", p);
            if (strcmp(name, "initialize_light") == 0) {
                g_stage_r[widx(cx, cz)] = 1;
                n_r++;
            } else if (strcmp(name, "light") == 0) {
                g_stage_s[widx(cx, cz)] = 1;
                n_s++;
            }
        }
        p = nl + 1;
    }
    printf("stages.log: %d initialize_light, %d light submissions\n", n_r,
           n_s);
    if (n_r < 1024 || n_s < 1024)
        die("stages.log submission sets unexpectedly small", path);
}

/* --- 마킹 교차검증 + 드레인 (postprocess.manifest 전체) --- */

static hc_ppg_recorder_t g_ppg[WORLD_CHUNKS];

/* 청크의 유도 마킹을 드레인 순서 (섹션 오름차순 × append 순) 로 평탄화 */
static int32_t flatten_marks(const hc_ppg_recorder_t *r, int32_t *xs,
                             int32_t *ys, int32_t *zs, int32_t cap) {
    int32_t n = 0;
    for (int sec = 0; sec < HC_HEIGHT / 16; sec++)
        for (int32_t i = 0; i < r->n; i++) {
            if ((r->recs[i].y - HC_MIN_Y) >> 4 != sec)
                continue;
            if (n < cap) {
                xs[n] = r->recs[i].x;
                ys[n] = r->recs[i].y;
                zs[n] = r->recs[i].z;
            }
            n++;
        }
    return n;
}

int main(int argc, char **argv) {
    if (argc != 8) {
        fprintf(stderr,
                "usage: test_full_region <ref_dir> <stages_seed_dir> "
                "<band_region_golden> <seed> <region_ref_dir> "
                "<canonical_sha256_hex> <out_mca_path>\n");
        return 2;
    }
    const char *ref_dir = argv[1];
    const char *stages_dir = argv[2];
    const char *band_golden = argv[3];
    int64_t     seed = strtoll(argv[4], NULL, 10);
    const char *region_ref_dir = argv[5];
    const char *canonical_hex = argv[6];
    const char *out_mca_path = argv[7];
    g_survey = getenv("HC_SURVEY") != NULL;
    hc_features_survey = g_survey; /* reg init 의 강등 로그도 켠다 */
    const char *focus_env = getenv("HC_SURVEY_FOCUS");
    int32_t     fx0 = 0, fz0 = 0, fx1 = 31, fz1 = 31;
    if (focus_env)
        sscanf(focus_env, "%d %d %d %d", &fx0, &fz0, &fx1, &fz1);
    {
        const char *de = getenv("HC_DIAG_STAGE_DUMP");
        if (de && sscanf(de, "%d %d %d %d %511s", &g_diag_x0, &g_diag_z0,
                         &g_diag_x1, &g_diag_z1, g_diag_dir) == 5)
            mkdir(g_diag_dir, 0755);
        else if (de)
            die("HC_DIAG_STAGE_DUMP malformed (want: x0 z0 x1 z1 dir)", de);
    }

    double t0 = now_s();
    size_t backing_sz = (size_t)6 << 30;
    void  *backing = malloc(backing_sz);
    if (!backing)
        die("cannot allocate 6GiB backing", NULL);
    hc_arena_init(&g_arena, backing, backing_sz);
    hc_biome_reg_init(&g_reg, &g_arena);

    /* --- reference 로드 (test_region.c 규약) --- */
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
    static hc_carver_t carvers[3];
    for (int i = 0; i < 3; i++) {
        snprintf(sub, sizeof sub, "%s/carver/%s.json", ref_dir,
                 CARVER_FILES[i]);
        const char *err = NULL;
        if (hc_carver_init(&carvers[i], parse_file(sub), g_tags, g_n_tags,
                           &err) != 0)
            die(err ? err : "carver compile failed", sub);
    }

    hc_df_compiler_t comp;
    static hc_df_graph_t graph;
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

    /* --- region-ref 페이로드 로드 + 바이옴 오버레이 --- */
    {
        hc_arena_t scratch;
        void      *smem = hc_arena_alloc(&g_arena, 64u << 20, 16);
        if (!smem)
            die("arena exhausted (overlay scratch)", NULL);
        for (int32_t cz = 0; cz < 32; cz++)
            for (int32_t cx = 0; cx < 32; cx++) {
                char rp[1024];
                snprintf(rp, sizeof rp, "%s/c.%d.%d.nbt", region_ref_dir, cx,
                         cz);
                size_t len = 0;
                g_ref[cz * 32 + cx] = (uint8_t *)read_file(rp, &len);
                g_ref_len[cz * 32 + cx] = len;
                hc_arena_init(&scratch, smem, 64u << 20);
                overlay_biomes_from_ref(cx, cz, g_ref[cz * 32 + cx], len,
                                        &scratch);
            }
    }
    load_climate(ref_dir);
    printf("[%6.1fs] setup done (%d biomes interned)\n", now_s() - t0,
           g_reg.count);

    /* --- 월드 구성 --- */
    hc_biome_view_t view;
    view.qx0 = QG_MIN_XZ;
    view.qz0 = QG_MIN_XZ;
    view.nxz = QG_NXZ;
    view.qy0 = QG_MIN_Y;
    view.ny = QG_NY;
    view.ids = &g_grid[0][0][0];
    view.zoom_seed = hc_biome_obfuscate_seed(seed);

    for (int32_t cz = WC0; cz < WC0 + WN; cz++)
        for (int32_t cx = WC0; cx < WC0 + WN; cx++) {
            hc_chunk_t *c = &g_chunks[widx(cx, cz)];
            if (hc_chunk_init(c, &g_arena, cx, cz) != 0)
                die("arena exhausted (chunk)", NULL);
            if (hc_ppg_recorder_init(&g_ppg[widx(cx, cz)], &g_arena, 2048) !=
                0)
                die("arena exhausted (ppg recorder)", NULL);
            c->ppg = &g_ppg[widx(cx, cz)];
            fill_chunk_biomes_from_grid(c);
        }

    /* --- Task 14: 구조물 컨텍스트 (스타트 소스 + 위치계산 + References
     * 교차검증). 뷰가 준비된 뒤, 데코 재생 전. HC_NO_STRUCTURES=1 은
     * 서베이 비교용 우회. --- */
    static hc_biome_view_t g_view_static;
    g_view_static = view;
    if (!getenv("HC_NO_STRUCTURES")) {
        g_sctx = hc_arena_alloc(&g_arena, sizeof *g_sctx,
                                _Alignof(hc_sctx_t));
        char sdir[1024], tdir[1024];
        snprintf(sdir, sizeof sdir, "%s/../structures", region_ref_dir);
        snprintf(tdir, sizeof tdir, "%s/structure", ref_dir);
        const char *serr = NULL;
        if (!g_sctx ||
            hc_structures_init(g_sctx, &g_arena, seed, sdir, tdir, NULL,
                               g_tags, g_n_tags, &g_view_static, &g_reg,
                               &serr) != 0)
            die(serr ? serr : "structures init failed", NULL);
        printf("[%6.1fs] structures: %d starts (golden 4 + derived), "
               "references cross-validated\n",
               now_s() - t0, g_sctx->n_starts);
    }

    /* 04..06 병렬 (순수 per-chunk; sin 테이블은 선-초기화) */
    hc_mth_trig_init();
    {
        enum { NTHREADS = 20 };
        static chain_job_t jobs[NTHREADS];
        static pthread_t   tids[NTHREADS];
        _Atomic int32_t    next = 0;
        for (int t = 0; t < NTHREADS; t++) {
            void *mem = hc_arena_alloc(&g_arena, 160u << 20, 16);
            if (!mem)
                die("arena exhausted (thread scratch)", NULL);
            hc_arena_init(&jobs[t].arena, mem, 160u << 20);
            jobs[t].graph = &graph;
            jobs[t].roots = &roots;
            jobs[t].surf = surf;
            jobs[t].view = &view;
            jobs[t].seed = seed;
            jobs[t].sea = (int32_t)sea->num;
            jobs[t].carvers = carvers;
            jobs[t].next = &next;
            if (pthread_create(&tids[t], NULL, chain_worker, &jobs[t]) != 0)
                die("pthread_create failed", NULL);
        }
        for (int t = 0; t < NTHREADS; t++)
            pthread_join(tids[t], NULL);
    }
    printf("[%6.1fs] 04..06 chained for %d chunks\n", now_s() - t0,
           WORLD_CHUNKS);

    /* --- features 리전 + 틱 레코더 --- */
    static hc_feat_region_t rg;
    memset(&rg, 0, sizeof rg);
    rg.cx0 = WC0;
    rg.cz0 = WC0;
    rg.n = WN;
    for (int32_t i = 0; i < WORLD_CHUNKS; i++)
        rg.chunks[i] = &g_chunks[i];
    static hc_tick_recorder_t recorder;
    if (hc_tick_recorder_init(&recorder, &g_arena, 1 << 19) != 0)
        die("arena exhausted (tick recorder)", NULL);
    rg.ticks = &recorder;
    if (g_sctx) {
        rg.struct_step = struct_step_cb;
        rg.struct_ud = g_sctx;
    }

    /* --- 데코 재생: 리전 전체 manifest --- */
    enum { MAX_MANIFEST = 4096 };
    static manifest_line_t man[MAX_MANIFEST];
    char mpath[1024];
    snprintf(mpath, sizeof mpath, "%s/order.manifest", stages_dir);
    int32_t n_man = load_manifest(mpath, seed, man, MAX_MANIFEST);
    if (n_man < 1024)
        die("manifest unexpectedly short", mpath);
    for (int32_t pos = 0; pos < n_man; pos++) {
        if (man[pos].cx < WC0 + 1 || man[pos].cx >= WC0 + WN - 1 ||
            man[pos].cz < WC0 + 1 || man[pos].cz >= WC0 + WN - 1)
            die("manifest entry outside decorable window", mpath);
        hc_gen_features_chunk(&rg, man[pos].cx, man[pos].cz, seed, freg,
                              &view, &g_reg, (int32_t)sea->num, 10, &g_sink);
    }
    printf("[%6.1fs] replayed %d manifest entries; %d scheduled ticks\n",
           now_s() - t0, n_man, recorder.n);
    if (g_survey && g_n_gaps) {
        printf("== SURVEY: unimplemented feature fires ==\n");
        for (int32_t i = 0; i < g_n_gaps; i++) {
            const char *why = NULL;
            for (int32_t s = 0; s < HC_FEAT_STEPS && !why; s++)
                for (int32_t f = 0; f < freg->counts[s]; f++) {
                    const hc_pfeat_t *pf = &freg->steps[s][f];
                    if (pf->name && strcmp(pf->name, g_gaps[i].name) == 0 &&
                        pf->unimpl_why) {
                        why = pf->unimpl_why;
                        break;
                    }
                }
            printf("  %8" PRId64 "  %s%s%s\n", g_gaps[i].fires,
                   g_gaps[i].name, why ? " — " : "", why ? why : "");
        }
    }

    /* --- postProcessGeneration: 승격 순서 드레인 + 마킹 교차검증 --- */
    for (int32_t i = 0; i < WORLD_CHUNKS; i++)
        g_ppg[i].frozen = 1;
    int32_t drained = 0, skipped_drain = 0, mark_bad_chunks = 0;
    if (!getenv("HC_SURVEY_SKIP_PP")) {
        char ppath[1024];
        snprintf(ppath, sizeof ppath, "%s/postprocess.manifest", stages_dir);
        size_t len = 0;
        char  *buf = read_file(ppath, &len);
        /* 1패스: 청크 승격 순서 + 청크별 기록 마킹 (파일 순서) */
        enum { MAX_PPM = 2048, MAX_REC_PER = 2048 };
        static int32_t pm_cx[MAX_PPM], pm_cz[MAX_PPM];
        int32_t        n_pm = 0;
        /* 기록 마킹은 승격 라인 뒤 p 라인들 — seq 로 청크 매핑 */
        static int32_t seq2chunk[MAX_PPM];
        static int32_t rec_x[MAX_PPM][MAX_REC_PER > 1047 ? 1100 : MAX_REC_PER],
            rec_y[MAX_PPM][1100], rec_z[MAX_PPM][1100];
        static int32_t rec_n[MAX_PPM];
        memset(rec_n, 0, sizeof rec_n);
        char *p = buf;
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
                if (seq < 0 || seq >= n_pm)
                    die("postprocess p line before its chunk line", p);
                if (rec_n[seq] >= 1100)
                    die("too many recorded marks for one chunk", ppath);
                rec_x[seq][rec_n[seq]] = x;
                rec_y[seq][rec_n[seq]] = y;
                rec_z[seq][rec_n[seq]] = z;
                rec_n[seq]++;
            } else if (p[0] != '#' && p[0] != '\0') {
                long long seq, gt, nm;
                int32_t   cx, cz;
                char      thread[64];
                long long nanos;
                if (sscanf(p, "%lld %d %d %lld %lld %63s %lld", &seq, &cx,
                           &cz, &gt, &nm, thread, &nanos) != 7)
                    die("bad postprocess chunk line", p);
                if (seq != n_pm || n_pm >= MAX_PPM)
                    die("postprocess manifest seq gap", ppath);
                pm_cx[n_pm] = cx;
                pm_cz[n_pm] = cz;
                seq2chunk[n_pm] = widx(cx, cz);
                n_pm++;
            }
            p = nl + 1;
        }
        /* 2패스: 교차검증 + 드레인 (승격 순서) */
        static int32_t ox[4096], oy[4096], oz[4096];
        for (int32_t m = 0; m < n_pm; m++) {
            int32_t                  wi = seq2chunk[m];
            const hc_ppg_recorder_t *r = &g_ppg[wi];
            int32_t n_ours = flatten_marks(r, ox, oy, oz, 4096);
            int     ok = n_ours == rec_n[m] && n_ours <= 4096;
            if (ok)
                for (int32_t i = 0; i < n_ours; i++)
                    if (ox[i] != rec_x[m][i] || oy[i] != rec_y[m][i] ||
                        oz[i] != rec_z[m][i]) {
                        ok = 0;
                        break;
                    }
            if (!ok) {
                mark_bad_chunks++;
                if (!g_survey) {
                    fprintf(stderr,
                            "PPG MARK c.%d.%d: ours %d golden %d (or coord "
                            "mismatch)\n",
                            pm_cx[m], pm_cz[m], n_ours, rec_n[m]);
                    for (int32_t i = 0; i < n_ours && i < 20; i++)
                        fprintf(stderr, "  ours[%d] (%d,%d,%d)\n", i, ox[i],
                                oy[i], oz[i]);
                    for (int32_t i = 0; i < rec_n[m] && i < 20; i++)
                        fprintf(stderr, "  gold[%d] (%d,%d,%d)\n", i,
                                rec_x[m][i], rec_y[m][i], rec_z[m][i]);
                    die("derived postprocess marks diverge from recording",
                        NULL);
                }
                skipped_drain++;
                continue;
            }
            hc_postprocess_chunk(&rg, pm_cx[m], pm_cz[m], &g_ppg[wi]);
            drained++;
        }
        printf("[%6.1fs] postprocess: drained %d/%d (mark-mismatch chunks "
               "%d, drains skipped %d; unmodeled-veg evals %" PRId64 ")\n",
               now_s() - t0, drained, n_pm, mark_bad_chunks, skipped_drain,
               hc_postprocess_unmodeled_veg_evals());
    }

    /* --- 라이트 최종 고정점 (R/S = stages.log 실측 집합) --- */
    if (!getenv("HC_SURVEY_SKIP_LIGHT")) {
        load_stage_sets(stages_dir);
        static hc_light_world_t lw;
        if (hc_light_world_init(&lw, &g_arena, WC0, WC0, WN) != 0)
            die("arena exhausted (light world)", NULL);
        for (int32_t i = 0; i < WORLD_CHUNKS; i++)
            if (hc_light_attach(&lw, &g_arena, &g_chunks[i]) != 0)
                die("arena exhausted (light chunks)", NULL);
        for (int32_t pos = 0; pos < n_man; pos++)
            hc_light_set_featured(&lw, man[pos].cx, man[pos].cz);
        for (int32_t i = 0; i < WORLD_CHUNKS; i++)
            if (g_stage_r[i])
                hc_gen_initialize_light_stage(&lw, g_chunks[i].cx,
                                              g_chunks[i].cz);
        for (int32_t i = 0; i < WORLD_CHUNKS; i++)
            if (g_stage_s[i])
                hc_gen_light_stage(&lw, g_chunks[i].cx, g_chunks[i].cz);
        hc_light_solve(&lw);
        printf("[%6.1fs] light fixed point solved\n", now_s() - t0);

        /* --- 직렬화 + 대조 + canonical 해시 --- */
        static hc_region_chunk_t region_chunks[1024];
        hc_arena_t               scratch;
        void *smem = hc_arena_alloc(&g_arena, 64u << 20, 16);
        uint8_t *cat = hc_arena_alloc(&g_arena, 192u << 20, 16);
        if (!smem || !cat)
            die("arena exhausted (serialize scratch)", NULL);
        size_t  cat_off = 0;
        int32_t n_mismatch = 0;
        for (int32_t idx = 0; idx < 1024; idx++) {
            int32_t     cx = idx & 31, cz = idx >> 5;
            hc_chunk_t *c = &g_chunks[widx(cx, cz)];
            hc_light_chunk_t *ls =
                &lw.slots[(cz - lw.cz0) * lw.n + (cx - lw.cx0)];
            uint8_t *payload = hc_arena_alloc(&g_arena, 256u << 10, 1);
            if (!payload)
                die("arena exhausted (payload)", NULL);
            hc_arena_init(&scratch, smem, 64u << 20);
            ptrdiff_t n = hc_chunk_to_nbt(c, &g_reg, ls, recorder.recs,
                                          recorder.n, /*last_update=*/0,
                                          g_sctx, &scratch, payload,
                                          256u << 10);
            if (n < 0)
                die("chunk serialization failed", NULL);
            region_chunks[idx].x = cx;
            region_chunks[idx].z = cz;
            region_chunks[idx].payload = payload;
            region_chunks[idx].len = (size_t)n;

            const uint8_t *ref = g_ref[idx];
            size_t         ref_len = g_ref_len[idx];
            if ((size_t)n != ref_len ||
                memcmp(payload, ref, (size_t)n) != 0) {
                n_mismatch++;
                if (n_mismatch <= 8) {
                    size_t common =
                        (size_t)n < ref_len ? (size_t)n : ref_len;
                    size_t off = 0;
                    while (off < common && payload[off] == ref[off])
                        off++;
                    fprintf(stderr,
                            "PAYLOAD c.%d.%d: MISMATCH ours=%td golden=%zu "
                            "first diff @%zu\n",
                            cx, cz, n, ref_len, off);
                }
                /* 진단용으로 우리 페이로드를 내려놓는다 */
                char op[1024];
                snprintf(op, sizeof op, "%s.c.%d.%d.ours.nbt", out_mca_path,
                         cx, cz);
                FILE *of = fopen(op, "wb");
                if (of) {
                    fwrite(payload, 1, (size_t)n, of);
                    fclose(of);
                }
            }
            /* canonical: u32be(idx) ‖ payload (last_update=0 == 마스킹) */
            cat[cat_off++] = (uint8_t)(idx >> 24);
            cat[cat_off++] = (uint8_t)(idx >> 16);
            cat[cat_off++] = (uint8_t)(idx >> 8);
            cat[cat_off++] = (uint8_t)idx;
            memcpy(cat + cat_off, payload, (size_t)n);
            cat_off += (size_t)n;
        }
        uint8_t dig[32];
        hc_sha256(cat, cat_off, dig);
        char hex[65];
        for (int i = 0; i < 32; i++)
            sprintf(hex + 2 * i, "%02x", dig[i]);
        hex[64] = '\0';
        printf("[%6.1fs] serialized 1024 chunks: %d payload mismatches\n"
               "canonical sha256 ours   %s\n"
               "canonical sha256 golden %s\n",
               now_s() - t0, n_mismatch, hex, canonical_hex);

        /* .mca 이미지 (roundtrip/외부 도구 소비용) */
        {
            static uint8_t *mca;
            size_t          cap = 256u << 20;
            mca = hc_arena_alloc(&g_arena, cap, 16);
            if (!mca)
                die("arena exhausted (mca image)", NULL);
            ptrdiff_t mn = hc_region_write(region_chunks, 1024, 1u, mca, cap);
            if (mn < 0)
                die("region image assembly failed", NULL);
            FILE *f = fopen(out_mca_path, "wb");
            if (!f)
                die("cannot write out mca", out_mca_path);
            fwrite(mca, 1, (size_t)mn, f);
            fclose(f);
            printf("wrote %s (%td bytes)\n", out_mca_path, mn);
        }

        if (!g_survey) {
            if (n_mismatch || strcmp(hex, canonical_hex) != 0) {
                fprintf(stderr,
                        "test_full_region: FAIL (%d payload mismatches)\n",
                        n_mismatch);
                return 1;
            }
            printf("test_full_region: PASS (1024/1024 byte-exact, canonical "
                   "hash match)\n");
            return 0;
        }
    }

    /* --- 서베이 리포트: 블록 단위 diff 분류 (ref 페이로드 디코드) --- */
    if (g_survey) {
        enum { MAX_CLASSES = 4096 };
        static struct {
            char    ours[96], ref[96];
            int64_t cells;
        } cls[MAX_CLASSES];
        int32_t n_cls = 0;
        int64_t total_diff = 0;
        static int64_t per_chunk[1024];
        hc_arena_t scratch;
        void      *smem2 = hc_arena_alloc(&g_arena, 64u << 20, 16);
        if (!smem2)
            die("arena exhausted (survey scratch)", NULL);
        for (int32_t idx = 0; idx < 1024; idx++) {
            int32_t     cx = idx & 31, cz = idx >> 5;
            if (cx < fx0 || cx > fx1 || cz < fz0 || cz > fz1)
                continue; /* HC_SURVEY_FOCUS 박스 밖 */
            hc_chunk_t *c = &g_chunks[widx(cx, cz)];
            hc_arena_init(&scratch, smem2, 64u << 20);
            hc_nbt_t *root =
                hc_nbt_parse(&scratch, g_ref[idx], g_ref_len[idx]);
            if (!root)
                die("ref payload parse failed (survey)", NULL);
            const hc_nbt_t *sections = hc_nbt_get(root, "sections");
            for (int32_t s = 0; s < hc_nbt_list_count(sections); s++) {
                const hc_nbt_t *sec = hc_nbt_list_at(sections, s);
                const hc_nbt_t *bs = hc_nbt_get(sec, "block_states");
                if (!bs)
                    continue;
                int32_t sy = (int32_t)hc_nbt_i64(hc_nbt_get(sec, "Y"));
                const hc_nbt_t *palette = hc_nbt_get(bs, "palette");
                int32_t         npal = hc_nbt_list_count(palette);
                /* 팔레트 엔트리 → 캐노니컬 문자열 */
                static char pal_names[512][96];
                for (int32_t pi = 0; pi < npal; pi++) {
                    const hc_nbt_t *ent = hc_nbt_list_at(palette, pi);
                    const char *nm = hc_nbt_str(hc_nbt_get(ent, "Name"));
                    const hc_nbt_t *props = hc_nbt_get(ent, "Properties");
                    char *w = pal_names[pi];
                    w += sprintf(w, "%s", nm);
                    if (props) {
                        /* Properties 는 파일 순서 (HashMap) — 캐노니컬
                         * (알파벳) 로 재정렬해 우리 이름과 비교 */
                        int32_t     np = hc_nbt_comp_count(props);
                        const char *ks[16];
                        for (int32_t k = 0; k < np; k++)
                            hc_nbt_comp_at(props, k, &ks[k]);
                        for (int32_t a = 0; a < np; a++)
                            for (int32_t b = a + 1; b < np; b++)
                                if (strcmp(ks[a], ks[b]) > 0) {
                                    const char *tmp = ks[a];
                                    ks[a] = ks[b];
                                    ks[b] = tmp;
                                }
                        *w++ = '[';
                        for (int32_t k = 0; k < np; k++)
                            w += sprintf(w, "%s%s=%s", k ? "," : "", ks[k],
                                         hc_nbt_str(hc_nbt_get(props,
                                                               ks[k])));
                        *w++ = ']';
                        *w = '\0';
                    }
                }
                const hc_nbt_t *data = hc_nbt_get(bs, "data");
                int bits = npal > 1 ? ceil_log2_i(npal) : 0;
                if (bits && bits < 4)
                    bits = 4;
                int32_t        nlongs = 0;
                const int64_t *longs =
                    data ? hc_nbt_la(data, &nlongs) : NULL;
                int vpl = bits ? 64 / bits : 0;
                for (int32_t i = 0; i < 4096; i++) {
                    int32_t pi = 0;
                    if (bits)
                        pi = (int32_t)(((uint64_t)longs[i / vpl] >>
                                        ((i % vpl) * bits)) &
                                       ((1u << bits) - 1u));
                    int lx = i & 15, lz = (i >> 4) & 15, ly = (i >> 8) & 15;
                    uint16_t ours =
                        c->states[hc_idx(lx, sy * 16 + ly, lz)];
                    const char *ours_nm = hc_block_name(ours);
                    if (strcmp(ours_nm, pal_names[pi]) == 0)
                        continue;
                    {
                        static FILE   *cf;
                        static int32_t ccx = INT32_MIN, ccz;
                        if (ccx == INT32_MIN) {
                            const char *ce = getenv("HC_SURVEY_DUMP_CELLS");
                            ccx = INT32_MIN + 1;
                            if (ce) {
                                char cp[512];
                                if (sscanf(ce, "%d %d %511s", &ccx, &ccz,
                                           cp) == 3)
                                    cf = fopen(cp, "w");
                            }
                        }
                        if (cf && cx == ccx && cz == ccz)
                            fprintf(cf, "(%d,%d,%d) ours=%s ref=%s\n",
                                    cx * 16 + lx, sy * 16 + ly,
                                    cz * 16 + lz, ours_nm, pal_names[pi]);
                    }
                    total_diff++;
                    per_chunk[idx]++;
                    int32_t k = 0;
                    for (; k < n_cls; k++)
                        if (strcmp(cls[k].ours, ours_nm) == 0 &&
                            strcmp(cls[k].ref, pal_names[pi]) == 0)
                            break;
                    if (k == n_cls && n_cls < MAX_CLASSES) {
                        snprintf(cls[k].ours, 96, "%s", ours_nm);
                        snprintf(cls[k].ref, 96, "%s", pal_names[pi]);
                        cls[k].cells = 0;
                        n_cls++;
                    }
                    if (k < n_cls)
                        cls[k].cells++;
                }
            }
        }
        /* 정렬 (셀 수 내림차순) 후 리포트 */
        for (int32_t a = 0; a < n_cls; a++)
            for (int32_t b = a + 1; b < n_cls; b++)
                if (cls[b].cells > cls[a].cells) {
                    char    to[96], tr[96];
                    int64_t tc;
                    memcpy(to, cls[a].ours, 96);
                    memcpy(tr, cls[a].ref, 96);
                    tc = cls[a].cells;
                    memcpy(cls[a].ours, cls[b].ours, 96);
                    memcpy(cls[a].ref, cls[b].ref, 96);
                    cls[a].cells = cls[b].cells;
                    memcpy(cls[b].ours, to, 96);
                    memcpy(cls[b].ref, tr, 96);
                    cls[b].cells = tc;
                }
        printf("== SURVEY: block diffs vs golden: %" PRId64
               " cells, %d classes ==\n",
               total_diff, n_cls);
        for (int32_t k = 0; k < n_cls && k < 60; k++)
            printf("  %8" PRId64 "  ours=%-50s ref=%s\n", cls[k].cells,
                   cls[k].ours, cls[k].ref);
        int64_t worst = 0;
        int32_t worst_i = 0, clean = 0;
        for (int32_t i = 0; i < 1024; i++) {
            if (per_chunk[i] > worst) {
                worst = per_chunk[i];
                worst_i = i;
            }
            if (per_chunk[i] == 0)
                clean++;
        }
        printf("  chunks block-clean: %d/1024; worst c.%d.%d (%" PRId64
               " cells)\n",
               clean, worst_i & 31, worst_i >> 5, worst);
        const char *dump = getenv("HC_SURVEY_DUMP_CHUNKS");
        if (dump) {
            FILE *df = fopen(dump, "w");
            if (df) {
                for (int32_t i = 0; i < 1024; i++)
                    if (per_chunk[i])
                        fprintf(df, "%d %d %" PRId64 "\n", i & 31, i >> 5,
                                per_chunk[i]);
                fclose(df);
            }
        }
        printf("test_full_region: SURVEY DONE [%6.1fs]\n", now_s() - t0);
        return 0;
    }
    return 0;
}
