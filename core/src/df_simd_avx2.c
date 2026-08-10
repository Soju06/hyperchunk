#include "hc_df_simd.h"
#include "hc_counters.h"

/* AVX2 4-레인 콘 스트림 평가기 (P2-4, ADR-004 D1). 이 TU 만 -mavx2 로
 * 컴파일된다 — 진입은 hc_isa_active()==AVX2 확인 후에만 (df_isa.c 디스패치,
 * 부재 호스트 SIGILL 방지).
 *
 * 패리티 불변 (ADR-004 D3/D4, 과제 §패리티 불변):
 *  - 레인 = 독립 평가점. 레인별 연산 시퀀스는 스칼라 eval_node /
 *    hc_perlin_sample_scaled / hc_octaves_value / hc_blended_compute 와
 *    문자 그대로 동일 순서다. 수평 리덕션 없음 (합칠 것이 없다).
 *  - FMA 인트린식 (_mm256_fmadd_* 계열) 사용 금지 — mul/add 분리만.
 *    check_no_fma.sh 가 산출물에서 재확인한다.
 *  - 나눗셈은 vdivpd 만 (IEEE 정확 반올림 == 스칼라 나눗셈 비트 동일).
 *    rcp/rsqrt 근사 명령 금지.
 *  - jmin/jmax/clamp/±0/NaN 은 cmp+blend 로 Java 시맨틱 재현 — blend 는
 *    비트 선택이라 값을 만들지 않는다.
 *  - Mth.floor/lfloor 의 포화·NaN 시맨틱이 필요한 범위 밖 좌표 레인이
 *    하나라도 있으면 그 노드는 전 레인 스칼라 폴백 (26.2 오버월드 도달
 *    불가 — 데이터팩 방어). 범위 안에서 _mm256_floor_pd == floor() 는
 *    IEEE 정확 연산이라 비트 동일. */

#if defined(__x86_64__) || defined(__i386__)

#include <assert.h>
#include <immintrin.h>

typedef __m256d V4;

static inline V4 bc(double v) {
    return _mm256_set1_pd(v);
}

/* --- Java 시맨틱 프리미티브 (df_eval.c 의 스칼라 판과 1:1) --- */

/* java.lang.Math.min: NaN a → a; (+0,-0) → -0; else a<=b?a:b */
static inline V4 v_jmin(V4 a, V4 b) {
    V4 le = _mm256_cmp_pd(a, b, _CMP_LE_OQ);
    V4 r = _mm256_blendv_pd(b, a, le);
    V4 z = _mm256_setzero_pd();
    V4 both0 = _mm256_and_pd(_mm256_cmp_pd(a, z, _CMP_EQ_OQ),
                             _mm256_cmp_pd(b, z, _CMP_EQ_OQ));
    /* blendv 는 마스크 부호비트만 본다 — both0(전비트 1) & b 의 부호 */
    r = _mm256_blendv_pd(r, b, _mm256_and_pd(both0, b));
    V4 nan_a = _mm256_cmp_pd(a, a, _CMP_UNORD_Q);
    return _mm256_blendv_pd(r, a, nan_a);
}

/* java.lang.Math.max: NaN a → a; (-0,+0) → +0(b); else a>=b?a:b */
static inline V4 v_jmax(V4 a, V4 b) {
    V4 ge = _mm256_cmp_pd(a, b, _CMP_GE_OQ);
    V4 r = _mm256_blendv_pd(b, a, ge);
    V4 z = _mm256_setzero_pd();
    V4 both0 = _mm256_and_pd(_mm256_cmp_pd(a, z, _CMP_EQ_OQ),
                             _mm256_cmp_pd(b, z, _CMP_EQ_OQ));
    r = _mm256_blendv_pd(r, b, _mm256_and_pd(both0, a));
    V4 nan_a = _mm256_cmp_pd(a, a, _CMP_UNORD_Q);
    return _mm256_blendv_pd(r, a, nan_a);
}

/* Mth.clamp: v < lo ? lo : Math.min(v, hi) */
static inline V4 v_clamp(V4 v, V4 lo, V4 hi) {
    V4 lt = _mm256_cmp_pd(v, lo, _CMP_LT_OQ);
    return _mm256_blendv_pd(v_jmin(v, hi), lo, lt);
}

/* Mth.lerp: a + t*(b-a) — mul→add 분리 (FMA 금지) */
static inline V4 v_lerp(V4 t, V4 a, V4 b) {
    return _mm256_add_pd(a, _mm256_mul_pd(t, _mm256_sub_pd(b, a)));
}

