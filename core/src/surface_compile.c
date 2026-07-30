#include "hc_surface.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* surface_rule JSON → IR 컴파일러 + SurfaceSystem 생성자.
 *
 * 시맨틱 근거 (26.2 javap, .hermes/notes/task7-surface/):
 *  - A1 §1.1 ctor: 9개 노이즈는 RandomState.getOrCreateNoise (fork-by-
 *    string, 상태 없음 — 순서 무관), clay bands 는
 *    root.fromHashOf("minecraft:clay_bands") 의 순차 draw 스크립트.
 *  - A3 코덱: 각 조건의 JSON 필드명/기본값 (is_3d 만 optional).
 *  - A4: sequence 는 자식을 선언 순서로 eager 적용, 싱글턴 sequence 는
 *    자식으로 붕괴. TestRule 은 ifTrue → thenRun 순.
 *  - 앵커 해석 (A3 §11): absolute / above_bottom(minGenY+off) /
 *    below_top((genDepth-1)+minGenY-off). WorldGenerationContext 는
 *    26.2 오버월드 상수이므로 컴파일 시 해석해도 동형이다 (y_above 는
 *    바닐라가 compute 마다 해석하지만 순수 함수다). */

static int32_t fail(hc_surface_t *s, const char *fmt, ...) {
    if (!s->err) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(s->errbuf, sizeof s->errbuf, fmt, ap);
        va_end(ap);
        s->err = s->errbuf;
    }
    return -1;
}

/* --- 식별자 헬퍼 (df_compile.c 와 같은 규약) --- */

static int id_eq(const char *ref, int32_t len, const char *name) {
    if (memchr(ref, ':', (size_t)len) == NULL) {
        if (strncmp(name, "minecraft:", 10) != 0)
            return 0;
        name += 10;
    }
    return (int32_t)strlen(name) == len && memcmp(ref, name, (size_t)len) == 0;
}

static char *norm_id(hc_arena_t *arena, const char *s, int32_t len) {
    int    bare = memchr(s, ':', (size_t)len) == NULL;
    size_t n = (bare ? 10 : 0) + (size_t)len + 1;
    char  *out = hc_arena_alloc(arena, n, 1);
    if (!out)
        return NULL;
    char *p = out;
    if (bare) {
        memcpy(p, "minecraft:", 10);
        p += 10;
    }
    memcpy(p, s, (size_t)len);
    p[len] = '\0';
    return out;
}

/* --- 컴파일 상태 --- */

typedef struct {
    hc_surface_t         *s;
    hc_arena_t           *arena;
    int64_t               seed;
    const hc_df_source_t *noise_params;
    int32_t               n_noise_params;
    hc_biome_reg_t       *biomes;
} sc_ctx_t;

/* --- 노이즈 인스턴스 (RandomState.getOrCreateNoise 와 동형) --- */

static int make_noise(sc_ctx_t *c, const char *key, hc_normal_noise_t *out) {
    const hc_json_t *params = NULL;
    for (int32_t i = 0; i < c->n_noise_params; i++)
        if (strcmp(key, c->noise_params[i].name) == 0) {
            params = c->noise_params[i].json;
            break;
        }
    if (!params)
        return (int)fail(c->s, "unknown noise '%s'", key);

    const hc_json_t *fo = hc_json_get(params, "firstOctave");
    const hc_json_t *amps_j = hc_json_get(params, "amplitudes");
    if (!fo || fo->kind != HC_JSON_NUM || !amps_j ||
        amps_j->kind != HC_JSON_ARR || amps_j->count < 1 || amps_j->count > 32)
        return (int)fail(c->s, "bad noise params for '%s'", key);
    double  amps[32];
    int32_t n_amp = 0;
    for (const hc_json_t *e = amps_j->child; e; e = e->next) {
        if (e->kind != HC_JSON_NUM)
            return (int)fail(c->s, "non-number amplitude in '%s'", key);
        amps[n_amp++] = e->num;
    }

    hc_xoro_t      base, rand;
    hc_xoro_fork_t fk;
    hc_xoro_init(&base, c->seed);
    hc_xoro_fork_positional(&base, &fk);
    hc_xoro_from_hash_of(&fk, key, &rand);
    if (hc_normal_noise_init(out, c->arena, &rand, (int32_t)fo->num, amps,
                             n_amp) != 0)
        return (int)fail(c->s, "noise init failed for '%s'", key);
    return 0;
}

