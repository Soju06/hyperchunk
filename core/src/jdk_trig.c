#include "hc_jdk_trig.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* JDK HotSpot x86-64 dsin/dcos 스텁 (Intel LIBM) 전사 — jdk25u
 * stubGenerator_x86_64_sin.cpp / _cos.cpp. 알고리즘 (스텁 헤더 주석 §1-7):
 *   X = N*pi/32 + r 축소 (P_1+P_2+P_3 로 전정밀도, c = 축소 잔차) 후
 *   B = M*pi/32 (M = N mod 64) 테이블 [C_hl, S_hi, S_lo, sigma] 로
 *   sin(B+r+c) 를 hi+med+pols+corr 4 부분 보상합산으로 재구성한다.
 * 전사 규칙: SSE 스칼라/레인 연산 1개 = C double 연산 1개, 명령 순서
 * 그대로 (빌드는 -ffp-contract=off — a*b+c 융합 없음). 대수 단순화 금지.
 * packed 레지스터는 레인별 변수 2개로 모델링, andpd/por 는 비트 연산. */

static inline double hc__f64(uint64_t b) {
    double d;
    memcpy(&d, &b, sizeof d);
    return d;
}

static inline uint64_t hc__bits(double d) {
    uint64_t b;
    memcpy(&b, &d, sizeof b);
    return b;
}

/* --- stubGenerator_x86_64_constants.cpp 의 juint 쌍 (lo,hi) → u64 --- */

#define HC__PI32INV 0x40245f306dc9c883ULL /* 32/pi */
#define HC__ONEHALF 0x3fe0000000000000ULL /* 0.5 (packed 양 레인 동일) */
#define HC__SIGN_MASK 0x8000000000000000ULL
#define HC__P_1 0x3fb921fb54400000ULL /* pi/32 상위 (N 곱 무오차) */
#define HC__P_2 0x3d90b4611a600000ULL
#define HC__P_3 0x3b63198a2e037073ULL
#define HC__SC_4_LO 0x3ec71de3a556c734ULL /* sin 계수 r^7 */
#define HC__SC_4_HI 0x3efa01a01a01a01aULL /* cos 계수 r^6 */
#define HC__SC_3_LO 0xbf2a01a01a01a01aULL
#define HC__SC_3_HI 0xbf56c16c16c16c17ULL
#define HC__SC_2_LO 0x3f81111111111111ULL
#define HC__SC_2_HI 0x3fa5555555555555ULL
#define HC__SC_1_LO 0xbfc5555555555555ULL
#define HC__SC_1_HI 0xbfe0000000000000ULL
#define HC__ALL_ONES 0x3fefffffffffffffULL /* 1 - 2^-53 (sin 소인자) */
#define HC__ONE 0x3ff0000000000000ULL

/* Ctable — 행 32바이트 = [C_hl, S_hi, S_lo, sigma], M = 0..63.
 * sigma 는 cos(B) 에 가장 가까운 2의 거듭제곱, C_hl = cos(B)-sigma,
 * S_hi+S_lo = 2x53비트 sin(B). _constants.cpp _Ctable 그대로. */
