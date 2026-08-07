#include "hc_gen_noise.h"
#include "hc_df_simd.h"

#include <assert.h>
#include <math.h>
#include <string.h>

/* NoiseChunk 26.2 (javap) — 셀 상태 기계.
 *
 * 바닐라 대응:
 *  - wrapNew: interpolated → NoiseInterpolator, flat_cache → FlatCache
 *    (여기서는 노드별 상태 배열 + hc_df_cellctx_t 모드로 재현).
 *  - fillSlice: 셀 코너 컬럼을 wrapped 함수의 SinglePointContext-급
 *    평가로 채운다 (x = 절대셀*4, y = (i+cellNoiseMinY)*8, z = (firstCellZ
 *    +j)*4; i = 0..cellCountY, j = 0..cellCountXZ 둘 다 '포함').
 *  - selectCellYZ: 코너 8개 선택 후 fillingCell 로 density_cell 채움
 *    (y 내림차순, x, z — NoiseChunk.fillAllDirectly 순서). 이 경로의
 *    interpolated 는 Mth.lerp3 (x→y→z 중첩) 이고, 블록 루프의
 *    updateForY→X→Z (y→x→z) 와 FP 순서가 다르다.
 *  - beardifier: cacheAllInCell(final_density + beardifier) 의 원소별
 *    덧셈 (beard.c). 구조물 참조 없는 청크 (nc->beard == NULL) 는
 *    Beardifier.EMPTY 로 정확히 `+ 0.0` (-0.0 → +0.0 정규화까지 동일). */

static const hc_df_cellctx_t *nc_cc(hc_noise_chunk_t *nc, hc_df_mode_t mode) {
    nc->cc.mode = mode;
    return &nc->cc;
}

/* 프리픽스 워크 폴백 — 콘 디스패치 미스 전용 (항상 옳고 느릴 뿐) */
static double nc_eval_prefix(hc_noise_chunk_t *nc, int32_t root, double x,
                             double y, double z, hc_df_mode_t mode) {
    hc_df_graph_t g = *nc->g; /* root 만 바꾼 얕은 사본 */
    g.root = root;
    return hc_df_eval_ex(&g, x, y, z, nc->scratch, nc_cc(nc, mode));
}

/* root 디스패치 (엔트리 ≤ 6 선형 탐색 — eval 비용 대비 무시 가능) */
static const hc_nc_cone_t *find_cone(const hc_nc_cone_t *arr, int32_t n,
                                     int32_t root) {
    for (int32_t i = 0; i < n; i++)
        if (arr[i].root == root)
            return &arr[i];
    return NULL;
}

static double nc_eval_cone(hc_noise_chunk_t *nc, const hc_nc_cone_t *co,
                           int32_t root, double x, double y, double z,
                           hc_df_mode_t mode) {
    if (co->prog)
        return hc_df_eval_prog(nc->g, co->prog, co->prog_words, root, x, y, z,
                               nc->scratch, nc_cc(nc, mode), co->mask);
    return hc_df_eval_cone(nc->g, co->list, co->len, root, x, y, z,
                           nc->scratch, nc_cc(nc, mode), co->mask);
}

double hc_nc_eval_sp(hc_noise_chunk_t *nc, int32_t root, int32_t x, int32_t y,
                     int32_t z) {
    const hc_nc_cone_t *co = find_cone(nc->cones_sp, HC_NC_N_SP_CONES, root);
    if (co)
        return nc_eval_cone(nc, co, root, (double)x, (double)y, (double)z,
                            HC_DF_MODE_SP);
    return nc_eval_prefix(nc, root, (double)x, (double)y, (double)z,
                          HC_DF_MODE_SP);
}

double hc_nc_eval_block(hc_noise_chunk_t *nc, int32_t root, int32_t x,
                        int32_t y, int32_t z) {
    const hc_nc_cone_t *co =
        find_cone(nc->cones_block, HC_NC_N_BLOCK_CONES, root);
    if (co)
        return nc_eval_cone(nc, co, root, (double)x, (double)y, (double)z,
                            HC_DF_MODE_BLOCK);
    return nc_eval_prefix(nc, root, (double)x, (double)y, (double)z,
                          HC_DF_MODE_BLOCK);
}

/* Mth.floor: (int)Math.floor(d) — Java d2i 는 saturating, NaN → 0 */
static int32_t mth_floor(double d) {
    double f = floor(d);
    if (f != f)
        return 0;
    if (f >= 2147483647.0)
        return INT32_MAX;
    if (f <= -2147483648.0)
        return INT32_MIN;
    return (int32_t)f;
}