/* Mth.clampedLerp(delta 첫 인자 판): delta<0→a, delta>1→b, NaN→lerp */
static inline V4 v_clamped_lerp(V4 t, V4 a, V4 b) {
    V4 r = v_lerp(t, a, b);
    r = _mm256_blendv_pd(r, a, _mm256_cmp_pd(t, bc(0.0), _CMP_LT_OQ));
    r = _mm256_blendv_pd(r, b, _mm256_cmp_pd(t, bc(1.0), _CMP_GT_OQ));
    return r;
}

/* Mth.smoothstep: ((t*t)*t) * ((t*(t*6-15)) + 10) */
static inline V4 v_smoothstep(V4 t) {
    V4 t3 = _mm256_mul_pd(_mm256_mul_pd(t, t), t);
    V4 inner = _mm256_add_pd(
        _mm256_mul_pd(t, _mm256_sub_pd(_mm256_mul_pd(t, bc(6.0)), bc(15.0))),
        bc(10.0));
    return _mm256_mul_pd(t3, inner);
}

/* |v| < bound 이고 비-NaN — Mth.floor/lfloor 무포화 보장 범위 검사 */
static inline int v_all_in(V4 v, double bound) {
    V4 abs = _mm256_andnot_pd(bc(-0.0), v);
    V4 ok = _mm256_cmp_pd(abs, bc(bound), _CMP_LT_OQ); /* NaN → false */
    return _mm256_movemask_pd(ok) == 0xF;
}

/* --- PerlinNoise.wrap x4: d - lfloor(d/2^25 + 0.5)*2^25 --- */

static V4 wrap_x4(V4 d) {
    V4 q = _mm256_add_pd(_mm256_div_pd(d, bc(33554432.0)), bc(0.5));
    if (!v_all_in(q, 4611686018427387904.0 /* 2^62 */)) {
        double ds[4], out[4];
        _mm256_storeu_pd(ds, d);
        for (int l = 0; l < 4; l++)
            out[l] = hc_octaves_wrap(ds[l]);
        return _mm256_loadu_pd(out);
    }
    V4 fl = _mm256_floor_pd(q); /* 범위 안: (double)mth_lfloor 와 동일 */
    return _mm256_sub_pd(d, _mm256_mul_pd(fl, bc(33554432.0)));
}

/* --- ImprovedNoise x4 --- */

/* SimplexNoise.GRADIENT — noise_perlin.c GRAD 와 동일 값 (SoT 는 바닐라
 * <clinit>; 값 불일치는 df_x4 게이트의 비트 대조가 즉시 잡는다).
 * 행 = (gx, gy, gz, 0) — 4x4 전치로 레인 벡터를 만든다. */
static const double GRAD4[16][4] __attribute__((aligned(32))) = {
    {1, 1, 0, 0},  {-1, 1, 0, 0},  {1, -1, 0, 0},  {-1, -1, 0, 0},
    {1, 0, 1, 0},  {-1, 0, 1, 0},  {1, 0, -1, 0},  {-1, 0, -1, 0},
    {0, 1, 1, 0},  {0, -1, 1, 0},  {0, 1, -1, 0},  {0, -1, -1, 0},
    {1, 1, 0, 0},  {0, -1, 1, 0},  {-1, 1, 0, 0},  {0, -1, -1, 0},
};

/* 코너 하나: 레인별 gradient 행 로드 → 전치 → ((g0*x)+(g1*y))+(g2*z).
 * 해시 체인은 정수 테이블 조회라 레인별 스칼라 (비트 정확 무비용). */
static inline V4 corner_dot(const hc_perlin_t *p, const int h[4],
                            const int iz[4], int zoff, V4 xv, V4 yv, V4 zv) {
    V4 r0 = _mm256_load_pd(GRAD4[p->perm[(h[0] + iz[0] + zoff) & 255] & 15]);
    V4 r1 = _mm256_load_pd(GRAD4[p->perm[(h[1] + iz[1] + zoff) & 255] & 15]);
    V4 r2 = _mm256_load_pd(GRAD4[p->perm[(h[2] + iz[2] + zoff) & 255] & 15]);
    V4 r3 = _mm256_load_pd(GRAD4[p->perm[(h[3] + iz[3] + zoff) & 255] & 15]);
    V4 t0 = _mm256_unpacklo_pd(r0, r1); /* x0 x1 z0 z1 */
    V4 t1 = _mm256_unpackhi_pd(r0, r1); /* y0 y1 w0 w1 */
    V4 t2 = _mm256_unpacklo_pd(r2, r3);
    V4 t3 = _mm256_unpackhi_pd(r2, r3);
    V4 gx = _mm256_permute2f128_pd(t0, t2, 0x20);
    V4 gy = _mm256_permute2f128_pd(t1, t3, 0x20);
    V4 gz = _mm256_permute2f128_pd(t0, t2, 0x31);
    /* SimplexNoise.dot 좌결합: ((g0*x) + (g1*y)) + (g2*z) */
    return _mm256_add_pd(
        _mm256_add_pd(_mm256_mul_pd(gx, xv), _mm256_mul_pd(gy, yv)),
        _mm256_mul_pd(gz, zv));
}

