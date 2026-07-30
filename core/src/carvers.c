#include "hc_carvers.h"

#include <assert.h>
#include <math.h>
#include <string.h>

/* 06_carvers — WorldCarver / CaveWorldCarver / CanyonWorldCarver 26.2
 * (javap) 의 1:1 재현. 근거는 .hermes/notes/task8-carvers/A2/A3/A4.
 *
 * 모든 RNG 소비는 노트의 드로우 래더 순서 그대로다. C 는 피연산자 평가
 * 순서를 보장하지 않으므로, 한 표현식에 드로우가 2개 이상이면 반드시
 * 지역 변수로 순서를 고정한다 (fprov_sample 의 trapezoid, 모멘텀 지터).
 *
 * float/double 규율: 바이트코드의 fmul/fadd/f2d/dmul 위치를 주석 없이도
 * 재현하도록 식 자체를 자바 형태로 옮겼다. FMA 금지 게이트가 회귀를
 * 잡는다 (ADR-004 D3). */

/* Java int << 4 래핑 (음수 좌시프트 UB 우회) — aquifer.c 와 동일 */
static int32_t shl32(int32_t v, int k) {
    return (int32_t)((uint32_t)v << k);
}

static int32_t imin(int32_t a, int32_t b) {
    return a < b ? a : b;
}
static int32_t imax(int32_t a, int32_t b) {
    return a > b ? a : b;
}

/* Mth.floor — surface.c/aquifer.c 와 동일 시맨틱 (d2i saturating) */
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

/* --- Mth.sin/cos(double)→float — 26.2 테이블 (A7 §4.1) ---
 *
 * 테이블: SIN[i] = (float) Math.sin((double) i / 10430.378350470453).
 * SIN_SCALE 비트 0x40C45F306DC9C883 == 65536/(2π) 의 정확 반올림.
 * glibc sin 이 65536 엔트리 전부에서 HotSpot 스텁과 비트 일치함은
 * golden/rng/mth_sin_table.txt 단위 게이트가 증명한다.
 * 인덱스: d2l (0 방향 절단, 포화) 후 & 65535 — floor 가 아니다. */

#define MTH_SIN_SCALE 10430.378350470453

static float g_sin_tab[65536];
static int   g_sin_ready = 0;

void hc_mth_trig_init(void) {
    if (g_sin_ready)
        return;
    for (int32_t i = 0; i < 65536; i++)
        g_sin_tab[i] = (float)sin((double)i / MTH_SIN_SCALE);
    g_sin_ready = 1;
}

const float *hc_mth_sin_table(void) {
    assert(g_sin_ready);
    return g_sin_tab;
}

/* d2l: |값| < 2^63 은 카버 각도 범위에서 자명 (A7 §4.2 OPEN — 포화 도달
 * 불가). debug assert 로만 방어한다. */
static int64_t d2l(double d) {
    assert(!(fabs(d) >= 9.223372036854776e18));
    return (int64_t)d;
}

float hc_mth_sin(double x) {
    assert(g_sin_ready);
    return g_sin_tab[(uint32_t)((uint64_t)d2l(x * MTH_SIN_SCALE)) & 65535u];
}

float hc_mth_cos(double x) {
    assert(g_sin_ready);
    return g_sin_tab[(uint32_t)((uint64_t)d2l(x * MTH_SIN_SCALE + 16384.0)) &
                     65535u];
}

/* --- FloatProvider.sample (A7 §5) --- */

static float fprov_sample(const hc_fprov_t *p, hc_lcg_t *r) {
    switch (p->kind) {
    case HC_FP_CONST: /* 0 드로우 */
        return p->a;
    case HC_FP_UNIFORM: /* Mth.randomBetween: nf*(max-min)+min, 전부 float */
        return hc_lcg_next_float(r) * (p->b - p->a) + p->a;
    default: { /* TRAPEZOID: min + d1*(range-g) + d2*g (A7 §5.2) */
        float range = p->b - p->a;
        float g = (range - p->c) / 2.0f;
        float h = range - g;
        float d1 = hc_lcg_next_float(r);
        float d2 = hc_lcg_next_float(r);
        return p->a + d1 * h + d2 * g;
    }
    }
}

/* UniformHeight.sample (A7 §6.1): min>max 는 경고 후 min (0 드로우) */
static int32_t height_sample(const hc_carver_t *cv, hc_lcg_t *r) {
    if (cv->y_min > cv->y_max)
        return cv->y_min;
    return hc_lcg_next_int(r, cv->y_max - cv->y_min + 1) + cv->y_min;
}

/* --- BlockColumn 접근 (surface.c 와 동일 시맨틱) --- */

