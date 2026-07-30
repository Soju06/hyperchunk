#include "hc_noise.h"

#include <math.h>
#include <stdio.h>

/* 바닐라 26.2 PerlinNoise (멀티옥타브) 스칼라 재구현. 시맨틱은 비난독화
 * PerlinNoise/Mth 클래스를 javap 로 확인해 옮겼고 (ADR-002 R4),
 * golden/rng/octaves_seed*.txt (modern) 와 router_seed*.txt 의
 * old_blended_noise (legacy) 로 비트단위 검증된다 (tests/unit/test_noise.c).
 *
 * FP 결합 순서를 바닐라와 정확히 같게 유지한다. FMA 접힘은
 * -ffp-contract=off 가 컴파일러 차원에서 막는다 (ADR-004 D3). */

/* Math.pow(2.0, n) — 정수 지수는 정확히 표현되므로 비트를 직접 조립한다.
 * libm pow 의 구현 편차를 원천 차단한다. normal 범위만 지원한다. */
static double pow2i(int32_t n) {
    union {
        uint64_t u;
        double d;
    } b;
    b.u = (uint64_t)(n + 1023) << 52;
    return b.d;
}

/* Mth.lfloor — Java d2l 은 포화 변환. C 의 범위 밖 캐스트는 UB 라
 * (UBSan float-cast-overflow 게이트 대상) 명시적으로 포화시킨다. */
static int64_t mth_lfloor(double d) {
    double f = floor(d);
    if (f != f)
        return 0;
    if (f >= 0x1p63)
        return INT64_MAX;
    if (f < -0x1p63)
        return INT64_MIN;
    return (int64_t)f;
}

double hc_octaves_wrap(double d) {
    /* PerlinNoise.wrap: d - lfloor(d/2^25 + 0.5) * 2^25 (좌표를 옥타브
     * 주파수 배율 후 2^25 격자로 되접어 정밀도 손실을 막는다) */
    return d - (double)mth_lfloor(d / 33554432.0 + 0.5) * 33554432.0;
}

static int alloc_tables(hc_octaves_t *o, hc_arena_t *a, int32_t first_octave,
                        const double *amps, int32_t count) {
    o->count = count;
    o->first_octave = first_octave;
    o->octaves = (hc_perlin_t **)hc_arena_alloc(
        a, sizeof(hc_perlin_t *) * (size_t)count, _Alignof(hc_perlin_t *));
    o->amplitudes = (double *)hc_arena_alloc(
        a, sizeof(double) * (size_t)count, _Alignof(double));
    if (!o->octaves || !o->amplitudes)
        return -1;
    for (int32_t i = 0; i < count; i++) {
        o->octaves[i] = NULL;
        o->amplitudes[i] = amps[i];
    }
    /* lowestFreqInputFactor = 2^firstOctave
     * lowestFreqValueFactor = 2^(count-1) / (2^count - 1) */
    o->lowest_freq_input = pow2i(first_octave);
    o->lowest_freq_value = pow2i(count - 1) / (pow2i(count) - 1.0);
    return 0;
}

int hc_octaves_init(hc_octaves_t *o, hc_arena_t *a, hc_xoro_t *rand,
                    int32_t first_octave, const double *amps, int32_t count) {
    if (alloc_tables(o, a, first_octave, amps, count) != 0)
        return -1;
    /* forkPositional() 이 rand 에서 nextLong 2회를 소비한다. 이후 각
     * 옥타브는 fromHashOf 로 시딩되므로 rand 를 더 소비하지 않는다 —
     * 진폭 0 옥타브는 어떤 것도 소비하지 않는다. */
    hc_xoro_fork_t f;
    hc_xoro_fork_positional(rand, &f);
    for (int32_t i = 0; i < count; i++) {
        if (amps[i] == 0.0)
            continue;
        hc_perlin_t *p =
            (hc_perlin_t *)hc_arena_alloc(a, sizeof *p, _Alignof(hc_perlin_t));
        if (!p)
            return -1;
        char key[24];
        snprintf(key, sizeof key, "octave_%d", (int)(first_octave + i));
        hc_xoro_t r;
        hc_xoro_from_hash_of(&f, key, &r);
        hc_perlin_init_from(p, &r);
        o->octaves[i] = p;
    }
    return 0;
}

/* RandomSource.consumeCount(262) — Xoroshiro 는 nextLong 262회.
 * (262 = 레거시 LCG 시절 ImprovedNoise 의 next() 소비량 3*2+256 에서 온
 * 상수이며, Xoroshiro 의 실제 ImprovedNoise 소비량과 다르지만 바닐라가
 * 그렇게 소비하므로 그대로 재현한다.) */
static void skip_octave(hc_xoro_t *rand) {
    for (int i = 0; i < 262; i++)
        (void)hc_xoro_next(rand);
}

int hc_octaves_init_legacy(hc_octaves_t *o, hc_arena_t *a, hc_xoro_t *rand,
                           int32_t first_octave, const double *amps,
                           int32_t count) {
    int32_t j = -first_octave;
    /* 바닐라는 j < count-1 (양수 옥타브) 이면 throw 한다.
     * ("Positive octaves are temporarily disabled") */
    if (j < count - 1)
        return -1;
    if (alloc_tables(o, a, first_octave, amps, count) != 0)
        return -1;

    /* 옥타브 0 (인덱스 j) 의 ImprovedNoise 는 저장 여부와 '무관하게' 항상
     * 먼저 생성된다 — 범위 밖이거나 진폭 0 이어도 RNG 는 소비된다. */
    hc_perlin_t head;
    hc_perlin_init_from(&head, rand);
    if (j >= 0 && j < count && amps[j] != 0.0) {
        hc_perlin_t *p =
            (hc_perlin_t *)hc_arena_alloc(a, sizeof *p, _Alignof(hc_perlin_t));
        if (!p)
            return -1;
        *p = head;
        o->octaves[j] = p;
    }
    /* 인덱스 j-1..0 (옥타브 -1..first_octave) 내림차순 순차 소비.
     * 진폭 0 이거나 범위 밖인 자리는 262 draw 를 건너뛴다. */
    for (int32_t k = j - 1; k >= 0; k--) {
        if (k < count && amps[k] != 0.0) {
            hc_perlin_t *p = (hc_perlin_t *)hc_arena_alloc(
                a, sizeof *p, _Alignof(hc_perlin_t));
            if (!p)
                return -1;
            hc_perlin_init_from(p, rand);
            o->octaves[k] = p;
        } else {
            skip_octave(rand);
        }
    }
    return 0;
}

double hc_octaves_value(const hc_octaves_t *o, double x, double y, double z,
                        double yscale, double ymax) {
    double d = 0.0;
    double e = o->lowest_freq_input;
    double f = o->lowest_freq_value;
    for (int32_t i = 0; i < o->count; i++) {
        const hc_perlin_t *p = o->octaves[i];
        if (p) {
            double g = hc_perlin_sample_scaled(
                p, hc_octaves_wrap(x * e), hc_octaves_wrap(y * e),
                hc_octaves_wrap(z * e), yscale * e, ymax * e);
            d += o->amplitudes[i] * g * f;
        }
        e *= 2.0;
        f /= 2.0;
    }
    return d;
}