/* hc_perlin_sample_scaled x4. yscale 은 옥타브 스칼라 (전 레인 동일),
 * ymax 는 레인별 (BlendedNoise 의 gy*o). 범위 밖/NaN 레인 → 전 레인
 * 스칼라 폴백. */
static V4 perlin_x4(const hc_perlin_t *p, V4 x, V4 y, V4 z, double yscale,
                    V4 ymax) {
    HC_CTR_INC(HC_CTR_X4_PERLIN);
    V4 dx = _mm256_add_pd(x, bc(p->xo));
    V4 dy = _mm256_add_pd(y, bc(p->yo));
    V4 dz = _mm256_add_pd(z, bc(p->zo));
    const double B30 = 1073741824.0; /* 2^30 — mth_floor 안전 마진 */
    if (!(v_all_in(dx, B30) && v_all_in(dy, B30) && v_all_in(dz, B30)))
        goto scalar_fallback;

    {
        V4 fdx = _mm256_floor_pd(dx);
        V4 fdy = _mm256_floor_pd(dy);
        V4 fdz = _mm256_floor_pd(dz);
        V4 fx = _mm256_sub_pd(dx, fdx);
        V4 fy = _mm256_sub_pd(dy, fdy);
        V4 fz = _mm256_sub_pd(dz, fdz);

        V4 gy = fy;
        if (yscale != 0.0) {
            /* t = (ymax >= 0 && ymax < fy) ? ymax : fy;
             * yadj = (double)mth_floor(t/yscale + (double)1.0E-7f)*yscale */
            V4 use_ymax = _mm256_and_pd(
                _mm256_cmp_pd(ymax, _mm256_setzero_pd(), _CMP_GE_OQ),
                _mm256_cmp_pd(ymax, fy, _CMP_LT_OQ));
            V4 t = _mm256_blendv_pd(fy, ymax, use_ymax);
            V4 q = _mm256_add_pd(_mm256_div_pd(t, bc(yscale)),
                                 bc(0x1.AD7F2Ap-24));
            if (!v_all_in(q, B30))
                goto scalar_fallback;
            V4 yadj = _mm256_mul_pd(_mm256_floor_pd(q), bc(yscale));
            gy = _mm256_sub_pd(fy, yadj);
        }

        /* 해시 체인 (레인별 정수 — 스칼라 P() 와 동일) */
        double fdxs[4], fdys[4], fdzs[4];
        _mm256_storeu_pd(fdxs, fdx);
        _mm256_storeu_pd(fdys, fdy);
        _mm256_storeu_pd(fdzs, fdz);
        int iz[4], aa[4], ab[4], ba[4], bb[4];
        for (int l = 0; l < 4; l++) {
            int ix = (int)fdxs[l];
            int iy = (int)fdys[l];
            iz[l] = (int)fdzs[l];
            int A = p->perm[ix & 255];
            int B = p->perm[(ix + 1) & 255];
            aa[l] = p->perm[(A + iy) & 255];
            ab[l] = p->perm[(A + iy + 1) & 255];
            ba[l] = p->perm[(B + iy) & 255];
            bb[l] = p->perm[(B + iy + 1) & 255];
        }

        V4 fx1 = _mm256_sub_pd(fx, bc(1.0));
        V4 gy1 = _mm256_sub_pd(gy, bc(1.0));
        V4 fz1 = _mm256_sub_pd(fz, bc(1.0));

        V4 v000 = corner_dot(p, aa, iz, 0, fx, gy, fz);
        V4 v100 = corner_dot(p, ba, iz, 0, fx1, gy, fz);
        V4 v010 = corner_dot(p, ab, iz, 0, fx, gy1, fz);
        V4 v110 = corner_dot(p, bb, iz, 0, fx1, gy1, fz);
        V4 v001 = corner_dot(p, aa, iz, 1, fx, gy, fz1);
        V4 v101 = corner_dot(p, ba, iz, 1, fx1, gy, fz1);
        V4 v011 = corner_dot(p, ab, iz, 1, fx, gy1, fz1);
        V4 v111 = corner_dot(p, bb, iz, 1, fx1, gy1, fz1);

        V4 sx = v_smoothstep(fx);
        V4 sy = v_smoothstep(fy); /* fade 는 양자화 전 fy */
        V4 sz = v_smoothstep(fz);

        V4 l0 = v_lerp(sy, v_lerp(sx, v000, v100), v_lerp(sx, v010, v110));
        V4 l1 = v_lerp(sy, v_lerp(sx, v001, v101), v_lerp(sx, v011, v111));
        return v_lerp(sz, l0, l1);
    }

scalar_fallback:; /* 포화/NaN 시맨틱은 스칼라 경로가 소유 (희귀) */
    HC_CTR_INC(HC_CTR_X4_SCALAR_FB);
    {
        double xs[4], ys[4], zs[4], ym[4], out[4];
        _mm256_storeu_pd(xs, x);
        _mm256_storeu_pd(ys, y);
        _mm256_storeu_pd(zs, z);
        _mm256_storeu_pd(ym, ymax);
        for (int l = 0; l < 4; l++)
            out[l] = hc_perlin_sample_scaled(p, xs[l], ys[l], zs[l], yscale,
                                             ym[l]);
        return _mm256_loadu_pd(out);
    }
}