static uint16_t col_get(const hc_chunk_t *c, int x, int32_t y, int z) {
    if (y < HC_MIN_Y || y > HC_MAX_Y)
        return HC_B_AIR;
    return c->states[hc_idx(x, y, z)];
}

/* ProtoChunk.setBlockState(pos, state, 3): 상태 기록 + WG 하이트맵 갱신
 * (A6 §3-5 — CARVERS 단계 status==SURFACE 라 WS_WG/OF_WG 둘만 갱신,
 * flags 는 죽은 인자). 유체 postprocess 마킹은 산출물 무영향 no-op. */
static void col_set(hc_chunk_t *c, int x, int32_t y, int z, uint16_t state) {
    if (y < HC_MIN_Y || y > HC_MAX_Y)
        return;
    c->states[hc_idx(x, y, z)] = state;
    hc_hm_update_both(c, x, y, z, state);
}

/* canReplaceBlock = state.is(config.replaceable) — 블록 단위 HolderSet */
static int replaceable_test(const hc_carver_t *cv, uint16_t state) {
    return (int)((cv->replaceable[state >> 6] >> (state & 63)) & 1u);
}

/* WorldCarver$CarveSkipChecker (A2 §1.4) */
typedef int (*skip_fn_t)(const void *env, double xd, double yd, double zd,
                         int32_t y);

/* --- getCarveState (A2 §4) — 블록 id 또는 -1 (null) ---
 *
 * lava_level 이하는 aquifer 우회 LAVA.createLegacyBlock() (lava[level=0]).
 * 그 외는 aquifer.computeSubstance(SinglePointContext(pos), 0.0) — A5 §4:
 * barrier 서브트리에 ctx-민감 래퍼가 없어 기존 BLOCK 평가 경로와 비트
 * 동일, hc_aquifer_substance 를 그대로 쓴다. 디버그 모드는 상수 false. */
static int32_t get_carve_state(hc_carve_env_t *e, int32_t wx, int32_t y,
                               int32_t wz) {
    if (y <= e->cv->lava_level)
        return HC_B_LAVA;
    return hc_aquifer_substance(&e->nc->aq, wx, y, wz, 0.0);
}

/* --- carveBlock (A2 §3) --- */

static int carve_block(hc_carve_env_t *e, int lx, int32_t y, int lz, int32_t wx,
                       int32_t wz, int *reached_surface) {
    uint16_t state = col_get(e->chunk, lx, y, lz);
    if (state == HC_B_GRASS_BLOCK || state == HC_B_MYCELIUM)
        *reached_surface = 1; /* 교체 가능 여부와 무관하게 먼저 세운다 */
    if (!replaceable_test(e->cv, state))
        return 0;
    int32_t carve_state = get_carve_state(e, wx, y, wz);
    if (carve_state < 0)
        return 0; /* aquifer null — 블록 보존 */
    col_set(e->chunk, lx, y, lz, (uint16_t)carve_state);
    /* markPosForPostProcessing(pos): no-op (A5 §6, A6 §8 — 06 산출물
     * blocks/heightmaps 에 무영향; Task 9 인계 항목) */
    if (*reached_surface && col_get(e->chunk, lx, y - 1, lz) == HC_B_DIRT) {
        /* grass 복구: 정확히 minecraft:dirt 위에서만, topMaterial 결과로 */
        int32_t top = hc_surface_top_material(
            e->surf, e->chunk, e->nc, e->view, wx, y - 1, wz,
            hc_block_is_fluid((uint16_t)carve_state));
        if (top >= 0)
            col_set(e->chunk, lx, y - 1, lz, (uint16_t)top);
    }
    return 1;
}

/* --- carveEllipsoid (A2 §2) --- */

