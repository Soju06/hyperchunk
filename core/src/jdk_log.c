#include "hc_jdk_log.h"

#include <stdint.h>
#include <string.h>

#ifndef __x86_64__
#error "hc_jdk_log: rcpps 는 벤더 의존 근사 — x86-64 전용 이식"
#endif
#include <emmintrin.h> /* rcpps 실행용 (아래 hc__rcpps 주석) */

/* JDK HotSpot x86-64 dlog 스텁 (Intel LIBM) 전사 — jdk25u
 * stubGenerator_x86_64_log.cpp. 알고리즘 (스텁 헤더 주석):
 *   x = 2^k·mx, mx ∈ [1,2). rcpss 근사 B0 ≈ 1/mx 를
 *   B = int(B0·2^7+0.5)/2^7 로 반올림, r = B·mx − 1 을 hi/lo 로 정확 계산.
 *   결과 = k·log(2) − log(B) + p(r), p 는 7차 다항, −log(B) 는 L_tbl
 *   에서 hi/lo 파트로 읽어 hi+lo 보상합산으로 재구성한다.
 * 전사 규칙: SSE 스칼라/레인 연산 1개 = C double 연산 1개, 명령 순서
 * 그대로 (빌드는 -ffp-contract=off — a*b+c 융합 없음). 대수 단순화 금지.
 * packed 레지스터는 레인별 변수 2개로 모델링, pand/por 는 비트 연산.
 * rcpps 만 예외 — C 산술로 재현 불가, 같은 명령을 그대로 실행한다. */

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

/* --- stubGenerator_x86_64_log.cpp 의 juint 쌍 (lo,hi) → u64 --- */

#define HC__ONE 0x3ff0000000000000ULL      /* xmm2: 지수 주입(por) / 1.0 */
#define HC__SCALE 0x77f0000000000000ULL    /* xmm3: mx·2^896 지수장 */
#define HC__HIMASK 0xffffe00000000000ULL   /* xmm5/xmm6: 상위 유효비트 */
#define HC__LOG2_HI 0x3fa62e42fefa3800ULL  /* _log2[0] = ln2/16 상위 */
#define HC__LOG2_LO 0x3ceef35793c76730ULL  /* _log2[1] = ln2/16 하위 */
#define HC__C1_LO 0x3fc2492492492492ULL    /* _coeff: 1/7 (r^7) */
#define HC__C1_HI 0xbfd0000000000000ULL    /*         -1/4 (r^4) */
#define HC__C2_LO 0xbfc5555e3d6fb175ULL    /*         ≈-1/6 (r^6) */
#define HC__C2_HI 0x3fd5555555555555ULL    /*         1/3 (r^3) */
#define HC__C3_LO 0x3fc999999999999aULL    /*         1/5 (r^5) */
#define HC__C3_HI 0xbfe0000000000000ULL    /*         -1/2 (r^2) */
#define HC__TWO128 0x47f0000000000000ULL   /* pinsrw 18416: 2^128 재정규화 */
#define HC__PINF 0x7ff0000000000000ULL     /* pinsrw 32752 */
#define HC__NEG1 0xbff0000000000000ULL     /* pinsrw 49136: -1.0 */

/* L_tbl — 행 16바이트 = [-log(B) hi, -log(B) lo]. 인덱스 = 반올림된
 * rcp 비트 16..23 (B ∈ [0.5, 1.0] 의 2^-8 격자) → 129행.
 * _L_tbl 의 juint 그대로. 마지막 행 (B=1.0) 은 {+0.0, -0.0}. */
