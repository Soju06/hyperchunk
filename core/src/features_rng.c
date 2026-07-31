#include "hc_features.h"

/* WorldgenRandom over XoroshiroRandomSource — 26.2 바이트코드 1:1
 * (task9pre A3 §1-2, §4). 핵심: 래퍼의 next(bits) 는 델리게이트
 * nextLong 의 상위 bits 비트 하나, nextLong 은 next(32) 두 번 (부호확장
 * 덧셈), nextInt 는 BitRandomSource 기본 (pow2 빠른 경로 / next(31) 거절
 * 루프). XoroshiroRandomSource 고유의 nextInt 경로는 이 스테이지에서
 * 도달 불가 (A3 §4.4). 검증: tests/unit/test_features_rng.c 의 A3 §6
 * 벡터. */

void hc_wgr_set_seed(hc_wgr_t *r, int64_t seed) {
    hc_xoro_init(&r->x, seed);
}

int32_t hc_wgr_next(hc_wgr_t *r, int bits) {
    return (int32_t)(hc_xoro_next(&r->x) >> (64 - bits));
}

int64_t hc_wgr_next_long(hc_wgr_t *r) {
    /* hi 먼저, lo 는 부호확장 후 덧셈 (A3 §1.4). 덧셈은 mod 2^64 랩 —
     * signed UB 회피를 위해 무부호로. */
    int32_t hi = hc_wgr_next(r, 32);
    int32_t lo = hc_wgr_next(r, 32);
    return (int64_t)(((uint64_t)(int64_t)hi << 32) + (uint64_t)(int64_t)lo);
}

int32_t hc_wgr_next_int(hc_wgr_t *r, int32_t bound) {
    if ((bound & (bound - 1)) == 0) /* pow2 (bound=1 포함) — 1 드로우 */
        return (int32_t)((uint64_t)((int64_t)bound *
                                    (int64_t)hc_wgr_next(r, 31)) >>
                         31);
    int32_t b, v;
    do {
        b = hc_wgr_next(r, 31);
        v = b % bound;
    } while ((int32_t)((uint32_t)b - (uint32_t)v + (uint32_t)(bound - 1)) <
             0); /* Java 의 int32 랩 오버플로 거절 — 무부호로 계산 */
    return v;
}

float hc_wgr_next_float(hc_wgr_t *r) {
    return (float)hc_wgr_next(r, 24) * 5.9604645e-8f;
}

double hc_wgr_next_double(hc_wgr_t *r) {
    int64_t hi = (int64_t)hc_wgr_next(r, 26) << 27;
    int64_t lo = (int64_t)hc_wgr_next(r, 27);
    return (double)(hi + lo) * 1.1102230246251565e-16;
}

int64_t hc_wgr_set_decoration_seed(hc_wgr_t *r, int64_t level_seed,
                                   int32_t min_block_x, int32_t min_block_z) {
    hc_wgr_set_seed(r, level_seed);
    int64_t  a = hc_wgr_next_long(r) | 1;
    int64_t  b = hc_wgr_next_long(r) | 1;
    uint64_t k = ((uint64_t)(int64_t)min_block_x * (uint64_t)a +
                  (uint64_t)(int64_t)min_block_z * (uint64_t)b) ^
                 (uint64_t)level_seed;
    hc_wgr_set_seed(r, (int64_t)k);
    return (int64_t)k;
}

void hc_wgr_set_feature_seed(hc_wgr_t *r, int64_t deco_seed, int32_t index,
                             int32_t step) {
    /* 10000*step 은 32-bit imul 후 i2l (A3 §2.2) */
    int32_t salt = (int32_t)((uint32_t)10000 * (uint32_t)step);
    uint64_t s = (uint64_t)deco_seed + (uint64_t)(int64_t)index +
                 (uint64_t)(int64_t)salt;
    hc_wgr_set_seed(r, (int64_t)s);
}

int32_t hc_mth_random_between_inclusive(hc_wgr_t *r, int32_t lo, int32_t hi) {
    return hc_wgr_next_int(r, hi - lo + 1) + lo; /* 항상 드로우 */
}

int32_t hc_mth_next_int_range(hc_wgr_t *r, int32_t lo, int32_t hi) {
    if (lo >= hi)
        return lo; /* 드로우 0 — randomBetweenInclusive 와 다르다 */
    return hc_wgr_next_int(r, hi - lo + 1) + lo;
}