static int carve_ellipsoid(hc_carve_env_t *e, double x, double y, double z,
                           double h_radius, double v_radius, skip_fn_t skip,
                           const void *skip_env) {
    hc_chunk_t *c = e->chunk;
    double      mid_x = (double)(shl32(c->cx, 4) + 8); /* getMiddleBlockX */
    double      mid_z = (double)(shl32(c->cz, 4) + 8);
    double      reach = 16.0 + h_radius * 2.0;
    if (fabs(x - mid_x) > reach || fabs(z - mid_z) > reach)
        return 0; /* NaN 은 dcmpl 로 '크지 않음' — C 비교도 동일 */
    int32_t min_block_x = shl32(c->cx, 4);
    int32_t min_block_z = shl32(c->cz, 4);
    int32_t min_x0 = imax(mth_floor(x - h_radius) - min_block_x - 1, 0);
    int32_t max_x0 = imin(mth_floor(x + h_radius) - min_block_x, 15);
    int32_t min_y = imax(mth_floor(y - v_radius) - 1, HC_MIN_Y + 1);
    /* isUpgrading()==false (신규 월드, A6 §9) → topPad 7 */
    int32_t max_y = imin(mth_floor(y + v_radius) + 1, HC_MIN_Y + HC_HEIGHT - 8);
    int32_t min_z0 = imax(mth_floor(z - h_radius) - min_block_z - 1, 0);
    int32_t max_z0 = imin(mth_floor(z + h_radius) - min_block_z, 15);

    int carved = 0;
    for (int32_t x0 = min_x0; x0 <= max_x0; x0++) {
        int32_t block_x = min_block_x + x0; /* chunkPos.getBlockX(x0) */
        double  dx = ((double)block_x + 0.5 - x) / h_radius;
        for (int32_t z0 = min_z0; z0 <= max_z0; z0++) {
            int32_t block_z = min_block_z + z0;
            double  dz = ((double)block_z + 0.5 - z) / h_radius; /* hR! */
            /* dcmpl+iflt: NaN 이면 컬럼을 '진행'한다 (>= 는 NaN 에서
             * false — 바이트코드 극성 그대로; 리뷰 확정, A2 §2 정정) */
            if (dx * dx + dz * dz >= 1.0)
                continue;
            int reached_surface = 0; /* MutableBoolean — 컬럼당 새로 */
            for (int32_t y0 = max_y; y0 > min_y; y0--) { /* y 내림차순 */
                double dy = ((double)y0 - 0.5 - y) / v_radius; /* -0.5! */
                if (skip(skip_env, dx, dy, dz, y0))
                    continue; /* 스킵된 블록은 마스크를 건드리지 않는다 */
                /* CarvingMask: x | z<<4 | (y-minY)<<8 (A6 §1) */
                size_t mi =
                    (size_t)(x0 | (z0 << 4) | ((y0 - HC_MIN_Y) << 8));
                if ((e->mask[mi >> 6] >> (mi & 63)) & 1u)
                    continue; /* debug off — 이미 판 자리 재방문 금지 */
                e->mask[mi >> 6] |= 1ull << (mi & 63); /* carveBlock 前 */
                carved |= carve_block(e, (int)x0, y0, (int)z0, block_x,
                                      block_z, &reached_surface);
            }
        }
    }
    return carved;
}

/* --- canReach (A2 §5) — width 합산은 FLOAT --- */

static int can_reach(const hc_chunk_t *c, double x, double z,
                     int32_t branch_index, int32_t branch_count, float width) {
    double mid_x = (double)(shl32(c->cx, 4) + 8);
    double mid_z = (double)(shl32(c->cz, 4) + 8);
    double dx = x - mid_x;
    double dz = z - mid_z;
    double remaining = (double)(branch_count - branch_index);
    double r = (double)(width + 2.0f + 16.0f); /* (w+2f)+16f 후 f2d */
    /* dcmpg: NaN → false (도달 불가 판정) — C <= 도 NaN 에서 false */
    return dx * dx + dz * dz - remaining * remaining <= r * r;
}

/* ======================= CaveWorldCarver (A3) ======================= */

/* 스킵 체커 (A3 §7): yd <= floorLevel (포함) 또는 반지름 밖 (>= 1.0) */
static int cave_skip(const void *env, double xd, double yd, double zd,
                     int32_t y) {
    (void)y;
    double floor_level = *(const double *)env;
    if (yd <= floor_level)
        return 1;
    return xd * xd + yd * yd + zd * zd >= 1.0;
}

/* getThickness (A3 §4): nf*2f + nf; 1/10 로 (nf*nf*3f+1f) 곱 */
static float cave_thickness(hc_lcg_t *rng) {
    float t1 = hc_lcg_next_float(rng);
    float t2 = hc_lcg_next_float(rng);
    float thickness = t1 * 2.0f + t2;
    if (hc_lcg_next_int(rng, 10) == 0) {
        float g1 = hc_lcg_next_float(rng);
        float g2 = hc_lcg_next_float(rng);
        thickness *= g1 * g2 * 3.0f + 1.0f;
    }
    return thickness;
}

/* createRoom (A3 §5): 0 드로우. 중심 x+1.0, y/z 그대로.
 * sin((double)HALF_PI_f) 는 상수 폴딩하지 않고 테이블 조회로 재현한다. */
static void create_room(hc_carve_env_t *e, double x, double y, double z,
                        float radius, double y_scale,
                        const double *floor_level) {
    double h_radius = 1.5 + (double)(hc_mth_sin(1.5707963705062866) * radius);
    double v_radius = h_radius * y_scale;
    carve_ellipsoid(e, x + 1.0, y, z, h_radius, v_radius, cave_skip,
                    floor_level);
}

