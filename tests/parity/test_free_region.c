/* P2-3: FREE 스케줄러 풀-리전 게이트 (ADR-008 D1/D2 이중 모드).
 *
 * test_full_region.c (소스 오브 트루스) 와 같은 스테이지 코드·같은 골든
 * 입력으로, 직렬 리플레이 루프 대신 hc_sched 이벤트 그래프를 돈다.
 *
 * 이벤트 모델: base 선형화 = REPLAY 루프가 실행할 정확한 순서
 *   [pos 마다: 08(P08<=pos) → 배치(bat_P<=pos: prepare + 멤버 09) → 데코]
 * 셀 신고 (읽기/쓰기 풋프린트 커버):
 *   데코  = center±2 청크 (블록 ±1 + 라이트 flush 헤일로 ±1) + 피스 그룹
 *   08    = ±1 (derive_geometry 마스크 스필)
 *   09    = ±1 (flood 반경 <=14블록)
 *   prepare = 전역 배리어 (in_r 전 청크 재유도 — 단조 래치가 관측면)
 * 피스 그룹 = 같은 구조물 피스 BB 에 닿는 데코들의 가상 셀 (spider/
 * shipwreck 래치·BE 순서 — 거리 >4 일 수 있어 반경 규칙 밖).
 *
 * 모드 (argv[7]):
 *   golden-free : 골든 이력 (σ_g, Λ_g) 을 FREE 로 — canonical == argv[6]
 *                 이 곧 FREE-vs-REPLAY 동일성 게이트 (REPLAY==상수는
 *                 full_region 이 상시 증명). pp 마킹 교차검증 포함.
 *   own-replay / own-free : 자체 순서 (σ* = 5×5 잔차 버킷 스트라이드 =
 *                 ADR-003/008 의 "체스판", Λ* = 데코 직후 08 + 버킷 경계
 *                 배치(피라미드 준수: 09 는 ±1 전부 08 후), π* = 골든
 *                 pp 집합의 행우선) 를 REPLAY/FREE 로 실행하고 canonical
 *                 을 출력 — 두 모드의 해시 동일성은 러너 스크립트가
 *                 판정한다. 산출물은 골든과 다른 (그러나 바닐라 규칙상
 *                 유효한) 이력이므로 골든 상수와 비교하지 않는다.
 *
 * 순서-민감 클래스와 그 "최소 동기화" (완료 노트 §3):
 *   블록/하이트맵/ppg/틱/BE = 같은 청크의 접근자가 전부 ±2 충돌쌍 →
 *   셀 FIFO 가 base 순서로 직렬화 (청크별 프로젝션 = REPLAY 동일).
 *   구조물 피스 래치 = 피스 그룹 셀. 라이트 이력 = 배리어(prepare) +
 *   ±1 셀 + 이벤트-로컬 ctx (hc_light_ctx_t). */

#undef NDEBUG
#define _POSIX_C_SOURCE 200809L

#include "../../core/include/hc_sched.h"
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
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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

/* --- reference/ 로더 (test_full_region.c 와 동일 규약) --- */

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
            load_tree(tab, n, cap, sub, pref, depth + 1);
        } else if (has_suffix(e->d_name, ".json")) {
            add_source(tab, n, cap, dir, rel_prefix, e->d_name);
        } else {
            die("unexpected file in reference tree", sub);
        }
    }
    closedir(d);
}

/* --- 와이드 쿼트 바이옴 그리드 (test_full_region.c 동일) --- */

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

/* --- 워커 컨텍스트 (P2-3) --- */

enum { NTHREADS = 20 };

typedef struct {
    hc_feat_region_t rg;      /* 마스터 rg 의 사본 — center 가 워커별 */
    hc_light_ctx_t   lctx;    /* 이벤트-로컬 라이트 큐/펜딩 */
    int32_t          cur_pos; /* 실행 중 이벤트의 base 카운터 (라이트 게이트) */
    char             pad[64];
} fw_t;

static fw_t g_wk[NTHREADS];

