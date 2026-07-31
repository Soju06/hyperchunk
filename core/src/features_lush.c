#include "features_internal.h"

#include <string.h>

/* R3 본문: multiface_growth(glow_lichen) / block_column / vegetation_patch
 * (+waterlogged) — 전부 26.2 바이트코드 재구성
 * (.hermes/notes/task9b-features/R3-lush-caves-bodies.md). 드로우 순서와
 * HashSet 순회 순서가 비트 정확성의 전부다. */

#define die hc_featx_die
#define mask_test hc_featx_mask_test
#define iprov_sample hc_featx_iprov_sample
#define sprov_sample hc_featx_sprov_sample
#define bpred_eval hc_featx_bpred_eval
#define run_nested_pf hc_featx_run_nested

/* MC Direction 서수: 0 DOWN, 1 UP, 2 NORTH, 3 SOUTH, 4 WEST, 5 EAST */
static const int8_t D_STEP[6][3] = {{0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                                    {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};
static const uint8_t D_OPP[6] = {1, 0, 3, 2, 5, 4};
static const uint8_t D_AXIS[6] = {1, 1, 2, 2, 0, 0}; /* 0=X 1=Y 2=Z */
/* glow_lichen facemask 비트 (hc_blocks: down,east,north,south,up,west) */
static const uint8_t D_LICHEN_BIT[6] = {0, 4, 2, 3, 5, 1};

/* Util.shuffle: i = n..2, swap(i-1, nextInt(i)) — n-1 드로우 (R3 §1.2) */
static void util_shuffle(hc_wgr_t *r, uint8_t *arr, int32_t n) {
    for (int32_t i = n; i > 1; i--) {
        int32_t j = hc_wgr_next_int(r, i);
        uint8_t t = arr[i - 1];
        arr[i - 1] = arr[j];
        arr[j] = t;
    }
}

static uint16_t get(feat_env_t *e, int32_t x, int32_t y, int32_t z) {
    return hc_feat_get_block(e->rg, x, y, z);
}

/* ================= multiface_growth (R3 §1) ================= */

enum { MF_NULL = 0xFFFF };

static int is_lichen(uint16_t s) {
    return s >= HC_B_GLOW_LICHEN_BASE && s < HC_B_GLOW_LICHEN_BASE + 126;
}
static int lichen_wl(uint16_t s) {
    return (s - HC_B_GLOW_LICHEN_BASE) / 63;
}
static int lichen_mask(uint16_t s) {
    return (s - HC_B_GLOW_LICHEN_BASE) % 63 + 1;
}

/* isAirOrWater = isAir() || is(Blocks.WATER) — 블록 정체성 (waterlogged
 * 상태는 물 블록이 아니다) */
static int air_or_water(uint16_t s) {
    return hc_block_is_air(s) || s == HC_B_WATER;
}

/* MultifaceBlock.canAttachTo: 지지면 완전 || 충돌면 완전 — 완전 큐브와
 * 잎(충돌 완전) 이 통과 (R1 §2.5/§3) */
static int mf_attach_ok(uint16_t s) {
    return hc_block_is_full_cube(s) || hc_block_is_leaves(s);
}

/* getStateForPlacement(current, pos, dir) — 부적합이면 MF_NULL (R3 §1.5) */
static uint16_t mf_state_for_placement(feat_env_t *e, uint16_t cur, int32_t x,
                                       int32_t y, int32_t z, int dir) {
    int bit = D_LICHEN_BIT[dir];
    if (is_lichen(cur) && ((lichen_mask(cur) >> bit) & 1))
        return MF_NULL; /* 그 면 이미 점유 */
    uint16_t nb = get(e, x + D_STEP[dir][0], y + D_STEP[dir][1],
                      z + D_STEP[dir][2]);
    if (!mf_attach_ok(nb))
        return MF_NULL;
    int mask, wl;
    if (is_lichen(cur)) {
        mask = lichen_mask(cur);
        wl = lichen_wl(cur);
    } else if (hc_block_fluid_is_water(cur)) {
        /* getFluidState().isSourceOfType(WATER) — 팔레트의 물은 소스뿐 */
        mask = 0;
        wl = 1;
    } else {
        mask = 0;
        wl = 0;
    }
    mask |= 1 << bit;
    return hc_block_glow_lichen(mask, wl);
}

/* MultifaceSpreader.spreadFromFaceTowardRandomDirection (R3 §1.6):
 * allShuffled(6) 5 드로우 → 셔플 순으로 [SAME_POSITION, SAME_PLANE,
 * WRAP_AROUND] 시도, 첫 성공에서 쓰고(flag 2) 정지. can_be_placed_on 은
 * 여기서 검사되지 않는다. */
static void mf_spread(feat_env_t *e, uint16_t state, int32_t x, int32_t y,
                      int32_t z, int face) {
    uint8_t dirs[6] = {0, 1, 2, 3, 4, 5};
    util_shuffle(e->rng, dirs, 6);
    for (int i = 0; i < 6; i++) {
        int sd = dirs[i];
        if (D_AXIS[sd] == D_AXIS[face])
            continue;
        /* hasFace(state, face) 는 방금 세팅돼 참; spreadDir 면 보유 시 skip */
        if ((lichen_mask(state) >> D_LICHEN_BIT[sd]) & 1)
            continue;
        for (int t = 0; t < 3; t++) {
            int32_t px, py, pz;
            int     pface;
            if (t == 0) { /* SAME_POSITION */
                px = x;
                py = y;
                pz = z;
                pface = sd;
            } else if (t == 1) { /* SAME_PLANE */
                px = x + D_STEP[sd][0];
                py = y + D_STEP[sd][1];
                pz = z + D_STEP[sd][2];
                pface = face;
            } else { /* WRAP_AROUND */
                px = x + D_STEP[sd][0] + D_STEP[face][0];
                py = y + D_STEP[sd][1] + D_STEP[face][1];
                pz = z + D_STEP[sd][2] + D_STEP[face][2];
                pface = D_OPP[sd];
            }
            uint16_t s = get(e, px, py, pz);
            /* stateCanBeReplaced: air || 자기 블록 || 소스 물 */
            if (!(hc_block_is_air(s) || is_lichen(s) || s == HC_B_WATER))
                continue;
            uint16_t placed = mf_state_for_placement(e, s, px, py, pz, pface);
            if (placed == MF_NULL)
                continue;
            hc_feat_set_block(e->rg, px, py, pz, placed); /* flag 2 */
            return; /* findFirst — 첫 성공에서 전체 정지 */
        }
    }
}

/* placeGrowthIfPossible (R3 §1.4) */
static int mf_place_growth(feat_env_t *e, const hc_mface_cfg_t *c, int32_t x,
                           int32_t y, int32_t z, uint16_t state_at_pos,
                           const uint8_t *dirs, int32_t ndirs) {
    for (int32_t i = 0; i < ndirs; i++) {
        int      d = dirs[i];
        uint16_t nb = get(e, x + D_STEP[d][0], y + D_STEP[d][1],
                          z + D_STEP[d][2]);
        if (!mask_test(c->can_place_on, nb))
            continue;
        uint16_t placed = mf_state_for_placement(e, state_at_pos, x, y, z, d);
        if (placed == MF_NULL)
            return 0; /* null → 헬퍼 전체 중단 (남은 방향 시도 없음) */
        hc_feat_set_block(e->rg, x, y, z, placed); /* flag 3 */
        /* 성공 시에만 드로우 — setBlock 뒤 */
        if (hc_wgr_next_float(e->rng) < c->chance_of_spreading)
            mf_spread(e, placed, x, y, z, d);
        return 1;
    }
    return 0;
}

int hc_featx_mface_place(feat_env_t *e, const hc_mface_cfg_t *c, int32_t x,
                         int32_t y, int32_t z) {
    if (!air_or_water(get(e, x, y, z)))
        return 0; /* 드로우 0 */
    /* validDirections: ceiling→UP, floor→DOWN, wall→HORIZONTAL(N,E,S,W) */
    uint8_t valid[6];
    int32_t nvalid = 0;
    if (c->can_place_on_ceiling)
        valid[nvalid++] = 1;
    if (c->can_place_on_floor)
        valid[nvalid++] = 0;
    if (c->can_place_on_wall) {
        valid[nvalid++] = 2; /* NORTH */
        valid[nvalid++] = 5; /* EAST */
        valid[nvalid++] = 3; /* SOUTH */
        valid[nvalid++] = 4; /* WEST */
    }
    uint8_t list[6];
    memcpy(list, valid, (size_t)nvalid);
    util_shuffle(e->rng, list, nvalid);
    if (mf_place_growth(e, c, x, y, z, get(e, x, y, z), list, nvalid))
        return 1;
    for (int32_t i = 0; i < nvalid; i++) {
        int dir = list[i];
        /* getShuffledDirectionsExcept(dir.opposite): 원본 순서 필터 → 셔플.
         * 검사 전 무조건 드로우. */
        uint8_t l2[6];
        int32_t n2 = 0;
        for (int32_t k = 0; k < nvalid; k++)
            if (valid[k] != D_OPP[dir])
                l2[n2++] = valid[k];
        util_shuffle(e->rng, l2, n2);
        for (int32_t step = 0; step < c->search_range; step++) {
            /* setWithOffset(origin, dir) — 커서는 전진하지 않는다 (R3 §1.3) */
            int32_t px = x + D_STEP[dir][0];
            int32_t py = y + D_STEP[dir][1];
            int32_t pz = z + D_STEP[dir][2];
            uint16_t s = get(e, px, py, pz);
            if (!air_or_water(s) && !is_lichen(s))
                break;
            if (mf_place_growth(e, c, px, py, pz, s, l2, n2))
                return 1;
        }
    }
    return 0;
}

/* ================= block_column (R3 §2) ================= */

enum { BCOL_MAX_LAYERS = 8 };

int hc_featx_bcol_place(feat_env_t *e, const hc_bcol_cfg_t *c, int32_t ox,
                        int32_t oy, int32_t oz) {
    if (c->n_layers > BCOL_MAX_LAYERS)
        die("block_column too many layers", e->pf->name);
    int32_t heights[BCOL_MAX_LAYERS];
    int32_t total = 0;
    /* 전 레이어 높이를 레이어 순서로 선샘플 */
    for (int32_t k = 0; k < c->n_layers; k++) {
        heights[k] = iprov_sample(e->rng, &c->layers[k].height);
        total += heights[k];
    }
    if (total == 0)
        return 0; /* 유일한 false 경로 */
    /* 스캔: origin+dir*1 .. origin+dir*total (드로우 0). OOB 읽기 = AIR
     * (VOID_AIR — #air 매치라 스캔이 경계를 통과한다, R3 §2.1) */
    int32_t sy = oy + c->dir_dy;
    int32_t l = 0;
    for (; l < total; l++) {
        if (!bpred_eval(e, &c->allowed, ox, sy, oz))
            break;
        sy += c->dir_dy;
    }
    if (l < total) {
        /* truncate (R3 §2.2) */
        int32_t excess = total - l;
        if (c->prioritize_tip) {
            for (int32_t idx = 0; idx < c->n_layers && excess > 0; idx++) {
                int32_t red = heights[idx] < excess ? heights[idx] : excess;
                excess -= red;
                heights[idx] -= red;
            }
        } else {
            for (int32_t idx = c->n_layers - 1; idx >= 0 && excess > 0;
                 idx--) {
                int32_t red = heights[idx] < excess ? heights[idx] : excess;
                excess -= red;
                heights[idx] -= red;
            }
        }
    }
    /* 쓰기: origin 부터 연속, 블록마다 프로바이더 드로우 → setBlock flag 2
     * (OOB 쓰기는 no-op — 드로우는 그래도 태운다) */
    int32_t wy = oy;
    for (int32_t li = 0; li < c->n_layers; li++) {
        int32_t h = heights[li];
        if (h == 0)
            continue;
        for (int32_t m = 0; m < h; m++) {
            uint16_t st = sprov_sample(e->rng, &c->layers[li].prov);
            hc_feat_set_block(e->rg, ox, wy, oz, st);
            wy += c->dir_dy;
        }
    }
    return 1; /* 0 블록으로 잘려도 true */
}

/* ================= vegetation_patch (R3 §3) ================= */

/* WATERLOGGED 프로퍼티 보유 상태의 wl=true 변형; 프로퍼티 없으면 MF_NULL,
 * 이미 true 면 자기 자신 */
static uint16_t with_waterlogged_true(uint16_t s) {
    if (s >= HC_B_OAK_LEAVES_BASE && s < HC_B_OAK_LEAVES_BASE + 28) {
        return ((s - HC_B_OAK_LEAVES_BASE) % 14) >= 7 ? s : (uint16_t)(s + 7);
    }
    if (is_lichen(s))
        return lichen_wl(s) ? s : (uint16_t)(s + 63);
    if (s >= HC_B_BIG_DRIPLEAF_BASE && s < HC_B_BIG_DRIPLEAF_BASE + 8)
        return ((s - HC_B_BIG_DRIPLEAF_BASE) & 1) ? s : (uint16_t)(s + 1);
    if (s >= HC_B_BIG_DRIPLEAF_STEM_BASE &&
        s < HC_B_BIG_DRIPLEAF_STEM_BASE + 8)
        return ((s - HC_B_BIG_DRIPLEAF_STEM_BASE) & 1) ? s : (uint16_t)(s + 1);
    if (s >= HC_B_SMALL_DRIPLEAF_BASE && s < HC_B_SMALL_DRIPLEAF_BASE + 16)
        return ((s - HC_B_SMALL_DRIPLEAF_BASE) & 1) ? s : (uint16_t)(s + 1);
    return MF_NULL;
}

/* isExposed: N,E,S,W,DOWN 단락 — 이웃의 마주보는 면이 sturdy(FULL) 가
 * 아니면 노출 (R3 §3.2; 잎은 sturdy 아님 → 완전 큐브 판정) */
static int wv_exposed(feat_env_t *e, int32_t x, int32_t y, int32_t z) {
    static const int8_t N5[5][3] = {
        {0, 0, -1}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}, {0, -1, 0}};
    for (int i = 0; i < 5; i++)
        if (!hc_block_is_full_cube(
                get(e, x + N5[i][0], y + N5[i][1], z + N5[i][2])))
            return 1;
    return 0;
}

int hc_featx_vpatch_place(feat_env_t *e, const hc_vpatch_cfg_t *c, int32_t ox,
                          int32_t oy, int32_t oz) {
    int32_t xr = iprov_sample(e->rng, &c->xz_radius) + 1; /* X 먼저 */
    int32_t zr = iprov_sample(e->rng, &c->xz_radius) + 1;
    int32_t dir = c->surface_ceiling ? 1 : -1; /* surface 방향의 dy */

    static hc_jset_t ground, water;
    hc_jset_init(&ground);

    /* placeGroundPatch — x 외측/z 내측 오름차순 (R3 §3.1) */
    for (int32_t x = -xr; x <= xr; x++) {
        int xedge = (x == -xr || x == xr);
        for (int32_t z = -zr; z <= zr; z++) {
            int zedge = (z == -zr || z == zr);
            if (xedge && zedge)
                continue; /* 모서리: 드로우 0 */
            if (xedge || zedge) {
                if (c->extra_edge_chance == 0.0f)
                    continue; /* 드로우 0 */
                if (hc_wgr_next_float(e->rng) > c->extra_edge_chance)
                    continue; /* 유지 조건은 f <= chance (포함) */
            }
            int32_t mx = ox + x, mz = oz + z, my = oy;
            for (int32_t k = 0;
                 hc_block_is_air(get(e, mx, my, mz)) && k < c->vertical_range;
                 k++)
                my += dir;
            for (int32_t k = 0;
                 !hc_block_is_air(get(e, mx, my, mz)) && k < c->vertical_range;
                 k++)
                my -= dir;
            int32_t  sy = my + dir; /* mutable2: 표면 첫 블록 */
            uint16_t surf = get(e, mx, sy, mz);
            if (!hc_block_is_air(get(e, mx, my, mz)) ||
                !hc_block_is_full_cube(surf)) /* isFaceSturdy(FULL) */
                continue;
            int32_t depth = iprov_sample(e->rng, &c->depth);
            if (c->extra_bottom_chance > 0.0f &&
                hc_wgr_next_float(e->rng) < c->extra_bottom_chance)
                depth += 1;
            int32_t ty = sy; /* topPos — placeGround 전에 캡처 */
            /* placeGround (R3 §3.1) */
            int     placed = 1;
            int32_t gy = sy;
            for (int32_t i = 0; i < depth; i++) {
                uint16_t st = sprov_sample(e->rng, &c->ground);
                uint16_t cur = get(e, mx, gy, mz);
                if (cur == st)
                    continue; /* 같은 블록: 쓰기/이동 없음, i 는 증가 */
                if (!mask_test(c->replaceable, cur)) {
                    placed = (i != 0);
                    break;
                }
                hc_feat_set_block(e->rg, mx, gy, mz, st); /* flag 2 */
                gy += dir;
            }
            if (placed)
                hc_jset_add(&ground, mx, ty, mz);
        }
    }

    hc_jset_t *veg = &ground;
    if (c->waterlogged) {
        /* WaterloggedVegetationPatchFeature.placeGroundPatch (R3 §3.2) */
        hc_jset_init(&water);
        hc_jit_t it;
        for (hc_jit_begin(&it, &ground); hc_jit_valid(&it); hc_jit_next(&it)) {
            const hc_jent_t *en = &ground.ent[it.e];
            if (!wv_exposed(e, en->x, en->y, en->z))
                hc_jset_add(&water, en->x, en->y, en->z);
        }
        for (hc_jit_begin(&it, &water); hc_jit_valid(&it); hc_jit_next(&it)) {
            const hc_jent_t *en = &water.ent[it.e];
            hc_feat_set_block(e->rg, en->x, en->y, en->z, HC_B_WATER);
        }
        veg = &water;
    }

    /* distributeVegetation — HashSet 순회 순서 (R3 §3.1) */
    if (c->veg_chance > 0.0f) {
        hc_jit_t it;
        for (hc_jit_begin(&it, veg); hc_jit_valid(&it); hc_jit_next(&it)) {
            const hc_jent_t *en = &veg->ent[it.e];
            if (!(hc_wgr_next_float(e->rng) < c->veg_chance))
                continue;
            if (!c->waterlogged) {
                /* pos.relative(dir.getOpposite()) */
                run_nested_pf(e, c->vegetation, en->x, en->y - dir, en->z);
            } else {
                /* placeVegetation(pos.below()) → 초목은 물 위치 자체에서
                 * 실행; 성공 시 wl=false 상태를 wl=true 로 재기록 */
                int ok = run_nested_pf(e, c->vegetation, en->x, en->y, en->z);
                if (ok) {
                    uint16_t s = get(e, en->x, en->y, en->z);
                    uint16_t w = with_waterlogged_true(s);
                    if (w != MF_NULL && w != s)
                        hc_feat_set_block(e->rg, en->x, en->y, en->z, w);
                }
            }
        }
    }
    return veg->size > 0;
}
