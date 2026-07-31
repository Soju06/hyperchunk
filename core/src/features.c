#include "hc_carvers.h" /* hc_mth_sin (Mth 테이블) */
#include "features_internal.h"
#include "hc_jdk_trig.h" /* ore 각도 sin/cos (JDK 스텁 이식) */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define die hc_featx_die
#define mask_test hc_featx_mask_test
#define iprov_sample hc_featx_iprov_sample
#define sprov_sample hc_featx_sprov_sample
#define run_nested_pf hc_featx_run_nested

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

_Noreturn void hc_featx_die(const char *what, const char *detail) {
    fprintf(stderr, "hc_features FATAL: %s%s%s\n", what, detail ? ": " : "",
            detail ? detail : "");
    abort();
}

/* --- FINAL 하이트맵 4종 (task9pre A4 §4, 26.2 Heightmap 재구성) ---
 *
 * isOpaque 술어 (Heightmap$Types):
 *   OCEAN_FLOOR(+_WG)          blocksMotion
 *   WORLD_SURFACE(+_WG)        !isAir
 *   MOTION_BLOCKING            blocksMotion || fluid 비어있지 않음
 *   MOTION_BLOCKING_NO_LEAVES  위 && !LeavesBlock
 * update() 는 증분이지만 결과는 항상 최종 블록의 순수함수 (A4 §4.2) —
 * 프라임(재계산)과 등가. 값 = 최고 통과 y + 1, 빈 컬럼 = HC_MIN_Y. */

static int hm_opaque(int type, uint16_t id) {
    switch (type) {
    case HC_HMF_OCEAN_FLOOR:
        return hc_block_blocks_motion(id);
    case HC_HMF_WORLD_SURFACE:
        return !hc_block_is_air(id);
    case HC_HMF_MOTION_BLOCKING:
        return hc_block_blocks_motion(id) || hc_block_fluid_nonempty(id);
    case HC_HMF_MOTION_BLOCKING_NO_LEAVES:
        return (hc_block_blocks_motion(id) || hc_block_fluid_nonempty(id)) &&
               !hc_block_is_leaves(id);
    }
    die("unknown final heightmap type", NULL);
    return 0;
}

static void hm_prime_one(hc_chunk_t *c, int type) {
    for (int lz = 0; lz < 16; lz++)
        for (int lx = 0; lx < 16; lx++) {
            size_t  col = hc_col_idx(lx, lz);
            int32_t v = HC_MIN_Y;
            for (int32_t y = HC_MAX_Y; y >= HC_MIN_Y; y--) {
                uint16_t s = c->states[hc_idx(lx, y, lz)];
                if (s != HC_B_AIR && hm_opaque(type, s)) {
                    v = y + 1;
                    break;
                }
            }
            c->heightmap_final[type][col] = v;
        }
    c->hm_final_primed |= (uint8_t)(1u << type);
}

void hc_feat_prime_final_maps(hc_chunk_t *c) {
    for (int t = 0; t < HC_HMF_COUNT; t++)
        hm_prime_one(c, t);
}

/* Heightmap.update(x,y,z,state) — 26.2 바이트코드 (A4 §4.2) */
static void hm_update(hc_chunk_t *c, int type, int lx, int32_t y, int lz,
                      uint16_t state) {
    size_t  col = hc_col_idx(lx, lz);
    int32_t first = c->heightmap_final[type][col];
    if (y <= first - 2)
        return;
    if (hm_opaque(type, state)) {
        if (y >= first)
            c->heightmap_final[type][col] = y + 1;
    } else if (first - 1 == y) {
        for (int32_t yy = y - 1; yy >= HC_MIN_Y; yy--)
            if (hm_opaque(type, c->states[hc_idx(lx, yy, lz)])) {
                c->heightmap_final[type][col] = yy + 1;
                return;
            }
        c->heightmap_final[type][col] = HC_MIN_Y;
    }
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
    int lx = x & 15, lz = z & 15;
    c->states[hc_idx(lx, y, lz)] = id;
    /* ProtoChunk.setBlockState: 섹션 쓰기 뒤 없는 FINAL 맵 지연 프라임 +
     * 4종 전부 증분 update (*_WG 는 frozen — heightmapsAfter(CARVERS)) */
    for (int t = 0; t < HC_HMF_COUNT; t++)
        if (!(c->hm_final_primed & (1u << t)))
            hm_prime_one(c, t);
    for (int t = 0; t < HC_HMF_COUNT; t++)
        hm_update(c, t, lx, y, lz, id);
    return 1;
}

/* *_WG 리프라임 (rg->wg_dropped 이후): OF_WG 는 blocksMotion, WS_WG 는
 * !isAir — FINAL 짝과 같은 술어지만 동결 시점이 다르므로 별도 저장. */
static void wg_prime_one(hc_chunk_t *c, int wg) {
    int ftype = wg == 0 ? HC_HMF_OCEAN_FLOOR : HC_HMF_WORLD_SURFACE;
    for (int lz = 0; lz < 16; lz++)
        for (int lx = 0; lx < 16; lx++) {
            size_t  col = hc_col_idx(lx, lz);
            int32_t v = HC_MIN_Y;
            for (int32_t y = HC_MAX_Y; y >= HC_MIN_Y; y--) {
                uint16_t s = c->states[hc_idx(lx, y, lz)];
                if (s != HC_B_AIR && hm_opaque(ftype, s)) {
                    v = y + 1;
                    break;
                }
            }
            c->heightmap_wg_reprimed[wg][col] = v;
        }
    c->wg_reprimed |= (uint8_t)(1u << wg);
}

