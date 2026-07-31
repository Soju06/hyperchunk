#include "hc_carvers.h" /* hc_mth_sin/cos (Mth 테이블 — mega jungle 가지) */
#include "features_internal.h"
#include "hc_jdk_trig.h" /* fancy trunk 의 Math.sin/cos (JDK 인트린식) */

#include <assert.h>
#include <math.h>
#include <string.h>

/* TreeFeature + trunk/foliage placer + decorator + FallenTreeFeature —
 * 전부 26.2 바이트코드 재구성 (.hermes/notes/task9b-features/R2). 드로우
 * 순서/반복 순서/자바 HashSet 순회 순서가 비트 정확성의 전부다. */

#define die hc_featx_die
#define mask_test hc_featx_mask_test
#define iprov_sample hc_featx_iprov_sample
#define sprov_sample hc_featx_sprov_sample

/* ================= Java HashSet<BlockPos> 순회 에뮬레이션 =================
 *
 * 왜: TreeDecorator$Context 는 HashSet 을 ObjectArrayList 로 복사한 뒤
 * getY 안정 정렬한다 — 같은 y 끼리는 HashSet 순회 순서가 남고, 데코레이터
 * 드로우가 그 순서를 따른다. updateLeaves 의 레벨 셋 poll 도 HashSet
 * 순서다. (R2 §12)
 *
 * JDK HashMap 시맨틱: cap 16 시작(0.75), index = (cap-1) & (h ^ h>>>16),
 * 체인 꼬리 삽입, 삽입 후 size > threshold 면 리사이즈(버킷을 lo/hi 로
 * 상대순서 보존 분할), 한 버킷이 8개가 되면 cap<64 → 리사이즈 / cap>=64 →
 * treeify(미이식 — 즉사). 순회 = 버킷 오름차순, 체인 head→tail.
 * BlockPos.hashCode = (y + z*31)*31 + x (Vec3i). */

enum { JSET_MAX_ENTRIES = 8192, JSET_MAX_CAP = 16384 };

typedef struct {
    int32_t x, y, z;
    int32_t next; /* 체인 다음 엔트리 인덱스, -1 끝 */
    uint32_t hash; /* spread 전 원시 hashCode */
    uint8_t  dead; /* remove 후 1 (엔트리 배열은 재사용 안 함) */
} jent_t;

typedef struct {
    jent_t  ent[JSET_MAX_ENTRIES];
    int32_t n_ent;
    int32_t bucket[JSET_MAX_CAP]; /* head 엔트리 인덱스, -1 빔 */
    int32_t cap;                  /* 2^k */
    int32_t size, threshold;
} jset_t;

static void jset_init(jset_t *s) {
    s->n_ent = 0;
    s->cap = 16;
    s->size = 0;
    s->threshold = 12;
    for (int32_t i = 0; i < s->cap; i++)
        s->bucket[i] = -1;
}

static uint32_t jpos_hash(int32_t x, int32_t y, int32_t z) {
    return (uint32_t)((y + z * 31) * 31 + x);
}

static int32_t jset_index(const jset_t *s, uint32_t h) {
    uint32_t spread = h ^ (h >> 16);
    return (int32_t)((uint32_t)(s->cap - 1) & spread);
}