/* --- preliminarySurfaceLevel (Long2IntMap 메모 — 값-중립) --- */

int32_t hc_nc_psl(hc_noise_chunk_t *nc, int32_t x, int32_t z) {
    /* QuartPos.toBlock(QuartPos.fromBlock(v)) = (v >> 2) << 2 — Java 의
     * 음수 좌시프트는 래핑이라 무부호로 우회한다 (값 범위상 동일) */
    int32_t qx = (int32_t)((uint32_t)(x >> 2) << 2);
    int32_t qz = (int32_t)((uint32_t)(z >> 2) << 2);
    /* ColumnPos.asLong(x, z) = (x & 0xFFFFFFFF) | (z << 32) */
    uint64_t key = (uint64_t)(uint32_t)qx | ((uint64_t)(uint32_t)qz << 32);
    uint32_t h = (uint32_t)(key ^ (key >> 29)) * 2654435761u;
    int32_t  mask = nc->psl_cap - 1;
    int32_t  i = (int32_t)(h & (uint32_t)mask);
    for (int32_t probes = 0; probes < nc->psl_cap;
         probes++, i = (i + 1) & mask) {
        if (!nc->psl_used[i]) {
            double v = hc_nc_eval_sp(nc, nc->roots.preliminary_surface_level,
                                     qx, 0, qz);
            nc->psl_used[i] = 1;
            nc->psl_key[i] = key;
            nc->psl_val[i] = mth_floor(v);
            return nc->psl_val[i];
        }
        if (nc->psl_key[i] == key)
            return nc->psl_val[i];
    }
    assert(!"psl memo full"); /* fail-loud: 용량 상한은 설계 오류 */
    return 0;
}

/* NoiseChunk.maxPreliminarySurfaceLevel — z 바깥, x 안쪽, step 4, 경계 포함 */
static int32_t nc_max_psl(hc_noise_chunk_t *nc, int32_t min_x, int32_t min_z,
                          int32_t max_x, int32_t max_z) {
    int32_t max = INT32_MIN;
    for (int32_t z = min_z; z <= max_z; z += 4)
        for (int32_t x = min_x; x <= max_x; x += 4) {
            int32_t v = hc_nc_psl(nc, x, z);
            if (v > max)
                max = v;
        }
    return max;
}

/* --- 상태 기계 --- */