int32_t hc_feat_height(hc_feat_region_t *rg, int hm_type, int32_t x,
                       int32_t z) {
    hc_chunk_t *c = hc_feat_region_chunk(rg, floor_div16(x), floor_div16(z));
    size_t col = hc_col_idx(x & 15, z & 15);
    switch (hm_type) {
    case HC_HM_OCEAN_FLOOR_WG:
        if (rg->wg_dropped) {
            if (!(c->wg_reprimed & 1u))
                wg_prime_one(c, 0);
            return c->heightmap_wg_reprimed[0][col];
        }
        return c->heightmap_ocean_floor[col];
    case HC_HM_WORLD_SURFACE_WG:
        if (rg->wg_dropped) {
            if (!(c->wg_reprimed & 2u))
                wg_prime_one(c, 1);
            return c->heightmap_wg_reprimed[1][col];
        }
        return c->heightmap_ws[col];
    default: {
        /* ChunkAccess.getHeight: 없는 단일 타입 지연 프라임 후 반환 */
        int t = hm_type - HC_HM_OCEAN_FLOOR;
        assert(t >= 0 && t < HC_HMF_COUNT);
        if (!(c->hm_final_primed & (1u << t)))
            hm_prime_one(c, t);
        return c->heightmap_final[t][col];
    }
    }
}

/* --- 실행 환경: feat_env_t 는 features_internal.h --- */

/* --- 블록 술어 (드로우 0) --- */

int hc_featx_mask_test(const uint64_t *mask, uint16_t id) {
    return (mask[id >> 6] >> (id & 63)) & 1u;
}

/* isFaceSturdy(pos, dir) — SupportType.FULL: getBlockSupportShape 의 그
 * 면이 완전. LeavesBlock 은 support shape 을 EMPTY 로 오버라이드해서
 * 절대 sturdy 가 아니다 (R1 §2.5 VERIFIED); 팔레트의 나머지는 완전
 * 큐브(support=collision=occlusion 전부 full)거나 식물류(전부 비-full).
 * CENTER/RIGID 도 완전 큐브에서 전부 true — 한 판정으로 축약. */
static int face_sturdy(feat_env_t *e, int32_t x, int32_t y, int32_t z,
                       int dir) {
    return hc_featx_face_sturdy_full(hc_feat_get_block(e->rg, x, y, z), dir);
}

/* would_survive: state.canSurvive(level, pos) — 블록별 디스패치 (R2) */
static int would_survive(feat_env_t *e, const char *name, int32_t x,
                         int32_t y, int32_t z);

#define bpred_eval hc_featx_bpred_eval

int hc_featx_bpred_eval(feat_env_t *e, const hc_bpred_t *p, int32_t x,
                        int32_t y, int32_t z) {
    x += p->off[0];
    y += p->off[1];
    z += p->off[2];
    switch (p->kind) {
    case HC_BP_MATCHING_FLUIDS_WATER:
        /* FluidState.is(water): 소스 물 + waterlogged=true (A2 §2.8) */
        return hc_block_fluid_is_water(hc_feat_get_block(e->rg, x, y, z));
    case HC_BP_MATCHING_FLUIDS_EMPTY:
        /* fluids=[empty]: FluidState.is(EMPTY) — 유체 없는 상태만 */
        return !hc_block_fluid_nonempty(hc_feat_get_block(e->rg, x, y, z));
    case HC_BP_REPLACEABLE:
        return hc_block_is_replaceable(hc_feat_get_block(e->rg, x, y, z));
    case HC_BP_MATCHING_BLOCK_TAG:
        return mask_test(p->tag_mask, hc_feat_get_block(e->rg, x, y, z));
    case HC_BP_NOT:
        return !bpred_eval(e, &p->children[0], x, y, z);
    case HC_BP_ALL_OF:
        for (int32_t i = 0; i < p->n_children; i++)
            if (!bpred_eval(e, &p->children[i], x, y, z))
                return 0;
        return 1;
    case HC_BP_ANY_OF:
        for (int32_t i = 0; i < p->n_children; i++)
            if (bpred_eval(e, &p->children[i], x, y, z))
                return 1;
        return 0;
    case HC_BP_INSIDE_WORLD_BOUNDS:
        return y >= HC_MIN_Y && y <= HC_MAX_Y;
    case HC_BP_SOLID:
        return hc_block_is_solid(hc_feat_get_block(e->rg, x, y, z));
    case HC_BP_WOULD_SURVIVE:
        return would_survive(e, p->ws_name, x, y, z);
    case HC_BP_HAS_STURDY_FACE:
        return face_sturdy(e, x, y, z, p->dir);
    case HC_BP_TRUE:
        return 1;
    }
    die("unknown block predicate kind", NULL);
    return 0;
}

/* --- providers --- */

int32_t hc_featx_iprov_sample(hc_wgr_t *r, const hc_iprov_t *p) {
    switch (p->kind) {
    case HC_IP_CONST:
        return p->a;
    case HC_IP_UNIFORM:
        return hc_mth_random_between_inclusive(r, p->a, p->b);
    case HC_IP_TRAPEZOID: {
        /* TrapezoidInt.sample (R4 §6.2) — 대칭 빠른 경로가 Height 판과
         * 다르다: min==−max && plateau==0 이면 a−b (드로우 2, 값이 일반
         * 경로와 다름 — 문자 그대로 이식). */
        if (p->c == 0 && p->b == -p->a)
            return hc_wgr_next_int(r, p->b + 1) - hc_wgr_next_int(r, p->b + 1);
        int32_t range = p->b - p->a;
        if (p->c == range)
            return hc_mth_random_between_inclusive(r, p->a, p->b);
        int32_t half = (range - p->c) / 2;
        int32_t upper = range - half;
        return p->a + hc_mth_random_between_inclusive(r, 0, upper) +
               hc_mth_random_between_inclusive(r, 0, half);
    }
    case HC_IP_BIASED_TO_BOTTOM: {
        /* BiasedToBottomInt.sample@0-30: min + nextInt(nextInt(b-a+1)+1)
         * — 안쪽 드로우 먼저 */
        int32_t inner = hc_wgr_next_int(r, p->b - p->a + 1);
        return p->a + hc_wgr_next_int(r, inner + 1);
    }
    case HC_IP_WEIGHTED_LIST: {
        /* WeightedList.getRandom: nextInt(total) 1 드로우 후 순회 (R4) */
        int32_t roll = hc_wgr_next_int(r, p->total_weight);
        for (int32_t i = 0; i < p->n_entries; i++) {
            roll -= p->entries[i].weight;
            if (roll < 0)
                return iprov_sample(r, &p->entries[i].prov);
        }
        die("weighted_list roll out of range", NULL);
        return 0;
    }
    }
    die("unsupported int provider sampled (clamped_normal?)", NULL);
    return 0;
}

