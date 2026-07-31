#include "hc_features.h"

#include <string.h>

/* reference JSON → 컴파일된 feature 레지스트리.
 *
 * 입력 계약:
 *  - order_txt: reference/features_order-26.2.txt (FeatureOrderGolden 산출)
 *  - biome_features: reference/biome_features-26.2.json
 *  - placed/configured: reference/{placed_feature,configured_feature} 로드
 *    테이블 (hc_df_source_t 규약 — 이름은 "minecraft:..." 정규화)
 *  - tags: reference/tags/block
 *
 * 실패는 전부 -1 + *err (정적 문자열) — fail-loud (ADR-009 D3).
 * walk_max_step 이하 step 의 feature 만 파이프라인/본문을 컴파일한다.
 * 모르는 modifier/앵커/프로바이더는 즉시 실패다. 본문은 ore/spring/
 * underwater_magma/monster_room 만 구현이고 나머지는 UNIMPLEMENTED
 * (파이프라인은 실행되고 본문 도달 시 placed=-1 로 표시 — 9a 그리드에선
 * npos==0 검증으로 도달하지 않음이 golden 트레이스로 증명된다). */

#define FAIL(msg)          \
    do {                   \
        *err = (msg);      \
        return -1;         \
    } while (0)

static const hc_json_t *find_source(const hc_df_source_t *tab, int32_t n,
                                    const char *name, int32_t len) {
    for (int32_t i = 0; i < n; i++)
        if ((int32_t)strlen(tab[i].name) == len &&
            memcmp(tab[i].name, name, (size_t)len) == 0)
            return tab[i].json;
    return NULL;
}

/* --- 블록 태그 확장 (carvers.c tag_mark_block 과 동일 규약: '#' 재귀;
 * 태그 값/블록 리스트는 블록 단위이고 테이블은 캐노니컬 상태명이라
 * '[' 앞부분만 대조해 그 블록의 모든 상태에 비트를 세운다. 미등재 블록은
 * 우리 스테이지가 생성 불가 → 멤버십 무의미라 스킵) --- */

static void tag_mark(uint64_t *bits, const char *name, int32_t len) {
    for (int32_t id = 0; id < HC_B_COUNT; id++) {
        const char *full = hc_block_name((uint16_t)id);
        size_t      base = strcspn(full, "[");
        if ((int32_t)base == len && memcmp(full, name, (size_t)len) == 0)
            bits[id >> 6] |= 1ull << (id & 63);
    }
}

static int tag_expand(uint64_t *bits, const char *name, int32_t len,
                      const hc_df_source_t *tags, int32_t n_tags, int depth,
                      const char **err) {
    if (depth > 8)
        FAIL("block tag recursion too deep");
    if (len > 0 && name[0] == '#') {
        const hc_json_t *tag = find_source(tags, n_tags, name + 1, len - 1);
        if (!tag)
            FAIL("referenced block tag not loaded");
        const hc_json_t *values = hc_json_get(tag, "values");
        if (!values || values->kind != HC_JSON_ARR)
            FAIL("tag values missing");
        for (const hc_json_t *v = values->child; v; v = v->next) {
            if (v->kind != HC_JSON_STR)
                FAIL("tag value not string");
            if (tag_expand(bits, v->s, v->slen, tags, n_tags, depth + 1, err))
                return -1;
        }
        return 0;
    }
    tag_mark(bits, name, len);
    return 0;
}

/* "minecraft:x" 접두 정규화: 순수 이름이면 그대로 비교에 쓰도록 (레퍼런스
 * 테이블 이름엔 이미 "minecraft:" 가 붙어 있다) */
static const hc_json_t *find_named(const hc_df_source_t *tab, int32_t n,
                                   const hc_json_t *sref) {
    return find_source(tab, n, sref->s, sref->slen);
}

/* --- 블록스테이트 {"Name":..,"Properties":{..}} → 내부 id --- */