/* --- clay bands (A1 §2 — RNG 순차 스크립트 그대로) --- */

static int xoro_next_boolean(hc_xoro_t *r) {
    /* XoroshiroRandomSource.nextBoolean = (nextLong() & 1) != 0 (javap) */
    return (hc_xoro_next(r) & 1u) != 0;
}

static void make_bands(uint16_t *bands, hc_xoro_t *r, int min_size,
                       uint16_t state) {
    int count = hc_xoro_next_int(r, 10) + 6; /* nextIntBetweenInclusive(6,15) */
    for (int i = 0; i < count; i++) {
        int width = min_size + hc_xoro_next_int(r, 3);
        int start = hc_xoro_next_int(r, HC_SURF_CLAY_BANDS);
        for (int m = 0; start + m < HC_SURF_CLAY_BANDS && m < width; m++)
            bands[start + m] = state; /* 배열 끝에서 클립, 랩 없음 */
    }
}

static void generate_bands(uint16_t *bands, hc_xoro_t *r) {
    for (int i = 0; i < HC_SURF_CLAY_BANDS; i++)
        bands[i] = HC_B_TERRACOTTA;

    /* pass 1: orange specks — i += nextInt(5)+1, 조건부 store, 무조건 ++i */
    for (int i = 0; i < HC_SURF_CLAY_BANDS;) {
        i += hc_xoro_next_int(r, 5) + 1;
        if (i < HC_SURF_CLAY_BANDS)
            bands[i] = HC_B_ORANGE_TERRACOTTA;
        i++;
    }
    make_bands(bands, r, 1, HC_B_YELLOW_TERRACOTTA);
    make_bands(bands, r, 2, HC_B_BROWN_TERRACOTTA);
    make_bands(bands, r, 1, HC_B_RED_TERRACOTTA);

    /* pass 5: white + 조건부 light-gray 테두리 (draw 는 가드 통과 시만) */
    int white = hc_xoro_next_int(r, 7) + 9; /* nextIntBetweenInclusive(9,15) */
    int placed = 0;
    for (int k = 0; placed < white && k < HC_SURF_CLAY_BANDS;) {
        bands[k] = HC_B_WHITE_TERRACOTTA;
        if (k - 1 > 0 && xoro_next_boolean(r)) /* > 0 — k==1 은 안 그림 */
            bands[k - 1] = HC_B_LIGHT_GRAY_TERRACOTTA;
        if (k + 1 < HC_SURF_CLAY_BANDS && xoro_next_boolean(r))
            bands[k + 1] = HC_B_LIGHT_GRAY_TERRACOTTA;
        placed++;
        k += hc_xoro_next_int(r, 16) + 4;
    }
}

/* --- BlockState JSON → 내부 id --- */

/* {"Name": "...", "Properties": {...}} → "name[k=v,...]" 직렬화 후 테이블
 * 조회. 우리 블록셋은 프로퍼티가 최대 1개라 JSON 순서 == 바닐라 직렬화
 * 순서다 — 조회 실패는 fail-loud. */