static hc_light_world_t *g_lw_hook;
static int32_t           g_live_at[WORLD_CHUNKS];
static _Atomic int64_t   g_n_live_writes;

static void light_write_hook(void *ud, int32_t x, int32_t y, int32_t z,
                             uint16_t old_id, uint16_t new_id) {
    fw_t *w = ud;
    if (!g_lw_hook)
        return;
    int32_t la = g_live_at[widx(x >> 4, z >> 4)];
    if (la == INT32_MAX || w->cur_pos < la)
        return; /* 동결 창 / 파이프라인 밖 */
    hc_light_accum_write_ctx(g_lw_hook, &w->lctx, x, y, z, old_id, new_id);
    atomic_fetch_add_explicit(&g_n_live_writes, 1, memory_order_relaxed);
}

static int raw_brightness_cb(void *ud, int32_t x, int32_t y, int32_t z) {
    fw_t *w = ud;
    if (!g_lw_hook)
        return 0;
    int32_t cx = x >> 4, cz = z >> 4;
    int     vis = 0;
    for (int dz = -1; dz <= 1 && !vis; dz++)
        for (int dx = -1; dx <= 1 && !vis; dx++) {
            int32_t la = g_live_at[widx(cx + dx, cz + dz)];
            if (la != INT32_MAX && la <= w->cur_pos)
                vis = 1;
        }
    if (!vis)
        return 15;
    int sky = hc_light_get(g_lw_hook, HC_LIGHT_SKY, x, y, z);
    int blk = hc_light_get(g_lw_hook, HC_LIGHT_BLOCK, x, y, z);
    return sky > blk ? sky : blk;
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

/* --- order.manifest (test_full_region.c 동일) --- */

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

/* --- 트레이스 싱크: 미구현 피처 즉사 (서베이 없음 — 게이트 전용) --- */

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

/* --- 병렬 04..06 체인 (test_full_region.c 동일, diag 덤프 제외) --- */

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
        hc_beard_t beard;
        if (g_sctx &&
            hc_structures_beard(g_sctx, &job->arena, c->cx, c->cz, &beard))
            nc->beard = &beard;
        hc_gen_noise_stage(c, nc);
        hc_gen_surface_stage(c, nc, job->surf, job->view);
        uint64_t mask[HC_CARVING_MASK_WORDS];
        memset(mask, 0, sizeof mask);
        hc_gen_carvers_stage(c, nc, job->surf, job->view, job->seed,
                             job->carvers, 3, mask);
        job->arena.off = mark;
    }
}