static int32_t compile_blockstate(const hc_json_t *state, const char **err) {
    const hc_json_t *name = hc_json_get(state, "Name");
    if (!name || name->kind != HC_JSON_STR) {
        *err = "blockstate Name missing";
        return -2;
    }
    char buf[128];
    if (name->slen >= (int32_t)sizeof buf) {
        *err = "blockstate name too long";
        return -2;
    }
    memcpy(buf, name->s, (size_t)name->slen);
    int32_t len = name->slen;
    const hc_json_t *props = hc_json_get(state, "Properties");
    if (props) {
        if (props->kind != HC_JSON_OBJ || props->count != 1) {
            /* ore/spring 타겟은 최대 1 프로퍼티 (redstone lit) — 복수
             * 프로퍼티 직렬화 순서는 상태 정의 순이라 별도 확인 필요 */
            *err = "multi-property blockstate unsupported";
            return -2;
        }
        const hc_json_t *p = props->child;
        if (p->kind != HC_JSON_STR) {
            *err = "blockstate property not string";
            return -2;
        }
        if (len + p->klen + p->slen + 3 >= (int32_t)sizeof buf) {
            *err = "blockstate canonical form too long";
            return -2;
        }
        buf[len++] = '[';
        memcpy(buf + len, p->key, (size_t)p->klen);
        len += p->klen;
        buf[len++] = '=';
        memcpy(buf + len, p->s, (size_t)p->slen);
        len += p->slen;
        buf[len++] = ']';
    }
    int32_t id = hc_block_by_name(buf, len);
    if (id < 0)
        *err = "blockstate not in hc_blocks table";
    return id; /* -1 = 미등재 */
}

/* --- VerticalAnchor / providers --- */

static int anchor_resolve(const hc_json_t *a, int32_t *out, const char **err) {
    /* 오버월드 고정: minGenY -64, genDepth 384 (task9a A2 §3.3) */
    const hc_json_t *v;
    if ((v = hc_json_get(a, "absolute")) && v->kind == HC_JSON_NUM) {
        *out = (int32_t)v->num;
        return 0;
    }
    if ((v = hc_json_get(a, "above_bottom")) && v->kind == HC_JSON_NUM) {
        *out = -64 + (int32_t)v->num;
        return 0;
    }
    if ((v = hc_json_get(a, "below_top")) && v->kind == HC_JSON_NUM) {
        *out = 319 - (int32_t)v->num;
        return 0;
    }
    FAIL("unknown vertical anchor");
}

static int iprov_compile(const hc_json_t *j, hc_iprov_t *p, const char **err) {
    if (j->kind == HC_JSON_NUM) {
        p->kind = HC_IP_CONST;
        p->a = (int32_t)j->num;
        return 0;
    }
    const hc_json_t *t = hc_json_get(j, "type");
    if (t && hc_json_streq(t, "minecraft:constant")) {
        const hc_json_t *v = hc_json_get(j, "value");
        if (!v || v->kind != HC_JSON_NUM)
            FAIL("constant int provider without value");
        p->kind = HC_IP_CONST;
        p->a = (int32_t)v->num;
        return 0;
    }
    if (t && hc_json_streq(t, "minecraft:uniform")) {
        const hc_json_t *lo = hc_json_get(j, "min_inclusive");
        const hc_json_t *hi = hc_json_get(j, "max_inclusive");
        if (!lo || !hi || lo->kind != HC_JSON_NUM || hi->kind != HC_JSON_NUM)
            FAIL("uniform int provider bounds missing");
        p->kind = HC_IP_UNIFORM;
        p->a = (int32_t)lo->num;
        p->b = (int32_t)hi->num;
        return 0;
    }
    /* clamped_normal 등 — 샘플 도달 시 즉사 (9b) */
    p->kind = HC_IP_UNSUPPORTED_9B;
    return 0;
}

