#include "hc_df_simd.h"
#include "hc_counters.h"

/* AVX-512 8-레인 콘 스트림 평가기 (P2-10, ADR-004 D2). 이 TU 만
 * -mavx512{f,dq,bw,vl} 로 컴파일된다 — 진입은 hc_isa_active()==AVX512
 * 확인 후에만 (df_isa.c 디스패치, 부재 호스트 SIGILL 방지). Zen4 기준선:
 * F+DQ+BW+VL 밖 확장 (VBMI/VP2INTERSECT 등) 금지.
 *
 * 패리티 불변 (ADR-004 D3/D4) — df_simd_avx2.c 와 동일 계약, 레인 8개:
 *  - 레인 = 독립 평가점. 레인별 연산 시퀀스는 스칼라 eval_node /
 *    hc_perlin_sample_scaled / hc_octaves_value / hc_blended_compute 와
 *    문자 그대로 동일 순서다. 수평 리덕션 없음.
 *  - FMA 인트린식 (_mm512_fmadd_* 계열) 사용 금지 — mul/add 분리만.
 *    check_no_fma.sh 가 산출물에서 재확인한다.
 *  - 나눗셈은 vdivpd 만 (IEEE 정확 반올림 == 스칼라 나눗셈 비트 동일).
 *    rcp/rsqrt 근사 명령 금지.
 *  - 선택은 opmask + vblendmpd/maskz — 비트 선택·상수 대입이라 값을
 *    만들지 않는다 (AVX2 의 cmp+blendv 와 결과 비트 동일; 마스킹된
 *    연산 레인은 스칼라가 그 분기에서 대입하는 상수와 동일한 +0.0/
 *    패스스루만 쓴다).
 *  - Mth.floor/lfloor 포화·NaN 시맨틱이 필요한 범위 밖 좌표 레인이
 *    하나라도 있으면 그 노드는 전 레인 스칼라 폴백 (26.2 오버월드 도달
 *    불가 — 데이터팩 방어). 범위 안에서 vrndscalepd(0x09) == floor() 는
 *    IEEE 정확 연산이라 비트 동일.
 *
 * AVX2 대비 구조 변경 (B-3 §5 표적):
 *  - gradient 룩업: GRAD 16방향을 컴포넌트별 zmm 2개 (총 6개) 에 상주
 *    시키고 vpermt2pd 1회로 픽 — AVX2 의 코너당 4로드+7셔플 붕괴.
 *  - zmm 32개: 인터프리터/커널의 라이브 레인지가 레지스터에 남는다
 *    (ymm16 스필 4.1G/런의 표적 — 코드 구조가 아니라 레지스터 파일).
 *  - 해시 체인은 레인별 스칼라 유지 (perm 테이블 워크 — 정수 조회라
 *    비트 정확 무비용; 벡터화 실험은 별도 커밋에서 실측 판정). */

#if defined(__x86_64__) || defined(__i386__)

#include <assert.h>
#include <immintrin.h>

typedef __m512d V8;

static inline V8 bc(double v) {
    return _mm512_set1_pd(v);
}

/* --- Java 시맨틱 프리미티브 (df_eval.c 의 스칼라 판과 1:1) --- */

/* java.lang.Math.min: NaN a → a; (+0,-0) → -0; else a<=b?a:b.
 * mask_blend(k, a, b) = k?b:a — 비트 선택 (반올림 없음). */
static inline V8 v_jmin(V8 a, V8 b) {
    __mmask8 le = _mm512_cmp_pd_mask(a, b, _CMP_LE_OQ);
    V8       r = _mm512_mask_blend_pd(le, b, a);
    V8       z = _mm512_setzero_pd();
    __mmask8 both0 = _mm512_cmp_pd_mask(a, z, _CMP_EQ_OQ) &
                     _mm512_cmp_pd_mask(b, z, _CMP_EQ_OQ);
    /* AVX2 판의 blendv 부호비트 트릭과 동형: both0 && signbit(b) → b */
    __mmask8 bneg = _mm512_movepi64_mask(_mm512_castpd_si512(b));
    r = _mm512_mask_blend_pd(both0 & bneg, r, b);
    __mmask8 nan_a = _mm512_cmp_pd_mask(a, a, _CMP_UNORD_Q);
    return _mm512_mask_blend_pd(nan_a, r, a);
}

