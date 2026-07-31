#include "hc_features.h"

#include <stdio.h>
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
 * 실패 정책 (ADR-009 D3 fail-loud, 9b 확장):
 *  - 알려진 타입의 config 이 어긋나면 즉시 -1 + *err (데이터팩은 고정).
 *  - 모르는 modifier/provider 타입은 HC_PM_DIE 마커로 컴파일 — 3x3 바이옴
 *    합집합에 든 feature 의 파이프라인은 전 청크에서 실행되므로, 마커가
 *    실행에 도달하면 그 즉시 죽는다 (컴파일-시 실패와 같은 소리 크기).
 *    합집합 밖 feature (다른 바이옴 전용) 만 마커를 안고 살아남는다.
 *  - 모르는 본문 타입은 UNIMPLEMENTED — 본문 도달 시 placed=-1 트레이스
 *    (그리드 골든에서 도달 0 이 증명된 것만 남는다). */

#define FAIL(msg)          \
    do {                   \
        *fc->err = (msg);  \
        return -1;         \
    } while (0)

enum { FC_MAX_DEPTH = 10, FC_CACHE = 128 };

typedef struct {
    hc_arena_t           *arena;
    const hc_df_source_t *placed;
    int32_t               n_placed;
    const hc_df_source_t *configured;
    int32_t               n_configured;
    const hc_df_source_t *tags;
    int32_t               n_tags;
    const char          **err;
    /* 중첩 placed feature 이름 캐시 (random_selector 등이 같은 트리를
     * 여러 feature 에서 참조) */
    struct {
        const char *name;
        hc_pfeat_t *pf;
    } cache[FC_CACHE];
    int32_t n_cache;
} fc_t;

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

static int tag_expand(fc_t *fc, uint64_t *bits, const char *name, int32_t len,
                      int depth) {
    if (depth > 8)
        FAIL("block tag recursion too deep");
    if (len > 0 && name[0] == '#') {
        const hc_json_t *tag =
            find_source(fc->tags, fc->n_tags, name + 1, len - 1);
        if (!tag)
            FAIL("referenced block tag not loaded");
        const hc_json_t *values = hc_json_get(tag, "values");
        if (!values || values->kind != HC_JSON_ARR)
            FAIL("tag values missing");
        for (const hc_json_t *v = values->child; v; v = v->next) {
            if (v->kind != HC_JSON_STR)
                FAIL("tag value not string");
            if (tag_expand(fc, bits, v->s, v->slen, depth + 1))
                return -1;
        }
        return 0;
    }
    tag_mark(bits, name, len);
    return 0;
}

static const hc_json_t *find_named(const hc_df_source_t *tab, int32_t n,
                                   const hc_json_t *sref) {
    return find_source(tab, n, sref->s, sref->slen);
}

/* --- 블록스테이트 {"Name":..,"Properties":{..}} → 내부 id ---
 * 캐노니컬 직렬화 = 프로퍼티 키 알파벳 오름차순 (골든 팔레트/26.2
 * BlockState#toString 과 동일). 반환 -1 = 미등재, -2 = 형식 오류. */

static int32_t compile_blockstate(fc_t *fc, const hc_json_t *state) {
    const hc_json_t *name = hc_json_get(state, "Name");
    if (!name || name->kind != HC_JSON_STR) {
        *fc->err = "blockstate Name missing";
        return -2;
    }
    char buf[192];
    if (name->slen >= (int32_t)sizeof buf) {
        *fc->err = "blockstate name too long";
        return -2;
    }
    memcpy(buf, name->s, (size_t)name->slen);
    int32_t len = name->slen;
    const hc_json_t *props = hc_json_get(state, "Properties");
    if (props) {
        if (props->kind != HC_JSON_OBJ || props->count < 1 ||
            props->count > 8) {
            *fc->err = "blockstate properties malformed";
            return -2;
        }
        const hc_json_t *sorted[8];
        int32_t          np = 0;
        for (const hc_json_t *p = props->child; p; p = p->next) {
            if (p->kind != HC_JSON_STR) {
                *fc->err = "blockstate property not string";
                return -2;
            }
            int32_t at = np++;
            while (at > 0) {
                const hc_json_t *q = sorted[at - 1];
                int32_t          m = q->klen < p->klen ? q->klen : p->klen;
                int              c = memcmp(q->key, p->key, (size_t)m);
                if (c < 0 || (c == 0 && q->klen <= p->klen))
                    break;
                sorted[at] = sorted[at - 1];
                at--;
            }
            sorted[at] = p;
        }
        for (int32_t i = 0; i < np; i++) {
            const hc_json_t *p = sorted[i];
            if (len + p->klen + p->slen + 3 >= (int32_t)sizeof buf) {
                *fc->err = "blockstate canonical form too long";
                return -2;
            }
            buf[len++] = i == 0 ? '[' : ',';
            memcpy(buf + len, p->key, (size_t)p->klen);
            len += p->klen;
            buf[len++] = '=';
            memcpy(buf + len, p->s, (size_t)p->slen);
            len += p->slen;
        }
        buf[len++] = ']';
    }
    int32_t id = hc_block_by_name(buf, len);
    if (id < 0)
        *fc->err = "blockstate not in hc_blocks table";
    return id;
}

/* --- VerticalAnchor / providers --- */

