#include "hc_noise.h"

/* 바닐라 26.2 BlendedNoise (old_blended_noise) 스칼라 재구현. 시맨틱은
 * 비난독화 BlendedNoise/Mth 클래스를 javap 로 확인해 옮겼고 (ADR-002 R4),
 * golden/rng/router_seed*.txt 의 old_blended_noise 슬롯으로 비트단위
 * 검증된다 (tests/unit/test_noise.c).
 *
 * 26.2 도 1.19 계열과 동일하게 블록 좌표 단위로 계산한다 — 셀 양자화
 * 없음 (parityConfigString 의 cellWidth=4 는 1.18 포맷 호환 표기일 뿐,
 * 필드도 floorDiv 도 바이트코드에 존재하지 않는다). */

/* Mth.clampedLerp(delta, start, end) — 26.2 는 delta 가 '첫' 인자다.
 * NaN delta 는 두 비교 모두 불성립 → lerp 경로 (Java dcmpg/dcmpl 동일). */
static double clamped_lerp(double delta, double start, double end) {
    if (delta < 0.0)
        return start;
    if (delta > 1.0)
        return end;
    return start + delta * (end - start); /* Mth.lerp */
}

int hc_blended_init(hc_blended_noise_t *b, hc_arena_t *a, hc_xoro_t *rand,
                    double xz_scale, double y_scale, double xz_factor,
                    double y_factor, double smear) {
    double amps16[16], amps8[8];
    for (int i = 0; i < 16; i++)
        amps16[i] = 1.0;
    for (int i = 0; i < 8; i++)
        amps8[i] = 1.0;
    /* 소비 순서: minLimit(옥타브 -15..0) → maxLimit(-15..0) → main(-7..0),
     * 전부 같은 rand 에서 레거시 경로로 순차 시딩된다. */
    if (hc_octaves_init_legacy(&b->min_limit, a, rand, -15, amps16, 16) != 0)
        return -1;
    if (hc_octaves_init_legacy(&b->max_limit, a, rand, -15, amps16, 16) != 0)
        return -1;
    if (hc_octaves_init_legacy(&b->main_noise, a, rand, -7, amps8, 8) != 0)
        return -1;
    b->xz_mult = 684.412 * xz_scale;
    b->y_mult = 684.412 * y_scale;
    b->xz_factor = xz_factor;
    b->y_factor = y_factor;
    b->smear = smear;
    return 0;
}

/* PerlinNoise.getOctaveNoise(i) = noiseLevels[count - 1 - i] */
static const hc_perlin_t *oct_at(const hc_octaves_t *o, int32_t i) {
    return o->octaves[o->count - 1 - i];
}

double hc_blended_compute(const hc_blended_noise_t *b, int32_t x, int32_t y,
                          int32_t z) {
    double d = (double)x * b->xz_mult;
    double e = (double)y * b->y_mult;
    double f = (double)z * b->xz_mult;
    double gx = d / b->xz_factor;
    double gy = e / b->y_factor;
    double gz = f / b->xz_factor;
    double smear_y = b->y_mult * b->smear; /* yScale 인자 (스미어) */
    double smear_g = smear_y / b->y_factor;

    /* main: (n/10 + 1)/2 을 min↔max 블렌드 비율로 쓴다 */
    double n = 0.0;
    double o = 1.0;
    for (int p = 0; p < 8; p++) {
        const hc_perlin_t *oct = oct_at(&b->main_noise, p);
        if (oct)
            n += hc_perlin_sample_scaled(oct, hc_octaves_wrap(gx * o),
                                         hc_octaves_wrap(gy * o),
                                         hc_octaves_wrap(gz * o), smear_g * o,
                                         gy * o) /
                 o;
        o /= 2.0;
    }
    double q = (n / 10.0 + 1.0) / 2.0;
    /* q 가 NaN 이면 둘 다 false — Java dcmpl/dcmpg 판정과 동일 */
    int over = q >= 1.0;  /* min 측 생략 */
    int under = q <= 0.0; /* max 측 생략 */

    double lo = 0.0, hi = 0.0;
    o = 1.0;
    for (int r = 0; r < 16; r++) {
        double s = hc_octaves_wrap(d * o);
        double t = hc_octaves_wrap(e * o);
        double u = hc_octaves_wrap(f * o);
        double v = smear_y * o;
        if (!over) {
            const hc_perlin_t *oct = oct_at(&b->min_limit, r);
            if (oct)
                lo += hc_perlin_sample_scaled(oct, s, t, u, v, e * o) / o;
        }
        if (!under) {
            const hc_perlin_t *oct = oct_at(&b->max_limit, r);
            if (oct)
                hi += hc_perlin_sample_scaled(oct, s, t, u, v, e * o) / o;
        }
        o /= 2.0;
    }

    return clamped_lerp(q, lo / 512.0, hi / 512.0) / 128.0;
}
