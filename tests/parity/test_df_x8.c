/* scalar-vs-avx512 백엔드 동일성 게이트 — 유닛 판 (P2-10, ADR-004 D4).
 * test_df_x4 (P2-4) 의 AVX-512 판 — 배터리·판정면 동일.
 *
 * 같은 그래프·같은 좌표 시퀀스를 스칼라 백엔드 nc 와 AVX-512 백엔드 nc
 * 로 이중 실행해 관측면 (fill_slice 슬라이스 값, select_cell_yz 의
 * density_cell) 을 IEEE-754 비트로 대조한다. x8 커널이 지나는 전 op
 * (NOISE 33, BLENDED, YCG, choice 컨트롤, INTERP lerp3, jmin/jmax ±0/NaN
 * 선택 로직) 이 실좌표 분포로 커버된다. 리전 판은
 * scripts/check_isa_equiv.sh (풀 리전 canonical 3-way).
 *
 * AVX-512(F/DQ/BW/VL) 부재 호스트는 SKIP (exit 77, ctest
 * SKIP_RETURN_CODE) — 로컬 claw(Zen3) 가 이 경우다. 실판정은 hc-e6
 * (Zen5) 등 AVX-512 호스트에서 (P2-10 노트에 기록).
 *
 * false-PASS 방어: 배터리 크기 동결 (df_cones 와 동일 원칙). */

#undef NDEBUG

#include "../../core/src/hc_counters.h"
#include "../../core/src/hc_df_compile.h"
#include "../../core/src/hc_df_simd.h"
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

static void check_bits(const char *what, int32_t i0, int32_t i1, double got,
                       double want) {
    uint64_t gb, wb;
    memcpy(&gb, &got, 8);
    memcpy(&wb, &want, 8);
    g_checks++;
    if (gb != wb) {
        g_fails++;
        if (g_fails <= 40)
            fprintf(stderr,
                    "FAIL %s [%d,%d]: avx512 %.17g (0x%016" PRIx64
                    ") != scalar %.17g (0x%016" PRIx64 ")\n",
                    what, i0, i1, got, gb, want, wb);
    }
}