/* createTunnel (A3 §6). 자식 두께 < 1.0f 라 재귀 깊이 최대 2. */
static void create_tunnel(hc_carve_env_t *e, int64_t tunnel_seed, double x,
                          double y, double z, double h_mult, double v_mult,
                          float thickness, float h_rot, float v_rot,
                          int32_t step, int32_t dist, double y_scale,
                          const double *floor_level) {
    hc_lcg_t rng; /* SingleThreadedRandomSource(tunnelSeed) — 동일 LCG */
    hc_lcg_init(&rng, tunnel_seed);
    int32_t split_point = hc_lcg_next_int(&rng, dist / 2) + dist / 4;
    int     steep = hc_lcg_next_int(&rng, 6) == 0;
    float   y_rota = 0.0f; /* yaw 모멘텀 */
    float   x_rota = 0.0f; /* pitch 모멘텀 */

    for (int32_t cs = step; cs < dist; cs++) {
        /* 각도는 전부 FLOAT, f2d 는 Mth.sin 호출에서만 */
        double h_radius =
            1.5 +
            (double)(hc_mth_sin((double)(3.1415927f * (float)cs /
                                         (float)dist)) *
                     thickness);
        double v_radius = h_radius * y_scale;
        float  cos_x = hc_mth_cos((double)v_rot);
        x += (double)(hc_mth_cos((double)h_rot) * cos_x);
        y += (double)hc_mth_sin((double)v_rot);
        z += (double)(hc_mth_sin((double)h_rot) * cos_x);

        /* 회전 갱신 순서 고정 (A3 §6): 이동은 갱신 前 회전을 썼다 */
        v_rot *= steep ? 0.92f : 0.7f;
        v_rot += x_rota * 0.1f;
        h_rot += y_rota * 0.1f;
        x_rota *= 0.9f;
        y_rota *= 0.75f;
        {
            float a = hc_lcg_next_float(&rng);
            float b = hc_lcg_next_float(&rng);
            float c = hc_lcg_next_float(&rng);
            x_rota += (a - b) * c * 2.0f;
        }
        {
            float a = hc_lcg_next_float(&rng);
            float b = hc_lcg_next_float(&rng);
            float c = hc_lcg_next_float(&rng);
            y_rota += (a - b) * c * 4.0f;
        }

        if (cs == split_point && thickness > 1.0f) { /* 엄격 > (fcmpl) */
            /* 자식 1 시드 → 두께 → 실행 완료 → 자식 2 시드 → 두께 → 실행 */
            int64_t seed1 = hc_lcg_next_long(&rng);
            float   th1 = hc_lcg_next_float(&rng) * 0.5f + 0.5f;
            create_tunnel(e, seed1, x, y, z, h_mult, v_mult, th1,
                          h_rot - 1.5707964f, v_rot / 3.0f, cs, dist,
                          1.0 /* 리터럴 — getYScale() 아님 */, floor_level);
            int64_t seed2 = hc_lcg_next_long(&rng);
            float   th2 = hc_lcg_next_float(&rng) * 0.5f + 0.5f;
            create_tunnel(e, seed2, x, y, z, h_mult, v_mult, th2,
                          h_rot + 1.5707964f, v_rot / 3.0f, cs, dist, 1.0,
                          floor_level);
            return; /* 부모는 분기점에서 끝 */
        }

        if (hc_lcg_next_int(&rng, 4) != 0) { /* ==0 이면 이 스텝만 스킵 */
            if (!can_reach(e->chunk, x, z, cs, dist, thickness))
                return; /* 터널 전체 중단 */
            carve_ellipsoid(e, x, y, z, h_radius * h_mult,
                            v_radius * v_mult, cave_skip, floor_level);
        }
    }
}

/* CaveWorldCarver.carve (A3 §3) — rng 는 setLargeFeatureSeed +
 * isStartChunk 를 지난 외부 WorldgenRandom 상태 */
