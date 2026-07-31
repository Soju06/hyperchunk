#include "hc_carvers.h" /* hc_mth_sin (Mth 테이블) */
#include "hc_features.h"
#include "hc_jdk_trig.h" /* ore 각도 sin/cos (JDK 스텁 이식) */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 07_features — 리전/술어/배치 파이프라인/피처 본문.
 * 시맨틱 출처 (전부 26.2 바이트코드 재구성):
 *  - 파이프라인 실행 순서: task9a A2 §1.2 (depth-first 루프 중첩; 각
 *    modifier 의 드로우는 위치가 그 단계에 도달하는 순간)
 *  - modifier 9종: task9a A2 §2-3
 *  - OreFeature: task9a A3 (검증 부록 포함 — BitSet 여유 할당)
 *  - Spring/UnderwaterMagma: task9a A4
 *  - MonsterRoom 검증 단계: 본 태스크 javap (place 오프셋 0..263) —
 *    성공 경로(방 설치)는 9b; 도달하면 fail-loud. */

/* --- 리전 --- */

hc_chunk_t *hc_feat_region_chunk(const hc_feat_region_t *rg, int32_t cx,
                                 int32_t cz) {
    int32_t dx = cx - rg->cx0, dz = cz - rg->cz0;
    assert(dx >= 0 && dx < rg->n && dz >= 0 && dz < rg->n);
    hc_chunk_t *c = rg->chunks[dz * rg->n + dx];
    assert(c && c->cx == cx && c->cz == cz);
    return c;
}

static int32_t floor_div16(int32_t v) {
    return v >> 4; /* 산술 시프트 == Java >> (blockToSectionCoord) */
}

uint16_t hc_feat_get_block(const hc_feat_region_t *rg, int32_t x, int32_t y,
                           int32_t z) {
    /* y 범위 밖 = AIR (VOID_AIR.isAir / bulk 섹션 범위 밖 AIR — 우리가
     * 쓰는 술어(isAir/유체/태그/isSolid)에서 결과 동일) */
    if (y < HC_MIN_Y || y > HC_MAX_Y)
        return HC_B_AIR;
    hc_chunk_t *c = hc_feat_region_chunk(rg, floor_div16(x), floor_div16(z));
    return c->states[hc_idx(x & 15, y, z & 15)];
}

int hc_feat_set_block(hc_feat_region_t *rg, int32_t x, int32_t y, int32_t z,
                      uint16_t id) {
    /* ensureCanWrite: 쓰기 창 = center ±1 (soft-fail, task9pre A4 §3.1);
     * isUpgrading 분기는 신규 월드에서 false. y 범위 밖: ProtoChunk
     * .setBlockState 가 VOID_AIR 반환으로 무시 — 같은 no-op. */
    int32_t cx = floor_div16(x), cz = floor_div16(z);
    if (cx < rg->center_cx - 1 || cx > rg->center_cx + 1 ||
        cz < rg->center_cz - 1 || cz > rg->center_cz + 1)
        return 0;
    if (y < HC_MIN_Y || y > HC_MAX_Y)
        return 0;
    hc_chunk_t *c = hc_feat_region_chunk(rg, cx, cz);
    c->states[hc_idx(x & 15, y, z & 15)] = id;
    /* FINAL 하이트맵 4종 갱신은 9b (step 9 부터 읽음) — *_WG 는 frozen */
    return 1;
}

int32_t hc_feat_height_wg(const hc_feat_region_t *rg, int hm_type, int32_t x,
                          int32_t z) {
    if (hm_type == HC_HM_LIVE_9B) {
        fprintf(stderr, "hc_features FATAL: live FINAL heightmap read "
                        "reached — unimplemented until 9b\n");
        abort();
    }
    hc_chunk_t *c = hc_feat_region_chunk(rg, floor_div16(x), floor_div16(z));
    size_t col = hc_col_idx(x & 15, z & 15);
    return hm_type == HC_HM_OCEAN_FLOOR_WG ? c->heightmap_ocean_floor[col]
                                           : c->heightmap_ws[col];
}

/* --- 실행 환경 --- */