static const uint64_t HC__LTBL[129][2] = {
    {0x3fe62e42fefa3800ULL, 0x3d2ef35793c76730ULL},
    {0x3fe5ee82aa241800ULL, 0x3d2202380cda46beULL},
    {0x3fe5af405c364800ULL, 0x3d2dfa63ac10c9fbULL},
    {0x3fe5707a26bb8c00ULL, 0x3d09980bff3303ddULL},
    {0x3fe5322e26867800ULL, 0x3d05ccc45d257531ULL},
    {0x3fe4f45a835a5000ULL, 0xbd2e6c516d93b8fbULL},
    {0x3fe4b6fd6f970c00ULL, 0x3cef7115ed4c541cULL},
    {0x3fe47a1527e8a400ULL, 0xbd22cb6af94d60aaULL},
    {0x3fe43d9ff2f92400ULL, 0xbcfd984f481051f7ULL},
    {0x3fe4019c2125cc00ULL, 0xbd26ce7930f0c74cULL},
    {0x3fe3c6080c36c000ULL, 0xbd02b7367cfe13c2ULL},
    {0x3fe38ae217197800ULL, 0xbd218b7abb5569a4ULL},
    {0x3fe35028ad9d8c00ULL, 0x3d10b83f9527e6acULL},
    {0x3fe315da44340800ULL, 0xbd274e93c5a0ed9cULL},
    {0x3fe2dbf557b0e000ULL, 0xbd17a6e507b9dc11ULL},
    {0x3fe2a2786d0ec000ULL, 0x3d206d2be797882dULL},
    {0x3fe269621134dc00ULL, 0xbd0b61f105226250ULL},
    {0x3fe230b0d8bebc00ULL, 0x3d12fc066e48667bULL},
    {0x3fe1f8635fc61800ULL, 0xbd2a7242c9fe81d3ULL},
    {0x3fe1c07849ae6000ULL, 0x3cccacdeed70e667ULL},
    {0x3fe188ee40f23c00ULL, 0x3d14cc4ef8ab4650ULL},
    {0x3fe151c3f6f29800ULL, 0xbd2edd97a293ae49ULL},
    {0x3fe11af823c75c00ULL, 0xbd258647bb9ddcb2ULL},
    {0x3fe0e4898611cc00ULL, 0x3d1c299807801742ULL},
    {0x3fe0ae76e2d05400ULL, 0x3d1f486b887e7e27ULL},
    {0x3fe078bf0533c400ULL, 0x3d26812241edf5fdULL},
    {0x3fe04360be760400ULL, 0xbd04c45fe79539e0ULL},
    {0x3fe00e5ae5b20800ULL, 0xbd053ba3b1727b1cULL},
    {0x3fdfb358af7a4800ULL, 0x3d0085fa3c164935ULL},
    {0x3fdf4aa7ee031800ULL, 0x3d12cde56f014a8bULL},
    {0x3fdee2a156b41000ULL, 0x3d2f27f45a470251ULL},
    {0x3fde7b42c3ddb000ULL, 0xbd2465505372bd08ULL},
    {0x3fde148a1a272800ULL, 0xbd1326b207322938ULL},
    {0x3fddae75484c9800ULL, 0xbd1ea42d60dc616aULL},
    {0x3fdd490246def800ULL, 0x3d235bafe9a767a8ULL},
    {0x3fdce42f18064800ULL, 0xbd0797c33ec7a6b0ULL},
    {0x3fdc7ff9c7455800ULL, 0xbd29b6ddc15249aeULL},
    {0x3fdc1c60693fa000ULL, 0x3d2cec807fe8e180ULL},
    {0x3fdbb9611b80e000ULL, 0x3d27d85bf40a666dULL},
    {0x3fdb56fa04462800ULL, 0x3d1095252d841995ULL},
    {0x3fdaf5295248d000ULL, 0xbd217cc552774458ULL},
    {0x3fda93ed3c8ad800ULL, 0x3d1e36f2bea77a5dULL},
    {0x3fda33440224f800ULL, 0x3d23c6457f9d79f5ULL},
    {0x3fd9d32bea15f000ULL, 0xbd26279e10d0c0b0ULL},
    {0x3fd973a343135800ULL, 0xbd152313a502d9f0ULL},
    {0x3fd914a8635bf800ULL, 0xbd1766b52ee6307dULL},
    {0x3fd8b639a88b3000ULL, 0xbd205ae1e5e70470ULL},
    {0x3fd85855776dc800ULL, 0x3d2fd56f3333778aULL},
    {0x3fd7fafa3bd81800ULL, 0xbd272090c812566aULL},
    {0x3fd79e26687cf800ULL, 0x3d29ec7d2efd1778ULL},
    {0x3fd741d876c67800ULL, 0x3d2d8b0949dc60b3ULL},
    {0x3fd6e60ee6af1800ULL, 0x3d1721657c222d87ULL},
    {0x3fd68ac83e9c6800ULL, 0x3d20a0d32756eba0ULL},
    {0x3fd630030b3ab000ULL, 0xbd2db623e731ae00ULL},
    {0x3fd5d5bddf596000ULL, 0xbd0a0b2a08a465dcULL},
    {0x3fd57bf753c8d000ULL, 0x3d1fadedee5d40efULL},
    {0x3fd522ae0738a000ULL, 0x3d2ebe708164c759ULL},
    {0x3fd4c9e09e173000ULL, 0xbd2e20891b0ad8a4ULL},
    {0x3fd4718dc271c800ULL, 0xbd2f27ce0967d675ULL},
    {0x3fd419b423d5e800ULL, 0x3d08e436ec90e09dULL},
    {0x3fd3c25277333000ULL, 0x3d183b54b606bd5cULL},
    {0x3fd36b6776be1000ULL, 0x3d116ecdb0f177c8ULL},
    {0x3fd314f1e1d36000ULL, 0xbd28e27ad3213cb8ULL},
    {0x3fd2bef07cdc9000ULL, 0x3d2a9cfa4a5004f4ULL},
    {0x3fd269621134d800ULL, 0x3d2c93c1df5bb3b6ULL},
    {0x3fd214456d0eb800ULL, 0x3d0a87deba46baeaULL},
    {0x3fd1bf99635a6800ULL, 0x3d2ca6ed5147bdb7ULL},
    {0x3fd16b5ccbacf800ULL, 0x3d2b9acdf7a51681ULL},
    {0x3fd1178e8227e800ULL, 0xbd2c210e63a5f01cULL},
    {0x3fd0c42d67616000ULL, 0x3d27188b163ceae9ULL},
    {0x3fd07138604d5800ULL, 0x3cf89cdb16ed4e91ULL},
    {0x3fd01eae5626c800ULL, 0xbd16f08c1485e94aULL},
    {0x3fcf991c6cb3b000ULL, 0x3d1bcbecca0cdf30ULL},
    {0x3fcef5ade4dd0000ULL, 0xbcca211565bb8e11ULL},
    {0x3fce530effe71000ULL, 0x3cc212276041f430ULL},
    {0x3fcdb13db0d49000ULL, 0xbd2aff2af715b035ULL},
    {0x3fcd1037f2656000ULL, 0xbd084a7e75b6f6e4ULL},
    {0x3fcc6ffbc6f01000ULL, 0xbcf1ec72c5962bd2ULL},
    {0x3fcbd087383be000ULL, 0xbd2d4bc4595412b6ULL},
    {0x3fcb31d8575bd000ULL, 0xbd0c358d4eace1aaULL},
    {0x3fca93ed3c8ae000ULL, 0xbd28724350562169ULL},
    {0x3fc9f6c407089000ULL, 0x3d29904d6865817aULL},
    {0x3fc95a5adcf70000ULL, 0x3d07f22858a0ff6fULL},
    {0x3fc8beafeb390000ULL, 0xbd073d54aae92cd1ULL},
    {0x3fc823c16551a000ULL, 0x3d1e0ddb9a631e83ULL},
    {0x3fc7898d85445000ULL, 0xbd1c661070914305ULL},
    {0x3fc6f0128b757000ULL, 0xbd25118de59c21e1ULL},
    {0x3fc6574ebe8c1000ULL, 0x3d19cf8b2c3c2e78ULL},
    {0x3fc5bf406b544000ULL, 0xbd127023eb68981cULL},
    {0x3fc527e5e4a1b000ULL, 0x3d2633e8e5697dc7ULL},
    {0x3fc4913d8333b000ULL, 0x3d25837954fdb678ULL},
    {0x3fc3fb45a5993000ULL, 0xbd2cd1d87e6a354dULL},
    {0x3fc365fcb0159000ULL, 0x3cc62fa8234b7289ULL},
    {0x3fc2d1610c868000ULL, 0x3d039d6ccb81b4a1ULL},
    {0x3fc23d712a49c000ULL, 0x3d100d238fd3df5cULL},
    {0x3fc1aa2b7e23f000ULL, 0x3d2ca78e44389934ULL},
    {0x3fc1178e8227e000ULL, 0x3d21ef78ce2d07f2ULL},
    {0x3fc08598b59e4000ULL, 0xbd27e5dd7009902cULL},
    {0x3fbfe89139dbe000ULL, 0xbd2534d64fa10afdULL},
    {0x3fbec739830a2000ULL, 0xbd2dc068afe645e0ULL},
    {0x3fbda72763844000ULL, 0x3d1a89401fa71733ULL},
    {0x3fbc885801bc4000ULL, 0x3d2646d1c65aacd3ULL},
    {0x3fbb6ac88dad6000ULL, 0xbd1390802bf768e5ULL},
    {0x3fba4e7640b1c000ULL, 0xbd0e42b6b94407c8ULL},
    {0x3fb9335e5d594000ULL, 0x3d23115c3abd47daULL},
    {0x3fb8197e2f40e000ULL, 0x3d0f80dcf96ffdf7ULL},
    {0x3fb700d30aeac000ULL, 0x3cec1e8da99ded32ULL},
    {0x3fb5e95a4d97a000ULL, 0xbd2c69063c5d1d1eULL},
    {0x3fb4d3115d208000ULL, 0xbcf53a2582f4e1efULL},
    {0x3fb3bdf5a7d1e000ULL, 0x3d2cc85ea5db4ed7ULL},
    {0x3fb2aa04a4472000ULL, 0xbd20b6e8ae9c697dULL},
    {0x3fb1973bd1466000ULL, 0xbd25325d560d9e9bULL},
    {0x3fb08598b59e4000ULL, 0xbd17e5dd7009902cULL},
    {0x3faeea31c006c000ULL, 0xbd0e113e4fc93b7bULL},
    {0x3faccb73cdddc000ULL, 0xbd1a68f247d82807ULL},
    {0x3faaaef2d0fb0000ULL, 0x3d20fc1a353bb42eULL},
    {0x3fa894aa149fc000ULL, 0xbd197995d05a267dULL},
    {0x3fa67c94f2d4c000ULL, 0xbd029efbec19afa2ULL},
    {0x3fa466aed42e0000ULL, 0xbd2c167375bdfd28ULL},
    {0x3fa252f32f8d0000ULL, 0x3d283e9ae021b67bULL},
    {0x3fa0415d89e74000ULL, 0x3d0111c05cf1d753ULL},
    {0x3f9c63d2ec148000ULL, 0x3d2578c63f9eb2f3ULL},
    {0x3f98492528c90000ULL, 0xbd2aa0ba325a0c34ULL},
    {0x3f9432a925980000ULL, 0x3d098139928637feULL},
    {0x3f90205658938000ULL, 0xbd23dc5b06e2f7d2ULL},
    {0x3f882448a3890000ULL, 0xbd275577da74f640ULL},
    {0x3f80101575890000ULL, 0xbd10c76b999d2be8ULL},
    {0x3f70080559580000ULL, 0x3d2166afcb31c67bULL},
    {0x0000000000000000ULL, 0x8000000000000000ULL}
};

