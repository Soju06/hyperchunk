#include "hc_gen_noise.h"

#include <assert.h>
#include <math.h>
#include <string.h>

/* Aquifer$NoiseBasedAquifer 26.2 (javap) — 상수/제어 흐름/RNG 소비 순서를
 * 바이트코드 그대로 재현한다.
 *
 * 그리드: X/Z 16칸 (>>4), Y 12칸 (floorDiv). 셀당 소스 위치는
 * aquiferRandom.at(gx,gy,gz) 에서 nextInt(10), nextInt(9), nextInt(10)
 * 순서로 뽑는다. 12셀 스캔 (dx 0..1, dy -1..1, dz 0..2 순) 에서 가장
 * 가까운 4개를 유지하고 (동률은 >= 삽입), 압력 혼합으로 stone/유체를
 * 판정한다. */

#define WAY_BELOW_MIN_Y (-32512) /* DimensionType.MIN_Y(-2032) << 4 */

/* Java int << k 는 래핑으로 정의 — C 의 음수 좌시프트 UB 를 무부호 우회 */
static int32_t shl32(int32_t v, int k) {
    return (int32_t)((uint32_t)v << k);
}

static int32_t grid_x(int32_t v) {
    return v >> 4;
}
static int32_t grid_z(int32_t v) {
    return v >> 4;
}

/* Math.floorDiv(a, b), b > 0 */
static int32_t floor_div(int32_t a, int32_t b) {
    int32_t q = a / b;
    if (a % b != 0 && (a ^ b) < 0)
        q--;
    return q;
}

static int32_t grid_y(int32_t v) {
    return floor_div(v, 12);
}

/* similarity(a, b) = 1.0 - (double)(b - a) / 25.0 — int 뺄셈 후 변환 */
static double similarity(int32_t a, int32_t b) {
    return 1.0 - (double)(b - a) / 25.0;
}

/* FLOWING_UPDATE_SIMULARITY = similarity(10^2, 12^2) (sic — 바닐라 오탈자) */
static double flowing_update_similarity(void) {
    return similarity(100, 144);
}

/* BlockPos.asLong: x 26비트 << 38 | z 26비트 << 12 | y 12비트 */
static uint64_t bp_pack(int32_t x, int32_t y, int32_t z) {
    return (((uint64_t)x & 0x3FFFFFFu) << 38) | ((uint64_t)y & 0xFFFu) |
           (((uint64_t)z & 0x3FFFFFFu) << 12);
}
static int32_t bp_x(uint64_t l) {
    return (int32_t)((int64_t)l >> 38);
}
static int32_t bp_y(uint64_t l) {
    return (int32_t)((int64_t)(l << 52) >> 52);
}
static int32_t bp_z(uint64_t l) {
    return (int32_t)((int64_t)(l << 26) >> 38);
}

/* Mth.floor — noise_chunk.c 와 동일 시맨틱 (d2i saturating) */
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

/* Mth.clamp(double): v < lo ? lo : Math.min(v, hi) */
static double mth_clamp(double v, double lo, double hi) {
    if (v < lo)
        return lo;
    return v <= hi ? v : hi; /* 실수 범위라 ±0/NaN 규칙 무관 */
}

/* Mth.map = lerp(inverseLerp(v,a,b), c, d) — 비클램프 */
static double mth_map(double v, double a, double b, double c, double d) {
    double t = (v - a) / (b - a);
    return c + t * (d - c);
}

/* Mth.clampedMap = clampedLerp(c, d, inverseLerp(v,a,b)) */
static double mth_clamped_map(double v, double a, double b, double c,
                              double d) {
    double t = (v - a) / (b - a);
    if (t < 0.0)
        return c;
    if (t > 1.0)
        return d;
    return c + t * (d - c);
}

/* 글로벌 FluidPicker (NoiseBasedChunkGenerator.createFluidPicker):
 * y < min(-54, seaLevel) → (-54, lava), 아니면 (seaLevel, water) */
static hc_fluid_status_t global_picker(const hc_aquifer_t *aq, int32_t y) {
    int32_t lava_lim =
        aq->nc->sea_level < -54 ? aq->nc->sea_level : -54; /* Math.min */
    if (y < lava_lim)
        return (hc_fluid_status_t){-54, HC_B_LAVA};
    return (hc_fluid_status_t){aq->nc->sea_level, HC_B_WATER};
}

