#include "hc_df_compile.h"

#include "hc_rng.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* worldgen JSON → 평탄 IR 컴파일러. 시맨틱 근거는 26.2 바이트코드다:
 *  - RandomState.getOrCreateNoise: 키 = ResourceKey.identifier().toString()
 *    ("minecraft:temperature"), ConcurrentHashMap 으로 키당 1회 생성·공유.
 *  - NoiseWiringHelper.wrapNew: BlendedNoise 만 withNewRandom(
 *    fromHashOf("minecraft:terrain")) 으로 교체, 마커는 그대로 통과.
 *  - TwoArgumentSimpleFunction.create: ADD/MUL 에서 상수 인자 발견 시
 *    MulOrAdd(비상수 인자, 상수값) — 단락 없는 별도 노드. */

static int32_t fail(hc_df_compiler_t *c, const char *fmt, ...) {
    if (!c->err) { /* 최초 원인 보존 */
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->errbuf, sizeof c->errbuf, fmt, ap);
        va_end(ap);
        c->err = c->errbuf;
    }
    return -1;
}

/* --- 식별자 헬퍼 (hc_json 문자열은 NUL 종단이 아니다) --- */

/* ref(JSON, 네임스페이스 생략 가능) 와 name(테이블, 항상 "minecraft:...") */
static int id_eq(const char *ref, int32_t len, const char *name) {
    if (memchr(ref, ':', (size_t)len) == NULL) {
        if (strncmp(name, "minecraft:", 10) != 0)
            return 0;
        name += 10;
    }
    return (int32_t)strlen(name) == len && memcmp(ref, name, (size_t)len) == 0;
}

/* "minecraft:" 프리픽스를 보정한 NUL 종단 사본 (Identifier.parse 기본
 * 네임스페이스와 동형) — fromHashOf 시딩 문자열로 쓰므로 정확해야 한다. */