/* rcpps xmm0, xmm0 — 근사 역수 (상대오차 ≤ 1.5·2^-12) 는 벤더/마이크로
 * 아키텍처 의존 테이블 (AMD Zen ≠ Intel) 이라 C 산술로 재현 불가. 스텁과
 * 같은 기계에서 같은 명령을 그대로 실행한다 — 골든 (이 기계의 JDK 캡처)
 * 과의 일치는 그래서 성립한다. 레인 2,3 은 스텁에선 호출자 쓰레기가
 * 흐르지만 pand (xmm6 hi 레인 0) 으로 소거돼 결과 무관 — 0 으로 둔다.
 * 서브노멀 입력 (레인1 은 항상 서브노멀 float) 은 rcpps 아키텍처 규정상
 * DAZ 와 무관하게 0 취급 → +Inf. */
static inline void hc__rcpps(uint32_t d0, uint32_t d1, uint32_t *r0,
                             uint32_t *r1) {
    __m128i vi = _mm_set_epi32(0, 0, (int32_t)d1, (int32_t)d0);
    __m128i vo = _mm_castps_si128(_mm_rcp_ps(_mm_castsi128_ps(vi)));
    *r0 = (uint32_t)_mm_cvtsi128_si32(vo);
    *r1 = (uint32_t)_mm_cvtsi128_si32(_mm_shuffle_epi32(vo, 0x55));
}