static void jset_resize(jset_t *s) {
    int32_t old_cap = s->cap;
    if (old_cap * 2 > JSET_MAX_CAP)
        die("jset resize overflow", NULL);
    s->cap = old_cap * 2;
    s->threshold = (int32_t)((double)s->cap * 0.75);
    /* JDK 8 split: 각 구버킷 체인을 (hash & oldCap) 비트로 lo/hi 분할,
     * 상대순서 보존; lo → j, hi → j+oldCap */
    for (int32_t j = 0; j < old_cap; j++) {
        int32_t head = s->bucket[j];
        int32_t lo_head = -1, lo_tail = -1, hi_head = -1, hi_tail = -1;
        int32_t e = head;
        while (e >= 0) {
            int32_t nxt = s->ent[e].next;
            uint32_t spread = s->ent[e].hash ^ (s->ent[e].hash >> 16);
            s->ent[e].next = -1;
            if ((spread & (uint32_t)old_cap) == 0) {
                if (lo_tail < 0)
                    lo_head = e;
                else
                    s->ent[lo_tail].next = e;
                lo_tail = e;
            } else {
                if (hi_tail < 0)
                    hi_head = e;
                else
                    s->ent[hi_tail].next = e;
                hi_tail = e;
            }
            e = nxt;
        }
        s->bucket[j] = lo_head;
        s->bucket[j + old_cap] = hi_head;
    }
}

/* HashSet.add — 이미 있으면 false (순서 불변) */
static int jset_add(jset_t *s, int32_t x, int32_t y, int32_t z) {
    uint32_t h = jpos_hash(x, y, z);
    int32_t  idx = jset_index(s, h);
    int32_t  e = s->bucket[idx];
    int32_t  tail = -1, bin = 0;
    while (e >= 0) {
        if (s->ent[e].x == x && s->ent[e].y == y && s->ent[e].z == z)
            return 0;
        tail = e;
        bin++;
        e = s->ent[e].next;
    }
    if (s->n_ent >= JSET_MAX_ENTRIES)
        die("jset entries overflow", NULL);
    int32_t ne = s->n_ent++;
    s->ent[ne].x = x;
    s->ent[ne].y = y;
    s->ent[ne].z = z;
    s->ent[ne].hash = h;
    s->ent[ne].next = -1;
    s->ent[ne].dead = 0;
    if (tail < 0)
        s->bucket[idx] = ne;
    else
        s->ent[tail].next = ne;
    /* treeifyBin 경로: 삽입 후 체인 8개 도달 (binCount>=7 로 진입) */
    if (bin + 1 >= 8) {
        if (s->cap < 64)
            jset_resize(s);
        else
            die("jset treeify reached — not ported (R2 §12)", NULL);
    }
    if (++s->size > s->threshold)
        jset_resize(s);
    return 1;
}

/* 순회: 버킷 오름차순 → 체인 순. it = {bucket, entry} */
typedef struct {
    const jset_t *s;
    int32_t       b, e;
} jit_t;

static void jit_begin(jit_t *it, const jset_t *s) {
    it->s = s;
    it->b = -1;
    it->e = -1;
    /* 첫 원소로 전진 */
    for (int32_t b = 0; b < s->cap; b++)
        if (s->bucket[b] >= 0) {
            it->b = b;
            it->e = s->bucket[b];
            return;
        }
    it->b = s->cap;
}

static int jit_valid(const jit_t *it) {
    return it->b < it->s->cap && it->e >= 0;
}

static void jit_next(jit_t *it) {
    const jset_t *s = it->s;
    if (s->ent[it->e].next >= 0) {
        it->e = s->ent[it->e].next;
        return;
    }
    for (int32_t b = it->b + 1; b < s->cap; b++)
        if (s->bucket[b] >= 0) {
            it->b = b;
            it->e = s->bucket[b];
            return;
        }
    it->b = s->cap;
    it->e = -1;
}

/* 첫 원소 꺼내 제거 (updateLeaves 의 it.next()+it.remove()) */
static int jset_poll_first(jset_t *s, int32_t *x, int32_t *y, int32_t *z) {
    for (int32_t b = 0; b < s->cap; b++) {
        int32_t e = s->bucket[b];
        if (e >= 0) {
            *x = s->ent[e].x;
            *y = s->ent[e].y;
            *z = s->ent[e].z;
            s->bucket[b] = s->ent[e].next;
            s->ent[e].dead = 1;
            s->size--;
            return 1;
        }
    }
    return 0;
}

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

