/* 라이브-콘 기계 등가 게이트 (P2-2).
 *
 * P2-1/P2-2 의 콘 평가 (root×mode 콘, WINDOW_SAFE flat 컷, y-불변 분할)
 * 는 전부 "프리픽스 [0..root] 워크와 비트 동일" 이 계약이다. 이 테스트는
 * 그 계약을 직접 게이트한다: reference/ 클로저를 컴파일해 실제 nc 를
 * 만들고, 슬롯×모드×좌표 배터리를 (a) 콘 경로 (hc_nc_eval_sp/_block,
 * cone_cell, fill_slice) 와 (b) 프리픽스 워크 (hc_df_eval_ex, 동일 cc)
 * 로 이중 평가해 IEEE-754 비트로 대조한다.
 *
 * 기존 골든과의 역할 분담: router_slots 골든은 cc==NULL 순수 수학그래프
 * 값을 바닐라 덤프에 고정하고, noise_stage 골든은 게이트 청크의 실제
 * 스테이지 출력을 고정한다. 이 테스트는 그 사이 — 콘/컷/분할/향후 메모
 * 기계가 프리픽스 시맨틱에서 벗어나는 클래스 (잘못된 컷, 스테일 호이스트,
 * 잘못된 캐시 키) 를 창-밖 컬럼·음수 좌표·재방문 시퀀스까지 포함해 잡는다.
 *
 * false-PASS 방어: 배터리 크기를 상수로 고정하고 총 검사 수를 대조한다.
 * 좌표 배터리는 창 안/경계/밖(aquifer 실경로인 -3..+1 청크 오프셋)과
 * 재방문 인터리브 (A,B,A — 상태 누출 검출) 를 포함한다. */

#undef NDEBUG

#include "../../core/src/hc_df_compile.h"
#include "../../core/src/hc_gen_noise.h"

#include <assert.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_fails = 0;

static void die(const char *msg, const char *detail) {
    fprintf(stderr, "SETUP ERROR: %s%s%s\n", msg, detail ? ": " : "",
            detail ? detail : "");
    exit(2);
}