/* --- 09 라이트 배치 타임라인 (test_full_region.c 의 ltask_t 기계) --- */

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
    /* 큐-드레인 배치 (09 전용) — δ_wake = 2ms */
    int32_t nb = 0, i = 0;
    int64_t t = -1;
    while (i < n) {
        if (out[i].kind != 1) {
            out[i].batch = -1;
            i++;
            continue;
        }
        if (out[i].sub_nanos > t)
            t = out[i].sub_nanos + 2000000;
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
    /* POST-실행가능성 병합 (T_PRE_MIN = 2ms) */
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

/* --- 마킹 드레인 헬퍼 (test_full_region.c 동일) --- */

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

/* ================= 이벤트 그래프 ================= */

enum { EV_DECO = 0, EV_L08 = 1, EV_L09 = 2, EV_PREPARE = 3 };

typedef struct {
    uint8_t kind;
    int32_t cx, cz;
    int32_t pos; /* base 카운터 (라이트 라이브-창 게이트 공간) */
} ev_ud_t;

enum { MAX_EV = 8192, MAX_CELLS = 1 << 18 };
static hc_sched_ev_t g_evs[MAX_EV];
static ev_ud_t       g_evud[MAX_EV];
static int32_t       g_ev_n;
static int32_t       g_cells[MAX_CELLS];
static int32_t       g_cells_n;
static int32_t       g_ncellspace;

static hc_light_world_t lw;
static int              g_light_on = 1;
static const hc_feat_reg_t *g_freg;
static const hc_biome_view_t *g_view;
static int32_t               g_sea;
static int64_t               g_seed;

static void exec_ev(void *ud, int32_t worker) {
    ev_ud_t *e = ud;
    fw_t    *w = &g_wk[worker];
    switch (e->kind) {
    case EV_DECO:
        w->cur_pos = e->pos;
        hc_gen_features_chunk(&w->rg, e->cx, e->cz, g_seed, g_freg, g_view,
                              &g_reg, g_sea, 10, &g_sink);
        if (g_light_on) {
            hc_light_set_featured(&lw, e->cx, e->cz);
            hc_light_accum_flush_ctx(&lw, &w->lctx);
        }
        break;
    case EV_L08:
        hc_light_accum_init_chunk(&lw, e->cx, e->cz);
        break;
    case EV_L09:
        hc_light_accum_light_chunk_ctx(&lw, &w->lctx, e->cx, e->cz);
        break;
    case EV_PREPARE:
        hc_light_accum_prepare(&lw);
        break;
    }
}

/* 셀 신고: (cx,cz)±r 박스 ∩ 월드. 반환 = 셀 수 */
static int32_t emit_box_cells(int32_t *dst, int32_t cx, int32_t cz,
                              int32_t r) {
    int32_t m = 0;
    for (int32_t dz = -r; dz <= r; dz++)
        for (int32_t dx = -r; dx <= r; dx++) {
            int32_t x = cx + dx, z = cz + dz;
            if (x < WC0 || x >= WC0 + WN || z < WC0 || z >= WC0 + WN)
                continue;
            dst[m++] = (z - WC0) * WN + (x - WC0);
        }
    return m;
}

/* 데코 이벤트의 피스-그룹 가상 셀: 이 청크(블록 박스)와 XZ 교차하는
 * 모든 구조물 피스 (hc_structures_step 의 배치 대상 초집합) */
static int32_t emit_piece_cells(int32_t *dst, int32_t cx, int32_t cz) {
    if (!g_sctx)
        return 0;
    int32_t m = 0;
    int32_t bx0 = cx * 16, bz0 = cz * 16, bx1 = bx0 + 15, bz1 = bz0 + 15;
    int32_t vbase = WORLD_CHUNKS;
    for (int32_t i = 0; i < g_sctx->n_starts; i++) {
        const hc_sstart_t *st = &g_sctx->starts[i];
        if (st->scx < cx - 8 || st->scx > cx + 8 || st->scz < cz - 8 ||
            st->scz > cz + 8) {
            vbase += st->n_pieces;
            continue;
        }
        for (int32_t j = 0; j < st->n_pieces; j++) {
            const hc_spiece_t *p = &st->pieces[j];
            if (p->bb[3] >= bx0 && p->bb[0] <= bx1 && p->bb[5] >= bz0 &&
                p->bb[2] <= bz1)
                dst[m++] = vbase + j;
        }
        vbase += st->n_pieces;
    }
    return m;
}

static void push_event(uint8_t kind, int32_t cx, int32_t cz, int32_t pos,
                       int32_t radius, int with_pieces) {
    if (g_ev_n >= MAX_EV)
        die("event cap exceeded", NULL);
    ev_ud_t *u = &g_evud[g_ev_n];
    u->kind = kind;
    u->cx = cx;
    u->cz = cz;
    u->pos = pos;
    hc_sched_ev_t *e = &g_evs[g_ev_n];
    e->exec = exec_ev;
    e->ud = u;
    if (kind == EV_PREPARE) {
        e->cells = NULL;
        e->n_cells = 0;
    } else {
        int32_t *dst = &g_cells[g_cells_n];
        int32_t  m = emit_box_cells(dst, cx, cz, radius);
        if (with_pieces)
            m += emit_piece_cells(dst + m, cx, cz);
        if (g_cells_n + m > MAX_CELLS)
            die("cell cap exceeded", NULL);
        e->cells = dst;
        e->n_cells = m;
        g_cells_n += m;
    }
    g_ev_n++;
}

/* ================= 병렬 직렬화 (청크별 순수; 연접/해시만 idx 순차) ================= */

static hc_tick_recorder_t g_recorder;
static hc_light_chunk_t   g_frozen[1024];
static uint8_t           *g_payload[1024];
static ptrdiff_t          g_plen[1024];

typedef struct {
    void  *smem;
    size_t ssz;
} ser_job_t;
static ser_job_t       g_sjob[NTHREADS];
static _Atomic int32_t g_ser_next;

static void *ser_worker(void *ud) {
    ser_job_t *job = ud;
    for (;;) {
        int32_t idx =
            atomic_fetch_add_explicit(&g_ser_next, 1, memory_order_relaxed);
        if (idx >= 1024)
            return NULL;
        int32_t     cx = idx & 31, cz = idx >> 5;
        hc_chunk_t *c = &g_chunks[widx(cx, cz)];
        hc_arena_t  scratch;
        hc_arena_init(&scratch, job->smem, job->ssz);
        ptrdiff_t n = hc_chunk_to_nbt(c, &g_reg, &g_frozen[idx],
                                      g_recorder.recs, g_recorder.n,
                                      /*last_update=*/0, g_sctx, &scratch,
                                      g_payload[idx], 256u << 10);
        if (n < 0)
            die("chunk serialization failed", NULL);
        g_plen[idx] = n;
    }
}

int main(int argc, char **argv) {
    if (argc != 8) {
        fprintf(stderr,
                "usage: test_free_region <ref_dir> <stages_seed_dir> "
                "<band_region_golden> <seed> <region_ref_dir> "
                "<canonical_sha256_hex> <golden-free|own-replay|own-free>\n");
        return 2;
    }
    const char *ref_dir = argv[1];
    const char *stages_dir = argv[2];
    const char *band_golden = argv[3];
    int64_t     seed = strtoll(argv[4], NULL, 10);
    const char *region_ref_dir = argv[5];
    const char *canonical_hex = argv[6];
    const char *mode = argv[7];
    int         own = strncmp(mode, "own-", 4) == 0;
    hc_schedule_policy_t policy =
        strcmp(mode, "own-replay") == 0 ? HC_SCHED_REPLAY : HC_SCHED_FREE;
    g_seed = seed;

    double t0 = now_s();
    size_t backing_sz = (size_t)8 << 30;
    void  *backing = malloc(backing_sz);
    if (!backing)
        die("cannot allocate 8GiB backing", NULL);
    hc_arena_init(&g_arena, backing, backing_sz);
    hc_biome_reg_init(&g_reg, &g_arena);

    /* --- reference 로드 (test_full_region.c 규약) --- */
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
    g_sea = (int32_t)sea->num;

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
    g_freg = freg;

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
                                        &scratch, 0);
            }
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
    printf("[%6.1fs] setup done (%d biomes interned)\n", now_s() - t0,
           g_reg.count);

    /* --- 월드 구성 --- */
    static hc_biome_view_t view;
    view.qx0 = QG_MIN_XZ;
    view.qz0 = QG_MIN_XZ;
    view.nxz = QG_NXZ;
    view.qy0 = QG_MIN_Y;
    view.ny = QG_NY;
    view.ids = &g_grid[0][0][0];
    view.zoom_seed = hc_biome_obfuscate_seed(seed);
    g_view = &view;

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

    g_sctx = hc_arena_alloc(&g_arena, sizeof *g_sctx, _Alignof(hc_sctx_t));
    {
        char sdir[1024], tdir[1024];
        snprintf(sdir, sizeof sdir, "%s/../structures", region_ref_dir);
        snprintf(tdir, sizeof tdir, "%s/structure", ref_dir);
        const char *serr = NULL;
        if (!g_sctx ||
            hc_structures_init(g_sctx, &g_arena, seed, sdir, tdir, NULL,
                               g_tags, g_n_tags, &view, &g_reg, &serr) != 0)
            die(serr ? serr : "structures init failed", NULL);
    }

    /* 04..06 병렬 체인 */
    hc_mth_trig_init();
    static chain_job_t jobs[NTHREADS];
    {
        static pthread_t tids[NTHREADS];
        _Atomic int32_t  next = 0;
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
            jobs[t].sea = g_sea;
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

    /* --- features 마스터 리전 + 레코더 --- */
    static hc_feat_region_t rg;
    memset(&rg, 0, sizeof rg);
    rg.cx0 = WC0;
    rg.cz0 = WC0;
    rg.n = WN;
    for (int32_t i = 0; i < WORLD_CHUNKS; i++)
        rg.chunks[i] = &g_chunks[i];
    if (hc_tick_recorder_init(&g_recorder, &g_arena, 1 << 19) != 0)
        die("arena exhausted (tick recorder)", NULL);
    rg.ticks = &g_recorder;
    rg.survive_reg = freg;
    rg.struct_step = struct_step_cb;
    rg.struct_ud = g_sctx;
    rg.be = &g_sctx->be;
    rg.free_read_guard = 1;

    /* --- manifest + 라이트 타임라인 --- */
    enum { MAX_MANIFEST = 4096 };
    static manifest_line_t man[MAX_MANIFEST];
    char mpath[1024];
    snprintf(mpath, sizeof mpath, "%s/order.manifest", stages_dir);
    int32_t n_man = load_manifest(mpath, seed, man, MAX_MANIFEST);
    if (n_man < 1024)
        die("manifest unexpectedly short", mpath);

    static ltask_t lt[8192];
    int32_t        n_lt = 0;
    static int32_t bat_P[TL_CAP];
    static int32_t idx08[4096];
    static int32_t P08[4096];
    int32_t        n08 = 0;

    if (hc_light_world_init(&lw, &g_arena, WC0, WC0, WN) != 0)
        die("arena exhausted (light world)", NULL);
    for (int32_t i = 0; i < WORLD_CHUNKS; i++)
        if (hc_light_attach(&lw, &g_arena, &g_chunks[i]) != 0)
            die("arena exhausted (light chunks)", NULL);
    for (int32_t i = 0; i < WORLD_CHUNKS; i++)
        g_live_at[i] = INT32_MAX;
    g_lw_hook = &lw;

    /* --- 워커 컨텍스트: rg 사본 + 라이트 ctx (체인 arena 재사용) --- */
    hc_features_prewarm(seed);
    for (int t = 0; t < NTHREADS; t++) {
        g_wk[t].rg = rg;
        g_wk[t].rg.on_block_write = light_write_hook;
        g_wk[t].rg.light_ud = &g_wk[t];
        g_wk[t].rg.raw_brightness = raw_brightness_cb;
        /* qlog 20: 단일-청크 시딩+드레인/데코-flush 규모 (전역 QLOG 22
         * 는 "관측 최대의 수십 배" — 이벤트-로컬은 그보다 작다; 넘치면
         * q_push 가 즉사한다) */
        if (hc_light_ctx_init(&g_wk[t].lctx, &jobs[t].arena, 20, 17,
                              WORLD_CHUNKS) != 0)
            die("arena exhausted (light ctx)", NULL);
    }

    /* ================= 이벤트 리스트 구성 ================= */
    g_ev_n = 0;
    g_cells_n = 0;
    {
        int32_t n_pieces = 0;
        for (int32_t i = 0; i < g_sctx->n_starts; i++)
            n_pieces += g_sctx->starts[i].n_pieces;
        g_ncellspace = WORLD_CHUNKS + n_pieces;
    }

    if (!own) {
        /* --- 골든 이력 (σ_g, Λ_g): REPLAY 루프의 정확한 선형화 --- */
        char lpath[1024];
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
        for (int32_t k = 0; k < n_lt; k++) {
            if (lt[k].kind != 0)
                continue;
            if (lt[k].comp_nanos < 0)
                die("initialize_light task without completion", lpath);
            g_live_at[widx(lt[k].cx, lt[k].cz)] =
                counter_at(lt[k].comp_nanos);
        }
        int32_t next_b = 0, c08 = 0;
        for (int32_t pos = 0; pos <= n_man; pos++) {
            while (c08 < n08 && P08[c08] <= pos) {
                const ltask_t *E = &lt[idx08[c08]];
                push_event(EV_L08, E->cx, E->cz, pos, 1, 0);
                c08++;
            }
            while (next_b < g_nbatch && bat_P[next_b] <= pos) {
                push_event(EV_PREPARE, 0, 0, pos, 0, 0);
                for (int32_t k = 0; k < n_lt; k++) {
                    if (lt[k].kind != 1 || lt[k].batch != next_b)
                        continue;
                    push_event(EV_L09, lt[k].cx, lt[k].cz, pos, 1, 0);
                }
                next_b++;
            }
            if (pos == n_man)
                break;
            if (man[pos].cx < WC0 + 1 || man[pos].cx >= WC0 + WN - 1 ||
                man[pos].cz < WC0 + 1 || man[pos].cz >= WC0 + WN - 1)
                die("manifest entry outside decorable window", mpath);
            push_event(EV_DECO, man[pos].cx, man[pos].cz, pos, 2, 1);
        }
        if (next_b != g_nbatch)
            die("light batches beyond manifest counter", NULL);
        printf("[%6.1fs] golden events: %d (deco %d, 08 %d, batches %d)\n",
               now_s() - t0, g_ev_n, n_man, n08, g_nbatch);
    } else {
        /* --- 자체 이력 (σ*, Λ*): 5×5 잔차 버킷 스트라이드 (체스판) +
         * 데코 직후 08 + 버킷 경계 배치 (09 eligibility: ±1 전부 08 —
         * 바닐라 09 피라미드 준수). base 카운터 = 이벤트 인덱스 공간. --- */
        static int32_t order[MAX_MANIFEST];
        int32_t        n_ord = 0;
        for (int32_t rx = 0; rx < 5; rx++)
            for (int32_t rz = 0; rz < 5; rz++)
                for (int32_t i = 0; i < n_man; i++) {
                    int32_t mx = ((man[i].cx % 5) + 5) % 5;
                    int32_t mz = ((man[i].cz % 5) + 5) % 5;
                    if (mx == rx && mz == rz)
                        order[n_ord++] = i;
                }
        if (n_ord != n_man)
            die("bucket order lost entries", NULL);
        static uint8_t deco_set[WORLD_CHUNKS], has08[WORLD_CHUNKS],
            has09[WORLD_CHUNKS];
        memset(deco_set, 0, sizeof deco_set);
        memset(has08, 0, sizeof has08);
        memset(has09, 0, sizeof has09);
        for (int32_t i = 0; i < n_man; i++)
            deco_set[widx(man[i].cx, man[i].cz)] = 1;
        int32_t done = 0;
        for (int32_t b = 0; b < 25; b++) {
            int32_t start = done;
            /* 이 버킷의 데코 + 즉시 08 */
            for (; done < n_ord; done++) {
                int32_t i = order[done];
                int32_t mx = ((man[i].cx % 5) + 5) % 5;
                int32_t mz = ((man[i].cz % 5) + 5) % 5;
                if (mx * 5 + mz != b)
                    break;
                push_event(EV_DECO, man[i].cx, man[i].cz, g_ev_n, 2, 1);
                g_live_at[widx(man[i].cx, man[i].cz)] = g_ev_n;
                push_event(EV_L08, man[i].cx, man[i].cz, g_ev_n, 1, 0);
                has08[widx(man[i].cx, man[i].cz)] = 1;
            }
            (void)start;
            /* 버킷 경계 배치: 새로 eligible 해진 09 전부 */
            int32_t n_elig = 0;
            static int32_t elig[WORLD_CHUNKS];
            for (int32_t cz = WC0 + 1; cz < WC0 + WN - 1; cz++)
                for (int32_t cx = WC0 + 1; cx < WC0 + WN - 1; cx++) {
                    int32_t wi = widx(cx, cz);
                    if (!deco_set[wi] || has09[wi] || !has08[wi])
                        continue;
                    int ok = 1;
                    for (int dz = -1; dz <= 1 && ok; dz++)
                        for (int dx = -1; dx <= 1 && ok; dx++) {
                            int32_t ni = widx(cx + dx, cz + dz);
                            if (deco_set[ni] ? !has08[ni] : 1)
                                ok = 0;
                        }
                    if (ok)
                        elig[n_elig++] = wi;
                }
            if (n_elig) {
                push_event(EV_PREPARE, 0, 0, g_ev_n, 0, 0);
                for (int32_t k = 0; k < n_elig; k++) {
                    int32_t wi = elig[k];
                    push_event(EV_L09, WC0 + (wi % WN), WC0 + (wi / WN),
                               g_ev_n, 1, 0);
                    has09[wi] = 1;
                }
            }
        }
        if (done != n_ord)
            die("bucket sweep incomplete", NULL);
        printf("[%6.1fs] own events: %d (σ*=5x5 체스판, Λ*=버킷 경계)\n",
               now_s() - t0, g_ev_n);
    }

    /* ================= 실행 ================= */
    double t_dag = now_s();
    {
        hc_arena_t sched_a;
        void      *smem = hc_arena_alloc(&g_arena, 32u << 20, 16);
        if (!smem)
            die("arena exhausted (sched)", NULL);
        hc_arena_init(&sched_a, smem, 32u << 20);
        if (hc_sched_run(g_evs, g_ev_n, g_ncellspace, policy, NTHREADS,
                         &sched_a) != 0)
            die("hc_sched_run failed", NULL);
    }
    printf("[%6.1fs] %s: %d events done in %.3fs; %" PRId64
           " live light writes; %d scheduled ticks\n",
           now_s() - t0, policy == HC_SCHED_FREE ? "FREE" : "REPLAY", g_ev_n,
           now_s() - t_dag, atomic_load(&g_n_live_writes), g_recorder.n);

    /* own 모드: 후행 eligibility 잔여 확인 — r.0.0 전체가 lit 이어야 함
     * (마진 데코 부재로 남는 잔여는 아래 seeded assert 가 잡는다) */

    /* --- postProcessGeneration --- */
    for (int t = 0; t < NTHREADS; t++)
        g_wk[t].cur_pos = INT32_MAX; /* 이후 pp/최종 라이브 쓰기 전부 유효 */
    for (int32_t i = 0; i < WORLD_CHUNKS; i++)
        g_ppg[i].frozen = 1;
    int32_t drained = 0;
    {
        char ppath[1024];
        snprintf(ppath, sizeof ppath, "%s/postprocess.manifest", stages_dir);
        size_t len = 0;
        char  *buf = read_file(ppath, &len);
        enum { MAX_PPM = 2048 };
        static int32_t pm_cx[MAX_PPM], pm_cz[MAX_PPM];
        int32_t        n_pm = 0;
        /* golden-free: 기록 마킹 교차검증도 수행 (FREE 의 마킹 프로젝션
         * == REPLAY 증명). own: 집합만 취하고 순서는 행우선 (π*). */
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
                n_pm++;
            }
            p = nl + 1;
        }
        if (own) {
            /* π*: 같은 집합, 행우선 순서 (cz, cx 오름차순) */
            for (int32_t a = 1; a < n_pm; a++) {
                int32_t kx = pm_cx[a], kz = pm_cz[a];
                int32_t b = a - 1;
                while (b >= 0 && (pm_cz[b] > kz ||
                                  (pm_cz[b] == kz && pm_cx[b] > kx))) {
                    pm_cx[b + 1] = pm_cx[b];
                    pm_cz[b + 1] = pm_cz[b];
                    b--;
                }
                pm_cx[b + 1] = kx;
                pm_cz[b + 1] = kz;
            }
        }
        static int32_t ox[4096], oy[4096], oz[4096];
        for (int32_t m = 0; m < n_pm; m++) {
            int32_t wi = widx(pm_cx[m], pm_cz[m]);
            if (!own) {
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
                    fprintf(stderr, "PPG MARK c.%d.%d diverges under FREE\n",
                            pm_cx[m], pm_cz[m]);
                    die("derived postprocess marks diverge from recording",
                        NULL);
                }
            }
            hc_postprocess_chunk(&g_wk[0].rg, pm_cx[m], pm_cz[m],
                                 &g_ppg[wi]);
            hc_light_accum_flush_ctx(&lw, &g_wk[0].lctx);
            drained++;
        }
        printf("[%6.1fs] postprocess: drained %d/%d%s\n", now_s() - t0,
               drained, n_pm, own ? " (π* 행우선)" : " (골든 마킹 교차검증)");
    }

    /* --- 라이트 최종 상태 --- */
    hc_light_accum_flush_ctx(&lw, &g_wk[0].lctx);
    hc_light_accum_prepare(&lw);
    for (int32_t idx = 0; idx < 1024; idx++) {
        int32_t cx = idx & 31, cz = idx >> 5;
        hc_light_chunk_t *sl =
            &lw.slots[(cz - lw.cz0) * lw.n + (cx - lw.cx0)];
        if (!sl->seeded)
            die("r.0.0 chunk not lit by any batch", NULL);
        g_frozen[idx] = *sl;
    }
    printf("[%6.1fs] light history final state ready\n", now_s() - t0);

    /* --- 병렬 직렬화 (섹터 배치 없음 — canonical 은 페이로드 연접) --- */
    double t_ser = now_s();
    for (int32_t idx = 0; idx < 1024; idx++) {
        g_payload[idx] = hc_arena_alloc(&g_arena, 256u << 10, 1);
        if (!g_payload[idx])
            die("arena exhausted (payload)", NULL);
    }
    for (int t = 0; t < NTHREADS; t++) {
        g_sjob[t].ssz = 96u << 20;
        g_sjob[t].smem = hc_arena_alloc(&jobs[t].arena, g_sjob[t].ssz, 16);
        if (!g_sjob[t].smem)
            die("arena exhausted (ser scratch)", NULL);
    }
    atomic_store(&g_ser_next, 0);
    {
        static pthread_t tids[NTHREADS];
        for (int t = 0; t < NTHREADS; t++)
            if (pthread_create(&tids[t], NULL, ser_worker, &g_sjob[t]) != 0)
                die("pthread_create failed (ser)", NULL);
        for (int t = 0; t < NTHREADS; t++)
            pthread_join(tids[t], NULL);
    }

    /* --- 연접 + canonical (idx 순차 — 해시 정의 자체가 순서) --- */
    uint8_t *cat = hc_arena_alloc(&g_arena, 192u << 20, 16);
    if (!cat)
        die("arena exhausted (cat)", NULL);
    size_t cat_off = 0;
    for (int32_t idx = 0; idx < 1024; idx++) {
        cat[cat_off++] = (uint8_t)(idx >> 24);
        cat[cat_off++] = (uint8_t)(idx >> 16);
        cat[cat_off++] = (uint8_t)(idx >> 8);
        cat[cat_off++] = (uint8_t)idx;
        memcpy(cat + cat_off, g_payload[idx], (size_t)g_plen[idx]);
        cat_off += (size_t)g_plen[idx];
    }
    uint8_t dig[32];
    hc_sha256(cat, cat_off, dig);
    char hex[65];
    for (int i2 = 0; i2 < 32; i2++)
        sprintf(hex + 2 * i2, "%02x", dig[i2]);
    hex[64] = '\0';
    printf("[%6.1fs] serialized 1024 chunks (parallel, %.3fs)\n"
           "canonical sha256 ours   %s\n",
           now_s() - t0, now_s() - t_ser, hex);

    if (!own) {
        printf("canonical sha256 golden %s\n", canonical_hex);
        if (strcmp(hex, canonical_hex) != 0) {
            fprintf(stderr, "test_free_region: FAIL (canonical mismatch — "
                            "FREE != REPLAY/golden)\n");
            return 1;
        }
        printf("test_free_region: PASS (golden-free — FREE 스케줄이 골든 "
               "이력과 canonical 동일)\n");
        return 0;
    }
    /* own 모드: 러너가 own-replay 와 own-free 의 이 라인을 비교한다 */
    printf("own-canonical %s %s\n",
           policy == HC_SCHED_FREE ? "free" : "replay", hex);
    printf("test_free_region: DONE (%s)\n", mode);
    return 0;
}