static int32_t block_state_id(sc_ctx_t *c, const hc_json_t *bs) {
    if (!bs || bs->kind != HC_JSON_OBJ)
        return fail(c->s, "result_state is not an object");
    const hc_json_t *name = hc_json_get(bs, "Name");
    if (!name || name->kind != HC_JSON_STR)
        return fail(c->s, "result_state missing Name");
    char buf[128];
    if (name->slen >= (int32_t)sizeof buf - 2)
        return fail(c->s, "block name too long");
    int n = snprintf(buf, sizeof buf, "%.*s", (int)name->slen, name->s);
    const hc_json_t *props = hc_json_get(bs, "Properties");
    if (props && props->kind == HC_JSON_OBJ && props->count > 0) {
        buf[n++] = '[';
        int firstp = 1;
        for (const hc_json_t *p = props->child; p; p = p->next) {
            if (p->kind != HC_JSON_STR)
                return fail(c->s, "non-string block property");
            int w = snprintf(buf + n, sizeof buf - (size_t)n, "%s%.*s=%.*s",
                             firstp ? "" : ",", (int)p->klen, p->key,
                             (int)p->slen, p->s);
            if (w < 0 || (size_t)w >= sizeof buf - (size_t)n)
                return fail(c->s, "block state too long");
            n += w;
            firstp = 0;
        }
        buf[n++] = ']';
        buf[n] = '\0';
    }
    int32_t id = hc_block_by_name(buf, n);
    if (id < 0)
        return fail(c->s, "unknown block state '%s'", buf);
    return id;
}

/* --- 수직 앵커 (A3 §11) --- */

static int anchor_y(sc_ctx_t *c, const hc_json_t *a, int32_t *out) {
    if (!a || a->kind != HC_JSON_OBJ || a->count != 1)
        return (int)fail(c->s, "bad vertical anchor");
    const hc_json_t *v = a->child;
    if (v->kind != HC_JSON_NUM)
        return (int)fail(c->s, "non-number vertical anchor");
    if (v->klen == 8 && memcmp(v->key, "absolute", 8) == 0)
        *out = (int32_t)v->num;
    else if (v->klen == 12 && memcmp(v->key, "above_bottom", 12) == 0)
        *out = HC_MIN_Y + (int32_t)v->num;
    else if (v->klen == 9 && memcmp(v->key, "below_top", 9) == 0)
        *out = (HC_HEIGHT - 1) + HC_MIN_Y - (int32_t)v->num;
    else
        return (int)fail(c->s, "unknown anchor kind '%.*s'", (int)v->klen,
                         v->key);
    return 0;
}

/* --- 샘플러 dedup (Context.getNoiseSampler: (키, is_3d) 별 1개) --- */

static int32_t sampler_index(sc_ctx_t *c, const char *ref, int32_t len,
                             int is3d) {
    hc_surface_t *s = c->s;
    char         *key = norm_id(c->arena, ref, len);
    if (!key)
        return fail(s, "arena exhausted (sampler key)");
    for (int32_t i = 0; i < s->n_samplers; i++)
        if (s->samplers[i].is3d == (is3d != 0) &&
            strcmp(s->samplers[i].key, key) == 0)
            return i;
    if (s->n_samplers >= HC_SURF_MAX_SAMPLERS)
        return fail(s, "sampler cap exceeded");
    hc_ssampler_t *sp = &s->samplers[s->n_samplers];
    if (make_noise(c, key, &sp->noise) != 0)
        return -1;
    sp->key = key;
    sp->is3d = (uint8_t)(is3d != 0);
    sp->memo_stamp = 0;
    sp->memo_val = 0.0;
    return s->n_samplers++;
}

/* --- 조건 컴파일 --- */

static int32_t emit_cond(sc_ctx_t *c, const hc_scond_t *k) {
    hc_surface_t *s = c->s;
    if (s->n_conds >= HC_SURF_MAX_CONDS)
        return fail(s, "condition cap exceeded");
    s->conds[s->n_conds] = *k;
    return s->n_conds++;
}

