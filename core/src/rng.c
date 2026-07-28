#include "hc_rng.h"

/* RED 단계 스텁 — 전 벡터가 golden 과 불일치해야 한다.
 * 실제 구현은 다음 커밋에서 채운다. */

void hc_xoro_init(hc_xoro_t *r, int64_t seed) {
    (void)seed;
    r->lo = 0;
    r->hi = 0;
}

uint64_t hc_xoro_next(hc_xoro_t *r) {
    (void)r;
    return 0;
}

int32_t hc_xoro_next_int(hc_xoro_t *r, int32_t bound) {
    (void)r;
    (void)bound;
    return -1;
}

double hc_xoro_next_double(hc_xoro_t *r) {
    (void)r;
    return -1.0;
}

void hc_lcg_init(hc_lcg_t *r, int64_t seed) {
    (void)seed;
    r->s = 0;
}

int32_t hc_lcg_next(hc_lcg_t *r, int bits) {
    (void)r;
    (void)bits;
    return -1;
}

int64_t hc_lcg_next_long(hc_lcg_t *r) {
    (void)r;
    return 0;
}

int32_t hc_lcg_next_int(hc_lcg_t *r, int32_t bound) {
    (void)r;
    (void)bound;
    return -1;
}

double hc_lcg_next_double(hc_lcg_t *r) {
    (void)r;
    return -1.0;
}
