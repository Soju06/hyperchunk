/* hyperchunk-bench — P2-0 베이스라인 측정 하네스.
 *
 *   hyperchunk-bench --seed 1234567890 --region 0 0 \
 *       [--repo <root>] [--threads <n>]
 *
 * cli/hyperchunk_verify.c 의 풀-리전 파이프라인(소스 오브 트루스 =
 * tests/parity/test_full_region.c)과 동일 시맨틱에 clock_gettime 계측만
 * 얹는다. 코어 알고리즘·호출 순서는 verify 와 바이트 단위로 같아야 하며,
 * 매 실행 canonical 페이로드 해시를 golden/SHA256SUMS 와 대조해 계측이
 * 패리티를 깨지 않았음을 판정한다 (해시 불일치 = exit 1).
 *
 * 계측 모델:
 *  - 체인 스테이지(04 noise / 07 surface / 08 carvers)는 워커 스레드별로
 *    CLOCK_MONOTONIC(wall)과 CLOCK_THREAD_CPUTIME_ID(cpu)를 스테이지
 *    양쪽에서 읽어 누적한다. VM steal/선점에 강건한 비중 판단은 cpu 합,
 *    처리량 판단은 체인 페이즈 wall 로 한다.
 *  - 09 피처/라이트/포스트프로세스/직렬화는 단일 스레드라 wall 누적만
 *    한다 (wall ≈ cpu; steal 은 반복 측정 편차로 드러난다).
 *  - 리플레이 입력 파싱(order/postprocess manifest, stages.log)과 마크
 *    대조는 하네스 오버헤드 버킷으로 분리한다 — 실제 생성기 비용이 아님.
 *
 * 출력: stderr 사람용 표, stdout JSON 1줄 (bench/run_bench.sh 가 수집).
 * exit 0 = PASS, 1 = 해시 불일치, 2 = 셋업 실패. */

#undef NDEBUG
#define _POSIX_C_SOURCE 200809L

#include "../core/src/hc_carvers.h"
#include "../core/src/hc_chunk_nbt.h"
#include "../core/src/hc_features.h"
#include "../core/src/hc_light.h"
#include "../core/src/hc_nbt.h"
#include "../core/src/hc_postprocess.h"
#include "../core/src/hc_region.h"
#include "../core/src/hc_sha256.h"
#include "../core/src/hc_structures.h"

#include <assert.h>
#include <dirent.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

/* ---------------- 계측 ---------------- */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t tcpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* 체인 워커의 스테이지별 누적 (스레드 로컬 → join 후 합산) */
typedef struct {
    uint64_t nc_init, beard, noise, surface, carvers;
} stage_acc_t;

/* 단일 스레드 페이즈 버킷 (wall ns) */
static uint64_t B_light_setup, B_light08, B_light09, B_light_flush;
static uint64_t B_features, B_pp, B_pp_verify, B_light_final;
static uint64_t B_serialize, B_sha256, B_replay_load;

static hc_sctx_t *g_sctx = NULL;

static void struct_step_cb(void *ud, hc_feat_region_t *rg,
                           const struct hc_feat_reg *reg, int32_t sea_level,
                           int32_t cx, int32_t cz, int64_t deco_seed,
                           int32_t step) {
    hc_structures_step((hc_sctx_t *)ud, rg, (const hc_feat_reg_t *)reg,
                       sea_level, cx, cz, deco_seed, step);
}

static void die(const char *msg, const char *detail) {
    fprintf(stderr, "hyperchunk-bench: %s%s%s\n", msg, detail ? ": " : "",
            detail ? detail : "");
    exit(2);
}

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

enum { WC0 = -5, WN = 41, WORLD_CHUNKS = WN * WN };

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

static uint8_t *g_ref[32 * 32];
static size_t   g_ref_len[32 * 32];

static int ceil_log2_i(int32_t n) {
    int b = 0;
    while ((1 << b) < n)
        b++;
    return b;
}

static void overlay_biomes_from_ref(int32_t cx, int32_t cz,
                                    const uint8_t *payload, size_t len,
                                    hc_arena_t *scratch,
                                    int tolerate_missing) {
    hc_nbt_t *root = hc_nbt_parse(scratch, payload, len);
    if (!root)
        die("region-ref payload parse failed", NULL);
    const hc_nbt_t *sections = hc_nbt_get(root, "sections");
    if (!sections) {
        if (tolerate_missing)
            return;
        die("region-ref payload missing sections", NULL);
    }
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
                int            bits = ceil_log2_i(npal);
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
    char buf[192];
    snprintf(buf, sizeof buf, "%s (step %d index %d, npos %d)", name, step,
             index, npos);
    die("UNIMPLEMENTED feature body fired in replay", buf);
}

