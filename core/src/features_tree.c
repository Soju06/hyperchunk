#include "hc_carvers.h" /* hc_mth_sin/cos (Mth 테이블 — mega jungle 가지) */
#include "features_internal.h"
#include "hc_jdk_trig.h" /* fancy trunk 의 Math.sin/cos (JDK 인트린식) */
#include "hc_structures.h" /* hc_state_parse/build (T14 엣지 패밀리) */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* TreeFeature + trunk/foliage placer + decorator + FallenTreeFeature —
 * 전부 26.2 바이트코드 재구성 (.hermes/notes/task9b-features/R2). 드로우
 * 순서/반복 순서/자바 HashSet 순회 순서가 비트 정확성의 전부다. */

#define die hc_featx_die
#define mask_test hc_featx_mask_test
#define iprov_sample hc_featx_iprov_sample
#define sprov_sample hc_featx_sprov_sample

/* Java HashSet<BlockPos> 에뮬레이션은 features_internal.h 로 이동
 * (R3 vegetation_patch 도 같은 순회 순서를 소비한다). 로컬 별칭만 유지. */
#define jset_t hc_jset_t
#define jent_t hc_jent_t
#define jit_t hc_jit_t
#define jset_init hc_jset_init
#define jset_add hc_jset_add
#define jset_poll_first hc_jset_poll_first
#define jit_begin hc_jit_begin
#define jit_valid hc_jit_valid
#define jit_next hc_jit_next
#define JSET_MAX_ENTRIES HC_JSET_MAX_ENTRIES

/* ================= 공용 헬퍼 (R2) ================= */

/* Mth.floor */
static int32_t mth_floor_d(double v) {
    int32_t i = (int32_t)v;
    return v < (double)i ? i - 1 : i;
}
static int32_t mth_floor_f(float v) {
    int32_t i = (int32_t)v;
    return v < (float)i ? i - 1 : i;
}

static int valid_tree_pos(feat_env_t *e, int32_t x, int32_t y, int32_t z) {
    uint16_t s = hc_feat_get_block(e->rg, x, y, z);
    return hc_block_is_air(s) ||
           mask_test(e->reg->tag_replaceable_by_trees, s);
}

static int is_free_pos(feat_env_t *e, int32_t x, int32_t y, int32_t z) {
    uint16_t s = hc_feat_get_block(e->rg, x, y, z);
    return hc_block_is_air(s) ||
           mask_test(e->reg->tag_replaceable_by_trees, s) ||
           mask_test(e->reg->tag_logs, s);
}

/* is(Blocks.VINE) — 상태 무관 블록 판정. 월드젠 배치는 단면 상태만
 * (VINE_BASE 5종) 이지만 T14 확장 상태 (다면) 도 방어적으로 커버. */
static int is_vine_state(uint16_t s) {
    if (s >= HC_B_VINE_BASE && s < HC_B_VINE_BASE + 5)
        return 1;
    return s >= HC_B_T14_BASE &&
           strncmp(hc_block_name(s), "minecraft:vine[", 15) == 0;
}

static int is_vine_pos(feat_env_t *e, int32_t x, int32_t y, int32_t z) {
    return is_vine_state(hc_feat_get_block(e->rg, x, y, z));
}

/* isOverSolidGround = 아래 블록 isFaceSturdy(UP) — support shape (잎 제외,
 * azalea 상부 슬랩 포함) */
static int over_solid_ground(feat_env_t *e, int32_t x, int32_t y, int32_t z) {
    return hc_featx_face_sturdy_full(hc_feat_get_block(e->rg, x, y - 1, z),
                                     1);
}

/* 나무 실행 상태: 4개 포지션 셋 + setter 들 */
typedef struct {
    feat_env_t *e;
    const hc_tree_cfg_t *cfg;
    jset_t roots, logs, leaves, deco;
} tree_ctx_t;

/* BiConsumer: 셋에 추가(항상) + setBlock flag 19 (창 밖이면 soft-fail —
 * 셋에는 남는다, R2 §3) */
static void t_set(tree_ctx_t *t, jset_t *set, int32_t x, int32_t y, int32_t z,
                  uint16_t state) {
    jset_add(set, x, y, z);
    hc_feat_set_block_ks(t->e->rg, x, y, z, state); /* flag 19 */
}

/* placeLog: validTreePos → trunk state 쓰기(logs 셋), true */
static int place_log_axis(tree_ctx_t *t, int32_t x, int32_t y, int32_t z,
                          uint16_t state) {
    if (!valid_tree_pos(t->e, x, y, z))
        return 0;
    t_set(t, &t->logs, x, y, z, state);
    return 1;
}
static int place_log(tree_ctx_t *t, int32_t x, int32_t y, int32_t z) {
    return place_log_axis(t, x, y, z, t->cfg->trunk_state);
}

/* placeBelowTrunkBlock: rule_based(getOptionalState) — 규칙 실패 시 무기록 */
static void place_below_trunk(tree_ctx_t *t, int32_t x, int32_t y, int32_t z) {
    uint16_t cur = hc_feat_get_block(t->e->rg, x, y, z);
    if (!mask_test(t->cfg->below_not_mask, cur))
        t_set(t, &t->logs, x, y, z, t->cfg->below_state);
}

/* 로그 상태의 axis 변형 (jungle/oak log — enum 레이아웃 x,y,z) */
static uint16_t log_with_axis(uint16_t axis_y_state, int axis /*0=x,1=y,2=z*/) {
    /* trunk_state 는 …_LOG_Y — 베이스는 -1 */
    return (uint16_t)(axis_y_state - 1 + axis);
}

/* getLogAxis(from, to) — R2 §7 */
static int log_axis_for(int32_t fx, int32_t fz, int32_t tx, int32_t tz) {
    int32_t dx = tx - fx;
    if (dx < 0)
        dx = -dx;
    int32_t dz = tz - fz;
    if (dz < 0)
        dz = -dz;
    int32_t m = dx > dz ? dx : dz;
    if (m > 0)
        return dx == m ? 0 : 2;
    return 1;
}

/* ================= foliage (R2 §6) ================= */

/* tryPlaceLeaf: persistent-잎 거부 → validTreePos → 잎 쓰기 (wl 은 소스 물) */
static int try_place_leaf(tree_ctx_t *t, int32_t x, int32_t y, int32_t z) {
    uint16_t cur = hc_feat_get_block(t->e->rg, x, y, z);
    /* getValueOrElse(PERSISTENT,false): 우리 팔레트의 잎은 전부
     * persistent=false 상태만 존재 → 항상 false */
    if (!valid_tree_pos(t->e, x, y, z))
        return 0;
    /* foliageProvider.getState — 게이트 통과 후에만 샘플 (weighted 는
     * 조건부 1 드로우, R5c §6.2; simple 은 0 드로우라 순서 무해) */
    uint16_t st = sprov_sample(t->e->rng, &t->cfg->foliage);
    /* 잎은 WATERLOGGED 보유 — isSourceOfType(WATER) */
    if (hc_block_fluid_is_water(cur))
        st = (uint16_t)(st + 7); /* wl 변형 (+7, hc_blocks 레이아웃) */
    t_set(t, &t->leaves, x, y, z, st);
    return 1;
}

/* shouldSkipLocation 디스패치 — (lx, yOff, lz, range, large) */
static int fol_should_skip(tree_ctx_t *t, int32_t lx, int32_t y, int32_t lz,
                           int32_t range, int large) {
    feat_env_t *e = t->e;
    switch (t->cfg->fol_kind) {
    case HC_FOL_BLOB:
        return lx == range && lz == range &&
               (hc_wgr_next_int(e->rng, 2) == 0 || y == 0);
    case HC_FOL_BUSH:
        return lx == range && lz == range && hc_wgr_next_int(e->rng, 2) == 0;
    case HC_FOL_MEGA_JUNGLE:
        return lx + lz >= 7 || lx * lx + lz * lz > range * range;
    case HC_FOL_FANCY: {
        float fx = (float)lx + 0.5f, fz = (float)lz + 0.5f;
        return fx * fx + fz * fz > (float)(range * range);
    }
    case HC_FOL_ACACIA:
        /* AcaciaFoliagePlacer.shouldSkipLocation (26.2, Task 14):
         * y==0 → (dx>1||dz>1) && dx!=0 && dz!=0;
         * else → dx==range && dz==range && range>0. 드로우 0. */
        if (y == 0)
            return (lx > 1 || lz > 1) && lx != 0 && lz != 0;
        return lx == range && lz == range && range > 0;
    }
    die("unknown foliage placer kind", NULL);
    (void)large;
    return 0;
}

static void place_leaves_row(tree_ctx_t *t, int32_t px, int32_t py, int32_t pz,
                             int32_t range, int32_t y_off, int large) {
    int32_t bound = range + (large ? 1 : 0);
    for (int32_t dx = -range; dx <= bound; dx++)
        for (int32_t dz = -range; dz <= bound; dz++) {
            int32_t lx, lz;
            if (large) {
                int32_t a = dx < 0 ? -dx : dx;
                int32_t b = dx - 1 < 0 ? -(dx - 1) : dx - 1;
                lx = a < b ? a : b;
                a = dz < 0 ? -dz : dz;
                b = dz - 1 < 0 ? -(dz - 1) : dz - 1;
                lz = a < b ? a : b;
            } else {
                lx = dx < 0 ? -dx : dx;
                lz = dz < 0 ? -dz : dz;
            }
            if (fol_should_skip(t, lx, y_off, lz, range, large))
                continue;
            try_place_leaf(t, px + dx, py + y_off, pz + dz);
        }
}

typedef struct {
    int32_t x, y, z;
    int32_t radius_offset;
    uint8_t double_trunk;
} attach_t;

static void create_foliage(tree_ctx_t *t, const attach_t *att, int32_t fh,
                           int32_t rad) {
    feat_env_t *e = t->e;
    /* public 진입: offset = offset.sample (우리 config 전부 상수 → 0 드로우) */
    int32_t offset = iprov_sample(e->rng, &t->cfg->fol_offset);
    switch (t->cfg->fol_kind) {
    case HC_FOL_BLOB:
        for (int32_t l = offset; l >= offset - fh; l--) {
            int32_t range = rad + att->radius_offset - 1 - l / 2;
            if (range < 0)
                range = 0;
            place_leaves_row(t, att->x, att->y, att->z, range, l,
                             att->double_trunk);
        }
        return;
    case HC_FOL_BUSH:
        for (int32_t l = offset; l >= offset - fh; l--) {
            int32_t range = rad + att->radius_offset - 1 - l;
            place_leaves_row(t, att->x, att->y, att->z, range, l,
                             att->double_trunk);
        }
        return;
    case HC_FOL_MEGA_JUNGLE: {
        int32_t rows = att->double_trunk
                           ? fh
                           : 1 + hc_wgr_next_int(e->rng, 2); /* DRAW */
        for (int32_t l = offset; l >= offset - rows; l--) {
            int32_t range = rad + att->radius_offset + 1 - l;
            place_leaves_row(t, att->x, att->y, att->z, range, l,
                             att->double_trunk);
        }
        return;
    }
    case HC_FOL_FANCY:
        for (int32_t l = offset; l >= offset - fh; l--) {
            int32_t range =
                rad + ((l == offset || l == offset - fh) ? 0 : 1);
            place_leaves_row(t, att->x, att->y, att->z, range, l,
                             att->double_trunk);
        }
        return;
    case HC_FOL_RANDOM_SPREAD: {
        /* RandomSpreadFoliagePlacer.createFoliage@0-102 (R5c §6.1):
         * 시도당 6 드로우 (x,y,z 순 nextInt(r)−nextInt(r) 삼각형), 오프셋은
         * 항상 고정 attachment 기준 (비누적). 위에서 샘플한 offset 값은 이
         * placer 가 쓰지 않는다 (드로우만 유효 — 상수라 0). C 피연산자
         * 평가 순서는 미지정이라 드로우를 명시적으로 순서화한다. */
        for (int32_t a = 0; a < t->cfg->leaf_attempts; a++) {
            int32_t x1 = hc_wgr_next_int(e->rng, rad);
            int32_t x2 = hc_wgr_next_int(e->rng, rad);
            int32_t y1 = hc_wgr_next_int(e->rng, fh);
            int32_t y2 = hc_wgr_next_int(e->rng, fh);
            int32_t z1 = hc_wgr_next_int(e->rng, rad);
            int32_t z2 = hc_wgr_next_int(e->rng, rad);
            try_place_leaf(t, att->x + (x1 - x2), att->y + (y1 - y2),
                           att->z + (z1 - z2));
        }
        return;
    }
    case HC_FOL_ACACIA: {
        /* AcaciaFoliagePlacer.createFoliage (26.2, Task 14): fh = 0
         * (foliageHeight 오버라이드). 3 행:
         *   (r + attRadOff,     -1 - fh)
         *   (r - 1,             -fh)
         *   (r + attRadOff - 1,  0)
         * 오프셋은 attachment.pos.above(offset) — offset 샘플은 공통
         * 경로에서 이미 소비됨 (상수 0). */
        int32_t ao = att->radius_offset;
        place_leaves_row(t, att->x, att->y + offset, att->z, rad + ao,
                         -1 - fh, att->double_trunk);
        place_leaves_row(t, att->x, att->y + offset, att->z, rad - 1, -fh,
                         att->double_trunk);
        place_leaves_row(t, att->x, att->y + offset, att->z, rad + ao - 1, 0,
                         att->double_trunk);
        return;
    }
    }
    die("unknown foliage placer kind", NULL);
}

