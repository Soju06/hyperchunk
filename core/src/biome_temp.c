#include "hc_biome.h"

#include "../include/hc_rng.h"

#include <assert.h>
#include <math.h>

/* Biome 온도 스택 — 26.2 바이트코드 (A6 노트) 1:1.
 *
 * Biome 의 세 정적 노이즈는 전부 PerlinSimplexNoise(월드 시드 무관,
 * LegacyRandomSource 고정 시드):
 *   TEMPERATURE_NOISE        seed 1234, 옥타브 {0}
 *   FROZEN_TEMPERATURE_NOISE seed 3456, 옥타브 {-2,-1,0}
 *   BIOME_INFO_NOISE         seed 2345, 옥타브 {0}
 * WorldgenRandom(LegacyRandomSource) 의 draw 스트림은 java.util.Random
 * 스트림 그대로다 (hc_lcg). 옥타브가 음수/0 뿐이라 PerlinSimplexNoise
 * ctor 의 재시드(l > 0) 분기와 consumeCount 스킵은 도달하지 않는다. */

/* --- SimplexNoise (2D 경로만 사용) --- */

typedef struct {
    double  xo, yo, zo;
    uint8_t p[256];
} hc_simplex_t;

/* GRADIENT[16][3] — % 12 로 0..11 만 도달하지만 표는 그대로 둔다 */
static const int SIMPLEX_GRAD[16][3] = {
    {1, 1, 0},  {-1, 1, 0},  {1, -1, 0}, {-1, -1, 0},
    {1, 0, 1},  {-1, 0, 1},  {1, 0, -1}, {-1, 0, -1},
    {0, 1, 1},  {0, -1, 1},  {0, 1, -1}, {0, -1, -1},
    {1, 1, 0},  {0, -1, 1},  {-1, 1, 0}, {0, -1, -1},
};

/* 소비 순서: nextDouble×3 (xo,yo,zo = *256), 이후 i=0..255 에
 * nextInt(256-i) 셔플 (ImprovedNoise 와 동일한 스왑 스크립트) */
static void simplex_init(hc_simplex_t *s, hc_lcg_t *r) {
    s->xo = hc_lcg_next_double(r) * 256.0;
    s->yo = hc_lcg_next_double(r) * 256.0;
    s->zo = hc_lcg_next_double(r) * 256.0;
    for (int i = 0; i < 256; i++)
        s->p[i] = (uint8_t)i;
    for (int i = 0; i < 256; i++) {
        int     j = hc_lcg_next_int(r, 256 - i);
        uint8_t k = s->p[i];
        s->p[i] = s->p[j + i];
        s->p[j + i] = k;
    }
}

static int simplex_p(const hc_simplex_t *s, int i) {
    return s->p[i & 255];
}

/* getCornerNoise3D: d = ((offset - x*x) - y*y) - z*z; d<0 → 0,
 * else (d*d)*(d*d)*dot — 정확한 곱 순서: d=d*d 후 d*d*dot */
static double simplex_corner(int gi, double x, double y, double z,
                             double offset) {
    double d = offset - x * x - y * y - z * z;
    if (d < 0.0)
        return 0.0;
    d = d * d;
    double dot = (double)SIMPLEX_GRAD[gi][0] * x +
                 (double)SIMPLEX_GRAD[gi][1] * y +
                 (double)SIMPLEX_GRAD[gi][2] * z;
    return d * d * dot;
}

static int mth_floor_d(double d) {
    return (int)floor(d);
}

/* SimplexNoise.getValue(x,y) — 2D, attenuation offset 0.5, 스케일 70 */
static double simplex_value2(const hc_simplex_t *s, double x, double y) {
    /* SQRT_3 는 런타임 계산 정적 — C 의 sqrt(3.0) 과 비트 동일 (IEEE
     * 정확 반올림 연산) */
    double SQRT_3 = sqrt(3.0);
    double F2 = 0.5 * (SQRT_3 - 1.0);
    double G2 = (3.0 - SQRT_3) / 6.0;
    double sk = (x + y) * F2;
    int    i = mth_floor_d(x + sk);
    int    j = mth_floor_d(y + sk);
    double t = (double)(i + j) * G2;
    double X0 = (double)i - t;
    double Y0 = (double)j - t;
    double x0 = x - X0;
    double y0 = y - Y0;
    int    i1, j1;
    if (x0 > y0) { /* 엄격 > — 동률은 (0,1) */
        i1 = 1;
        j1 = 0;
    } else {
        i1 = 0;
        j1 = 1;
    }
    double x1 = x0 - (double)i1 + G2;
    double y1 = y0 - (double)j1 + G2;
    double x2 = x0 - 1.0 + 2.0 * G2;
    double y2 = y0 - 1.0 + 2.0 * G2;
    int    ii = i & 255;
    int    jj = j & 255;
    int gi0 = simplex_p(s, ii + simplex_p(s, jj)) % 12;
    int gi1 = simplex_p(s, ii + i1 + simplex_p(s, jj + j1)) % 12;
    int gi2 = simplex_p(s, ii + 1 + simplex_p(s, jj + 1)) % 12;
    double n0 = simplex_corner(gi0, x0, y0, 0.0, 0.5);
    double n1 = simplex_corner(gi1, x1, y1, 0.0, 0.5);
    double n2 = simplex_corner(gi2, x2, y2, 0.0, 0.5);
    return 70.0 * (n0 + n1 + n2);
}