/* --- BlockState provider 샘플 (R3: source 먼저, values 나중) --- */

uint16_t hc_featx_sprov_sample(hc_wgr_t *r, const hc_sprov_t *p) {
    switch (p->kind) {
    case HC_SP_SIMPLE:
        return p->state;
    case HC_SP_WEIGHTED: {
        int32_t roll = hc_wgr_next_int(r, p->total_weight);
        for (int32_t i = 0; i < p->n_entries; i++) {
            roll -= p->entries[i].weight;
            if (roll < 0)
                return p->entries[i].state;
        }
        die("weighted state roll out of range", NULL);
        return 0;
    }
    case HC_SP_RANDOMIZED_INT: {
        uint16_t base = sprov_sample(r, p->source);
        int32_t  v = iprov_sample(r, &p->values);
        /* property=age 적용 — cave_vines 패밀리만 매핑 (컴파일이 보장) */
        if (base >= HC_B_CAVE_VINES_BASE &&
            base < HC_B_CAVE_VINES_BASE + 52) {
            int32_t berries = (base - HC_B_CAVE_VINES_BASE) / 26;
            if (v < 0 || v > 25)
                die("randomized_int age out of range", NULL);
            return (uint16_t)(HC_B_CAVE_VINES_BASE + berries * 26 + v);
        }
        die("randomized_int on unmapped block family", NULL);
        return 0;
    }
    }
    die("unknown state provider kind", NULL);
    return 0;
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
            if (min_y <= hc_feat_height(e->rg, HC_HM_OCEAN_FLOOR_WG, x, z))
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

/* --- MonsterRoomFeature (본 태스크 javap, place@0-848 + R5d:
 * StructurePiece.reorient@0-227, RandomizableContainer
 * .setBlockEntityLootTable@0-38, BaseSpawner.setEntityId /
 * getOrCreateNextSpawnData — 빈 spawnPotentials 는 WeightedList.getRandom
 * 셀렉터 null 조기 반환이라 0 드로우) --- */

/* Plane.HORIZONTAL 순서 N,E,S,W; 청크 팔레트 chest facing 오프셋과 회전 */
static const int8_t  MR_DX[4] = {0, 1, 0, -1};
static const int8_t  MR_DZ[4] = {-1, 0, 1, 0};
static const uint8_t MR_OPP[4] = {2, 3, 0, 1};
static const uint8_t MR_CW[4] = {1, 2, 3, 0};
static const uint8_t MR_CHEST_OFF[4] = {0, 3, 1, 2}; /* N,E,S,W → N0,E3,S1,W2 */

static int mr_is_chest(uint16_t s) {
    return s >= HC_B_CHEST_BASE && s < HC_B_CHEST_BASE + 4;
}

/* safeSetBlock(predicate = !#features_cannot_replace(현재 블록)) */
static void mr_safe_set(feat_env_t *e, int32_t x, int32_t y, int32_t z,
                        uint16_t st) {
    if (!mask_test(e->reg->tag_features_cannot_replace,
                   hc_feat_get_block(e->rg, x, y, z)))
        hc_feat_set_block(e->rg, x, y, z, st);
}

/* StructurePiece.reorient: 이웃에 CHEST 있으면 기본(facing=north) 그대로;
 * solidRender(풀 큐브) 이웃 정확히 1개면 그 반대; 아니면 폴백 워크
 * (기본 N 에서 opp → cw → opp 순으로 solidRender 를 피한다) */
static uint16_t mr_reorient_chest(feat_env_t *e, int32_t x, int32_t y,
                                  int32_t z) {
    int found = -1;
    for (int d = 0; d < 4; d++) {
        uint16_t s = hc_feat_get_block(e->rg, x + MR_DX[d], y, z + MR_DZ[d]);
        if (mr_is_chest(s))
            return (uint16_t)(HC_B_CHEST_BASE + 0); /* @61-62: 상태 그대로 */
        if (hc_block_is_full_cube(s)) { /* isSolidRender */
            if (found < 0)
                found = d;
            else {
                found = -2; /* 둘 이상 → 폴백 (@81-83) */
                break;
            }
        }
    }
    if (found >= 0)
        return (uint16_t)(HC_B_CHEST_BASE + MR_CHEST_OFF[MR_OPP[found]]);
    int dir = 0; /* 기본 facing = NORTH (@108-118) */
    if (hc_block_is_full_cube(
            hc_feat_get_block(e->rg, x + MR_DX[dir], y, z + MR_DZ[dir])))
        dir = MR_OPP[dir];
    if (hc_block_is_full_cube(
            hc_feat_get_block(e->rg, x + MR_DX[dir], y, z + MR_DZ[dir])))
        dir = MR_CW[dir];
    if (hc_block_is_full_cube(
            hc_feat_get_block(e->rg, x + MR_DX[dir], y, z + MR_DZ[dir])))
        dir = MR_OPP[dir];
    return (uint16_t)(HC_B_CHEST_BASE + MR_CHEST_OFF[dir]);
}

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
    /* --- 성공 경로 (place@264-848; 링 프리픽스에서 실제 발화 — R5d) --- */
    /* 방 셸/내부: x 밖, y 3→-1 내림차순, z 안 (@264-531). dy==4 는 y 범위
     * 밖이라 셸 판정에서 도달 불가. */
    for (int32_t dx = xmin; dx <= xmax; dx++)
        for (int32_t dy = 3; dy >= -1; dy--)
            for (int32_t dz = zmin; dz <= zmax; dz++) {
                int32_t  x = ox + dx, y = oy + dy, z = oz + dz;
                uint16_t st = hc_feat_get_block(e->rg, x, y, z);
                int shell = dx == xmin || dy == -1 || dz == zmin ||
                            dx == xmax || dz == zmax;
                if (!shell) {
                    /* 내부: chest/spawner (겹친 이전 던전) 만 보존 (@480-511) */
                    if (!mr_is_chest(st) && st != HC_B_SPAWNER)
                        mr_safe_set(e, x, y, z, HC_B_CAVE_AIR);
                    continue;
                }
                /* 아래가 비고체면 직접 setBlock(cave_air) — safeSet 아님,
                 * y<minY 는 스킵 (@358-404) */
                if (y >= HC_MIN_Y &&
                    !hc_block_is_solid(
                        hc_feat_get_block(e->rg, x, y - 1, z))) {
                    hc_feat_set_block(e->rg, x, y, z, HC_B_CAVE_AIR);
                    continue;
                }
                if (!hc_block_is_solid(st) || mr_is_chest(st))
                    continue;
                /* 바닥(dy==-1)만 nextInt(4) 드로우: 0 → cobble, 그 외 mossy
                 * (@427-477) */
                if (dy == -1 && hc_wgr_next_int(e->rng, 4) != 0)
                    mr_safe_set(e, x, y, z, HC_B_MOSSY_COBBLESTONE);
                else
                    mr_safe_set(e, x, y, z, HC_B_COBBLESTONE);
            }
    /* 상자 2개 × 시도 3회 (@532-748): 시도당 x,z 드로우 무조건; 공기 +
     * 수평 고체 이웃 정확히 1개면 reorient 된 chest + 전리품 시드
     * nextLong (RandomizableContainer.setBlockEntityLootTable@28 — 상자는
     * 공기 위에만 놓이므로 블록엔티티 존재가 보장된다) 후 다음 상자로. */
    for (int32_t ci = 0; ci < 2; ci++)
        for (int32_t attempt = 0; attempt < 3; attempt++) {
            int32_t px = ox + hc_wgr_next_int(e->rng, j * 2 + 1) - j;
            int32_t pz = oz + hc_wgr_next_int(e->rng, k * 2 + 1) - k;
            if (!hc_block_is_air(hc_feat_get_block(e->rg, px, oy, pz)))
                continue;
            int solids = 0;
            for (int d = 0; d < 4; d++)
                if (hc_block_is_solid(hc_feat_get_block(
                        e->rg, px + MR_DX[d], oy, pz + MR_DZ[d])))
                    solids++;
            if (solids != 1)
                continue;
            mr_safe_set(e, px, oy, pz, mr_reorient_chest(e, px, oy, pz));
            (void)hc_wgr_next_long(e->rng); /* 전리품 테이블 시드 */
            break;
        }
    /* 스포너 (@749-801): safeSet 후 getBlockEntity 가 SpawnerBlockEntity
     * 일 때만 randomEntityId = MOBS[nextInt(4)] 1 드로우 — 즉 최종 블록이
     * 스포너면 드로우 (safeSet 이 이전 던전의 스포너에 막힌 경우 포함;
     * chest/bedrock 에 막히면 드로우 없음). setEntityId 자체는 0 드로우
     * (빈 spawnPotentials → WeightedList.getRandom 셀렉터 null 조기 반환). */
    mr_safe_set(e, ox, oy, oz, HC_B_SPAWNER);
    if (hc_feat_get_block(e->rg, ox, oy, oz) == HC_B_SPAWNER)
        (void)hc_wgr_next_int(e->rng, 4);
    return 1;
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

/* --- selector 본문 (task9b R2/R3) --- */

static int rsel_place(feat_env_t *e, const hc_rsel_cfg_t *c, int32_t x,
                      int32_t y, int32_t z) {
    /* RandomSelectorFeature: 엔트리 순서로 nextFloat < chance, 첫 히트가
     * 배치를 독점; 전부 빗나가면 default (R2) */
    for (int32_t i = 0; i < c->n_entries; i++)
        if (hc_wgr_next_float(e->rng) < c->entries[i].chance)
            return run_nested_pf(e, c->entries[i].pf, x, y, z);
    return run_nested_pf(e, c->dflt, x, y, z);
}

static int rbool_place(feat_env_t *e, const hc_rbool_cfg_t *c, int32_t x,
                       int32_t y, int32_t z) {
    /* RandomBooleanSelectorFeature: nextBoolean = next(1) != 0 (R3) */
    int flag = hc_wgr_next(e->rng, 1) != 0;
    return run_nested_pf(e, flag ? c->on_true : c->on_false, x, y, z);
}

static int srsel_place(feat_env_t *e, const hc_srsel_cfg_t *c, int32_t x,
                       int32_t y, int32_t z) {
    /* SimpleRandomSelectorFeature: nextInt(n) 픽 (R3) */
    int32_t i = hc_wgr_next_int(e->rng, c->n);
    return run_nested_pf(e, c->feats[i], x, y, z);
}

/* --- 본문 디스패치: 1/0 = 확정, -1 = 미구현 --- */

static int tree_place(feat_env_t *e, int32_t x, int32_t y, int32_t z);
static int ftree_place(feat_env_t *e, int32_t x, int32_t y, int32_t z);
static int vpatch_place(feat_env_t *e, const hc_vpatch_cfg_t *c, int32_t x,
                        int32_t y, int32_t z);
static int bcol_place(feat_env_t *e, const hc_bcol_cfg_t *c, int32_t x,
                      int32_t y, int32_t z);
static int mface_place(feat_env_t *e, const hc_mface_cfg_t *c, int32_t x,
                       int32_t y, int32_t z);
static int sblock_place(feat_env_t *e, const hc_sblock_cfg_t *c, int32_t x,
                        int32_t y, int32_t z);
static int vines_place(feat_env_t *e, int32_t x, int32_t y, int32_t z);
static int bamboo_place(feat_env_t *e, const hc_bamboo_cfg_t *c, int32_t x,
                        int32_t y, int32_t z);
static int freeze_place(feat_env_t *e, int32_t x, int32_t y, int32_t z);

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
    case HC_CF_RANDOM_SELECTOR:
        return rsel_place(e, e->pf->cf.rsel, x, y, z);
    case HC_CF_RANDOM_BOOLEAN:
        return rbool_place(e, e->pf->cf.rbool, x, y, z);
    case HC_CF_SIMPLE_RANDOM_SELECTOR:
        return srsel_place(e, e->pf->cf.srsel, x, y, z);
    case HC_CF_TREE:
        return tree_place(e, x, y, z);
    case HC_CF_FALLEN_TREE:
        return ftree_place(e, x, y, z);
    case HC_CF_VEGETATION_PATCH:
        return vpatch_place(e, e->pf->cf.vpatch, x, y, z);
    case HC_CF_BLOCK_COLUMN:
        return bcol_place(e, e->pf->cf.bcol, x, y, z);
    case HC_CF_MULTIFACE_GROWTH:
        return mface_place(e, e->pf->cf.mface, x, y, z);
    case HC_CF_SIMPLE_BLOCK:
        return sblock_place(e, e->pf->cf.sblock, x, y, z);
    case HC_CF_VINES:
        return vines_place(e, x, y, z);
    case HC_CF_BAMBOO:
        return bamboo_place(e, e->pf->cf.bamboo, x, y, z);
    case HC_CF_FREEZE_TOP_LAYER:
        return freeze_place(e, x, y, z);
    case HC_CF_DISK:
        return hc_featx_disk_place(e, e->pf->cf.disk, x, y, z);
    case HC_CF_SEAGRASS:
        return hc_featx_seagrass_place(e, &e->pf->cf.seagrass, x, y, z);
    case HC_CF_LAKE:
        return hc_featx_lake_place(e, e->pf->cf.lake, x, y, z);
    case HC_CF_ROOT_SYSTEM:
        return hc_featx_rootsys_place(e, e->pf->cf.rootsys, x, y, z);
    case HC_CF_GEODE:
        return hc_featx_geode_place(e, e->pf->cf.geode, x, y, z);
    case HC_CF_KELP:
        return hc_featx_kelp_place(e, x, y, z);
    default:
        e->unknown = 1;
        return -1;
    }
}