/* java.lang.Math.max: NaN a → a; (-0,+0) → +0(b); else a>=b?a:b */
static inline V8 v_jmax(V8 a, V8 b) {
    __mmask8 ge = _mm512_cmp_pd_mask(a, b, _CMP_GE_OQ);
    V8       r = _mm512_mask_blend_pd(ge, b, a);
    V8       z = _mm512_setzero_pd();
    __mmask8 both0 = _mm512_cmp_pd_mask(a, z, _CMP_EQ_OQ) &
                     _mm512_cmp_pd_mask(b, z, _CMP_EQ_OQ);
    __mmask8 aneg = _mm512_movepi64_mask(_mm512_castpd_si512(a));
    r = _mm512_mask_blend_pd(both0 & aneg, r, b);
    __mmask8 nan_a = _mm512_cmp_pd_mask(a, a, _CMP_UNORD_Q);
    return _mm512_mask_blend_pd(nan_a, r, a);
}

/* Mth.clamp: v < lo ? lo : Math.min(v, hi) */
static inline V8 v_clamp(V8 v, V8 lo, V8 hi) {
    __mmask8 lt = _mm512_cmp_pd_mask(v, lo, _CMP_LT_OQ);
    return _mm512_mask_blend_pd(lt, v_jmin(v, hi), lo);
}

/* Mth.lerp: a + t*(b-a) — mul→add 분리 (FMA 금지) */
static inline V8 v_lerp(V8 t, V8 a, V8 b) {
    return _mm512_add_pd(a, _mm512_mul_pd(t, _mm512_sub_pd(b, a)));
}

/* Mth.clampedLerp(delta 첫 인자 판): delta<0→a, delta>1→b, NaN→lerp */
static inline V8 v_clamped_lerp(V8 t, V8 a, V8 b) {
    V8 r = v_lerp(t, a, b);
    r = _mm512_mask_blend_pd(_mm512_cmp_pd_mask(t, bc(0.0), _CMP_LT_OQ), r, a);
    r = _mm512_mask_blend_pd(_mm512_cmp_pd_mask(t, bc(1.0), _CMP_GT_OQ), r, b);
    return r;
}

/* Mth.smoothstep: ((t*t)*t) * ((t*(t*6-15)) + 10) */
static inline V8 v_smoothstep(V8 t) {
    V8 t3 = _mm512_mul_pd(_mm512_mul_pd(t, t), t);
    V8 inner = _mm512_add_pd(
        _mm512_mul_pd(t, _mm512_sub_pd(_mm512_mul_pd(t, bc(6.0)), bc(15.0))),
        bc(10.0));
    return _mm512_mul_pd(t3, inner);
}

/* |v| < bound 이고 비-NaN — Mth.floor/lfloor 무포화 보장 범위 검사 */
static inline int v_all_in(V8 v, double bound) {
    V8       abs = _mm512_abs_pd(v); /* 부호비트 클리어 — 비트 연산, 정확 */
    __mmask8 ok = _mm512_cmp_pd_mask(abs, bc(bound), _CMP_LT_OQ); /* NaN→0 */
    return ok == 0xFF;
}

/* floor: vrndscalepd imm 0x09 (toward -inf, no-exc) — 전 입력에서 정확
 * 연산이라 libm floor() 와 비트 동일 (P2-4 의 _mm256_floor_pd 대응) */
static inline V8 v_floor(V8 x) {
    return _mm512_roundscale_pd(x, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC);
}

/* --- PerlinNoise.wrap x8: d - lfloor(d/2^25 + 0.5)*2^25 --- */

static V8 wrap_x8(V8 d) {
    V8 q = _mm512_add_pd(_mm512_div_pd(d, bc(33554432.0)), bc(0.5));
    if (!v_all_in(q, 4611686018427387904.0 /* 2^62 */)) {
        double ds[8], out[8];
        _mm512_storeu_pd(ds, d);
        for (int l = 0; l < 8; l++)
            out[l] = hc_octaves_wrap(ds[l]);
        return _mm512_loadu_pd(out);
    }
    V8 fl = v_floor(q); /* 범위 안: (double)mth_lfloor 와 동일 */
    return _mm512_sub_pd(d, _mm512_mul_pd(fl, bc(33554432.0)));
}

/* --- ImprovedNoise x8 --- */

/* SimplexNoise.GRADIENT 를 컴포넌트별 SoA 로 (noise_perlin.c GRAD 와 동일
 * 값 — SoT 는 바닐라 <clinit>; 값 불일치는 df_x8 게이트의 비트 대조가
 * 즉시 잡는다). 각 컴포넌트 16 double = zmm 2개, vpermt2pd 의 4-비트
 * 인덱스 (hash & 15) 가 두 소스에 걸쳐 정확히 한 방향을 픽한다. */