static int get_int_field(sc_ctx_t *c, const hc_json_t *o, const char *key,
                         int32_t *out) {
    const hc_json_t *v = hc_json_get(o, key);
    if (!v || v->kind != HC_JSON_NUM)
        return (int)fail(c->s, "missing/non-number '%s'", key);
    *out = (int32_t)v->num;
    return 0;
}

static int get_bool_field(sc_ctx_t *c, const hc_json_t *o, const char *key,
                          uint8_t *out) {
    const hc_json_t *v = hc_json_get(o, key);
    if (!v || v->kind != HC_JSON_BOOL)
        return (int)fail(c->s, "missing/non-bool '%s'", key);
    *out = (uint8_t)v->boolean;
    return 0;
}

static int32_t compile_cond(sc_ctx_t *c, const hc_json_t *j);

static int32_t compile_cond_typed(sc_ctx_t *c, const hc_json_t *j,
                                  const char *type, int32_t tlen) {
    hc_surface_t *s = c->s;
    hc_scond_t    k;
    memset(&k, 0, sizeof k);

    if (id_eq(type, tlen, "minecraft:biome")) {
        k.kind = HC_SC_BIOME;
        const hc_json_t *lst = hc_json_get(j, "biome_is");
        if (!lst)
            return fail(s, "biome condition missing biome_is");
        /* 단일 문자열 또는 배열 (RegistryCodecs.homogeneousList) */
        if (lst->kind != HC_JSON_ARR && lst->kind != HC_JSON_STR)
            return fail(s, "biome_is is neither string nor array");
        int arr = lst->kind == HC_JSON_ARR;
        for (const hc_json_t *e = arr ? lst->child : lst; e;
             e = arr ? e->next : NULL) {
            if (e->kind != HC_JSON_STR)
                return fail(s, "non-string biome in biome_is");
            char *name = norm_id(c->arena, e->s, e->slen);
            if (!name)
                return fail(s, "arena exhausted (biome name)");
            int32_t id =
                hc_biome_intern(c->biomes, name, (int32_t)strlen(name));
            if (id < 0)
                return fail(s, "biome registry full");
            k.u.biome.bits[id >> 6] |= 1ull << (id & 63);
        }
        return emit_cond(c, &k);
    }
    if (id_eq(type, tlen, "minecraft:noise_threshold")) {
        k.kind = HC_SC_NOISE_THRESHOLD;
        const hc_json_t *noise = hc_json_get(j, "noise");
        const hc_json_t *mn = hc_json_get(j, "min_threshold");
        const hc_json_t *mx = hc_json_get(j, "max_threshold");
        const hc_json_t *is3d = hc_json_get(j, "is_3d"); /* 기본 false */
        if (!noise || noise->kind != HC_JSON_STR || !mn ||
            mn->kind != HC_JSON_NUM || !mx || mx->kind != HC_JSON_NUM)
            return fail(s, "bad noise_threshold fields");
        if (is3d && is3d->kind != HC_JSON_BOOL)
            return fail(s, "is_3d is not a bool");
        int32_t si = sampler_index(c, noise->s, noise->slen,
                                   is3d ? is3d->boolean : 0);
        if (si < 0)
            return -1;
        k.u.noise.sampler = si;
        k.u.noise.min_threshold = mn->num;
        k.u.noise.max_threshold = mx->num;
        return emit_cond(c, &k);
    }
    if (id_eq(type, tlen, "minecraft:vertical_gradient")) {
        k.kind = HC_SC_VERTICAL_GRADIENT;
        const hc_json_t *rn = hc_json_get(j, "random_name");
        if (!rn || rn->kind != HC_JSON_STR)
            return fail(s, "vertical_gradient missing random_name");
        if (anchor_y(c, hc_json_get(j, "true_at_and_below"),
                     &k.u.vgrad.true_y) ||
            anchor_y(c, hc_json_get(j, "false_at_and_above"),
                     &k.u.vgrad.false_y))
            return -1;
        /* getOrCreateRandomFactory: root.fromHashOf(id).forkPositional() */
        char *key = norm_id(c->arena, rn->s, rn->slen);
        if (!key)
            return fail(s, "arena exhausted (random_name)");
        hc_xoro_t tmp;
        hc_xoro_from_hash_of(&s->noise_random, key, &tmp);
        hc_xoro_fork_positional(&tmp, &k.u.vgrad.fork);
        return emit_cond(c, &k);
    }
    if (id_eq(type, tlen, "minecraft:y_above")) {
        k.kind = HC_SC_Y_ABOVE;
        if (anchor_y(c, hc_json_get(j, "anchor"), &k.u.y_above.anchor_y) ||
            get_int_field(c, j, "surface_depth_multiplier",
                          &k.u.y_above.mult) ||
            get_bool_field(c, j, "add_stone_depth", &k.u.y_above.add_stone))
            return -1;
        return emit_cond(c, &k);
    }
    if (id_eq(type, tlen, "minecraft:water")) {
        k.kind = HC_SC_WATER;
        if (get_int_field(c, j, "offset", &k.u.water.offset) ||
            get_int_field(c, j, "surface_depth_multiplier", &k.u.water.mult) ||
            get_bool_field(c, j, "add_stone_depth", &k.u.water.add_stone))
            return -1;
        return emit_cond(c, &k);
    }
    if (id_eq(type, tlen, "minecraft:temperature")) {
        k.kind = HC_SC_TEMPERATURE;
        return emit_cond(c, &k);
    }
    if (id_eq(type, tlen, "minecraft:steep")) {
        k.kind = HC_SC_STEEP;
        return emit_cond(c, &k);
    }
    if (id_eq(type, tlen, "minecraft:not")) {
        k.kind = HC_SC_NOT;
        const hc_json_t *inv = hc_json_get(j, "invert");
        if (!inv)
            return fail(s, "not condition missing invert");
        int32_t inner = compile_cond(c, inv);
        if (inner < 0)
            return -1;
        k.u.not_.inner = inner;
        return emit_cond(c, &k);
    }
    if (id_eq(type, tlen, "minecraft:hole")) {
        k.kind = HC_SC_HOLE;
        return emit_cond(c, &k);
    }
    if (id_eq(type, tlen, "minecraft:above_preliminary_surface")) {
        k.kind = HC_SC_ABOVE_PRELIM;
        return emit_cond(c, &k);
    }
    if (id_eq(type, tlen, "minecraft:stone_depth")) {
        k.kind = HC_SC_STONE_DEPTH;
        const hc_json_t *st = hc_json_get(j, "surface_type");
        if (!st || st->kind != HC_JSON_STR)
            return fail(s, "stone_depth missing surface_type");
        if (hc_json_streq(st, "ceiling"))
            k.u.stone_depth.ceiling = 1;
        else if (hc_json_streq(st, "floor"))
            k.u.stone_depth.ceiling = 0;
        else
            return fail(s, "unknown surface_type");
        if (get_int_field(c, j, "offset", &k.u.stone_depth.offset) ||
            get_int_field(c, j, "secondary_depth_range",
                          &k.u.stone_depth.secondary_range) ||
            get_bool_field(c, j, "add_surface_depth",
                           &k.u.stone_depth.add_surface))
            return -1;
        return emit_cond(c, &k);
    }
    return fail(s, "unknown condition type '%.*s'", (int)tlen, type);
}