/* ================= trunk placers (R2 §5/§7) ================= */

enum { MAX_ATTACH = 48 };

/* straight: 아래 dirt → free 만큼 로그 → attachment(top, 0, false) */
static int32_t trunk_straight(tree_ctx_t *t, int32_t x, int32_t y, int32_t z,
                              int32_t free, attach_t *out) {
    place_below_trunk(t, x, y - 1, z);
    for (int32_t i = 0; i < free; i++)
        place_log(t, x, y + i, z);
    out[0] = (attach_t){x, y + free, z, 0, 0};
    return 1;
}

/* giant (mega jungle 베이스) */
static int32_t trunk_giant(tree_ctx_t *t, int32_t x, int32_t y, int32_t z,
                           int32_t free, attach_t *out) {
    place_below_trunk(t, x, y - 1, z);
    place_below_trunk(t, x + 1, y - 1, z);
    place_below_trunk(t, x, y - 1, z + 1);
    place_below_trunk(t, x + 1, y - 1, z + 1);
    for (int32_t i = 0; i < free; i++) {
        if (is_free_pos(t->e, x, y + i, z))
            place_log(t, x, y + i, z);
        if (i < free - 1) {
            if (is_free_pos(t->e, x + 1, y + i, z))
                place_log(t, x + 1, y + i, z);
            if (is_free_pos(t->e, x + 1, y + i, z + 1))
                place_log(t, x + 1, y + i, z + 1);
            if (is_free_pos(t->e, x, y + i, z + 1))
                place_log(t, x, y + i, z + 1);
        }
    }
    out[0] = (attach_t){x, y + free, z, 0, 1};
    return 1;
}

static int32_t trunk_mega_jungle(tree_ctx_t *t, int32_t x, int32_t y,
                                 int32_t z, int32_t free, attach_t *out) {
    feat_env_t *e = t->e;
    int32_t n = trunk_giant(t, x, y, z, free, out);
    int32_t by = free - 2 - hc_wgr_next_int(e->rng, 4); /* DRAW */
    while (by > free / 2) {
        float f = hc_wgr_next_float(e->rng) * 2.0f; /* DRAW */
        double angle = (double)f * 3.141592653589793;
        int32_t dx = 0, dz = 0;
        for (int32_t l = 0; l < 5; l++) {
            /* Mth 테이블 sin/cos (float) — f2i 절단 */
            dx = (int32_t)(1.5f + hc_mth_cos(angle) * (float)l);
            dz = (int32_t)(1.5f + hc_mth_sin(angle) * (float)l);
            place_log(t, x + dx, y + by - 3 + l / 2, z + dz);
        }
        if (n >= MAX_ATTACH)
            die("attachment overflow (mega jungle)", NULL);
        out[n++] = (attach_t){x + dx, y + by, z + dz, -2, 0};
        by -= 2 + hc_wgr_next_int(e->rng, 4); /* DRAW */
    }
    return n;
}

/* fancy (R2 §7) */
typedef struct {
    attach_t att;
    int32_t  branch_base;
} fancy_fc_t;

static int fancy_make_limb(tree_ctx_t *t, int32_t fx, int32_t fy, int32_t fz,
                           int32_t tx, int32_t ty, int32_t tz, int place) {
    if (!place && fx == tx && fy == ty && fz == tz)
        return 1;
    int32_t dx = tx - fx, dy = ty - fy, dz = tz - fz;
    int32_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy,
            adz = dz < 0 ? -dz : dz;
    int32_t steps = adx > ady ? adx : ady;
    if (adz > steps)
        steps = adz;
    float sx = (float)dx / (float)steps;
    float sy = (float)dy / (float)steps;
    float sz = (float)dz / (float)steps;
    for (int32_t i = 0; i <= steps; i++) {
        int32_t px = fx + mth_floor_f(0.5f + (float)i * sx);
        int32_t py = fy + mth_floor_f(0.5f + (float)i * sy);
        int32_t pz = fz + mth_floor_f(0.5f + (float)i * sz);
        if (place) {
            int axis = log_axis_for(fx, fz, px, pz);
            place_log_axis(t, px, py, pz,
                           log_with_axis(t->cfg->trunk_state, axis));
        } else if (!is_free_pos(t->e, px, py, pz)) {
            return 0;
        }
    }
    return 1;
}

static float fancy_tree_shape(int32_t j, int32_t y) {
    if ((float)y < (float)j * 0.3f)
        return -1.0f;
    float f = (float)j / 2.0f;
    float f1 = f - (float)y;
    float f2 = (float)sqrt((double)(f * f - f1 * f1)); /* Mth.sqrt */
    if (f1 == 0.0f)
        f2 = f;
    else if (fabsf(f1) >= f)
        return 0.0f;
    return f2 * 0.5f;
}

static int fancy_trim(int32_t j, int32_t y) {
    return (double)y >= (double)j * 0.2;
}

static int32_t trunk_fancy(tree_ctx_t *t, int32_t x, int32_t y, int32_t z,
                           int32_t free, attach_t *out) {
    feat_env_t *e = t->e;
    int32_t j = free + 2;
    int32_t k = mth_floor_d((double)j * 0.618);
    place_below_trunk(t, x, y - 1, z);
    /* clusters = min(1, floor(1.382 + (j/13)^2)) ≡ 1 (floor 인자 >= 1.382) */
    int32_t i1 = y + k;
    static fancy_fc_t fcs[128];
    int32_t n_fc = 0;
    fcs[n_fc++] = (fancy_fc_t){{x, y + (j - 5), z, 0, 0}, i1};
    for (int32_t j1 = j - 5; j1 >= 0; j1--) {
        float f = fancy_tree_shape(j, j1);
        if (f < 0.0f)
            continue;
        /* clusters == 1 */
        double d0 =
            1.0 * (double)f * ((double)hc_wgr_next_float(e->rng) + 0.328);
        double d1 =
            (double)(hc_wgr_next_float(e->rng) * 2.0f) * 3.141592653589793;
        double d2 = d0 * hc_jdk_sin(d1) + 0.5;
        double d3 = d0 * hc_jdk_cos(d1) + 0.5;
        int32_t p1x = x + mth_floor_d(d2), p1y = y + j1 - 1,
                p1z = z + mth_floor_d(d3);
        if (fancy_make_limb(t, p1x, p1y, p1z, p1x, p1y + 5, p1z, 0)) {
            int32_t ddx = x - p1x, ddz = z - p1z;
            double  d4 = (double)p1y -
                        sqrt((double)(ddx * ddx + ddz * ddz)) * 0.381;
            int32_t bb = d4 > (double)i1 ? i1 : (int32_t)d4;
            if (fancy_make_limb(t, x, bb, z, p1x, p1y, p1z, 0)) {
                if (n_fc >= 128)
                    die("fancy foliage coords overflow", NULL);
                fcs[n_fc++] = (fancy_fc_t){{p1x, p1y, p1z, 0, 0}, bb};
            }
        }
    }
    fancy_make_limb(t, x, y, z, x, y + k, z, 1);
    /* makeBranches */
    for (int32_t i = 0; i < n_fc; i++) {
        int32_t bb = fcs[i].branch_base;
        if (!(x == fcs[i].att.x && bb == fcs[i].att.y && z == fcs[i].att.z) &&
            fancy_trim(j, bb - y))
            fancy_make_limb(t, x, bb, z, fcs[i].att.x, fcs[i].att.y,
                            fcs[i].att.z, 1);
    }
    int32_t n = 0;
    for (int32_t i = 0; i < n_fc; i++)
        if (fancy_trim(j, fcs[i].branch_base - y)) {
            if (n >= MAX_ATTACH)
                die("attachment overflow (fancy)", NULL);
            out[n++] = fcs[i].att;
        }
    return n;
}

/* ================= 데코레이터 (R2 §11) ================= */

/* Context 리스트: jset 순회 순서로 배열화 후 getY 안정 정렬 */
typedef struct {
    int32_t x, y, z;
} cpos_t;

static int32_t ctx_list(const jset_t *s, cpos_t *out, int32_t cap) {
    int32_t n = 0;
    jit_t   it;
    for (jit_begin(&it, s); jit_valid(&it); jit_next(&it)) {
        if (n >= cap)
            die("context list overflow", NULL);
        out[n].x = s->ent[it.e].x;
        out[n].y = s->ent[it.e].y;
        out[n].z = s->ent[it.e].z;
        n++;
    }
    /* 안정 삽입 정렬 by y (n 은 수백 — fastutil 안정 mergesort 와 결과 동일) */
    for (int32_t i = 1; i < n; i++) {
        cpos_t  v = out[i];
        int32_t k = i - 1;
        while (k >= 0 && out[k].y > v.y) {
            out[k + 1] = out[k];
            k--;
        }
        out[k + 1] = v;
    }
    return n;
}

static int ctx_is_air(feat_env_t *e, int32_t x, int32_t y, int32_t z) {
    return hc_block_is_air(hc_feat_get_block(e->rg, x, y, z));
}

/* Context.placeVine — deco 셋 + flag 19. face_off: E=0,N=1,S=2,U=3,W=4 */
static void ctx_place_vine(tree_ctx_t *t, jset_t *deco, int32_t x, int32_t y,
                           int32_t z, int face_off) {
    const char *dbg = getenv("HC_TREE_DEBUG_CELL");
    if (dbg) {
        int32_t dx, dy, dz;
        if (sscanf(dbg, "%d,%d,%d", &dx, &dy, &dz) == 3 && x == dx &&
            y == dy && z == dz)
            fprintf(stderr, "placeVine HIT (%d,%d,%d) face %d\n", x, y, z,
                    face_off);
    }
    jset_add(deco, x, y, z);
    hc_feat_set_block_ks(t->e->rg, x, y, z, /* flag 19 */
                         (uint16_t)(HC_B_VINE_BASE + face_off));
}

/* HORIZONTAL = [N, E, S, W]; step (dx,dz) + opposite face 오프셋 */
static const struct {
    int8_t dx, dz;
    uint8_t vine_face_opp; /* 반대편에서 이 방향을 보는 vine face */
} HORIZ[4] = {
    {0, -1, 2}, /* NORTH: opposite=S */
    {1, 0, 4},  /* EAST: opposite=W */
    {0, 1, 1},  /* SOUTH: opposite=N */
    {-1, 0, 0}, /* WEST: opposite=E */
};

/* bending (azalea — R5c §5, BendingTrunkPlacer.placeTrunk@0-227).
 * Direction.Plane.HORIZONTAL.getRandomDirection = faces[nextInt(4)],
 * faces = [N,E,S,W] — HORIZ 인덱스와 1:1. */