static const hc_feat_trace_t g_sink = {sink_pos_nop, sink_feature, NULL};

typedef struct {
    hc_arena_t              arena;
    const hc_df_graph_t    *graph;
    const hc_noise_roots_t *roots;
    hc_surface_t           *surf;
    const hc_biome_view_t  *view;
    int64_t                 seed;
    int32_t                 sea;
    const hc_carver_t      *carvers;
    _Atomic int32_t        *next;
    stage_acc_t             wall; /* CLOCK_MONOTONIC 누적 */
    stage_acc_t             cpu;  /* CLOCK_THREAD_CPUTIME_ID 누적 */
    int32_t                 chunks_done;
    char pad[64]; /* 워커 간 누적 필드 false sharing 차단 */
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
        uint64_t    w0 = now_ns(), c0 = tcpu_ns();
        hc_noise_chunk_t *nc = hc_arena_alloc(&job->arena, sizeof *nc,
                                              _Alignof(hc_noise_chunk_t));
        if (!nc || hc_nc_init(nc, &job->arena, job->graph, job->roots,
                              job->seed, c->cx, c->cz, job->sea) != 0)
            die("thread arena exhausted (noise chunk)", NULL);
        uint64_t w1 = now_ns(), c1 = tcpu_ns();
        job->wall.nc_init += w1 - w0;
        job->cpu.nc_init += c1 - c0;
        hc_beard_t beard;
        if (g_sctx &&
            hc_structures_beard(g_sctx, &job->arena, c->cx, c->cz, &beard))
            nc->beard = &beard;
        uint64_t w2 = now_ns(), c2 = tcpu_ns();
        job->wall.beard += w2 - w1;
        job->cpu.beard += c2 - c1;
        hc_gen_noise_stage(c, nc);
        uint64_t w3 = now_ns(), c3 = tcpu_ns();
        job->wall.noise += w3 - w2;
        job->cpu.noise += c3 - c2;
        hc_gen_surface_stage(c, nc, job->surf, job->view);
        uint64_t w4 = now_ns(), c4 = tcpu_ns();
        job->wall.surface += w4 - w3;
        job->cpu.surface += c4 - c3;
        uint64_t mask[HC_CARVING_MASK_WORDS];
        memset(mask, 0, sizeof mask);
        hc_gen_carvers_stage(c, nc, job->surf, job->view, job->seed,
                             job->carvers, 3, mask);
        uint64_t w5 = now_ns(), c5 = tcpu_ns();
        job->wall.carvers += w5 - w4;
        job->cpu.carvers += c5 - c4;
        job->chunks_done++;
        job->arena.off = mark;
    }
}

static hc_ppg_recorder_t g_ppg[WORLD_CHUNKS];

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

/* --- 라이브-창 라이트 훅 + 09 배치 모델 (hyperchunk_verify.c 와 동일;
 * 소스 오브 트루스 = tests/parity/test_full_region.c) --- */
static hc_light_world_t *g_lw_hook;
static int32_t           g_cur_pos;
static int32_t           g_live_at[WORLD_CHUNKS];

static void light_write_hook(void *ud, int32_t x, int32_t y, int32_t z,
                             uint16_t old_id, uint16_t new_id) {
    (void)ud;
    if (!g_lw_hook)
        return;
    int32_t la = g_live_at[widx(x >> 4, z >> 4)];
    if (la == INT32_MAX || g_cur_pos < la)
        return; /* 동결 창 / 파이프라인 밖 */
    hc_light_accum_write(g_lw_hook, x, y, z, old_id, new_id);
}

static int raw_brightness_cb(void *ud, int32_t x, int32_t y, int32_t z) {
    (void)ud;
    if (!g_lw_hook)
        return 0;
    int32_t cx = x >> 4, cz = z >> 4;
    int     vis = 0;
    for (int dz = -1; dz <= 1 && !vis; dz++)
        for (int dx = -1; dx <= 1 && !vis; dx++) {
            int32_t la = g_live_at[widx(cx + dx, cz + dz)];
            if (la != INT32_MAX && la <= g_cur_pos)
                vis = 1;
        }
    if (!vis)
        return 15;
    int sky = hc_light_get(g_lw_hook, HC_LIGHT_SKY, x, y, z);
    int blk = hc_light_get(g_lw_hook, HC_LIGHT_BLOCK, x, y, z);
    return sky > blk ? sky : blk;
}