static const double GRADX8[16] __attribute__((aligned(64))) = {
    1, -1, 1, -1, 1, -1, 1, -1, 0, 0, 0, 0, 1, 0, -1, 0,
};
static const double GRADY8[16] __attribute__((aligned(64))) = {
    1, 1, -1, -1, 0, 0, 0, 0, 1, -1, 1, -1, 1, -1, 1, -1,
};
static const double GRADZ8[16] __attribute__((aligned(64))) = {
    0, 0, 0, 0, 1, 1, -1, -1, 1, 1, -1, -1, 0, 1, 0, -1,
};

/* GRAD zmm 상주 세트 — perlin_x8 이 1회 로드해 8코너에 재사용 */
typedef struct {
    V8 xlo, xhi, ylo, yhi, zlo, zhi;
} grad_regs_t;

static inline grad_regs_t grad_load(void) {
    grad_regs_t g;
    g.xlo = _mm512_load_pd(GRADX8);
    g.xhi = _mm512_load_pd(GRADX8 + 8);
    g.ylo = _mm512_load_pd(GRADY8);
    g.yhi = _mm512_load_pd(GRADY8 + 8);
    g.zlo = _mm512_load_pd(GRADZ8);
    g.zhi = _mm512_load_pd(GRADZ8 + 8);
    return g;
}

/* 코너 하나: 레인별 gradient 인덱스 (정수 테이블 조회 — 스칼라, 비트
 * 정확 무비용) → vpermt2pd 컴포넌트 픽 → ((g0*x)+(g1*y))+(g2*z). */
static inline V8 corner_dot(const hc_perlin_t *p, const grad_regs_t *gr,
                            const int h[8], const int iz[8], int zoff, V8 xv,
                            V8 yv, V8 zv) {
    int64_t idx[8] __attribute__((aligned(64)));
    for (int l = 0; l < 8; l++)
        idx[l] = (int64_t)(p->perm[(h[l] + iz[l] + zoff) & 255] & 15);
    __m512i vi = _mm512_load_si512((const void *)idx);
    V8      gx = _mm512_permutex2var_pd(gr->xlo, vi, gr->xhi);
    V8      gy = _mm512_permutex2var_pd(gr->ylo, vi, gr->yhi);
    V8      gz = _mm512_permutex2var_pd(gr->zlo, vi, gr->zhi);
    /* SimplexNoise.dot 좌결합: ((g0*x) + (g1*y)) + (g2*z) */
    return _mm512_add_pd(
        _mm512_add_pd(_mm512_mul_pd(gx, xv), _mm512_mul_pd(gy, yv)),
        _mm512_mul_pd(gz, zv));
}

/* hc_perlin_sample_scaled x8. yscale 은 옥타브 스칼라 (전 레인 동일),
 * ymax 는 레인별 (BlendedNoise 의 gy*o). 범위 밖/NaN 레인 → 전 레인
 * 스칼라 폴백. */