static const uint64_t HC__CTABLE[64][4] = {
    {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0x3ff0000000000000ULL},
    {0xbf73b92e176d6d31ULL, 0x3fb917a6bc29b42cULL, 0xbc3e2718e0000000ULL, 0x3ff0000000000000ULL},
    {0xbf93ad06011469fbULL, 0x3fc8f8b83c69a60bULL, 0xbc626d19c0000000ULL, 0x3ff0000000000000ULL},
    {0xbfa60bea939d225aULL, 0x3fd294062ed59f06ULL, 0xbc75d28da0000000ULL, 0x3ff0000000000000ULL},
    {0xbfb37ca1866b95cfULL, 0x3fd87de2a6aea963ULL, 0xbc672cede0000000ULL, 0x3ff0000000000000ULL},
    {0xbfbe3a6873fa1279ULL, 0x3fde2b5d3806f63bULL, 0x3c5e0d8920000000ULL, 0x3ff0000000000000ULL},
    {0xbfc592675bc57974ULL, 0x3fe1c73b39ae68c8ULL, 0x3c8b25dd20000000ULL, 0x3ff0000000000000ULL},
    {0xbfcd0dfe53aba2fdULL, 0x3fe44cf325091dd6ULL, 0x3c68076a20000000ULL, 0x3ff0000000000000ULL},
    {0x3fca827999fcef32ULL, 0x3fe6a09e667f3bcdULL, 0xbc8bdd3420000000ULL, 0x3fe0000000000000ULL},
    {0x3fc133cc94247758ULL, 0x3fe8bc806b151741ULL, 0xbc82c5e120000000ULL, 0x3fe0000000000000ULL},
    {0x3fac73b39ae68c87ULL, 0x3fea9b66290ea1a3ULL, 0x3c39f630e0000000ULL, 0x3fe0000000000000ULL},
    {0xbf9d4a2c7f909c4eULL, 0x3fec38b2f180bdb1ULL, 0xbc76e0b180000000ULL, 0x3fe0000000000000ULL},
    {0xbfbe087565455a75ULL, 0x3fed906bcf328d46ULL, 0x3c7457e620000000ULL, 0x3fe0000000000000ULL},
    {0x3fa4a03176acf82dULL, 0x3fee9f4156c62ddaULL, 0x3c8760b1e0000000ULL, 0x3fd0000000000000ULL},
    {0xbfac1d1f0e5967d5ULL, 0x3fef6297cff75cb0ULL, 0x3c75621720000000ULL, 0x3fd0000000000000ULL},
    {0xbf9ba1650f592f50ULL, 0x3fefd88da3d12526ULL, 0xbc887df640000000ULL, 0x3fc0000000000000ULL},
    {0x0000000000000000ULL, 0x3ff0000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL},
    {0x3f9ba1650f592f50ULL, 0x3fefd88da3d12526ULL, 0xbc887df640000000ULL, 0xbfc0000000000000ULL},
    {0x3fac1d1f0e5967d5ULL, 0x3fef6297cff75cb0ULL, 0x3c75621720000000ULL, 0xbfd0000000000000ULL},
    {0xbfa4a03176acf82dULL, 0x3fee9f4156c62ddaULL, 0x3c8760b1e0000000ULL, 0xbfd0000000000000ULL},
    {0x3fbe087565455a75ULL, 0x3fed906bcf328d46ULL, 0x3c7457e620000000ULL, 0xbfe0000000000000ULL},
    {0x3f9d4a2c7f909c4eULL, 0x3fec38b2f180bdb1ULL, 0xbc76e0b180000000ULL, 0xbfe0000000000000ULL},
    {0xbfac73b39ae68c87ULL, 0x3fea9b66290ea1a3ULL, 0x3c39f630e0000000ULL, 0xbfe0000000000000ULL},
    {0xbfc133cc94247758ULL, 0x3fe8bc806b151741ULL, 0xbc82c5e120000000ULL, 0xbfe0000000000000ULL},
    {0xbfca827999fcef32ULL, 0x3fe6a09e667f3bcdULL, 0xbc8bdd3420000000ULL, 0xbfe0000000000000ULL},
    {0x3fcd0dfe53aba2fdULL, 0x3fe44cf325091dd6ULL, 0x3c68076a20000000ULL, 0xbff0000000000000ULL},
    {0x3fc592675bc57974ULL, 0x3fe1c73b39ae68c8ULL, 0x3c8b25dd20000000ULL, 0xbff0000000000000ULL},
    {0x3fbe3a6873fa1279ULL, 0x3fde2b5d3806f63bULL, 0x3c5e0d8920000000ULL, 0xbff0000000000000ULL},
    {0x3fb37ca1866b95cfULL, 0x3fd87de2a6aea963ULL, 0xbc672cede0000000ULL, 0xbff0000000000000ULL},
    {0x3fa60bea939d225aULL, 0x3fd294062ed59f06ULL, 0xbc75d28da0000000ULL, 0xbff0000000000000ULL},
    {0x3f93ad06011469fbULL, 0x3fc8f8b83c69a60bULL, 0xbc626d19c0000000ULL, 0xbff0000000000000ULL},
    {0x3f73b92e176d6d31ULL, 0x3fb917a6bc29b42cULL, 0xbc3e2718e0000000ULL, 0xbff0000000000000ULL},
    {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL, 0xbff0000000000000ULL},
    {0x3f73b92e176d6d31ULL, 0xbfb917a6bc29b42cULL, 0x3c3e2718e0000000ULL, 0xbff0000000000000ULL},
    {0x3f93ad06011469fbULL, 0xbfc8f8b83c69a60bULL, 0x3c626d19c0000000ULL, 0xbff0000000000000ULL},
    {0x3fa60bea939d225aULL, 0xbfd294062ed59f06ULL, 0x3c75d28da0000000ULL, 0xbff0000000000000ULL},
    {0x3fb37ca1866b95cfULL, 0xbfd87de2a6aea963ULL, 0x3c672cede0000000ULL, 0xbff0000000000000ULL},
    {0x3fbe3a6873fa1279ULL, 0xbfde2b5d3806f63bULL, 0xbc5e0d8920000000ULL, 0xbff0000000000000ULL},
    {0x3fc592675bc57974ULL, 0xbfe1c73b39ae68c8ULL, 0xbc8b25dd20000000ULL, 0xbff0000000000000ULL},
    {0x3fcd0dfe53aba2fdULL, 0xbfe44cf325091dd6ULL, 0xbc68076a20000000ULL, 0xbff0000000000000ULL},
    {0xbfca827999fcef32ULL, 0xbfe6a09e667f3bcdULL, 0x3c8bdd3420000000ULL, 0xbfe0000000000000ULL},
    {0xbfc133cc94247758ULL, 0xbfe8bc806b151741ULL, 0x3c82c5e120000000ULL, 0xbfe0000000000000ULL},
    {0xbfac73b39ae68c87ULL, 0xbfea9b66290ea1a3ULL, 0xbc39f630e0000000ULL, 0xbfe0000000000000ULL},
    {0x3f9d4a2c7f909c4eULL, 0xbfec38b2f180bdb1ULL, 0x3c76e0b180000000ULL, 0xbfe0000000000000ULL},
    {0x3fbe087565455a75ULL, 0xbfed906bcf328d46ULL, 0xbc7457e620000000ULL, 0xbfe0000000000000ULL},
    {0xbfa4a03176acf82dULL, 0xbfee9f4156c62ddaULL, 0xbc8760b1e0000000ULL, 0xbfd0000000000000ULL},
    {0x3fac1d1f0e5967d5ULL, 0xbfef6297cff75cb0ULL, 0xbc75621720000000ULL, 0xbfd0000000000000ULL},
    {0x3f9ba1650f592f50ULL, 0xbfefd88da3d12526ULL, 0x3c887df640000000ULL, 0xbfc0000000000000ULL},
    {0x0000000000000000ULL, 0xbff0000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL},
    {0xbf9ba1650f592f50ULL, 0xbfefd88da3d12526ULL, 0x3c887df640000000ULL, 0x3fc0000000000000ULL},
    {0xbfac1d1f0e5967d5ULL, 0xbfef6297cff75cb0ULL, 0xbc75621720000000ULL, 0x3fd0000000000000ULL},
    {0x3fa4a03176acf82dULL, 0xbfee9f4156c62ddaULL, 0xbc8760b1e0000000ULL, 0x3fd0000000000000ULL},
    {0xbfbe087565455a75ULL, 0xbfed906bcf328d46ULL, 0xbc7457e620000000ULL, 0x3fe0000000000000ULL},
    {0xbf9d4a2c7f909c4eULL, 0xbfec38b2f180bdb1ULL, 0x3c76e0b180000000ULL, 0x3fe0000000000000ULL},
    {0x3fac73b39ae68c87ULL, 0xbfea9b66290ea1a3ULL, 0xbc39f630e0000000ULL, 0x3fe0000000000000ULL},
    {0x3fc133cc94247758ULL, 0xbfe8bc806b151741ULL, 0x3c82c5e120000000ULL, 0x3fe0000000000000ULL},
    {0x3fca827999fcef32ULL, 0xbfe6a09e667f3bcdULL, 0x3c8bdd3420000000ULL, 0x3fe0000000000000ULL},
    {0xbfcd0dfe53aba2fdULL, 0xbfe44cf325091dd6ULL, 0xbc68076a20000000ULL, 0x3ff0000000000000ULL},
    {0xbfc592675bc57974ULL, 0xbfe1c73b39ae68c8ULL, 0xbc8b25dd20000000ULL, 0x3ff0000000000000ULL},
    {0xbfbe3a6873fa1279ULL, 0xbfde2b5d3806f63bULL, 0xbc5e0d8920000000ULL, 0x3ff0000000000000ULL},
    {0xbfb37ca1866b95cfULL, 0xbfd87de2a6aea963ULL, 0x3c672cede0000000ULL, 0x3ff0000000000000ULL},
    {0xbfa60bea939d225aULL, 0xbfd294062ed59f06ULL, 0x3c75d28da0000000ULL, 0x3ff0000000000000ULL},
    {0xbf93ad06011469fbULL, 0xbfc8f8b83c69a60bULL, 0x3c626d19c0000000ULL, 0x3ff0000000000000ULL},
    {0xbf73b92e176d6d31ULL, 0xbfb917a6bc29b42cULL, 0x3c3e2718e0000000ULL, 0x3ff0000000000000ULL}
};