typedef struct {
    uint8_t kind; /* 0 = 08 initialize_light, 1 = 09 light */
    int32_t cx, cz;
    int32_t sub_seq;
    int64_t sub_nanos;
    int64_t comp_nanos;
    int32_t batch;
} ltask_t;

enum { TL_CAP = 1 << 16 };
static int64_t g_tl_nanos[TL_CAP];
static int32_t g_tl_seq[TL_CAP];
static int32_t g_tl_n;
static int64_t g_batch_drain[TL_CAP];
static int32_t g_nbatch;

static int32_t counter_at(int64_t t) {
    int32_t lo = 0, hi = g_tl_n;
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (g_tl_nanos[mid] <= t)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo == 0 ? 0 : g_tl_seq[lo - 1];
}

static int32_t load_light_tasks(const char *path, ltask_t *out, int32_t cap) {
    size_t  len = 0;
    char   *buf = read_file(path, &len);
    char   *p = buf;
    int32_t n = 0;
    g_tl_n = 0;
    g_nbatch = 0;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (!nl)
            break;
        *nl = '\0';
        if (p[0] != '#' && p[0] != '\0') {
            int       si, kind = -1, is_sub = 0;
            char      sname[64];
            int32_t   cx, cz;
            long long seq, nanos;
            const char *body = p;
            if (p[0] == 's' && p[1] == ' ') {
                is_sub = 1;
                body = p + 2;
            }
            if (sscanf(body, "%d %63s %d %d %lld %lld", &si, sname, &cx, &cz,
                       &seq, &nanos) != 6)
                die("bad stages.log line", p);
            if (strcmp(sname, "initialize_light") == 0)
                kind = 0;
            else if (strcmp(sname, "light") == 0)
                kind = 1;
            if (g_tl_n >= TL_CAP)
                die("stage event timeline exceeds cap", path);
            g_tl_nanos[g_tl_n] = nanos;
            g_tl_seq[g_tl_n] = (int32_t)seq;
            g_tl_n++;
            if (kind >= 0) {
                if (is_sub) {
                    if (n >= cap)
                        die("stages.log tasks exceed cap", path);
                    out[n].kind = (uint8_t)kind;
                    out[n].cx = cx;
                    out[n].cz = cz;
                    out[n].sub_seq = (int32_t)seq;
                    out[n].sub_nanos = nanos;
                    out[n].comp_nanos = -1;
                    out[n].batch = -1;
                    n++;
                } else {
                    int32_t t = -1;
                    for (int32_t i = n - 1; i >= 0; i--)
                        if (out[i].kind == kind && out[i].cx == cx &&
                            out[i].cz == cz) {
                            t = i;
                            break;
                        }
                    if (t < 0 || out[t].comp_nanos >= 0)
                        die("stages.log completion without submit", p);
                    out[t].comp_nanos = nanos;
                }
            }
        }
        p = nl + 1;
    }
    for (int32_t a = 1; a < g_tl_n; a++) {
        int64_t kn = g_tl_nanos[a];
        int32_t ks = g_tl_seq[a];
        int32_t b = a - 1;
        while (b >= 0 && g_tl_nanos[b] > kn) {
            g_tl_nanos[b + 1] = g_tl_nanos[b];
            g_tl_seq[b + 1] = g_tl_seq[b];
            b--;
        }
        g_tl_nanos[b + 1] = kn;
        g_tl_seq[b + 1] = ks;
    }
    for (int32_t a = 1; a < n; a++) {
        ltask_t key = out[a];
        int32_t b = a - 1;
        while (b >= 0 && out[b].sub_nanos > key.sub_nanos) {
            out[b + 1] = out[b];
            b--;
        }
        out[b + 1] = key;
    }
    int32_t nb = 0, i = 0;
    int64_t t = -1;
    while (i < n) {
        if (out[i].kind != 1) {
            out[i].batch = -1;
            i++;
            continue;
        }
        if (out[i].sub_nanos > t)
            t = out[i].sub_nanos + 2000000; /* δ_wake = 2ms */
        if (nb >= TL_CAP)
            die("light batch count exceeds cap", path);
        g_batch_drain[nb] = t;
        int64_t end = t;
        while (i < n && (out[i].kind != 1 || out[i].sub_nanos <= t)) {
            if (out[i].kind != 1) {
                out[i].batch = -1;
                i++;
                continue;
            }
            out[i].batch = nb;
            if (out[i].comp_nanos > end)
                end = out[i].comp_nanos;
            i++;
        }
        t = end;
        nb++;
    }
    enum { T_PRE_MIN_NANOS = 2000000 };
    for (int32_t b = 1; b < nb; b++) {
        int64_t prev_last_post = -1, cur_first_post = -1;
        for (int32_t k = 0; k < n; k++) {
            if (out[k].kind != 1 || out[k].comp_nanos < 0)
                continue;
            if (out[k].batch < b && out[k].comp_nanos > prev_last_post)
                prev_last_post = out[k].comp_nanos;
            if (out[k].batch == b &&
                (cur_first_post < 0 || out[k].comp_nanos < cur_first_post))
                cur_first_post = out[k].comp_nanos;
        }
        if (prev_last_post < 0 || cur_first_post < 0)
            continue;
        if (cur_first_post - prev_last_post >= T_PRE_MIN_NANOS)
            continue;
        int64_t drain2 = g_batch_drain[b - 1];
        for (int32_t k = 0; k < n; k++)
            if (out[k].batch == b && out[k].kind == 1 &&
                out[k].sub_nanos > drain2)
                drain2 = out[k].sub_nanos;
        for (int32_t k = 0; k < n; k++) {
            if (out[k].batch == b) {
                out[k].batch = out[k].sub_nanos <= drain2 ? b - 1 : b;
            } else if (out[k].batch > b) {
                out[k].batch -= 1;
            }
        }
        g_batch_drain[b - 1] = drain2;
        for (int32_t k = b; k + 1 < nb; k++)
            g_batch_drain[k] = g_batch_drain[k + 1];
        nb--;
        b--;
    }
    g_nbatch = nb;
    return n;
}