static int is_vine_pos(feat_env_t *e, int32_t x, int32_t y, int32_t z) {
    uint16_t s = hc_feat_get_block(e->rg, x, y, z);
    return s >= HC_B_VINE_BASE && s < HC_B_VINE_BASE + 5;
}

/* isOverSolidGround = 아래 블록 isFaceSturdy(UP) — support shape (잎 제외) */
static int over_solid_ground(feat_env_t *e, int32_t x, int32_t y, int32_t z) {
    return hc_block_is_full_cube(hc_feat_get_block(e->rg, x, y - 1, z));
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
    hc_feat_set_block(t->e->rg, x, y, z, state);
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
    uint16_t st = t->cfg->foliage_state;
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
    jset_add(deco, x, y, z);
    hc_feat_set_block(t->e->rg, x, y, z,
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
            hc_feat_set_block(e->rg, px, logs[i].y, pz, st);
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
    for (int32_t i = 0; i < n_leaves; i++) {
        int32_t x = leaves[i].x, y = leaves[i].y, z = leaves[i].z;
        if (hc_wgr_next_float(e->rng) < prob && ctx_is_air(e, x - 1, y, z))
            dec_hanging_vine(t, deco, x - 1, y, z, 0); /* west, EAST face */
        if (hc_wgr_next_float(e->rng) < prob && ctx_is_air(e, x + 1, y, z))
            dec_hanging_vine(t, deco, x + 1, y, z, 4);
        if (hc_wgr_next_float(e->rng) < prob && ctx_is_air(e, x, y, z - 1))
            dec_hanging_vine(t, deco, x, y, z - 1, 2);
        if (hc_wgr_next_float(e->rng) < prob && ctx_is_air(e, x, y, z + 1))
            dec_hanging_vine(t, deco, x, y, z + 1, 1);
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
            hc_feat_set_block(e->rg, px, py, pz, st);
        }
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

/* getOptionalDistanceAt: #prevents_nearby_leaf_decay → 0; DISTANCE 프로퍼티
 * (잎) → 값; 그 외 empty(-1) */
static int32_t optional_distance_at(feat_env_t *e, uint16_t s) {
    if (mask_test(e->reg->tag_prevents_leaf_decay, s))
        return 0;
    if (s >= HC_B_OAK_LEAVES_BASE && s < HC_B_OAK_LEAVES_BASE + 28)
        return (s - HC_B_OAK_LEAVES_BASE) % 7 + 1;
    return -1;
}

/* 잎 상태의 distance 재기록 (wl/종 보존) */
static uint16_t leaf_with_distance(uint16_t s, int32_t d) {
    int32_t base = (s - HC_B_OAK_LEAVES_BASE) / 7 * 7 + HC_B_OAK_LEAVES_BASE;
    return (uint16_t)(base + (d - 1));
}

static void update_leaves(tree_ctx_t *t) {
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
        return;
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
            return;
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
            if (!(cur >= HC_B_OAK_LEAVES_BASE &&
                  cur < HC_B_OAK_LEAVES_BASE + 28))
                die("updateLeaves rewrite on non-leaf", hc_block_name(cur));
            hc_feat_set_block(e->rg, x, y, z, leaf_with_distance(cur, i));
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
    /* getMaxFreeTreeHeight */
    int32_t max_free = height;
    for (int32_t y = 0; y <= height; y++) {
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
    update_leaves(t);
    /* StructureTemplate.updateShapeAtEdge — 팔레트상 no-op 가정 (R2 §8) */
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

    /* placeStump: 무조건 쓰기 (flag 3) + stump 데코레이터 */
    hc_feat_set_block(e->rg, ox, oy, oz, cfg->trunk_state);
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
            jset_add(flogs, cx, cy, cz);
            cx += HORIZ[d].dx;
            cz += HORIZ[d].dz;
        }
        run_decorators(t, cfg->log_dec, cfg->n_log_dec, flogs, &t->leaves,
                       &t->deco);
    }
    return 1; /* place() 는 무조건 true */
}