static int hprov_compile(const hc_json_t *j, hc_hprov_t *p, const char **err) {
    const hc_json_t *t = hc_json_get(j, "type");
    if (!t || t->kind != HC_JSON_STR)
        FAIL("height provider without type");
    const hc_json_t *lo = hc_json_get(j, "min_inclusive");
    const hc_json_t *hi = hc_json_get(j, "max_inclusive");
    if (!lo || !hi)
        FAIL("height provider bounds missing");
    if (anchor_resolve(lo, &p->min_y, err) || anchor_resolve(hi, &p->max_y, err))
        return -1;
    p->plateau = 0;
    p->inner = 0;
    if (hc_json_streq(t, "minecraft:uniform")) {
        p->kind = HC_HP_UNIFORM;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:trapezoid")) {
        p->kind = HC_HP_TRAPEZOID;
        const hc_json_t *pl = hc_json_get(j, "plateau");
        if (pl) {
            if (pl->kind != HC_JSON_NUM)
                FAIL("trapezoid plateau not a number");
            p->plateau = (int32_t)pl->num;
        }
        return 0;
    }
    if (hc_json_streq(t, "minecraft:very_biased_to_bottom")) {
        p->kind = HC_HP_VERY_BIASED_TO_BOTTOM;
        const hc_json_t *in = hc_json_get(j, "inner");
        if (!in || in->kind != HC_JSON_NUM)
            FAIL("very_biased_to_bottom inner missing");
        p->inner = (int32_t)in->num;
        return 0;
    }
    FAIL("unsupported height provider type");
}

/* --- 블록 술어 --- */

static int bpred_compile(const hc_json_t *j, hc_bpred_t *p, hc_arena_t *arena,
                         const hc_df_source_t *tags, int32_t n_tags,
                         const char **err) {
    memset(p, 0, sizeof *p);
    const hc_json_t *t = hc_json_get(j, "type");
    if (!t || t->kind != HC_JSON_STR)
        FAIL("block predicate without type");
    const hc_json_t *off = hc_json_get(j, "offset");
    if (off) {
        if (off->kind != HC_JSON_ARR || off->count != 3)
            FAIL("block predicate offset malformed");
        const hc_json_t *v = off->child;
        for (int i = 0; i < 3; i++, v = v->next)
            p->off[i] = (int8_t)v->num;
    }
    if (hc_json_streq(t, "minecraft:matching_fluids")) {
        const hc_json_t *fl = hc_json_get(j, "fluids");
        /* 우리 데이터: ["minecraft:water"] 또는 "minecraft:water" 하나 */
        const hc_json_t *one = fl && fl->kind == HC_JSON_ARR ? fl->child : fl;
        if (!one || one->kind != HC_JSON_STR ||
            !hc_json_streq(one, "minecraft:water") ||
            (fl->kind == HC_JSON_ARR && fl->count != 1))
            FAIL("matching_fluids: only [minecraft:water] supported");
        p->kind = HC_BP_MATCHING_FLUIDS_WATER;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:matching_block_tag")) {
        const hc_json_t *tag = hc_json_get(j, "tag");
        if (!tag || tag->kind != HC_JSON_STR)
            FAIL("matching_block_tag without tag");
        char ref[96];
        if (tag->slen + 1 >= (int32_t)sizeof ref)
            FAIL("tag name too long");
        ref[0] = '#';
        memcpy(ref + 1, tag->s, (size_t)tag->slen);
        p->kind = HC_BP_MATCHING_BLOCK_TAG;
        return tag_expand(p->tag_mask, ref, tag->slen + 1, tags, n_tags, 0,
                          err);
    }
    if (hc_json_streq(t, "minecraft:not")) {
        const hc_json_t *inner = hc_json_get(j, "predicate");
        if (!inner)
            FAIL("not-predicate without predicate");
        p->kind = HC_BP_NOT;
        p->n_children = 1;
        p->children = hc_arena_alloc(arena, sizeof(hc_bpred_t),
                                     _Alignof(hc_bpred_t));
        if (!p->children)
            FAIL("arena exhausted (predicate)");
        return bpred_compile(inner, &p->children[0], arena, tags, n_tags, err);
    }
    if (hc_json_streq(t, "minecraft:all_of")) {
        const hc_json_t *list = hc_json_get(j, "predicates");
        if (!list || list->kind != HC_JSON_ARR || list->count < 1)
            FAIL("all_of without predicates");
        p->kind = HC_BP_ALL_OF;
        p->n_children = list->count;
        p->children = hc_arena_alloc(
            arena, sizeof(hc_bpred_t) * (size_t)list->count,
            _Alignof(hc_bpred_t));
        if (!p->children)
            FAIL("arena exhausted (predicates)");
        int32_t i = 0;
        for (const hc_json_t *c = list->child; c; c = c->next, i++)
            if (bpred_compile(c, &p->children[i], arena, tags, n_tags, err))
                return -1;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:inside_world_bounds")) {
        p->kind = HC_BP_INSIDE_WORLD_BOUNDS;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:matching_blocks")) {
        const hc_json_t *bl = hc_json_get(j, "blocks");
        if (!bl)
            FAIL("matching_blocks without blocks");
        p->kind = HC_BP_MATCHING_BLOCK_TAG; /* 블록 단위 마스크 — 동일 평가 */
        if (bl->kind == HC_JSON_STR) {
            tag_mark(p->tag_mask, bl->s, bl->slen);
            return 0;
        }
        if (bl->kind != HC_JSON_ARR)
            FAIL("matching_blocks malformed");
        for (const hc_json_t *v = bl->child; v; v = v->next) {
            if (v->kind != HC_JSON_STR)
                FAIL("matching_blocks entry not string");
            tag_mark(p->tag_mask, v->s, v->slen);
        }
        return 0;
    }
    if (hc_json_streq(t, "minecraft:solid")) {
        p->kind = HC_BP_SOLID;
        return 0;
    }
    FAIL("unsupported block predicate type");
}