static void nc_fill_slice(hc_noise_chunk_t *nc, int which,
                          int32_t abs_cell_x) {
    /* fillSlice(boolean, cellX): cellStartBlockX = cellX*cellWidth.
     * 슬라이스 포인트는 전부 청크 쿼트 창 안이라 flat_cache 는 항상
     * 테이블 히트다 — SP 모드 평가가 바닐라와 좌표 단위로 일치하고,
     * 콘은 WINDOW_SAFE (flat 자식 컷) 로 산출돼 있다 (hc_nc_init).
     *
     * 다중-루트 단일-워크 (P2-1): 점당 interp 자식 union 콘을 한 번만
     * 걷고 각 자식 값을 scratch 에서 회수한다 (hc_df.h 의 의도된 다중-
     * 루트 읽기). 평가는 순수라 (RNG 없음) 자식별 개별 워크와 값이
     * 비트 동일하다 — 공유 서브트리 중복 평가만 사라진다.
     *
     * y-불변 분할 (P2-2): (x,z) 컬럼당 y-불변부를 1회 평가하고 y 루프는
     * 가변부만 걷는다. y-불변 노드는 어느 y 에서도 비트 동일 값이라
     * (hc_df_mark_y_variant 보수 분류) 전 포인트 재평가와 결과가 같다.
     * y 인자는 관례상 첫 포인트의 by 를 넘긴다 (읽히지 않는다). */
    nc->cell_start_x = abs_cell_x * nc->cell_width;
    int32_t stride = nc->cell_count_y + 1;
    for (int32_t j = 0; j <= nc->cell_count_xz; j++) {
        int32_t bz = (nc->first_cell_z + j) * nc->cell_width;
        hc_df_eval_cone(nc->g, nc->cone_slice_inv.list,
                        nc->cone_slice_inv.len, -1,
                        (double)nc->cell_start_x,
                        (double)(nc->cell_noise_min_y * nc->cell_height),
                        (double)bz, nc->scratch, nc_cc(nc, HC_DF_MODE_SP),
                        nc->cone_slice_inv.mask);
        int32_t i = 0;
        /* AVX2 x4: y 4점 레인화 (P2-4). x/z 는 컬럼 고정 — 각 레인은
         * 독립 평가점이고 값은 스칼라 경로와 비트 동일 (df_x4 게이트).
         * y-불변부 값을 vscratch 에 브로드캐스트해 x4 스트림의 피연산자
         * 로 제공한다 (콘 닫힘상 var 의 콘-밖 피연산자는 inv 뿐). */
        if (nc->x4 && nc->cone_slice_var.x4_ok) {
            for (int32_t t = 0; t < nc->cone_slice_inv.len; t++) {
                int32_t n = nc->cone_slice_inv.list[t];
                double  v = nc->scratch[n];
                nc->vscratch[4 * n] = v;
                nc->vscratch[4 * n + 1] = v;
                nc->vscratch[4 * n + 2] = v;
                nc->vscratch[4 * n + 3] = v;
            }
            const int32_t *stream = nc->cone_slice_var.prog
                                        ? nc->cone_slice_var.prog
                                        : nc->cone_slice_var.list;
            int32_t swords = nc->cone_slice_var.prog
                                 ? nc->cone_slice_var.prog_words
                                 : nc->cone_slice_var.len;
            hc_df_lanes_t lanes;
            memset(&lanes, 0, sizeof lanes);
            for (; i + 3 <= nc->cell_count_y; i += 4) {
                for (int l = 0; l < 4; l++) {
                    lanes.x[l] = (double)nc->cell_start_x;
                    lanes.y[l] = (double)((i + l + nc->cell_noise_min_y) *
                                          nc->cell_height);
                    lanes.z[l] = (double)bz;
                }
                hc_df_eval_stream_x4_avx2(nc->g, stream, swords, &lanes,
                                          nc->vscratch,
                                          nc_cc(nc, HC_DF_MODE_SP));
                for (int32_t k = 0; k < nc->n_interp; k++) {
                    hc_df_interp_t *it = &nc->interp[k];
                    double *slice = which == 0 ? it->slice0 : it->slice1;
                    const double *v =
                        nc->vscratch + 4 * nc->g->nodes[it->node].a;
                    slice[j * stride + i] = v[0];
                    slice[j * stride + i + 1] = v[1];
                    slice[j * stride + i + 2] = v[2];
                    slice[j * stride + i + 3] = v[3];
                }
            }
            /* 꼬리 (49번째 포인트) 는 아래 스칼라 경로가 처리 */
        }
        for (; i <= nc->cell_count_y; i++) {
            int32_t by = (i + nc->cell_noise_min_y) * nc->cell_height;
            if (nc->cone_slice_var.prog)
                hc_df_eval_prog(nc->g, nc->cone_slice_var.prog,
                                nc->cone_slice_var.prog_words, -1,
                                (double)nc->cell_start_x, (double)by,
                                (double)bz, nc->scratch,
                                nc_cc(nc, HC_DF_MODE_SP),
                                nc->cone_slice_var.mask);
            else
                hc_df_eval_cone(nc->g, nc->cone_slice_var.list,
                                nc->cone_slice_var.len, -1,
                                (double)nc->cell_start_x, (double)by,
                                (double)bz, nc->scratch,
                                nc_cc(nc, HC_DF_MODE_SP),
                                nc->cone_slice_var.mask);
            for (int32_t k = 0; k < nc->n_interp; k++) {
                hc_df_interp_t *it = &nc->interp[k];
                double *slice = which == 0 ? it->slice0 : it->slice1;
                slice[j * stride + i] =
                    nc->scratch[nc->g->nodes[it->node].a];
            }
        }
    }
}

void hc_nc_initialize_first_cell_x(hc_noise_chunk_t *nc) {
    nc_fill_slice(nc, 0, nc->first_cell_x);
}

void hc_nc_advance_cell_x(hc_noise_chunk_t *nc, int32_t cell_x) {
    nc_fill_slice(nc, 1, nc->first_cell_x + cell_x + 1);
    nc->cell_start_x = (nc->first_cell_x + cell_x) * nc->cell_width;
}