/* forking (acacia, 26.2 ForkingTrunkPlacer — Task 14).
 * 드로우 순서: lean 방향 nextInt(4) → leanHeight = h - nextInt(4) - 1 →
 * leanSteps = 3 - nextInt(3) → [로그 배치] → branch 방향 nextInt(4) →
 * (lean 과 다르면) branchPos = leanHeight - nextInt(2) - 1,
 * branchSteps = 1 + nextInt(3). 메인 attachment radius_offset = 1,
 * 브랜치 = 0. HORIZONTAL 순서 N,E,S,W. */
static int32_t trunk_forking(tree_ctx_t *t, int32_t x, int32_t y, int32_t z,
                             int32_t free, attach_t *out) {
    static const int8_t HDX[4] = {0, 1, 0, -1}; /* N,E,S,W */
    static const int8_t HDZ[4] = {-1, 0, 1, 0};
    feat_env_t *e = t->e;
    place_below_trunk(t, x, y - 1, z);
    int32_t n_att = 0;
    int32_t lean = hc_wgr_next_int(e->rng, 4);            /* DRAW */
    int32_t lean_h = free - hc_wgr_next_int(e->rng, 4) - 1; /* DRAW */
    int32_t lean_steps = 3 - hc_wgr_next_int(e->rng, 3);  /* DRAW */
    int32_t tx = x, tz = z;
    int32_t ey = INT32_MIN;
    for (int32_t yo = 0; yo < free; yo++) {
        int32_t yy = y + yo;
        if (yo >= lean_h && lean_steps > 0) {
            tx += HDX[lean];
            tz += HDZ[lean];
            lean_steps--;
        }
        if (place_log(t, tx, yy, tz))
            ey = yy + 1;
    }
    if (ey != INT32_MIN)
        out[n_att++] = (attach_t){tx, ey, tz, 1, 0};
    tx = x;
    tz = z;
    int32_t branch = hc_wgr_next_int(e->rng, 4); /* DRAW */
    if (branch != lean) {
        int32_t bpos = lean_h - hc_wgr_next_int(e->rng, 2) - 1; /* DRAW */
        int32_t bsteps = 1 + hc_wgr_next_int(e->rng, 3);        /* DRAW */
        ey = INT32_MIN;
        for (int32_t yo = bpos; yo < free && bsteps > 0; bsteps--) {
            if (yo >= 1) {
                int32_t yy = y + yo;
                tx += HDX[branch];
                tz += HDZ[branch];
                if (place_log(t, tx, yy, tz))
                    ey = yy + 1;
            }
            yo++;
        }
        if (ey != INT32_MIN)
            out[n_att++] = (attach_t){tx, ey, tz, 0, 0};
    }
    return n_att;
}

static int32_t trunk_bending(tree_ctx_t *t, int32_t x, int32_t y, int32_t z,
                             int32_t free, attach_t *out) {
    feat_env_t *e = t->e;
    int32_t d = hc_wgr_next_int(e->rng, 4); /* DRAW: 방향 */
    int32_t h = free - 1;
    /* below = pos.below() — 커서 이동 전 (@15-27). simple provider 는
     * getOptionalState==getState 로 무조건 쓰기 (below_not_mask 전부 0). */
    place_below_trunk(t, x, y - 1, z);
    int32_t cx = x, cy = y, cz = z;
    int32_t n = 0;
    for (int32_t i = 0; i <= h; i++) {
        /* nextInt(2) 는 매 반복 무조건 드로우 (@54-78); 조건 참이면 수평
         * 이동이 '누적'된다 (mutable cursor) */
        if (i + 1 >= h + hc_wgr_next_int(e->rng, 2)) {
            cx += HORIZ[d].dx;
            cz += HORIZ[d].dz;
        }
        if (valid_tree_pos(e, cx, cy, cz))
            place_log(t, cx, cy, cz);
        if (i >= t->cfg->min_height_for_leaves) {
            if (n >= MAX_ATTACH)
                die("bending trunk attachment overflow", NULL);
            out[n++] = (attach_t){cx, cy, cz, 0, 0};
        }
        cy++;
    }
    int32_t bend = iprov_sample(e->rng, &t->cfg->bend_length); /* DRAW */
    for (int32_t j = 0; j <= bend; j++) {
        if (valid_tree_pos(e, cx, cy, cz))
            place_log(t, cx, cy, cz);
        /* attachment 는 로그 스킵과 무관하게 무조건 (@189-210) */
        if (n >= MAX_ATTACH)
            die("bending trunk attachment overflow", NULL);
        out[n++] = (attach_t){cx, cy, cz, 0, 0};
        cx += HORIZ[d].dx;
        cz += HORIZ[d].dz;
    }
    return n;
}

static void dec_cocoa(tree_ctx_t *t, jset_t *deco, const cpos_t *logs,
                      int32_t n_logs, float prob) {
    feat_env_t *e = t->e;
    if (!(hc_wgr_next_float(e->rng) < prob)) /* DRAW 항상 */
        return;
    if (n_logs == 0)
        return;
    int32_t min_y = logs[0].y;
    for (int32_t i = 0; i < n_logs; i++) {
        if (logs[i].y - min_y > 2)
            continue;
        for (int d = 0; d < 4; d++) {
            float f = hc_wgr_next_float(e->rng); /* DRAW 항상 */
            if (!(f <= 0.25f))
                continue;
            /* pos2 = log + opposite(dir) 의 x/z */
            int32_t px = logs[i].x - HORIZ[d].dx;
            int32_t pz = logs[i].z - HORIZ[d].dz;
            if (!ctx_is_air(e, px, logs[i].y, pz))
                continue;
            int32_t age = hc_wgr_next_int(e->rng, 3); /* DRAW 조건부 */
            /* facing = dir (N,E,S,W = 팔레트 오프셋 그대로) */
            uint16_t st = (uint16_t)(HC_B_COCOA_BASE + age * 4 + d);
            jset_add(deco, px, logs[i].y, pz);
            hc_feat_set_block_ks(e->rg, px, logs[i].y, pz, st); /* flag 19 */
        }
    }
}

static void dec_trunk_vine(tree_ctx_t *t, jset_t *deco, const cpos_t *logs,
                           int32_t n_logs) {
    feat_env_t *e = t->e;
    for (int32_t i = 0; i < n_logs; i++) {
        int32_t x = logs[i].x, y = logs[i].y, z = logs[i].z;
        if (hc_wgr_next_int(e->rng, 3) > 0 && ctx_is_air(e, x - 1, y, z))
            ctx_place_vine(t, deco, x - 1, y, z, 0); /* west pos, EAST face */
        if (hc_wgr_next_int(e->rng, 3) > 0 && ctx_is_air(e, x + 1, y, z))
            ctx_place_vine(t, deco, x + 1, y, z, 4); /* east pos, WEST face */
        if (hc_wgr_next_int(e->rng, 3) > 0 && ctx_is_air(e, x, y, z - 1))
            ctx_place_vine(t, deco, x, y, z - 1, 2); /* north pos, SOUTH */
        if (hc_wgr_next_int(e->rng, 3) > 0 && ctx_is_air(e, x, y, z + 1))
            ctx_place_vine(t, deco, x, y, z + 1, 1); /* south pos, NORTH */
    }
}

static void dec_hanging_vine(tree_ctx_t *t, jset_t *deco, int32_t x, int32_t y,
                             int32_t z, int face_off) {
    ctx_place_vine(t, deco, x, y, z, face_off);
    int32_t cnt = 4;
    y -= 1;
    while (ctx_is_air(t->e, x, y, z) && cnt > 0) {
        ctx_place_vine(t, deco, x, y, z, face_off);
        y -= 1;
        cnt--;
    }
}

static void dec_leave_vine(tree_ctx_t *t, jset_t *deco, const cpos_t *leaves,
                           int32_t n_leaves, float prob) {
    feat_env_t *e = t->e;
    const char *dbg = getenv("HC_TREE_DEBUG");
    for (int32_t i = 0; i < n_leaves; i++) {
        int32_t x = leaves[i].x, y = leaves[i].y, z = leaves[i].z;
        float f1 = hc_wgr_next_float(e->rng);
        if (f1 < prob && ctx_is_air(e, x - 1, y, z))
            dec_hanging_vine(t, deco, x - 1, y, z, 0); /* west, EAST face */
        float f2 = hc_wgr_next_float(e->rng);
        if (f2 < prob && ctx_is_air(e, x + 1, y, z))
            dec_hanging_vine(t, deco, x + 1, y, z, 4);
        float f3 = hc_wgr_next_float(e->rng);
        if (f3 < prob && ctx_is_air(e, x, y, z - 1))
            dec_hanging_vine(t, deco, x, y, z - 1, 2);
        float f4 = hc_wgr_next_float(e->rng);
        if (f4 < prob && ctx_is_air(e, x, y, z + 1))
            dec_hanging_vine(t, deco, x, y, z + 1, 1);
        if (dbg)
            fprintf(stderr,
                    "leave_vine leaf %d (%d,%d,%d) f %.4f %.4f %.4f %.4f\n",
                    i, x, y, z, (double)f1, (double)f2, (double)f3,
                    (double)f4);
    }
}

/* attached_to_logs (fallen_tree): shuffledCopy 후 per-log 드로우 */
static void dec_attached_to_logs(tree_ctx_t *t, jset_t *deco,
                                 const hc_tdec_t *dec, cpos_t *logs,
                                 int32_t n_logs) {
    feat_env_t *e = t->e;
    /* Util.shuffle: j = size..2, swap(j-1, nextInt(j)) */
    for (int32_t j = n_logs; j > 1; j--) {
        int32_t k = hc_wgr_next_int(e->rng, j);
        cpos_t  tmp = logs[j - 1];
        logs[j - 1] = logs[k];
        logs[k] = tmp;
    }
    for (int32_t i = 0; i < n_logs; i++) {
        /* directions = [up] (컴파일 보장) — nextInt(1) 은 드로우를 태운다 */
        int32_t di = hc_wgr_next_int(e->rng, 1);
        (void)di;
        int32_t px = logs[i].x, py = logs[i].y + 1, pz = logs[i].z;
        float   f = hc_wgr_next_float(e->rng); /* DRAW 항상 */
        if (f <= dec->prob && ctx_is_air(e, px, py, pz)) {
            uint16_t st = sprov_sample(e->rng, &dec->provider);
            jset_add(deco, px, py, pz);
            hc_feat_set_block_ks(e->rg, px, py, pz, st); /* flag 19 */
        }
    }
}

/* beehive (26.2 BeehiveDecorator — Task 14): 트리거/셔플/탐색 드로우까지
 * 재현. 실제 배치 성공은 die — 골든 리전에 bee_nest 가 없다 (BE 미구현;
 * 도달 = 상류 리플레이 발산 신호). */
static void dec_beehive(tree_ctx_t *t, const hc_tdec_t *dec,
                        const cpos_t *logs, int32_t n_logs,
                        const cpos_t *leaves, int32_t n_leaves) {
    feat_env_t *e = t->e;
    if (n_logs == 0)
        return; /* 드로우 0 */
    if (hc_wgr_next_float(e->rng) >= dec->prob)
        return;
    int32_t hive_y;
    if (n_leaves > 0) {
        int32_t a = leaves[0].y - 1, b = logs[0].y + 1;
        hive_y = a > b ? a : b;
    } else {
        int32_t a = logs[0].y + 1 + hc_wgr_next_int(e->rng, 3);
        int32_t b = logs[n_logs - 1].y;
        hive_y = a < b ? a : b;
    }
    /* SPAWN_DIRECTIONS = HORIZONTAL − NORTH(=SOUTH.opposite) = [E, S, W] */
    static const int8_t SDX[3] = {1, 0, -1}, SDZ[3] = {0, 1, 0};
    enum { MAX_PL = 512 };
    cpos_t  pl[MAX_PL];
    int32_t n_pl = 0;
    for (int32_t i = 0; i < n_logs; i++) {
        if (logs[i].y != hive_y)
            continue;
        for (int d = 0; d < 3; d++) {
            if (n_pl >= MAX_PL)
                die("beehive placement list overflow", NULL);
            pl[n_pl++] = (cpos_t){logs[i].x + SDX[d], logs[i].y,
                                  logs[i].z + SDZ[d]};
        }
    }
    if (n_pl == 0)
        return;
    for (int32_t j = n_pl; j > 1; j--) { /* Util.shuffle */
        int32_t k = hc_wgr_next_int(e->rng, j);
        cpos_t  tmp = pl[j - 1];
        pl[j - 1] = pl[k];
        pl[k] = tmp;
    }
    for (int32_t i = 0; i < n_pl; i++)
        if (ctx_is_air(e, pl[i].x, pl[i].y, pl[i].z) &&
            ctx_is_air(e, pl[i].x, pl[i].y, pl[i].z + 1))
            die("beehive placement fired — bee_nest unimplemented (golden "
                "region has none; upstream replay diverged?)",
                NULL);
}