/* 분기 선별 (sin/cos 공통 프롤로그):
 *   rax = (hi32(x) & 0x7fff0000) - 0x30300000; cmp 0x10c50000
 *   unsigned <= : 메인 패스 (2^-252 <= |x| < ~90112)
 *   signed  >  : 거대 인자 Payne-Hanek 경로 (9a 도메인 [0,pi) 밖 — abort)
 *   그 외      : 소인자 경로 (|x| < 2^-252; x=+0.0 포함)
 * 상수 0x30300000 = 2^-252 의 지수장, 폭 0x10c50000 → 상한 0x40f50000
 * (~90112). */

double hc_jdk_sin(double x) {
    const uint32_t hx = (uint32_t)(hc__bits(x) >> 32); /* movl rax,[rsp+12] */
    const uint32_t ax = (hx & 0x7fff0000u) - 0x30300000u; /* andl; subl */

    if (ax <= 281346048u) { /* jcc(above, L0) 불발 — 메인 패스 */
        /* --- §1 축소: y = x*32/pi + copysign(0.5,x), N = trunc(y) --- */
        double y = hc__f64(HC__PI32INV) * x;    /* mulsd xmm1, xmm0 */
        uint64_t sgn = HC__SIGN_MASK & hc__bits(x);      /* pand xmm4, xmm0 */
        y = y + hc__f64(HC__ONEHALF | sgn); /* por xmm5,xmm4; addpd xmm1 */
        int32_t n = (int32_t)y;                     /* cvttsd2sil rdx, xmm1 */
        double dn = (double)n;                      /* cvtsi2sdl xmm1, rdx */
        double m1 = hc__f64(HC__P_1) * dn;            /* mulsd xmm3, xmm1 */
        const uint64_t *tb = HC__CTABLE[n & 63]; /* andl 63; shll 5; lea */
        double m2 = hc__f64(HC__P_2) * dn; /* mulpd xmm6, xmm1 (레인 동일) */
        double m3 = hc__f64(HC__P_3) * dn;            /* mulsd xmm1, P_3 */
        double r1 = x - m1;      /* subsd xmm4, xmm3 (= subsd xmm0, xmm3) */
        double s_hi = hc__f64(tb[1]);               /* movq xmm7, [rax+8] */
        double r = r1 - m2;                           /* subsd xmm4, xmm6 */
        double psc4_lo = hc__f64(HC__SC_4_LO) * r1;   /* mulpd xmm5, xmm0 */
        double psc4_hi = hc__f64(HC__SC_4_HI) * r1;
        /* subpd xmm0, xmm6: hi 레인도 r1-m2 — r 로 병합 (비트 동일) */
        double shi_r = s_hi * r;                      /* mulsd xmm7, xmm4 */
        double c1 = r1 - r;         /* movddup xmm3,xmm4; subsd xmm3,xmm4 */
        double msc4_lo = psc4_lo * r;                 /* mulpd xmm5, xmm0 */
        double msc4_hi = psc4_hi * r;
        double r2 = r * r;                            /* mulpd xmm0, xmm0 */
        double c2 = c1 - m2;                          /* subsd xmm3, xmm6 */
        double negc = m3 - c2;               /* subsd xmm1, xmm3 (= -c) */
        double sigma = hc__f64(tb[3]);             /* movq xmm3, [rax+24] */
        double chl_sigma = hc__f64(tb[0]) + sigma;    /* addsd xmm2, xmm3 */
        double negd = shi_r - chl_sigma;     /* subsd xmm7, xmm2 (= -d) */
        double csr = chl_sigma * r;                   /* mulsd xmm2, xmm4 */
        double msc2_lo = hc__f64(HC__SC_2_LO) * r2;   /* mulpd xmm6, xmm0 */
        double msc2_hi = hc__f64(HC__SC_2_HI) * r2;
        double rs = sigma * r;                        /* mulsd xmm3, xmm4 */
        double csr3 = csr * r2;              /* mulpd xmm2, xmm0 (lo 레인) */
        double shi_r2 = s_hi * r2;                            /* (hi 레인) */
        double r4 = r2 * r2;                          /* mulpd xmm0, xmm0 */
        double psc3_lo = msc4_lo + hc__f64(HC__SC_3_LO); /* addpd xmm5 */
        double psc3_hi = msc4_hi + hc__f64(HC__SC_3_HI);
        double med = r * hc__f64(tb[0]);            /* mulsd xmm4, [rax] */
        double psc1_lo = msc2_lo + hc__f64(HC__SC_1_LO); /* addpd xmm6 */
        double psc1_hi = msc2_hi + hc__f64(HC__SC_1_HI);
        double msc3_lo = psc3_lo * r4;                /* mulpd xmm5, xmm0 */
        double msc3_hi = psc3_hi * r4;
        double res_int = rs + s_hi;                /* addsd xmm3, [rax+8] */
        double corr = negc * negd;           /* mulpd xmm1, xmm7 (lo 레인) */
        double res_hi = med + res_int;                /* addsd xmm4, xmm3 */
        double pols_lo = psc1_lo + msc3_lo;           /* addpd xmm6, xmm5 */
        double pols_hi = psc1_hi + msc3_hi;
        double k0 = s_hi - res_int;   /* movq xmm5,[rax+8]; subsd xmm5 */
        double k1 = res_int - res_hi;                 /* subsd xmm3, xmm4 */
        corr = corr + hc__f64(tb[2]);   /* addsd xmm1, [rax+16] (+S_lo) */
        pols_lo = pols_lo * csr3;            /* mulpd xmm6, xmm2 (lo 레인) */
        pols_hi = pols_hi * shi_r2;                           /* (hi 레인) */
        double k2 = k0 + rs;                          /* addsd xmm5, xmm0 */
        double k3 = k1 + med;                         /* addsd xmm3, xmm7 */
        double res_lo = corr + k2;                    /* addsd xmm1, xmm5 */
        res_lo = res_lo + k3;                         /* addsd xmm1, xmm3 */
        res_lo = res_lo + pols_lo;                    /* addsd xmm1, xmm6 */
        res_lo = res_lo + pols_hi;         /* unpckhpd; addsd xmm1, xmm6 */
        return res_hi + res_lo;                       /* addsd xmm0, xmm1 */
    }

    if ((int32_t)ax > 281346048) {
        /* jcc(greater, L1): |x| >= ~90112 또는 NaN/Inf — Payne-Hanek
         * 축소 경로. 9a 도메인 계약 [0, pi) 밖 — 전사 생략, fail-loud. */
        abort();
    }

    /* --- §7 소인자 |x| < 2^-252 --- */
    if ((ax >> 20) == 3325u) { /* shrl 20; cmpl 3325 — 지수장 0 (±0/서브노멀) */
        return x * hc__f64(HC__ALL_ONES); /* mulsd xmm0, ALL_ONES; +0→+0 */
    }
    /* 정규 소인자: 스텁은 2^-55*(2^55*x - x) 를 xmm3 에 계산만 하고
     * (inexact 플래그용, 값 미사용) xmm0 = x 를 그대로 반환한다 */
    return x;
}