typedef struct {
    hc_feat_region_t       *rg;
    hc_wgr_t               *rng;
    const hc_feat_reg_t    *reg;
    const hc_biome_view_t  *view;
    const hc_pfeat_t       *pf;
    int32_t                 step, index;
    const hc_feat_trace_t  *trace;
    int32_t                 npos;
    int32_t                 placed_any; /* 0/1 */
    int32_t                 unknown;    /* 미구현 본문 도달 */
} feat_env_t;

static void die(const char *what, const char *detail) {
    fprintf(stderr, "hc_features FATAL: %s%s%s\n", what, detail ? ": " : "",
            detail ? detail : "");
    abort();
}

/* --- 블록 술어 (드로우 0) --- */

static int mask_test(const uint64_t *mask, uint16_t id) {
    return (mask[id >> 6] >> (id & 63)) & 1u;
}

static int bpred_eval(feat_env_t *e, const hc_bpred_t *p, int32_t x, int32_t y,
                      int32_t z) {
    x += p->off[0];
    y += p->off[1];
    z += p->off[2];
    switch (p->kind) {
    case HC_BP_MATCHING_FLUIDS_WATER:
        /* 소스 물 블록만 — flowing/waterlogged 는 테이블에 없다 (A2 §2.8) */
        return hc_feat_get_block(e->rg, x, y, z) == HC_B_WATER;
    case HC_BP_MATCHING_BLOCK_TAG:
        return mask_test(p->tag_mask, hc_feat_get_block(e->rg, x, y, z));
    case HC_BP_NOT:
        return !bpred_eval(e, &p->children[0], x, y, z);
    case HC_BP_ALL_OF:
        for (int32_t i = 0; i < p->n_children; i++)
            if (!bpred_eval(e, &p->children[i], x, y, z))
                return 0;
        return 1;
    case HC_BP_INSIDE_WORLD_BOUNDS:
        return y >= HC_MIN_Y && y <= HC_MAX_Y;
    case HC_BP_SOLID:
        return hc_block_is_solid(hc_feat_get_block(e->rg, x, y, z));
    }
    die("unknown block predicate kind", NULL);
    return 0;
}

/* --- providers --- */

static int32_t iprov_sample(hc_wgr_t *r, const hc_iprov_t *p) {
    if (p->kind == HC_IP_CONST)
        return p->a;
    if (p->kind == HC_IP_UNSUPPORTED_9B)
        die("unsupported int provider sampled (clamped_normal — 9b)", NULL);
    return hc_mth_random_between_inclusive(r, p->a, p->b);
}

static int32_t hprov_sample(hc_wgr_t *r, const hc_hprov_t *p) {
    int32_t lo = p->min_y, hi = p->max_y;
    switch (p->kind) {
    case HC_HP_UNIFORM:
        if (lo > hi)
            return lo; /* 경고 경로 — 드로우 0 (A2 §3.2) */
        return hc_mth_random_between_inclusive(r, lo, hi);
    case HC_HP_TRAPEZOID: {
        if (lo > hi)
            return lo;
        int32_t range = hi - lo;
        if (p->plateau >= range)
            return hc_mth_random_between_inclusive(r, lo, hi);
        int32_t half_lower = (range - p->plateau) / 2;
        int32_t upper = range - half_lower;
        return lo + hc_mth_random_between_inclusive(r, 0, upper) +
               hc_mth_random_between_inclusive(r, 0, half_lower);
    }
    case HC_HP_VERY_BIASED_TO_BOTTOM: {
        if (hi - lo - p->inner + 1 <= 0)
            return lo;
        int32_t y1 = hc_mth_next_int_range(r, lo + p->inner, hi);
        int32_t y2 = hc_mth_next_int_range(r, lo, y1 - 1);
        return hc_mth_next_int_range(r, lo, y2 - 1 + p->inner);
    }
    }
    die("unknown height provider kind", NULL);
    return 0;
}

/* --- Mth.ceil(float) = (int)Math.ceil((double)f) (A3 §1) --- */
static int32_t mth_ceil_f(float f) {
    return (int32_t)ceil((double)f);
}
static int32_t mth_floor_d(double d) {
    return (int32_t)floor(d);
}

/* --- OreFeature (task9a A3 §1-5) --- */

/* size<=64 상한에서 width<=26, height<=14; 검증 부록의 여유 경계
 * (width+1)*(height+1)*(width+1) = 10935 비트 */