/* --- PerlinNoise.getValue x4 (hc_octaves_value 동형) --- */

static V4 octaves_x4(const hc_octaves_t *o, V4 x, V4 y, V4 z, double yscale,
                     V4 ymax) {
    V4     d = _mm256_setzero_pd();
    double e = o->lowest_freq_input;
    double f = o->lowest_freq_value;
    for (int32_t i = 0; i < o->count; i++) {
        const hc_perlin_t *p = o->octaves[i];
        if (p) {
            V4 g = perlin_x4(p, wrap_x4(_mm256_mul_pd(x, bc(e))),
                             wrap_x4(_mm256_mul_pd(y, bc(e))),
                             wrap_x4(_mm256_mul_pd(z, bc(e))), yscale * e,
                             _mm256_mul_pd(ymax, bc(e)));
            /* d += (amplitudes[i] * g) * f — 좌결합 그대로 */
            d = _mm256_add_pd(
                d, _mm256_mul_pd(_mm256_mul_pd(bc(o->amplitudes[i]), g),
                                 bc(f)));
        }
        e *= 2.0;
        f /= 2.0;
    }
    return d;
}

/* --- NormalNoise.getValue x4 --- */

static V4 normal_x4(const hc_normal_noise_t *n, V4 x, V4 y, V4 z) {
    V4 x2 = _mm256_mul_pd(x, bc(HC_NORMAL_INPUT_FACTOR));
    V4 y2 = _mm256_mul_pd(y, bc(HC_NORMAL_INPUT_FACTOR));
    V4 z2 = _mm256_mul_pd(z, bc(HC_NORMAL_INPUT_FACTOR));
    V4 zero = _mm256_setzero_pd();
    V4 a = octaves_x4(&n->first, x, y, z, 0.0, zero);
    V4 b = octaves_x4(&n->second, x2, y2, z2, 0.0, zero);
    return _mm256_mul_pd(_mm256_add_pd(a, b), bc(n->value_factor));
}

/* --- BlendedNoise.compute x4 (hc_blended_compute 동형) --- */

/* PerlinNoise.getOctaveNoise(i) = noiseLevels[count - 1 - i] */
static const hc_perlin_t *oct_at(const hc_octaves_t *o, int32_t i) {
    return o->octaves[o->count - 1 - i];
}