static const char *expected_canonical(const char *repo, int64_t seed) {
    char path[1024];
    snprintf(path, sizeof path, "%s/golden/SHA256SUMS", repo);
    char  *buf = read_file(path, NULL);
    char   want[128];
    snprintf(want, sizeof want, "seed%" PRId64 "_r.0.0.mca#canonical-payload",
             seed);
    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = '\0';
        if (strstr(p, want)) {
            static char hex[65];
            memcpy(hex, p, 64);
            hex[64] = '\0';
            return hex;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    die("canonical-payload line missing from golden/SHA256SUMS", want);
    return NULL;
}

enum { MAX_THREADS = 64 };

int main(int argc, char **argv) {
    int64_t     seed = 0;
    int         have_seed = 0;
    int32_t     rx = 0, rz = 0;
    int         have_region = 0;
    const char *repo = ".";
    int         nthreads = 20;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = strtoll(argv[++i], NULL, 10);
            have_seed = 1;
        } else if (strcmp(argv[i], "--region") == 0 && i + 2 < argc) {
            rx = (int32_t)strtol(argv[++i], NULL, 10);
            rz = (int32_t)strtol(argv[++i], NULL, 10);
            have_region = 1;
        } else if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) {
            repo = argv[++i];
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            nthreads = (int)strtol(argv[++i], NULL, 10);
        } else {
            die("usage: hyperchunk-bench --seed <s> --region <x> <z> "
                "[--repo <root>] [--threads <n>]",
                argv[i]);
        }
    }
    if (!have_seed || !have_region)
        die("--seed and --region are required", NULL);
    if (rx != 0 || rz != 0)
        die("phase 1 scope: only --region 0 0 is supported", NULL);
    if (nthreads < 1 || nthreads > MAX_THREADS)
        die("--threads out of range [1,64]", NULL);

    uint64_t t_proc0 = now_ns();

    size_t backing_sz = (size_t)6 << 30;
    if (nthreads > 20)
        backing_sz += (size_t)(nthreads - 20) * (160u << 20);
    void *backing = malloc(backing_sz);
    if (!backing)
        die("cannot allocate backing", NULL);
    hc_arena_init(&g_arena, backing, backing_sz);
    hc_biome_reg_init(&g_reg, &g_arena);

    char ref_dir[512], stages_dir[512], band_golden[512],
        region_ref_dir[512];
    snprintf(ref_dir, sizeof ref_dir, "%s/reference", repo);
    snprintf(stages_dir, sizeof stages_dir,
             "%s/golden/stages/seed%" PRId64, repo, seed);
    snprintf(band_golden, sizeof band_golden,
             "%s/golden/rng/biome_band_region_seed%" PRId64 ".txt", repo,
             seed);
    snprintf(region_ref_dir, sizeof region_ref_dir, "%s/golden/region-ref",
             repo);
    const char *canonical_hex = expected_canonical(repo, seed);

    char sub[2048];
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
                                        &scratch, 0);
            }
        /* 마진 링 — 이웃 리전 캡처의 기록 바이옴 (test_full_region.c
         * 와 동일; extract_margin_biomes.py 산출) */
        for (int32_t cz = WC0; cz < WC0 + WN; cz++)
            for (int32_t cx = WC0; cx < WC0 + WN; cx++) {
                if (cx >= 0 && cx < 32 && cz >= 0 && cz < 32)
                    continue;
                char rp[1024];
                snprintf(rp, sizeof rp, "%s-margin/c.%d.%d.nbt",
                         region_ref_dir, cx, cz);
                size_t len = 0;
                char  *buf = read_file(rp, &len);
                hc_arena_init(&scratch, smem, 64u << 20);
                overlay_biomes_from_ref(cx, cz, (const uint8_t *)buf, len,
                                        &scratch, 1);
            }
    }
    load_climate(ref_dir);

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

    static hc_biome_view_t g_view_static;
    g_view_static = view;
    {
        g_sctx =
            hc_arena_alloc(&g_arena, sizeof *g_sctx, _Alignof(hc_sctx_t));
        char sdir[1024], tdir[1024];
        snprintf(sdir, sizeof sdir, "%s/golden/structures", repo);
        snprintf(tdir, sizeof tdir, "%s/structure", ref_dir);
        const char *serr = NULL;
        if (!g_sctx ||
            hc_structures_init(g_sctx, &g_arena, seed, sdir, tdir, NULL,
                               g_tags, g_n_tags, &g_view_static, &g_reg,
                               &serr) != 0)
            die(serr ? serr : "structures init failed", NULL);
    }

    hc_mth_trig_init();

    uint64_t t_setup_end = now_ns();

    /* ---- 체인 페이즈: 04 noise / 07 surface / 08 carvers ---- */
    static chain_job_t jobs[MAX_THREADS];
    {
        static pthread_t tids[MAX_THREADS];
        _Atomic int32_t  next = 0;
        for (int t = 0; t < nthreads; t++) {
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
            memset(&jobs[t].wall, 0, sizeof jobs[t].wall);
            memset(&jobs[t].cpu, 0, sizeof jobs[t].cpu);
            jobs[t].chunks_done = 0;
            if (pthread_create(&tids[t], NULL, chain_worker, &jobs[t]) != 0)
                die("pthread_create failed", NULL);
        }
        for (int t = 0; t < nthreads; t++)
            pthread_join(tids[t], NULL);
    }
    uint64_t t_chain_end = now_ns();

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
    rg.struct_step = struct_step_cb;
    rg.struct_ud = g_sctx;
    rg.be = &g_sctx->be;   /* monster_room BE 기록 (Task 14) */
    rg.survive_reg = freg; /* postProcess updateShape canSurvive 폴드 */

    enum { MAX_MANIFEST = 4096 };
    static manifest_line_t man[MAX_MANIFEST];
    char mpath[1024];
    uint64_t tr0 = now_ns();
    snprintf(mpath, sizeof mpath, "%s/order.manifest", stages_dir);
    int32_t n_man = load_manifest(mpath, seed, man, MAX_MANIFEST);
    if (n_man < 1024)
        die("manifest unexpectedly short", mpath);
    B_replay_load += now_ns() - tr0;

    /* --- 라이트: stages.log v2 배치 재생을 데코 재생과 인터리브 --- */
    static ltask_t          lt[8192];
    int32_t                 n_lt = 0;
    static hc_light_chunk_t frozen[1024];
    static hc_light_world_t lw;
    static int32_t          bat_P[TL_CAP];
    static int32_t          idx08[4096];
    static int32_t          P08[4096];
    int32_t                 n08 = 0;
    {
        char lpath[1024];
        tr0 = now_ns();
        snprintf(lpath, sizeof lpath, "%s/stages.log", stages_dir);
        n_lt = load_light_tasks(lpath, lt, 8192);
        for (int32_t b = 0; b < g_nbatch; b++)
            bat_P[b] = counter_at(g_batch_drain[b]);
        for (int32_t k = 0; k < n_lt; k++)
            if (lt[k].kind == 0) {
                if (n08 >= 4096)
                    die("too many initialize_light tasks", lpath);
                idx08[n08] = k;
                P08[n08] = counter_at(lt[k].sub_nanos);
                n08++;
            }
        B_replay_load += now_ns() - tr0;
        tr0 = now_ns();
        if (hc_light_world_init(&lw, &g_arena, WC0, WC0, WN) != 0)
            die("arena exhausted (light world)", NULL);
        for (int32_t i = 0; i < WORLD_CHUNKS; i++)
            if (hc_light_attach(&lw, &g_arena, &g_chunks[i]) != 0)
                die("arena exhausted (light chunks)", NULL);
        B_light_setup += now_ns() - tr0;
        for (int32_t i = 0; i < WORLD_CHUNKS; i++)
            g_live_at[i] = INT32_MAX;
        for (int32_t k = 0; k < n_lt; k++) {
            if (lt[k].kind != 0)
                continue;
            if (lt[k].comp_nanos < 0)
                die("initialize_light task without completion", lpath);
            g_live_at[widx(lt[k].cx, lt[k].cz)] =
                counter_at(lt[k].comp_nanos);
        }
        g_lw_hook = &lw;
        rg.on_block_write = light_write_hook;
        rg.raw_brightness = raw_brightness_cb;
    }

    uint64_t t_feat0 = now_ns();
    int32_t next_b = 0, c08 = 0;
    for (int32_t pos = 0; pos <= n_man; pos++) {
        uint64_t t0 = now_ns();
        while (c08 < n08 && P08[c08] <= pos) {
            const ltask_t *E = &lt[idx08[c08]];
            hc_light_accum_init_chunk(&lw, E->cx, E->cz);
            c08++;
        }
        uint64_t t1 = now_ns();
        B_light08 += t1 - t0;
        while (next_b < g_nbatch && bat_P[next_b] <= pos) {
            hc_light_accum_prepare(&lw);
            for (int32_t k = 0; k < n_lt; k++) {
                if (lt[k].kind != 1 || lt[k].batch != next_b)
                    continue;
                hc_light_accum_light_chunk(&lw, lt[k].cx, lt[k].cz);
            }
            next_b++;
        }
        uint64_t t2 = now_ns();
        B_light09 += t2 - t1;
        if (pos == n_man)
            break;
        if (man[pos].cx < WC0 + 1 || man[pos].cx >= WC0 + WN - 1 ||
            man[pos].cz < WC0 + 1 || man[pos].cz >= WC0 + WN - 1)
            die("manifest entry outside decorable window", mpath);
        g_cur_pos = pos;
        hc_gen_features_chunk(&rg, man[pos].cx, man[pos].cz, seed, freg,
                              &view, &g_reg, (int32_t)sea->num, 10, &g_sink);
        uint64_t t3 = now_ns();
        B_features += t3 - t2;
        hc_light_set_featured(&lw, man[pos].cx, man[pos].cz);
        hc_light_accum_flush(&lw);
        B_light_flush += now_ns() - t3;
    }
    if (next_b != g_nbatch)
        die("light batches beyond manifest counter", NULL);
    uint64_t t_feat_end = now_ns();

    g_cur_pos = INT32_MAX; /* 전 배치 이후 — 08 있는 청크는 전부 라이브 */
    for (int32_t i = 0; i < WORLD_CHUNKS; i++)
        g_ppg[i].frozen = 1;
    int32_t n_pm_out = 0;
    {
        char ppath[1024];
        tr0 = now_ns();
        snprintf(ppath, sizeof ppath, "%s/postprocess.manifest", stages_dir);
        size_t len = 0;
        char  *buf = read_file(ppath, &len);
        enum { MAX_PPM = 2048 };
        static int32_t pm_cx[MAX_PPM], pm_cz[MAX_PPM];
        int32_t        n_pm = 0;
        static int32_t seq2chunk[MAX_PPM];
        static int32_t rec_x[MAX_PPM][1100], rec_y[MAX_PPM][1100],
            rec_z[MAX_PPM][1100];
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
        B_replay_load += now_ns() - tr0;
        n_pm_out = n_pm;
        static int32_t ox[4096], oy[4096], oz[4096];
        for (int32_t m = 0; m < n_pm; m++) {
            uint64_t t0 = now_ns();
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
            if (!ok)
                die("derived postprocess marks diverge from recording",
                    NULL);
            uint64_t t1 = now_ns();
            B_pp_verify += t1 - t0;
            hc_postprocess_chunk(&rg, pm_cx[m], pm_cz[m], &g_ppg[wi]);
            uint64_t t2 = now_ns();
            B_pp += t2 - t1;
            /* postProcess 라이브 쓰기의 checkBlock — 승격 단위 배치 */
            hc_light_accum_flush(&lw);
            B_light_flush += now_ns() - t2;
        }
    }

    /* 라이트 = 증분 이력의 최종 상태 (동결 스냅샷) */
    tr0 = now_ns();
    hc_light_accum_flush(&lw);
    hc_light_accum_prepare(&lw);
    for (int32_t idx = 0; idx < 1024; idx++) {
        int32_t           cx = idx & 31, cz = idx >> 5;
        hc_light_chunk_t *sl =
            &lw.slots[(cz - lw.cz0) * lw.n + (cx - lw.cx0)];
        if (!sl->seeded)
            die("r.0.0 chunk not lit by any batch", NULL);
        frozen[idx] = *sl;
    }
    B_light_final += now_ns() - tr0;

    hc_arena_t scratch;
    void    *smem = hc_arena_alloc(&g_arena, 64u << 20, 16);
    uint8_t *cat = hc_arena_alloc(&g_arena, 192u << 20, 16);
    if (!smem || !cat)
        die("arena exhausted (serialize scratch)", NULL);
    size_t cat_off = 0;
    tr0 = now_ns();
    for (int32_t idx = 0; idx < 1024; idx++) {
        int32_t           cx = idx & 31, cz = idx >> 5;
        hc_chunk_t       *c = &g_chunks[widx(cx, cz)];
        hc_light_chunk_t *ls = &frozen[idx];
        uint8_t *payload = hc_arena_alloc(&g_arena, 256u << 10, 1);
        if (!payload)
            die("arena exhausted (payload)", NULL);
        hc_arena_init(&scratch, smem, 64u << 20);
        ptrdiff_t n = hc_chunk_to_nbt(c, &g_reg, ls, recorder.recs,
                                      recorder.n, /*last_update=*/0, g_sctx,
                                      &scratch, payload, 256u << 10);
        if (n < 0)
            die("chunk serialization failed", NULL);
        cat[cat_off++] = (uint8_t)(idx >> 24);
        cat[cat_off++] = (uint8_t)(idx >> 16);
        cat[cat_off++] = (uint8_t)(idx >> 8);
        cat[cat_off++] = (uint8_t)idx;
        memcpy(cat + cat_off, payload, (size_t)n);
        cat_off += (size_t)n;
    }
    B_serialize = now_ns() - tr0;
    tr0 = now_ns();
    uint8_t dig[32];
    hc_sha256(cat, cat_off, dig);
    B_sha256 = now_ns() - tr0;
    char hex[65];
    for (int i = 0; i < 32; i++)
        sprintf(hex + 2 * i, "%02x", dig[i]);
    hex[64] = '\0';

    uint64_t t_proc_end = now_ns();
    int      pass = strcmp(hex, canonical_hex) == 0;

    /* ---- 리포트 ---- */
    stage_acc_t cw = {0, 0, 0, 0, 0}, cc = {0, 0, 0, 0, 0};
    for (int t = 0; t < nthreads; t++) {
        cw.nc_init += jobs[t].wall.nc_init;
        cw.beard += jobs[t].wall.beard;
        cw.noise += jobs[t].wall.noise;
        cw.surface += jobs[t].wall.surface;
        cw.carvers += jobs[t].wall.carvers;
        cc.nc_init += jobs[t].cpu.nc_init;
        cc.beard += jobs[t].cpu.beard;
        cc.noise += jobs[t].cpu.noise;
        cc.surface += jobs[t].cpu.surface;
        cc.carvers += jobs[t].cpu.carvers;
    }
    uint64_t setup_ns = t_setup_end - t_proc0;
    uint64_t chain_wall = t_chain_end - t_setup_end;
    uint64_t feat_phase_wall = t_feat_end - t_feat0;
    uint64_t chain_cpu_total =
        cc.nc_init + cc.beard + cc.noise + cc.surface + cc.carvers;
    /* 생성 비용 wall 합 — 하네스 오버헤드(replay_load, pp_verify) 제외 */
    uint64_t gen_wall = chain_wall + B_light_setup + B_light08 + B_light09 +
                        B_features + B_light_flush + B_pp + B_light_final +
                        B_serialize + B_sha256;

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);

    fprintf(stderr,
            "== hyperchunk-bench seed=%" PRId64 " threads=%d ==\n"
            "   (이 VM은 토폴로지 오보고 — 절대치는 참고치, 비중/배율만 유효)\n"
            "setup                 %8.1f ms  (참조 로드+컴파일, 생성 비용 아님)\n"
            "replay-input parse    %8.1f ms  (하네스 오버헤드)\n"
            "-- chain phase (%d threads) wall %.1f ms --\n"
            "  cpu-sum  nc_init    %8.1f ms\n"
            "  cpu-sum  beard      %8.1f ms\n"
            "  cpu-sum  04 noise   %8.1f ms\n"
            "  cpu-sum  07 surface %8.1f ms\n"
            "  cpu-sum  08 carvers %8.1f ms\n"
            "  cpu-sum  chain total%8.1f ms  (wall*threads 대비 가동률 %.0f%%)\n"
            "-- serial phases (wall) --\n"
            "  light setup+attach  %8.1f ms\n"
            "  light 08 init       %8.1f ms\n"
            "  light 09 batches    %8.1f ms\n"
            "  light flush         %8.1f ms\n"
            "  09 features         %8.1f ms\n"
            "  postprocess         %8.1f ms\n"
            "  pp mark verify      %8.1f ms  (하네스 오버헤드)\n"
            "  light final freeze  %8.1f ms\n"
            "  serialize (nbt)     %8.1f ms\n"
            "  sha256              %8.1f ms\n"
            "gen wall (오버헤드 제외) %8.1f ms   per-chunk(1024) %.2f ms\n"
            "process total         %8.1f ms   maxrss %.1f GiB\n"
            "%s\n",
            seed, nthreads, setup_ns / 1e6, B_replay_load / 1e6, nthreads,
            chain_wall / 1e6, cc.nc_init / 1e6, cc.beard / 1e6,
            cc.noise / 1e6, cc.surface / 1e6, cc.carvers / 1e6,
            chain_cpu_total / 1e6,
            100.0 * (double)chain_cpu_total /
                ((double)chain_wall * nthreads),
            B_light_setup / 1e6, B_light08 / 1e6, B_light09 / 1e6,
            B_light_flush / 1e6, B_features / 1e6, B_pp / 1e6,
            B_pp_verify / 1e6, B_light_final / 1e6, B_serialize / 1e6,
            B_sha256 / 1e6, gen_wall / 1e6, gen_wall / 1e6 / 1024.0,
            (t_proc_end - t_proc0) / 1e6,
            ru.ru_maxrss / 1024.0 / 1024.0,
            pass ? "PASS: bit-exact parity" : "FAIL: hash mismatch");

    printf("{\"seed\":%" PRId64 ",\"threads\":%d,\"pass\":%s,"
           "\"canonical\":\"%s\","
           "\"chunks\":{\"chain\":%d,\"decorated\":%d,\"postprocessed\":%d,"
           "\"emitted\":1024},"
           "\"setup_ns\":%" PRIu64 ",\"replay_load_ns\":%" PRIu64 ","
           "\"chain_wall_ns\":%" PRIu64 ","
           "\"chain_cpu_ns\":{\"nc_init\":%" PRIu64 ",\"beard\":%" PRIu64
           ",\"noise\":%" PRIu64 ",\"surface\":%" PRIu64
           ",\"carvers\":%" PRIu64 "},"
           "\"chain_wall_acc_ns\":{\"nc_init\":%" PRIu64 ",\"beard\":%" PRIu64
           ",\"noise\":%" PRIu64 ",\"surface\":%" PRIu64
           ",\"carvers\":%" PRIu64 "},"
           "\"serial_wall_ns\":{\"light_setup\":%" PRIu64
           ",\"light08\":%" PRIu64 ",\"light09\":%" PRIu64
           ",\"light_flush\":%" PRIu64 ",\"features\":%" PRIu64
           ",\"postprocess\":%" PRIu64 ",\"pp_verify\":%" PRIu64
           ",\"light_final\":%" PRIu64 ",\"serialize\":%" PRIu64
           ",\"sha256\":%" PRIu64 "},"
           "\"feat_phase_wall_ns\":%" PRIu64 ",\"gen_wall_ns\":%" PRIu64
           ",\"proc_wall_ns\":%" PRIu64 ",\"maxrss_kib\":%ld}\n",
           seed, nthreads, pass ? "true" : "false", hex, WORLD_CHUNKS, n_man,
           n_pm_out, setup_ns, B_replay_load, chain_wall, cc.nc_init,
           cc.beard, cc.noise, cc.surface, cc.carvers, cw.nc_init, cw.beard,
           cw.noise, cw.surface, cw.carvers, B_light_setup, B_light08,
           B_light09, B_light_flush, B_features, B_pp, B_pp_verify,
           B_light_final, B_serialize, B_sha256, feat_phase_wall, gen_wall,
           t_proc_end - t_proc0, ru.ru_maxrss);

    if (!pass) {
        fprintf(stderr, "  ours   %s\n  golden %s\n", hex, canonical_hex);
        return 1;
    }
    return 0;
}