static int32_t compile_cond(sc_ctx_t *c, const hc_json_t *j) {
    if (!j || j->kind != HC_JSON_OBJ)
        return fail(c->s, "condition is not an object");
    const hc_json_t *type = hc_json_get(j, "type");
    if (!type || type->kind != HC_JSON_STR)
        return fail(c->s, "condition missing type");
    return compile_cond_typed(c, j, type->s, type->slen);
}

/* --- 룰 컴파일 --- */

static int32_t emit_rule(sc_ctx_t *c, const hc_srule_t *r) {
    hc_surface_t *s = c->s;
    if (s->n_rules >= HC_SURF_MAX_RULES)
        return fail(s, "rule cap exceeded");
    s->rules[s->n_rules] = *r;
    return s->n_rules++;
}

static int32_t compile_rule(sc_ctx_t *c, const hc_json_t *j) {
    hc_surface_t *s = c->s;
    if (!j || j->kind != HC_JSON_OBJ)
        return fail(s, "rule is not an object");
    const hc_json_t *type = hc_json_get(j, "type");
    if (!type || type->kind != HC_JSON_STR)
        return fail(s, "rule missing type");
    hc_srule_t r;
    memset(&r, 0, sizeof r);

    if (id_eq(type->s, type->slen, "minecraft:sequence")) {
        const hc_json_t *seq = hc_json_get(j, "sequence");
        if (!seq || seq->kind != HC_JSON_ARR || seq->count < 1)
            return fail(s, "bad sequence");
        /* 싱글턴 붕괴 (A4 §3.2 — 자식 룰을 그대로 돌려준다) */
        if (seq->count == 1)
            return compile_rule(c, seq->child);
        int32_t kids[HC_SURF_MAX_SEQ];
        int32_t n = 0;
        for (const hc_json_t *e = seq->child; e; e = e->next) {
            if (n >= HC_SURF_MAX_SEQ)
                return fail(s, "sequence too long");
            int32_t ri = compile_rule(c, e); /* 선언 순서 eager */
            if (ri < 0)
                return -1;
            kids[n++] = ri;
        }
        if (s->n_children + n > HC_SURF_MAX_CHILDREN)
            return fail(s, "children cap exceeded");
        r.kind = HC_SR_SEQUENCE;
        r.u.seq.first = s->n_children;
        r.u.seq.count = n;
        memcpy(s->children + s->n_children, kids, (size_t)n * sizeof kids[0]);
        s->n_children += n;
        return emit_rule(c, &r);
    }
    if (id_eq(type->s, type->slen, "minecraft:block")) {
        int32_t id = block_state_id(c, hc_json_get(j, "result_state"));
        if (id < 0)
            return -1;
        r.kind = HC_SR_BLOCK;
        r.u.block.block = (uint16_t)id;
        return emit_rule(c, &r);
    }
    if (id_eq(type->s, type->slen, "minecraft:condition")) {
        /* TestRuleSource.apply: ifTrue 먼저, thenRun 다음 (A4 §5.2) */
        int32_t ci = compile_cond(c, hc_json_get(j, "if_true"));
        if (ci < 0)
            return -1;
        int32_t ri = compile_rule(c, hc_json_get(j, "then_run"));
        if (ri < 0)
            return -1;
        r.kind = HC_SR_CONDITION;
        r.u.test.cond = ci;
        r.u.test.then = ri;
        return emit_rule(c, &r);
    }
    if (id_eq(type->s, type->slen, "minecraft:bandlands")) {
        r.kind = HC_SR_BANDLANDS;
        return emit_rule(c, &r);
    }
    return fail(s, "unknown rule type '%.*s'", (int)type->slen, type->s);
}