/* FluidStatus.at(y): y < fluidLevel ? fluidType : AIR */
static int fs_at(hc_fluid_status_t fs, int32_t y) {
    return y < fs.level ? fs.type : HC_B_AIR;
}

static int fs_eq(hc_fluid_status_t a, hc_fluid_status_t b) {
    return a.level == b.level && a.type == b.type;
}

/* SURFACE_SAMPLING_OFFSETS_IN_CHUNKS — <clinit> 순서 그대로, {0,0} 이 첫째 */
static const int8_t SURF_OFFS[13][2] = {
    {0, 0},  {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {-3, 0}, {-2, 0},
    {-1, 0}, {1, 0},   {-2, 1},  {-1, 1}, {0, 1},  {1, 1},
};

/* --- computeFluid 계층 --- */

static int32_t compute_surface_level(hc_aquifer_t *aq, int32_t x, int32_t y,
                                     int32_t z, hc_fluid_status_t def,
                                     int32_t min_prelim, int center_fluid);
static uint8_t compute_fluid_type(hc_aquifer_t *aq, int32_t x, int32_t y,
                                  int32_t z, hc_fluid_status_t def,
                                  int32_t level);

static hc_fluid_status_t compute_fluid(hc_aquifer_t *aq, int32_t x, int32_t y,
                                       int32_t z) {
    hc_fluid_status_t def = global_picker(aq, y);
    int32_t           min_prelim = INT32_MAX;
    int32_t           y_p12 = y + 12;
    int32_t           y_m12 = y - 12;
    int               center_fluid = 0;

    for (int i = 0; i < 13; i++) {
        int32_t sx = x + shl32(SURF_OFFS[i][0], 4);
        int32_t sz = z + shl32(SURF_OFFS[i][1], 4);
        int32_t prelim = hc_nc_psl(aq->nc, sx, sz);
        int32_t adjusted = prelim + 8; /* adjustSurfaceLevel */
        int     is_center = SURF_OFFS[i][0] == 0 && SURF_OFFS[i][1] == 0;
        if (is_center && y_m12 > adjusted)
            return def;
        int near_surface = y_p12 > adjusted;
        if (near_surface || is_center) {
            hc_fluid_status_t fs = global_picker(aq, adjusted);
            if (fs_at(fs, adjusted) != HC_B_AIR) {
                if (is_center)
                    center_fluid = 1;
                if (near_surface)
                    return fs;
            }
        }
        if (prelim < min_prelim) /* Math.min, 비조정 prelim */
            min_prelim = prelim;
    }

    int32_t level =
        compute_surface_level(aq, x, y, z, def, min_prelim, center_fluid);
    return (hc_fluid_status_t){level,
                               compute_fluid_type(aq, x, y, z, def, level)};
}

static int32_t compute_surface_level(hc_aquifer_t *aq, int32_t x, int32_t y,
                                     int32_t z, hc_fluid_status_t def,
                                     int32_t min_prelim, int center_fluid) {
    hc_noise_chunk_t *nc = aq->nc;
    double            d1, d2;
    /* OverworldBiomeBuilder.isDeepDarkRegion: erosion 먼저, 단락 평가.
     * 상수는 (double)(float)-0.225 / (double)(float)0.9 */
    double erosion = hc_nc_eval_sp(nc, nc->roots.erosion, x, y, z);
    if (erosion < -0.22499999403953552 &&
        hc_nc_eval_sp(nc, nc->roots.depth, x, y, z) > 0.8999999761581421) {
        d1 = -1.0;
        d2 = -1.0;
    } else {
        int32_t dist = min_prelim + 8 - y;
        double  dampener =
            center_fluid ? mth_clamped_map((double)dist, 0.0, 64.0, 1.0, 0.0)
                          : 0.0;
        double flood = mth_clamp(
            hc_nc_eval_sp(nc, nc->roots.fluid_level_floodedness, x, y, z),
            -1.0, 1.0);
        double hi = mth_map(dampener, 1.0, 0.0, -0.3, 0.8);
        double lo = mth_map(dampener, 1.0, 0.0, -0.8, 0.4);
        d1 = flood - lo;
        d2 = flood - hi;
    }
    if (d2 > 0.0)
        return def.level;
    if (d1 > 0.0) {
        /* computeRandomizedFluidSurfaceLevel: 16/40/16 그리드 */
        int32_t gx = floor_div(x, 16);
        int32_t gy = floor_div(y, 40);
        int32_t gz = floor_div(z, 16);
        int32_t base = gy * 40 + 20;
        double  spread =
            hc_nc_eval_sp(nc, nc->roots.fluid_level_spread, gx, gy, gz) *
            10.0;
        int32_t quant = mth_floor(spread / 3.0) * 3; /* Mth.quantize */
        int32_t level = base + quant;
        return min_prelim < level ? min_prelim : level; /* Math.min */
    }
    return WAY_BELOW_MIN_Y;
}

static uint8_t compute_fluid_type(hc_aquifer_t *aq, int32_t x, int32_t y,
                                  int32_t z, hc_fluid_status_t def,
                                  int32_t level) {
    hc_noise_chunk_t *nc = aq->nc;
    uint8_t           type = def.type;
    if (level <= -10 && level != WAY_BELOW_MIN_Y && def.type != HC_B_LAVA) {
        int32_t gx = floor_div(x, 64);
        int32_t gy = floor_div(y, 40);
        int32_t gz = floor_div(z, 64);
        double  lava = hc_nc_eval_sp(nc, nc->roots.lava, gx, gy, gz);
        if (fabs(lava) > 0.3)
            type = HC_B_LAVA;
    }
    return type;
}

/* getIndex(gx, gy, gz) */
static int32_t cell_index(const hc_aquifer_t *aq, int32_t gx, int32_t gy,
                          int32_t gz) {
    int32_t i = gx - aq->min_grid_x;
    int32_t j = gy - aq->min_grid_y;
    int32_t k = gz - aq->min_grid_z;
    int32_t idx = (j * aq->grid_size_z + k) * aq->grid_size_x + i;
    assert(idx >= 0 && idx < aq->n_cells);
    return idx;
}

/* getAquiferStatus — 지연 계산 + 캐시 */
static hc_fluid_status_t get_status(hc_aquifer_t *aq, int32_t idx) {
    if (aq->fs_set[idx])
        return aq->fs_cache[idx];
    uint64_t loc = aq->loc_cache[idx]; /* 스캔 루프가 항상 먼저 채운다 */
    assert(loc != UINT64_MAX);
    hc_fluid_status_t s = compute_fluid(aq, bp_x(loc), bp_y(loc), bp_z(loc));
    aq->fs_cache[idx] = s;
    aq->fs_set[idx] = 1;
    return s;
}

/* calculatePressure — barrier 는 NaN 센티널 1회 메모.
 * barrier 노이즈는 블록 ctx(NoiseChunk) 로 평가된다 → BLOCK 모드. */
static double calc_pressure(hc_aquifer_t *aq, int32_t x, int32_t y, int32_t z,
                            double *barrier, hc_fluid_status_t f1,
                            hc_fluid_status_t f2) {
    int a = fs_at(f1, y);
    int b = fs_at(f2, y);
    if ((a == HC_B_LAVA && b == HC_B_WATER) ||
        (a == HC_B_WATER && b == HC_B_LAVA))
        return 2.0;

    int32_t level_diff = f1.level - f2.level;
    if (level_diff < 0)
        level_diff = -level_diff; /* Math.abs(int) */
    if (level_diff == 0)
        return 0.0;

    double avg = 0.5 * (double)(f1.level + f2.level); /* int 합 후 i2d */
    double y_delta = (double)y + 0.5 - avg;
    double half = (double)level_diff / 2.0;
    double t = half - fabs(y_delta);

    double q;
    if (y_delta > 0.0) {
        double p = 0.0 + t;
        q = p > 0.0 ? p / 1.5 : p / 2.5;
    } else {
        double p = 3.0 + t;
        q = p > 0.0 ? p / 3.0 : p / 10.0;
    }

    double bv;
    if (q < -2.0 || q > 2.0) {
        bv = 0.0;
    } else {
        if (*barrier != *barrier) /* Double.isNaN */
            *barrier =
                hc_nc_eval_block(aq->nc, aq->nc->roots.barrier, x, y, z);
        bv = *barrier;
    }
    return 2.0 * (bv + q);
}

int hc_aquifer_substance(hc_aquifer_t *aq, int32_t x, int32_t y, int32_t z,
                         double density) {
    if (density > 0.0) {
        aq->should_schedule_fluid_update = 0;
        return -1;
    }
    hc_fluid_status_t global = global_picker(aq, y);
    if (y > aq->skip_sampling_above_y) {
        aq->should_schedule_fluid_update = 0;
        return fs_at(global, y);
    }
    if (fs_at(global, y) == HC_B_LAVA) {
        aq->should_schedule_fluid_update = 0;
        return HC_B_LAVA;
    }

    /* 12셀 스캔 — SAMPLE_OFFSET (-5, +1, -5), 루프 x 0..1 / y -1..1 / z 0..1 */
    int32_t base_gx = grid_x(x - 5);
    int32_t base_gy = grid_y(y + 1);
    int32_t base_gz = grid_z(z - 5);

    int32_t dist1 = INT32_MAX, dist2 = INT32_MAX, dist3 = INT32_MAX,
            dist4 = INT32_MAX;
    int32_t idx1 = 0, idx2 = 0, idx3 = 0, idx4 = 0;

    for (int32_t dx = 0; dx <= 1; dx++) {
        for (int32_t dy = -1; dy <= 1; dy++) {
            for (int32_t dz = 0; dz <= 1; dz++) {
                int32_t  gx = base_gx + dx;
                int32_t  gy = base_gy + dy;
                int32_t  gz = base_gz + dz;
                int32_t  idx = cell_index(aq, gx, gy, gz);
                uint64_t loc = aq->loc_cache[idx];
                if (loc == UINT64_MAX) {
                    hc_xoro_t r;
                    hc_xoro_at(&aq->fork, gx, gy, gz, &r);
                    /* 소비 순서: nextInt(10) → nextInt(9) → nextInt(10) */
                    int32_t ox = hc_xoro_next_int(&r, 10);
                    int32_t oy = hc_xoro_next_int(&r, 9);
                    int32_t oz = hc_xoro_next_int(&r, 10);
                    loc = bp_pack(shl32(gx, 4) + ox, gy * 12 + oy,
                                  shl32(gz, 4) + oz);
                    aq->loc_cache[idx] = loc;
                }
                int32_t ddx = bp_x(loc) - x;
                int32_t ddy = bp_y(loc) - y;
                int32_t ddz = bp_z(loc) - z;
                int32_t d = ddx * ddx + ddy * ddy + ddz * ddz;
                /* 4-최근접 유지, 동률(>=)은 앞에 삽입 */
                if (dist1 >= d) {
                    idx4 = idx3;
                    idx3 = idx2;
                    idx2 = idx1;
                    idx1 = idx;
                    dist4 = dist3;
                    dist3 = dist2;
                    dist2 = dist1;
                    dist1 = d;
                } else if (dist2 >= d) {
                    idx4 = idx3;
                    idx3 = idx2;
                    idx2 = idx;
                    dist4 = dist3;
                    dist3 = dist2;
                    dist2 = d;
                } else if (dist3 >= d) {
                    idx4 = idx3;
                    idx3 = idx;
                    dist4 = dist3;
                    dist3 = d;
                } else if (dist4 >= d) {
                    idx4 = idx;
                    dist4 = d;
                }
            }
        }
    }

    hc_fluid_status_t fs1 = get_status(aq, idx1);
    double            sim12 = similarity(dist1, dist2);
    int               bs = fs_at(fs1, y);
    const double      FLOWING = flowing_update_similarity();

    if (sim12 <= 0.0) {
        if (sim12 >= FLOWING) {
            hc_fluid_status_t fs2 = get_status(aq, idx2);
            aq->should_schedule_fluid_update = !fs_eq(fs1, fs2);
        } else {
            aq->should_schedule_fluid_update = 0;
        }
        return bs;
    }

    if (bs == HC_B_WATER &&
        fs_at(global_picker(aq, y - 1), y - 1) == HC_B_LAVA) {
        aq->should_schedule_fluid_update = 1;
        return bs;
    }

    double            barrier = NAN; /* MutableDouble(NaN) 메모 */
    hc_fluid_status_t fs2 = get_status(aq, idx2);

    double p12 = sim12 * calc_pressure(aq, x, y, z, &barrier, fs1, fs2);
    if (density + p12 > 0.0) {
        aq->should_schedule_fluid_update = 0;
        return -1;
    }

    hc_fluid_status_t fs3 = get_status(aq, idx3);
    double            sim13 = similarity(dist1, dist3);
    if (sim13 > 0.0) {
        double p13 =
            sim12 * sim13 * calc_pressure(aq, x, y, z, &barrier, fs1, fs3);
        if (density + p13 > 0.0) {
            aq->should_schedule_fluid_update = 0;
            return -1;
        }
    }
    double sim23 = similarity(dist2, dist3);
    if (sim23 > 0.0) {
        double p23 =
            sim12 * sim23 * calc_pressure(aq, x, y, z, &barrier, fs2, fs3);
        if (density + p23 > 0.0) {
            aq->should_schedule_fluid_update = 0;
            return -1;
        }
    }

    int b1 = !fs_eq(fs1, fs2);
    int b2 = sim23 >= FLOWING && !fs_eq(fs2, fs3);
    int b3 = sim13 >= FLOWING && !fs_eq(fs1, fs3);
    if (b1 || b2 || b3) {
        aq->should_schedule_fluid_update = 1;
    } else {
        aq->should_schedule_fluid_update =
            sim13 >= FLOWING && similarity(dist1, dist4) >= FLOWING &&
            !fs_eq(fs1, get_status(aq, idx4));
    }
    return bs;
}

int hc_aquifer_init(hc_aquifer_t *aq, hc_arena_t *arena,
                    hc_noise_chunk_t *nc, const hc_xoro_fork_t *fork) {
    aq->nc = nc;
    aq->fork = *fork;
    aq->should_schedule_fluid_update = 0;

    int32_t min_bx = nc->min_block_x, min_bz = nc->min_block_z;
    int32_t max_bx = min_bx + 15, max_bz = min_bz + 15;
    aq->min_grid_x = grid_x(min_bx - 5);
    int32_t max_grid_x = grid_x(max_bx - 5) + 1;
    aq->grid_size_x = max_grid_x - aq->min_grid_x + 1;
    aq->min_grid_y = grid_y(HC_MIN_Y + 1) - 1;
    int32_t max_grid_y = grid_y(HC_MIN_Y + HC_HEIGHT + 1) + 1;
    int32_t grid_size_y = max_grid_y - aq->min_grid_y + 1;
    aq->min_grid_z = grid_z(min_bz - 5);
    int32_t max_grid_z = grid_z(max_bz - 5) + 1;
    aq->grid_size_z = max_grid_z - aq->min_grid_z + 1;
    aq->n_cells = aq->grid_size_x * grid_size_y * aq->grid_size_z;

    aq->loc_cache = hc_arena_alloc(
        arena, sizeof(uint64_t) * (size_t)aq->n_cells, _Alignof(uint64_t));
    aq->fs_cache = hc_arena_alloc(
        arena, sizeof(hc_fluid_status_t) * (size_t)aq->n_cells,
        _Alignof(hc_fluid_status_t));
    aq->fs_set =
        hc_arena_alloc(arena, sizeof(uint8_t) * (size_t)aq->n_cells, 1);
    if (!aq->loc_cache || !aq->fs_cache || !aq->fs_set)
        return -1;
    for (int32_t i = 0; i < aq->n_cells; i++)
        aq->loc_cache[i] = UINT64_MAX; /* Long.MAX_VALUE 마커와 동형 */
    memset(aq->fs_set, 0, (size_t)aq->n_cells);

    /* skipSamplingAboveY: maxPsl(그리드 경계, +9 오프셋) + 8, +12 → 그리드
     * 상단 - 1 (생성자에서 즉시 계산 — psl 평가는 순수라 순서 무관) */
    int32_t surf = hc_nc_max_psl_range(nc, shl32(aq->min_grid_x, 4),
                                       shl32(aq->min_grid_z, 4),
                                       shl32(max_grid_x, 4) + 9,
                                       shl32(max_grid_z, 4) + 9) +
                   8;
    int32_t g = grid_y(surf + 12) + 1;
    aq->skip_sampling_above_y = (g * 12 + 11) - 1; /* fromGridY(g,11) - 1 */
    return 0;
}