void hc_cave_carve(hc_carve_env_t *e, hc_lcg_t *rng, int32_t scx,
                   int32_t scz) {
    const hc_carver_t *cv = e->cv;
    int32_t max_distance = 112; /* sectionToBlockCoord(getRange()*2-1) */
    /* 중첩 nextInt — 안쪽부터: a=nextInt(15), b=nextInt(a+1), n=nextInt(b+1) */
    int32_t a = hc_lcg_next_int(rng, 15); /* getCaveBound() */
    int32_t b = hc_lcg_next_int(rng, a + 1);
    int32_t cave_count = hc_lcg_next_int(rng, b + 1);

    for (int32_t cave = 0; cave < cave_count; cave++) {
        double x = (double)(shl32(scx, 4) + hc_lcg_next_int(rng, 16));
        double y = (double)height_sample(cv, rng);
        double z = (double)(shl32(scz, 4) + hc_lcg_next_int(rng, 16));
        double h_mult = (double)fprov_sample(&cv->horiz_radius_mult, rng);
        double v_mult = (double)fprov_sample(&cv->vert_radius_mult, rng);
        double floor_level = (double)fprov_sample(&cv->floor_level, rng);

        int32_t tunnels = 1;
        if (hc_lcg_next_int(rng, 4) == 0) { /* 방(room) 롤 — 1/4 */
            double y_scale = (double)fprov_sample(&cv->y_scale, rng);
            float  radius = 1.0f + hc_lcg_next_float(rng) * 6.0f;
            create_room(e, x, y, z, radius, y_scale, &floor_level);
            tunnels += hc_lcg_next_int(rng, 4);
        }

        for (int32_t i = 0; i < tunnels; i++) {
            float h_rot = hc_lcg_next_float(rng) * 6.2831855f;
            float v_rot = (hc_lcg_next_float(rng) - 0.5f) / 4.0f;
            float thickness = cave_thickness(rng);
            int32_t distance =
                max_distance - hc_lcg_next_int(rng, max_distance / 4);
            int64_t tunnel_seed = hc_lcg_next_long(rng); /* distance 後 */
            create_tunnel(e, tunnel_seed, x, y, z, h_mult, v_mult, thickness,
                          h_rot, v_rot, 0, distance,
                          1.0 /* getYScale() = 1.0D, 0 드로우 */,
                          &floor_level);
        }
    }
}

/* ====================== CanyonWorldCarver (A4) ====================== */

/* 스킵 체커 (A4 §7): (xd²+zd²)·wf²[y-minGenY-1] + yd²/6 >= 1.0 */
static int canyon_skip(const void *env, double xd, double yd, double zd,
                       int32_t y) {
    const float *wf = (const float *)env;
    return (xd * xd + zd * zd) * (double)wf[y - HC_MIN_Y - 1] +
               yd * yd / 6.0 >=
           1.0;
}

/* initWidthFactors (A4 §5): yIndex 0 은 무조건 재계산 (nextInt 없음) */
static void canyon_init_width_factors(const hc_carver_t *cv, hc_lcg_t *rng,
                                      float *wf) {
    float width_factor = 1.0f;
    for (int32_t yi = 0; yi < HC_HEIGHT; yi++) {
        if (yi == 0 || hc_lcg_next_int(rng, cv->width_smoothness) == 0) {
            float d1 = hc_lcg_next_float(rng);
            float d2 = hc_lcg_next_float(rng);
            width_factor = 1.0f + d1 * d2;
        }
        wf[yi] = width_factor * width_factor; /* 제곱 저장, 매 행 */
    }
}

/* updateVerticalRadius (A4 §6): 드로우는 무조건 1 (factor==1 이어도) */
static double canyon_update_vertical_radius(const hc_carver_t *cv,
                                            hc_lcg_t *rng, double v_radius,
                                            float distance, float cs) {
    float vm = 1.0f - fabsf(0.5f - cs / distance) * 2.0f;
    float factor = cv->vert_radius_default_factor +
                   cv->vert_radius_center_factor * vm;
    float rb = hc_lcg_next_float(rng) * (1.0f - 0.75f) + 0.75f;
    return (double)factor * v_radius * (double)rb; /* 좌결합 — dmul 순서 */
}

/* doCarve (A4 §4) — 분기/재귀 없음, 정확히 1회 */
static void canyon_do_carve(hc_carve_env_t *e, int64_t tunnel_seed, double x,
                            double y, double z, float thickness, float h_rot,
                            float v_rot, int32_t step, int32_t distance,
                            double y_scale) {
    const hc_carver_t *cv = e->cv;
    hc_lcg_t           rng;
    hc_lcg_init(&rng, tunnel_seed);
    float wf[HC_HEIGHT];
    canyon_init_width_factors(cv, &rng, wf);
    float y_rota = 0.0f; /* 수평 회전 모멘텀 */
    float x_rota = 0.0f; /* 수직 회전 모멘텀 */

    for (int32_t cs = step; cs < distance; cs++) {
        double h_radius =
            1.5 +
            (double)(hc_mth_sin((double)((float)cs * 3.1415927f /
                                         (float)distance)) *
                     thickness);
        /* (b) 수직 반경은 factor 곱 前의 h_radius 에서 (A4 §4) */
        double v_radius = h_radius * y_scale;
        h_radius =
            h_radius * (double)fprov_sample(&cv->horiz_radius_factor, &rng);
        v_radius = canyon_update_vertical_radius(cv, &rng, v_radius,
                                                 (float)distance, (float)cs);
        float cos_x = hc_mth_cos((double)v_rot);
        float sin_x = hc_mth_sin((double)v_rot);
        x += (double)(hc_mth_cos((double)h_rot) * cos_x);
        y += (double)sin_x;
        z += (double)(hc_mth_sin((double)h_rot) * cos_x);

        v_rot *= 0.7f;
        v_rot += x_rota * 0.05f;
        h_rot += y_rota * 0.05f;
        x_rota *= 0.8f;
        y_rota *= 0.5f;
        {
            float a = hc_lcg_next_float(&rng);
            float b = hc_lcg_next_float(&rng);
            float c = hc_lcg_next_float(&rng);
            x_rota += (a - b) * c * 2.0f;
        }
        {
            float a = hc_lcg_next_float(&rng);
            float b = hc_lcg_next_float(&rng);
            float c = hc_lcg_next_float(&rng);
            y_rota += (a - b) * c * 4.0f;
        }

        if (hc_lcg_next_int(&rng, 4) != 0) {
            if (!can_reach(e->chunk, x, z, cs, distance, thickness))
                return; /* 캐니언 전체 중단 */
            carve_ellipsoid(e, x, y, z, h_radius, v_radius, canyon_skip, wf);
        }
    }
}