/* --- 지지면/생존 판정 (R1 §2.5/§5, R4 §1.3) --- */

/* getBlockSupportShape 면 완전 판정 — 완전 큐브만 (잎 제외, R1 §2.5).
 * isFaceSturdy(FULL/CENTER/RIGID), canSupportCenter 가 전부 여기로
 * 축약된다. */
static int support_face_full(uint16_t id) {
    return hc_block_is_full_cube(id);
}

/* VineBlock.isAcceptableNeighbour = MultifaceBlock.canAttachTo:
 * isFaceFull(support) || isFaceFull(COLLISION) — 잎은 support 가 EMPTY
 * 지만 collision 이 완전 큐브라 부착 가능 (R1 §3 표 + R4 §2). */
static int can_attach_to(uint16_t id) {
    return hc_block_is_full_cube(id) || hc_block_is_leaves(id);
}

/* state.canSurvive(level, pos) — 블록별 디스패치 (R1 §5).
 * 나머지 패밀리는 recon 랜딩과 함께 (도달 시 즉사). */
static int can_survive_state(feat_env_t *e, uint16_t s, int32_t x, int32_t y,
                             int32_t z) {
    if (s == HC_B_SHORT_GRASS || s == HC_B_FERN || s == HC_B_POPPY ||
        s == HC_B_DANDELION || s == HC_B_TALL_GRASS_LOWER)
        return mask_test(e->reg->tag_supports_vegetation,
                         hc_feat_get_block(e->rg, x, y - 1, z));
    if (s == HC_B_AZALEA || s == HC_B_FLOWERING_AZALEA)
        return mask_test(e->reg->tag_supports_azalea,
                         hc_feat_get_block(e->rg, x, y - 1, z));
    if (s == HC_B_MOSS_CARPET) /* CarpetBlock: 아래가 비-공기 */
        return !hc_block_is_air(hc_feat_get_block(e->rg, x, y - 1, z));
    if (s == HC_B_SPORE_BLOSSOM) {
        /* canSupportCenter(above, DOWN) && !isWaterAt(pos);
         * #unstable_bottom_center(울타리 문) 은 팔레트에 없다 */
        return support_face_full(hc_feat_get_block(e->rg, x, y + 1, z)) &&
               !hc_block_fluid_is_water(hc_feat_get_block(e->rg, x, y, z));
    }
    if (s >= HC_B_SMALL_DRIPLEAF_BASE &&
        s < HC_B_SMALL_DRIPLEAF_BASE + 16) {
        /* SmallDripleafBlock LOWER: 아래 ∈ {clay, moss_block} ||
         * (위가 소스 물 && 아래 ∈ #supports_vegetation) — R1 §5.
         * (UPPER 하프는 feature 배치 경로에서 안 온다) */
        uint16_t below = hc_feat_get_block(e->rg, x, y - 1, z);
        if (mask_test(e->reg->tag_supports_small_dripleaf, below))
            return 1;
        return hc_block_fluid_is_water(
                   hc_feat_get_block(e->rg, x, y + 1, z)) &&
               mask_test(e->reg->tag_supports_vegetation, below);
    }
    if (s == HC_B_MELON)
        /* 26.2 에서 melon 은 전용 클래스 없이 plain Block 등록
         * (Blocks 2-인자 register → bsm#10 Block::new) —
         * BlockBehaviour.canSurvive 기본값 iconst_1 (항상 생존) */
        return 1;
    die("canSurvive unmapped block state", hc_block_name(s));
    return 0;
}