/* --- placement modifier --- */

static int hm_type_parse(const hc_json_t *v, uint8_t *out, const char **err) {
    if (!v || v->kind != HC_JSON_STR)
        FAIL("heightmap type missing");
    if (hc_json_streq(v, "OCEAN_FLOOR_WG")) {
        *out = HC_HM_OCEAN_FLOOR_WG;
        return 0;
    }
    if (hc_json_streq(v, "WORLD_SURFACE_WG")) {
        *out = HC_HM_WORLD_SURFACE_WG;
        return 0;
    }
    /* FINAL 맵 (live) 은 step 9 유지관리와 함께 9b — 실행 도달 시 즉사 */
    *out = HC_HM_LIVE_9B;
    return 0;
}

static int pmod_compile(const hc_json_t *j, hc_pmod_t *m, hc_arena_t *arena,
                        const hc_df_source_t *tags, int32_t n_tags,
                        const char **err) {
    memset(m, 0, sizeof *m);
    const hc_json_t *t = hc_json_get(j, "type");
    if (!t || t->kind != HC_JSON_STR)
        FAIL("placement modifier without type");
    if (hc_json_streq(t, "minecraft:rarity_filter")) {
        const hc_json_t *c = hc_json_get(j, "chance");
        if (!c || c->kind != HC_JSON_NUM)
            FAIL("rarity_filter without chance");
        m->kind = HC_PM_RARITY_FILTER;
        m->chance = (int32_t)c->num;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:count")) {
        const hc_json_t *c = hc_json_get(j, "count");
        if (!c)
            FAIL("count without count");
        m->kind = HC_PM_COUNT;
        return iprov_compile(c, &m->count, err);
    }
    if (hc_json_streq(t, "minecraft:in_square")) {
        m->kind = HC_PM_IN_SQUARE;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:height_range")) {
        const hc_json_t *h = hc_json_get(j, "height");
        if (!h)
            FAIL("height_range without height");
        m->kind = HC_PM_HEIGHT_RANGE;
        return hprov_compile(h, &m->height, err);
    }
    if (hc_json_streq(t, "minecraft:heightmap")) {
        m->kind = HC_PM_HEIGHTMAP;
        return hm_type_parse(hc_json_get(j, "heightmap"), &m->hm_type, err);
    }
    if (hc_json_streq(t, "minecraft:environment_scan")) {
        const hc_json_t *dir = hc_json_get(j, "direction_of_search");
        const hc_json_t *steps = hc_json_get(j, "max_steps");
        const hc_json_t *target = hc_json_get(j, "target_condition");
        const hc_json_t *allowed = hc_json_get(j, "allowed_search_condition");
        if (!dir || dir->kind != HC_JSON_STR || !steps ||
            steps->kind != HC_JSON_NUM || !target)
            FAIL("environment_scan malformed");
        m->kind = HC_PM_ENV_SCAN;
        m->scan_dy = hc_json_streq(dir, "down") ? -1 : 1;
        m->max_steps = (int32_t)steps->num;
        if (allowed) {
            m->has_allowed = 1;
            if (bpred_compile(allowed, &m->allowed, arena, tags, n_tags, err))
                return -1;
        }
        return bpred_compile(target, &m->pred, arena, tags, n_tags, err);
    }
    if (hc_json_streq(t, "minecraft:surface_relative_threshold_filter")) {
        m->kind = HC_PM_SURF_REL_THRESHOLD;
        if (hm_type_parse(hc_json_get(j, "heightmap"), &m->hm_type, err))
            return -1;
        const hc_json_t *lo = hc_json_get(j, "min_inclusive");
        const hc_json_t *hi = hc_json_get(j, "max_inclusive");
        m->min_incl = lo && lo->kind == HC_JSON_NUM ? (int32_t)lo->num
                                                    : INT32_MIN;
        m->max_incl = hi && hi->kind == HC_JSON_NUM ? (int32_t)hi->num
                                                    : INT32_MAX;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:block_predicate_filter")) {
        const hc_json_t *pred = hc_json_get(j, "predicate");
        if (!pred)
            FAIL("block_predicate_filter without predicate");
        m->kind = HC_PM_BLOCK_PRED;
        return bpred_compile(pred, &m->pred, arena, tags, n_tags, err);
    }
    if (hc_json_streq(t, "minecraft:biome")) {
        m->kind = HC_PM_BIOME;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:random_offset")) {
        const hc_json_t *xz = hc_json_get(j, "xz_spread");
        const hc_json_t *ys = hc_json_get(j, "y_spread");
        if (!xz || !ys)
            FAIL("random_offset spreads missing");
        m->kind = HC_PM_RANDOM_OFFSET;
        if (iprov_compile(xz, &m->count, err))
            return -1;
        return iprov_compile(ys, &m->y_spread, err);
    }
    FAIL("unsupported placement modifier type (9b?)");
}