static V4 blended_x4(const hc_blended_noise_t *b, V4 xi, V4 yi, V4 zi) {
    V4     d = _mm256_mul_pd(xi, bc(b->xz_mult));
    V4     e = _mm256_mul_pd(yi, bc(b->y_mult));
    V4     f = _mm256_mul_pd(zi, bc(b->xz_mult));
    V4     gx = _mm256_div_pd(d, bc(b->xz_factor));
    V4     gy = _mm256_div_pd(e, bc(b->y_factor));
    V4     gz = _mm256_div_pd(f, bc(b->xz_factor));
    double smear_y = b->y_mult * b->smear;
    double smear_g = smear_y / b->y_factor;

    V4     n = _mm256_setzero_pd();
    double o = 1.0;
    for (int p = 0; p < 8; p++) {
        const hc_perlin_t *oct = oct_at(&b->main_noise, p);
        if (oct) {
            V4 s = perlin_x4(oct, wrap_x4(_mm256_mul_pd(gx, bc(o))),
                             wrap_x4(_mm256_mul_pd(gy, bc(o))),
                             wrap_x4(_mm256_mul_pd(gz, bc(o))), smear_g * o,
                             _mm256_mul_pd(gy, bc(o)));
            n = _mm256_add_pd(n, _mm256_div_pd(s, bc(o)));
        }
        o /= 2.0;
    }
    V4 q = _mm256_div_pd(
        _mm256_add_pd(_mm256_div_pd(n, bc(10.0)), bc(1.0)), bc(2.0));

    /* over = q >= 1.0 (min 측 생략), under = q <= 0.0 (max 측 생략).
     * 스칼라는 생략 측 누산을 0.0 으로 남긴다 — 레인 벡터에서는 일단
     * 계산 후 마스크로 +0.0 강제 (계산된 죽은 값은 읽히지 않는다). */
    V4  overm = _mm256_cmp_pd(q, bc(1.0), _CMP_GE_OQ);
    V4  underm = _mm256_cmp_pd(q, bc(0.0), _CMP_LE_OQ);
    int over_all = _mm256_movemask_pd(overm) == 0xF;
    int under_all = _mm256_movemask_pd(underm) == 0xF;
    if (hc_ctr_on) { /* 혼합-레인 = 패리티가 강제하는 죽은-레인 뱅크 계산 */
        int mo = _mm256_movemask_pd(overm), mu = _mm256_movemask_pd(underm);
        if ((mo != 0 && mo != 0xF) || (mu != 0 && mu != 0xF))
            hc_ctr_tls[HC_CTR_X4_BLEND_MIX]++;
    }

    V4 lo = _mm256_setzero_pd();
    V4 hi = _mm256_setzero_pd();
    o = 1.0;
    for (int r = 0; r < 16; r++) {
        V4     s = wrap_x4(_mm256_mul_pd(d, bc(o)));
        V4     t = wrap_x4(_mm256_mul_pd(e, bc(o)));
        V4     u = wrap_x4(_mm256_mul_pd(f, bc(o)));
        double v = smear_y * o;
        if (!over_all) {
            const hc_perlin_t *oct = oct_at(&b->min_limit, r);
            if (oct)
                lo = _mm256_add_pd(
                    lo, _mm256_div_pd(perlin_x4(oct, s, t, u, v,
                                                _mm256_mul_pd(e, bc(o))),
                                      bc(o)));
        }
        if (!under_all) {
            const hc_perlin_t *oct = oct_at(&b->max_limit, r);
            if (oct)
                hi = _mm256_add_pd(
                    hi, _mm256_div_pd(perlin_x4(oct, s, t, u, v,
                                                _mm256_mul_pd(e, bc(o))),
                                      bc(o)));
        }
        o /= 2.0;
    }
    /* 생략 레인은 스칼라와 동일하게 +0.0 (andnot = ~mask & val) */
    lo = _mm256_andnot_pd(overm, lo);
    hi = _mm256_andnot_pd(underm, hi);

    V4 res = v_clamped_lerp(q, _mm256_div_pd(lo, bc(512.0)),
                            _mm256_div_pd(hi, bc(512.0)));
    return _mm256_div_pd(res, bc(128.0));
}

/* --- 스트림 인터프리터 --- */

typedef struct {
    const hc_df_graph_t   *g;
    const hc_df_lanes_t   *lanes;
    double                *vsc; /* [g->n][4], 32B 정렬 */
    const hc_df_cellctx_t *cc;
    V4                     xv, yv, zv;
} x4_env_t;

