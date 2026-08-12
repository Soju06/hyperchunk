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
 *  - 해시 체인 벡터화 (08d4f07): perm[256] 워드 확장 zmm 8개 상주 +
 *    vpermt2w 4-way 선택 — 스칼라 P() 체인과 인덱스 동치 (정수 산술,
 *    반올림 부재).
 *  - 2-웨이 콘-스트림 인터리브 (P2-11): 독립 8-레인 스트림 2개를 단일
 *    스레드에서 문장 단위로 교차 실행 — 의존-체인 지연 상호 은닉 (B-4
 *    §4.4 실측 근거). 아래 x8x2 섹션의 패리티 논증 참조. */

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

/* 코너 하나: gradient 인덱스 qword 벡터 → vpermt2pd 컴포넌트 픽 →
 * ((g0*x)+(g1*y))+(g2*z). 인덱스는 perm 룩업 결과 &15 (아래 해시
 * 벡터화가 산출) — vpermt2pd 는 하위 4비트만 쓴다 (16 double 2소스). */
static inline V8 corner_dot(const grad_regs_t *gr, __m512i vi, V8 xv, V8 yv,
                            V8 zv) {
    V8 gx = _mm512_permutex2var_pd(gr->xlo, vi, gr->xhi);
    V8 gy = _mm512_permutex2var_pd(gr->ylo, vi, gr->yhi);
    V8 gz = _mm512_permutex2var_pd(gr->zlo, vi, gr->zhi);
    /* SimplexNoise.dot 좌결합: ((g0*x) + (g1*y)) + (g2*z) */
    return _mm512_add_pd(
        _mm512_add_pd(_mm512_mul_pd(gx, xv), _mm512_mul_pd(gy, yv)),
        _mm512_mul_pd(gz, zv));
}

/* --- perm 해시 체인 벡터화 (B-3 §5 표적 ②: 스칼라 해시 4.15G) ---
 *
 * perm[256] (uint8) 을 16-비트 워드로 넓혀 zmm 8개에 상주시키고 256-
 * 엔트리 조회를 vpermt2w 4회 (64-엔트리 2-소스 permute) + 인덱스 상위
 * 2비트 4-way 선택으로 푼다 — 전부 BW 명령 (Zen4 기준선 안). 룩업·
 * 덧셈·마스킹 전부 정수 연산이라 반올림이 존재하지 않고, 인덱스 산술은
 * 스칼라 P() 와 두 보수 동치다:
 *   (A + iy) & 255 == (A + (iy & 255)) & 255   (mod-256 저비트 보존)
 * — iy/iz 를 사전 마스킹하면 16-비트 워드 덧셈이 오버플로 없이
 * (최대 255+255+1 = 511) 스칼라 int 덧셈과 같은 저 8비트를 만든다. */

typedef struct {
    __m512i w[8]; /* w[2k], w[2k+1] = perm[64k .. 64k+63] 워드 */
} perm_regs_t;

static inline perm_regs_t perm_load(const hc_perlin_t *p) {
    perm_regs_t t;
    for (int k = 0; k < 8; k++)
        t.w[k] = _mm512_cvtepu8_epi16(
            _mm256_loadu_si256((const __m256i *)(p->perm + 32 * k)));
    return t;
}