/* --- configured feature 본문 --- */

static int cf_compile(const hc_json_t *cf, hc_pfeat_t *pf,
                      const hc_df_source_t *tags, int32_t n_tags,
                      const char **err) {
    const hc_json_t *t = hc_json_get(cf, "type");
    const hc_json_t *cfg = hc_json_get(cf, "config");
    if (!t || t->kind != HC_JSON_STR || !cfg)
        FAIL("configured feature malformed");
    if (hc_json_streq(t, "minecraft:ore")) {
        pf->cf_kind = HC_CF_ORE;
        hc_ore_cfg_t *o = &pf->cf.ore;
        memset(o, 0, sizeof *o);
        const hc_json_t *size = hc_json_get(cfg, "size");
        const hc_json_t *disc = hc_json_get(cfg, "discard_chance_on_air_exposure");
        const hc_json_t *targets = hc_json_get(cfg, "targets");
        if (!size || size->kind != HC_JSON_NUM || !disc ||
            disc->kind != HC_JSON_NUM || !targets ||
            targets->kind != HC_JSON_ARR)
            FAIL("ore config malformed");
        o->size = (int32_t)size->num;
        o->discard_on_air = (float)disc->num;
        if (o->size < 0 || o->size > 64)
            FAIL("ore size out of codec range");
        if (targets->count > HC_ORE_MAX_TARGETS)
            FAIL("ore target list too long");
        int32_t i = 0;
        for (const hc_json_t *tg = targets->child; tg; tg = tg->next, i++) {
            const hc_json_t *rule = hc_json_get(tg, "target");
            const hc_json_t *state = hc_json_get(tg, "state");
            if (!rule || !state)
                FAIL("ore target malformed");
            const hc_json_t *pt = hc_json_get(rule, "predicate_type");
            if (!pt || !hc_json_streq(pt, "minecraft:tag_match"))
                FAIL("ore rule test not tag_match (overworld contract)");
            const hc_json_t *tag = hc_json_get(rule, "tag");
            if (!tag || tag->kind != HC_JSON_STR)
                FAIL("tag_match without tag");
            char ref[96];
            if (tag->slen + 1 >= (int32_t)sizeof ref)
                FAIL("tag name too long");
            ref[0] = '#';
            memcpy(ref + 1, tag->s, (size_t)tag->slen);
            if (tag_expand(o->targets[i].rule_mask, ref, tag->slen + 1, tags,
                           n_tags, 0, err))
                return -1;
            int32_t id = compile_blockstate(state, err);
            if (id == -2)
                return -1;
            if (id < 0)
                FAIL("ore target state not in hc_blocks table");
            o->targets[i].state = (uint16_t)id;
        }
        o->n_targets = i;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:spring_feature")) {
        pf->cf_kind = HC_CF_SPRING;
        hc_spring_cfg_t *s = &pf->cf.spring;
        memset(s, 0, sizeof *s);
        const hc_json_t *state = hc_json_get(cfg, "state");
        const hc_json_t *name = state ? hc_json_get(state, "Name") : NULL;
        if (!name || name->kind != HC_JSON_STR)
            FAIL("spring state malformed");
        /* FluidState.createLegacyBlock: 소스 유체 → level=0 블록 (A4 §3) */
        if (hc_json_streq(name, "minecraft:water"))
            s->fluid_block = HC_B_WATER;
        else if (hc_json_streq(name, "minecraft:lava"))
            s->fluid_block = HC_B_LAVA;
        else
            FAIL("spring fluid not water/lava");
        const hc_json_t *rb = hc_json_get(cfg, "requires_block_below");
        const hc_json_t *rc = hc_json_get(cfg, "rock_count");
        const hc_json_t *hc = hc_json_get(cfg, "hole_count");
        s->requires_block_below =
            rb ? (uint8_t)(rb->kind == HC_JSON_BOOL && rb->boolean) : 1;
        s->rock_count = rc && rc->kind == HC_JSON_NUM ? (int32_t)rc->num : 4;
        s->hole_count = hc && hc->kind == HC_JSON_NUM ? (int32_t)hc->num : 1;
        const hc_json_t *valid = hc_json_get(cfg, "valid_blocks");
        if (!valid)
            FAIL("spring valid_blocks missing");
        /* HolderSet: 태그 문자열 하나 또는 블록 리스트 */
        if (valid->kind == HC_JSON_STR)
            return tag_expand(s->valid_mask, valid->s, valid->slen, tags,
                              n_tags, 0, err);
        if (valid->kind != HC_JSON_ARR)
            FAIL("spring valid_blocks malformed");
        for (const hc_json_t *v = valid->child; v; v = v->next) {
            if (v->kind != HC_JSON_STR)
                FAIL("spring valid_blocks entry not string");
            if (tag_expand(s->valid_mask, v->s, v->slen, tags, n_tags, 0, err))
                return -1;
        }
        return 0;
    }
    if (hc_json_streq(t, "minecraft:underwater_magma")) {
        pf->cf_kind = HC_CF_UNDERWATER_MAGMA;
        hc_umagma_cfg_t *u = &pf->cf.umagma;
        const hc_json_t *fr = hc_json_get(cfg, "floor_search_range");
        const hc_json_t *pr = hc_json_get(cfg, "placement_radius_around_floor");
        const hc_json_t *pp =
            hc_json_get(cfg, "placement_probability_per_valid_position");
        if (!fr || !pr || !pp)
            FAIL("underwater_magma config malformed");
        u->floor_search_range = (int32_t)fr->num;
        u->placement_radius = (int32_t)pr->num;
        u->placement_prob = (float)pp->num;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:monster_room")) {
        pf->cf_kind = HC_CF_MONSTER_ROOM;
        return 0;
    }
    pf->cf_kind = HC_CF_UNIMPLEMENTED;
    return 0;
}