double hc_jdk_cos(double x) {
    const uint32_t hx = (uint32_t)(hc__bits(x) >> 32); /* movl rax,[rsp+12] */
    const uint32_t ax = (hx & 0x7fff0000u) - 0x30300000u; /* andl; subl */

    if (ax <= 281346048u) { /* jcc(above, L0) 불발 — 메인 패스 */
        /* sin 메인 패스와 동형 — 차이는 테이블 인덱스 M = (N+16) mod 64
         * (cos(x) = sin(x + pi/2), addq rdx 1865232; 1865232 % 64 == 16)
         * 와 말미 합산이 xmm0 누적이라는 점 (가환 — 비트 동일 순서 유지) */
        double y = hc__f64(HC__PI32INV) * x;          /* mulsd xmm1, xmm0 */
        uint64_t sgn = HC__SIGN_MASK & hc__bits(x);      /* pand xmm4, xmm0 */
        y = y + hc__f64(HC__ONEHALF | sgn); /* por xmm5,xmm4; addpd xmm1 */
        int32_t n = (int32_t)y;                     /* cvttsd2sil rdx, xmm1 */
        double dn = (double)n;                      /* cvtsi2sdl xmm1, rdx */
        double m1 = hc__f64(HC__P_1) * dn;            /* mulsd xmm3, xmm1 */
        /* addq rdx,1865232; andq 63 — rdx 는 32비트 cvt 결과 제로 확장 */
        const uint64_t *tb = HC__CTABLE[((uint32_t)n + 1865232u) & 63u];
        double m2 = hc__f64(HC__P_2) * dn; /* mulpd xmm2, xmm1 (레인 동일) */
        double r1 = x - m1;      /* subsd xmm0, xmm3 (= subsd xmm4, xmm3) */
        double m3 = hc__f64(HC__P_3) * dn;            /* mulsd xmm1, P_3 */
        double s_hi = hc__f64(tb[1]);               /* movq xmm7, [rax+8] */
        double r = r1 - m2;                           /* subsd xmm4, xmm2 */
        double psc4_lo = hc__f64(HC__SC_4_LO) * r1;   /* mulpd xmm5, xmm0 */
        double psc4_hi = hc__f64(HC__SC_4_HI) * r1;
        /* subpd xmm0, xmm2: hi 레인도 r1-m2 — r 로 병합 (비트 동일) */
        double shi_r = s_hi * r;                      /* mulsd xmm7, xmm4 */
        double c1 = r1 - r;        /* movdqu xmm3,xmm4; subsd xmm3,xmm4 */
        double msc4_lo = psc4_lo * r;                 /* mulpd xmm5, xmm0 */
        double msc4_hi = psc4_hi * r;
        double r2 = r * r;                            /* mulpd xmm0, xmm0 */
        double c2 = c1 - m2;                          /* subsd xmm3, xmm2 */
        double negc = m3 - c2;               /* subsd xmm1, xmm3 (= -c) */
        double sigma = hc__f64(tb[3]);             /* movq xmm3, [rax+24] */
        double chl_sigma = hc__f64(tb[0]) + sigma;    /* addsd xmm2, xmm3 */
        double negd = shi_r - chl_sigma;     /* subsd xmm7, xmm2 (= -d) */
        double csr = chl_sigma * r;                   /* mulsd xmm2, xmm4 */
        double msc2_lo = hc__f64(HC__SC_2_LO) * r2;   /* mulpd xmm6, xmm0 */
        double msc2_hi = hc__f64(HC__SC_2_HI) * r2;
        double rs = sigma * r;                        /* mulsd xmm3, xmm4 */
        double csr3 = csr * r2;              /* mulpd xmm2, xmm0 (lo 레인) */
        double shi_r2 = s_hi * r2;                            /* (hi 레인) */
        double r4 = r2 * r2;                          /* mulpd xmm0, xmm0 */
        double psc3_lo = msc4_lo + hc__f64(HC__SC_3_LO); /* addpd xmm5 */
        double psc3_hi = msc4_hi + hc__f64(HC__SC_3_HI);
        double med = r * hc__f64(tb[0]);            /* mulsd xmm4, [rax] */
        double psc1_lo = msc2_lo + hc__f64(HC__SC_1_LO); /* addpd xmm6 */
        double psc1_hi = msc2_hi + hc__f64(HC__SC_1_HI);
        double msc3_lo = psc3_lo * r4;                /* mulpd xmm5, xmm0 */
        double msc3_hi = psc3_hi * r4;
        double res_int = rs + s_hi;                /* addsd xmm3, [rax+8] */
        double corr = negc * negd;           /* mulpd xmm1, xmm7 (lo 레인) */
        double res_hi = med + res_int;                /* addsd xmm4, xmm3 */
        double pols_lo = psc1_lo + msc3_lo;           /* addpd xmm6, xmm5 */
        double pols_hi = psc1_hi + msc3_hi;
        double k0 = s_hi - res_int;   /* movq xmm5,[rax+8]; subsd xmm5 */
        double k1 = res_int - res_hi;                 /* subsd xmm3, xmm4 */
        corr = corr + hc__f64(tb[2]);   /* addsd xmm1, [rax+16] (+S_lo) */
        pols_lo = pols_lo * csr3;            /* mulpd xmm6, xmm2 (lo 레인) */
        pols_hi = pols_hi * shi_r2;                           /* (hi 레인) */
        double acc = rs + k0;                         /* addsd xmm0, xmm5 */
        double k3 = k1 + med;                         /* addsd xmm3, xmm7 */
        acc = acc + corr;                             /* addsd xmm0, xmm1 */
        acc = acc + k3;                               /* addsd xmm0, xmm3 */
        acc = acc + pols_lo;                          /* addsd xmm0, xmm6 */
        acc = acc + pols_hi;               /* unpckhpd; addsd xmm0, xmm6 */
        return acc + res_hi;                          /* addsd xmm0, xmm4 */
    }

    if ((int32_t)ax > 281346048) {
        /* jcc(greater, L1): Payne-Hanek / NaN / Inf — 9a 도메인 밖 */
        abort();
    }

    /* --- §7 소인자 |x| < 2^-252: 1 - |x| (부호 비트만 지운다) --- */
    /* pextrw rax,xmm0,3; andl 32767; pinsrw xmm0,rax,3 */
    double xa = hc__f64(hc__bits(x) & 0x7fffffffffffffffULL);
    return hc__f64(HC__ONE) - xa; /* movq xmm1, ONE; subsd xmm1, xmm0 */
}