/* --- 시스템 초기화 --- */

int hc_surface_init(hc_surface_t *s, hc_arena_t *arena, int64_t seed,
                    const hc_json_t *settings,
                    const hc_df_source_t *noise_params,
                    int32_t n_noise_params, hc_biome_reg_t *biomes) {
    memset(s, 0, sizeof *s);
    s->biomes = biomes;

    sc_ctx_t c = {s, arena, seed, noise_params, n_noise_params, biomes};

    /* settings 필드 */
    int32_t db = block_state_id(&c, hc_json_get(settings, "default_block"));
    if (db < 0)
        return -1;
    s->default_block = (uint16_t)db;
    const hc_json_t *sea = hc_json_get(settings, "sea_level");
    const hc_json_t *legacy = hc_json_get(settings, "legacy_random_source");
    const hc_json_t *rule = hc_json_get(settings, "surface_rule");
    if (!sea || sea->kind != HC_JSON_NUM || !legacy ||
        legacy->kind != HC_JSON_BOOL || !rule)
        return (int)fail(s, "settings missing sea_level/legacy/surface_rule");
    s->sea_level = (int32_t)sea->num;
    s->legacy_random = legacy->boolean;
    if (s->legacy_random)
        return (int)fail(s, "legacy_random_source unsupported (26.2 "
                            "overworld is xoroshiro)");

    /* ROOT positional factory (A5 §1.3 step 1) */
    hc_xoro_t base;
    hc_xoro_init(&base, seed);
    hc_xoro_fork_positional(&base, &s->noise_random);

    /* ctor 노이즈 9개 (A1 §1.1 — 생성 순서는 상태 없음이라 값에 무관) */
    if (make_noise(&c, "minecraft:clay_bands_offset",
                   &s->clay_bands_offset_noise) ||
        make_noise(&c, "minecraft:surface", &s->surface_noise) ||
        make_noise(&c, "minecraft:surface_secondary",
                   &s->surface_secondary_noise) ||
        make_noise(&c, "minecraft:badlands_pillar",
                   &s->badlands_pillar_noise) ||
        make_noise(&c, "minecraft:badlands_pillar_roof",
                   &s->badlands_pillar_roof_noise) ||
        make_noise(&c, "minecraft:badlands_surface",
                   &s->badlands_surface_noise) ||
        make_noise(&c, "minecraft:iceberg_pillar", &s->iceberg_pillar_noise) ||
        make_noise(&c, "minecraft:iceberg_pillar_roof",
                   &s->iceberg_pillar_roof_noise) ||
        make_noise(&c, "minecraft:iceberg_surface", &s->iceberg_surface_noise))
        return -1;

    /* clay bands: fromHashOf("minecraft:clay_bands") — 노이즈 키와 다른
     * 문자열이다 (A1 §1.1 주의) */
    hc_xoro_t bands_rand;
    hc_xoro_from_hash_of(&s->noise_random, "minecraft:clay_bands",
                         &bands_rand);
    generate_bands(s->clay_bands, &bands_rand);

    /* IR 배열 */
    s->rules = hc_arena_alloc(arena, HC_SURF_MAX_RULES * sizeof(hc_srule_t),
                              _Alignof(hc_srule_t));
    s->conds = hc_arena_alloc(arena, HC_SURF_MAX_CONDS * sizeof(hc_scond_t),
                              _Alignof(hc_scond_t));
    s->children = hc_arena_alloc(arena, HC_SURF_MAX_CHILDREN * sizeof(int32_t),
                                 _Alignof(int32_t));
    s->samplers =
        hc_arena_alloc(arena, HC_SURF_MAX_SAMPLERS * sizeof(hc_ssampler_t),
                       _Alignof(hc_ssampler_t));
    if (!s->rules || !s->conds || !s->children || !s->samplers)
        return (int)fail(s, "arena exhausted (surface IR)");

    s->root_rule = compile_rule(&c, rule);
    if (s->root_rule < 0)
        return -1;

    /* 확장 패스 트리거 바이옴 — intern 하지 않는다 (레지스트리에 없으면
     * 어떤 쿼트도 그 id 를 가질 수 없어 -1 매치 불가가 정답이다) */
    s->biome_eroded_badlands =
        hc_biome_find(biomes, "minecraft:eroded_badlands", 25);
    s->biome_frozen_ocean = hc_biome_find(biomes, "minecraft:frozen_ocean", 22);
    s->biome_deep_frozen_ocean =
        hc_biome_find(biomes, "minecraft:deep_frozen_ocean", 27);
    return 0;
}