/* place_on_ground (26.2 PlaceOnGroundDecorator — Task 14, leaf_litter):
 * 최저 y 로그들의 xz 박스를 radius/height 로 인플레이트, tries 회
 * (rbi ×3 드로우 고정) 시도. above 가 air/vine && pos 가 isSolidRender
 * && MBNL 하이트맵 <= above.y 일 때만 프로바이더 샘플 (조건부 드로우) +
 * setBlock flag 19. */
static void dec_place_on_ground(tree_ctx_t *t, jset_t *deco,
                                const hc_tdec_t *dec, const cpos_t *logs,
                                int32_t n_logs) {
    feat_env_t *e = t->e;
    if (n_logs == 0)
        return; /* getLowestTrunkOrRootOfTree 빈 리스트 (roots 없음) */
    int32_t min_y = logs[0].y; /* ctx 리스트는 y 오름차순 안정 정렬 */
    int32_t x0 = logs[0].x, x1 = logs[0].x, z0 = logs[0].z, z1 = logs[0].z;
    for (int32_t i = 0; i < n_logs; i++) {
        if (logs[i].y != min_y)
            continue;
        if (logs[i].x < x0)
            x0 = logs[i].x;
        if (logs[i].x > x1)
            x1 = logs[i].x;
        if (logs[i].z < z0)
            z0 = logs[i].z;
        if (logs[i].z > z1)
            z1 = logs[i].z;
    }
    x0 -= dec->radius;
    x1 += dec->radius;
    z0 -= dec->radius;
    z1 += dec->radius;
    int32_t y0 = min_y - dec->height, y1 = min_y + dec->height;
    for (int32_t i = 0; i < dec->tries; i++) {
        int32_t px = hc_mth_random_between_inclusive(e->rng, x0, x1);
        int32_t py = hc_mth_random_between_inclusive(e->rng, y0, y1);
        int32_t pz = hc_mth_random_between_inclusive(e->rng, z0, z1);
        uint16_t above = hc_feat_get_block(e->rg, px, py + 1, pz);
        if (!(hc_block_is_air(above) || is_vine_state(above)))
            continue;
        if (!hc_block_is_full_cube(hc_feat_get_block(e->rg, px, py, pz)))
            continue; /* isSolidRender */
        if (hc_feat_height(e->rg, HC_HM_MOTION_BLOCKING_NO_LEAVES, px, pz) >
            py + 1)
            continue;
        uint16_t st =
            hc_featx_sprov_sample_at(e->rng, &dec->provider, px, py + 1, pz);
        jset_add(deco, px, py + 1, pz);
        hc_feat_set_block_ks(e->rg, px, py + 1, pz, st); /* flag 19 */
    }
}

/* 데코레이터 실행 (Context 생성 = 리스트 + 정렬) */
static void run_decorators(tree_ctx_t *t, const hc_tdec_t *decs, int32_t n_dec,
                           jset_t *logs, jset_t *leaves, jset_t *deco) {
    if (n_dec == 0)
        return;
    static cpos_t log_arr[JSET_MAX_ENTRIES], leaf_arr[JSET_MAX_ENTRIES];
    int32_t n_logs = ctx_list(logs, log_arr, JSET_MAX_ENTRIES);
    int32_t n_leaves = ctx_list(leaves, leaf_arr, JSET_MAX_ENTRIES);
    for (int32_t i = 0; i < n_dec; i++) {
        switch (decs[i].kind) {
        case HC_TDEC_COCOA:
            dec_cocoa(t, deco, log_arr, n_logs, decs[i].prob);
            break;
        case HC_TDEC_TRUNK_VINE:
            dec_trunk_vine(t, deco, log_arr, n_logs);
            break;
        case HC_TDEC_LEAVE_VINE:
            dec_leave_vine(t, deco, leaf_arr, n_leaves, decs[i].prob);
            break;
        case HC_TDEC_ATTACHED_TO_LOGS:
            dec_attached_to_logs(t, deco, &decs[i], log_arr, n_logs);
            break;
        case HC_TDEC_BEEHIVE:
            dec_beehive(t, &decs[i], log_arr, n_logs, leaf_arr, n_leaves);
            break;
        case HC_TDEC_PLACE_ON_GROUND:
            dec_place_on_ground(t, deco, &decs[i], log_arr, n_logs);
            break;
        default:
            die("unknown tree decorator", NULL);
        }
    }
}

/* ================= updateLeaves (R2 §4) ================= */

enum { BOX_MAX_SPAN = 64 };

typedef struct {
    int32_t min_x, min_y, min_z, max_x, max_y, max_z;
    uint64_t bits[(BOX_MAX_SPAN * BOX_MAX_SPAN * BOX_MAX_SPAN + 63) / 64];
} shape_t;

static int box_inside(const shape_t *b, int32_t x, int32_t y, int32_t z) {
    return x >= b->min_x && x <= b->max_x && y >= b->min_y && y <= b->max_y &&
           z >= b->min_z && z <= b->max_z;
}

static size_t shape_idx(const shape_t *b, int32_t x, int32_t y, int32_t z) {
    size_t sx = (size_t)(b->max_x - b->min_x + 1);
    size_t sz = (size_t)(b->max_z - b->min_z + 1);
    return ((size_t)(y - b->min_y) * sz + (size_t)(z - b->min_z)) * sx +
           (size_t)(x - b->min_x);
}

static void shape_fill(shape_t *b, int32_t x, int32_t y, int32_t z) {
    size_t i = shape_idx(b, x, y, z);
    b->bits[i >> 6] |= 1ull << (i & 63);
}

static int shape_full(const shape_t *b, int32_t x, int32_t y, int32_t z) {
    size_t i = shape_idx(b, x, y, z);
    return (b->bits[i >> 6] >> (i & 63)) & 1u;
}

static void box_extend(shape_t *b, const jset_t *s, int *any) {
    jit_t it;
    for (jit_begin(&it, s); jit_valid(&it); jit_next(&it)) {
        const jent_t *en = &s->ent[it.e];
        if (!*any) {
            b->min_x = b->max_x = en->x;
            b->min_y = b->max_y = en->y;
            b->min_z = b->max_z = en->z;
            *any = 1;
        } else {
            if (en->x < b->min_x)
                b->min_x = en->x;
            if (en->x > b->max_x)
                b->max_x = en->x;
            if (en->y < b->min_y)
                b->min_y = en->y;
            if (en->y > b->max_y)
                b->max_y = en->y;
            if (en->z < b->min_z)
                b->min_z = en->z;
            if (en->z > b->max_z)
                b->max_z = en->z;
        }
    }
}

