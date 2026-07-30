#include "hc_gen_noise.h"

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
 *  - beardifier: 구조물 참조 없는 청크는 Beardifier.EMPTY 로 정확히 0.0.
 *    Ap2 ADD 의 fillArray 는 원소별 `+ 0.0` — density_cell 에 그대로
 *    반영한다 (-0.0 → +0.0 정규화까지 동일). */

static const hc_df_cellctx_t *nc_cc(hc_noise_chunk_t *nc, hc_df_mode_t mode) {
    nc->cc.mode = mode;
    return &nc->cc;
}

static double nc_eval(hc_noise_chunk_t *nc, int32_t root, double x, double y,
                      double z, hc_df_mode_t mode) {
    hc_df_graph_t g = *nc->g; /* root 만 바꾼 얕은 사본 */
    g.root = root;
    return hc_df_eval_ex(&g, x, y, z, nc->scratch, nc_cc(nc, mode));
}

double hc_nc_eval_sp(hc_noise_chunk_t *nc, int32_t root, int32_t x, int32_t y,
                     int32_t z) {
    return nc_eval(nc, root, (double)x, (double)y, (double)z, HC_DF_MODE_SP);
}

double hc_nc_eval_block(hc_noise_chunk_t *nc, int32_t root, int32_t x,
                        int32_t y, int32_t z) {
    return nc_eval(nc, root, (double)x, (double)y, (double)z,
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
    /* QuartPos.toBlock(QuartPos.fromBlock(v)) = (v >> 2) << 2 */
    int32_t qx = (x >> 2) << 2;
    int32_t qz = (z >> 2) << 2;
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
     * 테이블 히트다 — SP 모드 평가가 바닐라와 좌표 단위로 일치한다. */
    nc->cell_start_x = abs_cell_x * nc->cell_width;
    int32_t stride = nc->cell_count_y + 1;
    for (int32_t j = 0; j <= nc->cell_count_xz; j++) {
        int32_t bz = (nc->first_cell_z + j) * nc->cell_width;
        for (int32_t k = 0; k < nc->n_interp; k++) {
            hc_df_interp_t *it = &nc->interp[k];
            double *slice = which == 0 ? it->slice0 : it->slice1;
            int32_t child = nc->g->nodes[it->node].a;
            for (int32_t i = 0; i <= nc->cell_count_y; i++) {
                int32_t by = (i + nc->cell_noise_min_y) * nc->cell_height;
                slice[j * stride + i] =
                    nc_eval(nc, child, (double)nc->cell_start_x, (double)by,
                            (double)bz, HC_DF_MODE_SP);
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
    for (int32_t iy = nc->cell_height - 1; iy >= 0; iy--)
        for (int32_t ix = 0; ix < nc->cell_width; ix++)
            for (int32_t iz = 0; iz < nc->cell_width; iz++) {
                nc->cc.in_cell_x = ix;
                nc->cc.in_cell_y = iy;
                nc->cc.in_cell_z = iz;
                double d = nc_eval(nc, nc->roots.final_density,
                                   (double)(nc->cell_start_x + ix),
                                   (double)(nc->cell_start_y + iy),
                                   (double)(nc->cell_start_z + iz),
                                   HC_DF_MODE_CELL);
                /* + beardifier(0.0): Ap2 ADD fillArray 의 원소별 덧셈 */
                nc->density_cell[ai++] = d + 0.0;
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

    /* flat_cache 테이블 구축 — 노드 인덱스 오름차순 == 바닐라 mapAll 의
     * bottom-up 생성 순서 (자식 테이블이 부모 구축 시점에 준비됨).
     * FlatCache 생성자: SinglePointContext(quart<<2, 0, quart<<2) 로
     * wrapped 를 25점 평가. */
    for (int32_t f = 0; f < nc->n_flat; f++) {
        hc_df_flat_t *fl = &nc->flat[f];
        int32_t       child = g->nodes[fl->node].a;
        for (int32_t qi = 0; qi <= nc->noise_size_xz; qi++) {
            int32_t bx = (nc->first_noise_x + qi) << 2; /* QuartPos.toBlock */
            for (int32_t qj = 0; qj <= nc->noise_size_xz; qj++) {
                int32_t bz = (nc->first_noise_z + qj) << 2;
                fl->values[qi + qj * flat_size] = nc_eval(
                    nc, child, (double)bx, 0.0, (double)bz, HC_DF_MODE_SP);
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