static unsigned char g_backing[96u << 20];
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
    if (!v) {
        fprintf(stderr, "JSON parse error in %s at %zu: %s\n", path, pos,
                err ? err : "?");
        exit(2);
    }
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
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        DIR *probe = opendir(sub);
        if (probe) {
            closedir(probe);
            if (depth >= 2)
                die("reference tree deeper than expected", sub);
            char pref[512];
            snprintf(pref, sizeof pref, "%s%s/", rel_prefix, e->d_name);
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

/* 프리픽스 참조 평가: root 만 바꾼 얕은 사본 + 전용 scratch + 동일 cc.
 * (nc_eval_prefix 와 동형 — 콘 경로와 코드·버퍼가 분리돼 있어 콘 쪽
 * scratch 오염이 참조값에 스미지 않는다.) */
static const hc_df_graph_t *g_graph;
static double              *g_ref_scratch;

static double ref_eval(hc_noise_chunk_t *nc, int32_t root, hc_df_mode_t mode,
                       double x, double y, double z) {
    hc_df_graph_t gc = *g_graph;
    gc.root = root;
    nc->cc.mode = mode;
    return hc_df_eval_ex(&gc, x, y, z, g_ref_scratch, &nc->cc);
}

static void check_bits(const char *what, int32_t root, double x, double y,
                       double z, double got, double want) {
    uint64_t gb, wb;
    memcpy(&gb, &got, 8);
    memcpy(&wb, &want, 8);
    g_checks++;
    if (gb != wb) {
        g_fails++;
        if (g_fails <= 40)
            fprintf(stderr,
                    "FAIL %s root=%d (%g,%g,%g): cone %.17g "
                    "(0x%016" PRIx64 ") != prefix %.17g (0x%016" PRIx64 ")\n",
                    what, root, x, y, z, got, gb, want, wb);
    }
}

/* --- 좌표 배터리 --- */

/* 청크-상대 XZ 오프셋: 창 안 (정렬/비정렬/경계) + 창 밖 (aquifer 의
 * SURF_OFFS 실경로 -3..+1 청크, compute_fluid 소스 지터 포함). */
static const int32_t XZ_OFFS[][2] = {
    {0, 0},   {3, 5},   {12, 15}, {15, 15}, {16, 0},  {0, 16},
    {19, 21}, {-1, -1}, {-4, 0},  {-16, 9}, {-48, 5}, {-41, 17},
    {16, 17}, {31, -9},
};
#define N_XZ ((int32_t)(sizeof XZ_OFFS / sizeof XZ_OFFS[0]))

static const int32_t YS[] = {-64, -60, -8, 0, 17, 63, 128, 319};
#define N_Y ((int32_t)(sizeof YS / sizeof YS[0]))

enum { N_SP_ROOTS = 6, N_BLOCK_ROOTS = 4 };

/* 배터리 크기 동결 (false-PASS 방어): main 말미에서 대조 */
#define WANT_SP (2 * N_SP_ROOTS * N_XZ * N_Y)       /* 청크 2개 */
#define WANT_REVISIT (2 * 3 * N_SP_ROOTS)           /* A,B,A 인터리브 */
#define WANT_BLOCK (2 * N_BLOCK_ROOTS * N_XZ * N_Y) /* BLOCK 모드 */
#define WANT_CELL (2 * 5 * 3)                       /* 셀 3점 × 5셀 */
#define WANT_SLICE (2 * 5 * 49 * 8) /* 슬라이스 5컬럼 × 49y × interp 8 */

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_df_cones <ref_dir>\n");
        return 2;
    }
    const char *ref_dir = argv[1];
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

    int64_t          seed = 1234567890;
    hc_df_compiler_t comp;
    hc_df_graph_t    graph;
    if (hc_df_compiler_init(&comp, &graph, &g_arena, seed, g_dfs, g_n_dfs,
                            g_noises, g_n_noises) != 0)
        die("compiler init failed", NULL);
    hc_noise_roots_t roots;
    memset(&roots, -1, sizeof roots);
    for (const hc_json_t *m = router->child; m; m = m->next) {
        int32_t r = hc_df_compile_expr(&comp, m);
        if (r < 0) {
            fprintf(stderr, "compile failed: %s\n", comp.err ? comp.err : "?");
            return 2;
        }
#define S(k)                                                                   \
    if ((int32_t)strlen(#k) == m->klen &&                                      \
        memcmp(m->key, #k, (size_t)m->klen) == 0)                              \
    roots.k = r
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
    graph.root = roots.final_density;
    if (roots.final_density < 0 || roots.preliminary_surface_level < 0 ||
        roots.vein_toggle < 0)
        die("router slot missing", NULL);
    g_graph = &graph;
    g_ref_scratch = hc_arena_alloc(&g_arena, sizeof(double) * 2 *
                                                 (size_t)graph.n,
                                   _Alignof(double));
    if (!g_ref_scratch)
        die("arena exhausted (ref scratch)", NULL);

    const int32_t sp_roots[N_SP_ROOTS] = {
        roots.preliminary_surface_level, roots.erosion, roots.depth,
        roots.fluid_level_floodedness,   roots.fluid_level_spread,
        roots.lava,
    };
    const int32_t block_roots[N_BLOCK_ROOTS] = {
        roots.barrier, roots.vein_toggle, roots.vein_ridged, roots.vein_gap,
    };

    /* 청크 2개: 원점과 음수 좌표 (floorDiv/쿼트 창 경계의 부호 케이스) */
    const int32_t chunks[2][2] = {{0, 0}, {-3, -5}};
    for (int ci = 0; ci < 2; ci++) {
        int32_t cx = chunks[ci][0], cz = chunks[ci][1];
        hc_noise_chunk_t *nc =
            hc_arena_alloc(&g_arena, sizeof *nc, _Alignof(hc_noise_chunk_t));
        if (!nc || hc_nc_init(nc, &g_arena, &graph, &roots, seed, cx, cz, 63))
            die("hc_nc_init failed (arena?)", NULL);

        int32_t bx0 = cx * 16, bz0 = cz * 16;

        /* --- SP: 콘 vs 프리픽스, 창 안/밖 × y 배터리 --- */
        for (int32_t r = 0; r < N_SP_ROOTS; r++)
            for (int32_t o = 0; o < N_XZ; o++)
                for (int32_t yi = 0; yi < N_Y; yi++) {
                    int32_t x = bx0 + XZ_OFFS[o][0];
                    int32_t z = bz0 + XZ_OFFS[o][1];
                    double  got = hc_nc_eval_sp(nc, sp_roots[r], x, YS[yi], z);
                    double  want = ref_eval(nc, sp_roots[r], HC_DF_MODE_SP,
                                            (double)x, (double)YS[yi],
                                            (double)z);
                    check_bits("sp", sp_roots[r], x, YS[yi], z, got, want);
                }

        /* --- SP 재방문 인터리브 (A,B,A): 상태 누출/캐시 키 클래스 --- */
        for (int32_t r = 0; r < N_SP_ROOTS; r++) {
            const int32_t seq[3][2] = {{0, 0}, {-48, 5}, {0, 0}};
            for (int s = 0; s < 3; s++) {
                int32_t x = bx0 + seq[s][0], z = bz0 + seq[s][1];
                double  got = hc_nc_eval_sp(nc, sp_roots[r], x, 0, z);
                double  want = ref_eval(nc, sp_roots[r], HC_DF_MODE_SP,
                                        (double)x, 0.0, (double)z);
                check_bits("sp-revisit", sp_roots[r], x, 0, z, got, want);
            }
        }

        /* --- 셀 상태 준비: fill_slice 등가를 먼저 검사 (slice0 이 첫
         * 슬라이스 상태를 유지하는 동안), 그 뒤 BLOCK/CELL --- */
        hc_nc_initialize_first_cell_x(nc);
        {
            int32_t stride = nc->cell_count_y + 1;
            for (int32_t j = 0; j <= nc->cell_count_xz; j++) {
                int32_t bz = (nc->first_cell_z + j) * nc->cell_width;
                for (int32_t i = 0; i <= nc->cell_count_y; i++) {
                    int32_t by =
                        (i + nc->cell_noise_min_y) * nc->cell_height;
                    for (int32_t k = 0; k < nc->n_interp; k++) {
                        double got = nc->interp[k].slice0[j * stride + i];
                        double want = ref_eval(
                            nc, graph.nodes[nc->interp[k].node].a,
                            HC_DF_MODE_SP, (double)(nc->first_cell_x * 4),
                            (double)by, (double)bz);
                        check_bits("slice", nc->interp[k].node,
                                   nc->first_cell_x * 4, by, bz, got, want);
                    }
                }
            }
        }

        /* --- CELL: cone_cell vs 프리픽스 (셀 5개 × 셀 내 3점) --- */
        hc_nc_advance_cell_x(nc, 0);
        const int32_t cells[5][2] = {{47, 0}, {40, 1}, {20, 2}, {8, 3}, {0, 0}};
        for (int cl = 0; cl < 5; cl++) {
            hc_nc_select_cell_yz(nc, cells[cl][0], cells[cl][1]);
            const int32_t pts[3][3] = {{0, 0, 0}, {3, 7, 3}, {1, 4, 2}};
            for (int p = 0; p < 3; p++) {
                nc->cc.in_cell_x = pts[p][0];
                nc->cc.in_cell_y = pts[p][1];
                nc->cc.in_cell_z = pts[p][2];
                int32_t x = nc->cell_start_x + pts[p][0];
                int32_t y = nc->cell_start_y + pts[p][1];
                int32_t z = nc->cell_start_z + pts[p][2];
                nc->cc.mode = HC_DF_MODE_CELL;
                double got = hc_df_eval_cone(
                    &graph, nc->cone_cell.list, nc->cone_cell.len,
                    roots.final_density, (double)x, (double)y, (double)z,
                    nc->scratch, &nc->cc, nc->cone_cell.mask);
                double want = ref_eval(nc, roots.final_density,
                                       HC_DF_MODE_CELL, (double)x, (double)y,
                                       (double)z);
                check_bits("cell", roots.final_density, x, y, z, got, want);
            }
        }

        /* --- BLOCK: 점진 lerp 상태를 실제 경로로 세팅 후 배터리 --- */
        hc_nc_update_for_y(nc, nc->cell_start_y + 3, 3.0 / 8.0);
        hc_nc_update_for_x(nc, nc->cell_start_x + 1, 1.0 / 4.0);
        hc_nc_update_for_z(nc, nc->cell_start_z + 2, 2.0 / 4.0);
        for (int32_t r = 0; r < N_BLOCK_ROOTS; r++)
            for (int32_t o = 0; o < N_XZ; o++)
                for (int32_t yi = 0; yi < N_Y; yi++) {
                    int32_t x = bx0 + XZ_OFFS[o][0];
                    int32_t z = bz0 + XZ_OFFS[o][1];
                    double  got =
                        hc_nc_eval_block(nc, block_roots[r], x, YS[yi], z);
                    double  want = ref_eval(nc, block_roots[r],
                                            HC_DF_MODE_BLOCK, (double)x,
                                            (double)YS[yi], (double)z);
                    check_bits("block", block_roots[r], x, YS[yi], z, got,
                               want);
                }
    }

    int want_total =
        WANT_SP + WANT_REVISIT + WANT_BLOCK + WANT_CELL + WANT_SLICE;
    if (g_checks != want_total) {
        fprintf(stderr, "BATTERY SIZE MISMATCH: ran %d checks, want %d\n",
                g_checks, want_total);
        return 2;
    }
    printf("test_df_cones: %d nodes; %d checks, %d failures\n", graph.n,
           g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