static char *norm_id(hc_df_compiler_t *c, const char *s, int32_t len) {
    int    bare = memchr(s, ':', (size_t)len) == NULL;
    size_t n = (bare ? 10 : 0) + (size_t)len + 1;
    char  *out = hc_arena_alloc(c->arena, n, 1);
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

/* --- JSON 필드 헬퍼 (필수 필드 누락은 fail-loud) --- */

static int get_num(hc_df_compiler_t *c, const hc_json_t *obj, const char *key,
                   double *out) {
    const hc_json_t *v = hc_json_get(obj, key);
    if (!v || v->kind != HC_JSON_NUM)
        return (int)fail(c, "missing/non-number field '%s'", key);
    *out = v->num;
    return 0;
}

static const hc_json_t *get_field(hc_df_compiler_t *c, const hc_json_t *obj,
                                  const char *key) {
    const hc_json_t *v = hc_json_get(obj, key);
    if (!v)
        fail(c, "missing field '%s'", key);
    return v;
}

/* --- 노드 방출 --- */

static hc_df_node_t mknode(uint8_t op) {
    hc_df_node_t n;
    memset(&n, 0, sizeof n);
    n.op = op;
    n.a = n.b = n.c = -1;
    n.aux = n.aux2 = -1;
    return n;
}

static int32_t emit(hc_df_compiler_t *c, const hc_df_node_t *nd) {
    if (c->g->n >= HC_DFC_MAX_NODES)
        return fail(c, "node cap exceeded");
    c->g->nodes[c->g->n] = *nd;
    return c->g->n++;
}

/* --- 노이즈 인스턴스 (키 dedup + fork-by-string 시딩) --- */

static int32_t noise_index(hc_df_compiler_t *c, const char *ref, int32_t len) {
    char *key = norm_id(c, ref, len);
    if (!key)
        return fail(c, "arena exhausted (noise key)");
    for (int32_t i = 0; i < c->g->n_noises; i++)
        if (strcmp(key, c->noise_keys[i]) == 0)
            return i;

    const hc_json_t *params = NULL;
    for (int32_t i = 0; i < c->n_noise_params; i++)
        if (strcmp(key, c->noise_params[i].name) == 0) {
            params = c->noise_params[i].json;
            break;
        }
    if (!params)
        return fail(c, "unknown noise '%s'", key);

    double fo = 0.0;
    if (get_num(c, params, "firstOctave", &fo))
        return -1;
    const hc_json_t *amps_j = get_field(c, params, "amplitudes");
    if (!amps_j)
        return -1;
    if (amps_j->kind != HC_JSON_ARR || amps_j->count < 1 ||
        amps_j->count > 32)
        return fail(c, "bad amplitudes for '%s'", key);
    double amps[32];
    int32_t n_amp = 0;
    for (const hc_json_t *e = amps_j->child; e; e = e->next) {
        if (e->kind != HC_JSON_NUM)
            return fail(c, "non-number amplitude in '%s'", key);
        amps[n_amp++] = e->num;
    }

    if (c->g->n_noises >= HC_DFC_MAX_NOISES)
        return fail(c, "noise cap exceeded");
    int32_t idx = c->g->n_noises;

    /* RandomState: XoroshiroRandomSource(seed).forkPositional()
     *              .fromHashOf(키) 로 NormalNoise.create */
    hc_xoro_t      base, rand;
    hc_xoro_fork_t fk;
    hc_xoro_init(&base, c->seed);
    hc_xoro_fork_positional(&base, &fk);
    hc_xoro_from_hash_of(&fk, key, &rand);
    if (hc_normal_noise_init(&c->g->noises[idx], c->arena, &rand,
                             (int32_t)fo, amps, n_amp) != 0)
        return fail(c, "normal noise init failed for '%s'", key);

    c->noise_keys[idx] = key;
    c->g->n_noises++;
    return idx;
}

/* 필드가 노이즈 참조 문자열인지 확인 후 인스턴스 인덱스로 */
static int32_t noise_field(hc_df_compiler_t *c, const hc_json_t *obj,
                           const char *field) {
    const hc_json_t *v = get_field(c, obj, field);
    if (!v)
        return -1;
    if (v->kind != HC_JSON_STR)
        return fail(c, "field '%s' is not a noise reference", field);
    return noise_index(c, v->s, v->slen);
}

/* --- 스플라인 (Codec.FLOAT — 숫자는 전부 float 로 강하) --- */

static int32_t compile_spline_spec(hc_df_compiler_t *c,
                                   const hc_json_t *spec);

static int32_t spline_slot(hc_df_compiler_t *c) {
    if (c->g->n_splines >= HC_DFC_MAX_SPLINES)
        return fail(c, "spline cap exceeded");
    return c->g->n_splines++;
}

static int32_t compile_spline_spec(hc_df_compiler_t *c,
                                   const hc_json_t *spec) {
    if (spec->kind == HC_JSON_NUM) {
        /* 바닐라 codec: 숫자 = CubicSpline.constant((float)값) */
        int32_t si = spline_slot(c);
        if (si < 0)
            return -1;
        hc_df_spline_t *s = &c->g->splines[si];
        memset(s, 0, sizeof *s);
        s->coord = -1;
        s->n = 0;
        s->k = (float)spec->num;
        return si;
    }
    if (spec->kind != HC_JSON_OBJ)
        return fail(c, "spline spec is neither number nor object");

    const hc_json_t *coord_j = get_field(c, spec, "coordinate");
    const hc_json_t *pts = get_field(c, spec, "points");
    if (!coord_j || !pts)
        return -1;
    if (pts->kind != HC_JSON_ARR || pts->count < 1)
        return fail(c, "spline points missing/empty");

    int32_t coord = hc_df_compile_expr(c, coord_j);
    if (coord < 0)
        return -1;

    int32_t         n = pts->count;
    float          *loc = hc_arena_alloc(c->arena, sizeof(float) * (size_t)n,
                                         _Alignof(float));
    float          *der = hc_arena_alloc(c->arena, sizeof(float) * (size_t)n,
                                         _Alignof(float));
    int32_t        *val = hc_arena_alloc(c->arena,
                                         sizeof(int32_t) * (size_t)n,
                                         _Alignof(int32_t));
    if (!loc || !der || !val)
        return fail(c, "arena exhausted (spline arrays)");

    int32_t j = 0;
    for (const hc_json_t *p = pts->child; p; p = p->next, j++) {
        if (p->kind != HC_JSON_OBJ)
            return fail(c, "spline point is not an object");
        double lv = 0.0, dv = 0.0;
        if (get_num(c, p, "location", &lv) || get_num(c, p, "derivative", &dv))
            return -1;
        loc[j] = (float)lv;
        der[j] = (float)dv;
        const hc_json_t *vj = get_field(c, p, "value");
        if (!vj)
            return -1;
        int32_t vs = compile_spline_spec(c, vj);
        if (vs < 0)
            return -1;
        val[j] = vs;
    }

    int32_t si = spline_slot(c);
    if (si < 0)
        return -1;
    hc_df_spline_t *s = &c->g->splines[si];
    s->coord = coord;
    s->n = n;
    s->k = 0.0f;
    s->loc = loc;
    s->der = der;
    s->val = val;
    return si;
}

/* --- FIND_TOP_SURFACE density 의존 콘 --- */

static int mark_spline_deps(hc_df_compiler_t *c, int32_t si, uint8_t *mark);

static int mark_deps(hc_df_compiler_t *c, int32_t idx, uint8_t *mark) {
    if (mark[idx])
        return 0;
    mark[idx] = 1;
    const hc_df_node_t *nd = &c->g->nodes[idx];

    if (nd->op == HC_DF_FIND_TOP_SURFACE)
        return (int)fail(c, "nested find_top_surface unsupported (6b)");

    const int32_t ops[3] = {nd->a, nd->b, nd->c};
    for (int i = 0; i < 3; i++)
        if (ops[i] >= 0 && mark_deps(c, ops[i], mark))
            return -1;

    if (nd->op == HC_DF_INTERVAL_SELECT) {
        int32_t n = c->g->ipool[nd->aux];
        for (int32_t i = 0; i < n; i++)
            if (mark_deps(c, c->g->ipool[nd->aux + 1 + i], mark))
                return -1;
    } else if (nd->op == HC_DF_SPLINE) {
        if (mark_spline_deps(c, nd->aux, mark))
            return -1;
    }
    return 0;
}

static int mark_spline_deps(hc_df_compiler_t *c, int32_t si, uint8_t *mark) {
    const hc_df_spline_t *s = &c->g->splines[si];
    if (s->n == 0)
        return 0;
    if (mark_deps(c, s->coord, mark))
        return -1;
    for (int32_t j = 0; j < s->n; j++)
        if (mark_spline_deps(c, s->val[j], mark))
            return -1;
    return 0;
}

/* density 서브그래프의 전이 의존을 오름차순 (== 위상 순서) 으로 ipool 에
 * 적재한다. FTS 평가는 이 콘만 y 를 바꿔 재평가한다. */
static int build_cone(hc_df_compiler_t *c, int32_t density,
                      int32_t *off_out, int32_t *len_out) {
    uint8_t *mark = hc_arena_alloc(c->arena, (size_t)c->g->n, 1);
    if (!mark)
        return (int)fail(c, "arena exhausted (cone mark)");
    memset(mark, 0, (size_t)c->g->n);
    if (mark_deps(c, density, mark))
        return -1;

    int32_t len = 0;
    for (int32_t i = 0; i < c->g->n; i++)
        len += mark[i];
    if (c->g->n_ipool + len > HC_DFC_MAX_IPOOL)
        return (int)fail(c, "ipool cap exceeded (cone)");

    *off_out = c->g->n_ipool;
    *len_out = len;
    for (int32_t i = 0; i < c->g->n; i++)
        if (mark[i])
            c->g->ipool[c->g->n_ipool++] = i;
    return 0;
}

/* --- 표현식 컴파일 --- */

/* 이름 참조: dedup + 사이클 검출. 같은 이름은 한 번만 컴파일된다. */
static int32_t compile_named(hc_df_compiler_t *c, const char *ref,
                             int32_t len) {
    for (int32_t i = 0; i < c->n_dfs; i++) {
        if (!id_eq(ref, len, c->dfs[i].name))
            continue;
        if (c->df_state[i] == -2)
            return fail(c, "reference cycle at '%s'", c->dfs[i].name);
        if (c->df_state[i] >= 0)
            return c->df_state[i];
        c->df_state[i] = -2;
        int32_t idx = hc_df_compile_expr(c, c->dfs[i].json);
        c->df_state[i] = idx; /* 실패 시 -1 로 복귀 */
        return idx;
    }
    return fail(c, "unresolved df reference '%.*s'", (int)len, ref);
}

/* 바닐라 create(): 인자가 Constant '인스턴스' 일 때만 MulOrAdd — JSON
 * 숫자 리터럴과 inline {"type":"constant"} 만 해당한다. 이름 참조는
 * HolderHolder 라 상수여도 Ap2 로 남는다. */
static int is_const_arg(const hc_json_t *v, double *out) {
    if (v->kind == HC_JSON_NUM) {
        *out = v->num;
        return 1;
    }
    if (v->kind == HC_JSON_OBJ) {
        const hc_json_t *t = hc_json_get(v, "type");
        if (t && t->kind == HC_JSON_STR &&
            id_eq(t->s, t->slen, "minecraft:constant")) {
            const hc_json_t *arg = hc_json_get(v, "argument");
            if (arg && arg->kind == HC_JSON_NUM) {
                *out = arg->num;
                return 1;
            }
        }
    }
    return 0;
}

/* 단항 op ("argument" 필드) */
static int32_t compile_unary(hc_df_compiler_t *c, const hc_json_t *obj,
                             uint8_t op) {
    const hc_json_t *arg = get_field(c, obj, "argument");
    if (!arg)
        return -1;
    hc_df_node_t nd = mknode(op);
    nd.a = hc_df_compile_expr(c, arg);
    if (nd.a < 0)
        return -1;
    return emit(c, &nd);
}

static int32_t compile_ap2(hc_df_compiler_t *c, const hc_json_t *obj,
                           uint8_t op) {
    const hc_json_t *a1 = get_field(c, obj, "argument1");
    const hc_json_t *a2 = get_field(c, obj, "argument2");
    if (!a1 || !a2)
        return -1;

    if (op == HC_DF_ADD || op == HC_DF_MUL) {
        double k;
        uint8_t cop = op == HC_DF_ADD ? HC_DF_ADD_CONST : HC_DF_MUL_CONST;
        if (is_const_arg(a1, &k)) { /* 바닐라도 arg1 을 먼저 본다 */
            hc_df_node_t nd = mknode(cop);
            nd.k0 = k;
            nd.a = hc_df_compile_expr(c, a2);
            return nd.a < 0 ? -1 : emit(c, &nd);
        }
        if (is_const_arg(a2, &k)) {
            hc_df_node_t nd = mknode(cop);
            nd.k0 = k;
            nd.a = hc_df_compile_expr(c, a1);
            return nd.a < 0 ? -1 : emit(c, &nd);
        }
    }

    hc_df_node_t nd = mknode(op);
    nd.a = hc_df_compile_expr(c, a1);
    if (nd.a < 0)
        return -1;
    nd.b = hc_df_compile_expr(c, a2);
    if (nd.b < 0)
        return -1;
    return emit(c, &nd);
}

static int type_is(const hc_json_t *t, const char *bare) {
    const char *s = t->s;
    int32_t     len = t->slen;
    if (len > 10 && memcmp(s, "minecraft:", 10) == 0) {
        s += 10;
        len -= 10;
    }
    return (int32_t)strlen(bare) == len && memcmp(s, bare, (size_t)len) == 0;
}

int32_t hc_df_compile_expr(hc_df_compiler_t *c, const hc_json_t *expr) {
    if (c->err)
        return -1;
    if (++c->depth > HC_DFC_MAX_DEPTH) {
        c->depth--;
        return fail(c, "expression nesting too deep");
    }

    int32_t r = -1;

    if (expr->kind == HC_JSON_NUM) {
        hc_df_node_t nd = mknode(HC_DF_CONST);
        nd.k0 = expr->num;
        r = emit(c, &nd);
        goto out;
    }
    if (expr->kind == HC_JSON_STR) {
        r = compile_named(c, expr->s, expr->slen);
        goto out;
    }
    if (expr->kind != HC_JSON_OBJ) {
        r = fail(c, "df expression is neither number, string nor object");
        goto out;
    }

    {
        const hc_json_t *t = hc_json_get(expr, "type");
        if (!t || t->kind != HC_JSON_STR) {
            r = fail(c, "df object without type");
            goto out;
        }

        if (type_is(t, "add")) {
            r = compile_ap2(c, expr, HC_DF_ADD);
        } else if (type_is(t, "mul")) {
            r = compile_ap2(c, expr, HC_DF_MUL);
        } else if (type_is(t, "min")) {
            r = compile_ap2(c, expr, HC_DF_MIN);
        } else if (type_is(t, "max")) {
            r = compile_ap2(c, expr, HC_DF_MAX);
        } else if (type_is(t, "abs")) {
            r = compile_unary(c, expr, HC_DF_ABS);
        } else if (type_is(t, "square")) {
            r = compile_unary(c, expr, HC_DF_SQUARE);
        } else if (type_is(t, "cube")) {
            r = compile_unary(c, expr, HC_DF_CUBE);
        } else if (type_is(t, "half_negative")) {
            r = compile_unary(c, expr, HC_DF_HALF_NEGATIVE);
        } else if (type_is(t, "quarter_negative")) {
            r = compile_unary(c, expr, HC_DF_QUARTER_NEGATIVE);
        } else if (type_is(t, "squeeze")) {
            r = compile_unary(c, expr, HC_DF_SQUEEZE);
        } else if (type_is(t, "invert")) {
            r = compile_unary(c, expr, HC_DF_INVERT);
        } else if (type_is(t, "blend_density")) {
            r = compile_unary(c, expr, HC_DF_BLEND_DENSITY);
        } else if (type_is(t, "interpolated")) {
            r = compile_unary(c, expr, HC_DF_INTERPOLATED);
        } else if (type_is(t, "flat_cache")) {
            r = compile_unary(c, expr, HC_DF_FLAT_CACHE);
        } else if (type_is(t, "cache_2d")) {
            r = compile_unary(c, expr, HC_DF_CACHE_2D);
        } else if (type_is(t, "cache_once")) {
            r = compile_unary(c, expr, HC_DF_CACHE_ONCE);
        } else if (type_is(t, "cache_all_in_cell")) {
            r = compile_unary(c, expr, HC_DF_CACHE_ALL_IN_CELL);
        } else if (type_is(t, "blend_offset")) {
            hc_df_node_t nd = mknode(HC_DF_BLEND_OFFSET);
            r = emit(c, &nd);
        } else if (type_is(t, "blend_alpha")) {
            hc_df_node_t nd = mknode(HC_DF_BLEND_ALPHA);
            r = emit(c, &nd);
        } else if (type_is(t, "constant")) {
            hc_df_node_t nd = mknode(HC_DF_CONST);
            if (get_num(c, expr, "argument", &nd.k0)) {
                r = -1;
            } else {
                r = emit(c, &nd);
            }
        } else if (type_is(t, "clamp")) {
            const hc_json_t *in = get_field(c, expr, "input");
            hc_df_node_t     nd = mknode(HC_DF_CLAMP);
            if (!in || get_num(c, expr, "min", &nd.k0) ||
                get_num(c, expr, "max", &nd.k1)) {
                r = -1;
            } else {
                nd.a = hc_df_compile_expr(c, in);
                r = nd.a < 0 ? -1 : emit(c, &nd);
            }
        } else if (type_is(t, "y_clamped_gradient")) {
            hc_df_node_t nd = mknode(HC_DF_Y_CLAMPED_GRADIENT);
            if (get_num(c, expr, "from_y", &nd.k0) ||
                get_num(c, expr, "to_y", &nd.k1) ||
                get_num(c, expr, "from_value", &nd.k2) ||
                get_num(c, expr, "to_value", &nd.k3)) {
                r = -1;
            } else {
                r = emit(c, &nd);
            }
        } else if (type_is(t, "range_choice")) {
            const hc_json_t *in = get_field(c, expr, "input");
            const hc_json_t *wi = get_field(c, expr, "when_in_range");
            const hc_json_t *wo = get_field(c, expr, "when_out_of_range");
            hc_df_node_t     nd = mknode(HC_DF_RANGE_CHOICE);
            if (!in || !wi || !wo ||
                get_num(c, expr, "min_inclusive", &nd.k0) ||
                get_num(c, expr, "max_exclusive", &nd.k1)) {
                r = -1;
            } else {
                nd.a = hc_df_compile_expr(c, in);
                nd.b = nd.a < 0 ? -1 : hc_df_compile_expr(c, wi);
                nd.c = nd.b < 0 ? -1 : hc_df_compile_expr(c, wo);
                r = nd.c < 0 ? -1 : emit(c, &nd);
            }
        } else if (type_is(t, "noise")) {
            hc_df_node_t nd = mknode(HC_DF_NOISE);
            nd.aux = noise_field(c, expr, "noise");
            if (nd.aux < 0 || get_num(c, expr, "xz_scale", &nd.k0) ||
                get_num(c, expr, "y_scale", &nd.k1)) {
                r = -1;
            } else {
                r = emit(c, &nd);
            }
        } else if (type_is(t, "shifted_noise")) {
            const hc_json_t *sx = get_field(c, expr, "shift_x");
            const hc_json_t *sy = get_field(c, expr, "shift_y");
            const hc_json_t *sz = get_field(c, expr, "shift_z");
            hc_df_node_t     nd = mknode(HC_DF_SHIFTED_NOISE);
            nd.aux = noise_field(c, expr, "noise");
            if (!sx || !sy || !sz || nd.aux < 0 ||
                get_num(c, expr, "xz_scale", &nd.k0) ||
                get_num(c, expr, "y_scale", &nd.k1)) {
                r = -1;
            } else {
                nd.a = hc_df_compile_expr(c, sx);
                nd.b = nd.a < 0 ? -1 : hc_df_compile_expr(c, sy);
                nd.c = nd.b < 0 ? -1 : hc_df_compile_expr(c, sz);
                r = nd.c < 0 ? -1 : emit(c, &nd);
            }
        } else if (type_is(t, "shift_a")) {
            hc_df_node_t nd = mknode(HC_DF_SHIFT_A);
            nd.aux = noise_field(c, expr, "argument");
            r = nd.aux < 0 ? -1 : emit(c, &nd);
        } else if (type_is(t, "shift_b")) {
            hc_df_node_t nd = mknode(HC_DF_SHIFT_B);
            nd.aux = noise_field(c, expr, "argument");
            r = nd.aux < 0 ? -1 : emit(c, &nd);
        } else if (type_is(t, "old_blended_noise")) {
            double xzs, ys, xzf, yf, sm;
            if (get_num(c, expr, "xz_scale", &xzs) ||
                get_num(c, expr, "y_scale", &ys) ||
                get_num(c, expr, "xz_factor", &xzf) ||
                get_num(c, expr, "y_factor", &yf) ||
                get_num(c, expr, "smear_scale_multiplier", &sm)) {
                r = -1;
                goto out;
            }
            if (c->g->n_blended >= HC_DFC_MAX_BLENDED) {
                r = fail(c, "blended noise cap exceeded");
                goto out;
            }
            /* NoiseWiringHelper.wrapNew: withNewRandom(
             *   forkPositional().fromHashOf("minecraft:terrain")) */
            hc_xoro_t      base, rand;
            hc_xoro_fork_t fk;
            hc_xoro_init(&base, c->seed);
            hc_xoro_fork_positional(&base, &fk);
            hc_xoro_from_hash_of(&fk, "minecraft:terrain", &rand);
            int32_t bi = c->g->n_blended;
            if (hc_blended_init(&c->g->blended[bi], c->arena, &rand, xzs, ys,
                                xzf, yf, sm) != 0) {
                r = fail(c, "blended noise init failed");
                goto out;
            }
            c->g->n_blended++;
            hc_df_node_t nd = mknode(HC_DF_BLENDED_NOISE);
            nd.aux = bi;
            r = emit(c, &nd);
        } else if (type_is(t, "spline")) {
            const hc_json_t *sp = get_field(c, expr, "spline");
            if (!sp) {
                r = -1;
                goto out;
            }
            hc_df_node_t nd = mknode(HC_DF_SPLINE);
            nd.aux = compile_spline_spec(c, sp);
            r = nd.aux < 0 ? -1 : emit(c, &nd);
        } else if (type_is(t, "interval_select")) {
            const hc_json_t *in = get_field(c, expr, "input");
            const hc_json_t *fns = get_field(c, expr, "functions");
            const hc_json_t *ths = get_field(c, expr, "thresholds");
            if (!in || !fns || !ths) {
                r = -1;
                goto out;
            }
            if (fns->kind != HC_JSON_ARR || ths->kind != HC_JSON_ARR ||
                fns->count < 1 || ths->count != fns->count - 1 ||
                fns->count > 64) {
                r = fail(c, "interval_select functions/thresholds mismatch");
                goto out;
            }
            hc_df_node_t nd = mknode(HC_DF_INTERVAL_SELECT);
            nd.a = hc_df_compile_expr(c, in);
            if (nd.a < 0) {
                r = -1;
                goto out;
            }
            /* 함수들을 먼저 전부 컴파일한 뒤 (중첩이 풀을 건드린다)
             * 연속 구간으로 풀에 적재 */
            int32_t fn_idx[64];
            int32_t nf = 0;
            for (const hc_json_t *f = fns->child; f; f = f->next) {
                int32_t fi = hc_df_compile_expr(c, f);
                if (fi < 0) {
                    r = -1;
                    goto out;
                }
                fn_idx[nf++] = fi;
            }
            if (c->g->n_ipool + nf + 1 > HC_DFC_MAX_IPOOL ||
                c->g->n_dpool + nf - 1 > HC_DFC_MAX_DPOOL) {
                r = fail(c, "pool cap exceeded (interval_select)");
                goto out;
            }
            nd.aux = c->g->n_ipool;
            c->g->ipool[c->g->n_ipool++] = nf;
            for (int32_t i = 0; i < nf; i++)
                c->g->ipool[c->g->n_ipool++] = fn_idx[i];
            nd.aux2 = c->g->n_dpool;
            for (const hc_json_t *e = ths->child; e; e = e->next) {
                if (e->kind != HC_JSON_NUM) {
                    r = fail(c, "non-number interval_select threshold");
                    goto out;
                }
                c->g->dpool[c->g->n_dpool++] = e->num;
            }
            r = emit(c, &nd);
        } else if (type_is(t, "find_top_surface")) {
            const hc_json_t *den = get_field(c, expr, "density");
            const hc_json_t *ub = get_field(c, expr, "upper_bound");
            hc_df_node_t     nd = mknode(HC_DF_FIND_TOP_SURFACE);
            if (!den || !ub || get_num(c, expr, "lower_bound", &nd.k0) ||
                get_num(c, expr, "cell_height", &nd.k1)) {
                r = -1;
                goto out;
            }
            nd.a = hc_df_compile_expr(c, den);
            nd.b = nd.a < 0 ? -1 : hc_df_compile_expr(c, ub);
            if (nd.b < 0) {
                r = -1;
                goto out;
            }
            int32_t off = 0, len = 0;
            if (build_cone(c, nd.a, &off, &len)) {
                r = -1;
                goto out;
            }
            nd.aux = off;
            nd.aux2 = len;
            r = emit(c, &nd);
        } else {
            r = fail(c, "unknown df type '%.*s'", (int)t->slen, t->s);
        }
    }

out:
    c->depth--;
    return r;
}

int hc_df_compiler_init(hc_df_compiler_t *c, hc_df_graph_t *g,
                        hc_arena_t *arena, int64_t seed,
                        const hc_df_source_t *dfs, int32_t n_dfs,
                        const hc_df_source_t *noise_params,
                        int32_t n_noise_params) {
    memset(c, 0, sizeof *c);
    memset(g, 0, sizeof *g);
    c->arena = arena;
    c->seed = seed;
    c->dfs = dfs;
    c->n_dfs = n_dfs;
    c->noise_params = noise_params;
    c->n_noise_params = n_noise_params;
    c->g = g;
    g->root = -1;

    g->nodes = hc_arena_alloc(arena, sizeof(*g->nodes) * HC_DFC_MAX_NODES,
                              _Alignof(hc_df_node_t));
    g->noises = hc_arena_alloc(arena, sizeof(*g->noises) * HC_DFC_MAX_NOISES,
                               _Alignof(hc_normal_noise_t));
    g->blended = hc_arena_alloc(arena,
                                sizeof(*g->blended) * HC_DFC_MAX_BLENDED,
                                _Alignof(hc_blended_noise_t));
    g->splines = hc_arena_alloc(arena,
                                sizeof(*g->splines) * HC_DFC_MAX_SPLINES,
                                _Alignof(hc_df_spline_t));
    g->ipool = hc_arena_alloc(arena, sizeof(int32_t) * HC_DFC_MAX_IPOOL,
                              _Alignof(int32_t));
    g->dpool = hc_arena_alloc(arena, sizeof(double) * HC_DFC_MAX_DPOOL,
                              _Alignof(double));
    c->df_state = hc_arena_alloc(arena, sizeof(int32_t) * (size_t)n_dfs,
                                 _Alignof(int32_t));
    c->noise_keys = hc_arena_alloc(arena,
                                   sizeof(char *) * HC_DFC_MAX_NOISES,
                                   _Alignof(char *));
    if (!g->nodes || !g->noises || !g->blended || !g->splines || !g->ipool ||
        !g->dpool || (n_dfs > 0 && !c->df_state) || !c->noise_keys)
        return -1;
    for (int32_t i = 0; i < n_dfs; i++)
        c->df_state[i] = -1;
    return 0;
}
