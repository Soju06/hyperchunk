/* B-4 단가 마이크로벤치 (판정 입력 c_i 전용 — 게이트 비연결 독립 도구).
 *
 * B-3 §6.2 조항 2 (단가-회귀 잔차) 와 조항 3 (K_fp) 의 입력인 프리미티브
 * 단독 처리량을 잰다. 코어 라이브러리는 무손대 — 셋업은 test_df_x8 의
 * reference JSON → compile → hc_nc_* 경로 그대로, 콘-스트림 커널 루프는
 * gen_noise_stage.c 의 nc 호출 시퀀스 (initialize_first → cell_x:
 * advance → cell_z × cell_y: select → swap) 를 update/aquifer/vein 없이
 * 재현한다 (= B-3 의 "x4/x8 커널" 정의역).
 *
 * 모드:
 *   df   --repo <root> [--isa scalar|avx2|avx512] [--iters N] [--warm]
 *        [--coords K] [--ctr]
 *        기본은 fresh (매 반복 arena 되감기 + hc_nc_init 재실행 — 메모
 *        콜드, 프로덕션 동형). --warm 은 같은 nc 재사용 (진단용).
 *        타이밍은 커널 루프만 (nc_init 제외). ISA 부재 호스트는 exit 77.
 *   zoom --repo <root> [--passes N] [--ctr]
 *        golden/rng/surface_seed<seed>.txt 의 zoomed 벡터 12,288개 루프.
 *        pass0 = 콜드; 정상상태도 순수 히트가 아니다 — 1024-슬롯 직결
 *        캐시의 충돌 미스가 패스당 잔존 (B-4 실측 ~5.5%). --ctr 시
 *        패스별 zoom_q/zoom_miss 델타를 함께 찍는다 (누적 오독 방지).
 *   sha  [--backend ni|sw] [--total B] [--chunk B]
 *        스트리밍 업데이트 처리량 (기본 total 47,243,127 B = canonical
 *        스트림 크기, chunk 46 KiB ≈ 청크당 스트림).
 *
 * 출력: stdout 에 `mb <key> <value>` 라인 (파서 친화), 진행은 stderr.
 * 카운터 (--ctr): 런타임 카운터는 기본 빌드에서, HOT 카운터 (x8_node 등)
 * 는 -DHC_CTR_HOT=ON 별도 빌드 (/tmp) 에서만 값이 나온다 (B-3 §9 규율).
 */

#define _POSIX_C_SOURCE 200809L

#include "../core/src/hc_biome.h"
#include "../core/src/hc_counters.h"
#include "../core/src/hc_df_compile.h"
#include "../core/src/hc_df_simd.h"
#include "../core/src/hc_gen_noise.h"
#include "../core/src/hc_sha256.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void die(const char *msg, const char *detail) {
    fprintf(stderr, "microbench: %s%s%s\n", msg, detail ? ": " : "",
            detail ? detail : "");
    exit(2);
}

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int64_t tcpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---------- df 셋업 (test_df_x8.c 와 동일 경로) ---------- */

static unsigned char g_backing[160u << 20];
static hc_arena_t    g_arena;

static char *read_file(const char *path) {
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
    return buf;
}

static const hc_json_t *parse_file(const char *path) {
    const char *err = NULL;
    size_t      pos = 0;
    hc_json_t  *v = hc_json_parse(read_file(path), &g_arena, &err, &pos);
    if (!v)
        die("JSON parse error", path);
    return v;
}

#define MAX_SOURCES 64
static hc_df_source_t g_dfs[MAX_SOURCES], g_noises[MAX_SOURCES];
static int32_t        g_n_dfs = 0, g_n_noises = 0;

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
            continue;
        }
        size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".json") != 0)
            die("unexpected file in reference tree", sub);
        char *name =
            hc_arena_alloc(&g_arena, 10 + strlen(rel_prefix) + l + 1, 1);
        if (!name)
            die("arena exhausted (source name)", e->d_name);
        sprintf(name, "minecraft:%s%.*s", rel_prefix, (int)(l - 5),
                e->d_name);
        if (*n >= MAX_SOURCES)
            die("too many reference sources", name);
        tab[*n].name = name;
        tab[*n].json = parse_file(sub);
        (*n)++;
    }
    closedir(d);
}