/* --- order 테이블 파서 --- */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static int parse_i32(const char **pp, int32_t *out) {
    const char *p = skip_ws(*pp);
    int neg = 0;
    if (*p == '-') {
        neg = 1;
        p++;
    }
    if (*p < '0' || *p > '9')
        return -1;
    int64_t v = 0;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (*p++ - '0');
    *out = (int32_t)(neg ? -v : v);
    *pp = p;
    return 0;
}

int hc_feat_reg_init(hc_feat_reg_t *reg, hc_arena_t *arena,
                     const char *order_txt, const hc_json_t *biome_features,
                     const hc_df_source_t *placed, int32_t n_placed,
                     const hc_df_source_t *configured, int32_t n_configured,
                     const hc_df_source_t *tags, int32_t n_tags,
                     hc_biome_reg_t *biomes, int32_t walk_max_step,
                     const char **err) {
    memset(reg, 0, sizeof *reg);

    /* 1. order 테이블 → step 별 이름 배열 */
    const char *p = order_txt;
    int32_t cur_step = -1, cur_i = 0;
    while (*p) {
        const char *nl = p;
        while (*nl && *nl != '\n')
            nl++;
        if (*p == '#' || p == nl) {
            /* 주석/빈 줄 */
        } else if (strncmp(p, "step ", 5) == 0) {
            const char *q = p + 5;
            int32_t s, n;
            if (parse_i32(&q, &s))
                FAIL("order: bad step line");
            q = skip_ws(q);
            if (strncmp(q, "count", 5) != 0)
                FAIL("order: bad step line");
            q += 5;
            if (parse_i32(&q, &n))
                FAIL("order: bad step count");
            if (s < 0 || s >= HC_FEAT_STEPS || n < 0)
                FAIL("order: step out of range");
            if (cur_step >= 0 && cur_i != reg->counts[cur_step])
                FAIL("order: step underfilled");
            cur_step = s;
            cur_i = 0;
            reg->counts[s] = n;
            if (n > 0) {
                reg->steps[s] = hc_arena_alloc(
                    arena, sizeof(hc_pfeat_t) * (size_t)n,
                    _Alignof(hc_pfeat_t));
                if (!reg->steps[s])
                    FAIL("arena exhausted (steps)");
                memset(reg->steps[s], 0, sizeof(hc_pfeat_t) * (size_t)n);
            }
        } else {
            const char *q = p;
            int32_t idx;
            if (parse_i32(&q, &idx))
                FAIL("order: bad feature line");
            if (cur_step < 0 || idx != cur_i ||
                cur_i >= reg->counts[cur_step])
                FAIL("order: index out of sequence");
            q = skip_ws(q);
            int32_t len = (int32_t)(nl - q);
            while (len > 0 && (q[len - 1] == ' ' || q[len - 1] == '\r'))
                len--;
            char *name = hc_arena_alloc(arena, (size_t)len + 1, 1);
            if (!name)
                FAIL("arena exhausted (name)");
            memcpy(name, q, (size_t)len);
            name[len] = '\0';
            reg->steps[cur_step][cur_i].name = name;
            cur_i++;
        }
        p = *nl ? nl + 1 : nl;
    }
    if (cur_step >= 0 && cur_i != reg->counts[cur_step])
        FAIL("order: last step underfilled");

    /* 2. walk_max_step 이하 step 의 파이프라인+본문 컴파일 */
    for (int32_t s = 0; s <= walk_max_step && s < HC_FEAT_STEPS; s++) {
        for (int32_t i = 0; i < reg->counts[s]; i++) {
            hc_pfeat_t *pf = &reg->steps[s][i];
            const hc_json_t *pj =
                find_source(placed, n_placed, pf->name,
                            (int32_t)strlen(pf->name));
            if (!pj)
                FAIL("placed feature JSON not loaded");
            const hc_json_t *placement = hc_json_get(pj, "placement");
            if (!placement || placement->kind != HC_JSON_ARR)
                FAIL("placed feature without placement list");
            pf->n_mods = placement->count;
            if (pf->n_mods > 0) {
                pf->mods = hc_arena_alloc(
                    arena, sizeof(hc_pmod_t) * (size_t)pf->n_mods,
                    _Alignof(hc_pmod_t));
                if (!pf->mods)
                    FAIL("arena exhausted (mods)");
            }
            int32_t mi = 0;
            for (const hc_json_t *mj = placement->child; mj;
                 mj = mj->next, mi++)
                if (pmod_compile(mj, &pf->mods[mi], arena, tags, n_tags, err))
                    return -1;
            const hc_json_t *fref = hc_json_get(pj, "feature");
            if (!fref || fref->kind != HC_JSON_STR)
                FAIL("inline configured feature unsupported at step<=8");
            const hc_json_t *cj = find_named(configured, n_configured, fref);
            if (!cj)
                FAIL("configured feature JSON not loaded");
            if (cf_compile(cj, pf, tags, n_tags, err))
                return -1;
        }
    }

    /* 3. 바이옴 멤버십. 전 바이옴을 인턴한다. order 테이블(오버월드 합집합)
     * 밖의 feature 를 참조하는 바이옴 — 네더/엔드 — 은 행 전체를 0 으로
     * 비워둔다: 오버월드 청크에 나올 수 없고, 나오면(데이터팩 변형)
     * 그 청크의 트레이스/블록 게이트가 즉시 diff 로 드러낸다. */
    if (!biome_features || biome_features->kind != HC_JSON_OBJ)
        FAIL("biome_features not an object");
    for (int32_t s = 0; s < HC_FEAT_STEPS; s++) {
        reg->words[s] = (reg->counts[s] + 63) / 64;
        if (reg->words[s] == 0)
            reg->words[s] = 1;
    }
    /* 먼저 전 바이옴 인턴 → n_biomes 확정 */
    for (const hc_json_t *b = biome_features->child; b; b = b->next)
        if (hc_biome_intern(biomes, b->key, b->klen) < 0)
            FAIL("biome registry full");
    reg->n_biomes = biomes->count;
    for (int32_t s = 0; s < HC_FEAT_STEPS; s++) {
        size_t words = (size_t)reg->words[s] * (size_t)reg->n_biomes;
        reg->member[s] = hc_arena_alloc(arena, words * sizeof(uint64_t),
                                        _Alignof(uint64_t));
        if (!reg->member[s])
            FAIL("arena exhausted (membership)");
        memset(reg->member[s], 0, words * sizeof(uint64_t));
    }
    for (const hc_json_t *b = biome_features->child; b; b = b->next) {
        int32_t bid = hc_biome_find(biomes, b->key, b->klen);
        if (b->kind != HC_JSON_ARR)
            FAIL("biome feature lists malformed");
        int32_t s = 0;
        int resolved = 1;
        for (const hc_json_t *stepj = b->child; stepj;
             stepj = stepj->next, s++) {
            if (s >= HC_FEAT_STEPS || stepj->kind != HC_JSON_ARR)
                FAIL("biome step list malformed");
            for (const hc_json_t *f = stepj->child; f; f = f->next) {
                if (f->kind != HC_JSON_STR)
                    FAIL("biome feature entry not string");
                int32_t idx = -1;
                for (int32_t i = 0; i < reg->counts[s]; i++)
                    if ((int32_t)strlen(reg->steps[s][i].name) == f->slen &&
                        memcmp(reg->steps[s][i].name, f->s,
                               (size_t)f->slen) == 0) {
                        idx = i;
                        break;
                    }
                if (idx < 0) {
                    /* 오버월드 order 테이블 밖 (네더/엔드 feature) —
                     * 이 바이옴은 오버월드 청크에 못 나온다 */
                    resolved = 0;
                    continue;
                }
                reg->member[s][(size_t)bid * (size_t)reg->words[s] +
                               (size_t)(idx >> 6)] |= 1ull << (idx & 63);
            }
        }
        if (!resolved)
            for (int32_t st = 0; st < HC_FEAT_STEPS; st++)
                memset(&reg->member[st][(size_t)bid * (size_t)reg->words[st]],
                       0, (size_t)reg->words[st] * sizeof(uint64_t));
    }
    return 0;
}