static int anchor_resolve(fc_t *fc, const hc_json_t *a, int32_t *out) {
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

static int iprov_compile(fc_t *fc, const hc_json_t *j, hc_iprov_t *p,
                         int depth) {
    memset(p, 0, sizeof *p);
    if (depth > FC_MAX_DEPTH)
        FAIL("int provider recursion too deep");
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
    if (t && hc_json_streq(t, "minecraft:trapezoid")) {
        const hc_json_t *lo = hc_json_get(j, "min");
        const hc_json_t *hi = hc_json_get(j, "max");
        const hc_json_t *pl = hc_json_get(j, "plateau");
        if (!lo || !hi || lo->kind != HC_JSON_NUM || hi->kind != HC_JSON_NUM)
            FAIL("trapezoid int provider bounds missing");
        p->kind = HC_IP_TRAPEZOID;
        p->a = (int32_t)lo->num;
        p->b = (int32_t)hi->num;
        p->c = pl && pl->kind == HC_JSON_NUM ? (int32_t)pl->num : 0;
        return 0;
    }
    if (t && hc_json_streq(t, "minecraft:weighted_list")) {
        const hc_json_t *dist = hc_json_get(j, "distribution");
        if (!dist || dist->kind != HC_JSON_ARR || dist->count < 1)
            FAIL("weighted_list distribution missing");
        p->kind = HC_IP_WEIGHTED_LIST;
        p->n_entries = dist->count;
        p->entries = hc_arena_alloc(
            fc->arena, sizeof(hc_iprov_entry_t) * (size_t)dist->count,
            _Alignof(hc_iprov_entry_t));
        if (!p->entries)
            FAIL("arena exhausted (weighted_list)");
        int32_t i = 0, total = 0;
        for (const hc_json_t *e = dist->child; e; e = e->next, i++) {
            const hc_json_t *w = hc_json_get(e, "weight");
            const hc_json_t *d = hc_json_get(e, "data");
            if (!w || w->kind != HC_JSON_NUM || !d)
                FAIL("weighted_list entry malformed");
            p->entries[i].weight = (int32_t)w->num;
            if (p->entries[i].weight < 0)
                FAIL("weighted_list negative weight");
            total += p->entries[i].weight;
            if (iprov_compile(fc, d, &p->entries[i].prov, depth + 1))
                return -1;
        }
        p->total_weight = total;
        if (total <= 0)
            FAIL("weighted_list zero total weight");
        return 0;
    }
    if (t && hc_json_streq(t, "minecraft:biased_to_bottom")) {
        const hc_json_t *mn = hc_json_get(j, "min_inclusive");
        const hc_json_t *mx = hc_json_get(j, "max_inclusive");
        if (!mn || mn->kind != HC_JSON_NUM || !mx || mx->kind != HC_JSON_NUM)
            FAIL("biased_to_bottom bounds missing");
        p->kind = HC_IP_BIASED_TO_BOTTOM;
        p->a = (int32_t)mn->num;
        p->b = (int32_t)mx->num;
        return 0;
    }
    /* clamped_normal 등 — 샘플 도달 시 즉사 */
    p->kind = HC_IP_UNSUPPORTED;
    return 0;
}

static int hprov_compile(fc_t *fc, const hc_json_t *j, hc_hprov_t *p) {
    const hc_json_t *t = hc_json_get(j, "type");
    if (!t || t->kind != HC_JSON_STR)
        FAIL("height provider without type");
    const hc_json_t *lo = hc_json_get(j, "min_inclusive");
    const hc_json_t *hi = hc_json_get(j, "max_inclusive");
    if (!lo || !hi)
        FAIL("height provider bounds missing");
    if (anchor_resolve(fc, lo, &p->min_y) || anchor_resolve(fc, hi, &p->max_y))
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

/* --- BlockState provider --- */

static int sprov_compile(fc_t *fc, const hc_json_t *j, hc_sprov_t *p,
                         int depth) {
    memset(p, 0, sizeof *p);
    if (depth > FC_MAX_DEPTH)
        FAIL("state provider recursion too deep");
    const hc_json_t *t = hc_json_get(j, "type");
    if (!t || t->kind != HC_JSON_STR)
        FAIL("state provider without type");
    if (hc_json_streq(t, "minecraft:simple_state_provider")) {
        const hc_json_t *s = hc_json_get(j, "state");
        if (!s)
            FAIL("simple_state_provider without state");
        int32_t id = compile_blockstate(fc, s);
        if (id < 0)
            return -1;
        p->kind = HC_SP_SIMPLE;
        p->state = (uint16_t)id;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:weighted_state_provider")) {
        const hc_json_t *entries = hc_json_get(j, "entries");
        if (!entries || entries->kind != HC_JSON_ARR || entries->count < 1)
            FAIL("weighted_state_provider entries missing");
        p->kind = HC_SP_WEIGHTED;
        p->n_entries = entries->count;
        p->entries = hc_arena_alloc(
            fc->arena, sizeof p->entries[0] * (size_t)entries->count,
            _Alignof(int32_t));
        if (!p->entries)
            FAIL("arena exhausted (weighted states)");
        int32_t i = 0, total = 0;
        for (const hc_json_t *e = entries->child; e; e = e->next, i++) {
            const hc_json_t *w = hc_json_get(e, "weight");
            const hc_json_t *d = hc_json_get(e, "data");
            if (!w || w->kind != HC_JSON_NUM || !d)
                FAIL("weighted_state_provider entry malformed");
            int32_t id = compile_blockstate(fc, d);
            if (id < 0)
                return -1;
            p->entries[i].weight = (int32_t)w->num;
            p->entries[i].state = (uint16_t)id;
            if (p->entries[i].weight < 0)
                FAIL("weighted state negative weight");
            total += p->entries[i].weight;
        }
        p->total_weight = total;
        if (total <= 0)
            FAIL("weighted state zero total weight");
        return 0;
    }
    if (hc_json_streq(t, "minecraft:randomized_int_state_provider")) {
        const hc_json_t *prop = hc_json_get(j, "property");
        const hc_json_t *src = hc_json_get(j, "source");
        const hc_json_t *vals = hc_json_get(j, "values");
        if (!prop || prop->kind != HC_JSON_STR || !src || !vals)
            FAIL("randomized_int_state_provider malformed");
        if (!hc_json_streq(prop, "age"))
            FAIL("randomized_int property != age (unmapped)");
        p->kind = HC_SP_RANDOMIZED_INT;
        p->prop_age = 1;
        p->source = hc_arena_alloc(fc->arena, sizeof(hc_sprov_t),
                                   _Alignof(hc_sprov_t));
        if (!p->source)
            FAIL("arena exhausted (randomized_int)");
        if (sprov_compile(fc, src, p->source, depth + 1))
            return -1;
        return iprov_compile(fc, vals, &p->values, depth + 1);
    }
    FAIL("unsupported state provider type");
}

/* --- 블록 술어 --- */

static int bpred_compile(fc_t *fc, const hc_json_t *j, hc_bpred_t *p,
                         int depth) {
    memset(p, 0, sizeof *p);
    if (depth > FC_MAX_DEPTH)
        FAIL("block predicate recursion too deep");
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
        /* [water] 또는 [water, flowing_water] (firefly_bush) 만. 팔레트에
         * flowing 상태가 없어서 (월드젠은 level=0 소스만 쓴다) 두 목록의
         * FluidState.is() 결과가 전 상태에서 일치 — 같은 술어로 축약. */
        const hc_json_t *fl = hc_json_get(j, "fluids");
        if (!fl)
            FAIL("matching_fluids without fluids");
        int saw_water = 0, saw_empty = 0, n_fl = 0;
        const hc_json_t *one = fl->kind == HC_JSON_ARR ? fl->child : fl;
        for (; one; one = fl->kind == HC_JSON_ARR ? one->next : NULL) {
            if (one->kind != HC_JSON_STR)
                FAIL("matching_fluids entry not string");
            n_fl++;
            if (hc_json_streq(one, "minecraft:water"))
                saw_water = 1;
            else if (hc_json_streq(one, "minecraft:empty"))
                saw_empty = 1;
            else if (!hc_json_streq(one, "minecraft:flowing_water"))
                FAIL("matching_fluids: unsupported fluid");
        }
        if (saw_empty) {
            /* fluids=[empty] (patch_melon 등) — 단독일 때만 */
            if (n_fl != 1)
                FAIL("matching_fluids: empty mixed with fluids");
            p->kind = HC_BP_MATCHING_FLUIDS_EMPTY;
            return 0;
        }
        if (!saw_water)
            FAIL("matching_fluids: water missing");
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
        return tag_expand(fc, p->tag_mask, ref, tag->slen + 1, 0);
    }
    if (hc_json_streq(t, "minecraft:not")) {
        const hc_json_t *inner = hc_json_get(j, "predicate");
        if (!inner)
            FAIL("not-predicate without predicate");
        p->kind = HC_BP_NOT;
        p->n_children = 1;
        p->children = hc_arena_alloc(fc->arena, sizeof(hc_bpred_t),
                                     _Alignof(hc_bpred_t));
        if (!p->children)
            FAIL("arena exhausted (predicate)");
        return bpred_compile(fc, inner, &p->children[0], depth + 1);
    }
    if (hc_json_streq(t, "minecraft:all_of") ||
        hc_json_streq(t, "minecraft:any_of")) {
        const hc_json_t *list = hc_json_get(j, "predicates");
        if (!list || list->kind != HC_JSON_ARR || list->count < 1)
            FAIL("all_of/any_of without predicates");
        p->kind = hc_json_streq(t, "minecraft:all_of") ? HC_BP_ALL_OF
                                                       : HC_BP_ANY_OF;
        p->n_children = list->count;
        p->children =
            hc_arena_alloc(fc->arena, sizeof(hc_bpred_t) * (size_t)list->count,
                           _Alignof(hc_bpred_t));
        if (!p->children)
            FAIL("arena exhausted (predicates)");
        int32_t i = 0;
        for (const hc_json_t *c = list->child; c; c = c->next, i++)
            if (bpred_compile(fc, c, &p->children[i], depth + 1))
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
    if (hc_json_streq(t, "minecraft:would_survive")) {
        const hc_json_t *s = hc_json_get(j, "state");
        const hc_json_t *name = s ? hc_json_get(s, "Name") : NULL;
        if (!name || name->kind != HC_JSON_STR)
            FAIL("would_survive state malformed");
        char *copy = hc_arena_alloc(fc->arena, (size_t)name->slen + 1, 1);
        if (!copy)
            FAIL("arena exhausted (would_survive)");
        memcpy(copy, name->s, (size_t)name->slen);
        copy[name->slen] = '\0';
        p->kind = HC_BP_WOULD_SURVIVE;
        p->ws_name = copy;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:has_sturdy_face")) {
        const hc_json_t *dir = hc_json_get(j, "direction");
        if (!dir || dir->kind != HC_JSON_STR)
            FAIL("has_sturdy_face without direction");
        static const char *DIRS[6] = {"down", "up",   "north",
                                      "south", "west", "east"};
        p->kind = HC_BP_HAS_STURDY_FACE;
        p->dir = -1;
        for (int i = 0; i < 6; i++)
            if (hc_json_streq(dir, DIRS[i]))
                p->dir = (int8_t)i;
        if (p->dir < 0)
            FAIL("has_sturdy_face bad direction");
        return 0;
    }
    if (hc_json_streq(t, "minecraft:true")) {
        p->kind = HC_BP_TRUE;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:replaceable")) {
        p->kind = HC_BP_REPLACEABLE;
        return 0;
    }
    FAIL("unsupported block predicate type");
}

/* --- placement modifier --- */

static int hm_type_parse(fc_t *fc, const hc_json_t *v, uint8_t *out) {
    if (!v || v->kind != HC_JSON_STR)
        FAIL("heightmap type missing");
    static const struct {
        const char *name;
        uint8_t     type;
    } MAP[] = {
        {"OCEAN_FLOOR_WG", HC_HM_OCEAN_FLOOR_WG},
        {"WORLD_SURFACE_WG", HC_HM_WORLD_SURFACE_WG},
        {"OCEAN_FLOOR", HC_HM_OCEAN_FLOOR},
        {"WORLD_SURFACE", HC_HM_WORLD_SURFACE},
        {"MOTION_BLOCKING", HC_HM_MOTION_BLOCKING},
        {"MOTION_BLOCKING_NO_LEAVES", HC_HM_MOTION_BLOCKING_NO_LEAVES},
    };
    for (size_t i = 0; i < sizeof MAP / sizeof MAP[0]; i++)
        if (hc_json_streq(v, MAP[i].name)) {
            *out = MAP[i].type;
            return 0;
        }
    FAIL("unknown heightmap type");
}

static int pmod_compile(fc_t *fc, const hc_json_t *j, hc_pmod_t *m) {
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
        return iprov_compile(fc, c, &m->count, 0);
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
        return hprov_compile(fc, h, &m->height);
    }
    if (hc_json_streq(t, "minecraft:heightmap")) {
        m->kind = HC_PM_HEIGHTMAP;
        return hm_type_parse(fc, hc_json_get(j, "heightmap"), &m->hm_type);
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
            if (bpred_compile(fc, allowed, &m->allowed, 0))
                return -1;
        }
        return bpred_compile(fc, target, &m->pred, 0);
    }
    if (hc_json_streq(t, "minecraft:surface_relative_threshold_filter")) {
        m->kind = HC_PM_SURF_REL_THRESHOLD;
        if (hm_type_parse(fc, hc_json_get(j, "heightmap"), &m->hm_type))
            return -1;
        const hc_json_t *lo = hc_json_get(j, "min_inclusive");
        const hc_json_t *hi = hc_json_get(j, "max_inclusive");
        m->min_incl =
            lo && lo->kind == HC_JSON_NUM ? (int32_t)lo->num : INT32_MIN;
        m->max_incl =
            hi && hi->kind == HC_JSON_NUM ? (int32_t)hi->num : INT32_MAX;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:block_predicate_filter")) {
        const hc_json_t *pred = hc_json_get(j, "predicate");
        if (!pred)
            FAIL("block_predicate_filter without predicate");
        m->kind = HC_PM_BLOCK_PRED;
        return bpred_compile(fc, pred, &m->pred, 0);
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
        if (iprov_compile(fc, xz, &m->count, 0))
            return -1;
        return iprov_compile(fc, ys, &m->y_spread, 0);
    }
    if (hc_json_streq(t, "minecraft:surface_water_depth_filter")) {
        const hc_json_t *d = hc_json_get(j, "max_water_depth");
        if (!d || d->kind != HC_JSON_NUM)
            FAIL("surface_water_depth_filter without max_water_depth");
        m->kind = HC_PM_SURFACE_WATER_DEPTH;
        m->max_incl = (int32_t)d->num;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:noise_threshold_count")) {
        const hc_json_t *nl = hc_json_get(j, "noise_level");
        const hc_json_t *below = hc_json_get(j, "below_noise");
        const hc_json_t *above = hc_json_get(j, "above_noise");
        if (!nl || nl->kind != HC_JSON_NUM || !below ||
            below->kind != HC_JSON_NUM || !above ||
            above->kind != HC_JSON_NUM)
            FAIL("noise_threshold_count malformed");
        m->kind = HC_PM_NOISE_THRESHOLD_COUNT;
        m->noise_level = nl->num;
        m->below_noise = (int32_t)below->num;
        m->above_noise = (int32_t)above->num;
        return 0;
    }
    FAIL("unsupported placement modifier type");
}