/* Direction.values(): DOWN, UP, N, S, W, E */
static const int8_t DIR6[6][3] = {{0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                                  {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};

/* 잎 패밀리 베이스: oak/jungle 또는 azalea/flowering — 각각 [base, +28),
 * 레이아웃 base + 종*14 + wl*7 + (distance-1) (R5c §7.3.6). 잎 아니면 0. */
static uint16_t leaf_family_base(uint16_t s) {
    if (s >= HC_B_OAK_LEAVES_BASE && s < HC_B_OAK_LEAVES_BASE + 28)
        return HC_B_OAK_LEAVES_BASE;
    if (s >= HC_B_AZALEA_LEAVES_BASE && s < HC_B_AZALEA_LEAVES_BASE + 28)
        return HC_B_AZALEA_LEAVES_BASE;
    /* acacia — T14 [base, base+14), wl*7+(d-1) (Task 14). (s-fam)%7 및
     * /7*7 산술이 2-종 패밀리와 동형 (종 항이 0 일 뿐). */
    uint16_t ac = hc_block_acacia_leaves_base();
    if (s >= ac && s < ac + 14)
        return ac;
    return 0;
}

/* getOptionalDistanceAt: #prevents_nearby_leaf_decay → 0; DISTANCE 프로퍼티
 * (잎) → 값; 그 외 empty(-1) */
static int32_t optional_distance_at(feat_env_t *e, uint16_t s) {
    if (mask_test(e->reg->tag_prevents_leaf_decay, s))
        return 0;
    uint16_t fam = leaf_family_base(s);
    if (fam)
        return (s - fam) % 7 + 1;
    return -1;
}

/* 잎 상태의 distance 재기록 (wl/종 보존) */
static uint16_t leaf_with_distance(uint16_t s, int32_t d) {
    uint16_t fam = leaf_family_base(s);
    int32_t  base = (s - fam) / 7 * 7 + fam;
    return (uint16_t)(base + (d - 1));
}

static shape_t *update_leaves(tree_ctx_t *t) {
    feat_env_t *e = t->e;
    shape_t    *box;
    static shape_t box_storage;
    box = &box_storage;
    memset(box->bits, 0, sizeof box->bits);
    int any = 0;
    box_extend(box, &t->roots, &any);
    box_extend(box, &t->logs, &any);
    box_extend(box, &t->leaves, &any);
    box_extend(box, &t->deco, &any);
    if (!any)
        return NULL;
    if (box->max_x - box->min_x >= BOX_MAX_SPAN ||
        box->max_y - box->min_y >= BOX_MAX_SPAN ||
        box->max_z - box->min_z >= BOX_MAX_SPAN)
        die("tree bounding box too large", NULL);

    /* fill: union(deco, roots) — 순서 무관 (idempotent) */
    jit_t it;
    for (jit_begin(&it, &t->deco); jit_valid(&it); jit_next(&it)) {
        const jent_t *en = &t->deco.ent[it.e];
        if (box_inside(box, en->x, en->y, en->z))
            shape_fill(box, en->x, en->y, en->z);
    }
    for (jit_begin(&it, &t->roots); jit_valid(&it); jit_next(&it)) {
        const jent_t *en = &t->roots.ent[it.e];
        if (box_inside(box, en->x, en->y, en->z))
            shape_fill(box, en->x, en->y, en->z);
    }

    static jset_t levels[7];
    for (int i = 0; i < 7; i++)
        jset_init(&levels[i]);
    /* lists.get(0).addAll(logs) — logs 셋 순회 순서 */
    for (jit_begin(&it, &t->logs); jit_valid(&it); jit_next(&it)) {
        const jent_t *en = &t->logs.ent[it.e];
        jset_add(&levels[0], en->x, en->y, en->z);
    }

    int32_t i = 0;
    for (;;) {
        while (i < 7 && levels[i].size == 0)
            i++;
        if (i >= 7)
            return box;
        int32_t x = 0, y = 0, z = 0;
        if (!jset_poll_first(&levels[i], &x, &y, &z))
            die("updateLeaves poll on empty level", NULL);
        if (!box_inside(box, x, y, z))
            continue;
        if (i != 0) {
            uint16_t cur = hc_feat_get_block(e->rg, x, y, z);
            /* setValue(DISTANCE, i) — 잎이 아니면 프로퍼티 없음: 바닐라는
             * setValue 가 IllegalArgument 를 던질 상황이지만 여기 도달하는
             * 상태는 항상 잎이다 (enqueue 게이트) */
            if (!leaf_family_base(cur))
                die("updateLeaves rewrite on non-leaf", hc_block_name(cur));
            hc_feat_set_block_ks(e->rg, x, y, z, /* flag 19 */
                                 leaf_with_distance(cur, i));
        }
        shape_fill(box, x, y, z);
        for (int d = 0; d < 6; d++) {
            int32_t nx = x + DIR6[d][0], ny = y + DIR6[d][1],
                    nz = z + DIR6[d][2];
            if (!box_inside(box, nx, ny, nz))
                continue;
            if (shape_full(box, nx, ny, nz))
                continue;
            int32_t od =
                optional_distance_at(e, hc_feat_get_block(e->rg, nx, ny, nz));
            if (od < 0)
                continue;
            int32_t k = od < i + 1 ? od : i + 1;
            if (k < 7) {
                jset_add(&levels[k], nx, ny, nz);
                if (k < i)
                    i = k;
            }
        }
    }
}

/* ================= StructureTemplate.updateShapeAtEdge (R5) =================
 *
 * 나무 상자 경계의 두 셀에 BlockState.updateShape 를 돌리는 마무리 패스.
 * 앞선 면 이벤트의 쓰기가 뒤 이벤트에 보인다 — 열거 순서가 진리.
 * 모든 쓰기는 flag 2 경로 (hc_feat_set_block). */

static const uint8_t D_OPP6[6] = {1, 0, 3, 2, 5, 4}; /* DOWN,UP,N,S,W,E */

/* canAttachTo: 지지면 완전 || 충돌면 완전 — 완전 큐브와 잎 (R5 §7) */
static int edge_attach_ok(uint16_t s) {
    return hc_block_is_full_cube(s) || hc_block_is_leaves(s);
}

/* vine face 팔레트 오프셋(E,N,S,U,W) → 지지 이웃 방향 (dx,dz) */
static const int8_t VINE_FACE_STEP[5][2] = {
    {1, 0},  /* E: 지지는 동쪽 */
    {0, -1}, /* N */
    {0, 1},  /* S */
    {0, 0},  /* U: 위 (특별 처리) */
    {-1, 0}, /* W */
};

/* VineBlock.updateShape (dir != DOWN): getUpdatedState + hasFaces (R5 §4).
 * 팔레트는 단면 vine 뿐 — 그 한 면의 판정으로 축약. */
static uint16_t edge_vine_update(feat_env_t *e, uint16_t s, int32_t x,
                                 int32_t y, int32_t z) {
    int off = s - HC_B_VINE_BASE;
    if (off == 3) { /* up 면: isAcceptableNeighbour(above, DOWN) */
        return edge_attach_ok(hc_feat_get_block(e->rg, x, y + 1, z))
                   ? s
                   : HC_B_AIR;
    }
    /* 수평 면: canSupportAtFace(d) — 이웃 attach || 위 vine 같은 면 */
    if (edge_attach_ok(hc_feat_get_block(e->rg, x + VINE_FACE_STEP[off][0], y,
                                         z + VINE_FACE_STEP[off][1])))
        return s;
    if (hc_feat_get_block(e->rg, x, y + 1, z) == s)
        return s; /* 위 vine 이 같은 단면 상태 (getValue(prop) 동치) */
    return HC_B_AIR;
}

/* Task 12: updateShape 가 스케줄하는 틱 기록 (즉시 상태 변경과 별개).
 * 바이트코드 근거 (task12-region/R-D + task14 디컴파일):
 *  - LeavesBlock.updateShape: waterlogged → 물 소스 틱 (@0-29); 이어
 *    i = getDistanceAt(ns)+1, (i==1 && distance==1) 이 아니면 자기 블록
 *    틱 (@34-72, delay 1 — 저장 t 는 proto 경로에서 0 고정).
 *  - LiquidBlock.updateShape: 자신 또는 이웃 유체가 소스면 getType 틱;
 *    이어 dir==DOWN 이고 자신이 물 소스이며 아래가 #enables_bubble_
 *    column_* (magma/soul_sand) 이면 자기 블록(물) 틱 delay 20
 *    (LiquidBlock.java:156-175, 194-198). 흐름수는 이웃 소스일 때만
 *    (getType = flowing_water).
 *  - FallingBlock(모래/자갈)·BrushableBlock(suspicious_*).updateShape:
 *    무조건 자기 블록 틱 (FallingBlock.java:31-43, BrushableBlock:72-84).
 *    ※ 기존 "리전 전체 0건" 주석은 오측 — 골든 실측 t=0 sand 358건
 *    (구조물·나무 경계). 기존 게이트 청크에는 해당 케이스가 없어
 *    이 추가는 게이트 불변.
 *  - SimpleWaterloggedBlock 계열 (stairs/fence/chest/slab/trapdoor/
 *    lichen/dripleaf 등): waterlogged=true 면 물 소스 틱 — 공통 패턴.
 *  - seagrass/kelp 는 결과 의존 틱이라 edge_update_state 가 자체 처리. */
static int falling_family(uint16_t s) {
    if (s == HC_B_SAND || s == HC_B_RED_SAND || s == HC_B_GRAVEL)
        return 1;
    if (s >= HC_B_T14_BASE) {
        const char *nm = hc_block_name(s);
        if (strncmp(nm, "minecraft:suspicious_", 21) == 0)
            return 1;
    }
    return 0;
}

static void edge_schedule_ticks(feat_env_t *e, uint16_t s, int32_t x,
                                int32_t y, int32_t z, int dir, uint16_t ns) {
    if (!e->rg->ticks)
        return;
    if (leaf_family_base(s)) {
        if (hc_block_is_waterlogged(s))
            hc_feat_schedule_tick(e->rg, x, y, z, s, HC_TICK_WATER, 0);
        int32_t od = optional_distance_at(e, ns);
        int32_t i = (od < 0 ? 7 : od) + 1;
        int32_t dist = (s - leaf_family_base(s)) % 7 + 1;
        if (!(i == 1 && dist == 1))
            hc_feat_schedule_tick(e->rg, x, y, z, s, HC_TICK_BLOCK, 0);
        return;
    }
    if (s == HC_B_WATER || s == HC_B_LAVA ||
        (s >= HC_B_WATER_FLOW_BASE && s < HC_B_WATER_FLOW_BASE + 15)) {
        int flow = s != HC_B_WATER && s != HC_B_LAVA;
        int ns_src = ns == HC_B_WATER || ns == HC_B_LAVA ||
                     hc_block_is_waterlogged(ns);
        if (!flow || ns_src)
            hc_feat_schedule_tick(e->rg, x, y, z, s,
                                  s == HC_B_LAVA
                                      ? HC_TICK_LAVA
                                      : (flow ? HC_TICK_FLOWING_WATER
                                              : HC_TICK_WATER),
                                  0);
        /* tryScheduleBubbleBlockColumn — 물 소스 && 아래가 magma (이
         * 리전에 soul_sand 부재). 블록 틱 (i=minecraft:water). */
        if (s == HC_B_WATER && dir == 0 && ns == HC_B_MAGMA_BLOCK)
            hc_feat_schedule_tick(e->rg, x, y, z, s, HC_TICK_BLOCK, 0);
        return;
    }
    if (falling_family(s)) {
        hc_feat_schedule_tick(e->rg, x, y, z, s, HC_TICK_BLOCK, 0);
        return;
    }
    /* seagrass/kelp 계열은 edge_update_state 가 처리 (결과 의존) */
    if (s == HC_B_SEAGRASS ||
        (s >= HC_B_KELP_BASE && s <= HC_B_KELP_PLANT))
        return;
    if (hc_block_is_waterlogged(s))
        hc_feat_schedule_tick(e->rg, x, y, z, s, HC_TICK_WATER, 0);
}

/* ---------- Task 14: 템플릿 팔레트 패밀리 (T14 이름 기반) ----------
 *
 * 구조물 배치 경로 (structures_template.c) 가 hc_featx_edge_* 익스포트로
 * 이 디스패치를 공유한다. 시맨틱 = 26.2 디컴파일 그대로:
 * StairBlock/FenceBlock/DoorBlock/ChestBlock (task14-blockprops-decomp/
 * blocks/), Seagrass/TallSeagrass/Kelp (task14-decomp/blocks/). */

typedef struct {
    char     base[128];
    hc_skv_t kv[16];
    int      n;
} sview_t;

static void sview_of(uint16_t s, sview_t *v) {
    v->n = hc_state_parse(hc_block_name(s), v->base, sizeof v->base, v->kv,
                          16);
}

static const char *sview_get(const sview_t *v, const char *k) {
    for (int i = 0; i < v->n; i++)
        if (strcmp(v->kv[i].k, k) == 0)
            return v->kv[i].v;
    return NULL;
}

static void sview_set(sview_t *v, const char *k, const char *val) {
    for (int i = 0; i < v->n; i++)
        if (strcmp(v->kv[i].k, k) == 0) {
            snprintf(v->kv[i].v, sizeof v->kv[i].v, "%s", val);
            return;
        }
    die("state property missing for rewrite", k);
}

enum { TF_NONE = 0, TF_STAIRS, TF_FENCE, TF_DOOR, TF_CHEST, TF_SLAB,
       TF_TRAPDOOR };

static int base_ends(const char *nm, size_t n, const char *suf) {
    size_t sl = strlen(suf);
    return n >= sl && strncmp(nm + n - sl, suf, sl) == 0;
}

static int t14_family(uint16_t s) {
    if (s < HC_B_T14_BASE)
        return TF_NONE;
    const char *nm = hc_block_name(s);
    const char *br = strchr(nm, '[');
    size_t      n = br ? (size_t)(br - nm) : strlen(nm);
    if (base_ends(nm, n, "_stairs"))
        return TF_STAIRS;
    if (base_ends(nm, n, "_fence"))
        return TF_FENCE;
    if (base_ends(nm, n, "_trapdoor"))
        return TF_TRAPDOOR;
    if (base_ends(nm, n, "_door"))
        return TF_DOOR;
    if (base_ends(nm, n, "_slab"))
        return TF_SLAB;
    if ((n == 15 && strncmp(nm, "minecraft:chest", 15) == 0) ||
        (n == 23 && strncmp(nm, "minecraft:trapped_chest", 23) == 0))
        return TF_CHEST;
    return TF_NONE;
}

/* facing 문자열 ↔ MC 서수 (0=D,1=U,2=N,3=S,4=W,5=E) */
static int face_ord(const char *v) {
    if (strcmp(v, "north") == 0)
        return 2;
    if (strcmp(v, "south") == 0)
        return 3;
    if (strcmp(v, "west") == 0)
        return 4;
    if (strcmp(v, "east") == 0)
        return 5;
    return -1; /* up/down — 수평 로직 미사용 */
}

/* Direction.getClockWise / getCounterClockWise (Y축) */
static const int ORD_CW[6] = {0, 1, 5, 4, 2, 3};  /* N→E,S→W,W→N,E→S */
static const int ORD_CCW[6] = {0, 1, 4, 5, 3, 2}; /* N→W,S→E,W→S,E→N */
static const char *const ORD_NAME[6] = {"down", "up",   "north",
                                        "south", "west", "east"};

/* isFaceSturdy(SupportType.FULL) — 풀큐브/azalea(기존) + T14 형상 블록.
 * stairs: half 면 + straight/inner 의 측면 풀페이스 (26.2 셰이프 유도);
 * slab: type 면; trapdoor: 닫힘 상태의 half 면. 그 외 부분형상 0. */
static int t14_face_sturdy(uint16_t s, int dir) {
    if (hc_featx_face_sturdy_full(s, dir))
        return 1;
    int fam = t14_family(s);
    if (fam == TF_NONE)
        return 0;
    sview_t v;
    sview_of(s, &v);
    if (fam == TF_STAIRS) {
        const char *half = sview_get(&v, "half");
        if (dir == 0)
            return strcmp(half, "bottom") == 0;
        if (dir == 1)
            return strcmp(half, "top") == 0;
        const char *shape = sview_get(&v, "shape");
        int         f = face_ord(sview_get(&v, "facing"));
        if (strcmp(shape, "straight") == 0)
            return dir == f;
        if (strcmp(shape, "inner_left") == 0)
            return dir == f || dir == ORD_CCW[f];
        if (strcmp(shape, "inner_right") == 0)
            return dir == f || dir == ORD_CW[f];
        return 0; /* outer_* — 측면 풀페이스 없음 */
    }
    if (fam == TF_SLAB) {
        const char *t = sview_get(&v, "type");
        if (strcmp(t, "double") == 0)
            return 1;
        return dir == (strcmp(t, "top") == 0 ? 1 : 0);
    }
    if (fam == TF_TRAPDOOR) {
        if (strcmp(sview_get(&v, "open"), "true") == 0)
            return 0;
        return dir == (strcmp(sview_get(&v, "half"), "top") == 0 ? 1 : 0);
    }
    return 0; /* fence/door/chest — 풀페이스 없음 */
}

/* FenceBlock.connectsTo (FenceBlock.java:61-70, Block.java:268-276) */
static int fence_connects_to(uint16_t self, uint16_t ns, int dir_to_ns) {
    /* isExceptionForConnection: 잎 instanceof / barrier (호박·멜론·셜커
     * 리전 부재) */
    const char *nsn = hc_block_name(ns);
    const char *br = strchr(nsn, '[');
    size_t      nl = br ? (size_t)(br - nsn) : strlen(nsn);
    int exception = hc_block_is_leaves(ns) ||
                    (nl == 17 && strncmp(nsn, "minecraft:barrier", 17) == 0);
    /* 이웃의 우리쪽 면 = dir 의 반대면 */
    static const int OPP[6] = {1, 0, 3, 2, 5, 4};
    int face_solid = t14_face_sturdy(ns, OPP[dir_to_ns]);
    /* isSameFence: #fences && 우드 여부 일치 (self 는 항상 우드 —
     * nether_brick_fence 는 이 경로에 등장하지 않는다) */
    int same_fence = 0;
    if (base_ends(nsn, nl, "_fence")) {
        int ns_wooden =
            !(nl == 28 &&
              strncmp(nsn, "minecraft:nether_brick_fence", 28) == 0);
        const char *sn = hc_block_name(self);
        const char *sbr = strchr(sn, '[');
        size_t      sl = sbr ? (size_t)(sbr - sn) : strlen(sn);
        int self_wooden =
            !(sl == 28 && strncmp(sn, "minecraft:nether_brick_fence", 28) == 0);
        same_fence = ns_wooden == self_wooden;
    }
    /* fence gate — 리전 부재 */
    return (!exception && face_solid) || same_fence;
}

static int is_stairs_state(uint16_t s) { return t14_family(s) == TF_STAIRS; }

/* StairBlock.getStairsShape (StairBlock.java:132-168) — 월드 재읽기 */
static const char *stairs_shape_of(feat_env_t *e, const sview_t *v,
                                   int32_t x, int32_t y, int32_t z) {
    static const int8_t STEP[6][3] = {{0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                                      {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};
    static const int    OPP[6] = {1, 0, 3, 2, 5, 4};
    int         f = face_ord(sview_get(v, "facing"));
    const char *half = sview_get(v, "half");
#define WORLD(d)                                                              \
    hc_feat_get_block(e->rg, x + STEP[d][0], y + STEP[d][1], z + STEP[d][2])
    uint16_t behind = WORLD(f);
    if (is_stairs_state(behind)) {
        sview_t bv;
        sview_of(behind, &bv);
        if (strcmp(sview_get(&bv, "half"), half) == 0) {
            int bf = face_ord(sview_get(&bv, "facing"));
            int axis_diff = ((f == 2 || f == 3) != (bf == 2 || bf == 3));
            if (axis_diff) {
                /* canTakeShape(state, pos + OPP(bf)) */
                uint16_t probe = WORLD(OPP[bf]);
                int      blocked = 0;
                if (is_stairs_state(probe)) {
                    sview_t pv;
                    sview_of(probe, &pv);
                    blocked = face_ord(sview_get(&pv, "facing")) == f &&
                              strcmp(sview_get(&pv, "half"), half) == 0;
                }
                if (!blocked)
                    return bf == ORD_CCW[f] ? "outer_left" : "outer_right";
            }
        }
    }
    uint16_t front = WORLD(OPP[f]);
    if (is_stairs_state(front)) {
        sview_t fv;
        sview_of(front, &fv);
        if (strcmp(sview_get(&fv, "half"), half) == 0) {
            int ff = face_ord(sview_get(&fv, "facing"));
            int axis_diff = ((f == 2 || f == 3) != (ff == 2 || ff == 3));
            if (axis_diff) {
                uint16_t probe = WORLD(ff);
                int      blocked = 0;
                if (is_stairs_state(probe)) {
                    sview_t pv;
                    sview_of(probe, &pv);
                    blocked = face_ord(sview_get(&pv, "facing")) == f &&
                              strcmp(sview_get(&pv, "half"), half) == 0;
                }
                if (!blocked)
                    return ff == ORD_CCW[f] ? "inner_left" : "inner_right";
            }
        }
    }
#undef WORLD
    return "straight";
}

/* ChestBlock.getConnectedDirection (ChestBlock.java:168-171) */
static int chest_connected_dir(const sview_t *v) {
    int f = face_ord(sview_get(v, "facing"));
    return strcmp(sview_get(v, "type"), "left") == 0 ? ORD_CW[f]
                                                     : ORD_CCW[f];
}

/* T14 패밀리 updateShape — 즉시 상태 변경만 (틱은 edge_schedule_ticks
 * 의 generic waterlogged 브랜치) */
static uint16_t t14_update_state(feat_env_t *e, int fam, uint16_t s,
                                 int32_t x, int32_t y, int32_t z, int dir,
                                 uint16_t ns) {
    static const int OPP[6] = {1, 0, 3, 2, 5, 4};
    if (fam == TF_STAIRS) {
        if (dir < 2)
            return s;
        sview_t v;
        sview_of(s, &v);
        const char *shape = stairs_shape_of(e, &v, x, y, z);
        if (strcmp(shape, sview_get(&v, "shape")) == 0)
            return s;
        sview_set(&v, "shape", shape);
        return hc_state_build(v.base, v.kv, v.n);
    }
    if (fam == TF_FENCE) {
        if (dir < 2)
            return s;
        sview_t v;
        sview_of(s, &v);
        int         conn = fence_connects_to(s, ns, dir);
        const char *cur = sview_get(&v, ORD_NAME[dir]);
        if ((strcmp(cur, "true") == 0) == (conn != 0))
            return s;
        sview_set(&v, ORD_NAME[dir], conn ? "true" : "false");
        return hc_state_build(v.base, v.kv, v.n);
    }
    if (fam == TF_DOOR) {
        sview_t v;
        sview_of(s, &v);
        int lower = strcmp(sview_get(&v, "half"), "lower") == 0;
        int vert = dir == 0 || dir == 1;
        if (vert && lower == (dir == 1)) {
            /* 반대쪽 반토막에서 온 업데이트 */
            if (t14_family(ns) == TF_DOOR) {
                sview_t nv;
                sview_of(ns, &nv);
                if ((strcmp(sview_get(&nv, "half"), "lower") == 0) !=
                    lower) {
                    sview_set(&nv, "half", lower ? "lower" : "upper");
                    return hc_state_build(nv.base, nv.kv, nv.n);
                }
            }
            return HC_B_AIR;
        }
        if (lower && dir == 0 && !t14_face_sturdy(ns, 1))
            return HC_B_AIR;
        return s;
    }
    if (fam == TF_CHEST) {
        sview_t v;
        sview_of(s, &v);
        int same = t14_family(ns) == TF_CHEST;
        if (same) {
            /* chestCanConnectTo = 같은 블록 (chest vs trapped 구분) */
            const char *a = hc_block_name(s), *b = hc_block_name(ns);
            const char *abr = strchr(a, '['), *bbr = strchr(b, '[');
            size_t      al = abr ? (size_t)(abr - a) : strlen(a);
            size_t      bl = bbr ? (size_t)(bbr - b) : strlen(b);
            same = al == bl && strncmp(a, b, al) == 0;
        }
        if (same && dir >= 2) {
            sview_t nv;
            sview_of(ns, &nv);
            const char *nt = sview_get(&nv, "type");
            if (strcmp(sview_get(&v, "type"), "single") == 0 &&
                strcmp(nt, "single") != 0 &&
                strcmp(sview_get(&v, "facing"),
                       sview_get(&nv, "facing")) == 0 &&
                chest_connected_dir(&nv) == OPP[dir]) {
                sview_set(&v, "type",
                          strcmp(nt, "left") == 0 ? "right" : "left");
                return hc_state_build(v.base, v.kv, v.n);
            }
        } else if (chest_connected_dir(&v) == dir &&
                   strcmp(sview_get(&v, "type"), "single") != 0) {
            sview_set(&v, "type", "single");
            return hc_state_build(v.base, v.kv, v.n);
        }
        return s;
    }
    return s; /* TF_SLAB / TF_TRAPDOOR — 상태 불변 (틱만) */
}

/* BlockState.updateShape 디스패치 — 즉시 상태 변경만 (tick 스케줄 =
 * edge_schedule_ticks; seagrass/kelp 는 결과 의존이라 여기서 스케줄).
 * ns 는 전달된 이웃 상태 (재읽기 아님 — R5 §2). */
static uint16_t edge_update_state(feat_env_t *e, uint16_t s, int32_t x,
                                  int32_t y, int32_t z, int dir, uint16_t ns) {
    if (s == HC_B_AIR)
        return s;
    /* vine */
    if (s >= HC_B_VINE_BASE && s < HC_B_VINE_BASE + 5) {
        if (dir == 0) /* DOWN → 불변 */
            return s;
        return edge_vine_update(e, s, x, y, z);
    }
    /* glow_lichen (MultifaceBlock) */
    if (s >= HC_B_GLOW_LICHEN_BASE && s < HC_B_GLOW_LICHEN_BASE + 126) {
        /* facemask 비트 = down,east,north,south,up,west; dir(MC 서수) 매핑 */
        static const uint8_t LB[6] = {0, 4, 2, 3, 5, 1};
        int wl = (s - HC_B_GLOW_LICHEN_BASE) / 63;
        int mask = (s - HC_B_GLOW_LICHEN_BASE) % 63 + 1;
        if ((mask >> LB[dir]) & 1) {
            if (!edge_attach_ok(ns)) {
                mask &= ~(1 << LB[dir]);
                if (mask == 0)
                    return HC_B_AIR;
                return hc_block_glow_lichen(mask, wl);
            }
        }
        return s;
    }
    /* cocoa: dir == FACING && 그 방향 블록이 #supports_cocoa 아니면 AIR */
    if (s >= HC_B_COCOA_BASE && s < HC_B_COCOA_BASE + 12) {
        static const uint8_t FD[4] = {2, 5, 3, 4}; /* N,E,S,W → MC 서수 */
        static const int8_t FS[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
        int f = (s - HC_B_COCOA_BASE) % 4;
        if (dir == FD[f] &&
            !mask_test(e->reg->tag_supports_cocoa,
                       hc_feat_get_block(e->rg, x + FS[f][0], y,
                                         z + FS[f][1])))
            return HC_B_AIR;
        return s;
    }
    /* VegetationBlock 단순 계열: !canSurvive → AIR (방향 무관).
     * bush/firefly_bush 는 VegetationBlock 무오버라이드 (26.2 javap) */
    if (s == HC_B_SHORT_GRASS || s == HC_B_FERN || s == HC_B_POPPY ||
        s == HC_B_DANDELION || s == HC_B_BUSH || s == HC_B_FIREFLY_BUSH)
        return mask_test(e->reg->tag_supports_vegetation,
                         hc_feat_get_block(e->rg, x, y - 1, z))
                   ? s
                   : HC_B_AIR;
    if (s == HC_B_AZALEA || s == HC_B_FLOWERING_AZALEA)
        return mask_test(e->reg->tag_supports_azalea,
                         hc_feat_get_block(e->rg, x, y - 1, z))
                   ? s
                   : HC_B_AIR;
    /* DoublePlantBlock: tall_grass + small_dripleaf (R5 §4) */
    if (s == HC_B_TALL_GRASS_LOWER || s == HC_B_TALL_GRASS_UPPER ||
        (s >= HC_B_SMALL_DRIPLEAF_BASE && s < HC_B_SMALL_DRIPLEAF_BASE + 16)) {
        int dp_grass = (s == HC_B_TALL_GRASS_LOWER || s == HC_B_TALL_GRASS_UPPER);
        int upper = dp_grass ? (s == HC_B_TALL_GRASS_UPPER)
                             : (((s - HC_B_SMALL_DRIPLEAF_BASE) / 2) & 1);
        int ns_same = dp_grass ? (ns == HC_B_TALL_GRASS_LOWER ||
                                  ns == HC_B_TALL_GRASS_UPPER)
                               : (ns >= HC_B_SMALL_DRIPLEAF_BASE &&
                                  ns < HC_B_SMALL_DRIPLEAF_BASE + 16);
        int ns_upper = 0;
        if (ns_same)
            ns_upper = dp_grass ? (ns == HC_B_TALL_GRASS_UPPER)
                                : (((ns - HC_B_SMALL_DRIPLEAF_BASE) / 2) & 1);
        if ((dir == 0 || dir == 1) && (!upper) == (dir == 1)) {
            if (!ns_same || ns_upper == upper)
                return HC_B_AIR;
        }
        /* canSurvive (LOWER&&DOWN 검사와 super 검사가 같은 판정으로 합침 —
         * 두 경로 다 !canSurvive → AIR) */
        uint16_t below = hc_feat_get_block(e->rg, x, y - 1, z);
        int ok;
        if (upper) {
            int below_same = dp_grass ? (below == HC_B_TALL_GRASS_LOWER)
                                      : (below >= HC_B_SMALL_DRIPLEAF_BASE &&
                                         below < HC_B_SMALL_DRIPLEAF_BASE + 16 &&
                                         !(((below - HC_B_SMALL_DRIPLEAF_BASE) /
                                            2) &
                                           1));
            ok = below_same;
        } else if (dp_grass) {
            ok = mask_test(e->reg->tag_supports_vegetation, below);
        } else {
            /* SmallDripleafBlock.mayPlaceOn (R1 §5) */
            ok = mask_test(e->reg->tag_supports_small_dripleaf, below) ||
                 (hc_block_fluid_is_water(
                      hc_feat_get_block(e->rg, x, y + 1, z)) &&
                  mask_test(e->reg->tag_supports_vegetation, below));
        }
        return ok ? s : HC_B_AIR;
    }
    /* moss_carpet (CarpetBlock): 아래 공기 → AIR */
    if (s == HC_B_MOSS_CARPET)
        return hc_block_is_air(hc_feat_get_block(e->rg, x, y - 1, z))
                   ? HC_B_AIR
                   : s;
    /* spore_blossom: dir==UP && !canSurvive → AIR */
    if (s == HC_B_SPORE_BLOSSOM) {
        if (dir == 1) {
            uint16_t above = hc_feat_get_block(e->rg, x, y + 1, z);
            if (!hc_block_is_full_cube(above) ||
                hc_block_fluid_is_water(hc_feat_get_block(e->rg, x, y, z)))
                return HC_B_AIR;
        }
        return s;
    }
    /* cave_vines 머리 (GrowingPlantHead, growth DOWN) */
    if (s >= HC_B_CAVE_VINES_BASE && s < HC_B_CAVE_VINES_BASE + 52) {
        int berries = (s - HC_B_CAVE_VINES_BASE) / 26;
        if (dir == 1) { /* growthDirection.getOpposite() = UP */
            /* canSurvive: 위가 head|body || isFaceSturdy(위) */
            uint16_t above = hc_feat_get_block(e->rg, x, y + 1, z);
            int can = (above >= HC_B_CAVE_VINES_BASE &&
                       above < HC_B_CAVE_VINES_PLANT_BASE + 2) ||
                      hc_block_is_full_cube(above);
            if (can) {
                uint16_t below = hc_feat_get_block(e->rg, x, y - 1, z);
                if (below >= HC_B_CAVE_VINES_BASE &&
                    below < HC_B_CAVE_VINES_PLANT_BASE + 2)
                    return (uint16_t)(HC_B_CAVE_VINES_PLANT_BASE + berries);
            }
        }
        if (dir == 0 && ns >= HC_B_CAVE_VINES_BASE &&
            ns < HC_B_CAVE_VINES_PLANT_BASE + 2)
            return (uint16_t)(HC_B_CAVE_VINES_PLANT_BASE + berries);
        return s;
    }
    /* cave_vines_plant (GrowingPlantBody): head 전환은 region random 을
     * 소비 (worldgen_region_random — 미모델) — 도달 시 즉사 */
    if (s >= HC_B_CAVE_VINES_PLANT_BASE && s < HC_B_CAVE_VINES_PLANT_BASE + 2) {
        if (dir == 0 && !(ns >= HC_B_CAVE_VINES_BASE &&
                          ns < HC_B_CAVE_VINES_PLANT_BASE + 2))
            die("edge update: cave_vines_plant→head conversion "
                "(region random) reached",
                NULL);
        return s;
    }
    /* bamboo: dir==UP && 이웃 bamboo 의 age 가 크면 age cycle */
    if (s >= HC_B_BAMBOO_BASE && s < HC_B_BAMBOO_BASE + 12) {
        if (dir == 1 && ns >= HC_B_BAMBOO_BASE && ns < HC_B_BAMBOO_BASE + 12) {
            int sage = (s - HC_B_BAMBOO_BASE) / 6;
            int nage = (ns - HC_B_BAMBOO_BASE) / 6;
            if (nage > sage)
                return (uint16_t)(s + 6); /* age 0→1 (cycle) */
        }
        return s;
    }
    /* big_dripleaf: DOWN&&!canSurvive → AIR; UP&&이웃 big_dripleaf → stem */
    if (s >= HC_B_BIG_DRIPLEAF_BASE && s < HC_B_BIG_DRIPLEAF_BASE + 8) {
        if (dir == 0) {
            uint16_t below = hc_feat_get_block(e->rg, x, y - 1, z);
            int ok = (below >= HC_B_BIG_DRIPLEAF_BASE &&
                      below < HC_B_BIG_DRIPLEAF_STEM_BASE + 8) ||
                     mask_test(e->reg->tag_supports_big_dripleaf, below);
            if (!ok)
                return HC_B_AIR;
        }
        if (dir == 1 && ns >= HC_B_BIG_DRIPLEAF_BASE &&
            ns < HC_B_BIG_DRIPLEAF_BASE + 8)
            return (uint16_t)(HC_B_BIG_DRIPLEAF_STEM_BASE +
                              (s - HC_B_BIG_DRIPLEAF_BASE));
        return s;
    }
    /* 소형 버섯: MushroomBlock.canSurvive = below∈#mushroom_grow_block ||
     * (getRawBrightness(pos,0) < 13 && below.isSolidRender) — 라이트를
     * 라이브로 읽는다 (WorldGenRegion.getLightEngine @386 → ServerLevel).
     * 월드젠 윈도우 분석 (Task 10 NOTES): 엣지 업데이트 시점에 버섯 청크
     * 섹션은 그리드 08 인접-등록으로 존재(스카이 저장값 0)하고, 주변 링
     * 청크의 09 는 아직 자격 미달 → rawBrightness = 0 < 13 → 생존.
     * (버섯은 dampening/emission 0 이라 라이트 게이트에 비가시 — 잔차는
     * 전이적 read 경유만 가능. 게이트가 이 근방을 지목하면 재검토.) */
    if (s == HC_B_RED_MUSHROOM || s == HC_B_BROWN_MUSHROOM) {
        uint16_t below = hc_feat_get_block(e->rg, x, y - 1, z);
        fprintf(stderr,
                "hc_features note: mushroom edge-update at (%d,%d,%d) "
                "below=%s -> modeled as rawBrightness 0 (survives)\n",
                x, y, z, hc_block_name(below));
        return s;
    }
    /* seagrass (SeagrassBlock.java:45-72): super(VegetationBlock) 결과
     * 비공기면 물 소스 틱 — 매 평가마다. mayPlaceOn = 아래 sturdy(UP) &&
     * !#cannot_support_seagrass(=magma). */
    if (s == HC_B_SEAGRASS) {
        uint16_t r = s;
        if (dir == 0 &&
            (!t14_face_sturdy(ns, 1) || ns == HC_B_MAGMA_BLOCK))
            r = HC_B_AIR;
        if (r != HC_B_AIR)
            hc_feat_schedule_tick(e->rg, x, y, z, s, HC_TICK_WATER, 0);
        return r;
    }
    /* tall_seagrass (DoublePlantBlock.updateShape + TallSeagrass
     * canSurvive; 자기 위치 유체는 항상 물 소스라 mayPlaceOn 로 축약) */
    if (s == HC_B_TALL_SEAGRASS_LOWER || s == HC_B_TALL_SEAGRASS_UPPER) {
        int upper = s == HC_B_TALL_SEAGRASS_UPPER;
        int vert = dir == 0 || dir == 1;
        int ns_same = ns == HC_B_TALL_SEAGRASS_LOWER ||
                      ns == HC_B_TALL_SEAGRASS_UPPER;
        int ns_upper = ns == HC_B_TALL_SEAGRASS_UPPER;
        if (vert && (!upper) == (dir == 1) &&
            !(ns_same && ns_upper != upper))
            return HC_B_AIR; /* 반대 반토막 자리가 유효하지 않음 */
        if (!upper && dir == 0 &&
            (!t14_face_sturdy(ns, 1) || ns == HC_B_MAGMA_BLOCK))
            return HC_B_AIR;
        return s;
    }
    /* kelp 머리 (GrowingPlantHeadBlock.updateShape, scheduleFluidTicks) */
    if (s >= HC_B_KELP_BASE && s < HC_B_KELP_BASE + 4) {
        int ns_kelp = ns >= HC_B_KELP_BASE && ns <= HC_B_KELP_PLANT;
        if (dir == 0) {
            int can = (ns >= HC_B_KELP_BASE && ns <= HC_B_KELP_PLANT) ||
                      (t14_face_sturdy(ns, 1) && ns != HC_B_MAGMA_BLOCK);
            if (!can)
                hc_feat_schedule_tick(e->rg, x, y, z, s, HC_TICK_BLOCK, 0);
            else {
                uint16_t above = hc_feat_get_block(e->rg, x, y + 1, z);
                if (above >= HC_B_KELP_BASE && above <= HC_B_KELP_PLANT)
                    return HC_B_KELP_PLANT;
            }
        }
        if (dir != 1 || !ns_kelp) {
            hc_feat_schedule_tick(e->rg, x, y, z, s, HC_TICK_WATER, 0);
            return s;
        }
        return HC_B_KELP_PLANT;
    }
    /* kelp_plant (GrowingPlantBodyBlock.updateShape) — 머리 전환은
     * 지역 랜덤 age 드로우 (재현 범위 밖) — 도달 즉사 */
    if (s == HC_B_KELP_PLANT) {
        if (dir == 0) {
            int can = (ns >= HC_B_KELP_BASE && ns <= HC_B_KELP_PLANT) ||
                      (t14_face_sturdy(ns, 1) && ns != HC_B_MAGMA_BLOCK);
            if (!can)
                hc_feat_schedule_tick(e->rg, x, y, z, s, HC_TICK_BLOCK, 0);
        }
        if (dir == 1 &&
            !(ns >= HC_B_KELP_BASE && ns <= HC_B_KELP_PLANT))
            die("edge update: kelp_plant→head conversion (region random) "
                "reached",
                NULL);
        hc_feat_schedule_tick(e->rg, x, y, z, s, HC_TICK_WATER, 0);
        return s;
    }
    /* T14 템플릿 패밀리 (stairs/fence/door/chest/slab/trapdoor) */
    if (s >= HC_B_T14_BASE) {
        int fam = t14_family(s);
        if (fam != TF_NONE)
            return t14_update_state(e, fam, s, x, y, z, dir, ns);
    }
    /* 그 외 (잎/통나무/물/돌/흙/이끼 등): tick 스케줄만 — 불변 */
    return s;
}

/* 한 면 이벤트: s1/s2 사전 읽기, s3/s4 계산·조건부 쓰기 (R5 §2) */
static void edge_face(feat_env_t *e, int32_t x, int32_t y, int32_t z,
                      int dir) {
    int32_t nx = x + DIR6[dir][0], ny = y + DIR6[dir][1],
            nz = z + DIR6[dir][2];
    uint16_t s1 = hc_feat_get_block(e->rg, x, y, z);
    uint16_t s2 = hc_feat_get_block(e->rg, nx, ny, nz);
    edge_schedule_ticks(e, s1, x, y, z, dir, s2);
    uint16_t s3 = edge_update_state(e, s1, x, y, z, dir, s2);
    if (s3 != s1)
        hc_feat_set_block(e->rg, x, y, z, s3);
    edge_schedule_ticks(e, s2, nx, ny, nz, D_OPP6[dir], s3);
    uint16_t s4 = edge_update_state(e, s2, nx, ny, nz, D_OPP6[dir], s3);
    if (s4 != s2)
        hc_feat_set_block(e->rg, nx, ny, nz, s4);
}

/* ---------- Task 14 익스포트 (structures_template.c 가 사용) ---------- */

uint16_t hc_featx_edge_state(feat_env_t *e, uint16_t s, int32_t x, int32_t y,
                             int32_t z, int dir_mc, uint16_t ns) {
    return edge_update_state(e, s, x, y, z, dir_mc, ns);
}

void hc_featx_edge_ticks(feat_env_t *e, uint16_t s, int32_t x, int32_t y,
                         int32_t z, int dir_mc, uint16_t ns) {
    edge_schedule_ticks(e, s, x, y, z, dir_mc, ns);
}

void hc_featx_edge_face(feat_env_t *e, int32_t x, int32_t y, int32_t z,
                        int dir_mc) {
    edge_face(e, x, y, z, dir_mc);
}

int hc_featx_face_sturdy_t14(uint16_t s, int dir_mc) {
    return t14_face_sturdy(s, dir_mc);
}

/* forAllFaces: Z 패스 → Y 패스 → X 패스 (R5 §3) */
static void update_shape_at_edge(feat_env_t *e, const shape_t *box) {
    int32_t sx = box->max_x - box->min_x + 1;
    int32_t sy = box->max_y - box->min_y + 1;
    int32_t sz = box->max_z - box->min_z + 1;
    /* Z 패스: rising→NORTH(2), falling→SOUTH(3) */
    for (int32_t x = 0; x < sx; x++)
        for (int32_t y = 0; y < sy; y++) {
            int prev = 0;
            for (int32_t z = 0; z <= sz; z++) {
                int cur = z != sz && shape_full(box, box->min_x + x,
                                                box->min_y + y,
                                                box->min_z + z);
                if (!prev && cur)
                    edge_face(e, box->min_x + x, box->min_y + y,
                              box->min_z + z, 2);
                if (prev && !cur)
                    edge_face(e, box->min_x + x, box->min_y + y,
                              box->min_z + z - 1, 3);
                prev = cur;
            }
        }
    /* Y 패스: rising→DOWN(0), falling→UP(1) */
    for (int32_t z = 0; z < sz; z++)
        for (int32_t x = 0; x < sx; x++) {
            int prev = 0;
            for (int32_t y = 0; y <= sy; y++) {
                int cur = y != sy && shape_full(box, box->min_x + x,
                                                box->min_y + y,
                                                box->min_z + z);
                if (!prev && cur)
                    edge_face(e, box->min_x + x, box->min_y + y,
                              box->min_z + z, 0);
                if (prev && !cur)
                    edge_face(e, box->min_x + x, box->min_y + y - 1,
                              box->min_z + z, 1);
                prev = cur;
            }
        }
    /* X 패스: rising→WEST(4), falling→EAST(5) */
    for (int32_t y = 0; y < sy; y++)
        for (int32_t z = 0; z < sz; z++) {
            int prev = 0;
            for (int32_t x = 0; x <= sx; x++) {
                int cur = x != sx && shape_full(box, box->min_x + x,
                                                box->min_y + y,
                                                box->min_z + z);
                if (!prev && cur)
                    edge_face(e, box->min_x + x, box->min_y + y,
                              box->min_z + z, 4);
                if (prev && !cur)
                    edge_face(e, box->min_x + x - 1, box->min_y + y,
                              box->min_z + z, 5);
                prev = cur;
            }
        }
}

/* ================= TreeFeature.place (R2 §3) ================= */

static int32_t two_layers_size(const hc_tree_cfg_t *c, int32_t y) {
    return y < c->ts_limit ? c->ts_lower : c->ts_upper;
}

int hc_featx_tree_place(feat_env_t *e, int32_t ox, int32_t oy, int32_t oz) {
    const hc_tree_cfg_t *cfg = e->pf->cf.tree;
    static tree_ctx_t t_storage;
    tree_ctx_t *t = &t_storage;
    t->e = e;
    t->cfg = cfg;
    jset_init(&t->roots);
    jset_init(&t->logs);
    jset_init(&t->leaves);
    jset_init(&t->deco);

    /* doPlace */
    int32_t height = cfg->base_height +
                     hc_wgr_next_int(e->rng, cfg->rand_a + 1) +
                     hc_wgr_next_int(e->rng, cfg->rand_b + 1); /* 2 draws */
    int32_t fh = cfg->fol_height; /* foliageHeight — 0 draws (전 placer) */
    int32_t rad =
        iprov_sample(e->rng, &cfg->fol_radius); /* 상수 → 0 draws */
    /* rootPos = origin (root placer 없음) */
    if (!(oy >= HC_MIN_Y + 1 && oy + height + 1 <= HC_MAX_Y + 1))
        return 0;
    /* getMaxFreeTreeHeight — 상계는 height+1 (TreeFeature@100: 나무 위
     * 1층까지 검사; R2-trees §전사오류 정정, Task 14 감사). 층 height+1
     * 에서 첫 장애물이면 max_free = height-1 로 클립. */
    int32_t max_free = height;
    for (int32_t y = 0; y <= height + 1; y++) {
        int32_t size = two_layers_size(cfg, y);
        int found = 0;
        for (int32_t dx = -size; dx <= size && !found; dx++)
            for (int32_t dz = -size; dz <= size; dz++) {
                int32_t px = ox + dx, py = oy + y, pz = oz + dz;
                if (!is_free_pos(e, px, py, pz) ||
                    (!cfg->ignore_vines && is_vine_pos(e, px, py, pz))) {
                    max_free = y - 2;
                    found = 1;
                    break;
                }
            }
        if (found)
            break;
    }
    if (max_free < height &&
        (cfg->ts_min_clipped < 0 || max_free < cfg->ts_min_clipped))
        return 0;

    static attach_t atts[MAX_ATTACH];
    int32_t n_att;
    switch (cfg->trunk_kind) {
    case HC_TRUNK_STRAIGHT:
        n_att = trunk_straight(t, ox, oy, oz, max_free, atts);
        break;
    case HC_TRUNK_MEGA_JUNGLE:
        n_att = trunk_mega_jungle(t, ox, oy, oz, max_free, atts);
        break;
    case HC_TRUNK_FANCY:
        n_att = trunk_fancy(t, ox, oy, oz, max_free, atts);
        break;
    case HC_TRUNK_BENDING:
        n_att = trunk_bending(t, ox, oy, oz, max_free, atts);
        break;
    case HC_TRUNK_FORKING:
        n_att = trunk_forking(t, ox, oy, oz, max_free, atts);
        break;
    default:
        die("unknown trunk placer kind", NULL);
        return 0;
    }
    for (int32_t i = 0; i < n_att; i++)
        create_foliage(t, &atts[i], fh, rad);

    /* place() 마무리 */
    if (t->logs.size == 0 && t->leaves.size == 0)
        return 0;
    run_decorators(t, cfg->decorators, cfg->n_decorators, &t->logs,
                   &t->leaves, &t->deco);
    shape_t *box = update_leaves(t);
    /* StructureTemplate.updateShapeAtEdge — 경계 vine 등 지지 재계산 (R5;
     * R2 §8 의 no-op 가정은 golden 07 diff 로 반증됐다) */
    if (box)
        update_shape_at_edge(e, box);
    return 1;
}

/* ================= FallenTreeFeature (R2 §10) ================= */

int hc_featx_ftree_place(feat_env_t *e, int32_t ox, int32_t oy, int32_t oz) {
    const hc_ftree_cfg_t *cfg = e->pf->cf.ftree;
    static tree_ctx_t t_storage;
    tree_ctx_t *t = &t_storage;
    static hc_tree_cfg_t fake_cfg; /* run_decorators 는 t->cfg 를 안 읽는다 */
    t->e = e;
    t->cfg = &fake_cfg;
    jset_init(&t->logs);
    jset_init(&t->leaves);
    jset_init(&t->deco);
    jset_init(&t->roots);

    /* placeStump: 무조건 쓰기 (flag 3) + stump 데코레이터.
     * placeLogBlock @31-35: 로그마다 무조건 markAboveForPostProcessing
     * (열린 하늘 아래선 위가 공기라 no-mark — Task 13). */
    hc_feat_set_block(e->rg, ox, oy, oz, cfg->trunk_state);
    hc_feat_mark_above(e->rg, ox, oy, oz);
    jset_add(&t->logs, ox, oy, oz);
    run_decorators(t, cfg->stump_dec, cfg->n_stump_dec, &t->logs, &t->leaves,
                   &t->deco);

    /* HORIZONTAL.getRandomDirection = [N,E,S,W][nextInt(4)] */
    int32_t d = hc_wgr_next_int(e->rng, 4);
    int32_t len = iprov_sample(e->rng, &cfg->log_length) - 2;
    int32_t off = 2 + hc_wgr_next_int(e->rng, 2);
    int32_t mx = ox + HORIZ[d].dx * off, my = oy, mz = oz + HORIZ[d].dz * off;

    /* setGroundHeightForFallenLogStartPos */
    my += 1;
    for (int32_t i = 0; i < 6; i++) {
        if (valid_tree_pos(e, mx, my, mz) &&
            over_solid_ground(e, mx, my, mz))
            break;
        my -= 1;
    }

    /* canPlaceEntireFallenLog */
    int ok = 1;
    {
        int32_t gap = 0, cx = mx, cy = my, cz = mz;
        for (int32_t i = 0; i < len; i++) {
            if (!valid_tree_pos(e, cx, cy, cz)) {
                ok = 0;
                break;
            }
            if (!over_solid_ground(e, cx, cy, cz)) {
                if (++gap > 2) {
                    ok = 0;
                    break;
                }
            } else {
                gap = 0;
            }
            cx += HORIZ[d].dx;
            cz += HORIZ[d].dz;
        }
    }
    if (ok) {
        /* placeFallenLog: HashSet 에 모아 attached_to_logs 데코 */
        jset_t *flogs = &t->roots; /* 빈 셋 재사용 (스텀프 로그와 분리) */
        int32_t cx = mx, cy = my, cz = mz;
        int     axis = HORIZ[d].dx != 0 ? 0 : 2;
        for (int32_t i = 0; i < len; i++) {
            hc_feat_set_block(e->rg, cx, cy, cz,
                              log_with_axis(cfg->trunk_state, axis));
            hc_feat_mark_above(e->rg, cx, cy, cz); /* placeLogBlock @31-35 */
            jset_add(flogs, cx, cy, cz);
            cx += HORIZ[d].dx;
            cz += HORIZ[d].dz;
        }
        run_decorators(t, cfg->log_dec, cfg->n_log_dec, flogs, &t->leaves,
                       &t->deco);
    }
    return 1; /* place() 는 무조건 true */
}