/* 32 워드 인덱스 (각 0..255) → perm[idx] 32개 (워드) */
static inline __m512i perm_lookup32(const perm_regs_t *t, __m512i idx) {
    /* vpermt2w 는 워드 인덱스 하위 6비트만 본다 — 상위 2비트는 선택으로 */
    __m512i   c0 = _mm512_permutex2var_epi16(t->w[0], idx, t->w[1]);
    __m512i   c1 = _mm512_permutex2var_epi16(t->w[2], idx, t->w[3]);
    __m512i   c2 = _mm512_permutex2var_epi16(t->w[4], idx, t->w[5]);
    __m512i   c3 = _mm512_permutex2var_epi16(t->w[6], idx, t->w[7]);
    __m512i   hi = _mm512_srli_epi16(idx, 6);
    __mmask32 m1 = _mm512_cmpeq_epi16_mask(hi, _mm512_set1_epi16(1));
    __mmask32 m2 = _mm512_cmpeq_epi16_mask(hi, _mm512_set1_epi16(2));
    __mmask32 m3 = _mm512_cmpeq_epi16_mask(hi, _mm512_set1_epi16(3));
    __m512i   r = _mm512_mask_blend_epi16(m1, c0, c1);
    r = _mm512_mask_blend_epi16(m2, r, c2);
    return _mm512_mask_blend_epi16(m3, r, c3);
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

        /* 해시 체인 벡터화 (B-3 표적 ② — perm_lookup32, 산술 동치는
         * perm_regs_t 주석 참조). 3단 조회를 워드-벡터 배치로:
         * L1 16조회 (A,B) → L2 32조회 (aa,ab,ba,bb) → 코너 2×32조회.
         * 전부 정수 — 스칼라 P() 체인과 인덱스가 같으므로 값이 같다. */
        perm_regs_t pr = perm_load(p);
        __m256i     m255d = _mm256_set1_epi32(255);
        __m128i     one16 = _mm_set1_epi16(1);
        __m512i     m255w = _mm512_set1_epi16(255);

        /* 정수 좌표: 정수값 double 이라 vcvtpd2dq 정확 (스칼라 (int)
         * 캐스트 동치, |좌표|<2^30 가드 위) — 저 8비트 사전 마스킹 */
        __m128i ixw = _mm256_cvtepi32_epi16(
            _mm256_and_si256(_mm512_cvtpd_epi32(fdx), m255d));
        __m128i iyw = _mm256_cvtepi32_epi16(
            _mm256_and_si256(_mm512_cvtpd_epi32(fdy), m255d));
        __m128i izw = _mm256_cvtepi32_epi16(
            _mm256_and_si256(_mm512_cvtpd_epi32(fdz), m255d));

        /* L1: 워드 0-7 = perm[ix&255] = A, 8-15 = perm[(ix+1)&255] = B
         * (상위 16워드는 미정의 인덱스 — 결과 미사용) */
        __m256i ixp = _mm256_inserti128_si256(
            _mm256_castsi128_si256(ixw), _mm_add_epi16(ixw, one16), 1);
        __m512i AB = perm_lookup32(
            &pr, _mm512_and_si512(_mm512_castsi256_si512(ixp), m255w));

        /* L2: 그룹 [A,A,B,B] + [iy,iy+1,iy,iy+1] → [aa,ab,ba,bb] */
        __m512i h4 = _mm512_shuffle_i32x4(AB, AB, _MM_SHUFFLE(1, 1, 0, 0));
        __m256i iyp = _mm256_inserti128_si256(
            _mm256_castsi128_si256(iyw), _mm_add_epi16(iyw, one16), 1);
        __m512i iy4 = _mm512_shuffle_i32x4(_mm512_castsi256_si512(iyp),
                                           _mm512_castsi256_si512(iyp),
                                           _MM_SHUFFLE(1, 0, 1, 0));
        __m512i AABB = perm_lookup32(
            &pr, _mm512_and_si512(_mm512_add_epi16(h4, iy4), m255w));

        /* 코너: 그룹 재배열 (aa,ba,ab,bb) = (v000,v100,v010,v110) 순서,
         * +iz 뱅크 (z0) 와 +iz+1 뱅크 (z1) 각 1배치. 워드 합은 최대
         * 255+255+1 = 511 — 오버플로 없음. */
        __m512i hcn = _mm512_shuffle_i32x4(AABB, AABB, _MM_SHUFFLE(3, 1, 2, 0));
        __m512i sum0 = _mm512_add_epi16(hcn, _mm512_broadcast_i32x4(izw));
        __m512i g15 = _mm512_set1_epi16(15);
        __m512i gh0 = _mm512_and_si512(
            perm_lookup32(&pr, _mm512_and_si512(sum0, m255w)), g15);
        __m512i gh1 = _mm512_and_si512(
            perm_lookup32(&pr, _mm512_and_si512(
                                   _mm512_add_epi16(sum0,
                                                    _mm512_set1_epi16(1)),
                                   m255w)),
            g15);

        V8 fx1 = _mm512_sub_pd(fx, bc(1.0));
        V8 gy1 = _mm512_sub_pd(gy, bc(1.0));
        V8 fz1 = _mm512_sub_pd(fz, bc(1.0));

        grad_regs_t gr = grad_load();
#define GH(v, g)                                                             \
    _mm512_cvtepu16_epi64((g) == 0 ? _mm512_castsi512_si128(v)               \
                                   : _mm512_extracti32x4_epi32((v), (g)))
        V8 v000 = corner_dot(&gr, GH(gh0, 0), fx, gy, fz);
        V8 v100 = corner_dot(&gr, GH(gh0, 1), fx1, gy, fz);
        V8 v010 = corner_dot(&gr, GH(gh0, 2), fx, gy1, fz);
        V8 v110 = corner_dot(&gr, GH(gh0, 3), fx1, gy1, fz);
        V8 v001 = corner_dot(&gr, GH(gh1, 0), fx, gy, fz1);
        V8 v101 = corner_dot(&gr, GH(gh1, 1), fx1, gy, fz1);
        V8 v011 = corner_dot(&gr, GH(gh1, 2), fx, gy1, fz1);
        V8 v111 = corner_dot(&gr, GH(gh1, 3), fx1, gy1, fz1);
#undef GH

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

/* ==== P2-11: 2-웨이 콘-스트림 인터리브 ====================================
 *
 * B-4 §4.4 실측: 이 커널은 FP-포트가 아니라 의존-체인에 결박돼 있다
 * (mul-포트 가동률 9.0% / add-포트 9.7%, SMT 동일코어 co-run 합산 1.52x).
 * 체인 하나의 명령 수 (~perlin 428 insn) 가 OoO 윈도우와 같은 자릿수라
 * 하드웨어가 인접 호출을 겹치지 못한다 — 독립 콘-스트림 2개 (8+8 레인 =
 * 독립 평가점 16개) 를 단일 스레드에서 **문장 단위로** 교차 실행해 두
 * 체인의 지연을 상호 은닉한다.
 *
 * 패리티 논증 (절대 조건 — 스트림 내 재결합·순서 변경 금지):
 *  - 각 dual 함수는 위 단일-스트림 함수의 문장을 A/B 접미사로 복제한
 *    것이다: 스트림별 피연산자·결합 순서·선택 시맨틱이 원본과 문자
 *    그대로 같고, 두 스트림은 값을 교환하지 않는다 (수평 리덕션 없음).
 *    독립 계산의 교차는 기계 스케줄링일 뿐이라 IEEE-754 결과는 각 스트림
 *    단독 실행과 비트 동일하다.
 *  - 공유는 읽기 전용뿐: 그래프/스트림 워드, perm/GRAD 상주 레지스터
 *    (perm_load/grad_load 1회 공유 — 로드 횟수는 값을 만들지 않는다),
 *    옥타브 스칼라 (e/f/o/yscale — 원본에서도 전 레인 공통이던 값).
 *  - 제어 흐름이 스트림별로 갈리는 지점 (RC/IS 레그 필요 집합, blended
 *    over/under 편측, 포화 가드) 은 그 지점의 해당 스트림만 기존 단일-
 *    스트림 함수로 위임한다 — 위임 경로는 원본 코드 그 자체다
 *    (HC_CTR_X8_SPLIT 계수, 26.2 지형에서 희귀 예상).
 *  - 카운터는 스트림당 1회 증가 — 단일 경로 2회 실행과 총계 동일.
 *    perlin dual-커밋은 HC_CTR_X8_PERLIN2 로 별도 계수 (인터리브
 *    커버리지 = 2·perlin2 / perlin 진단용).
 *
 * 레지스터 압력 (zmm 32): 해시 단계는 perm 8 공유 + 스트림당 ~9 라이브,
 * 코너 단계는 GRAD 6 공유 + 스트림당 ~14 라이브 — 코너 꼬리에서 소폭
 * 스필이 나올 수 있으나 (측정 대상) 포트 유휴 90% 무대라 스토어/로드
 * 대역은 병목이 아니다. P2-10 스필 회수 (-54~87%) 의 재유입 여부는
 * 정적 스택-mov 계수로 판정한다. */

/* PerlinNoise.wrap x8 ×2 — 가드 실패 스트림이 하나라도 있으면 both 위임
 * (위임 경로 = 원본 wrap_x8, 값 재계산은 순수라 비트 동일).
 * always_inline: 슬로패스 포함 사이즈 때문에 GCC 가 아웃라인하면 포인터-
 * 출력 스토어/리로드 왕복이 wrap→perlin 의존-체인에 얹힌다 (단일 경로의
 * wrap_x8 인라인과 비대칭) — 패스트패스는 스트림당 벡터 op ~7개다. */
__attribute__((always_inline)) static inline void wrap_x8_dual(V8 dA, V8 dB,
                                                               V8 *oA,
                                                               V8 *oB) {
    V8 qA = _mm512_add_pd(_mm512_div_pd(dA, bc(33554432.0)), bc(0.5));
    V8 qB = _mm512_add_pd(_mm512_div_pd(dB, bc(33554432.0)), bc(0.5));
    if (!v_all_in(qA, 4611686018427387904.0 /* 2^62 */) ||
        !v_all_in(qB, 4611686018427387904.0)) {
        HC_CTR_INC(HC_CTR_X8_SPLIT);
        *oA = wrap_x8(dA);
        *oB = wrap_x8(dB);
        return;
    }
    V8 flA = v_floor(qA);
    V8 flB = v_floor(qB);
    *oA = _mm512_sub_pd(dA, _mm512_mul_pd(flA, bc(33554432.0)));
    *oB = _mm512_sub_pd(dB, _mm512_mul_pd(flB, bc(33554432.0)));
}

/* hc_perlin_sample_scaled x8 ×2 — perlin_x8 의 문장 A/B 복제.
 * 포화 가드 (전 스트림 통과 전) 실패 시 스트림별 단일 경로 위임 —
 * 카운터 (X8_PERLIN/X8_SCALAR_FB) 는 위임 함수가 원본대로 증가시키므로
 * 커밋 시점 (전 가드 통과 후) 에만 여기서 증가시킨다 (총계 보존). */
static void perlin_x8_dual(const hc_perlin_t *p, V8 xA, V8 yA, V8 zA, V8 xB,
                           V8 yB, V8 zB, double yscale, V8 ymaxA, V8 ymaxB,
                           V8 *rA, V8 *rB) {
    V8 dxA = _mm512_add_pd(xA, bc(p->xo));
    V8 dxB = _mm512_add_pd(xB, bc(p->xo));
    V8 dyA = _mm512_add_pd(yA, bc(p->yo));
    V8 dyB = _mm512_add_pd(yB, bc(p->yo));
    V8 dzA = _mm512_add_pd(zA, bc(p->zo));
    V8 dzB = _mm512_add_pd(zB, bc(p->zo));
    const double B30 = 1073741824.0; /* 2^30 — mth_floor 안전 마진 */
    if (!(v_all_in(dxA, B30) && v_all_in(dyA, B30) && v_all_in(dzA, B30) &&
          v_all_in(dxB, B30) && v_all_in(dyB, B30) && v_all_in(dzB, B30)))
        goto split;

    {
        V8 fdxA = v_floor(dxA);
        V8 fdxB = v_floor(dxB);
        V8 fdyA = v_floor(dyA);
        V8 fdyB = v_floor(dyB);
        V8 fdzA = v_floor(dzA);
        V8 fdzB = v_floor(dzB);
        V8 fxA = _mm512_sub_pd(dxA, fdxA);
        V8 fxB = _mm512_sub_pd(dxB, fdxB);
        V8 fyA = _mm512_sub_pd(dyA, fdyA);
        V8 fyB = _mm512_sub_pd(dyB, fdyB);
        V8 fzA = _mm512_sub_pd(dzA, fdzA);
        V8 fzB = _mm512_sub_pd(dzB, fdzB);

        V8 gyA = fyA;
        V8 gyB = fyB;
        if (yscale != 0.0) {
            __mmask8 umA =
                _mm512_cmp_pd_mask(ymaxA, _mm512_setzero_pd(), _CMP_GE_OQ) &
                _mm512_cmp_pd_mask(ymaxA, fyA, _CMP_LT_OQ);
            __mmask8 umB =
                _mm512_cmp_pd_mask(ymaxB, _mm512_setzero_pd(), _CMP_GE_OQ) &
                _mm512_cmp_pd_mask(ymaxB, fyB, _CMP_LT_OQ);
            V8 tA = _mm512_mask_blend_pd(umA, fyA, ymaxA);
            V8 tB = _mm512_mask_blend_pd(umB, fyB, ymaxB);
            V8 qA = _mm512_add_pd(_mm512_div_pd(tA, bc(yscale)),
                                  bc(0x1.AD7F2Ap-24));
            V8 qB = _mm512_add_pd(_mm512_div_pd(tB, bc(yscale)),
                                  bc(0x1.AD7F2Ap-24));
            if (!v_all_in(qA, B30) || !v_all_in(qB, B30))
                goto split;
            V8 yadjA = _mm512_mul_pd(v_floor(qA), bc(yscale));
            V8 yadjB = _mm512_mul_pd(v_floor(qB), bc(yscale));
            gyA = _mm512_sub_pd(fyA, yadjA);
            gyB = _mm512_sub_pd(fyB, yadjB);
        }

        /* 전 가드 통과 — dual 커밋 */
        HC_CTR_INC(HC_CTR_X8_PERLIN);
        HC_CTR_INC(HC_CTR_X8_PERLIN);
        HC_CTR_INC(HC_CTR_X8_PERLIN2);

        perm_regs_t pr = perm_load(p); /* 읽기 전용 — 두 스트림 공유 */
        __m256i     m255d = _mm256_set1_epi32(255);
        __m128i     one16 = _mm_set1_epi16(1);
        __m512i     m255w = _mm512_set1_epi16(255);

        __m128i ixwA = _mm256_cvtepi32_epi16(
            _mm256_and_si256(_mm512_cvtpd_epi32(fdxA), m255d));
        __m128i ixwB = _mm256_cvtepi32_epi16(
            _mm256_and_si256(_mm512_cvtpd_epi32(fdxB), m255d));
        __m128i iywA = _mm256_cvtepi32_epi16(
            _mm256_and_si256(_mm512_cvtpd_epi32(fdyA), m255d));
        __m128i iywB = _mm256_cvtepi32_epi16(
            _mm256_and_si256(_mm512_cvtpd_epi32(fdyB), m255d));
        __m128i izwA = _mm256_cvtepi32_epi16(
            _mm256_and_si256(_mm512_cvtpd_epi32(fdzA), m255d));
        __m128i izwB = _mm256_cvtepi32_epi16(
            _mm256_and_si256(_mm512_cvtpd_epi32(fdzB), m255d));

        __m256i ixpA = _mm256_inserti128_si256(
            _mm256_castsi128_si256(ixwA), _mm_add_epi16(ixwA, one16), 1);
        __m256i ixpB = _mm256_inserti128_si256(
            _mm256_castsi128_si256(ixwB), _mm_add_epi16(ixwB, one16), 1);
        __m512i ABA = perm_lookup32(
            &pr, _mm512_and_si512(_mm512_castsi256_si512(ixpA), m255w));
        __m512i ABB = perm_lookup32(
            &pr, _mm512_and_si512(_mm512_castsi256_si512(ixpB), m255w));

        __m512i h4A = _mm512_shuffle_i32x4(ABA, ABA, _MM_SHUFFLE(1, 1, 0, 0));
        __m512i h4B = _mm512_shuffle_i32x4(ABB, ABB, _MM_SHUFFLE(1, 1, 0, 0));
        __m256i iypA = _mm256_inserti128_si256(
            _mm256_castsi128_si256(iywA), _mm_add_epi16(iywA, one16), 1);
        __m256i iypB = _mm256_inserti128_si256(
            _mm256_castsi128_si256(iywB), _mm_add_epi16(iywB, one16), 1);
        __m512i iy4A = _mm512_shuffle_i32x4(_mm512_castsi256_si512(iypA),
                                            _mm512_castsi256_si512(iypA),
                                            _MM_SHUFFLE(1, 0, 1, 0));
        __m512i iy4B = _mm512_shuffle_i32x4(_mm512_castsi256_si512(iypB),
                                            _mm512_castsi256_si512(iypB),
                                            _MM_SHUFFLE(1, 0, 1, 0));
        __m512i AABBA = perm_lookup32(
            &pr, _mm512_and_si512(_mm512_add_epi16(h4A, iy4A), m255w));
        __m512i AABBB = perm_lookup32(
            &pr, _mm512_and_si512(_mm512_add_epi16(h4B, iy4B), m255w));

        __m512i hcnA =
            _mm512_shuffle_i32x4(AABBA, AABBA, _MM_SHUFFLE(3, 1, 2, 0));
        __m512i hcnB =
            _mm512_shuffle_i32x4(AABBB, AABBB, _MM_SHUFFLE(3, 1, 2, 0));
        __m512i sum0A = _mm512_add_epi16(hcnA, _mm512_broadcast_i32x4(izwA));
        __m512i sum0B = _mm512_add_epi16(hcnB, _mm512_broadcast_i32x4(izwB));
        __m512i g15 = _mm512_set1_epi16(15);
        __m512i gh0A = _mm512_and_si512(
            perm_lookup32(&pr, _mm512_and_si512(sum0A, m255w)), g15);
        __m512i gh0B = _mm512_and_si512(
            perm_lookup32(&pr, _mm512_and_si512(sum0B, m255w)), g15);
        __m512i gh1A = _mm512_and_si512(
            perm_lookup32(&pr, _mm512_and_si512(
                                   _mm512_add_epi16(sum0A,
                                                    _mm512_set1_epi16(1)),
                                   m255w)),
            g15);
        __m512i gh1B = _mm512_and_si512(
            perm_lookup32(&pr, _mm512_and_si512(
                                   _mm512_add_epi16(sum0B,
                                                    _mm512_set1_epi16(1)),
                                   m255w)),
            g15);

        V8 fx1A = _mm512_sub_pd(fxA, bc(1.0));
        V8 fx1B = _mm512_sub_pd(fxB, bc(1.0));
        V8 gy1A = _mm512_sub_pd(gyA, bc(1.0));
        V8 gy1B = _mm512_sub_pd(gyB, bc(1.0));
        V8 fz1A = _mm512_sub_pd(fzA, bc(1.0));
        V8 fz1B = _mm512_sub_pd(fzB, bc(1.0));

        grad_regs_t gr = grad_load(); /* 읽기 전용 — 두 스트림 공유 */
#define GH(v, g)                                                             \
    _mm512_cvtepu16_epi64((g) == 0 ? _mm512_castsi512_si128(v)               \
                                   : _mm512_extracti32x4_epi32((v), (g)))
        V8 v000A = corner_dot(&gr, GH(gh0A, 0), fxA, gyA, fzA);
        V8 v000B = corner_dot(&gr, GH(gh0B, 0), fxB, gyB, fzB);
        V8 v100A = corner_dot(&gr, GH(gh0A, 1), fx1A, gyA, fzA);
        V8 v100B = corner_dot(&gr, GH(gh0B, 1), fx1B, gyB, fzB);
        V8 v010A = corner_dot(&gr, GH(gh0A, 2), fxA, gy1A, fzA);
        V8 v010B = corner_dot(&gr, GH(gh0B, 2), fxB, gy1B, fzB);
        V8 v110A = corner_dot(&gr, GH(gh0A, 3), fx1A, gy1A, fzA);
        V8 v110B = corner_dot(&gr, GH(gh0B, 3), fx1B, gy1B, fzB);
        V8 v001A = corner_dot(&gr, GH(gh1A, 0), fxA, gyA, fz1A);
        V8 v001B = corner_dot(&gr, GH(gh1B, 0), fxB, gyB, fz1B);
        V8 v101A = corner_dot(&gr, GH(gh1A, 1), fx1A, gyA, fz1A);
        V8 v101B = corner_dot(&gr, GH(gh1B, 1), fx1B, gyB, fz1B);
        V8 v011A = corner_dot(&gr, GH(gh1A, 2), fxA, gy1A, fz1A);
        V8 v011B = corner_dot(&gr, GH(gh1B, 2), fxB, gy1B, fz1B);
        V8 v111A = corner_dot(&gr, GH(gh1A, 3), fx1A, gy1A, fz1A);
        V8 v111B = corner_dot(&gr, GH(gh1B, 3), fx1B, gy1B, fz1B);
#undef GH

        V8 sxA = v_smoothstep(fxA);
        V8 sxB = v_smoothstep(fxB);
        V8 syA = v_smoothstep(fyA); /* fade 는 양자화 전 fy */
        V8 syB = v_smoothstep(fyB);
        V8 szA = v_smoothstep(fzA);
        V8 szB = v_smoothstep(fzB);

        V8 l0A = v_lerp(syA, v_lerp(sxA, v000A, v100A),
                        v_lerp(sxA, v010A, v110A));
        V8 l0B = v_lerp(syB, v_lerp(sxB, v000B, v100B),
                        v_lerp(sxB, v010B, v110B));
        V8 l1A = v_lerp(syA, v_lerp(sxA, v001A, v101A),
                        v_lerp(sxA, v011A, v111A));
        V8 l1B = v_lerp(syB, v_lerp(sxB, v001B, v101B),
                        v_lerp(sxB, v011B, v111B));
        *rA = v_lerp(szA, l0A, l1A);
        *rB = v_lerp(szB, l0B, l1B);
        return;
    }

split: /* 포화/NaN 시맨틱 — 스트림별 원본 단일 경로 위임 (희귀) */
    HC_CTR_INC(HC_CTR_X8_SPLIT);
    *rA = perlin_x8(p, xA, yA, zA, yscale, ymaxA);
    *rB = perlin_x8(p, xB, yB, zB, yscale, ymaxB);
}

/* PerlinNoise.getValue x8 ×2 (octaves_x8 의 A/B 복제) */
static void octaves_x8_dual(const hc_octaves_t *o, V8 xA, V8 yA, V8 zA, V8 xB,
                            V8 yB, V8 zB, double yscale, V8 ymaxA, V8 ymaxB,
                            V8 *rA, V8 *rB) {
    V8     dA = _mm512_setzero_pd();
    V8     dB = _mm512_setzero_pd();
    double e = o->lowest_freq_input;
    double f = o->lowest_freq_value;
    for (int32_t i = 0; i < o->count; i++) {
        const hc_perlin_t *p = o->octaves[i];
        if (p) {
            V8 wxA, wxB, wyA, wyB, wzA, wzB, gA, gB;
            wrap_x8_dual(_mm512_mul_pd(xA, bc(e)), _mm512_mul_pd(xB, bc(e)),
                         &wxA, &wxB);
            wrap_x8_dual(_mm512_mul_pd(yA, bc(e)), _mm512_mul_pd(yB, bc(e)),
                         &wyA, &wyB);
            wrap_x8_dual(_mm512_mul_pd(zA, bc(e)), _mm512_mul_pd(zB, bc(e)),
                         &wzA, &wzB);
            perlin_x8_dual(p, wxA, wyA, wzA, wxB, wyB, wzB, yscale * e,
                           _mm512_mul_pd(ymaxA, bc(e)),
                           _mm512_mul_pd(ymaxB, bc(e)), &gA, &gB);
            /* d += (amplitudes[i] * g) * f — 좌결합 그대로 */
            dA = _mm512_add_pd(
                dA, _mm512_mul_pd(_mm512_mul_pd(bc(o->amplitudes[i]), gA),
                                  bc(f)));
            dB = _mm512_add_pd(
                dB, _mm512_mul_pd(_mm512_mul_pd(bc(o->amplitudes[i]), gB),
                                  bc(f)));
        }
        e *= 2.0;
        f /= 2.0;
    }
    *rA = dA;
    *rB = dB;
}

/* NormalNoise.getValue x8 ×2 */
static void normal_x8_dual(const hc_normal_noise_t *n, V8 xA, V8 yA, V8 zA,
                           V8 xB, V8 yB, V8 zB, V8 *rA, V8 *rB) {
    V8 x2A = _mm512_mul_pd(xA, bc(HC_NORMAL_INPUT_FACTOR));
    V8 x2B = _mm512_mul_pd(xB, bc(HC_NORMAL_INPUT_FACTOR));
    V8 y2A = _mm512_mul_pd(yA, bc(HC_NORMAL_INPUT_FACTOR));
    V8 y2B = _mm512_mul_pd(yB, bc(HC_NORMAL_INPUT_FACTOR));
    V8 z2A = _mm512_mul_pd(zA, bc(HC_NORMAL_INPUT_FACTOR));
    V8 z2B = _mm512_mul_pd(zB, bc(HC_NORMAL_INPUT_FACTOR));
    V8 zero = _mm512_setzero_pd();
    V8 aA, aB, bA, bB;
    octaves_x8_dual(&n->first, xA, yA, zA, xB, yB, zB, 0.0, zero, zero, &aA,
                    &aB);
    octaves_x8_dual(&n->second, x2A, y2A, z2A, x2B, y2B, z2B, 0.0, zero, zero,
                    &bA, &bB);
    *rA = _mm512_mul_pd(_mm512_add_pd(aA, bA), bc(n->value_factor));
    *rB = _mm512_mul_pd(_mm512_add_pd(aB, bB), bc(n->value_factor));
}

/* BlendedNoise.compute x8 ×2 (blended_x8 의 A/B 복제). over/under 단락
 * 플래그는 스트림별 — 편측만 필요한 라운드는 그 스트림만 단일 perlin_x8
 * 위임 (원본 경로, X8_SPLIT 계수). */
static void blended_x8_dual(const hc_blended_noise_t *b, V8 xiA, V8 yiA,
                            V8 ziA, V8 xiB, V8 yiB, V8 ziB, V8 *resA,
                            V8 *resB) {
    V8     dA = _mm512_mul_pd(xiA, bc(b->xz_mult));
    V8     dB = _mm512_mul_pd(xiB, bc(b->xz_mult));
    V8     eA = _mm512_mul_pd(yiA, bc(b->y_mult));
    V8     eB = _mm512_mul_pd(yiB, bc(b->y_mult));
    V8     fA = _mm512_mul_pd(ziA, bc(b->xz_mult));
    V8     fB = _mm512_mul_pd(ziB, bc(b->xz_mult));
    V8     gxA = _mm512_div_pd(dA, bc(b->xz_factor));
    V8     gxB = _mm512_div_pd(dB, bc(b->xz_factor));
    V8     gyA = _mm512_div_pd(eA, bc(b->y_factor));
    V8     gyB = _mm512_div_pd(eB, bc(b->y_factor));
    V8     gzA = _mm512_div_pd(fA, bc(b->xz_factor));
    V8     gzB = _mm512_div_pd(fB, bc(b->xz_factor));
    double smear_y = b->y_mult * b->smear;
    double smear_g = smear_y / b->y_factor;

    V8     nA = _mm512_setzero_pd();
    V8     nB = _mm512_setzero_pd();
    double o = 1.0;
    for (int p = 0; p < 8; p++) {
        const hc_perlin_t *oct = oct_at(&b->main_noise, p);
        if (oct) {
            V8 sA, sB, tA, tB, uA, uB, vA, vB;
            wrap_x8_dual(_mm512_mul_pd(gxA, bc(o)), _mm512_mul_pd(gxB, bc(o)),
                         &sA, &sB);
            wrap_x8_dual(_mm512_mul_pd(gyA, bc(o)), _mm512_mul_pd(gyB, bc(o)),
                         &tA, &tB);
            wrap_x8_dual(_mm512_mul_pd(gzA, bc(o)), _mm512_mul_pd(gzB, bc(o)),
                         &uA, &uB);
            perlin_x8_dual(oct, sA, tA, uA, sB, tB, uB, smear_g * o,
                           _mm512_mul_pd(gyA, bc(o)),
                           _mm512_mul_pd(gyB, bc(o)), &vA, &vB);
            nA = _mm512_add_pd(nA, _mm512_div_pd(vA, bc(o)));
            nB = _mm512_add_pd(nB, _mm512_div_pd(vB, bc(o)));
        }
        o /= 2.0;
    }
    V8 qA = _mm512_div_pd(
        _mm512_add_pd(_mm512_div_pd(nA, bc(10.0)), bc(1.0)), bc(2.0));
    V8 qB = _mm512_div_pd(
        _mm512_add_pd(_mm512_div_pd(nB, bc(10.0)), bc(1.0)), bc(2.0));

    __mmask8 overmA = _mm512_cmp_pd_mask(qA, bc(1.0), _CMP_GE_OQ);
    __mmask8 overmB = _mm512_cmp_pd_mask(qB, bc(1.0), _CMP_GE_OQ);
    __mmask8 undermA = _mm512_cmp_pd_mask(qA, bc(0.0), _CMP_LE_OQ);
    __mmask8 undermB = _mm512_cmp_pd_mask(qB, bc(0.0), _CMP_LE_OQ);
    int      over_allA = (overmA == 0xFF);
    int      over_allB = (overmB == 0xFF);
    int      under_allA = (undermA == 0xFF);
    int      under_allB = (undermB == 0xFF);
    if (hc_ctr_on) { /* 혼합-레인 계수 — 스트림당 1회 (원본과 총계 동일) */
        int mo = (int)overmA, mu = (int)undermA;
        if ((mo != 0 && mo != 0xFF) || (mu != 0 && mu != 0xFF))
            hc_ctr_tls[HC_CTR_X8_BLEND_MIX]++;
        mo = (int)overmB;
        mu = (int)undermB;
        if ((mo != 0 && mo != 0xFF) || (mu != 0 && mu != 0xFF))
            hc_ctr_tls[HC_CTR_X8_BLEND_MIX]++;
    }

    V8 loA = _mm512_setzero_pd();
    V8 loB = _mm512_setzero_pd();
    V8 hiA = _mm512_setzero_pd();
    V8 hiB = _mm512_setzero_pd();
    o = 1.0;
    for (int r = 0; r < 16; r++) {
        V8 sA, sB, tA, tB, uA, uB;
        wrap_x8_dual(_mm512_mul_pd(dA, bc(o)), _mm512_mul_pd(dB, bc(o)), &sA,
                     &sB);
        wrap_x8_dual(_mm512_mul_pd(eA, bc(o)), _mm512_mul_pd(eB, bc(o)), &tA,
                     &tB);
        wrap_x8_dual(_mm512_mul_pd(fA, bc(o)), _mm512_mul_pd(fB, bc(o)), &uA,
                     &uB);
        double v = smear_y * o;
        {
            const hc_perlin_t *oct = oct_at(&b->min_limit, r);
            if (oct) {
                if (!over_allA && !over_allB) {
                    V8 pA, pB;
                    perlin_x8_dual(oct, sA, tA, uA, sB, tB, uB, v,
                                   _mm512_mul_pd(eA, bc(o)),
                                   _mm512_mul_pd(eB, bc(o)), &pA, &pB);
                    loA = _mm512_add_pd(loA, _mm512_div_pd(pA, bc(o)));
                    loB = _mm512_add_pd(loB, _mm512_div_pd(pB, bc(o)));
                } else if (!over_allA) {
                    HC_CTR_INC(HC_CTR_X8_SPLIT);
                    loA = _mm512_add_pd(
                        loA,
                        _mm512_div_pd(perlin_x8(oct, sA, tA, uA, v,
                                                _mm512_mul_pd(eA, bc(o))),
                                      bc(o)));
                } else if (!over_allB) {
                    HC_CTR_INC(HC_CTR_X8_SPLIT);
                    loB = _mm512_add_pd(
                        loB,
                        _mm512_div_pd(perlin_x8(oct, sB, tB, uB, v,
                                                _mm512_mul_pd(eB, bc(o))),
                                      bc(o)));
                }
            }
        }
        {
            const hc_perlin_t *oct = oct_at(&b->max_limit, r);
            if (oct) {
                if (!under_allA && !under_allB) {
                    V8 pA, pB;
                    perlin_x8_dual(oct, sA, tA, uA, sB, tB, uB, v,
                                   _mm512_mul_pd(eA, bc(o)),
                                   _mm512_mul_pd(eB, bc(o)), &pA, &pB);
                    hiA = _mm512_add_pd(hiA, _mm512_div_pd(pA, bc(o)));
                    hiB = _mm512_add_pd(hiB, _mm512_div_pd(pB, bc(o)));
                } else if (!under_allA) {
                    HC_CTR_INC(HC_CTR_X8_SPLIT);
                    hiA = _mm512_add_pd(
                        hiA,
                        _mm512_div_pd(perlin_x8(oct, sA, tA, uA, v,
                                                _mm512_mul_pd(eA, bc(o))),
                                      bc(o)));
                } else if (!under_allB) {
                    HC_CTR_INC(HC_CTR_X8_SPLIT);
                    hiB = _mm512_add_pd(
                        hiB,
                        _mm512_div_pd(perlin_x8(oct, sB, tB, uB, v,
                                                _mm512_mul_pd(eB, bc(o))),
                                      bc(o)));
                }
            }
        }
        o /= 2.0;
    }
    /* 생략 레인은 스칼라와 동일하게 +0.0 (maskz: ~over 만 통과) */
    loA = _mm512_maskz_mov_pd(_knot_mask8(overmA), loA);
    loB = _mm512_maskz_mov_pd(_knot_mask8(overmB), loB);
    hiA = _mm512_maskz_mov_pd(_knot_mask8(undermA), hiA);
    hiB = _mm512_maskz_mov_pd(_knot_mask8(undermB), hiB);

    V8 rA = v_clamped_lerp(qA, _mm512_div_pd(loA, bc(512.0)),
                           _mm512_div_pd(hiA, bc(512.0)));
    V8 rB = v_clamped_lerp(qB, _mm512_div_pd(loB, bc(512.0)),
                           _mm512_div_pd(hiB, bc(512.0)));
    *resA = _mm512_div_pd(rA, bc(128.0));
    *resB = _mm512_div_pd(rB, bc(128.0));
}

/* x8_node 의 A/B 복제 — 같은 노드 (같은 그래프) 를 두 스트림 scratch 에
 * 대해 교차 실행. 케이스 본문은 x8_node 와 문장 단위 대응 (사이드-바이-
 * 사이드 리뷰 면). */
static void x8_node2(const x8_env_t *ea, const x8_env_t *eb, int32_t idx) {
    const hc_df_node_t *nd = &ea->g->nodes[idx];
    double             *slotA = ea->vsc + 8 * (size_t)idx;
    double             *slotB = eb->vsc + 8 * (size_t)idx;
#define OPVA(n) _mm512_load_pd(ea->vsc + 8 * (size_t)(n))
#define OPVB(n) _mm512_load_pd(eb->vsc + 8 * (size_t)(n))
    V8 rA, rB;
    switch (nd->op) {
    case HC_DF_CONST:
        rA = bc(nd->k0);
        rB = rA;
        break;
    case HC_DF_X:
        rA = ea->xv;
        rB = eb->xv;
        break;
    case HC_DF_Y:
        rA = ea->yv;
        rB = eb->yv;
        break;
    case HC_DF_Z:
        rA = ea->zv;
        rB = eb->zv;
        break;

    case HC_DF_ADD:
        rA = _mm512_add_pd(OPVA(nd->a), OPVA(nd->b));
        rB = _mm512_add_pd(OPVB(nd->a), OPVB(nd->b));
        break;
    case HC_DF_MUL: {
        V8       aA = OPVA(nd->a);
        V8       aB = OPVB(nd->a);
        __mmask8 nzA = _knot_mask8(
            _mm512_cmp_pd_mask(aA, _mm512_setzero_pd(), _CMP_EQ_OQ));
        __mmask8 nzB = _knot_mask8(
            _mm512_cmp_pd_mask(aB, _mm512_setzero_pd(), _CMP_EQ_OQ));
        rA = _mm512_maskz_mul_pd(nzA, aA, OPVA(nd->b));
        rB = _mm512_maskz_mul_pd(nzB, aB, OPVB(nd->b));
        break;
    }
    case HC_DF_MIN:
        rA = v_jmin(OPVA(nd->a), OPVA(nd->b));
        rB = v_jmin(OPVB(nd->a), OPVB(nd->b));
        break;
    case HC_DF_MAX:
        rA = v_jmax(OPVA(nd->a), OPVA(nd->b));
        rB = v_jmax(OPVB(nd->a), OPVB(nd->b));
        break;
    case HC_DF_ADD_CONST:
        rA = _mm512_add_pd(OPVA(nd->a), bc(nd->k0));
        rB = _mm512_add_pd(OPVB(nd->a), bc(nd->k0));
        break;
    case HC_DF_MUL_CONST:
        rA = _mm512_mul_pd(OPVA(nd->a), bc(nd->k0));
        rB = _mm512_mul_pd(OPVB(nd->a), bc(nd->k0));
        break;

    case HC_DF_ABS:
        rA = _mm512_andnot_pd(bc(-0.0), OPVA(nd->a));
        rB = _mm512_andnot_pd(bc(-0.0), OPVB(nd->a));
        break;
    case HC_DF_SQUARE: {
        V8 aA = OPVA(nd->a);
        V8 aB = OPVB(nd->a);
        rA = _mm512_mul_pd(aA, aA);
        rB = _mm512_mul_pd(aB, aB);
        break;
    }
    case HC_DF_CUBE: {
        V8 aA = OPVA(nd->a);
        V8 aB = OPVB(nd->a);
        rA = _mm512_mul_pd(_mm512_mul_pd(aA, aA), aA);
        rB = _mm512_mul_pd(_mm512_mul_pd(aB, aB), aB);
        break;
    }
    case HC_DF_HALF_NEGATIVE: {
        V8       aA = OPVA(nd->a);
        V8       aB = OPVB(nd->a);
        __mmask8 posA =
            _mm512_cmp_pd_mask(aA, _mm512_setzero_pd(), _CMP_GT_OQ);
        __mmask8 posB =
            _mm512_cmp_pd_mask(aB, _mm512_setzero_pd(), _CMP_GT_OQ);
        rA = _mm512_mask_mul_pd(aA, _knot_mask8(posA), aA, bc(0.5));
        rB = _mm512_mask_mul_pd(aB, _knot_mask8(posB), aB, bc(0.5));
        break;
    }
    case HC_DF_QUARTER_NEGATIVE: {
        V8       aA = OPVA(nd->a);
        V8       aB = OPVB(nd->a);
        __mmask8 posA =
            _mm512_cmp_pd_mask(aA, _mm512_setzero_pd(), _CMP_GT_OQ);
        __mmask8 posB =
            _mm512_cmp_pd_mask(aB, _mm512_setzero_pd(), _CMP_GT_OQ);
        rA = _mm512_mask_mul_pd(aA, _knot_mask8(posA), aA, bc(0.25));
        rB = _mm512_mask_mul_pd(aB, _knot_mask8(posB), aB, bc(0.25));
        break;
    }
    case HC_DF_SQUEEZE: {
        V8 dA = v_clamp(OPVA(nd->a), bc(-1.0), bc(1.0));
        V8 dB = v_clamp(OPVB(nd->a), bc(-1.0), bc(1.0));
        rA = _mm512_sub_pd(
            _mm512_div_pd(dA, bc(2.0)),
            _mm512_div_pd(_mm512_mul_pd(_mm512_mul_pd(dA, dA), dA),
                          bc(24.0)));
        rB = _mm512_sub_pd(
            _mm512_div_pd(dB, bc(2.0)),
            _mm512_div_pd(_mm512_mul_pd(_mm512_mul_pd(dB, dB), dB),
                          bc(24.0)));
        break;
    }
    case HC_DF_INVERT:
        rA = _mm512_div_pd(bc(1.0), OPVA(nd->a));
        rB = _mm512_div_pd(bc(1.0), OPVB(nd->a));
        break;

    case HC_DF_CLAMP:
        rA = v_clamp(OPVA(nd->a), bc(nd->k0), bc(nd->k1));
        rB = v_clamp(OPVB(nd->a), bc(nd->k0), bc(nd->k1));
        break;

    case HC_DF_Y_CLAMPED_GRADIENT: {
        V8 tA = _mm512_div_pd(_mm512_sub_pd(ea->yv, bc(nd->k0)),
                              bc(nd->k1 - nd->k0));
        V8 tB = _mm512_div_pd(_mm512_sub_pd(eb->yv, bc(nd->k0)),
                              bc(nd->k1 - nd->k0));
        rA = v_clamped_lerp(tA, bc(nd->k2), bc(nd->k3));
        rB = v_clamped_lerp(tB, bc(nd->k2), bc(nd->k3));
        break;
    }

    case HC_DF_RANGE_CHOICE: {
        V8       dA = OPVA(nd->a);
        V8       dB = OPVB(nd->a);
        __mmask8 inA = _mm512_cmp_pd_mask(dA, bc(nd->k0), _CMP_GE_OQ) &
                       _mm512_cmp_pd_mask(dA, bc(nd->k1), _CMP_LT_OQ);
        __mmask8 inB = _mm512_cmp_pd_mask(dB, bc(nd->k0), _CMP_GE_OQ) &
                       _mm512_cmp_pd_mask(dB, bc(nd->k1), _CMP_LT_OQ);
        rA = _mm512_mask_blend_pd(inA, OPVA(nd->c), OPVA(nd->b));
        rB = _mm512_mask_blend_pd(inB, OPVB(nd->c), OPVB(nd->b));
        break;
    }

    case HC_DF_NOISE: {
        V8 nxA = _mm512_mul_pd(ea->xv, bc(nd->k0));
        V8 nxB = _mm512_mul_pd(eb->xv, bc(nd->k0));
        V8 nyA = _mm512_mul_pd(ea->yv, bc(nd->k1));
        V8 nyB = _mm512_mul_pd(eb->yv, bc(nd->k1));
        V8 nzA = _mm512_mul_pd(ea->zv, bc(nd->k0));
        V8 nzB = _mm512_mul_pd(eb->zv, bc(nd->k0));
        normal_x8_dual(&ea->g->noises[nd->aux], nxA, nyA, nzA, nxB, nyB, nzB,
                       &rA, &rB);
        break;
    }
    case HC_DF_BLENDED_NOISE:
        blended_x8_dual(&ea->g->blended[nd->aux], ea->xv, ea->yv, ea->zv,
                        eb->xv, eb->yv, eb->zv, &rA, &rB);
        break;

    case HC_DF_INTERVAL_SELECT: {
        /* 선택·회수는 레인별 스칼라 (정수/비교 — 반올림 없음) */
        const double *avA = ea->vsc + 8 * (size_t)nd->a;
        const double *avB = eb->vsc + 8 * (size_t)nd->a;
        int32_t       nf = ea->g->ipool[nd->aux];
        double        outA[8], outB[8];
        for (int l = 0; l < 8; l++) {
            double  dvA = avA[l];
            int32_t selA = nf - 1;
            for (int32_t j = 0; j < nf - 1; j++)
                if (dvA < ea->g->dpool[nd->aux2 + j]) {
                    selA = j;
                    break;
                }
            outA[l] = ea->vsc[8 * (size_t)ea->g->ipool[nd->aux + 1 + selA] +
                              l];
            double  dvB = avB[l];
            int32_t selB = nf - 1;
            for (int32_t j = 0; j < nf - 1; j++)
                if (dvB < eb->g->dpool[nd->aux2 + j]) {
                    selB = j;
                    break;
                }
            outB[l] = eb->vsc[8 * (size_t)eb->g->ipool[nd->aux + 1 + selB] +
                              l];
        }
        rA = _mm512_loadu_pd(outA);
        rB = _mm512_loadu_pd(outB);
        break;
    }

    case HC_DF_BLEND_OFFSET:
        rA = _mm512_setzero_pd();
        rB = _mm512_setzero_pd();
        break;
    case HC_DF_BLEND_ALPHA:
        rA = bc(1.0);
        rB = rA;
        break;

    case HC_DF_INTERPOLATED: {
        int doneA = 0, doneB = 0;
        if (ea->cc && ea->cc->interp_of[idx] >= 0) {
            const hc_df_interp_t *it = &ea->cc->interp[ea->cc->interp_of[idx]];
            if (ea->cc->mode == HC_DF_MODE_CELL) {
                V8 dxv = _mm512_loadu_pd(ea->lanes->dx);
                V8 dyv = _mm512_loadu_pd(ea->lanes->dy);
                V8 dzv = _mm512_loadu_pd(ea->lanes->dz);
                V8 a0 = v_lerp(dyv, v_lerp(dxv, bc(it->n000), bc(it->n100)),
                               v_lerp(dxv, bc(it->n010), bc(it->n110)));
                V8 a1 = v_lerp(dyv, v_lerp(dxv, bc(it->n001), bc(it->n101)),
                               v_lerp(dxv, bc(it->n011), bc(it->n111)));
                rA = v_lerp(dzv, a0, a1);
                doneA = 1;
            } else if (ea->cc->mode == HC_DF_MODE_BLOCK) {
                rA = bc(it->value);
                doneA = 1;
            }
        }
        if (!doneA)
            rA = OPVA(nd->a); /* SP pass-through */
        if (eb->cc && eb->cc->interp_of[idx] >= 0) {
            const hc_df_interp_t *it = &eb->cc->interp[eb->cc->interp_of[idx]];
            if (eb->cc->mode == HC_DF_MODE_CELL) {
                V8 dxv = _mm512_loadu_pd(eb->lanes->dx);
                V8 dyv = _mm512_loadu_pd(eb->lanes->dy);
                V8 dzv = _mm512_loadu_pd(eb->lanes->dz);
                V8 a0 = v_lerp(dyv, v_lerp(dxv, bc(it->n000), bc(it->n100)),
                               v_lerp(dxv, bc(it->n010), bc(it->n110)));
                V8 a1 = v_lerp(dyv, v_lerp(dxv, bc(it->n001), bc(it->n101)),
                               v_lerp(dxv, bc(it->n011), bc(it->n111)));
                rB = v_lerp(dzv, a0, a1);
                doneB = 1;
            } else if (eb->cc->mode == HC_DF_MODE_BLOCK) {
                rB = bc(it->value);
                doneB = 1;
            }
        }
        if (!doneB)
            rB = OPVB(nd->a);
        break;
    }

    case HC_DF_FLAT_CACHE: {
        int doneA = 0, doneB = 0;
        if (ea->cc && ea->cc->flat_of[idx] >= 0) {
            const hc_df_flat_t *fl = &ea->cc->flat[ea->cc->flat_of[idx]];
            int32_t             size = ea->cc->noise_size_xz + 1;
            const double       *av = ea->vsc + 8 * (size_t)nd->a;
            double              out[8];
            for (int l = 0; l < 8; l++) {
                int32_t qx = (int32_t)ea->lanes->x[l] >> 2;
                int32_t qz = (int32_t)ea->lanes->z[l] >> 2;
                int32_t i = qx - ea->cc->first_noise_x;
                int32_t j = qz - ea->cc->first_noise_z;
                if (i >= 0 && j >= 0 && i < size && j < size) {
                    out[l] = fl->values[i + j * size];
                } else {
                    assert(!"flat_cache out-of-window in x8 context");
                    out[l] = av[l];
                }
            }
            rA = _mm512_loadu_pd(out);
            doneA = 1;
        }
        if (!doneA)
            rA = OPVA(nd->a);
        if (eb->cc && eb->cc->flat_of[idx] >= 0) {
            const hc_df_flat_t *fl = &eb->cc->flat[eb->cc->flat_of[idx]];
            int32_t             size = eb->cc->noise_size_xz + 1;
            const double       *av = eb->vsc + 8 * (size_t)nd->a;
            double              out[8];
            for (int l = 0; l < 8; l++) {
                int32_t qx = (int32_t)eb->lanes->x[l] >> 2;
                int32_t qz = (int32_t)eb->lanes->z[l] >> 2;
                int32_t i = qx - eb->cc->first_noise_x;
                int32_t j = qz - eb->cc->first_noise_z;
                if (i >= 0 && j >= 0 && i < size && j < size) {
                    out[l] = fl->values[i + j * size];
                } else {
                    assert(!"flat_cache out-of-window in x8 context");
                    out[l] = av[l];
                }
            }
            rB = _mm512_loadu_pd(out);
            doneB = 1;
        }
        if (!doneB)
            rB = OPVB(nd->a);
        break;
    }

    case HC_DF_BLEND_DENSITY:
    case HC_DF_CACHE_2D:
    case HC_DF_CACHE_ONCE:
    case HC_DF_CACHE_ALL_IN_CELL:
        rA = OPVA(nd->a);
        rB = OPVB(nd->a);
        break;

    default:
        /* hc_df_stream_x4_ok 화이트리스트가 사전 차단한다 (x4/x8 공용) */
        assert(!"unsupported op in x8x2 stream");
        rA = _mm512_setzero_pd();
        rB = rA;
        break;
    }
    _mm512_store_pd(slotA, rA);
    _mm512_store_pd(slotB, rB);
#undef OPVA
#undef OPVB
}

/* x8_run 의 2-스트림 판 — 같은 스트림 워드를 두 env 로 소비한다. 컨트롤
 * (RC/IS) 은 스트림별 마스크/선택으로 갈릴 수 있다: 두 스트림이 다 필요한
 * 레그만 dual 로, 한쪽만 필요한 레그는 그 스트림의 원본 x8_run 으로 위임
 * (스트림별 실행 시퀀스는 단독 실행과 동일 — then 레그가 else 레그보다
 * 먼저라는 순서도 스트림별로 보존된다). */
static void x8_run2(const x8_env_t *ea, const x8_env_t *eb, const int32_t *p,
                    int32_t words) {
    const int32_t *end = p + words;
    while (p < end) {
        int32_t v = *p++;
        if (v >= 0) {
            HC_CTR_INC_HOT(HC_CTR_X8_NODE);
            HC_CTR_INC_HOT(HC_CTR_X8_NODE);
            x8_node2(ea, eb, v);
            continue;
        }
        if (v == -1) { /* PROG_RC */
            int32_t             ch = p[0], wt = p[1], we = p[2];
            const hc_df_node_t *nd = &ea->g->nodes[ch];
            V8 dA = _mm512_load_pd(ea->vsc + 8 * (size_t)nd->a);
            V8 dB = _mm512_load_pd(eb->vsc + 8 * (size_t)nd->a);
            __mmask8 inA = _mm512_cmp_pd_mask(dA, bc(nd->k0), _CMP_GE_OQ) &
                           _mm512_cmp_pd_mask(dA, bc(nd->k1), _CMP_LT_OQ);
            __mmask8 inB = _mm512_cmp_pd_mask(dB, bc(nd->k0), _CMP_GE_OQ) &
                           _mm512_cmp_pd_mask(dB, bc(nd->k1), _CMP_LT_OQ);
            int mA = (int)inA, mB = (int)inB;
            if (mA != 0 && mA != 0xFF)
                HC_CTR_INC(HC_CTR_X8_RC_MIX);
            if (mB != 0 && mB != 0xFF)
                HC_CTR_INC(HC_CTR_X8_RC_MIX);
            if (mA != 0 && mB != 0) {
                x8_run2(ea, eb, p + 3, wt);
            } else if (mA != 0) {
                HC_CTR_INC(HC_CTR_X8_SPLIT);
                x8_run(ea, p + 3, wt);
            } else if (mB != 0) {
                HC_CTR_INC(HC_CTR_X8_SPLIT);
                x8_run(eb, p + 3, wt);
            }
            if (mA != 0xFF && mB != 0xFF) {
                x8_run2(ea, eb, p + 3 + wt, we);
            } else if (mA != 0xFF) {
                HC_CTR_INC(HC_CTR_X8_SPLIT);
                x8_run(ea, p + 3 + wt, we);
            } else if (mB != 0xFF) {
                HC_CTR_INC(HC_CTR_X8_SPLIT);
                x8_run(eb, p + 3 + wt, we);
            }
            _mm512_store_pd(
                ea->vsc + 8 * (size_t)ch,
                _mm512_mask_blend_pd(
                    inA, _mm512_load_pd(ea->vsc + 8 * (size_t)nd->c),
                    _mm512_load_pd(ea->vsc + 8 * (size_t)nd->b)));
            _mm512_store_pd(
                eb->vsc + 8 * (size_t)ch,
                _mm512_mask_blend_pd(
                    inB, _mm512_load_pd(eb->vsc + 8 * (size_t)nd->c),
                    _mm512_load_pd(eb->vsc + 8 * (size_t)nd->b)));
            p += 3 + wt + we;
        } else { /* PROG_IS */
            int32_t             ch = p[0], nf = p[1];
            const int32_t      *w = p + 2;
            const hc_df_node_t *nd = &ea->g->nodes[ch];
            const double       *avA = ea->vsc + 8 * (size_t)nd->a;
            const double       *avB = eb->vsc + 8 * (size_t)nd->a;
            int32_t             selA[8], selB[8];
            for (int l = 0; l < 8; l++) {
                double  dvA = avA[l];
                int32_t sA = nf - 1;
                for (int32_t j = 0; j < nf - 1; j++)
                    if (dvA < ea->g->dpool[nd->aux2 + j]) {
                        sA = j;
                        break;
                    }
                selA[l] = sA;
                double  dvB = avB[l];
                int32_t sB = nf - 1;
                for (int32_t j = 0; j < nf - 1; j++)
                    if (dvB < eb->g->dpool[nd->aux2 + j]) {
                        sB = j;
                        break;
                    }
                selB[l] = sB;
            }
            int mixA = 0, mixB = 0;
            for (int l = 1; l < 8; l++) {
                mixA |= (selA[l] != selA[0]);
                mixB |= (selB[l] != selB[0]);
            }
            if (mixA)
                HC_CTR_INC(HC_CTR_X8_IS_MIX);
            if (mixB)
                HC_CTR_INC(HC_CTR_X8_IS_MIX);
            const int32_t *q = p + 2 + nf;
            for (int32_t k = 0; k < nf; k++) {
                int needA = 0, needB = 0;
                for (int l = 0; l < 8; l++) {
                    needA |= (selA[l] == k);
                    needB |= (selB[l] == k);
                }
                if (w[k] > 0) {
                    if (needA && needB) {
                        x8_run2(ea, eb, q, w[k]);
                    } else if (needA) {
                        HC_CTR_INC(HC_CTR_X8_SPLIT);
                        x8_run(ea, q, w[k]);
                    } else if (needB) {
                        HC_CTR_INC(HC_CTR_X8_SPLIT);
                        x8_run(eb, q, w[k]);
                    }
                }
                q += w[k];
            }
            double outA[8], outB[8];
            for (int l = 0; l < 8; l++) {
                outA[l] = ea->vsc[8 * (size_t)ea->g->ipool[nd->aux + 1 +
                                                           selA[l]] +
                                  l];
                outB[l] = eb->vsc[8 * (size_t)eb->g->ipool[nd->aux + 1 +
                                                           selB[l]] +
                                  l];
            }
            _mm512_storeu_pd(ea->vsc + 8 * (size_t)ch, _mm512_loadu_pd(outA));
            _mm512_storeu_pd(eb->vsc + 8 * (size_t)ch, _mm512_loadu_pd(outB));
            p = q;
        }
    }
}

void hc_df_eval_stream_x8x2_avx512(
    const hc_df_graph_t *g, const int32_t *stream, int32_t words,
    const hc_df_lanes8_t *lanesA, double *vscA, const hc_df_cellctx_t *ccA,
    const hc_df_lanes8_t *lanesB, double *vscB, const hc_df_cellctx_t *ccB) {
#ifndef NDEBUG
    /* 좌표는 정수값 블록 좌표 계약 (BLENDED int 캐스트 동치의 전제) */
    for (int l = 0; l < 8; l++) {
        assert(lanesA->x[l] == (double)(int32_t)lanesA->x[l]);
        assert(lanesA->y[l] == (double)(int32_t)lanesA->y[l]);
        assert(lanesA->z[l] == (double)(int32_t)lanesA->z[l]);
        assert(lanesB->x[l] == (double)(int32_t)lanesB->x[l]);
        assert(lanesB->y[l] == (double)(int32_t)lanesB->y[l]);
        assert(lanesB->z[l] == (double)(int32_t)lanesB->z[l]);
    }
#endif
    x8_env_t ea = {
        .g = g,
        .lanes = lanesA,
        .vsc = vscA,
        .cc = ccA,
        .xv = _mm512_loadu_pd(lanesA->x),
        .yv = _mm512_loadu_pd(lanesA->y),
        .zv = _mm512_loadu_pd(lanesA->z),
    };
    x8_env_t eb = {
        .g = g,
        .lanes = lanesB,
        .vsc = vscB,
        .cc = ccB,
        .xv = _mm512_loadu_pd(lanesB->x),
        .yv = _mm512_loadu_pd(lanesB->y),
        .zv = _mm512_loadu_pd(lanesB->z),
    };
    x8_run2(&ea, &eb, stream, words);
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

void hc_df_eval_stream_x8x2_avx512(
    const hc_df_graph_t *g, const int32_t *stream, int32_t words,
    const hc_df_lanes8_t *lanesA, double *vscA, const hc_df_cellctx_t *ccA,
    const hc_df_lanes8_t *lanesB, double *vscB, const hc_df_cellctx_t *ccB) {
    (void)g;
    (void)stream;
    (void)words;
    (void)lanesA;
    (void)vscA;
    (void)ccA;
    (void)lanesB;
    (void)vscB;
    (void)ccB;
    abort();
}

#endif