static int would_survive(feat_env_t *e, const char *name, int32_t x,
                         int32_t y, int32_t z) {
    /* SaplingBlock/FireflyBushBlock 은 VegetationBlock canSurvive 를
     * 오버라이드하지 않는다 (R2 §2 / 본 세션 javap):
     * below.is(#supports_vegetation) */
    if (strcmp(name, "minecraft:oak_sapling") == 0 ||
        strcmp(name, "minecraft:jungle_sapling") == 0 ||
        strcmp(name, "minecraft:firefly_bush") == 0)
        return mask_test(e->reg->tag_supports_vegetation,
                         hc_feat_get_block(e->rg, x, y - 1, z));
    if (strcmp(name, "minecraft:sugar_cane") == 0) {
        /* SugarCaneBlock.canSurvive (본 세션 javap): below.is(자기 블록) —
         * 팔레트에 sugar_cane 없음 → 항상 거짓; below.is(#supports_sugar_
         * cane) 이면 below 의 수평 4방 (HORIZONTAL: N,E,S,W) 중 유체가
         * #fluid:supports_sugar_cane_adjacently(=#water) 이거나 블록이
         * frosted_ice(팔레트 밖) 인 이웃이 있어야 참. */
        uint16_t below = hc_feat_get_block(e->rg, x, y - 1, z);
        if (!mask_test(e->reg->tag_supports_sugar_cane, below))
            return 0;
        static const int8_t H4[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
        for (int i = 0; i < 4; i++)
            if (hc_block_fluid_is_water(hc_feat_get_block(
                    e->rg, x + H4[i][0], y - 1, z + H4[i][1])))
                return 1;
        return 0;
    }
    die("would_survive dispatch unmapped", name);
    return 0;
}

static int tree_place(feat_env_t *e, int32_t x, int32_t y, int32_t z) {
    return hc_featx_tree_place(e, x, y, z);
}
static int ftree_place(feat_env_t *e, int32_t x, int32_t y, int32_t z) {
    return hc_featx_ftree_place(e, x, y, z);
}
static int vpatch_place(feat_env_t *e, const hc_vpatch_cfg_t *c, int32_t x,
                        int32_t y, int32_t z) {
    return hc_featx_vpatch_place(e, c, x, y, z);
}
static int bcol_place(feat_env_t *e, const hc_bcol_cfg_t *c, int32_t x,
                      int32_t y, int32_t z) {
    return hc_featx_bcol_place(e, c, x, y, z);
}
static int mface_place(feat_env_t *e, const hc_mface_cfg_t *c, int32_t x,
                       int32_t y, int32_t z) {
    return hc_featx_mface_place(e, c, x, y, z);
}
/* --- SimpleBlockFeature (R4 §1) --- */

/* DoublePlantBlock 패밀리 (R1 §3 표): tall_grass, small_dripleaf.
 * placeAt 은 하프별로 copyWaterloggedFrom (wl 프로퍼티가 있으면 그
 * 위치의 물 여부로 세팅 — small_dripleaf 만 해당). */
static int double_plant_halves(feat_env_t *e, uint16_t s, int32_t x,
                               int32_t y, int32_t z, uint16_t *lower,
                               uint16_t *upper) {
    if (s == HC_B_TALL_GRASS_LOWER || s == HC_B_TALL_GRASS_UPPER) {
        *lower = HC_B_TALL_GRASS_LOWER;
        *upper = HC_B_TALL_GRASS_UPPER;
        return 1;
    }
    if (s >= HC_B_SMALL_DRIPLEAF_BASE &&
        s < HC_B_SMALL_DRIPLEAF_BASE + 16) {
        int32_t facing = (s - HC_B_SMALL_DRIPLEAF_BASE) / 4;
        int wl_lo =
            hc_block_fluid_is_water(hc_feat_get_block(e->rg, x, y, z));
        int wl_hi =
            hc_block_fluid_is_water(hc_feat_get_block(e->rg, x, y + 1, z));
        *lower =
            (uint16_t)(HC_B_SMALL_DRIPLEAF_BASE + facing * 4 + 0 * 2 + wl_lo);
        *upper =
            (uint16_t)(HC_B_SMALL_DRIPLEAF_BASE + facing * 4 + 1 * 2 + wl_hi);
        return 1;
    }
    return 0;
}

static int sblock_place(feat_env_t *e, const hc_sblock_cfg_t *c, int32_t x,
                        int32_t y, int32_t z) {
    /* 프로바이더 드로우가 canSurvive 보다 먼저 — 거부 위치도 드로우를
     * 태운다 (R4 §1) */
    uint16_t state = sprov_sample(e->rng, &c->to_place);
    if (!can_survive_state(e, state, x, y, z))
        return 0;
    uint16_t lower, upper;
    if (double_plant_halves(e, state, x, y, z, &lower, &upper)) {
        if (!hc_block_is_air(hc_feat_get_block(e->rg, x, y + 1, z)))
            return 0;
        /* DoublePlantBlock.placeAt — lower 먼저, flag 2 (R4 §1.4) */
        hc_feat_set_block(e->rg, x, y, z, lower);
        hc_feat_set_block(e->rg, x, y + 1, z, upper);
        return 1;
    }
    hc_feat_set_block(e->rg, x, y, z, state);
    /* schedule_tick: 우리 config 셋엔 없음 (기본 false) — 블록 바이트
     * 재생에 비활성 */
    return 1;
}

/* --- VinesFeature (R4 §2) — 드로우 0 --- */

static int vines_place(feat_env_t *e, int32_t x, int32_t y, int32_t z) {
    if (!hc_block_is_air(hc_feat_get_block(e->rg, x, y, z)))
        return 0;
    /* Direction.values() 에서 DOWN 제외: UP, NORTH, SOUTH, WEST, EAST.
     * 첫 부착 가능 면이 승리. vine 팔레트 오프셋: E,N,S,U,W. */
    static const struct {
        int8_t dx, dy, dz;
        uint8_t face_off; /* HC_B_VINE_BASE + off */
    } D[5] = {
        {0, 1, 0, 3},  /* UP */
        {0, 0, -1, 1}, /* NORTH */
        {0, 0, 1, 2},  /* SOUTH */
        {-1, 0, 0, 4}, /* WEST */
        {1, 0, 0, 0},  /* EAST */
    };
    for (int i = 0; i < 5; i++) {
        uint16_t nb =
            hc_feat_get_block(e->rg, x + D[i].dx, y + D[i].dy, z + D[i].dz);
        if (can_attach_to(nb)) {
            hc_feat_set_block(e->rg, x, y, z,
                              (uint16_t)(HC_B_VINE_BASE + D[i].face_off));
            return 1;
        }
    }
    return 0;
}

/* --- BambooFeature (R4 §3) --- */

static int bamboo_place(feat_env_t *e, const hc_bamboo_cfg_t *c, int32_t x,
                        int32_t y, int32_t z) {
    /* 정적 상태: TRUNK[age=1,none,0] FINAL_LARGE[age=1,large,1]
     * TOP_LARGE[age=1,large,0] TOP_SMALL[age=1,small,0] */
    const uint16_t TRUNK = HC_B_BAMBOO_BASE + 1 * 6 + 0 * 2 + 0;
    const uint16_t FINAL_LARGE = HC_B_BAMBOO_BASE + 1 * 6 + 2 * 2 + 1;
    const uint16_t TOP_LARGE = HC_B_BAMBOO_BASE + 1 * 6 + 2 * 2 + 0;
    const uint16_t TOP_SMALL = HC_B_BAMBOO_BASE + 1 * 6 + 1 * 2 + 0;
    int placed = 0;
    if (hc_block_is_air(hc_feat_get_block(e->rg, x, y, z))) {
        if (mask_test(e->reg->tag_supports_bamboo,
                      hc_feat_get_block(e->rg, x, y - 1, z))) {
            int32_t height = hc_wgr_next_int(e->rng, 12) + 5; /* DRAW 1 */
            if (hc_wgr_next_float(e->rng) < c->probability) { /* DRAW 2 항상 */
                int32_t rad = hc_wgr_next_int(e->rng, 4) + 1; /* DRAW 3 조건 */
                for (int32_t px = x - rad; px <= x + rad; px++)
                    for (int32_t pz = z - rad; pz <= z + rad; pz++) {
                        int32_t dx = px - x, dz = pz - z;
                        if (dx * dx + dz * dz <= rad * rad) {
                            int32_t sy = hc_feat_height(
                                             e->rg, HC_HM_WORLD_SURFACE, px,
                                             pz) -
                                         1;
                            if (sy >= HC_MIN_Y && sy <= HC_MAX_Y &&
                                mask_test(e->reg->tag_podzol_replaceable,
                                          hc_feat_get_block(e->rg, px, sy,
                                                            pz)))
                                hc_feat_set_block(e->rg, px, sy, pz,
                                                  HC_B_PODZOL);
                        }
                    }
            }
            int32_t cy = y;
            for (int32_t i = 0;
                 i < height && hc_block_is_air(hc_feat_get_block(e->rg, x, cy,
                                                                 z));
                 i++) {
                hc_feat_set_block(e->rg, x, cy, z, TRUNK);
                cy++;
            }
            if (cy - y >= 3) {
                /* 팁 3연타 — FINAL_LARGE 는 루프가 멈춘 위치를 무조건
                 * 덮어쓴다 (R4 §3 3항) */
                hc_feat_set_block(e->rg, x, cy, z, FINAL_LARGE);
                hc_feat_set_block(e->rg, x, cy - 1, z, TOP_LARGE);
                hc_feat_set_block(e->rg, x, cy - 2, z, TOP_SMALL);
            }
        }
        placed++; /* canSurvive 실패/팁 생략과 무관 (R4 §3 4항 quirk) */
    }
    return placed > 0;
}

/* --- SnowAndFreezeFeature (R4 §8 / A5) — 드로우 0 --- */

static int freeze_place(feat_env_t *e, int32_t x, int32_t y, int32_t z) {
    (void)y;
    for (int32_t dx = 0; dx < 16; dx++) {
        for (int32_t dz = 0; dz < 16; dz++) {
            int32_t wx = x + dx, wz = z + dz;
            int32_t hy = hc_feat_height(e->rg, HC_HM_MOTION_BLOCKING, wx, wz);
            uint16_t b = hc_biome_view_get(e->view, wx, hy, wz);
            /* shouldFreeze(below)/shouldSnow(top) 는 둘 다 온도 게이트가
             * 첫 단락 (warmEnoughToRain ≥ 0.15f) — 이 그리드의 4개 바이옴
             * (0.5..0.95) 은 전부 warm 이라 무조건 통과 실패 (A5 수치
             * 증명). 찬 컬럼이 나타나면 얼음/눈 경로 미구현 — 즉사. */
            if (hc_biome_cold_enough_to_snow(e->biomes, b, wx, hy - 1, wz,
                                             e->sea_level) ||
                hc_biome_cold_enough_to_snow(e->biomes, b, wx, hy, wz,
                                             e->sea_level))
                die("freeze_top_layer cold column — ice/snow path "
                    "unimplemented",
                    NULL);
        }
    }
    return 1; /* place() 는 무조건 true (R4 §8) */
}

/* --- 파이프라인 (A2 §1.2 루프 중첩) --- */

static void run_mods(feat_env_t *e, int32_t mi, int32_t x, int32_t y,
                     int32_t z);

static void leaf(feat_env_t *e, int32_t x, int32_t y, int32_t z) {
    /* Feature.place 진입부의 ensureCanWrite(pos) — 창 밖이면 본문 미실행,
     * 드로우 0, false (R3 §3.1; XZ ±1 청크만 검사). 그리드 config 의
     * 파이프라인 위치는 전부 창 안이지만 바닐라 게이트를 그대로 둔다. */
    int32_t placed;
    int32_t pcx = floor_div16(x), pcz = floor_div16(z);
    if (pcx < e->rg->center_cx - 1 || pcx > e->rg->center_cx + 1 ||
        pcz < e->rg->center_cz - 1 || pcz > e->rg->center_cz + 1)
        placed = 0;
    else
        placed = cf_place(e, x, y, z);
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
        int32_t ny = hc_feat_height(e->rg, m->hm_type, x, z);
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
        int64_t h = hc_feat_height(e->rg, m->hm_type, x, z);
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
        if (e->nested)
            die("biome modifier inside nested placed feature "
                "(vanilla IllegalStateException path)",
                e->pf->name);
        uint16_t b = hc_biome_view_get(e->view, x, y, z);
        assert(b < (uint16_t)e->reg->n_biomes);
        const uint64_t *row =
            &e->reg->member[e->step][(size_t)b * (size_t)e->reg->words[e->step]];
        if ((row[e->index >> 6] >> (e->index & 63)) & 1u)
            run_mods(e, mi + 1, x, y, z);
        return;
    }
    case HC_PM_SURFACE_WATER_DEPTH: {
        /* SurfaceWaterDepthFilter: OCEAN_FLOOR/WORLD_SURFACE (LIVE) —
         * 통과 iff WS − OF <= max_water_depth (A2 §0.2 검증) */
        int32_t of = hc_feat_height(e->rg, HC_HM_OCEAN_FLOOR, x, z);
        int32_t ws = hc_feat_height(e->rg, HC_HM_WORLD_SURFACE, x, z);
        if (ws - of <= m->max_incl)
            run_mods(e, mi + 1, x, y, z);
        return;
    }
    case HC_PM_NOISE_THRESHOLD_COUNT: {
        /* Biome.BIOME_INFO_NOISE.getValue(x/200, z/200, false) < level ?
         * below : above — 드로우 0; NaN/동치 → above (A2 검증 dcmpg) */
        double  n = hc_biome_info_noise((double)x / 200.0, (double)z / 200.0);
        int32_t cnt = n < m->noise_level ? m->below_noise : m->above_noise;
        for (int32_t i = 0; i < cnt; i++)
            run_mods(e, mi + 1, x, y, z);
        return;
    }
    case HC_PM_NOISE_BASED_COUNT: {
        /* NoiseBasedCountPlacement.count — 드로우 0:
         * d2i(Math.ceil((BIOME_INFO_NOISE(x/factor, z/factor) + offset)
         * * ratio)); 노이즈 |값| 유계라 d2i 포화/NaN 경로 도달 불가 */
        double n = hc_biome_info_noise((double)x / m->noise_factor,
                                       (double)z / m->noise_factor);
        int32_t cnt =
            (int32_t)ceil((n + m->noise_offset) * (double)m->noise_ratio);
        for (int32_t i = 0; i < cnt; i++)
            run_mods(e, mi + 1, x, y, z);
        return;
    }
    case HC_PM_DIE:
        fprintf(stderr, "hc_features: placed feature %s: %s\n",
                e->pf->name ? e->pf->name : "<inline>",
                m->die_what ? m->die_what : "?");
        die("unsupported placement modifier executed", e->pf->name);
        return;
    }
    die("unknown placement modifier kind", NULL);
}