static void x4_node(const x4_env_t *e, int32_t idx) {
    const hc_df_node_t *nd = &e->g->nodes[idx];
    double             *slot = e->vsc + 4 * (size_t)idx;
#define OPV(n) _mm256_load_pd(e->vsc + 4 * (size_t)(n))
    V4 r;
    switch (nd->op) {
    case HC_DF_CONST:
        r = bc(nd->k0);
        break;
    case HC_DF_X:
        r = e->xv;
        break;
    case HC_DF_Y:
        r = e->yv;
        break;
    case HC_DF_Z:
        r = e->zv;
        break;

    case HC_DF_ADD:
        r = _mm256_add_pd(OPV(nd->a), OPV(nd->b));
        break;
    case HC_DF_MUL: {
        /* Ap2 MUL 단락: arg1 == 0.0 (±0) 이면 +0.0 */
        V4 a = OPV(nd->a);
        V4 m = _mm256_cmp_pd(a, _mm256_setzero_pd(), _CMP_EQ_OQ);
        r = _mm256_blendv_pd(_mm256_mul_pd(a, OPV(nd->b)),
                             _mm256_setzero_pd(), m);
        break;
    }
    case HC_DF_MIN:
        r = v_jmin(OPV(nd->a), OPV(nd->b));
        break;
    case HC_DF_MAX:
        r = v_jmax(OPV(nd->a), OPV(nd->b));
        break;
    case HC_DF_ADD_CONST:
        r = _mm256_add_pd(OPV(nd->a), bc(nd->k0));
        break;
    case HC_DF_MUL_CONST:
        r = _mm256_mul_pd(OPV(nd->a), bc(nd->k0));
        break;

    case HC_DF_ABS:
        r = _mm256_andnot_pd(bc(-0.0), OPV(nd->a));
        break;
    case HC_DF_SQUARE: {
        V4 a = OPV(nd->a);
        r = _mm256_mul_pd(a, a);
        break;
    }
    case HC_DF_CUBE: {
        V4 a = OPV(nd->a);
        r = _mm256_mul_pd(_mm256_mul_pd(a, a), a);
        break;
    }
    case HC_DF_HALF_NEGATIVE: {
        V4 a = OPV(nd->a);
        V4 pos = _mm256_cmp_pd(a, _mm256_setzero_pd(), _CMP_GT_OQ);
        r = _mm256_blendv_pd(_mm256_mul_pd(a, bc(0.5)), a, pos);
        break;
    }
    case HC_DF_QUARTER_NEGATIVE: {
        V4 a = OPV(nd->a);
        V4 pos = _mm256_cmp_pd(a, _mm256_setzero_pd(), _CMP_GT_OQ);
        r = _mm256_blendv_pd(_mm256_mul_pd(a, bc(0.25)), a, pos);
        break;
    }
    case HC_DF_SQUEEZE: {
        /* d/2 - ((d*d)*d)/24 */
        V4 d = v_clamp(OPV(nd->a), bc(-1.0), bc(1.0));
        r = _mm256_sub_pd(
            _mm256_div_pd(d, bc(2.0)),
            _mm256_div_pd(_mm256_mul_pd(_mm256_mul_pd(d, d), d), bc(24.0)));
        break;
    }
    case HC_DF_INVERT:
        r = _mm256_div_pd(bc(1.0), OPV(nd->a));
        break;

    case HC_DF_CLAMP:
        r = v_clamp(OPV(nd->a), bc(nd->k0), bc(nd->k1));
        break;

    case HC_DF_Y_CLAMPED_GRADIENT: {
        /* t = (y-k0)/(k1-k0); t<0→k2, t>1→k3, else k2+t*(k3-k2) */
        V4 t = _mm256_div_pd(_mm256_sub_pd(e->yv, bc(nd->k0)),
                             bc(nd->k1 - nd->k0));
        r = v_clamped_lerp(t, bc(nd->k2), bc(nd->k3));
        break;
    }

    case HC_DF_RANGE_CHOICE: {
        V4 d = OPV(nd->a);
        V4 in = _mm256_and_pd(_mm256_cmp_pd(d, bc(nd->k0), _CMP_GE_OQ),
                              _mm256_cmp_pd(d, bc(nd->k1), _CMP_LT_OQ));
        r = _mm256_blendv_pd(OPV(nd->c), OPV(nd->b), in);
        break;
    }

    case HC_DF_NOISE: {
        V4 nx = _mm256_mul_pd(e->xv, bc(nd->k0));
        V4 ny = _mm256_mul_pd(e->yv, bc(nd->k1));
        V4 nz = _mm256_mul_pd(e->zv, bc(nd->k0));
        r = normal_x4(&e->g->noises[nd->aux], nx, ny, nz);
        break;
    }
    case HC_DF_BLENDED_NOISE:
        /* 스칼라는 (int32_t) 캐스트 후 승격 — 레인 좌표는 정수값 전제
         * (블록 좌표 계약, hc_df_simd.h) 라 값이 같다 */
        r = blended_x4(&e->g->blended[nd->aux], e->xv, e->yv, e->zv);
        break;

    case HC_DF_INTERVAL_SELECT: {
        /* 선택·회수는 레인별 스칼라 (정수/비교 — 반올림 없음) */
        const double *av = e->vsc + 4 * (size_t)nd->a;
        int32_t       nf = e->g->ipool[nd->aux];
        double        out[4];
        for (int l = 0; l < 4; l++) {
            double  dv = av[l];
            int32_t sel = nf - 1;
            for (int32_t j = 0; j < nf - 1; j++)
                if (dv < e->g->dpool[nd->aux2 + j]) {
                    sel = j;
                    break;
                }
            out[l] = e->vsc[4 * (size_t)e->g->ipool[nd->aux + 1 + sel] + l];
        }
        r = _mm256_loadu_pd(out);
        break;
    }

    case HC_DF_BLEND_OFFSET:
        r = _mm256_setzero_pd();
        break;
    case HC_DF_BLEND_ALPHA:
        r = bc(1.0);
        break;

    case HC_DF_INTERPOLATED:
        if (e->cc && e->cc->interp_of[idx] >= 0) {
            const hc_df_interp_t *it = &e->cc->interp[e->cc->interp_of[idx]];
            if (e->cc->mode == HC_DF_MODE_CELL) {
                /* interp_lerp3 동형 — 델타는 레인별 (사전 나눗셈 값) */
                V4 dxv = _mm256_loadu_pd(e->lanes->dx);
                V4 dyv = _mm256_loadu_pd(e->lanes->dy);
                V4 dzv = _mm256_loadu_pd(e->lanes->dz);
                V4 a0 = v_lerp(dyv, v_lerp(dxv, bc(it->n000), bc(it->n100)),
                               v_lerp(dxv, bc(it->n010), bc(it->n110)));
                V4 a1 = v_lerp(dyv, v_lerp(dxv, bc(it->n001), bc(it->n101)),
                               v_lerp(dxv, bc(it->n011), bc(it->n111)));
                r = v_lerp(dzv, a0, a1);
                break;
            }
            if (e->cc->mode == HC_DF_MODE_BLOCK) {
                r = bc(it->value);
                break;
            }
        }
        r = OPV(nd->a); /* SP pass-through */
        break;

    case HC_DF_FLAT_CACHE:
        if (e->cc && e->cc->flat_of[idx] >= 0) {
            /* 레인별 조회 (좌표는 레인별 — 창 검사 포함, eval_node 동형) */
            const hc_df_flat_t *fl = &e->cc->flat[e->cc->flat_of[idx]];
            int32_t             size = e->cc->noise_size_xz + 1;
            const double       *av = e->vsc + 4 * (size_t)nd->a;
            double              out[4];
            for (int l = 0; l < 4; l++) {
                int32_t qx = (int32_t)e->lanes->x[l] >> 2;
                int32_t qz = (int32_t)e->lanes->z[l] >> 2;
                int32_t i = qx - e->cc->first_noise_x;
                int32_t j = qz - e->cc->first_noise_z;
                if (i >= 0 && j >= 0 && i < size && j < size) {
                    out[l] = fl->values[i + j * size];
                } else {
                    /* 창-밖 폴백 — x4 호출 문맥 (fill_slice/cell) 은 전부
                     * 창 안 보장이라 도달 자체가 콘 계산 버그다 */
                    assert(!"flat_cache out-of-window in x4 context");
                    out[l] = av[l];
                }
            }
            r = _mm256_loadu_pd(out);
            break;
        }
        r = OPV(nd->a);
        break;

    case HC_DF_BLEND_DENSITY:
    case HC_DF_CACHE_2D:
    case HC_DF_CACHE_ONCE:
    case HC_DF_CACHE_ALL_IN_CELL:
        r = OPV(nd->a);
        break;

    default:
        /* hc_df_stream_x4_ok 화이트리스트가 사전 차단한다 */
        assert(!"unsupported op in x4 stream");
        r = _mm256_setzero_pd();
        break;
    }
    _mm256_store_pd(slot, r);
#undef OPV
}

