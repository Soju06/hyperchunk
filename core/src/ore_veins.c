#include "hc_gen_noise.h"

#include <math.h>

/* OreVeinifier 26.2 (javap) — 상수는 전부 (double)(float) 승격값 그대로.
 *
 * VeinType (<clinit>):
 *   COPPER: ore=copper_ore, raw=raw_copper_block, filler=granite, y 0..50
 *   IRON:   ore=deepslate_iron_ore, raw=raw_iron_block, filler=tuff, y -60..-8
 *
 * vein_toggle/vein_ridged 는 ctx == NoiseChunk (블록 루프) 로 평가된다 —
 * interpolated 마커가 점진 lerp 값을 돌려주는 BLOCK 모드. vein_gap 은
 * 마커 없는 노이즈라 모드 무관이지만 같은 ctx 를 쓴다. RNG 는
 * oreRandom.at(x,y,z) 에서 nextFloat 1~3회: solidness(0.7f) →
 * richness(clampedMap) → raw-ore(0.02f). */

static double clamped_map(double v, double a, double b, double c, double d) {
    double t = (v - a) / (b - a);
    if (t < 0.0)
        return c;
    if (t > 1.0)
        return d;
    return c + t * (d - c);
}

int hc_ore_vein_block(hc_noise_chunk_t *nc, int32_t x, int32_t y, int32_t z) {
    double toggle = hc_nc_eval_block(nc, nc->roots.vein_toggle, x, y, z);
    int    is_copper = toggle > 0.0; /* dcmpl ifle — NaN → IRON */
    double abs_toggle = fabs(toggle);

    int32_t min_y = is_copper ? 0 : -60;
    int32_t max_y = is_copper ? 50 : -8;
    int32_t below_top = max_y - y;
    int32_t above_bottom = y - min_y;
    if (above_bottom < 0 || below_top < 0)
        return -1;

    int32_t dist = below_top < above_bottom ? below_top : above_bottom;
    /* EDGE_ROUNDOFF_BEGIN=20, MAX_EDGE_ROUNDOFF=-0.2 */
    double edge = clamped_map((double)dist, 0.0, 20.0, -0.2, 0.0);
    /* VEININESS_THRESHOLD = 0.4f — dcmpg ifge: NaN 도 null 경로 */
    if (!(abs_toggle + edge >= 0.4000000059604645))
        return -1;

    hc_xoro_t r;
    hc_xoro_at(&nc->ore_fork, x, y, z, &r);
    /* VEIN_SOLIDNESS = 0.7f */
    if (hc_xoro_next_float(&r) > 0.7f)
        return -1;
    if (hc_nc_eval_block(nc, nc->roots.vein_ridged, x, y, z) >= 0.0)
        return -1;

    /* MIN/MAX_RICHNESS = 0.1f/0.3f, MAX_RICHNESS_THRESHOLD = 0.6f */
    double richness =
        clamped_map(abs_toggle, 0.4000000059604645, 0.6000000238418579,
                    0.10000000149011612, 0.30000001192092896);
    /* nextFloat 는 richness 비교 '전에' 소비된다 (바이트코드 순서) */
    if ((double)hc_xoro_next_float(&r) < richness &&
        hc_nc_eval_block(nc, nc->roots.vein_gap, x, y, z) >
            -0.30000001192092896 /* SKIP_ORE_IF_GAP_NOISE_IS_BELOW = -0.3f */) {
        /* CHANCE_OF_RAW_ORE_BLOCK = 0.02f */
        if (hc_xoro_next_float(&r) < 0.02f)
            return is_copper ? HC_B_RAW_COPPER_BLOCK : HC_B_RAW_IRON_BLOCK;
        return is_copper ? HC_B_COPPER_ORE : HC_B_DEEPSLATE_IRON_ORE;
    }
    return is_copper ? HC_B_GRANITE : HC_B_TUFF;
}
