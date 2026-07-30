#include "hc_biome.h"

/* LinearCongruentialGenerator.next: seed *= seed*MULT + INC; seed += salt.
 * 전부 64-bit 랩어라운드 — uint64 로 계산한다. */
static uint64_t lcg_next(uint64_t seed, uint64_t salt) {
    seed *= seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return seed + salt;
}

/* getFiddle: floorMod(l >> 24, 1024)/1024 → [-0.45, 0.45). l>>24 은 자바
 * 산술 시프트지만 하위 10비트만 쓰므로 논리 시프트 + 마스크와 동일하다. */
static double fiddle(uint64_t l) {
    double d = (double)(int32_t)((l >> 24) & 1023) / 1024.0;
    return (d - 0.5) * 0.9;
}

/* getFiddledDistance — 합산 순서 z, y, x (부동소수점 순서 보존) */
static double fiddled_distance(uint64_t seed, int32_t x, int32_t y, int32_t z,
                               double dx, double dy, double dz) {
    uint64_t l = seed;
    l = lcg_next(l, (uint64_t)(int64_t)x);
    l = lcg_next(l, (uint64_t)(int64_t)y);
    l = lcg_next(l, (uint64_t)(int64_t)z);
    l = lcg_next(l, (uint64_t)(int64_t)x);
    l = lcg_next(l, (uint64_t)(int64_t)y);
    l = lcg_next(l, (uint64_t)(int64_t)z);
    double fx = fiddle(l);
    l = lcg_next(l, seed);
    double fy = fiddle(l);
    l = lcg_next(l, seed);
    double fz = fiddle(l);
    return (dz + fz) * (dz + fz) + (dy + fy) * (dy + fy) +
           (dx + fx) * (dx + fx);
}

void hc_biome_zoom(int64_t zoom_seed, int32_t x, int32_t y, int32_t z,
                   int32_t *qx, int32_t *qy, int32_t *qz) {
    int32_t bx = x - 2, by = y - 2, bz = z - 2;
    int32_t q0x = bx >> 2, q0y = by >> 2, q0z = bz >> 2;
    double  fx = (double)(bx & 3) / 4.0;
    double  fy = (double)(by & 3) / 4.0;
    double  fz = (double)(bz & 3) / 4.0;

    int32_t best = 0;
    double  best_dist = 1.0 / 0.0; /* +Inf — 첫 후보가 항상 이긴다 */
    for (int32_t i = 0; i < 8; i++) {
        int lo_x = (i & 4) == 0, lo_y = (i & 2) == 0, lo_z = (i & 1) == 0;
        int32_t cx = lo_x ? q0x : q0x + 1;
        int32_t cy = lo_y ? q0y : q0y + 1;
        int32_t cz = lo_z ? q0z : q0z + 1;
        double  dx = lo_x ? fx : fx - 1.0;
        double  dy = lo_y ? fy : fy - 1.0;
        double  dz = lo_z ? fz : fz - 1.0;
        double  d =
            fiddled_distance((uint64_t)zoom_seed, cx, cy, cz, dx, dy, dz);
        if (best_dist > d) { /* 엄격 비교 — 동률은 선착 후보 유지 */
            best = i;
            best_dist = d;
        }
    }
    *qx = (best & 4) == 0 ? q0x : q0x + 1;
    *qy = (best & 2) == 0 ? q0y : q0y + 1;
    *qz = (best & 1) == 0 ? q0z : q0z + 1;
}
