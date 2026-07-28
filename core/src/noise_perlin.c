#include "hc_df.h"

/* 의도적 스텁 (TDD RED) — GREEN 커밋에서 실제 구현으로 교체된다. */

void hc_perlin_init(hc_perlin_t *p, int64_t seed) {
    (void)seed;
    p->xo = p->yo = p->zo = 0.0;
    for (int i = 0; i < 256; i++)
        p->perm[i] = 0;
}

double hc_perlin_sample(const hc_perlin_t *p, double x, double y, double z) {
    (void)p;
    (void)x;
    (void)y;
    (void)z;
    return 0.0;
}