/* --- tree decorator (R2 §11) --- */

static int tdec_compile(fc_t *fc, const hc_json_t *j, hc_tdec_t *d) {
    memset(d, 0, sizeof *d);
    const hc_json_t *t = hc_json_get(j, "type");
    if (!t || t->kind != HC_JSON_STR)
        FAIL("tree decorator without type");
    const hc_json_t *pr = hc_json_get(j, "probability");
    if (hc_json_streq(t, "minecraft:cocoa")) {
        if (!pr || pr->kind != HC_JSON_NUM)
            FAIL("cocoa without probability");
        d->kind = HC_TDEC_COCOA;
        d->prob = (float)pr->num;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:trunk_vine")) {
        d->kind = HC_TDEC_TRUNK_VINE;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:leave_vine")) {
        if (!pr || pr->kind != HC_JSON_NUM)
            FAIL("leave_vine without probability");
        d->kind = HC_TDEC_LEAVE_VINE;
        d->prob = (float)pr->num;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:attached_to_logs")) {
        const hc_json_t *bp = hc_json_get(j, "block_provider");
        const hc_json_t *dirs = hc_json_get(j, "directions");
        if (!pr || pr->kind != HC_JSON_NUM || !bp || !dirs ||
            dirs->kind != HC_JSON_ARR || dirs->count != 1 ||
            !hc_json_streq(dirs->child, "up"))
            FAIL("attached_to_logs shape unsupported (directions != [up])");
        d->kind = HC_TDEC_ATTACHED_TO_LOGS;
        d->prob = (float)pr->num;
        d->dir = 1; /* UP */
        return sprov_compile(fc, bp, &d->provider, 0);
    }
    FAIL("unsupported tree decorator");
}

/* --- placed feature (파이프라인 + 본문), 중첩/인라인 지원 --- */

/* 잎 패밀리: [OAK_BASE, +28) = oak/jungle × {d1..7, wl}, [AZALEA_BASE, +28)
 * = azalea/flowering 동일 레이아웃 (hc_blocks.h) */
static int leaf_state_ok(uint16_t s) {
    return (s >= HC_B_OAK_LEAVES_BASE && s < HC_B_OAK_LEAVES_BASE + 28) ||
           (s >= HC_B_AZALEA_LEAVES_BASE && s < HC_B_AZALEA_LEAVES_BASE + 28);
}

static int cf_compile(fc_t *fc, const hc_json_t *cf, hc_pfeat_t *pf,
                      int depth);

static int compile_pipeline(fc_t *fc, const hc_json_t *placement,
                            hc_pfeat_t *pf) {
    if (!placement || placement->kind != HC_JSON_ARR)
        FAIL("placed feature without placement list");
    pf->n_mods = placement->count;
    pf->mods = NULL;
    if (pf->n_mods > 0) {
        pf->mods =
            hc_arena_alloc(fc->arena, sizeof(hc_pmod_t) * (size_t)pf->n_mods,
                           _Alignof(hc_pmod_t));
        if (!pf->mods)
            FAIL("arena exhausted (mods)");
    }
    int32_t mi = 0;
    for (const hc_json_t *mj = placement->child; mj; mj = mj->next, mi++) {
        if (pmod_compile(fc, mj, &pf->mods[mi]) != 0) {
            /* 미지원 modifier → 실행-시 즉사 마커 (파일 머리의 정책) */
            const char *why = *fc->err;
            memset(&pf->mods[mi], 0, sizeof pf->mods[mi]);
            pf->mods[mi].kind = HC_PM_DIE;
            pf->mods[mi].die_what = why;
            *fc->err = NULL;
        }
    }
    return 0;
}

/* ref: "minecraft:이름" 문자열 (placed 테이블 참조) 또는
 * {"feature": <cf-ref|inline>, "placement": [...]} 인라인 오브젝트 */
static hc_pfeat_t *compile_placed_ref(fc_t *fc, const hc_json_t *ref,
                                      int depth);

static int compile_placed_into(fc_t *fc, const hc_json_t *pj, hc_pfeat_t *pf,
                               int depth) {
    if (depth > FC_MAX_DEPTH)
        FAIL("placed feature nesting too deep");
    if (compile_pipeline(fc, hc_json_get(pj, "placement"), pf))
        return -1;
    const hc_json_t *fref = hc_json_get(pj, "feature");
    if (!fref)
        FAIL("placed feature without feature");
    const hc_json_t *cj = fref;
    if (fref->kind == HC_JSON_STR) {
        cj = find_named(fc->configured, fc->n_configured, fref);
        if (!cj)
            FAIL("configured feature JSON not loaded");
    }
    if (cf_compile(fc, cj, pf, depth) != 0) {
        /* 지원 밖 본문 변형 (예: deep_dark 의 sculk_vein multiface) —
         * 합집합 밖 feature 만 실제로 남는다; 도달하면 placed=-1 이
         * 트레이스 게이트에서 diff 로 드러난다 (파일 머리 정책). */
        pf->cf_kind = HC_CF_UNIMPLEMENTED;
        pf->unimpl_why = *fc->err;
        *fc->err = NULL;
    }
    return 0;
}

static hc_pfeat_t *compile_placed_ref(fc_t *fc, const hc_json_t *ref,
                                      int depth) {
    if (depth > FC_MAX_DEPTH) {
        *fc->err = "placed ref nesting too deep";
        return NULL;
    }
    const hc_json_t *pj = ref;
    const char      *name = NULL;
    if (ref->kind == HC_JSON_STR) {
        for (int32_t i = 0; i < fc->n_cache; i++)
            if ((int32_t)strlen(fc->cache[i].name) == ref->slen &&
                memcmp(fc->cache[i].name, ref->s, (size_t)ref->slen) == 0)
                return fc->cache[i].pf;
        pj = find_named(fc->placed, fc->n_placed, ref);
        if (!pj) {
            *fc->err = "nested placed feature JSON not loaded";
            return NULL;
        }
        char *copy = hc_arena_alloc(fc->arena, (size_t)ref->slen + 1, 1);
        if (!copy) {
            *fc->err = "arena exhausted (nested name)";
            return NULL;
        }
        memcpy(copy, ref->s, (size_t)ref->slen);
        copy[ref->slen] = '\0';
        name = copy;
    }
    hc_pfeat_t *pf =
        hc_arena_alloc(fc->arena, sizeof(hc_pfeat_t), _Alignof(hc_pfeat_t));
    if (!pf) {
        *fc->err = "arena exhausted (nested placed)";
        return NULL;
    }
    memset(pf, 0, sizeof *pf);
    pf->name = name;
    if (name) {
        /* 자기참조 사이클 방지를 위해 컴파일 전에 캐시에 넣는다 */
        if (fc->n_cache >= FC_CACHE) {
            *fc->err = "nested placed cache full";
            return NULL;
        }
        fc->cache[fc->n_cache].name = name;
        fc->cache[fc->n_cache].pf = pf;
        fc->n_cache++;
    }
    if (compile_placed_into(fc, pj, pf, depth + 1))
        return NULL;
    return pf;
}