/* prog_run (df_eval.c) 과 동일한 스트림 디코딩 — 세그먼트는 '필요한
 * 레인이 하나라도 있으면' 4레인으로 실행한다. 죽은 레인의 값은 순수
 * 계산이고 blend/회수가 선택하지 않으므로 관측 불가. */
static void x4_run(const x4_env_t *e, const int32_t *p, int32_t words) {
    const int32_t *end = p + words;
    while (p < end) {
        int32_t v = *p++;
        if (v >= 0) {
            HC_CTR_INC_HOT(HC_CTR_X4_NODE);
            x4_node(e, v);
            continue;
        }
        if (v == -1) { /* PROG_RC */
            int32_t             ch = p[0], wt = p[1], we = p[2];
            const hc_df_node_t *nd = &e->g->nodes[ch];
            V4 d = _mm256_load_pd(e->vsc + 4 * (size_t)nd->a);
            V4 in = _mm256_and_pd(_mm256_cmp_pd(d, bc(nd->k0), _CMP_GE_OQ),
                                  _mm256_cmp_pd(d, bc(nd->k1), _CMP_LT_OQ));
            int m = _mm256_movemask_pd(in);
            if (m != 0 && m != 0xF)
                HC_CTR_INC(HC_CTR_X4_RC_MIX);
            if (m != 0)
                x4_run(e, p + 3, wt);
            if (m != 0xF)
                x4_run(e, p + 3 + wt, we);
            _mm256_store_pd(
                e->vsc + 4 * (size_t)ch,
                _mm256_blendv_pd(
                    _mm256_load_pd(e->vsc + 4 * (size_t)nd->c),
                    _mm256_load_pd(e->vsc + 4 * (size_t)nd->b), in));
            p += 3 + wt + we;
        } else { /* PROG_IS */
            int32_t             ch = p[0], nf = p[1];
            const int32_t      *w = p + 2;
            const hc_df_node_t *nd = &e->g->nodes[ch];
            const double       *av = e->vsc + 4 * (size_t)nd->a;
            int32_t             sel[4];
            for (int l = 0; l < 4; l++) {
                double  dv = av[l];
                int32_t s = nf - 1;
                for (int32_t j = 0; j < nf - 1; j++)
                    if (dv < e->g->dpool[nd->aux2 + j]) {
                        s = j;
                        break;
                    }
                sel[l] = s;
            }
            if (sel[0] != sel[1] || sel[1] != sel[2] || sel[2] != sel[3])
                HC_CTR_INC(HC_CTR_X4_IS_MIX);
            const int32_t *q = p + 2 + nf;
            for (int32_t k = 0; k < nf; k++) {
                int need = sel[0] == k || sel[1] == k || sel[2] == k ||
                           sel[3] == k;
                if (need && w[k] > 0)
                    x4_run(e, q, w[k]);
                q += w[k];
            }
            double out[4];
            for (int l = 0; l < 4; l++)
                out[l] =
                    e->vsc[4 * (size_t)e->g->ipool[nd->aux + 1 + sel[l]] + l];
            _mm256_storeu_pd(e->vsc + 4 * (size_t)ch, _mm256_loadu_pd(out));
            p = q;
        }
    }
}