void hc_nc_select_cell_yz(hc_noise_chunk_t *nc, int32_t cell_y,
                          int32_t cell_z) {
    int32_t stride = nc->cell_count_y + 1;
    for (int32_t k = 0; k < nc->n_interp; k++) {
        hc_df_interp_t *it = &nc->interp[k];
        const double   *s0 = it->slice0, *s1 = it->slice1;
        /* 자릿수 = x z y. slice[z][y], slice0 = x0, slice1 = x1 */
        it->n000 = s0[cell_z * stride + cell_y];
        it->n001 = s0[(cell_z + 1) * stride + cell_y];
        it->n100 = s1[cell_z * stride + cell_y];
        it->n101 = s1[(cell_z + 1) * stride + cell_y];
        it->n010 = s0[cell_z * stride + cell_y + 1];
        it->n011 = s0[(cell_z + 1) * stride + cell_y + 1];
        it->n110 = s1[cell_z * stride + cell_y + 1];
        it->n111 = s1[(cell_z + 1) * stride + cell_y + 1];
    }
    nc->cell_start_y = (cell_y + nc->cell_noise_min_y) * nc->cell_height;
    nc->cell_start_z = (nc->first_cell_z + cell_z) * nc->cell_width;

    /* fillingCell: density_cell 을 y 내림차순, x, z 순으로 채운다.
     * interpolationCounter 는 이 경로에서 증가하지 않지만 (NoiseChunk.
     * fillAllDirectly), final_density 하위에 counter-민감 노드가 셀 채움
     * 경로에 없음을 증명했으므로 (cache_once 는 전부 interpolated 안)
     * 카운터 자체는 재현할 필요가 없다. */
    int32_t ai = 0;
    /* AVX2 x4: iz 4점 레인화 (cell_width == 4 일 때 최내측 루프가 정확히
     * 4레인, P2-4). 각 레인은 독립 평가점 — 값은 스칼라 경로와 비트 동일
     * (df_x4 게이트). density_cell 채움 순서 (iy 내림, ix, iz) 보존. */
    if (nc->x4 && nc->cone_cell.x4_ok && nc->cell_width == 4) {
        const int32_t *stream =
            nc->cone_cell.prog ? nc->cone_cell.prog : nc->cone_cell.list;
        int32_t swords = nc->cone_cell.prog ? nc->cone_cell.prog_words
                                            : nc->cone_cell.len;
        for (int32_t iy = nc->cell_height - 1; iy >= 0; iy--)
            for (int32_t ix = 0; ix < nc->cell_width; ix++) {
                int32_t       bx = nc->cell_start_x + ix;
                int32_t       by = nc->cell_start_y + iy;
                int32_t       bz0 = nc->cell_start_z;
                hc_df_lanes_t lanes;
                for (int l = 0; l < 4; l++) {
                    lanes.x[l] = (double)bx;
                    lanes.y[l] = (double)by;
                    lanes.z[l] = (double)(bz0 + l);
                    /* interp_lerp3 델타 — 스칼라와 같은 식 */
                    lanes.dx[l] = (double)ix / (double)nc->cell_width;
                    lanes.dy[l] = (double)iy / (double)nc->cell_height;
                    lanes.dz[l] = (double)l / (double)nc->cell_width;
                }
                hc_df_eval_stream_x4_avx2(nc->g, stream, swords, &lanes,
                                          nc->vscratch,
                                          nc_cc(nc, HC_DF_MODE_CELL));
                const double *v =
                    nc->vscratch + 4 * (size_t)nc->roots.final_density;
                for (int l = 0; l < 4; l++)
                    nc->density_cell[ai++] =
                        v[l] + hc_beard_compute(nc->beard, bx, by, bz0 + l);
            }
        return;
    }
    for (int32_t iy = nc->cell_height - 1; iy >= 0; iy--)
        for (int32_t ix = 0; ix < nc->cell_width; ix++)
            for (int32_t iz = 0; iz < nc->cell_width; iz++) {
                nc->cc.in_cell_x = ix;
                nc->cc.in_cell_y = iy;
                nc->cc.in_cell_z = iz;
                int32_t bx = nc->cell_start_x + ix;
                int32_t by = nc->cell_start_y + iy;
                int32_t bz = nc->cell_start_z + iz;
                double  d = nc_eval_cone(nc, &nc->cone_cell,
                                         nc->roots.final_density, (double)bx,
                                         (double)by, (double)bz,
                                         HC_DF_MODE_CELL);
                /* Ap2 ADD fillArray 의 원소별 덧셈 — beardifier 부재/
                 * EMPTY/affectedBox 밖은 정확히 +0.0 (-0.0 정규화 동일) */
                nc->density_cell[ai++] =
                    d + hc_beard_compute(nc->beard, bx, by, bz);
            }
}

/* Mth.lerp(double) */
static double lerp_d(double t, double a, double b) {
    return a + t * (b - a);
}

void hc_nc_update_for_y(hc_noise_chunk_t *nc, int32_t block_y, double t) {
    nc->cc.in_cell_y = block_y - nc->cell_start_y;
    for (int32_t k = 0; k < nc->n_interp; k++) {
        hc_df_interp_t *it = &nc->interp[k];
        it->vxz00 = lerp_d(t, it->n000, it->n010);
        it->vxz10 = lerp_d(t, it->n100, it->n110);
        it->vxz01 = lerp_d(t, it->n001, it->n011);
        it->vxz11 = lerp_d(t, it->n101, it->n111);
    }
}