/* --- configured feature 본문 --- */

static int cf_compile(fc_t *fc, const hc_json_t *cf, hc_pfeat_t *pf,
                      int depth) {
    const hc_json_t *t = hc_json_get(cf, "type");
    const hc_json_t *cfg = hc_json_get(cf, "config");
    if (!t || t->kind != HC_JSON_STR || !cfg)
        FAIL("configured feature malformed");
    if (hc_json_streq(t, "minecraft:ore")) {
        pf->cf_kind = HC_CF_ORE;
        hc_ore_cfg_t *o = &pf->cf.ore;
        memset(o, 0, sizeof *o);
        const hc_json_t *size = hc_json_get(cfg, "size");
        const hc_json_t *disc =
            hc_json_get(cfg, "discard_chance_on_air_exposure");
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
            if (tag_expand(fc, o->targets[i].rule_mask, ref, tag->slen + 1, 0))
                return -1;
            int32_t id = compile_blockstate(fc, state);
            if (id < 0)
                return -1;
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
        if (valid->kind == HC_JSON_STR)
            return tag_expand(fc, s->valid_mask, valid->s, valid->slen, 0);
        if (valid->kind != HC_JSON_ARR)
            FAIL("spring valid_blocks malformed");
        for (const hc_json_t *v = valid->child; v; v = v->next) {
            if (v->kind != HC_JSON_STR)
                FAIL("spring valid_blocks entry not string");
            if (tag_expand(fc, s->valid_mask, v->s, v->slen, 0))
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
    if (hc_json_streq(t, "minecraft:random_selector")) {
        pf->cf_kind = HC_CF_RANDOM_SELECTOR;
        hc_rsel_cfg_t *r = hc_arena_alloc(fc->arena, sizeof *r,
                                          _Alignof(hc_rsel_cfg_t));
        if (!r)
            FAIL("arena exhausted (random_selector)");
        memset(r, 0, sizeof *r);
        pf->cf.rsel = r;
        const hc_json_t *feats = hc_json_get(cfg, "features");
        const hc_json_t *dflt = hc_json_get(cfg, "default");
        if (!feats || feats->kind != HC_JSON_ARR || !dflt)
            FAIL("random_selector config malformed");
        r->n_entries = feats->count;
        if (r->n_entries > 0) {
            r->entries = hc_arena_alloc(
                fc->arena, sizeof r->entries[0] * (size_t)r->n_entries,
                _Alignof(hc_pfeat_t *));
            if (!r->entries)
                FAIL("arena exhausted (selector entries)");
        }
        int32_t i = 0;
        for (const hc_json_t *e = feats->child; e; e = e->next, i++) {
            const hc_json_t *ch = hc_json_get(e, "chance");
            const hc_json_t *fr = hc_json_get(e, "feature");
            if (!ch || ch->kind != HC_JSON_NUM || !fr)
                FAIL("random_selector entry malformed");
            r->entries[i].chance = (float)ch->num;
            r->entries[i].pf = compile_placed_ref(fc, fr, depth + 1);
            if (!r->entries[i].pf)
                return -1;
        }
        r->dflt = compile_placed_ref(fc, dflt, depth + 1);
        return r->dflt ? 0 : -1;
    }
    if (hc_json_streq(t, "minecraft:random_boolean_selector")) {
        pf->cf_kind = HC_CF_RANDOM_BOOLEAN;
        hc_rbool_cfg_t *r = hc_arena_alloc(fc->arena, sizeof *r,
                                           _Alignof(hc_rbool_cfg_t));
        if (!r)
            FAIL("arena exhausted (random_boolean)");
        pf->cf.rbool = r;
        const hc_json_t *ft = hc_json_get(cfg, "feature_true");
        const hc_json_t *ff = hc_json_get(cfg, "feature_false");
        if (!ft || !ff)
            FAIL("random_boolean_selector config malformed");
        r->on_true = compile_placed_ref(fc, ft, depth + 1);
        r->on_false = compile_placed_ref(fc, ff, depth + 1);
        return (r->on_true && r->on_false) ? 0 : -1;
    }
    if (hc_json_streq(t, "minecraft:simple_random_selector")) {
        pf->cf_kind = HC_CF_SIMPLE_RANDOM_SELECTOR;
        hc_srsel_cfg_t *r = hc_arena_alloc(fc->arena, sizeof *r,
                                           _Alignof(hc_srsel_cfg_t));
        if (!r)
            FAIL("arena exhausted (simple_random_selector)");
        pf->cf.srsel = r;
        const hc_json_t *feats = hc_json_get(cfg, "features");
        if (!feats || feats->kind != HC_JSON_ARR || feats->count < 1)
            FAIL("simple_random_selector features missing");
        r->n = feats->count;
        r->feats = hc_arena_alloc(fc->arena,
                                  sizeof(hc_pfeat_t *) * (size_t)r->n,
                                  _Alignof(hc_pfeat_t *));
        if (!r->feats)
            FAIL("arena exhausted (srsel feats)");
        int32_t i = 0;
        for (const hc_json_t *e = feats->child; e; e = e->next, i++) {
            r->feats[i] = compile_placed_ref(fc, e, depth + 1);
            if (!r->feats[i])
                return -1;
        }
        return 0;
    }
    if (hc_json_streq(t, "minecraft:vegetation_patch") ||
        hc_json_streq(t, "minecraft:waterlogged_vegetation_patch")) {
        pf->cf_kind = HC_CF_VEGETATION_PATCH;
        hc_vpatch_cfg_t *v = hc_arena_alloc(fc->arena, sizeof *v,
                                            _Alignof(hc_vpatch_cfg_t));
        if (!v)
            FAIL("arena exhausted (vegetation_patch)");
        memset(v, 0, sizeof *v);
        pf->cf.vpatch = v;
        v->waterlogged =
            (uint8_t)hc_json_streq(t, "minecraft:waterlogged_vegetation_patch");
        const hc_json_t *surface = hc_json_get(cfg, "surface");
        const hc_json_t *depth_j = hc_json_get(cfg, "depth");
        const hc_json_t *ebc = hc_json_get(cfg, "extra_bottom_block_chance");
        const hc_json_t *eec = hc_json_get(cfg, "extra_edge_column_chance");
        const hc_json_t *vc = hc_json_get(cfg, "vegetation_chance");
        const hc_json_t *vr = hc_json_get(cfg, "vertical_range");
        const hc_json_t *xz = hc_json_get(cfg, "xz_radius");
        const hc_json_t *repl = hc_json_get(cfg, "replaceable");
        const hc_json_t *gs = hc_json_get(cfg, "ground_state");
        const hc_json_t *vf = hc_json_get(cfg, "vegetation_feature");
        if (!surface || surface->kind != HC_JSON_STR || !depth_j || !ebc ||
            !eec || !vc || !vr || vr->kind != HC_JSON_NUM || !xz || !repl ||
            repl->kind != HC_JSON_STR || !gs || !vf)
            FAIL("vegetation_patch config malformed");
        v->surface_ceiling = (uint8_t)hc_json_streq(surface, "ceiling");
        if (!v->surface_ceiling && !hc_json_streq(surface, "floor"))
            FAIL("vegetation_patch bad surface");
        v->extra_bottom_chance = (float)ebc->num;
        v->extra_edge_chance = (float)eec->num;
        v->veg_chance = (float)vc->num;
        v->vertical_range = (int32_t)vr->num;
        if (iprov_compile(fc, depth_j, &v->depth, 0) ||
            iprov_compile(fc, xz, &v->xz_radius, 0) ||
            tag_expand(fc, v->replaceable, repl->s, repl->slen, 0) ||
            sprov_compile(fc, gs, &v->ground, 0))
            return -1;
        v->vegetation = compile_placed_ref(fc, vf, depth + 1);
        return v->vegetation ? 0 : -1;
    }
    if (hc_json_streq(t, "minecraft:block_column")) {
        pf->cf_kind = HC_CF_BLOCK_COLUMN;
        hc_bcol_cfg_t *b = hc_arena_alloc(fc->arena, sizeof *b,
                                          _Alignof(hc_bcol_cfg_t));
        if (!b)
            FAIL("arena exhausted (block_column)");
        memset(b, 0, sizeof *b);
        pf->cf.bcol = b;
        const hc_json_t *dir = hc_json_get(cfg, "direction");
        const hc_json_t *layers = hc_json_get(cfg, "layers");
        const hc_json_t *pt = hc_json_get(cfg, "prioritize_tip");
        const hc_json_t *ap = hc_json_get(cfg, "allowed_placement");
        if (!dir || dir->kind != HC_JSON_STR || !layers ||
            layers->kind != HC_JSON_ARR || layers->count < 1 || !pt ||
            pt->kind != HC_JSON_BOOL || !ap)
            FAIL("block_column config malformed");
        if (hc_json_streq(dir, "up"))
            b->dir_dy = 1;
        else if (hc_json_streq(dir, "down"))
            b->dir_dy = -1;
        else
            FAIL("block_column bad direction");
        b->prioritize_tip = (uint8_t)pt->boolean;
        b->n_layers = layers->count;
        b->layers = hc_arena_alloc(
            fc->arena, sizeof b->layers[0] * (size_t)b->n_layers,
            _Alignof(hc_iprov_t));
        if (!b->layers)
            FAIL("arena exhausted (column layers)");
        int32_t i = 0;
        for (const hc_json_t *l = layers->child; l; l = l->next, i++) {
            const hc_json_t *h = hc_json_get(l, "height");
            const hc_json_t *pr = hc_json_get(l, "provider");
            if (!h || !pr)
                FAIL("block_column layer malformed");
            if (iprov_compile(fc, h, &b->layers[i].height, 0) ||
                sprov_compile(fc, pr, &b->layers[i].prov, 0))
                return -1;
        }
        return bpred_compile(fc, ap, &b->allowed, 0);
    }
    if (hc_json_streq(t, "minecraft:multiface_growth")) {
        pf->cf_kind = HC_CF_MULTIFACE_GROWTH;
        hc_mface_cfg_t *m = hc_arena_alloc(fc->arena, sizeof *m,
                                           _Alignof(hc_mface_cfg_t));
        if (!m)
            FAIL("arena exhausted (multiface_growth)");
        memset(m, 0, sizeof *m);
        pf->cf.mface = m;
        const hc_json_t *block = hc_json_get(cfg, "block");
        if (!block || block->kind != HC_JSON_STR ||
            !hc_json_streq(block, "minecraft:glow_lichen"))
            FAIL("multiface_growth: only glow_lichen supported");
        const hc_json_t *sr = hc_json_get(cfg, "search_range");
        const hc_json_t *fl = hc_json_get(cfg, "can_place_on_floor");
        const hc_json_t *ce = hc_json_get(cfg, "can_place_on_ceiling");
        const hc_json_t *wa = hc_json_get(cfg, "can_place_on_wall");
        const hc_json_t *cs = hc_json_get(cfg, "chance_of_spreading");
        const hc_json_t *on = hc_json_get(cfg, "can_be_placed_on");
        /* codec 기본 (26.2 MultifaceGrowthConfiguration): search_range 10,
         * floor/ceiling/wall false, chance_of_spreading 0.5f — R3 확정 */
        m->search_range =
            sr && sr->kind == HC_JSON_NUM ? (int32_t)sr->num : 10;
        m->can_place_on_floor =
            fl ? (uint8_t)(fl->kind == HC_JSON_BOOL && fl->boolean) : 0;
        m->can_place_on_ceiling =
            ce ? (uint8_t)(ce->kind == HC_JSON_BOOL && ce->boolean) : 0;
        m->can_place_on_wall =
            wa ? (uint8_t)(wa->kind == HC_JSON_BOOL && wa->boolean) : 0;
        m->chance_of_spreading =
            cs && cs->kind == HC_JSON_NUM ? (float)cs->num : 0.5f;
        if (!on)
            FAIL("multiface_growth can_be_placed_on missing");
        if (on->kind == HC_JSON_STR)
            return tag_expand(fc, m->can_place_on, on->s, on->slen, 0);
        if (on->kind != HC_JSON_ARR)
            FAIL("multiface_growth can_be_placed_on malformed");
        for (const hc_json_t *v = on->child; v; v = v->next) {
            if (v->kind != HC_JSON_STR)
                FAIL("can_be_placed_on entry not string");
            if (tag_expand(fc, m->can_place_on, v->s, v->slen, 0))
                return -1;
        }
        return 0;
    }
    if (hc_json_streq(t, "minecraft:simple_block")) {
        pf->cf_kind = HC_CF_SIMPLE_BLOCK;
        hc_sblock_cfg_t *s = hc_arena_alloc(fc->arena, sizeof *s,
                                            _Alignof(hc_sblock_cfg_t));
        if (!s)
            FAIL("arena exhausted (simple_block)");
        memset(s, 0, sizeof *s);
        pf->cf.sblock = s;
        const hc_json_t *tp = hc_json_get(cfg, "to_place");
        if (!tp)
            FAIL("simple_block to_place missing");
        return sprov_compile(fc, tp, &s->to_place, 0);
    }
    if (hc_json_streq(t, "minecraft:vines")) {
        pf->cf_kind = HC_CF_VINES;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:bamboo")) {
        pf->cf_kind = HC_CF_BAMBOO;
        hc_bamboo_cfg_t *b = hc_arena_alloc(fc->arena, sizeof *b,
                                            _Alignof(hc_bamboo_cfg_t));
        if (!b)
            FAIL("arena exhausted (bamboo)");
        pf->cf.bamboo = b;
        const hc_json_t *pr = hc_json_get(cfg, "probability");
        if (!pr || pr->kind != HC_JSON_NUM)
            FAIL("bamboo probability missing");
        b->probability = (float)pr->num;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:freeze_top_layer")) {
        pf->cf_kind = HC_CF_FREEZE_TOP_LAYER;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:tree")) {
        pf->cf_kind = HC_CF_TREE;
        hc_tree_cfg_t *c = hc_arena_alloc(fc->arena, sizeof *c,
                                          _Alignof(hc_tree_cfg_t));
        if (!c)
            FAIL("arena exhausted (tree)");
        memset(c, 0, sizeof *c);
        pf->cf.tree = c;
        if (hc_json_get(cfg, "root_placer"))
            FAIL("tree root_placer unsupported");
        const hc_json_t *tp = hc_json_get(cfg, "trunk_placer");
        const hc_json_t *fp = hc_json_get(cfg, "foliage_placer");
        const hc_json_t *tpr = hc_json_get(cfg, "trunk_provider");
        const hc_json_t *fpr = hc_json_get(cfg, "foliage_provider");
        const hc_json_t *btp = hc_json_get(cfg, "below_trunk_provider");
        const hc_json_t *ms = hc_json_get(cfg, "minimum_size");
        const hc_json_t *dec = hc_json_get(cfg, "decorators");
        const hc_json_t *iv = hc_json_get(cfg, "ignore_vines");
        if (!tp || !fp || !tpr || !fpr || !ms || !dec ||
            dec->kind != HC_JSON_ARR)
            FAIL("tree config malformed");
        /* trunk placer */
        const hc_json_t *tt = hc_json_get(tp, "type");
        if (!tt)
            FAIL("trunk placer without type");
        if (hc_json_streq(tt, "minecraft:straight_trunk_placer"))
            c->trunk_kind = HC_TRUNK_STRAIGHT;
        else if (hc_json_streq(tt, "minecraft:mega_jungle_trunk_placer"))
            c->trunk_kind = HC_TRUNK_MEGA_JUNGLE;
        else if (hc_json_streq(tt, "minecraft:fancy_trunk_placer"))
            c->trunk_kind = HC_TRUNK_FANCY;
        else if (hc_json_streq(tt, "minecraft:bending_trunk_placer"))
            c->trunk_kind = HC_TRUNK_BENDING;
        else
            FAIL("unsupported trunk placer");
        const hc_json_t *bh = hc_json_get(tp, "base_height");
        const hc_json_t *ra = hc_json_get(tp, "height_rand_a");
        const hc_json_t *rb = hc_json_get(tp, "height_rand_b");
        if (!bh || !ra || !rb)
            FAIL("trunk placer heights missing");
        c->base_height = (int32_t)bh->num;
        c->rand_a = (int32_t)ra->num;
        c->rand_b = (int32_t)rb->num;
        if (c->trunk_kind == HC_TRUNK_BENDING) {
            /* min_height_for_leaves 코덱 기본 1 (BendingTrunkPlacer
             * lambda$static$0@8-14 — R5c §1.3) */
            const hc_json_t *mh = hc_json_get(tp, "min_height_for_leaves");
            const hc_json_t *bl = hc_json_get(tp, "bend_length");
            c->min_height_for_leaves =
                mh && mh->kind == HC_JSON_NUM ? (int32_t)mh->num : 1;
            if (!bl)
                FAIL("bending_trunk_placer bend_length missing");
            if (iprov_compile(fc, bl, &c->bend_length, 0))
                return -1;
        }
        /* foliage placer */
        const hc_json_t *ft = hc_json_get(fp, "type");
        if (!ft)
            FAIL("foliage placer without type");
        if (hc_json_streq(ft, "minecraft:blob_foliage_placer"))
            c->fol_kind = HC_FOL_BLOB;
        else if (hc_json_streq(ft, "minecraft:bush_foliage_placer"))
            c->fol_kind = HC_FOL_BUSH;
        else if (hc_json_streq(ft, "minecraft:jungle_foliage_placer"))
            c->fol_kind = HC_FOL_MEGA_JUNGLE;
        else if (hc_json_streq(ft, "minecraft:fancy_foliage_placer"))
            c->fol_kind = HC_FOL_FANCY;
        else if (hc_json_streq(ft, "minecraft:random_spread_foliage_placer"))
            c->fol_kind = HC_FOL_RANDOM_SPREAD;
        else
            FAIL("unsupported foliage placer");
        const hc_json_t *fr = hc_json_get(fp, "radius");
        const hc_json_t *fo = hc_json_get(fp, "offset");
        /* random_spread 는 height 가 없고 foliage_height +
         * leaf_placement_attempts 를 갖는다 (R5c §7.3.5) */
        const hc_json_t *fhh =
            hc_json_get(fp, c->fol_kind == HC_FOL_RANDOM_SPREAD
                                ? "foliage_height"
                                : "height");
        if (!fr || !fo || !fhh || fhh->kind != HC_JSON_NUM)
            FAIL("foliage placer fields missing");
        if (iprov_compile(fc, fr, &c->fol_radius, 0) ||
            iprov_compile(fc, fo, &c->fol_offset, 0))
            return -1;
        c->fol_height = (int32_t)fhh->num;
        if (c->fol_kind == HC_FOL_RANDOM_SPREAD) {
            const hc_json_t *la = hc_json_get(fp, "leaf_placement_attempts");
            if (!la || la->kind != HC_JSON_NUM)
                FAIL("random_spread leaf_placement_attempts missing");
            c->leaf_attempts = (int32_t)la->num;
        }
        /* providers — simple 전용 */
        hc_sprov_t sp;
        if (sprov_compile(fc, tpr, &sp, 0))
            return -1;
        if (sp.kind != HC_SP_SIMPLE)
            FAIL("tree trunk provider not simple");
        c->trunk_state = sp.state;
        if (sprov_compile(fc, fpr, &c->foliage, 0))
            return -1;
        /* 전 상태가 잎 패밀리 [base, base+28) (oak/jungle 또는
         * azalea/flowering) 안이어야 한다 — try_place_leaf 의 wl +7 과
         * updateLeaves 의 distance 재산출이 이 레이아웃을 전제 (R5c §7.3). */
        if (c->foliage.kind == HC_SP_SIMPLE) {
            if (!leaf_state_ok(c->foliage.state))
                FAIL("tree foliage state not a leaf");
        } else if (c->foliage.kind == HC_SP_WEIGHTED) {
            for (int32_t fi = 0; fi < c->foliage.n_entries; fi++)
                if (!leaf_state_ok(c->foliage.entries[fi].state))
                    FAIL("tree foliage weighted entry not a leaf");
        } else {
            FAIL("tree foliage provider kind unsupported");
        }
        /* below_trunk: rule_based {rules:[{if_true: not(matching_block_tag),
         * then: simple}]} — 또는 simple (azalea, R5c §7.3.4: getOptionalState
         * == getState, 무조건 쓰기 = below_not_mask 전부 0) */
        if (!btp)
            FAIL("below_trunk_provider missing");
        {
            const hc_json_t *bt = hc_json_get(btp, "type");
            if (bt && hc_json_streq(bt, "minecraft:simple_state_provider")) {
                if (sprov_compile(fc, btp, &sp, 0))
                    return -1;
                if (sp.kind != HC_SP_SIMPLE)
                    FAIL("below_trunk simple provider not simple");
                c->below_state = sp.state;
                goto below_trunk_done;
            }
            const hc_json_t *rules = hc_json_get(btp, "rules");
            if (!bt ||
                !hc_json_streq(bt, "minecraft:rule_based_state_provider") ||
                !rules || rules->kind != HC_JSON_ARR || rules->count != 1)
                FAIL("below_trunk provider shape unsupported");
            const hc_json_t *rule = rules->child;
            const hc_json_t *cond = hc_json_get(rule, "if_true");
            const hc_json_t *then = hc_json_get(rule, "then");
            const hc_json_t *ct = cond ? hc_json_get(cond, "type") : NULL;
            const hc_json_t *inner =
                cond ? hc_json_get(cond, "predicate") : NULL;
            const hc_json_t *itag =
                inner ? hc_json_get(inner, "tag") : NULL;
            if (!ct || !hc_json_streq(ct, "minecraft:not") || !inner ||
                !itag || itag->kind != HC_JSON_STR || !then)
                FAIL("below_trunk rule shape unsupported");
            char ref[96];
            if (itag->slen + 1 >= (int32_t)sizeof ref)
                FAIL("tag name too long");
            ref[0] = '#';
            memcpy(ref + 1, itag->s, (size_t)itag->slen);
            if (tag_expand(fc, c->below_not_mask, ref, itag->slen + 1, 0))
                return -1;
            if (sprov_compile(fc, then, &sp, 0))
                return -1;
            if (sp.kind != HC_SP_SIMPLE)
                FAIL("below_trunk then-provider not simple");
            c->below_state = sp.state;
        }
    below_trunk_done:
        c->ignore_vines =
            iv ? (uint8_t)(iv->kind == HC_JSON_BOOL && iv->boolean) : 0;
        /* minimum_size: two_layers_feature_size */
        {
            const hc_json_t *mt = hc_json_get(ms, "type");
            if (!mt ||
                !hc_json_streq(mt, "minecraft:two_layers_feature_size"))
                FAIL("minimum_size type unsupported");
            const hc_json_t *lim = hc_json_get(ms, "limit");
            const hc_json_t *lo = hc_json_get(ms, "lower_size");
            const hc_json_t *up = hc_json_get(ms, "upper_size");
            const hc_json_t *mc = hc_json_get(ms, "min_clipped_height");
            c->ts_limit = lim && lim->kind == HC_JSON_NUM ? (int32_t)lim->num
                                                          : 1;
            c->ts_lower = lo && lo->kind == HC_JSON_NUM ? (int32_t)lo->num
                                                        : 0;
            c->ts_upper = up && up->kind == HC_JSON_NUM ? (int32_t)up->num
                                                        : 1;
            c->ts_min_clipped =
                mc && mc->kind == HC_JSON_NUM ? (int32_t)mc->num : -1;
        }
        c->n_decorators = dec->count;
        if (c->n_decorators > 0) {
            c->decorators = hc_arena_alloc(
                fc->arena, sizeof(hc_tdec_t) * (size_t)c->n_decorators,
                _Alignof(hc_tdec_t));
            if (!c->decorators)
                FAIL("arena exhausted (decorators)");
            int32_t i = 0;
            for (const hc_json_t *dj = dec->child; dj; dj = dj->next, i++)
                if (tdec_compile(fc, dj, &c->decorators[i]))
                    return -1;
        }
        return 0;
    }
    if (hc_json_streq(t, "minecraft:disk")) {
        /* DiskFeature (Task 10 링 본문 — 본 세션 javap: place @54 radius
         * 샘플 1 드로우, placeColumn 은 target/rule 이 전부 술어라 드로우 0.
         * state_provider 는 RuleBasedStateProvider: rules 순서로 if_true
         * (셀 위치 평가) 첫 히트의 then, 아니면 fallback). */
        pf->cf_kind = HC_CF_DISK;
        hc_disk_cfg_t *d =
            hc_arena_alloc(fc->arena, sizeof *d, _Alignof(hc_disk_cfg_t));
        if (!d)
            FAIL("arena exhausted (disk)");
        memset(d, 0, sizeof *d);
        pf->cf.disk = d;
        const hc_json_t *hh = hc_json_get(cfg, "half_height");
        const hc_json_t *rad = hc_json_get(cfg, "radius");
        const hc_json_t *tgt = hc_json_get(cfg, "target");
        const hc_json_t *sp = hc_json_get(cfg, "state_provider");
        if (!hh || hh->kind != HC_JSON_NUM || !rad || !tgt || !sp)
            FAIL("disk config malformed");
        d->half_height = (int32_t)hh->num;
        if (iprov_compile(fc, rad, &d->radius, 0) ||
            bpred_compile(fc, tgt, &d->target, 0))
            return -1;
        const hc_json_t *spt = hc_json_get(sp, "type");
        if (!spt)
            FAIL("disk state_provider without type");
        if (!hc_json_streq(spt, "minecraft:rule_based_state_provider")) {
            /* withAlternative 코덱: 맨 BlockStateProvider =
             * RuleBasedBlockStateProvider.simple (fallback = provider,
             * rules = []) — disk_gravel/disk_clay 가 이 형태 */
            d->n_rules = 0;
            d->rules = NULL;
            return sprov_compile(fc, sp, &d->fallback, 0);
        }
        const hc_json_t *fb = hc_json_get(sp, "fallback");
        const hc_json_t *rules = hc_json_get(sp, "rules");
        if (!fb || !rules || rules->kind != HC_JSON_ARR)
            FAIL("rule_based_state_provider malformed");
        if (sprov_compile(fc, fb, &d->fallback, 0))
            return -1;
        d->n_rules = rules->count;
        d->rules = hc_arena_alloc(fc->arena,
                                  sizeof(hc_disk_rule_t) * (size_t)d->n_rules,
                                  _Alignof(hc_disk_rule_t));
        if (!d->rules)
            FAIL("arena exhausted (disk rules)");
        int32_t ri = 0;
        for (const hc_json_t *r = rules->child; r; r = r->next, ri++) {
            const hc_json_t *cond = hc_json_get(r, "if_true");
            const hc_json_t *then = hc_json_get(r, "then");
            if (!cond || !then)
                FAIL("disk rule malformed");
            if (bpred_compile(fc, cond, &d->rules[ri].if_true, 0) ||
                sprov_compile(fc, then, &d->rules[ri].then, 0))
                return -1;
        }
        return 0;
    }
    if (hc_json_streq(t, "minecraft:seagrass")) {
        /* SeagrassFeature (본 세션 javap — R-노트 인용은 features_ring.c) */
        pf->cf_kind = HC_CF_SEAGRASS;
        const hc_json_t *prob = hc_json_get(cfg, "probability");
        if (!prob || prob->kind != HC_JSON_NUM)
            FAIL("seagrass config malformed");
        pf->cf.seagrass.probability = (float)prob->num;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:kelp")) {
        /* KelpFeature — NoneFeatureConfiguration (본 세션 javap;
         * 시맨틱 인용은 features_ring.c) */
        pf->cf_kind = HC_CF_KELP;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:lake")) {
        /* LakeFeature (R5b §0-§1). fluid/barrier 는 데이터 전부 simple. */
        pf->cf_kind = HC_CF_LAKE;
        hc_lake_cfg_t *l =
            hc_arena_alloc(fc->arena, sizeof *l, _Alignof(hc_lake_cfg_t));
        if (!l)
            FAIL("arena exhausted (lake)");
        memset(l, 0, sizeof *l);
        pf->cf.lake = l;
        const hc_json_t *fl = hc_json_get(cfg, "fluid");
        const hc_json_t *ba = hc_json_get(cfg, "barrier");
        const hc_json_t *cp = hc_json_get(cfg, "can_place_feature");
        const hc_json_t *ca =
            hc_json_get(cfg, "can_replace_with_air_or_fluid");
        const hc_json_t *cb = hc_json_get(cfg, "can_replace_with_barrier");
        if (!fl || !ba || !cp || !ca || !cb)
            FAIL("lake config malformed");
        hc_sprov_t sp;
        if (sprov_compile(fc, fl, &sp, 0))
            return -1;
        if (sp.kind != HC_SP_SIMPLE)
            FAIL("lake fluid provider not simple");
        l->fluid = sp.state;
        if (sprov_compile(fc, ba, &sp, 0))
            return -1;
        if (sp.kind != HC_SP_SIMPLE)
            FAIL("lake barrier provider not simple");
        l->barrier = sp.state;
        if (bpred_compile(fc, cp, &l->can_place, 0) ||
            bpred_compile(fc, ca, &l->can_replace_airfluid, 0) ||
            bpred_compile(fc, cb, &l->can_replace_barrier, 0))
            return -1;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:root_system")) {
        /* RootSystemFeature (R5c §1.1/§7.1) */
        pf->cf_kind = HC_CF_ROOT_SYSTEM;
        hc_rootsys_cfg_t *r =
            hc_arena_alloc(fc->arena, sizeof *r, _Alignof(hc_rootsys_cfg_t));
        if (!r)
            FAIL("arena exhausted (root_system)");
        memset(r, 0, sizeof *r);
        pf->cf.rootsys = r;
        static const struct {
            const char *key;
            size_t      off;
        } RS_INTS[] = {
            {"required_vertical_space_for_tree",
             offsetof(hc_rootsys_cfg_t, required_vertical_space)},
            {"allowed_vertical_water_for_tree",
             offsetof(hc_rootsys_cfg_t, allowed_vertical_water)},
            {"root_radius", offsetof(hc_rootsys_cfg_t, root_radius)},
            {"root_placement_attempts",
             offsetof(hc_rootsys_cfg_t, root_attempts)},
            {"root_column_max_height",
             offsetof(hc_rootsys_cfg_t, root_column_max_height)},
            {"hanging_root_radius",
             offsetof(hc_rootsys_cfg_t, hanging_radius)},
            {"hanging_roots_vertical_span",
             offsetof(hc_rootsys_cfg_t, hanging_span)},
            {"hanging_root_placement_attempts",
             offsetof(hc_rootsys_cfg_t, hanging_attempts)},
        };
        for (size_t i = 0; i < sizeof RS_INTS / sizeof RS_INTS[0]; i++) {
            const hc_json_t *v = hc_json_get(cfg, RS_INTS[i].key);
            if (!v || v->kind != HC_JSON_NUM)
                FAIL("root_system int field missing");
            *(int32_t *)((char *)r + RS_INTS[i].off) = (int32_t)v->num;
        }
        /* level_test_distance/max_level_deviation != 0 이면 placeDirtAndTree
         * @56-158 의 4방향 레벨 테스트가 살아난다 — 데이터는 0 (죽은 분기,
         * R5c §2.3); 미구현이므로 0 만 허용 */
        const hc_json_t *ltd = hc_json_get(cfg, "level_test_distance");
        const hc_json_t *mld = hc_json_get(cfg, "max_level_deviation");
        if (!ltd || ltd->kind != HC_JSON_NUM || (int32_t)ltd->num != 0 ||
            !mld || mld->kind != HC_JSON_NUM || (int32_t)mld->num != 0)
            FAIL("root_system level test unsupported (nonzero)");
        const hc_json_t *rr = hc_json_get(cfg, "root_replaceable");
        if (!rr || rr->kind != HC_JSON_STR)
            FAIL("root_replaceable malformed");
        if (tag_expand(fc, r->root_replaceable, rr->s, rr->slen, 0))
            return -1;
        hc_sprov_t sp;
        const hc_json_t *rsp = hc_json_get(cfg, "root_state_provider");
        const hc_json_t *hsp = hc_json_get(cfg, "hanging_root_state_provider");
        const hc_json_t *atp = hc_json_get(cfg, "allowed_tree_position");
        const hc_json_t *fj = hc_json_get(cfg, "feature");
        if (!rsp || !hsp || !atp || !fj)
            FAIL("root_system config malformed");
        if (sprov_compile(fc, rsp, &sp, 0))
            return -1;
        if (sp.kind != HC_SP_SIMPLE)
            FAIL("root_state provider not simple");
        r->root_state = sp.state;
        if (sprov_compile(fc, hsp, &sp, 0))
            return -1;
        if (sp.kind != HC_SP_SIMPLE)
            FAIL("hanging_root_state provider not simple");
        r->hanging_state = sp.state;
        if (bpred_compile(fc, atp, &r->allowed_tree_position, 0))
            return -1;
        /* 인라인 placed feature {"feature": "...", "placement": []} —
         * PlacedFeature.CODEC.fieldOf("feature") (R5c §1.1/§3) */
        r->tree = compile_placed_ref(fc, fj, depth + 1);
        if (!r->tree)
            return -1;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:geode")) {
        /* GeodeFeature (R5a §1 — 코덱 기본값 포함) */
        pf->cf_kind = HC_CF_GEODE;
        hc_geode_cfg_t *g =
            hc_arena_alloc(fc->arena, sizeof *g, _Alignof(hc_geode_cfg_t));
        if (!g)
            FAIL("arena exhausted (geode)");
        memset(g, 0, sizeof *g);
        pf->cf.geode = g;
        const hc_json_t *bl = hc_json_get(cfg, "blocks");
        if (!bl)
            FAIL("geode blocks missing");
        static const struct {
            const char *key;
            size_t      off;
        } GB_PROV[] = {
            {"filling_provider", offsetof(hc_geode_cfg_t, fill)},
            {"inner_layer_provider", offsetof(hc_geode_cfg_t, inner)},
            {"alternate_inner_layer_provider",
             offsetof(hc_geode_cfg_t, alt_inner)},
            {"middle_layer_provider", offsetof(hc_geode_cfg_t, middle)},
            {"outer_layer_provider", offsetof(hc_geode_cfg_t, outer)},
        };
        hc_sprov_t sp;
        for (size_t i = 0; i < sizeof GB_PROV / sizeof GB_PROV[0]; i++) {
            const hc_json_t *v = hc_json_get(bl, GB_PROV[i].key);
            if (!v)
                FAIL("geode layer provider missing");
            if (sprov_compile(fc, v, &sp, 0))
                return -1;
            if (sp.kind != HC_SP_SIMPLE)
                FAIL("geode layer provider not simple");
            *(uint16_t *)((char *)g + GB_PROV[i].off) = sp.state;
        }
        const hc_json_t *ip = hc_json_get(bl, "inner_placements");
        if (!ip || ip->kind != HC_JSON_ARR || ip->count < 1 ||
            ip->count > (int32_t)(sizeof g->placements /
                                  sizeof g->placements[0]))
            FAIL("geode inner_placements malformed");
        g->n_placements = ip->count;
        {
            int32_t i = 0;
            for (const hc_json_t *st = ip->child; st; st = st->next, i++) {
                int32_t id = compile_blockstate(fc, st);
                if (id < 0)
                    FAIL("geode inner placement state unregistered");
                g->placements[i] = (uint16_t)id;
            }
        }
        const hc_json_t *cr = hc_json_get(bl, "cannot_replace");
        const hc_json_t *ib = hc_json_get(bl, "invalid_blocks");
        if (!cr || cr->kind != HC_JSON_STR || !ib || ib->kind != HC_JSON_STR)
            FAIL("geode tags malformed");
        if (tag_expand(fc, g->cannot_replace, cr->s, cr->slen, 0) ||
            tag_expand(fc, g->invalid_blocks, ib->s, ib->slen, 0))
            return -1;
        /* layers/crack/chances — optionalFieldOf 기본값 (R5a §1) */
        const hc_json_t *ly = hc_json_get(cfg, "layers");
        const hc_json_t *v;
        g->layer_fill = 1.7;
        g->layer_inner = 2.2;
        g->layer_middle = 3.2;
        g->layer_outer = 4.2;
        if (ly) {
            if ((v = hc_json_get(ly, "filling")))
                g->layer_fill = v->num;
            if ((v = hc_json_get(ly, "inner_layer")))
                g->layer_inner = v->num;
            if ((v = hc_json_get(ly, "middle_layer")))
                g->layer_middle = v->num;
            if ((v = hc_json_get(ly, "outer_layer")))
                g->layer_outer = v->num;
        }
        const hc_json_t *ck = hc_json_get(cfg, "crack");
        g->crack_chance = 1.0;
        g->crack_base = 2.0;
        g->crack_offset = 2;
        if (ck) {
            if ((v = hc_json_get(ck, "generate_crack_chance")))
                g->crack_chance = v->num;
            if ((v = hc_json_get(ck, "base_crack_size")))
                g->crack_base = v->num;
            if ((v = hc_json_get(ck, "crack_point_offset")))
                g->crack_offset = (int32_t)v->num;
        }
        g->use_potential =
            (v = hc_json_get(cfg, "use_potential_placements_chance"))
                ? v->num
                : 0.35;
        g->use_alt = (v = hc_json_get(cfg, "use_alternate_layer0_chance"))
                         ? v->num
                         : 0.0;
        g->require_alt =
            (v = hc_json_get(cfg, "placements_require_layer0_alternate"))
                ? (v->kind == HC_JSON_BOOL && v->boolean)
                : 1;
        if ((v = hc_json_get(cfg, "outer_wall_distance"))) {
            if (iprov_compile(fc, v, &g->outer_wall, 0))
                return -1;
        } else {
            g->outer_wall.kind = HC_IP_UNIFORM;
            g->outer_wall.a = 4;
            g->outer_wall.b = 5;
        }
        /* d = nPoints / (double)outerWallDistance.maxInclusive()
         * (place@98-111) — UniformInt 전용 액세서 */
        if (g->outer_wall.kind != HC_IP_UNIFORM)
            FAIL("geode outer_wall_distance not uniform");
        g->outer_wall_max = g->outer_wall.b;
        if ((v = hc_json_get(cfg, "distribution_points"))) {
            if (iprov_compile(fc, v, &g->dist_points, 0))
                return -1;
        } else {
            g->dist_points.kind = HC_IP_UNIFORM;
            g->dist_points.a = 3;
            g->dist_points.b = 4;
        }
        if ((v = hc_json_get(cfg, "point_offset"))) {
            if (iprov_compile(fc, v, &g->point_offset, 0))
                return -1;
        } else {
            g->point_offset.kind = HC_IP_UNIFORM;
            g->point_offset.a = 1;
            g->point_offset.b = 2;
        }
        g->min_gen =
            (v = hc_json_get(cfg, "min_gen_offset")) ? (int32_t)v->num : -16;
        g->max_gen =
            (v = hc_json_get(cfg, "max_gen_offset")) ? (int32_t)v->num : 16;
        g->noise_mult =
            (v = hc_json_get(cfg, "noise_multiplier")) ? v->num : 0.05;
        v = hc_json_get(cfg, "invalid_blocks_threshold");
        if (!v || v->kind != HC_JSON_NUM)
            FAIL("geode invalid_blocks_threshold missing");
        g->invalid_threshold = (int32_t)v->num;
        return 0;
    }
    if (hc_json_streq(t, "minecraft:fallen_tree")) {
        pf->cf_kind = HC_CF_FALLEN_TREE;
        hc_ftree_cfg_t *c = hc_arena_alloc(fc->arena, sizeof *c,
                                           _Alignof(hc_ftree_cfg_t));
        if (!c)
            FAIL("arena exhausted (fallen_tree)");
        memset(c, 0, sizeof *c);
        pf->cf.ftree = c;
        const hc_json_t *tpr = hc_json_get(cfg, "trunk_provider");
        const hc_json_t *ll = hc_json_get(cfg, "log_length");
        const hc_json_t *sd = hc_json_get(cfg, "stump_decorators");
        const hc_json_t *ld = hc_json_get(cfg, "log_decorators");
        if (!tpr || !ll)
            FAIL("fallen_tree config malformed");
        hc_sprov_t sp;
        if (sprov_compile(fc, tpr, &sp, 0))
            return -1;
        if (sp.kind != HC_SP_SIMPLE)
            FAIL("fallen_tree trunk provider not simple");
        c->trunk_state = sp.state;
        if (iprov_compile(fc, ll, &c->log_length, 0))
            return -1;
        if (sd) {
            if (sd->kind != HC_JSON_ARR)
                FAIL("stump_decorators malformed");
            c->n_stump_dec = sd->count;
            c->stump_dec = hc_arena_alloc(
                fc->arena, sizeof(hc_tdec_t) * (size_t)(sd->count ? sd->count
                                                                  : 1),
                _Alignof(hc_tdec_t));
            if (!c->stump_dec)
                FAIL("arena exhausted (stump dec)");
            int32_t i = 0;
            for (const hc_json_t *dj = sd->child; dj; dj = dj->next, i++)
                if (tdec_compile(fc, dj, &c->stump_dec[i]))
                    return -1;
        }
        if (ld) {
            if (ld->kind != HC_JSON_ARR)
                FAIL("log_decorators malformed");
            c->n_log_dec = ld->count;
            c->log_dec = hc_arena_alloc(
                fc->arena, sizeof(hc_tdec_t) * (size_t)(ld->count ? ld->count
                                                                  : 1),
                _Alignof(hc_tdec_t));
            if (!c->log_dec)
                FAIL("arena exhausted (log dec)");
            int32_t i = 0;
            for (const hc_json_t *dj = ld->child; dj; dj = dj->next, i++)
                if (tdec_compile(fc, dj, &c->log_dec[i]))
                    return -1;
        }
        return 0;
    }
    /* root_system 등 그리드-외 exotics — 본문 도달 시 placed=-1. */
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

    fc_t fc_storage;
    fc_t *fc = &fc_storage;
    memset(fc, 0, sizeof *fc);
    fc->arena = arena;
    fc->placed = placed;
    fc->n_placed = n_placed;
    fc->configured = configured;
    fc->n_configured = n_configured;
    fc->tags = tags;
    fc->n_tags = n_tags;
    fc->err = err;

    /* 0. 본문 canSurvive 태그 마스크 (R1 §4/§5, R4 §1.3/§3.1) */
    static const struct {
        const char *tag;
        size_t      off;
    } RT_TAGS[] = {
        {"#minecraft:supports_vegetation",
         offsetof(hc_feat_reg_t, tag_supports_vegetation)},
        {"#minecraft:supports_bamboo",
         offsetof(hc_feat_reg_t, tag_supports_bamboo)},
        {"#minecraft:beneath_bamboo_podzol_replaceable",
         offsetof(hc_feat_reg_t, tag_podzol_replaceable)},
        {"#minecraft:supports_azalea",
         offsetof(hc_feat_reg_t, tag_supports_azalea)},
        {"#minecraft:supports_small_dripleaf",
         offsetof(hc_feat_reg_t, tag_supports_small_dripleaf)},
        {"#minecraft:supports_big_dripleaf",
         offsetof(hc_feat_reg_t, tag_supports_big_dripleaf)},
        {"#minecraft:supports_cocoa",
         offsetof(hc_feat_reg_t, tag_supports_cocoa)},
        {"#minecraft:supports_sugar_cane",
         offsetof(hc_feat_reg_t, tag_supports_sugar_cane)},
        {"#minecraft:replaceable_by_trees",
         offsetof(hc_feat_reg_t, tag_replaceable_by_trees)},
        {"#minecraft:logs", offsetof(hc_feat_reg_t, tag_logs)},
        {"#minecraft:prevents_nearby_leaf_decay",
         offsetof(hc_feat_reg_t, tag_prevents_leaf_decay)},
        {"#minecraft:cannot_support_seagrass",
         offsetof(hc_feat_reg_t, tag_cannot_support_seagrass)},
        {"#minecraft:features_cannot_replace",
         offsetof(hc_feat_reg_t, tag_features_cannot_replace)},
    };
    for (size_t i = 0; i < sizeof RT_TAGS / sizeof RT_TAGS[0]; i++)
        if (tag_expand(fc, (uint64_t *)((char *)reg + RT_TAGS[i].off),
                       RT_TAGS[i].tag, (int32_t)strlen(RT_TAGS[i].tag), 0))
            return -1;

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
            const hc_json_t *pj = find_source(placed, n_placed, pf->name,
                                              (int32_t)strlen(pf->name));
            if (!pj)
                FAIL("placed feature JSON not loaded");
            const char *keep = pf->name;
            if (compile_placed_into(fc, pj, pf, 0) != 0) {
                if (*err) {
                    static char where[192];
                    snprintf(where, sizeof where, "%s (feature %s)",
                             *err, keep);
                    *err = where;
                }
                return -1;
            }
            pf->name = keep;
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