/* JVM f2i — 포화 변환 (NaN→0, 오버플로→INT_MAX/MIN). C 캐스트는 범위
 * 밖에서 UB (리뷰 확정: verify:canyon). 바닐라 distance_factor 로는 도달
 * 불가하지만 mth_floor/d2l 과 같은 규율로 방어한다. */
static int32_t jvm_f2i(float f) {
    if (f != f)
        return 0;
    if (f >= 2147483648.0f)
        return INT32_MAX;
    if (f <= -2147483648.0f)
        return INT32_MIN;
    return (int32_t)f;
}

/* CanyonWorldCarver.carve (A4 §3) */
void hc_canyon_carve(hc_carve_env_t *e, hc_lcg_t *rng, int32_t scx,
                     int32_t scz) {
    const hc_carver_t *cv = e->cv;
    double  x = (double)(shl32(scx, 4) + hc_lcg_next_int(rng, 16));
    int32_t y = height_sample(cv, rng);
    double  z = (double)(shl32(scz, 4) + hc_lcg_next_int(rng, 16));
    float   h_rot = hc_lcg_next_float(rng) * 6.2831855f;
    float   v_rot = fprov_sample(&cv->vertical_rotation, rng);
    double  y_scale = (double)fprov_sample(&cv->y_scale, rng); /* 0 드로우 */
    float   thickness = fprov_sample(&cv->thickness, rng);     /* 2 드로우 */
    int32_t distance =
        jvm_f2i(112.0f * fprov_sample(&cv->distance_factor, rng));
    int64_t tunnel_seed = hc_lcg_next_long(rng);
    canyon_do_carve(e, tunnel_seed, x, (double)y, z, thickness, h_rot, v_rot,
                    0, distance, y_scale);
    /* carve 는 무조건 true 반환 — 호출자가 버린다 (A4 §3) */
}

/* ==================== 설정 컴파일 (JSON → hc_carver_t) ==================== */

static int json_num_field(const hc_json_t *obj, const char *key, double *out) {
    const hc_json_t *v = hc_json_get(obj, key);
    if (!v || v->kind != HC_JSON_NUM)
        return -1;
    *out = v->num;
    return 0;
}

/* FloatProvider JSON: 숫자 리터럴(Constant) 또는 {type, ...} */
static int fprov_parse(hc_fprov_t *p, const hc_json_t *j, const char **err) {
    if (!j) {
        *err = "float provider missing";
        return -1;
    }
    if (j->kind == HC_JSON_NUM) {
        p->kind = HC_FP_CONST;
        p->a = (float)j->num;
        p->b = p->c = 0.0f;
        return 0;
    }
    if (j->kind != HC_JSON_OBJ) {
        *err = "float provider not num/obj";
        return -1;
    }
    const hc_json_t *type = hc_json_get(j, "type");
    double           x, y, z;
    if (type && hc_json_streq(type, "minecraft:constant")) {
        /* FloatProviders.CODEC = either(FLOAT, dispatch) — 명시적
         * {"type":"minecraft:constant","value":v} 형태도 유효 (리뷰 확정) */
        if (json_num_field(j, "value", &x)) {
            *err = "constant provider value";
            return -1;
        }
        p->kind = HC_FP_CONST;
        p->a = (float)x;
        p->b = p->c = 0.0f;
        return 0;
    }
    if (type && hc_json_streq(type, "minecraft:uniform")) {
        if (json_num_field(j, "min_inclusive", &x) ||
            json_num_field(j, "max_exclusive", &y)) {
            *err = "uniform provider fields";
            return -1;
        }
        p->kind = HC_FP_UNIFORM;
        p->a = (float)x;
        p->b = (float)y;
        p->c = 0.0f;
        /* UniformFloat MapCodec.validate: max > min 엄격 (== 도 거부) */
        if (!(p->b > p->a)) {
            *err = "uniform provider max must exceed min";
            return -1;
        }
        return 0;
    }
    if (type && hc_json_streq(type, "minecraft:trapezoid")) {
        if (json_num_field(j, "min", &x) || json_num_field(j, "max", &y) ||
            json_num_field(j, "plateau", &z)) {
            *err = "trapezoid provider fields";
            return -1;
        }
        p->kind = HC_FP_TRAPEZOID;
        p->a = (float)x;
        p->b = (float)y;
        p->c = (float)z;
        /* TrapezoidFloat validate: max < min 거부 (== 허용), plateau 는
         * 스팬 이하 (음수는 바닐라도 통과 — 그대로 둔다) */
        if (p->b < p->a) {
            *err = "trapezoid provider max < min";
            return -1;
        }
        if (p->c > p->b - p->a) {
            *err = "trapezoid plateau exceeds span";
            return -1;
        }
        return 0;
    }
    *err = "unsupported float provider type"; /* clamped_normal 등:
        nextGaussian 미구현 — fail-loud, ADR-003 D5 fallback 영역 */
    return -1;
}