enum { ORE_VISITED_WORDS = (27 * 15 * 27 + 63) / 64 };

static int ore_can_place(feat_env_t *e, const hc_ore_cfg_t *cfg, int32_t ti,
                         uint16_t cur, int32_t x, int32_t y, int32_t z) {
    /* (1) rule test — tag_match 뿐 (드로우 0) */
    if (!mask_test(cfg->targets[ti].rule_mask, cur))
        return 0;
    /* (2) shouldSkipAirCheck (A3 §3) */
    float chance = cfg->discard_on_air;
    if (chance <= 0.0f)
        return 1;
    if (!(chance >= 1.0f)) {
        if (hc_wgr_next_float(e->rng) >= chance)
            return 1;
    }
    /* (3) isAdjacentToAir — DOWN,UP,NORTH,SOUTH,WEST,EAST 단락 */
    static const int8_t D[6][3] = {{0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                                   {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};
    for (int i = 0; i < 6; i++)
        if (hc_block_is_air(
                hc_feat_get_block(e->rg, x + D[i][0], y + D[i][1], z + D[i][2])))
            return 0;
    return 1;
}

static int ore_do_place(feat_env_t *e, const hc_ore_cfg_t *cfg, double x1,
                        double x2, double z1, double z2, double y1, double y2,
                        int32_t min_x, int32_t min_y, int32_t min_z,
                        int32_t width, int32_t height) {
    int32_t  placed = 0;
    uint64_t visited[ORE_VISITED_WORDS];
    memset(visited, 0, sizeof visited);
    int32_t size = cfg->size;
    double  shape[64 * 4];

    for (int32_t i = 0; i < size; i++) {
        float  t = (float)i / (float)size;
        double cx = x1 + ((double)t) * (x2 - x1); /* Mth.lerp(d,a,b)=a+d*(b-a) */
        double cy = y1 + ((double)t) * (y2 - y1);
        double cz = z1 + ((double)t) * (z2 - z1);
        double dr = hc_wgr_next_double(e->rng) * (double)size / 16.0;
        /* 반지름 sine 은 Mth 테이블 (float 덧셈 후 확폭 — A3 §2.1) */
        double radius =
            ((double)(hc_mth_sin((double)(3.1415927f * t)) + 1.0f) * dr + 1.0) /
            2.0;
        shape[i * 4 + 0] = cx;
        shape[i * 4 + 1] = cy;
        shape[i * 4 + 2] = cz;
        shape[i * 4 + 3] = radius;
    }
    /* 겹침 가지치기 (드로우 0, A3 §2.2) */
    for (int32_t i = 0; i < size - 1; i++) {
        if (shape[i * 4 + 3] <= 0.0)
            continue;
        for (int32_t j = i + 1; j < size; j++) {
            if (shape[j * 4 + 3] <= 0.0)
                continue;
            double dx = shape[i * 4 + 0] - shape[j * 4 + 0];
            double dy = shape[i * 4 + 1] - shape[j * 4 + 1];
            double dz = shape[i * 4 + 2] - shape[j * 4 + 2];
            double dr = shape[i * 4 + 3] - shape[j * 4 + 3];
            if (dr * dr > dx * dx + dy * dy + dz * dz) {
                if (dr > 0.0)
                    shape[j * 4 + 3] = -1.0;
                else
                    shape[i * 4 + 3] = -1.0;
            }
        }
    }
    /* 스윕 (A3 §2.3): x → y → z, 레벨별 조기 탈출 */
    for (int32_t i = 0; i < size; i++) {
        double r = shape[i * 4 + 3];
        if (r < 0.0)
            continue;
        double cx = shape[i * 4 + 0];
        double cy = shape[i * 4 + 1];
        double cz = shape[i * 4 + 2];
        int32_t x0 = mth_floor_d(cx - r);
        if (x0 < min_x)
            x0 = min_x;
        int32_t y0 = mth_floor_d(cy - r);
        if (y0 < min_y)
            y0 = min_y;
        int32_t z0 = mth_floor_d(cz - r);
        if (z0 < min_z)
            z0 = min_z;
        int32_t x9 = mth_floor_d(cx + r);
        if (x9 < x0)
            x9 = x0;
        int32_t y9 = mth_floor_d(cy + r);
        if (y9 < y0)
            y9 = y0;
        int32_t z9 = mth_floor_d(cz + r);
        if (z9 < z0)
            z9 = z0;
        for (int32_t x = x0; x <= x9; x++) {
            double ddx = ((double)x + 0.5 - cx) / r;
            if (ddx * ddx >= 1.0)
                continue;
            for (int32_t y = y0; y <= y9; y++) {
                double ddy = ((double)y + 0.5 - cy) / r;
                if (ddx * ddx + ddy * ddy >= 1.0)
                    continue;
                for (int32_t z = z0; z <= z9; z++) {
                    double ddz = ((double)z + 0.5 - cz) / r;
                    if (ddx * ddx + ddy * ddy + ddz * ddz >= 1.0)
                        continue;
                    if (y < HC_MIN_Y || y > HC_MAX_Y) /* isOutsideBuildHeight */
                        continue;
                    int32_t bit = (x - min_x) + (y - min_y) * width +
                                  (z - min_z) * width * height;
                    assert(bit >= 0 && bit < ORE_VISITED_WORDS * 64);
                    if ((visited[bit >> 6] >> (bit & 63)) & 1u)
                        continue;
                    visited[bit >> 6] |= 1ull << (bit & 63);
                    /* ensureCanWrite — visited 마킹 뒤 (A3 §2.3) */
                    int32_t ccx = floor_div16(x), ccz = floor_div16(z);
                    if (ccx < e->rg->center_cx - 1 || ccx > e->rg->center_cx + 1 ||
                        ccz < e->rg->center_cz - 1 || ccz > e->rg->center_cz + 1)
                        continue;
                    uint16_t cur = hc_feat_get_block(e->rg, x, y, z);
                    for (int32_t ti = 0; ti < cfg->n_targets; ti++) {
                        if (ore_can_place(e, cfg, ti, cur, x, y, z)) {
                            hc_chunk_t *c =
                                hc_feat_region_chunk(e->rg, ccx, ccz);
                            c->states[hc_idx(x & 15, y, z & 15)] =
                                cfg->targets[ti].state;
                            placed++;
                            break;
                        }
                    }
                }
            }
        }
    }
    return placed > 0;
}

static int ore_place(feat_env_t *e, const hc_ore_cfg_t *cfg, int32_t ox,
                     int32_t oy, int32_t oz) {
    float angle = hc_wgr_next_float(e->rng) * 3.1415927f;
    float f7 = (float)cfg->size / 8.0f;
    int32_t i8 = mth_ceil_f(((float)cfg->size / 16.0f * 2.0f + 1.0f) / 2.0f);
    /* JDK Math.sin/cos(double) — HotSpot x86-64 스텁 C 이식 (jdk_trig.c).
     * 게이트 = golden/rng/jdk_sincos.txt 벡터 (test_features_rng.c) */
    double s = hc_jdk_sin((double)angle);
    double c = hc_jdk_cos((double)angle);
    double x1 = (double)ox + s * (double)f7;
    double x2 = (double)ox - s * (double)f7;
    double z1 = (double)oz + c * (double)f7;
    double z2 = (double)oz - c * (double)f7;
    double y1 = (double)(oy + hc_wgr_next_int(e->rng, 3) - 2);
    double y2 = (double)(oy + hc_wgr_next_int(e->rng, 3) - 2);
    int32_t min_x = ox - mth_ceil_f(f7) - i8;
    int32_t min_y = oy - 2 - i8;
    int32_t min_z = oz - mth_ceil_f(f7) - i8;
    int32_t width = 2 * (mth_ceil_f(f7) + i8);
    int32_t height = 2 * (2 + i8);
    for (int32_t x = min_x; x <= min_x + width; x++)
        for (int32_t z = min_z; z <= min_z + width; z++)
            if (min_y <= hc_feat_height_wg(e->rg, HC_HM_OCEAN_FLOOR_WG, x, z))
                return ore_do_place(e, cfg, x1, x2, z1, z2, y1, y2, min_x,
                                    min_y, min_z, width, height);
    return 0;
}

/* --- SpringFeature (task9a A4 §3) — 드로우 0 --- */

static int spring_place(feat_env_t *e, const hc_spring_cfg_t *cfg, int32_t ox,
                        int32_t oy, int32_t oz) {
    if (!mask_test(cfg->valid_mask, hc_feat_get_block(e->rg, ox, oy + 1, oz)))
        return 0;
    if (cfg->requires_block_below &&
        !mask_test(cfg->valid_mask, hc_feat_get_block(e->rg, ox, oy - 1, oz)))
        return 0;
    uint16_t s = hc_feat_get_block(e->rg, ox, oy, oz);
    if (!hc_block_is_air(s) && !mask_test(cfg->valid_mask, s))
        return 0;
    /* W,E,N,S,down 순서 — rocks 5회 뒤 holes 5회 (읽기만, 결과는 순서
     * 무관하지만 바닐라 순서를 그대로 둔다) */
    static const int8_t N[5][3] = {
        {-1, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 0, 1}, {0, -1, 0}};
    int32_t rocks = 0, holes = 0;
    for (int i = 0; i < 5; i++)
        rocks += mask_test(cfg->valid_mask, hc_feat_get_block(e->rg, ox + N[i][0],
                                                              oy + N[i][1],
                                                              oz + N[i][2]));
    for (int i = 0; i < 5; i++)
        holes += hc_block_is_air(hc_feat_get_block(e->rg, ox + N[i][0],
                                                   oy + N[i][1], oz + N[i][2]));
    if (rocks == cfg->rock_count && holes == cfg->hole_count) {
        hc_feat_set_block(e->rg, ox, oy, oz, cfg->fluid_block);
        /* scheduleTick → fluid_ticks NBT — 07 blocks/heightmaps 덤프 밖 */
        return 1;
    }
    return 0;
}

/* --- MonsterRoomFeature 검증 단계 (본 태스크 javap, place 0..263) --- */

static int monster_room_place(feat_env_t *e, int32_t ox, int32_t oy,
                              int32_t oz) {
    int32_t j = hc_wgr_next_int(e->rng, 2) + 2; /* DRAW 1 */
    int32_t xmin = -j - 1, xmax = j + 1;
    int32_t k = hc_wgr_next_int(e->rng, 2) + 2; /* DRAW 2 */
    int32_t zmin = -k - 1, zmax = k + 1;
    int32_t doors = 0;
    for (int32_t dx = xmin; dx <= xmax; dx++)
        for (int32_t dy = -1; dy <= 4; dy++)
            for (int32_t dz = zmin; dz <= zmax; dz++) {
                int32_t x = ox + dx, y = oy + dy, z = oz + dz;
                int solid =
                    hc_block_is_solid(hc_feat_get_block(e->rg, x, y, z));
                if (dy == -1 && !solid)
                    return 0;
                if (dy == 4 && !solid)
                    return 0;
                if ((dx == xmin || dx == xmax || dz == zmin || dz == zmax) &&
                    dy == 0 &&
                    hc_block_is_air(hc_feat_get_block(e->rg, x, y, z)) &&
                    hc_block_is_air(hc_feat_get_block(e->rg, x, y + 1, z)))
                    doors++;
            }
    if (doors < 1 || doors > 5)
        return 0;
    /* 성공 경로(방 설치 + 이끼/스포너 드로우)는 9b — 그리드 골든에서
     * 검증 통과 사례 0 (trace f-placed=0). 도달하면 조용한 발산 대신
     * 즉사한다. */
    die("monster_room validation passed — success path unimplemented (9b)",
        NULL);
    return 0;
}

/* --- UnderwaterMagmaFeature (task9a A4 §5) --- */

static int umagma_place(feat_env_t *e, const hc_umagma_cfg_t *cfg, int32_t ox,
                        int32_t oy, int32_t oz) {
    /* Column.scan: origin 블록이 정확히 물이어야 (블록 정체성) */
    if (hc_feat_get_block(e->rg, ox, oy, oz) != HC_B_WATER)
        return 0;
    /* scanDirection(UP) — 읽기만; ceil 존재 여부는 getFloor 에 무관하나
     * 바닐라가 먼저 수행한다 (드로우 없음이라 결과 영향 0) */
    /* scanDirection(DOWN): i=1; y 이동하며 물인 동안 진행 */
    int32_t y = oy;
    int32_t i = 1;
    while (i < cfg->floor_search_range &&
           hc_feat_get_block(e->rg, ox, y, oz) == HC_B_WATER) {
        y--;
        i++;
    }
    if (hc_feat_get_block(e->rg, ox, y, oz) == HC_B_WATER)
        return 0; /* tip 이 물이면 floor 없음 → getFloor empty */
    int32_t fy = y;
    int32_t r = cfg->placement_radius;
    int32_t placed = 0;
    /* betweenClosed: x 최속, y, z 최완 (A4 §5) — z 밖, y 중간, x 안쪽 */
    for (int32_t dz = -r; dz <= r; dz++)
        for (int32_t dy = -r; dy <= r; dy++)
            for (int32_t dx = -r; dx <= r; dx++) {
                int32_t px = ox + dx, py = fy + dy, pz = oz + dz;
                /* 확률 필터가 스트림 첫 단 — 위치마다 무조건 1 드로우 */
                if (!(hc_wgr_next_float(e->rng) < cfg->placement_prob))
                    continue;
                uint16_t s = hc_feat_get_block(e->rg, px, py, pz);
                if (s == HC_B_WATER || hc_block_is_air(s))
                    continue;
                /* 아래 블록의 UP 면 + 수평 4방 이웃 면이 전부 완전 폐색 */
                if (!hc_block_is_full_cube(
                        hc_feat_get_block(e->rg, px, py - 1, pz)))
                    continue;
                int ok = 1;
                static const int8_t H[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
                for (int d = 0; d < 4; d++)
                    if (!hc_block_is_full_cube(hc_feat_get_block(
                            e->rg, px + H[d][0], py, pz + H[d][1]))) {
                        ok = 0;
                        break;
                    }
                if (!ok)
                    continue;
                if (hc_feat_set_block(e->rg, px, py, pz, HC_B_MAGMA_BLOCK))
                    placed++;
                else
                    placed++; /* 바닐라 placedCount 는 setBlock 반환과 무관 */
            }
    return placed > 0;
}

/* --- 본문 디스패치: 1/0 = 확정, -1 = 미구현 --- */

static int32_t cf_place(feat_env_t *e, int32_t x, int32_t y, int32_t z) {
    switch (e->pf->cf_kind) {
    case HC_CF_ORE:
        return ore_place(e, &e->pf->cf.ore, x, y, z);
    case HC_CF_SPRING:
        return spring_place(e, &e->pf->cf.spring, x, y, z);
    case HC_CF_MONSTER_ROOM:
        return monster_room_place(e, x, y, z);
    case HC_CF_UNDERWATER_MAGMA:
        return umagma_place(e, &e->pf->cf.umagma, x, y, z);
    default:
        e->unknown = 1;
        return -1;
    }
}

/* --- 파이프라인 (A2 §1.2 루프 중첩) --- */

static void run_mods(feat_env_t *e, int32_t mi, int32_t x, int32_t y,
                     int32_t z);

static void leaf(feat_env_t *e, int32_t x, int32_t y, int32_t z) {
    int32_t placed = cf_place(e, x, y, z);
    e->npos++;
    if (placed == 1)
        e->placed_any = 1;
    if (e->trace && e->trace->on_pos)
        e->trace->on_pos(e->trace->ud, e->step, e->index, x, y, z, placed);
}

static void run_mods(feat_env_t *e, int32_t mi, int32_t x, int32_t y,
                     int32_t z) {
    if (mi == e->pf->n_mods) {
        leaf(e, x, y, z);
        return;
    }
    const hc_pmod_t *m = &e->pf->mods[mi];
    switch (m->kind) {
    case HC_PM_RARITY_FILTER: {
        /* 임계는 binary32 몫 (A2 §2.3); 드로우는 항상 */
        float f = hc_wgr_next_float(e->rng);
        if (f < 1.0f / (float)m->chance)
            run_mods(e, mi + 1, x, y, z);
        return;
    }
    case HC_PM_COUNT: {
        int32_t n = iprov_sample(e->rng, &m->count);
        for (int32_t i = 0; i < n; i++)
            run_mods(e, mi + 1, x, y, z);
        return;
    }
    case HC_PM_IN_SQUARE: {
        int32_t nx = hc_wgr_next_int(e->rng, 16) + x; /* X 먼저 */
        int32_t nz = hc_wgr_next_int(e->rng, 16) + z;
        run_mods(e, mi + 1, nx, y, nz);
        return;
    }
    case HC_PM_HEIGHT_RANGE:
        run_mods(e, mi + 1, x, hprov_sample(e->rng, &m->height), z);
        return;
    case HC_PM_HEIGHTMAP: {
        int32_t ny = hc_feat_height_wg(e->rg, m->hm_type, x, z);
        if (ny > HC_MIN_Y)
            run_mods(e, mi + 1, x, ny, z);
        return;
    }
    case HC_PM_ENV_SCAN: {
        /* A2 §2.9: 시작 위치 allowed 게이트 → (target? → 이동 → 범위 →
         * allowed? break) 루프 → 최종 target 테스트 1회 */
        int32_t cy = y;
        if (m->has_allowed && !bpred_eval(e, &m->allowed, x, cy, z))
            return;
        for (int32_t i = 0; i < m->max_steps; i++) {
            if (bpred_eval(e, &m->pred, x, cy, z)) {
                run_mods(e, mi + 1, x, cy, z);
                return;
            }
            cy += m->scan_dy;
            if (cy < HC_MIN_Y || cy > HC_MAX_Y)
                return; /* isOutsideBuildHeight → empty, 최종 테스트 없음 */
            if (m->has_allowed && !bpred_eval(e, &m->allowed, x, cy, z))
                break; /* → 최종 테스트로 */
        }
        if (bpred_eval(e, &m->pred, x, cy, z))
            run_mods(e, mi + 1, x, cy, z);
        return;
    }
    case HC_PM_SURF_REL_THRESHOLD: {
        int64_t h = hc_feat_height_wg(e->rg, m->hm_type, x, z);
        if (h + (int64_t)m->min_incl <= (int64_t)y &&
            (int64_t)y <= h + (int64_t)m->max_incl)
            run_mods(e, mi + 1, x, y, z);
        return;
    }
    case HC_PM_BLOCK_PRED:
        if (bpred_eval(e, &m->pred, x, y, z))
            run_mods(e, mi + 1, x, y, z);
        return;
    case HC_PM_RANDOM_OFFSET: {
        /* 샘플 순서: xzSpread→x, ySpread→y, xzSpread→z (task9a A2 §0.2) */
        int32_t dx = iprov_sample(e->rng, &m->count);
        int32_t dy = iprov_sample(e->rng, &m->y_spread);
        int32_t dz = iprov_sample(e->rng, &m->count);
        run_mods(e, mi + 1, x + dx, y + dy, z + dz);
        return;
    }
    case HC_PM_BIOME: {
        /* 최종 수정 위치의 3-D 바이옴 (fiddled zoom, 저장 쿼트) — 멤버십은
         * 이 feature 의 (step,index) 슬롯 (A2 §4; 슬롯은 전역 유일) */
        uint16_t b = hc_biome_view_get(e->view, x, y, z);
        assert(b < (uint16_t)e->reg->n_biomes);
        const uint64_t *row =
            &e->reg->member[e->step][(size_t)b * (size_t)e->reg->words[e->step]];
        if ((row[e->index >> 6] >> (e->index & 63)) & 1u)
            run_mods(e, mi + 1, x, y, z);
        return;
    }
    }
    die("unknown placement modifier kind", NULL);
}

/* features_compile.c / gen_features_stage.c 가 쓰는 내부 진입점 */
void hc_feat_run_placed(hc_feat_region_t *rg, hc_wgr_t *rng,
                        const hc_feat_reg_t *reg, const hc_biome_view_t *view,
                        const hc_pfeat_t *pf, int32_t step, int32_t index,
                        int32_t origin_x, int32_t origin_y, int32_t origin_z,
                        const hc_feat_trace_t *trace) {
    feat_env_t e;
    e.rg = rg;
    e.rng = rng;
    e.reg = reg;
    e.view = view;
    e.pf = pf;
    e.step = step;
    e.index = index;
    e.trace = trace;
    e.npos = 0;
    e.placed_any = 0;
    e.unknown = 0;
    run_mods(&e, 0, origin_x, origin_y, origin_z);
    if (trace && trace->on_feature) {
        int32_t placed = e.placed_any ? 1 : (e.unknown ? -1 : 0);
        trace->on_feature(trace->ud, step, index, pf->name, e.npos, placed);
    }
}