void hc_nc_update_for_x(hc_noise_chunk_t *nc, int32_t block_x, double t) {
    nc->cc.in_cell_x = block_x - nc->cell_start_x;
    for (int32_t k = 0; k < nc->n_interp; k++) {
        hc_df_interp_t *it = &nc->interp[k];
        it->vz0 = lerp_d(t, it->vxz00, it->vxz10);
        it->vz1 = lerp_d(t, it->vxz01, it->vxz11);
    }
}

void hc_nc_update_for_z(hc_noise_chunk_t *nc, int32_t block_z, double t) {
    nc->cc.in_cell_z = block_z - nc->cell_start_z;
    for (int32_t k = 0; k < nc->n_interp; k++) {
        hc_df_interp_t *it = &nc->interp[k];
        it->value = lerp_d(t, it->vz0, it->vz1);
    }
}

void hc_nc_swap_slices(hc_noise_chunk_t *nc) {
    for (int32_t k = 0; k < nc->n_interp; k++) {
        double *tmp = nc->interp[k].slice0;
        nc->interp[k].slice0 = nc->interp[k].slice1;
        nc->interp[k].slice1 = tmp;
    }
}

/* 콘 1개 산출 — mark 버퍼가 그대로 eval assert 용 mask 가 된다 (P2-1).
 * want_prog: lazy 브랜치 프로그램 (P2-4) 산출 여부 — 평가에 쓰이지 않는
 * 중간 콘 (slice union) 은 끈다. */
static int cone_build_ex(hc_noise_chunk_t *nc, hc_arena_t *arena,
                         hc_df_mode_t mode, const int32_t *roots,
                         int32_t n_roots, int32_t dispatch_root,
                         uint32_t flags, int want_prog, hc_nc_cone_t *out) {
    const hc_df_graph_t *g = nc->g;
    uint8_t             *mark = hc_arena_alloc(arena, (size_t)g->n, 1);
    if (!mark)
        return -1;
    memset(mark, 0, (size_t)g->n);
    int32_t len = hc_df_cone_mark_ex(g, mode, roots, n_roots, mark, flags);
    if (len < 0)
        return -1; /* FTS 가 비-SP 콘에 등장 — 콘 규칙 위반 (fail-loud) */
    int32_t *list = hc_arena_alloc(arena, sizeof(int32_t) * (size_t)len, 4);
    if (len > 0 && !list)
        return -1;
    hc_df_cone_collect(g, mark, list);
    out->root = dispatch_root;
    out->len = len;
    out->list = list;
    out->mask = mark;
    /* lazy 브랜치 프로그램 (P2-4) — 브랜치 구조가 없는 콘은 NULL 로
     * 남고 플레인 워크를 쓴다. df_cones 게이트가 두 경로 모두 판정. */
    out->prog = NULL;
    out->prog_words = 0;
    if (want_prog &&
        hc_df_cone_program(g, roots, n_roots, list, len, arena, &out->prog,
                           &out->prog_words))
        return -1;
    /* AVX2 x4 적격 (스트림 화이트리스트, P2-4) — 실사용 여부는 nc->x4
     * (런타임 디스패치) 와 호출 지점이 함께 결정한다. */
    out->x4_ok = (uint8_t)(out->prog
                               ? hc_df_stream_x4_ok(g, out->prog,
                                                    out->prog_words)
                               : hc_df_stream_x4_ok(g, list, len));
    return 0;
}

static int cone_build(hc_noise_chunk_t *nc, hc_arena_t *arena,
                      hc_df_mode_t mode, const int32_t *roots,
                      int32_t n_roots, int32_t dispatch_root,
                      hc_nc_cone_t *out) {
    return cone_build_ex(nc, arena, mode, roots, n_roots, dispatch_root, 0, 1,
                         out);
}