/* --- PerlinSimplexNoise (음수/0 옥타브 한정) --- */

typedef struct {
    hc_simplex_t levels[3]; /* 인덱스 0 = 옥타브 j(=0), 이후 내림차순 */
    int          count;
    double       input_factor; /* 2^j == 1.0 (j=0) */
    double       value_factor; /* 1/(2^k - 1) */
} hc_psn_t;

/* octaves = 연속 구간 {first..0} 만 지원 (Biome 의 세 노이즈가 전부
 * 이 형태다). 같은 RNG 에서 옥타브 0 → -1 → ... 순차 소비, 스킵 없음. */
static void psn_init(hc_psn_t *n, int64_t seed, int first_octave) {
    assert(first_octave <= 0 && first_octave >= -2);
    int k = -first_octave + 1;
    hc_lcg_t r;
    hc_lcg_init(&r, seed);
    for (int i = 0; i < k; i++)
        simplex_init(&n->levels[i], &r);
    n->count = k;
    n->input_factor = 1.0;                      /* 2^0 */
    n->value_factor = 1.0 / (pow(2.0, k) - 1.0); /* k=1→1.0, k=3→1/7 */
}

/* getValue(x, y, useNoiseOffsets=false) — 오프셋 미적용 (A6 §5.2) */
static double psn_value(const hc_psn_t *n, double x, double y) {
    double d = 0.0;
    double f = n->input_factor;
    double g = n->value_factor;
    for (int i = 0; i < n->count; i++) {
        d += simplex_value2(&n->levels[i], x * f, y * f) * g;
        f /= 2.0;
        g *= 2.0;
    }
    return d;
}

/* --- 정적 노이즈 지연 초기화 (Phase 1 단일 스레드) --- */

static hc_psn_t g_temp_noise, g_frozen_noise, g_info_noise;
static int      g_noises_ready = 0;

static void ensure_noises(void) {
    if (g_noises_ready)
        return;
    psn_init(&g_temp_noise, 1234, 0);
    psn_init(&g_frozen_noise, 3456, -2);
    psn_init(&g_info_noise, 2345, 0);
    g_noises_ready = 1;
}

/* --- Biome 온도 --- */

/* TemperatureModifier.FROZEN.modifyTemperature — double 경로 (i2d) */
static float modify_frozen(int32_t x, int32_t z, float t) {
    double d0 =
        psn_value(&g_frozen_noise, (double)x * 0.05, (double)z * 0.05) * 7.0;
    double d1 = psn_value(&g_info_noise, (double)x * 0.2, (double)z * 0.2);
    if (d0 + d1 < 0.3) {
        double d3 =
            psn_value(&g_info_noise, (double)x * 0.09, (double)z * 0.09);
        if (d3 < 0.8)
            return 0.2f;
    }
    return t;
}

/* getHeightAdjustedTemperature — y > seaLevel+17 분기의 x/8, z/8 은
 * float 나눗셈 후 double 승격 (i2f; fdiv 8.0f; f2d), 마지막 식은 전부
 * float 연산 순서 그대로: ((f1 + (float)y) - (float)i) * 0.05f / 40.0f */
float hc_biome_temperature(const hc_biome_reg_t *r, int32_t id, int32_t x,
                           int32_t y, int32_t z, int32_t sea_level) {
    assert(id >= 0 && id < r->count);
    float base = r->temperature[id];
    assert(base == base); /* NaN = 기후 미설정 — 로더 버그 */
    ensure_noises();
    float f = (r->temp_modifier[id] == HC_BIOME_TEMP_MOD_FROZEN)
                  ? modify_frozen(x, z, base)
                  : base;
    int i = sea_level + 17;
    if (y > i) {
        float f1 = (float)(psn_value(&g_temp_noise,
                                     (double)((float)x / 8.0f),
                                     (double)((float)z / 8.0f)) *
                           8.0);
        return f - (f1 + (float)y - (float)i) * 0.05f / 40.0f;
    }
    return f;
}

int hc_biome_cold_enough_to_snow(const hc_biome_reg_t *r, int32_t id,
                                 int32_t x, int32_t y, int32_t z,
                                 int32_t sea_level) {
    /* warmEnoughToRain = temp >= 0.15f (NaN → false); cold = !warm */
    return !(hc_biome_temperature(r, id, x, y, z, sea_level) >= 0.15f);
}

int hc_biome_should_melt_iceberg(const hc_biome_reg_t *r, int32_t id,
                                 int32_t x, int32_t y, int32_t z,
                                 int32_t sea_level) {
    return hc_biome_temperature(r, id, x, y, z, sea_level) > 0.1f;
}

double hc_biome_info_noise(double x, double z) {
    ensure_noises();
    return psn_value(&g_info_noise, x, z);
}