static hc_df_graph_t    g_graph;
static hc_noise_roots_t g_roots;

static void df_setup(const char *repo, int64_t seed) {
    char sub[1024];
    snprintf(sub, sizeof sub, "%s/reference/density_function", repo);
    load_tree(g_dfs, &g_n_dfs, sub, "", 0);
    snprintf(sub, sizeof sub, "%s/reference/noise", repo);
    load_tree(g_noises, &g_n_noises, sub, "", 0);
    if (g_n_dfs < 19 || g_n_noises < 35)
        die("reference closure incomplete", repo);
    snprintf(sub, sizeof sub, "%s/reference/overworld-26.2.json", repo);
    const hc_json_t *settings = parse_file(sub);
    const hc_json_t *router = hc_json_get(settings, "noise_router");
    if (!router || router->kind != HC_JSON_OBJ || router->count != 15)
        die("noise_router missing or not 15 slots", sub);

    hc_df_compiler_t comp;
    if (hc_df_compiler_init(&comp, &g_graph, &g_arena, seed, g_dfs, g_n_dfs,
                            g_noises, g_n_noises) != 0)
        die("compiler init failed", NULL);
    memset(&g_roots, -1, sizeof g_roots);
    for (const hc_json_t *m = router->child; m; m = m->next) {
        int32_t r = hc_df_compile_expr(&comp, m);
        if (r < 0)
            die("compile failed", comp.err ? comp.err : "?");
#define S(k)                                                                   \
    if ((int32_t)strlen(#k) == m->klen &&                                      \
        memcmp(m->key, #k, (size_t)m->klen) == 0)                              \
    g_roots.k = r
        S(final_density);
        S(barrier);
        S(fluid_level_floodedness);
        S(fluid_level_spread);
        S(lava);
        S(erosion);
        S(depth);
        S(preliminary_surface_level);
        S(vein_toggle);
        S(vein_ridged);
        S(vein_gap);
#undef S
    }
    g_graph.root = g_roots.final_density;
    if (g_roots.final_density < 0)
        die("router slot missing", NULL);
}

/* 콘-스트림 커널 루프 — gen_noise_stage.c:55-127 의 nc 호출 시퀀스에서
 * update_for_{y,x,z}/aquifer/vein/블록 루프를 뺀 것 (커널 정의역). */
static void kernel_pass(hc_noise_chunk_t *nc) {
    hc_nc_initialize_first_cell_x(nc);
    int32_t cw = nc->cell_width;
    int32_t cells_xz = 16 / cw;
    for (int32_t cell_x = 0; cell_x < cells_xz; cell_x++) {
        hc_nc_advance_cell_x(nc, cell_x);
        for (int32_t cell_z = 0; cell_z < cells_xz; cell_z++)
            for (int32_t cell_y = nc->cell_count_y - 1; cell_y >= 0;
                 cell_y--)
                hc_nc_select_cell_yz(nc, cell_y, cell_z);
        hc_nc_swap_slices(nc);
    }
}

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

/* 관심 카운터 덤프 (--ctr). HOT 계수는 HOT 빌드에서만 비영. */
static void dump_ctrs(void) {
    static const struct {
        int         ev;
        const char *name;
    } tab[] = {
        {HC_CTR_X8_SLICE, "x8_slice"},   {HC_CTR_X8_CELL, "x8_cell"},
        {HC_CTR_X8_NODE, "x8_node"},     {HC_CTR_X8_PERLIN, "x8_perlin"},
        {HC_CTR_X8_SCALAR_FB, "x8_scalar_fb"},
        {HC_CTR_X8_RC_MIX, "x8_rc_mix"}, {HC_CTR_X8_IS_MIX, "x8_is_mix"},
        {HC_CTR_X8_BLEND_MIX, "x8_blend_mix"},
        {HC_CTR_X4_SLICE, "x4_slice"},   {HC_CTR_X4_CELL, "x4_cell"},
        {HC_CTR_X4_NODE, "x4_node"},     {HC_CTR_X4_PERLIN, "x4_perlin"},
        {HC_CTR_SP_NODE, "sp_node"},     {HC_CTR_FTS_ITER, "fts_iter"},
        {HC_CTR_ZOOM_Q, "zoom_q"},       {HC_CTR_ZOOM_MISS, "zoom_miss"},
    };
    hc_ctr_flush();
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        printf("mb ctr_%s %" PRIu64 "\n", tab[i].name,
               hc_ctr_total(tab[i].ev));
}

static int mode_df(const char *repo, const char *isa, int iters, int warm,
                   int n_coords, int want_ctr) {
    /* 요청 ISA 강제 — 강등되면 부재 호스트 (exit 77, ctest SKIP 규약) */
    int want = !strcmp(isa, "scalar")  ? HC_ISA_SCALAR
               : !strcmp(isa, "avx2")  ? HC_ISA_AVX2
               : !strcmp(isa, "avx512") ? HC_ISA_AVX512
                                        : -1;
    if (want < 0)
        die("unknown --isa", isa);
    hc_isa_force(want);
    if ((int)hc_isa_active() != want) {
        fprintf(stderr, "SKIP: host lacks requested ISA %s\n", isa);
        return 77;
    }

    int64_t seed = 1234567890;
    hc_arena_init(&g_arena, g_backing, sizeof g_backing);
    df_setup(repo, seed);
    if (want_ctr)
        hc_ctr_enable();

    /* 리전 r.0.0 산개 좌표 (결정적) — 지형별 노드-믹스 분산 확인용 */
    static const int32_t coords[][2] = {{0, 0},   {31, 31}, {5, 17},
                                        {17, 5},  {11, 28}, {28, 11},
                                        {23, 23}, {8, 8}};
    int max_coords = (int)(sizeof coords / sizeof coords[0]);
    if (n_coords < 1 || n_coords > max_coords)
        n_coords = max_coords;

    printf("mb mode df\n");
    printf("mb isa %s\n", isa);
    printf("mb iters %d\n", iters);
    printf("mb warm %d\n", warm);
    printf("mb coords %d\n", n_coords);

    size_t   snap = g_arena.off; /* compile 이후 — nc 는 여기서 되감는다 */
    int64_t *lat = malloc((size_t)iters * sizeof *lat);
    if (!lat)
        die("malloc lat", NULL);

    int64_t grand_cpu = 0, grand_mono = 0;
    for (int c = 0; c < n_coords; c++) {
        int32_t cx = coords[c][0], cz = coords[c][1];

        hc_noise_chunk_t *nc = NULL;
        if (warm) {
            g_arena.off = snap;
            nc = hc_arena_alloc(&g_arena, sizeof *nc,
                                _Alignof(hc_noise_chunk_t));
            if (!nc || hc_nc_init(nc, &g_arena, &g_graph, &g_roots, seed,
                                  cx, cz, 63))
                die("hc_nc_init failed", NULL);
            if (want == HC_ISA_AVX512 && !nc->x8)
                die("nc not x8-eligible", NULL);
            if (want == HC_ISA_AVX2 && !nc->x4)
                die("nc not x4-eligible", NULL);
        }

        int64_t cpu_sum = 0, mono_sum = 0;
        for (int it = 0; it < iters; it++) {
            if (!warm) {
                g_arena.off = snap;
                nc = hc_arena_alloc(&g_arena, sizeof *nc,
                                    _Alignof(hc_noise_chunk_t));
                if (!nc || hc_nc_init(nc, &g_arena, &g_graph, &g_roots,
                                      seed, cx, cz, 63))
                    die("hc_nc_init failed", NULL);
            }
            int64_t m0 = now_ns(), c0 = tcpu_ns();
            kernel_pass(nc);
            int64_t c1 = tcpu_ns(), m1 = now_ns();
            lat[it] = c1 - c0;
            cpu_sum += c1 - c0;
            mono_sum += m1 - m0;
        }
        grand_cpu += cpu_sum;
        grand_mono += mono_sum;

        int64_t first = lat[0];
        qsort(lat, (size_t)iters, sizeof *lat, cmp_i64);
        printf("mb coord %d cx %d cz %d first_ns %" PRId64 " med_ns %" PRId64
               " min_ns %" PRId64 " cpu_sum_ns %" PRId64 "\n",
               c, cx, cz, first, lat[iters / 2], lat[0], cpu_sum);
        fprintf(stderr, "  df coord (%d,%d): med %.3f ms\n", cx, cz,
                (double)lat[iters / 2] / 1e6);
    }
    printf("mb grand_cpu_ns %" PRId64 "\n", grand_cpu);
    printf("mb grand_mono_ns %" PRId64 "\n", grand_mono);
    printf("mb kernel_passes %d\n", n_coords * iters);
    if (want_ctr)
        dump_ctrs();
    free(lat);
    return 0;
}

static int mode_zoom(const char *repo, int passes, int want_ctr) {
    char path[1024];
    int64_t seed = 1234567890;
    snprintf(path, sizeof path, "%s/golden/rng/surface_seed%" PRId64 ".txt",
             repo, seed);
    hc_arena_init(&g_arena, g_backing, sizeof g_backing);
    char *txt = read_file(path);

    /* obfuscated_seed 대조 (골든 파일 헤더) */
    int64_t obf = hc_biome_obfuscate_seed(seed);
    char   *hdr = strstr(txt, "obfuscated_seed ");
    if (hdr) {
        int64_t want = strtoll(hdr + 16, NULL, 10);
        if (want != obf)
            die("obfuscated_seed mismatch vs golden header", path);
    }

    enum { MAXV = 20000 };
    static int32_t vx[MAXV], vy[MAXV], vz[MAXV];
    int            nv = 0;
    for (char *p = txt; (p = strstr(p, "zoomed ")) != NULL;) {
        p += 7;
        char *end;
        long  x = strtol(p, &end, 10);
        long  y = strtol(end, &end, 10);
        long  z = strtol(end, &end, 10);
        if (nv >= MAXV)
            die("too many zoomed vectors", path);
        vx[nv] = (int32_t)x;
        vy[nv] = (int32_t)y;
        vz[nv] = (int32_t)z;
        nv++;
        p = end;
    }
    if (nv < 10000)
        die("too few zoomed vectors (golden format?)", path);
    if (want_ctr)
        hc_ctr_enable();

    printf("mb mode zoom\n");
    printf("mb vectors %d\n", nv);
    printf("mb passes %d\n", passes);

    int64_t  sink = 0;
    uint64_t q_prev = 0, miss_prev = 0;
    for (int pass = 0; pass < passes; pass++) {
        int64_t m0 = now_ns(), c0 = tcpu_ns();
        for (int i = 0; i < nv; i++) {
            int32_t qx, qy, qz;
            hc_biome_zoom(obf, vx[i], vy[i], vz[i], &qx, &qy, &qz);
            sink += qx + qy + qz;
        }
        int64_t c1 = tcpu_ns(), m1 = now_ns();
        printf("mb pass %d cpu_ns %" PRId64 " mono_ns %" PRId64
               " ns_per_q %.3f",
               pass, c1 - c0, m1 - m0, (double)(c1 - c0) / nv);
        if (want_ctr) {
            /* 패스별 델타 — 누적치 오독 방지 (B-4 §9 함정) */
            hc_ctr_flush();
            uint64_t q = hc_ctr_total(HC_CTR_ZOOM_Q);
            uint64_t miss = hc_ctr_total(HC_CTR_ZOOM_MISS);
            printf(" d_q %" PRIu64 " d_miss %" PRIu64, q - q_prev,
                   miss - miss_prev);
            q_prev = q;
            miss_prev = miss;
        }
        printf("\n");
    }
    printf("mb sink %" PRId64 "\n", sink);
    if (want_ctr)
        dump_ctrs();
    return 0;
}

static int mode_sha(const char *backend, int64_t total, int chunk) {
    if (!strcmp(backend, "ni")) {
        hc_sha256_force(1);
        if (!hc_sha256_ni_active())
            die("--backend ni: host lacks SHA-NI", NULL);
    } else if (!strcmp(backend, "sw")) {
        hc_sha256_force(0);
    } else {
        die("unknown --backend", backend);
    }

    unsigned char *buf = malloc((size_t)chunk);
    if (!buf)
        die("malloc sha buf", NULL);
    uint64_t s = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < chunk; i++) {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        buf[i] = (unsigned char)(s * 0x2545F4914F6CDD1DULL >> 56);
    }

    hc_sha256_ctx_t ctx;
    hc_sha256_init(&ctx);
    int64_t m0 = now_ns(), c0 = tcpu_ns();
    int64_t left = total;
    while (left > 0) {
        int n = left < chunk ? (int)left : chunk;
        hc_sha256_update(&ctx, buf, (size_t)n);
        left -= n;
    }
    uint8_t out[32];
    hc_sha256_final(&ctx, out);
    int64_t c1 = tcpu_ns(), m1 = now_ns();

    printf("mb mode sha\n");
    printf("mb backend %s\n", backend);
    printf("mb total_bytes %" PRId64 "\n", total);
    printf("mb chunk_bytes %d\n", chunk);
    printf("mb cpu_ns %" PRId64 "\n", c1 - c0);
    printf("mb mono_ns %" PRId64 "\n", m1 - m0);
    printf("mb gb_per_s %.3f\n", (double)total / (double)(m1 - m0));
    printf("mb digest8 %02x%02x%02x%02x%02x%02x%02x%02x\n", out[0], out[1],
           out[2], out[3], out[4], out[5], out[6], out[7]);
    free(buf);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2)
        die("usage: microbench df|zoom|sha [options]", NULL);
    const char *mode = argv[1];
    const char *repo = ".", *isa = "avx512", *backend = "ni";
    int         iters = 64, warm = 0, n_coords = 0, passes = 8, want_ctr = 0;
    int64_t     total = 47243127;
    int         chunk = 46 * 1024;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--repo") && i + 1 < argc)
            repo = argv[++i];
        else if (!strcmp(argv[i], "--isa") && i + 1 < argc)
            isa = argv[++i];
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc)
            iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--warm"))
            warm = 1;
        else if (!strcmp(argv[i], "--coords") && i + 1 < argc)
            n_coords = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ctr"))
            want_ctr = 1;
        else if (!strcmp(argv[i], "--passes") && i + 1 < argc)
            passes = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--backend") && i + 1 < argc)
            backend = argv[++i];
        else if (!strcmp(argv[i], "--total") && i + 1 < argc)
            total = strtoll(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--chunk") && i + 1 < argc)
            chunk = atoi(argv[++i]);
        else
            die("unknown arg", argv[i]);
    }
    if (iters < 1 || iters > 65536)
        die("--iters out of range", NULL);
    if (passes < 1 || passes > 4096)
        die("--passes out of range", NULL);
    if (chunk < 1 || total < 1)
        die("--chunk/--total must be >= 1", NULL);

    if (!strcmp(mode, "df"))
        return mode_df(repo, isa, iters, warm, n_coords, want_ctr);
    if (!strcmp(mode, "zoom"))
        return mode_zoom(repo, passes, want_ctr);
    if (!strcmp(mode, "sha"))
        return mode_sha(backend, total, chunk);
    die("unknown mode", mode);
    return 2;
}
