#include "hc_beard.h"

#include "beard_kernel.h"

#include <math.h>
#include <string.h>

/* Beardifier 26.2 — compute 경로 포트. 모든 산술은 바이트코드 순서 그대로
 * (double, FMA 금지 빌드). 커널은 beard_kernel.h 의 실서버 비트 고정. */

/* Mth.fastInvSqrt(double) — javap: 0.5*v; bits = 6910469410427058090L -
 * (bits>>1); v' = longBitsToDouble; v' * (1.5 - d0*v'*v') */
static double mth_fast_inv_sqrt(double v) {
    double  d0 = 0.5 * v;
    int64_t i;
    memcpy(&i, &v, 8);
    i = 6910469410427058090LL - (i >> 1);
    memcpy(&v, &i, 8);
    return v * (1.5 - d0 * v * v);
}

/* Mth.clampedMap(v, 0, 6, 1, 0) == lerp(clamp(inverseLerp(v,0,6),0,1),1,0) */
static double clamped_map_610(double v) {
    double t = (v - 0.0) / (6.0 - 0.0); /* inverseLerp */
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return 1.0 + t * (0.0 - 1.0); /* Mth.lerp(t, 1, 0) */
}

/* getBuryContribution: clampedMap(Mth.length(dx,dy,dz), 0, 6, 1, 0) */
static double bury_contribution(double dx, double dy, double dz) {
    double d = sqrt(dx * dx + dy * dy + dz * dz);
    return clamped_map_610(d);
}

/* getBeardContribution(dx,dy,dz,yToGround): 커널 범위 밖 0.0;
 * value = -dyo * fastInvSqrt(distSqr/2)/2; value * KERNEL[zi*576+xi*24+yi] */
static double beard_contribution(int32_t dx, int32_t dy, int32_t dz,
                                 int32_t y_to_ground) {
    int32_t xi = dx + 12, yi = dy + 12, zi = dz + 12;
    if (xi < 0 || xi >= 24 || yi < 0 || yi >= 24 || zi < 0 || zi >= 24)
        return 0.0;
    double dyo = (double)y_to_ground + 0.5;
    double dsq = (double)dx * (double)dx + dyo * dyo +
                 (double)dz * (double)dz;
    double value = -dyo * mth_fast_inv_sqrt(dsq / 2.0) / 2.0;
    float  k;
    uint32_t kb = HC_BEARD_KERNEL_BITS[zi * 24 * 24 + xi * 24 + yi];
    memcpy(&k, &kb, 4);
    return value * (double)k;
}

static int32_t max_i32(int32_t a, int32_t b) {
    return a > b ? a : b;
}

double hc_beard_compute(const hc_beard_t *b, int32_t x, int32_t y,
                        int32_t z) {
    if (!b || !b->has_any)
        return 0.0;
    if (x < b->affected[0] || x > b->affected[3] || y < b->affected[1] ||
        y > b->affected[4] || z < b->affected[2] || z > b->affected[5])
        return 0.0;
    double v = 0.0;
    for (int32_t i = 0; i < b->n_rigids; i++) {
        const hc_beard_rigid_t *r = &b->rigids[i];
        int32_t dx = max_i32(0, max_i32(r->bb[0] - x, x - r->bb[3]));
        int32_t dz = max_i32(0, max_i32(r->bb[2] - z, z - r->bb[5]));
        int32_t ground = r->bb[1] + r->gld;
        int32_t dy_to_ground = y - ground;
        int32_t dy;
        switch (r->adj) {
        case HC_TA_BURY:
        case HC_TA_BEARD_THIN:
            dy = dy_to_ground;
            break;
        case HC_TA_BEARD_BOX:
            dy = max_i32(0, max_i32(ground - y, y - r->bb[4]));
            break;
        case HC_TA_ENCAPSULATE:
            dy = max_i32(0, max_i32(r->bb[1] - y, y - r->bb[4]));
            break;
        default: /* NONE — forStructuresInChunk 가 걸러서 도달 불가 */
            dy = 0;
            break;
        }
        switch (r->adj) {
        case HC_TA_BURY:
            v += bury_contribution((double)dx, (double)dy / 2.0,
                                   (double)dz);
            break;
        case HC_TA_BEARD_THIN:
        case HC_TA_BEARD_BOX:
            v += beard_contribution(dx, dy, dz, dy_to_ground) * 0.8;
            break;
        case HC_TA_ENCAPSULATE:
            v += bury_contribution((double)dx / 2.0, (double)dy / 2.0,
                                   (double)dz / 2.0) *
                 0.8;
            break;
        default:
            break;
        }
    }
    for (int32_t i = 0; i < b->n_junctions; i++) {
        const hc_beard_junction_t *j = &b->junctions[i];
        int32_t dx = x - j->sx;
        int32_t dy = y - j->sgy;
        int32_t dz = z - j->sz;
        v += beard_contribution(dx, dy, dz, dy) * 0.4;
    }
    return v;
}