int hc_nc_init(hc_noise_chunk_t *nc, hc_arena_t *arena,
               const hc_df_graph_t *g, const hc_noise_roots_t *roots,
               int64_t seed, int32_t cx, int32_t cz, int32_t sea_level) {
    memset(nc, 0, sizeof *nc);
    nc->g = g;
    nc->roots = *roots;
    nc->cx = cx;
    nc->cz = cz;
    nc->sea_level = sea_level;
    nc->min_block_x = cx * 16;
    nc->min_block_z = cz * 16;
    /* 26.2 오버월드 고정 (NoiseSettings -64/384/1/2, clamp 는 항등) */
    nc->cell_width = 4;
    nc->cell_height = 8;
    nc->cell_count_xz = 16 / nc->cell_width;
    nc->cell_count_y = HC_HEIGHT / nc->cell_height; /* floorDiv(384,8)=48 */
    nc->cell_noise_min_y = HC_MIN_Y / nc->cell_height; /* floorDiv(-64,8) */
    nc->first_cell_x = nc->min_block_x >= 0
                           ? nc->min_block_x / nc->cell_width
                           : -((-nc->min_block_x + nc->cell_width - 1) /
                               nc->cell_width); /* Math.floorDiv */
    nc->first_cell_z = nc->min_block_z >= 0
                           ? nc->min_block_z / nc->cell_width
                           : -((-nc->min_block_z + nc->cell_width - 1) /
                               nc->cell_width);
    nc->first_noise_x = nc->min_block_x >> 2; /* QuartPos.fromBlock */
    nc->first_noise_z = nc->min_block_z >> 2;
    nc->noise_size_xz = (nc->cell_count_xz * nc->cell_width) >> 2; /* 4 */

    nc->scratch = hc_arena_alloc(arena, sizeof(double) * 2 * (size_t)g->n,
                                 _Alignof(double));
    nc->interp_of =
        hc_arena_alloc(arena, sizeof(int32_t) * (size_t)g->n, 4);
    nc->flat_of = hc_arena_alloc(arena, sizeof(int32_t) * (size_t)g->n, 4);
    if (!nc->scratch || !nc->interp_of || !nc->flat_of)
        return -1;

    /* AVX2 백엔드 (P2-4): cpuid 디스패치 결과를 청크당 1회 캐시.
     * SoA vscratch 는 x4 경로 전용 — 스칼라 백엔드는 할당하지 않는다. */
    nc->x4 = (hc_isa_active() == HC_ISA_AVX2);
    nc->vscratch = NULL;
    if (nc->x4) {
        nc->vscratch =
            hc_arena_alloc(arena, sizeof(double) * 4 * (size_t)g->n, 32);
        if (!nc->vscratch)
            return -1;
    }

    /* 마커 조사: 그래프 전체의 interpolated / flat_cache — 바닐라는 15슬롯
     * 전부를 한 visitor 로 mapAll 하므로 모든 마커가 상태를 얻는다. */
    nc->n_interp = 0;
    nc->n_flat = 0;
    for (int32_t i = 0; i < g->n; i++) {
        nc->interp_of[i] = -1;
        nc->flat_of[i] = -1;
        if (g->nodes[i].op == HC_DF_INTERPOLATED)
            nc->n_interp++;
        else if (g->nodes[i].op == HC_DF_FLAT_CACHE)
            nc->n_flat++;
    }
    nc->interp = hc_arena_alloc(
        arena, sizeof(hc_df_interp_t) * (size_t)nc->n_interp,
        _Alignof(hc_df_interp_t));
    nc->flat = hc_arena_alloc(arena, sizeof(hc_df_flat_t) * (size_t)nc->n_flat,
                              _Alignof(hc_df_flat_t));
    if ((nc->n_interp && !nc->interp) || (nc->n_flat && !nc->flat))
        return -1;

    int32_t slice_len =
        (nc->cell_count_xz + 1) * (nc->cell_count_y + 1); /* 5*49 */
    int32_t flat_size = nc->noise_size_xz + 1;            /* 5 */
    int32_t ii = 0, fi = 0;
    for (int32_t i = 0; i < g->n; i++) {
        if (g->nodes[i].op == HC_DF_INTERPOLATED) {
            hc_df_interp_t *it = &nc->interp[ii];
            memset(it, 0, sizeof *it);
            it->node = i;
            it->slice0 = hc_arena_alloc(
                arena, sizeof(double) * (size_t)slice_len, _Alignof(double));
            it->slice1 = hc_arena_alloc(
                arena, sizeof(double) * (size_t)slice_len, _Alignof(double));
            if (!it->slice0 || !it->slice1)
                return -1;
            memset(it->slice0, 0, sizeof(double) * (size_t)slice_len);
            memset(it->slice1, 0, sizeof(double) * (size_t)slice_len);
            nc->interp_of[i] = ii++;
        } else if (g->nodes[i].op == HC_DF_FLAT_CACHE) {
            hc_df_flat_t *fl = &nc->flat[fi];
            fl->node = i;
            fl->values = hc_arena_alloc(
                arena, sizeof(double) * (size_t)(flat_size * flat_size),
                _Alignof(double));
            if (!fl->values)
                return -1;
            nc->flat_of[i] = fi++;
        }
    }

    nc->cc.first_noise_x = nc->first_noise_x;
    nc->cc.first_noise_z = nc->first_noise_z;
    nc->cc.noise_size_xz = nc->noise_size_xz;
    nc->cc.cell_width = nc->cell_width;
    nc->cc.cell_height = nc->cell_height;
    nc->cc.interp_of = nc->interp_of;
    nc->cc.flat_of = nc->flat_of;
    nc->cc.interp = nc->interp;
    nc->cc.flat = nc->flat;

    /* --- (root×mode) 라이브 콘 산출 (P2-1) — 첫 평가 전 1회.
     * 노이즈 스테이지의 root×모드 조합은 고정이다: final_density 는
     * CELL(selectCellYZ), barrier/vein_* 은 BLOCK(블록 루프),
     * 나머지(psl/erosion/depth/floodedness/spread/lava)는 SP. */
    {
        const int32_t sp_roots[HC_NC_N_SP_CONES] = {
            roots->preliminary_surface_level, roots->erosion, roots->depth,
            roots->fluid_level_floodedness,   roots->fluid_level_spread,
            roots->lava,
        };
        const int32_t block_roots[HC_NC_N_BLOCK_CONES] = {
            roots->barrier, roots->vein_toggle, roots->vein_ridged,
            roots->vein_gap,
        };
        for (int32_t i = 0; i < HC_NC_N_SP_CONES; i++)
            if (cone_build(nc, arena, HC_DF_MODE_SP, &sp_roots[i], 1,
                           sp_roots[i], &nc->cones_sp[i]))
                return -1;
        for (int32_t i = 0; i < HC_NC_N_BLOCK_CONES; i++)
            if (cone_build(nc, arena, HC_DF_MODE_BLOCK, &block_roots[i], 1,
                           block_roots[i], &nc->cones_block[i]))
                return -1;
        if (cone_build(nc, arena, HC_DF_MODE_CELL, &roots->final_density, 1,
                       roots->final_density, &nc->cone_cell))
            return -1;

        /* fill_slice 단일-워크용 interp 자식 union (SP). 슬라이스 포인트는
         * 전부 청크 쿼트 창 안 (nc_fill_slice 주석의 증명) — flat_cache
         * 자식을 컷하고 (WINDOW_SAFE), 남은 콘을 y-불변/가변으로 분할해
         * 컬럼당 y-불변부를 1회만 평가한다 (P2-2). 분할은 값-불변:
         * y-불변 노드는 어느 y 로 평가해도 비트 동일하고 (hc_df.h 분류
         * 규칙), 분산 전파가 단조라 불변부 선평가가 위상 순서를 지킨다. */
        int32_t *slice_roots = hc_arena_alloc(
            arena, sizeof(int32_t) * (size_t)(nc->n_interp + 1), 4);
        uint8_t *yv = hc_arena_alloc(arena, (size_t)g->n, 1);
        if (!slice_roots || !yv)
            return -1;
        for (int32_t k = 0; k < nc->n_interp; k++)
            slice_roots[k] = g->nodes[nc->interp[k].node].a;
        hc_nc_cone_t slice_all;
        if (cone_build_ex(nc, arena, HC_DF_MODE_SP, slice_roots, nc->n_interp,
                          -1, HC_DF_CONE_WINDOW_SAFE, 0, &slice_all))
            return -1;
        hc_df_mark_y_variant(g, yv);
        {
            int32_t n_inv = 0;
            for (int32_t j = 0; j < slice_all.len; j++)
                n_inv += !yv[slice_all.list[j]];
            int32_t *inv = hc_arena_alloc(
                arena, sizeof(int32_t) * (size_t)(n_inv + 1), 4);
            int32_t *var = hc_arena_alloc(
                arena,
                sizeof(int32_t) * (size_t)(slice_all.len - n_inv + 1), 4);
            if (!inv || !var)
                return -1;
            int32_t ni = 0, nv = 0;
            for (int32_t j = 0; j < slice_all.len; j++) {
                int32_t node = slice_all.list[j];
                if (yv[node])
                    var[nv++] = node;
                else
                    inv[ni++] = node;
            }
            nc->cone_slice_inv =
                (hc_nc_cone_t){-1, ni, inv, slice_all.mask, NULL, 0, 0};
            nc->cone_slice_var =
                (hc_nc_cone_t){-1, nv, var, slice_all.mask, NULL, 0, 0};
            /* var 부분의 lazy 브랜치 프로그램 (P2-4). 루트 = interp 자식
             * (전부 y-가변이라 var 콘 소속). var-내부 도달성은 var 안에
             * 닫힌다 — y-분산 전파가 단조라 가변 노드에서 가변 노드로
             * 가는 읽기 경로가 불변 노드를 경유할 수 없다. */
            if (hc_df_cone_program(g, slice_roots, nc->n_interp, var, nv,
                                   arena, &nc->cone_slice_var.prog,
                                   &nc->cone_slice_var.prog_words))
                return -1;
            nc->cone_slice_var.x4_ok = (uint8_t)(
                nc->cone_slice_var.prog
                    ? hc_df_stream_x4_ok(g, nc->cone_slice_var.prog,
                                         nc->cone_slice_var.prog_words)
                    : hc_df_stream_x4_ok(g, var, nv));
        }

        /* flat_cache 테이블 구축용 자식 콘 (SP). 구축 포인트는 정의상 창
         * 격자 자신 — WINDOW_SAFE 로 상류 flat_cache 자식 컷 (하류 flat
         * 테이블은 노드 오름차순 구축이라 참조 시점에 준비돼 있다). */
        nc->cones_flat = hc_arena_alloc(
            arena, sizeof(hc_nc_cone_t) * (size_t)(nc->n_flat + 1),
            _Alignof(hc_nc_cone_t));
        if (!nc->cones_flat)
            return -1;
        for (int32_t f = 0; f < nc->n_flat; f++) {
            int32_t child = g->nodes[nc->flat[f].node].a;
            if (cone_build_ex(nc, arena, HC_DF_MODE_SP, &child, 1, child,
                              HC_DF_CONE_WINDOW_SAFE, 1, &nc->cones_flat[f]))
                return -1;
        }
    }

    /* flat_cache 테이블 구축 — 노드 인덱스 오름차순 == 바닐라 mapAll 의
     * bottom-up 생성 순서 (자식 테이블이 부모 구축 시점에 준비됨).
     * FlatCache 생성자: SinglePointContext(quart<<2, 0, quart<<2) 로
     * wrapped 를 25점 평가. */
    for (int32_t f = 0; f < nc->n_flat; f++) {
        hc_df_flat_t       *fl = &nc->flat[f];
        const hc_nc_cone_t *co = &nc->cones_flat[f];
        for (int32_t qi = 0; qi <= nc->noise_size_xz; qi++) {
            int32_t bx = (int32_t)((uint32_t)(nc->first_noise_x + qi) << 2);
            for (int32_t qj = 0; qj <= nc->noise_size_xz; qj++) {
                int32_t bz = (int32_t)((uint32_t)(nc->first_noise_z + qj) << 2);
                fl->values[qi + qj * flat_size] =
                    nc_eval_cone(nc, co, co->root, (double)bx, 0.0,
                                 (double)bz, HC_DF_MODE_SP);
            }
        }
    }

    /* psl 메모 */
    nc->psl_cap = 4096;
    nc->psl_key = hc_arena_alloc(arena, sizeof(uint64_t) * (size_t)nc->psl_cap,
                                 _Alignof(uint64_t));
    nc->psl_val =
        hc_arena_alloc(arena, sizeof(int32_t) * (size_t)nc->psl_cap, 4);
    nc->psl_used =
        hc_arena_alloc(arena, sizeof(uint8_t) * (size_t)nc->psl_cap, 1);
    if (!nc->psl_key || !nc->psl_val || !nc->psl_used)
        return -1;
    memset(nc->psl_used, 0, (size_t)nc->psl_cap);

    /* RandomState: base.forkPositional() → fromHashOf → forkPositional.
     * fromHashOf 는 팩토리 상태를 소비하지 않으므로 aquifer/ore 순서는
     * 수치에 영향이 없다 (바이트코드 확인). */
    hc_xoro_t      base, r;
    hc_xoro_fork_t f0;
    hc_xoro_init(&base, seed);
    hc_xoro_fork_positional(&base, &f0);
    hc_xoro_from_hash_of(&f0, "minecraft:aquifer", &r);
    hc_xoro_fork_positional(&r, &nc->aq.fork);
    hc_xoro_from_hash_of(&f0, "minecraft:ore", &r);
    hc_xoro_fork_positional(&r, &nc->ore_fork);

    return hc_aquifer_init(&nc->aq, arena, nc, &nc->aq.fork);
}

int32_t hc_nc_max_psl_range(hc_noise_chunk_t *nc, int32_t min_x,
                            int32_t min_z, int32_t max_x, int32_t max_z) {
    return nc_max_psl(nc, min_x, min_z, max_x, max_z);
}
