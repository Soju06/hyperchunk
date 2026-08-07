#include "hc_noise.h"

/* 바닐라 26.2 NormalNoise (value+shift 쌍) 스칼라 재구현. 시맨틱은
 * 비난독화 NormalNoise 클래스를 javap 로 확인해 옮겼고 (ADR-002 R4),
 * golden/rng/octaves_seed*.txt 의 .valueFactor / .getValue 벡터로
 * 비트단위 검증된다 (tests/unit/test_noise.c). */

/* expectedDeviation(n) = 0.1 * (1 + 1/(n+1)) — 결합 순서 그대로.
 * n+1 도 Java iadd 래핑으로 계산한다 (C signed 오버플로 UB 회피). */
static double expected_deviation(int32_t n) {
    return 0.1 * (1.0 + 1.0 / (double)(int32_t)((uint32_t)n + 1u));
}

int hc_normal_noise_init(hc_normal_noise_t *n, hc_arena_t *a, hc_xoro_t *rand,
                         int32_t first_octave, const double *amps,
                         int32_t count) {
    /* first/second 를 같은 rand 에서 순차 초기화 — 각각 forkPositional 로
     * nextLong 2회씩, 합계 4회 소비 (ADR-002 Pitfall 2) */
    if (hc_octaves_init(&n->first, a, rand, first_octave, amps, count) != 0)
        return -1;
    if (hc_octaves_init(&n->second, a, rand, first_octave, amps, count) != 0)
        return -1;

    int32_t min_idx = INT32_MAX, max_idx = INT32_MIN;
    for (int32_t i = 0; i < count; i++) {
        if (amps[i] != 0.0) {
            min_idx = i < min_idx ? i : min_idx;
            max_idx = i > max_idx ? i : max_idx;
        }
    }
    /* 진폭이 전부 0 이면 Java 는 MIN-MAX 를 int 랩어라운드로 계산한다.
     * C signed 오버플로는 UB 라 무부호로 계산해 재해석한다. */
    int32_t range = (int32_t)((uint32_t)max_idx - (uint32_t)min_idx);
    n->value_factor = 0.16666666666666666 / expected_deviation(range);
    return 0;
}

double hc_normal_noise_value(const hc_normal_noise_t *n, double x, double y,
                             double z) {
    double x2 = x * HC_NORMAL_INPUT_FACTOR;
    double y2 = y * HC_NORMAL_INPUT_FACTOR;
    double z2 = z * HC_NORMAL_INPUT_FACTOR;
    return (hc_octaves_value(&n->first, x, y, z, 0.0, 0.0) +
            hc_octaves_value(&n->second, x2, y2, z2, 0.0, 0.0)) *
           n->value_factor;
}