static V8 perlin_x8(const hc_perlin_t *p, V8 x, V8 y, V8 z, double yscale,
                    V8 ymax) {
    HC_CTR_INC(HC_CTR_X8_PERLIN);
    V8 dx = _mm512_add_pd(x, bc(p->xo));
    V8 dy = _mm512_add_pd(y, bc(p->yo));
    V8 dz = _mm512_add_pd(z, bc(p->zo));
    const double B30 = 1073741824.0; /* 2^30 — mth_floor 안전 마진 */
    if (!(v_all_in(dx, B30) && v_all_in(dy, B30) && v_all_in(dz, B30)))
        goto scalar_fallback;

    {
        V8 fdx = v_floor(dx);
        V8 fdy = v_floor(dy);
        V8 fdz = v_floor(dz);
        V8 fx = _mm512_sub_pd(dx, fdx);
        V8 fy = _mm512_sub_pd(dy, fdy);
        V8 fz = _mm512_sub_pd(dz, fdz);

        V8 gy = fy;
        if (yscale != 0.0) {
            /* t = (ymax >= 0 && ymax < fy) ? ymax : fy;
             * yadj = (double)mth_floor(t/yscale + (double)1.0E-7f)*yscale */
            __mmask8 use_ymax =
                _mm512_cmp_pd_mask(ymax, _mm512_setzero_pd(), _CMP_GE_OQ) &
                _mm512_cmp_pd_mask(ymax, fy, _CMP_LT_OQ);
            V8 t = _mm512_mask_blend_pd(use_ymax, fy, ymax);
            V8 q = _mm512_add_pd(_mm512_div_pd(t, bc(yscale)),
                                 bc(0x1.AD7F2Ap-24));
            if (!v_all_in(q, B30))
                goto scalar_fallback;
            V8 yadj = _mm512_mul_pd(v_floor(q), bc(yscale));
            gy = _mm512_sub_pd(fy, yadj);
        }

        /* 해시 체인 (레인별 정수 — 스칼라 P() 와 동일) */
        double fdxs[8], fdys[8], fdzs[8];
        _mm512_storeu_pd(fdxs, fdx);
        _mm512_storeu_pd(fdys, fdy);
        _mm512_storeu_pd(fdzs, fdz);
        int iz[8], aa[8], ab[8], ba[8], bb[8];
        for (int l = 0; l < 8; l++) {
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

        V8 fx1 = _mm512_sub_pd(fx, bc(1.0));
        V8 gy1 = _mm512_sub_pd(gy, bc(1.0));
        V8 fz1 = _mm512_sub_pd(fz, bc(1.0));

        grad_regs_t gr = grad_load();
        V8 v000 = corner_dot(p, &gr, aa, iz, 0, fx, gy, fz);
        V8 v100 = corner_dot(p, &gr, ba, iz, 0, fx1, gy, fz);
        V8 v010 = corner_dot(p, &gr, ab, iz, 0, fx, gy1, fz);
        V8 v110 = corner_dot(p, &gr, bb, iz, 0, fx1, gy1, fz);
        V8 v001 = corner_dot(p, &gr, aa, iz, 1, fx, gy, fz1);
        V8 v101 = corner_dot(p, &gr, ba, iz, 1, fx1, gy, fz1);
        V8 v011 = corner_dot(p, &gr, ab, iz, 1, fx, gy1, fz1);
        V8 v111 = corner_dot(p, &gr, bb, iz, 1, fx1, gy1, fz1);

        V8 sx = v_smoothstep(fx);
        V8 sy = v_smoothstep(fy); /* fade 는 양자화 전 fy */
        V8 sz = v_smoothstep(fz);

        V8 l0 = v_lerp(sy, v_lerp(sx, v000, v100), v_lerp(sx, v010, v110));
        V8 l1 = v_lerp(sy, v_lerp(sx, v001, v101), v_lerp(sx, v011, v111));
        return v_lerp(sz, l0, l1);
    }

scalar_fallback:; /* 포화/NaN 시맨틱은 스칼라 경로가 소유 (희귀) */
    HC_CTR_INC(HC_CTR_X8_SCALAR_FB);
    {
        double xs[8], ys[8], zs[8], ym[8], out[8];
        _mm512_storeu_pd(xs, x);
        _mm512_storeu_pd(ys, y);
        _mm512_storeu_pd(zs, z);
        _mm512_storeu_pd(ym, ymax);
        for (int l = 0; l < 8; l++)
            out[l] = hc_perlin_sample_scaled(p, xs[l], ys[l], zs[l], yscale,
                                             ym[l]);
        return _mm512_loadu_pd(out);
    }
}

/* --- PerlinNoise.getValue x8 (hc_octaves_value 동형) --- */

static V8 octaves_x8(const hc_octaves_t *o, V8 x, V8 y, V8 z, double yscale,
                     V8 ymax) {
    V8     d = _mm512_setzero_pd();
    double e = o->lowest_freq_input;
    double f = o->lowest_freq_value;
    for (int32_t i = 0; i < o->count; i++) {
        const hc_perlin_t *p = o->octaves[i];
        if (p) {
            V8 g = perlin_x8(p, wrap_x8(_mm512_mul_pd(x, bc(e))),
                             wrap_x8(_mm512_mul_pd(y, bc(e))),
                             wrap_x8(_mm512_mul_pd(z, bc(e))), yscale * e,
                             _mm512_mul_pd(ymax, bc(e)));
            /* d += (amplitudes[i] * g) * f — 좌결합 그대로 */
            d = _mm512_add_pd(
                d, _mm512_mul_pd(_mm512_mul_pd(bc(o->amplitudes[i]), g),
                                 bc(f)));
        }
        e *= 2.0;
        f /= 2.0;
    }
    return d;
}

/* --- NormalNoise.getValue x8 --- */

static V8 normal_x8(const hc_normal_noise_t *n, V8 x, V8 y, V8 z) {
    V8 x2 = _mm512_mul_pd(x, bc(HC_NORMAL_INPUT_FACTOR));
    V8 y2 = _mm512_mul_pd(y, bc(HC_NORMAL_INPUT_FACTOR));
    V8 z2 = _mm512_mul_pd(z, bc(HC_NORMAL_INPUT_FACTOR));
    V8 zero = _mm512_setzero_pd();
    V8 a = octaves_x8(&n->first, x, y, z, 0.0, zero);
    V8 b = octaves_x8(&n->second, x2, y2, z2, 0.0, zero);
    return _mm512_mul_pd(_mm512_add_pd(a, b), bc(n->value_factor));
}

/* --- BlendedNoise.compute x8 (hc_blended_compute 동형) --- */

/* PerlinNoise.getOctaveNoise(i) = noiseLevels[count - 1 - i] */
static const hc_perlin_t *oct_at(const hc_octaves_t *o, int32_t i) {
    return o->octaves[o->count - 1 - i];
}

static V8 blended_x8(const hc_blended_noise_t *b, V8 xi, V8 yi, V8 zi) {
    V8     d = _mm512_mul_pd(xi, bc(b->xz_mult));
    V8     e = _mm512_mul_pd(yi, bc(b->y_mult));
    V8     f = _mm512_mul_pd(zi, bc(b->xz_mult));
    V8     gx = _mm512_div_pd(d, bc(b->xz_factor));
    V8     gy = _mm512_div_pd(e, bc(b->y_factor));
    V8     gz = _mm512_div_pd(f, bc(b->xz_factor));
    double smear_y = b->y_mult * b->smear;
    double smear_g = smear_y / b->y_factor;

    V8     n = _mm512_setzero_pd();
    double o = 1.0;
    for (int p = 0; p < 8; p++) {
        const hc_perlin_t *oct = oct_at(&b->main_noise, p);
        if (oct) {
            V8 s = perlin_x8(oct, wrap_x8(_mm512_mul_pd(gx, bc(o))),
                             wrap_x8(_mm512_mul_pd(gy, bc(o))),
                             wrap_x8(_mm512_mul_pd(gz, bc(o))), smear_g * o,
                             _mm512_mul_pd(gy, bc(o)));
            n = _mm512_add_pd(n, _mm512_div_pd(s, bc(o)));
        }
        o /= 2.0;
    }
    V8 q = _mm512_div_pd(
        _mm512_add_pd(_mm512_div_pd(n, bc(10.0)), bc(1.0)), bc(2.0));

    /* over = q >= 1.0 (min 측 생략), under = q <= 0.0 (max 측 생략).
     * 스칼라는 생략 측 누산을 0.0 으로 남긴다 — 레인 벡터에서는 일단
     * 계산 후 maskz 로 +0.0 강제 (계산된 죽은 값은 읽히지 않는다). */
    __mmask8 overm = _mm512_cmp_pd_mask(q, bc(1.0), _CMP_GE_OQ);
    __mmask8 underm = _mm512_cmp_pd_mask(q, bc(0.0), _CMP_LE_OQ);
    int      over_all = (overm == 0xFF);
    int      under_all = (underm == 0xFF);
    if (hc_ctr_on) { /* 혼합-레인 = 패리티가 강제하는 죽은-레인 뱅크 계산 */
        int mo = (int)overm, mu = (int)underm;
        if ((mo != 0 && mo != 0xFF) || (mu != 0 && mu != 0xFF))
            hc_ctr_tls[HC_CTR_X8_BLEND_MIX]++;
    }

    V8 lo = _mm512_setzero_pd();
    V8 hi = _mm512_setzero_pd();
    o = 1.0;
    for (int r = 0; r < 16; r++) {
        V8     s = wrap_x8(_mm512_mul_pd(d, bc(o)));
        V8     t = wrap_x8(_mm512_mul_pd(e, bc(o)));
        V8     u = wrap_x8(_mm512_mul_pd(f, bc(o)));
        double v = smear_y * o;
        if (!over_all) {
            const hc_perlin_t *oct = oct_at(&b->min_limit, r);
            if (oct)
                lo = _mm512_add_pd(
                    lo, _mm512_div_pd(perlin_x8(oct, s, t, u, v,
                                                _mm512_mul_pd(e, bc(o))),
                                      bc(o)));
        }
        if (!under_all) {
            const hc_perlin_t *oct = oct_at(&b->max_limit, r);
            if (oct)
                hi = _mm512_add_pd(
                    hi, _mm512_div_pd(perlin_x8(oct, s, t, u, v,
                                                _mm512_mul_pd(e, bc(o))),
                                      bc(o)));
        }
        o /= 2.0;
    }
    /* 생략 레인은 스칼라와 동일하게 +0.0 (maskz: ~over 만 통과) */
    lo = _mm512_maskz_mov_pd(_knot_mask8(overm), lo);
    hi = _mm512_maskz_mov_pd(_knot_mask8(underm), hi);

    V8 res = v_clamped_lerp(q, _mm512_div_pd(lo, bc(512.0)),
                            _mm512_div_pd(hi, bc(512.0)));
    return _mm512_div_pd(res, bc(128.0));
}

/* --- 스트림 인터프리터 --- */

typedef struct {
    const hc_df_graph_t   *g;
    const hc_df_lanes8_t  *lanes;
    double                *vsc; /* [g->n][8], 64B 정렬 */
    const hc_df_cellctx_t *cc;
    V8                     xv, yv, zv;
} x8_env_t;

static void x8_node(const x8_env_t *e, int32_t idx) {
    const hc_df_node_t *nd = &e->g->nodes[idx];
    double             *slot = e->vsc + 8 * (size_t)idx;
#define OPV(n) _mm512_load_pd(e->vsc + 8 * (size_t)(n))
    V8 r;
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
        r = _mm512_add_pd(OPV(nd->a), OPV(nd->b));
        break;
    case HC_DF_MUL: {
        /* Ap2 MUL 단락: arg1 == 0.0 (±0) 이면 +0.0 — maskz mul 이 정확히
         * 그 시맨틱 (마스크-오프 레인 = +0.0 대입, 연산 없음) */
        V8       a = OPV(nd->a);
        __mmask8 nz = _knot_mask8(
            _mm512_cmp_pd_mask(a, _mm512_setzero_pd(), _CMP_EQ_OQ));
        r = _mm512_maskz_mul_pd(nz, a, OPV(nd->b));
        break;
    }
    case HC_DF_MIN:
        r = v_jmin(OPV(nd->a), OPV(nd->b));
        break;
    case HC_DF_MAX:
        r = v_jmax(OPV(nd->a), OPV(nd->b));
        break;
    case HC_DF_ADD_CONST:
        r = _mm512_add_pd(OPV(nd->a), bc(nd->k0));
        break;
    case HC_DF_MUL_CONST:
        r = _mm512_mul_pd(OPV(nd->a), bc(nd->k0));
        break;

    case HC_DF_ABS:
        r = _mm512_andnot_pd(bc(-0.0), OPV(nd->a));
        break;
    case HC_DF_SQUARE: {
        V8 a = OPV(nd->a);
        r = _mm512_mul_pd(a, a);
        break;
    }
    case HC_DF_CUBE: {
        V8 a = OPV(nd->a);
        r = _mm512_mul_pd(_mm512_mul_pd(a, a), a);
        break;
    }
    case HC_DF_HALF_NEGATIVE: {
        /* pos ? a : a*0.5 — mask mul: 마스크-오프(pos) 레인은 src(a) */
        V8       a = OPV(nd->a);
        __mmask8 pos =
            _mm512_cmp_pd_mask(a, _mm512_setzero_pd(), _CMP_GT_OQ);
        r = _mm512_mask_mul_pd(a, _knot_mask8(pos), a, bc(0.5));
        break;
    }
    case HC_DF_QUARTER_NEGATIVE: {
        V8       a = OPV(nd->a);
        __mmask8 pos =
            _mm512_cmp_pd_mask(a, _mm512_setzero_pd(), _CMP_GT_OQ);
        r = _mm512_mask_mul_pd(a, _knot_mask8(pos), a, bc(0.25));
        break;
    }
    case HC_DF_SQUEEZE: {
        /* d/2 - ((d*d)*d)/24 */
        V8 d = v_clamp(OPV(nd->a), bc(-1.0), bc(1.0));
        r = _mm512_sub_pd(
            _mm512_div_pd(d, bc(2.0)),
            _mm512_div_pd(_mm512_mul_pd(_mm512_mul_pd(d, d), d), bc(24.0)));
        break;
    }
    case HC_DF_INVERT:
        r = _mm512_div_pd(bc(1.0), OPV(nd->a));
        break;

    case HC_DF_CLAMP:
        r = v_clamp(OPV(nd->a), bc(nd->k0), bc(nd->k1));
        break;

    case HC_DF_Y_CLAMPED_GRADIENT: {
        /* t = (y-k0)/(k1-k0); t<0→k2, t>1→k3, else k2+t*(k3-k2) */
        V8 t = _mm512_div_pd(_mm512_sub_pd(e->yv, bc(nd->k0)),
                             bc(nd->k1 - nd->k0));
        r = v_clamped_lerp(t, bc(nd->k2), bc(nd->k3));
        break;
    }

    case HC_DF_RANGE_CHOICE: {
        V8       d = OPV(nd->a);
        __mmask8 in = _mm512_cmp_pd_mask(d, bc(nd->k0), _CMP_GE_OQ) &
                      _mm512_cmp_pd_mask(d, bc(nd->k1), _CMP_LT_OQ);
        r = _mm512_mask_blend_pd(in, OPV(nd->c), OPV(nd->b));
        break;
    }

    case HC_DF_NOISE: {
        V8 nx = _mm512_mul_pd(e->xv, bc(nd->k0));
        V8 ny = _mm512_mul_pd(e->yv, bc(nd->k1));
        V8 nz = _mm512_mul_pd(e->zv, bc(nd->k0));
        r = normal_x8(&e->g->noises[nd->aux], nx, ny, nz);
        break;
    }
    case HC_DF_BLENDED_NOISE:
        /* 스칼라는 (int32_t) 캐스트 후 승격 — 레인 좌표는 정수값 전제
         * (블록 좌표 계약, hc_df_simd.h) 라 값이 같다 */
        r = blended_x8(&e->g->blended[nd->aux], e->xv, e->yv, e->zv);
        break;

    case HC_DF_INTERVAL_SELECT: {
        /* 선택·회수는 레인별 스칼라 (정수/비교 — 반올림 없음) */
        const double *av = e->vsc + 8 * (size_t)nd->a;
        int32_t       nf = e->g->ipool[nd->aux];
        double        out[8];
        for (int l = 0; l < 8; l++) {
            double  dv = av[l];
            int32_t sel = nf - 1;
            for (int32_t j = 0; j < nf - 1; j++)
                if (dv < e->g->dpool[nd->aux2 + j]) {
                    sel = j;
                    break;
                }
            out[l] = e->vsc[8 * (size_t)e->g->ipool[nd->aux + 1 + sel] + l];
        }
        r = _mm512_loadu_pd(out);
        break;
    }

    case HC_DF_BLEND_OFFSET:
        r = _mm512_setzero_pd();
        break;
    case HC_DF_BLEND_ALPHA:
        r = bc(1.0);
        break;

    case HC_DF_INTERPOLATED:
        if (e->cc && e->cc->interp_of[idx] >= 0) {
            const hc_df_interp_t *it = &e->cc->interp[e->cc->interp_of[idx]];
            if (e->cc->mode == HC_DF_MODE_CELL) {
                /* interp_lerp3 동형 — 델타는 레인별 (사전 나눗셈 값) */
                V8 dxv = _mm512_loadu_pd(e->lanes->dx);
                V8 dyv = _mm512_loadu_pd(e->lanes->dy);
                V8 dzv = _mm512_loadu_pd(e->lanes->dz);
                V8 a0 = v_lerp(dyv, v_lerp(dxv, bc(it->n000), bc(it->n100)),
                               v_lerp(dxv, bc(it->n010), bc(it->n110)));
                V8 a1 = v_lerp(dyv, v_lerp(dxv, bc(it->n001), bc(it->n101)),
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
            const double       *av = e->vsc + 8 * (size_t)nd->a;
            double              out[8];
            for (int l = 0; l < 8; l++) {
                int32_t qx = (int32_t)e->lanes->x[l] >> 2;
                int32_t qz = (int32_t)e->lanes->z[l] >> 2;
                int32_t i = qx - e->cc->first_noise_x;
                int32_t j = qz - e->cc->first_noise_z;
                if (i >= 0 && j >= 0 && i < size && j < size) {
                    out[l] = fl->values[i + j * size];
                } else {
                    /* 창-밖 폴백 — x8 호출 문맥 (fill_slice/cell) 은 전부
                     * 창 안 보장이라 도달 자체가 콘 계산 버그다 */
                    assert(!"flat_cache out-of-window in x8 context");
                    out[l] = av[l];
                }
            }
            r = _mm512_loadu_pd(out);
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
        /* hc_df_stream_x4_ok 화이트리스트가 사전 차단한다 (x4/x8 공용) */
        assert(!"unsupported op in x8 stream");
        r = _mm512_setzero_pd();
        break;
    }
    _mm512_store_pd(slot, r);
#undef OPV
}

/* prog_run (df_eval.c) 과 동일한 스트림 디코딩 — 세그먼트는 '필요한
 * 레인이 하나라도 있으면' 8레인으로 실행한다. 죽은 레인의 값은 순수
 * 계산이고 blend/회수가 선택하지 않으므로 관측 불가. */
static void x8_run(const x8_env_t *e, const int32_t *p, int32_t words) {
    const int32_t *end = p + words;
    while (p < end) {
        int32_t v = *p++;
        if (v >= 0) {
            HC_CTR_INC_HOT(HC_CTR_X8_NODE);
            x8_node(e, v);
            continue;
        }
        if (v == -1) { /* PROG_RC */
            int32_t             ch = p[0], wt = p[1], we = p[2];
            const hc_df_node_t *nd = &e->g->nodes[ch];
            V8       d = _mm512_load_pd(e->vsc + 8 * (size_t)nd->a);
            __mmask8 in = _mm512_cmp_pd_mask(d, bc(nd->k0), _CMP_GE_OQ) &
                          _mm512_cmp_pd_mask(d, bc(nd->k1), _CMP_LT_OQ);
            int m = (int)in;
            if (m != 0 && m != 0xFF)
                HC_CTR_INC(HC_CTR_X8_RC_MIX);
            if (m != 0)
                x8_run(e, p + 3, wt);
            if (m != 0xFF)
                x8_run(e, p + 3 + wt, we);
            _mm512_store_pd(
                e->vsc + 8 * (size_t)ch,
                _mm512_mask_blend_pd(
                    in, _mm512_load_pd(e->vsc + 8 * (size_t)nd->c),
                    _mm512_load_pd(e->vsc + 8 * (size_t)nd->b)));
            p += 3 + wt + we;
        } else { /* PROG_IS */
            int32_t             ch = p[0], nf = p[1];
            const int32_t      *w = p + 2;
            const hc_df_node_t *nd = &e->g->nodes[ch];
            const double       *av = e->vsc + 8 * (size_t)nd->a;
            int32_t             sel[8];
            for (int l = 0; l < 8; l++) {
                double  dv = av[l];
                int32_t s = nf - 1;
                for (int32_t j = 0; j < nf - 1; j++)
                    if (dv < e->g->dpool[nd->aux2 + j]) {
                        s = j;
                        break;
                    }
                sel[l] = s;
            }
            int mix = 0;
            for (int l = 1; l < 8; l++)
                mix |= (sel[l] != sel[0]);
            if (mix)
                HC_CTR_INC(HC_CTR_X8_IS_MIX);
            const int32_t *q = p + 2 + nf;
            for (int32_t k = 0; k < nf; k++) {
                int need = 0;
                for (int l = 0; l < 8; l++)
                    need |= (sel[l] == k);
                if (need && w[k] > 0)
                    x8_run(e, q, w[k]);
                q += w[k];
            }
            double out[8];
            for (int l = 0; l < 8; l++)
                out[l] =
                    e->vsc[8 * (size_t)e->g->ipool[nd->aux + 1 + sel[l]] + l];
            _mm512_storeu_pd(e->vsc + 8 * (size_t)ch, _mm512_loadu_pd(out));
            p = q;
        }
    }
}

void hc_df_eval_stream_x8_avx512(const hc_df_graph_t *g, const int32_t *stream,
                                 int32_t words, const hc_df_lanes8_t *lanes,
                                 double *vscratch, const hc_df_cellctx_t *cc) {
#ifndef NDEBUG
    /* 좌표는 정수값 블록 좌표 계약 (BLENDED int 캐스트 동치의 전제) */
    for (int l = 0; l < 8; l++) {
        assert(lanes->x[l] == (double)(int32_t)lanes->x[l]);
        assert(lanes->y[l] == (double)(int32_t)lanes->y[l]);
        assert(lanes->z[l] == (double)(int32_t)lanes->z[l]);
    }
#endif
    x8_env_t e = {
        .g = g,
        .lanes = lanes,
        .vsc = vscratch,
        .cc = cc,
        .xv = _mm512_loadu_pd(lanes->x),
        .yv = _mm512_loadu_pd(lanes->y),
        .zv = _mm512_loadu_pd(lanes->z),
    };
    x8_run(&e, stream, words);
}

#else /* 비-x86: 디스패치가 SCALAR 를 돌려 도달 불가 — 링크 스텁 */

#include <stdlib.h>

void hc_df_eval_stream_x8_avx512(const hc_df_graph_t *g, const int32_t *stream,
                                 int32_t words, const hc_df_lanes8_t *lanes,
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