void hc_df_eval_stream_x4_avx2(const hc_df_graph_t *g, const int32_t *stream,
                               int32_t words, const hc_df_lanes_t *lanes,
                               double *vscratch, const hc_df_cellctx_t *cc) {
#ifndef NDEBUG
    /* 좌표는 정수값 블록 좌표 계약 (BLENDED int 캐스트 동치의 전제) */
    for (int l = 0; l < 4; l++) {
        assert(lanes->x[l] == (double)(int32_t)lanes->x[l]);
        assert(lanes->y[l] == (double)(int32_t)lanes->y[l]);
        assert(lanes->z[l] == (double)(int32_t)lanes->z[l]);
    }
#endif
    x4_env_t e = {
        .g = g,
        .lanes = lanes,
        .vsc = vscratch,
        .cc = cc,
        .xv = _mm256_loadu_pd(lanes->x),
        .yv = _mm256_loadu_pd(lanes->y),
        .zv = _mm256_loadu_pd(lanes->z),
    };
    x4_run(&e, stream, words);
}

#else /* 비-x86: 디스패치가 SCALAR 를 돌려 도달 불가 — 링크 스텁 */

#include <stdlib.h>

void hc_df_eval_stream_x4_avx2(const hc_df_graph_t *g, const int32_t *stream,
                               int32_t words, const hc_df_lanes_t *lanes,
                               double *vscratch, const hc_df_cellctx_t *cc) {
    (void)g;
    (void)stream;
    (void)words;
    (void)lanes;
    (void)vscratch;
    (void)cc;
    abort();
}

#endif