/* 프롤로그 공통 비트 연산 + 메인 패스 (L_2TAG_PACKET_1_0_2).
 * xb = (재정규화된) x 비트, rax = pextrw16-16 (메인 진입) / pextrw16
 * (서브노멀 재진입 — subl 16 없음), rcx = 지수 바이어스 16352 / 18416. */
static double hc__log_main(uint64_t xb, int32_t rax, int32_t rcx) {
    uint64_t t = xb | HC__ONE;              /* por xmm0, xmm2 */
    uint64_t q = t >> 27;                   /* psrlq xmm0, 27 */
    uint32_t d0 = (uint32_t)q >> 2;         /* psrld xmm0, 2 (dw0) */
    uint32_t d1 = (uint32_t)(q >> 32) >> 2; /*               (dw1) */
    uint32_t r0, r1;
    hc__rcpps(d0, d1, &r0, &r1);            /* rcpps xmm0, xmm0 */
    uint64_t mant = (xb << 12) >> 12;       /* psllq 12; psrlq 12 (가수) */

    /* --- L1 --- */
    uint32_t dw0 = r0 + 32768u;             /* paddd xmm0, xmm4 (dw0 만) */
    mant |= HC__SCALE;                      /* por xmm1, xmm3 (= mx·2^896) */
    uint32_t rdx = dw0;                     /* movdl rdx, xmm0 */
    /* psllq xmm0, 29 — lo 레인 64비트 = dw0 | dw1<<32; dw1 저비트 3개가
     * B 비트 61..63 으로 올라온다 (rcp(서브노멀)=+Inf 라 실제 0) */
    uint64_t bl = ((uint64_t)dw0 | ((uint64_t)r1 << 32)) << 29;
    uint64_t hib = HC__HIMASK & mant;       /* pand xmm5, xmm1 */
    uint64_t bb = bl & HC__HIMASK;          /* pand xmm0, xmm6 (hi 레인 소거) */
    double xlo = hc__f64(mant) - hc__f64(hib); /* subsd xmm1, xmm5 */
    double bhi = hc__f64(hib) * hc__f64(bb);   /* mulpd xmm5, xmm0 (lo; hi 0·0) */
    rax &= 32752;                           /* andl rax, 32752 */
    rax -= rcx;                             /* subl rax, rcx (= 16·k) */
    double k16 = (double)rax;               /* cvtsi2sdl xmm7, rax */
    xlo = xlo * hc__f64(bb);                /* mulsd xmm1, xmm0 */
    /* movq xmm6, log2; movdqu xmm3, coeff */
    double bm1 = bhi - hc__f64(HC__ONE);    /* subsd xmm5, xmm2 (-1.0) */
    rdx &= 16711680u;                       /* andl rdx, 0xff0000 */
    rdx >>= 12;                             /* shrl rdx, 12 (바이트 오프셋) */
    const uint64_t *tb = HC__LTBL[rdx >> 4];   /* movdqu xmm0, [r8+rdx] */
    /* movdqu xmm4, coeff+16 */
    double r = xlo + bm1;                   /* addsd xmm1, xmm5 (= B·mx−1) */
    /* movdqu xmm2, coeff+32 */
    double kl2hi = hc__f64(HC__LOG2_HI) * k16; /* mulsd xmm6, xmm7 */
    /* movddup xmm5, xmm1 — 양 레인 = r */
    double kl2lo = k16 * hc__f64(HC__LOG2_LO); /* mulsd xmm7, [log2+8] */
    double p7 = hc__f64(HC__C1_LO) * r;     /* mulsd xmm3, xmm1 (hi=-1/4 유지) */
    double hi0 = hc__f64(tb[0]) + kl2hi;    /* addsd xmm0, xmm6 */
    double pa_lo = hc__f64(HC__C2_LO) * r;  /* mulpd xmm4, xmm5 (lo) */
    double pa_hi = hc__f64(HC__C2_HI) * r;  /*                  (hi) */
    double r2_lo = r * r;                   /* mulpd xmm5, xmm5 (lo) */
    double r2_hi = r * r;                   /*                  (hi) */
    /* movddup xmm6, xmm0 — hi0 */
    double hi1 = hi0 + r;                   /* addsd xmm0, xmm1 */
    pa_lo = pa_lo + hc__f64(HC__C3_LO);     /* addpd xmm4, xmm2 (lo) */
    pa_hi = pa_hi + hc__f64(HC__C3_HI);     /*                  (hi) */
    double pb_lo = p7 * r2_lo;              /* mulpd xmm3, xmm5 (lo) */
    double pb_hi = hc__f64(HC__C1_HI) * r2_hi; /*               (hi) */
    double e0 = hi0 - hi1;                  /* subsd xmm6, xmm0 */
    double pa = pa_lo * r;                  /* mulsd xmm4, xmm1 */
    /* pshufd xmm2, xmm0, 238 — xmm0.hi = 테이블 lo 파트 tb[1] 그대로 */
    double rl = r + e0;                     /* addsd xmm1, xmm6 */
    double r4 = r2_lo * r2_lo;              /* mulsd xmm5, xmm5 (hi=r2 유지) */
    double c7 = kl2lo + hc__f64(tb[1]);     /* addsd xmm7, xmm2 */
    double s_lo = pa + pb_lo;               /* addpd xmm4, xmm3 (lo) */
    double s_hi = pa_hi + pb_hi;            /*                  (hi) */
    rl = rl + c7;                           /* addsd xmm1, xmm7 */
    double w_lo = s_lo * r4;                /* mulpd xmm4, xmm5 (lo) */
    double w_hi = s_hi * r2_hi;             /*                  (hi) */
    rl = rl + w_lo;                         /* addsd xmm1, xmm4 */
    /* pshufd xmm5, xmm4, 238 — w_hi */
    rl = rl + w_hi;                         /* addsd xmm1, xmm5 */
    return hi1 + rl;                        /* addsd xmm0, xmm1 */
}