/* VerticalAnchor.resolveY (A7 §6.2) — 오버월드 minGenY -64 / genDepth 384 */
static int anchor_resolve(const hc_json_t *j, int32_t *out) {
    if (!j || j->kind != HC_JSON_OBJ)
        return -1;
    double v;
    if (json_num_field(j, "absolute", &v) == 0) {
        *out = (int32_t)v;
        return 0;
    }
    if (json_num_field(j, "above_bottom", &v) == 0) {
        *out = HC_MIN_Y + (int32_t)v;
        return 0;
    }
    if (json_num_field(j, "below_top", &v) == 0) {
        *out = HC_HEIGHT - 1 + HC_MIN_Y - (int32_t)v;
        return 0;
    }
    return -1;
}

/* 태그 확장: "#minecraft:x" 재귀. 태그 값은 블록 이름 (상태 프로퍼티
 * 없음) 이고 state.is(tag) 는 블록 단위이므로, 테이블의 캐노니컬 상태명
 * ("minecraft:deepslate[axis=y]" 등) 에서 '[' 앞부분만 대조해 해당 블록의
 * 모든 상태에 비트를 세운다. 테이블에 없는 블록은 우리 파이프라인이 생성
 * 불가 → 멤버십 무의미, 건너뛴다. */
static void tag_mark_block(uint64_t *bits, const char *name, int32_t len) {
    for (int32_t id = 0; id < HC_B_COUNT; id++) {
        const char *full = hc_block_name((uint16_t)id);
        size_t      base = strcspn(full, "[");
        if ((int32_t)base == len && memcmp(full, name, (size_t)len) == 0)
            bits[id >> 6] |= 1ull << (id & 63);
    }
}

static int tag_expand(uint64_t *bits, const char *name, int32_t len,
                      const hc_df_source_t *tags, int32_t n_tags, int depth,
                      const char **err) {
    if (depth > 4) {
        *err = "tag recursion too deep";
        return -1;
    }
    if (len > 0 && name[0] == '#') {
        const hc_json_t *tag = NULL;
        for (int32_t i = 0; i < n_tags; i++) {
            if ((int32_t)strlen(tags[i].name) == len - 1 &&
                memcmp(tags[i].name, name + 1, (size_t)(len - 1)) == 0) {
                tag = tags[i].json;
                break;
            }
        }
        if (!tag) {
            *err = "referenced block tag not loaded";
            return -1;
        }
        const hc_json_t *values = hc_json_get(tag, "values");
        if (!values || values->kind != HC_JSON_ARR) {
            *err = "tag values missing";
            return -1;
        }
        for (const hc_json_t *v = values->child; v; v = v->next) {
            if (v->kind != HC_JSON_STR) {
                *err = "tag value not string";
                return -1;
            }
            if (tag_expand(bits, v->s, v->slen, tags, n_tags, depth + 1,
                           err) != 0)
                return -1;
        }
        return 0;
    }
    tag_mark_block(bits, name, len);
    return 0;
}