/* 배터리 동결 (false-PASS 방어) */
#define N_CELLS 6
#define WANT_SLICE0 (2 * 5 * 49 * 8) /* 청크 2 × 5컬럼 × 49y × interp 8 */
#define WANT_SLICE1 (2 * 5 * 49 * 8) /* advance 후 slice1 동일 배터리 */
#define WANT_CELL (2 * N_CELLS * 128) /* 셀 6개 × density_cell 128 */

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_df_x8 <ref_dir>\n");
        return 2;
    }
    hc_arena_init(&g_arena, g_backing, sizeof g_backing);

    /* AVX-512 부재 호스트 → SKIP (강제가 하위 레벨로 강등되면 부재) */
    hc_isa_force(HC_ISA_AVX512);
    if (hc_isa_active() != HC_ISA_AVX512) {
        fprintf(stderr,
                "SKIP: host lacks AVX-512 (F/DQ/BW/VL) — backend gate "
                "vacuous\n");
        return 77;
    }

    /* P2-11 비-공허 방어: 2-웨이 인터리브 경로가 실제로 돌았는지 카운터로
     * 확인한다 (dual 이 조용히 비활성화되면 이 게이트의 dual 커버가
     * 공허해진다). 단일 스레드 테스트 — 스폰 규약 무관. */
    hc_ctr_enable();

    const char *ref_dir = argv[1];
    char        sub[1024];
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
    if (roots.final_density < 0)
        die("router slot missing", NULL);

    /* 청크 2개 (원점 + 음수 좌표), 백엔드별 nc 를 강제-초기화 */
    const int32_t chunks[2][2] = {{0, 0}, {-3, -5}};
    for (int ci = 0; ci < 2; ci++) {
        int32_t cx = chunks[ci][0], cz = chunks[ci][1];

        hc_isa_force(HC_ISA_SCALAR);
        hc_noise_chunk_t *ncs =
            hc_arena_alloc(&g_arena, sizeof *ncs, _Alignof(hc_noise_chunk_t));
        if (!ncs || hc_nc_init(ncs, &g_arena, &graph, &roots, seed, cx, cz, 63))
            die("hc_nc_init scalar failed (arena?)", NULL);
        assert(ncs->x4 == 0 && ncs->x8 == 0);

        hc_isa_force(HC_ISA_AVX512);
        hc_noise_chunk_t *ncv =
            hc_arena_alloc(&g_arena, sizeof *ncv, _Alignof(hc_noise_chunk_t));
        if (!ncv || hc_nc_init(ncv, &g_arena, &graph, &roots, seed, cx, cz, 63))
            die("hc_nc_init avx512 failed (arena?)", NULL);
        assert(ncv->x8 == 1);
        /* 26.2 핫 콘은 x8 적격이어야 한다 — 화이트리스트 회귀 fail-loud */
        assert(ncv->cone_slice_var.x4_ok && ncv->cone_cell.x4_ok);

        int32_t stride = ncs->cell_count_y + 1;

        /* --- fill_slice: slice0 (initialize) --- */
        hc_nc_initialize_first_cell_x(ncs);
        hc_nc_initialize_first_cell_x(ncv);
        for (int32_t j = 0; j <= ncs->cell_count_xz; j++)
            for (int32_t i = 0; i <= ncs->cell_count_y; i++)
                for (int32_t k = 0; k < ncs->n_interp; k++)
                    check_bits("slice0", j * stride + i, k,
                               ncv->interp[k].slice0[j * stride + i],
                               ncs->interp[k].slice0[j * stride + i]);

        /* --- fill_slice: slice1 (advance) --- */
        hc_nc_advance_cell_x(ncs, 0);
        hc_nc_advance_cell_x(ncv, 0);
        for (int32_t j = 0; j <= ncs->cell_count_xz; j++)
            for (int32_t i = 0; i <= ncs->cell_count_y; i++)
                for (int32_t k = 0; k < ncs->n_interp; k++)
                    check_bits("slice1", j * stride + i, k,
                               ncv->interp[k].slice1[j * stride + i],
                               ncs->interp[k].slice1[j * stride + i]);

        /* --- select_cell_yz: density_cell 128 값 (y 극단 포함 6셀) --- */
        const int32_t cells[N_CELLS][2] = {{47, 0}, {40, 1}, {24, 3},
                                           {12, 2}, {5, 1},  {0, 0}};
        for (int cl = 0; cl < N_CELLS; cl++) {
            hc_nc_select_cell_yz(ncs, cells[cl][0], cells[cl][1]);
            hc_nc_select_cell_yz(ncv, cells[cl][0], cells[cl][1]);
            for (int32_t t = 0; t < 128; t++)
                check_bits("cell", cl, t, ncv->density_cell[t],
                           ncs->density_cell[t]);
        }
    }

    int want_total = WANT_SLICE0 + WANT_SLICE1 + WANT_CELL;
    if (g_checks != want_total) {
        fprintf(stderr, "BATTERY SIZE MISMATCH: ran %d checks, want %d\n",
                g_checks, want_total);
        return 2;
    }
    /* P2-11 비-공허 방어: dual 경로 실작동 확인 (fill_slice 48=6그룹 →
     * 페어 3 + select_cell iy 8 → 페어 8, 청크/셀 배터리에서 반드시 >0) */
    hc_ctr_flush();
    if (hc_ctr_total(HC_CTR_X8_DUAL) == 0) {
        fprintf(stderr, "DUAL PATH VACUOUS: x8_dual counter is 0\n");
        return 2;
    }
    printf("test_df_x8: %d nodes; %d checks, %d failures; x8_dual %llu "
           "x8_split %llu\n",
           graph.n, g_checks, g_fails,
           (unsigned long long)hc_ctr_total(HC_CTR_X8_DUAL),
           (unsigned long long)hc_ctr_total(HC_CTR_X8_SPLIT));
    return g_fails == 0 ? 0 : 1;
}