/* 분기 선별 (프롤로그): rax = pextrw(x,3) - 16; cmp 32736
 *   unsigned <  : 메인 패스 (양수 정규수 — 지수장 ≥ 1, < 0x7ff)
 *   그 외       : L_2TAG_PACKET_0_0_2 특수 경로 (rax += 16 복원 후
 *                 부호/Inf/NaN/±0/서브노멀 선별) */

double hc_jdk_log(double x) {
    const uint64_t xb = hc__bits(x);
    const uint32_t pex = (uint32_t)(xb >> 48) & 0xffffu; /* pextrw rax,xmm0,3 */

    if ((uint32_t)((int32_t)pex - 16) < 32736u) /* subl 16; cmpl 32736 */
        return hc__log_main(xb, (int32_t)pex - 16, 16352);

    /* --- L0 특수 경로 (addl rax, 16 — pex 로 복원됨) --- */
    if (pex >= 32768u) {                    /* cmpl 32768; jcc ae — 부호 비트 */
        /* L2: movdl rdx(lo)/rcx(hi); addl rcx, rcx (부호 탈락) */
        const uint32_t lo = (uint32_t)xb;
        const uint32_t hi2 = (uint32_t)(xb >> 32) * 2u;
        if (hi2 >= 0xffe00000u) {           /* cmpl -2097152; jcc ae — L5 */
            if (hi2 > 0xffe00000u)          /* jcc above L4: 음수 NaN */
                return x + x;               /* addsd xmm0, xmm0 (quiet) */
            if (lo != 0u)                   /* cmpl rdx,0; jcc above L4 */
                return x + x;
            /* -Inf → L6 낙하 */
        } else if ((lo | hi2) == 0u) {      /* orl; jcc equal — -0.0 */
            goto L7;
        }
        /* L6: 음수/−Inf → 0·(+Inf) = QNaN indefinite (0xfff8…).
         * volatile — GCC 가 0·Inf 를 +QNaN 으로 상수 접는 것을 막고
         * 실제 mulsd 를 강제한다 (스텁과 동일 비트) */
        {
            volatile double z = 0.0;        /* xorpd xmm0, xmm0 */
            return z * hc__f64(HC__PINF);   /* pinsrw 32752; mulsd */
        }
    }
    if (pex < 16u) {                        /* cmpl 16; jcc below — L3 */
        /* L3: +0 또는 상위 니블 0 서브노멀. xorpd+addsd (0+x) 는 양수에서
         * 비트 항등 — dword OR 0 검사만 전사한다 */
        if (((uint32_t)xb | (uint32_t)(xb >> 32)) == 0u)
            goto L7;                        /* +0.0 → -Inf */
        /* 서브노멀 재정규화: ×2^128 (정확) 후 rcx=18416 로 메인 재진입.
         * pextrw 는 재정규화된 값에서 새로 뜨고 subl 16 이 없다 */
        double xr = x * hc__f64(HC__TWO128); /* pinsrw 18416; mulsd */
        uint64_t xrb = hc__bits(xr);
        return hc__log_main(xrb, (int32_t)((uint32_t)(xrb >> 48) & 0xffffu),
                            18416);
    }
    /* L4: +Inf / +NaN (pex ∈ [32752, 32767]) */
    return x + x;                           /* addsd xmm0, xmm0 */

L7: /* ±0.0 → -1.0 / +0.0 = -Inf (L_2TAG_PACKET_7_0_2) */
    {
        volatile double z = 0.0;            /* xorpd xmm1, xmm1 */
        return hc__f64(HC__NEG1) / z;       /* pinsrw 49136; divsd */
    }
}