int hc_carver_init(hc_carver_t *cv, const hc_json_t *carver_json,
                   const hc_df_source_t *tags, int32_t n_tags,
                   const char **err) {
    const char *e = "";
    memset(cv, 0, sizeof *cv);

    const hc_json_t *type = hc_json_get(carver_json, "type");
    const hc_json_t *cfg = hc_json_get(carver_json, "config");
    if (!type || !cfg || cfg->kind != HC_JSON_OBJ) {
        *err = "carver json missing type/config";
        return -1;
    }
    if (hc_json_streq(type, "minecraft:cave"))
        cv->kind = HC_CARVER_CAVE;
    else if (hc_json_streq(type, "minecraft:canyon"))
        cv->kind = HC_CARVER_CANYON;
    else {
        *err = "unsupported carver type";
        return -1;
    }

    double prob;
    if (json_num_field(cfg, "probability", &prob)) {
        *err = "probability missing";
        return -1;
    }
    cv->probability = (float)prob;
    /* Codec.floatRange(0,1) — 범위 밖은 바닐라도 로드 거부 (리뷰 확정) */
    if (!(cv->probability >= 0.0f && cv->probability <= 1.0f)) {
        *err = "probability out of [0,1]";
        return -1;
    }

    /* isDebugEnabled = DEBUG_CARVERS || debugSettings.isDebugMode():
     * 디버그 카버링 (mask/replaceable 우회 + 디버그 블록 치환) 은 미구현.
     * 조용한 발산 대신 fail-loud (리뷰 확정: verify:worldcarver-base). */
    const hc_json_t *dbg = hc_json_get(cfg, "debug_settings");
    if (dbg && dbg->kind == HC_JSON_OBJ) {
        const hc_json_t *mode = hc_json_get(dbg, "debug_mode");
        if (mode && mode->kind == HC_JSON_BOOL && mode->boolean) {
            *err = "debug_mode carvers unsupported";
            return -1;
        }
    }

    /* y: HeightProvider — 설정 4종 전부 minecraft:uniform (A7 §6.1) */
    const hc_json_t *yj = hc_json_get(cfg, "y");
    if (!yj || yj->kind != HC_JSON_OBJ) {
        *err = "y provider missing";
        return -1;
    }
    const hc_json_t *ytype = hc_json_get(yj, "type");
    if (!ytype || !hc_json_streq(ytype, "minecraft:uniform")) {
        *err = "y provider not uniform";
        return -1;
    }
    if (anchor_resolve(hc_json_get(yj, "min_inclusive"), &cv->y_min) ||
        anchor_resolve(hc_json_get(yj, "max_inclusive"), &cv->y_max)) {
        *err = "y anchors";
        return -1;
    }

    if (fprov_parse(&cv->y_scale, hc_json_get(cfg, "yScale"), &e)) {
        *err = e;
        return -1;
    }
    if (anchor_resolve(hc_json_get(cfg, "lava_level"), &cv->lava_level)) {
        *err = "lava_level anchor";
        return -1;
    }

    const hc_json_t *repl = hc_json_get(cfg, "replaceable");
    if (!repl || repl->kind != HC_JSON_STR) {
        *err = "replaceable not a tag string";
        return -1;
    }
    if (tag_expand(cv->replaceable, repl->s, repl->slen, tags, n_tags, 0,
                   &e)) {
        *err = e;
        return -1;
    }

    if (cv->kind == HC_CARVER_CAVE) {
        if (fprov_parse(&cv->horiz_radius_mult,
                        hc_json_get(cfg, "horizontal_radius_multiplier"),
                        &e) ||
            fprov_parse(&cv->vert_radius_mult,
                        hc_json_get(cfg, "vertical_radius_multiplier"), &e) ||
            fprov_parse(&cv->floor_level, hc_json_get(cfg, "floor_level"),
                        &e)) {
            *err = e;
            return -1;
        }
    } else {
        if (fprov_parse(&cv->vertical_rotation,
                        hc_json_get(cfg, "vertical_rotation"), &e)) {
            *err = e;
            return -1;
        }
        const hc_json_t *shape = hc_json_get(cfg, "shape");
        if (!shape || shape->kind != HC_JSON_OBJ) {
            *err = "canyon shape missing";
            return -1;
        }
        double ws, vd, vc;
        if (fprov_parse(&cv->distance_factor,
                        hc_json_get(shape, "distance_factor"), &e) ||
            fprov_parse(&cv->thickness, hc_json_get(shape, "thickness"),
                        &e) ||
            fprov_parse(&cv->horiz_radius_factor,
                        hc_json_get(shape, "horizontal_radius_factor"), &e)) {
            *err = e;
            return -1;
        }
        if (json_num_field(shape, "width_smoothness", &ws) ||
            json_num_field(shape, "vertical_radius_default_factor", &vd) ||
            json_num_field(shape, "vertical_radius_center_factor", &vc)) {
            *err = "canyon shape scalar fields";
            return -1;
        }
        cv->width_smoothness = (int32_t)ws;
        if (cv->width_smoothness < 1) { /* POSITIVE_INT 코덱 (A4 §8.2) */
            *err = "width_smoothness < 1";
            return -1;
        }
        cv->vert_radius_default_factor = (float)vd;
        cv->vert_radius_center_factor = (float)vc;
    }
    return 0;
}