int hc_featx_run_nested(feat_env_t *e, const hc_pfeat_t *pf, int32_t x,
                        int32_t y, int32_t z) {
    feat_env_t ne = *e;
    ne.pf = pf;
    ne.npos = 0;
    ne.placed_any = 0;
    ne.nested = 1;
    ne.trace = NULL; /* p-라인은 최상위 전용 (FORMAT.md depth-exclusion) */
    run_mods(&ne, 0, x, y, z);
    e->unknown |= ne.unknown;
    return ne.placed_any;
}

/* features_compile.c / gen_features_stage.c 가 쓰는 내부 진입점 */
void hc_feat_run_placed(hc_feat_region_t *rg, hc_wgr_t *rng,
                        int64_t level_seed, const hc_feat_reg_t *reg,
                        const hc_biome_view_t *view,
                        const hc_biome_reg_t *biomes, int32_t sea_level,
                        const hc_pfeat_t *pf, int32_t step, int32_t index,
                        int32_t origin_x, int32_t origin_y, int32_t origin_z,
                        const hc_feat_trace_t *trace) {
    feat_env_t e;
    e.rg = rg;
    e.rng = rng;
    e.level_seed = level_seed;
    e.reg = reg;
    e.view = view;
    e.biomes = biomes;
    e.sea_level = sea_level;
    e.pf = pf;
    e.step = step;
    e.index = index;
    e.trace = trace;
    e.npos = 0;
    e.placed_any = 0;
    e.unknown = 0;
    e.nested = 0;
    run_mods(&e, 0, origin_x, origin_y, origin_z);
    if (trace && trace->on_feature) {
        int32_t placed = e.placed_any ? 1 : (e.unknown ? -1 : 0);
        trace->on_feature(trace->ud, step, index, pf->name, e.npos, placed);
    }
}
