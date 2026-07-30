#include "hc_rng.h"

/* 두 RNG 모두 golden/rng/ 의 26.2 실측 벡터로 전 구간 검증된다
 * (tests/unit/test_rng.c). 알고리즘은 공개 문서 + golden 대조로 확보했다
 * (ADR-002 R4: 디컴파일 코드 복사 금지). */

/* --- Xoroshiro128++ (1.18+ worldgen, XoroshiroRandomSource) --- */

static uint64_t rotl64(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

/* RandomSupport.mixStafford13 — SplitMix64 finalizer 의 Stafford 변형 13 */
static uint64_t mix_stafford13(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

#define HC_GOLDEN_RATIO_64 0x9E3779B97F4A7C15ULL
#define HC_SILVER_RATIO_64 0x6A09E667F3BCC909ULL

void hc_xoro_init(hc_xoro_t *r, int64_t seed) {
    /* RandomSupport.upgradeSeedTo128bit:
     * lo = mix(seed ^ SILVER), hi = mix(lo_unmixed + GOLDEN) */
    uint64_t lo = (uint64_t)seed ^ HC_SILVER_RATIO_64;
    uint64_t hi = lo + HC_GOLDEN_RATIO_64;
    r->lo = mix_stafford13(lo);
    r->hi = mix_stafford13(hi);
    /* 128비트 all-zero 는 축퇴 상태 — 바닐라도 같은 상수로 회피한다 */
    if ((r->lo | r->hi) == 0) {
        r->lo = HC_GOLDEN_RATIO_64;
        r->hi = HC_SILVER_RATIO_64;
    }
}

uint64_t hc_xoro_next(hc_xoro_t *r) {
    uint64_t s0 = r->lo, s1 = r->hi;
    uint64_t res = rotl64(s0 + s1, 17) + s0; /* ++ scrambler */
    s1 ^= s0;
    r->lo = rotl64(s0, 49) ^ s1 ^ (s1 << 21);
    r->hi = rotl64(s1, 28);
    return res;
}

int32_t hc_xoro_next_int(hc_xoro_t *r, int32_t bound) {
    /* XoroshiroRandomSource.nextInt(bound): nextLong 의 '하위' 32비트를
     * 무부호로 취해 곱셈-거절한다 ((int)nextLong 캐스트 의미).
     * 상위 32비트를 쓰면 golden nextInt(256)=36 이 201 로 어긋난다. */
    uint32_t b = (uint32_t)bound;
    uint32_t v = (uint32_t)hc_xoro_next(r);
    uint64_t m = (uint64_t)v * (uint64_t)b;
    uint32_t frac = (uint32_t)m;
    if (frac < b) {
        uint32_t t = (0u - b) % b; /* (2^32 - b) mod b: 편향 거절 문턱 */
        while (frac < t) {
            v = (uint32_t)hc_xoro_next(r);
            m = (uint64_t)v * (uint64_t)b;
            frac = (uint32_t)m;
        }
    }
    return (int32_t)(m >> 32);
}

double hc_xoro_next_double(hc_xoro_t *r) {
    /* nextBits(53) * 2^-53. 53비트 정수의 double 변환은 정확하고
     * 2^-53 곱은 지수 스케일링뿐이라 반올림이 발생하지 않는다. */
    return (double)(hc_xoro_next(r) >> 11) * 0x1.0p-53;
}

float hc_xoro_next_float(hc_xoro_t *r) {
    /* nextBits(24) * 5.9604645E-8f (= 2^-24). 24비트 정수의 float 변환과
     * 2의 거듭제곱 스케일 곱은 둘 다 정확하다. */
    return (float)(hc_xoro_next(r) >> 40) * 0x1.0p-24f;
}

/* --- positional fork (XoroshiroPositionalRandomFactory) --- */

#include "hc_md5.h"

#include <string.h>

/* raw (lo,hi) 생성자: 재믹싱 없음, all-zero 가드만 (Xoroshiro128PlusPlus
 * (long,long) 생성자와 동일) */
static void xoro_init_raw(hc_xoro_t *r, uint64_t lo, uint64_t hi) {
    r->lo = lo;
    r->hi = hi;
    if ((r->lo | r->hi) == 0) {
        r->lo = HC_GOLDEN_RATIO_64;
        r->hi = HC_SILVER_RATIO_64;
    }
}

void hc_xoro_fork(hc_xoro_t *r, hc_xoro_t *out) {
    uint64_t lo = hc_xoro_next(r); /* 첫 draw 가 lo */
    uint64_t hi = hc_xoro_next(r);
    xoro_init_raw(out, lo, hi);
}

void hc_xoro_fork_positional(hc_xoro_t *r, hc_xoro_fork_t *f) {
    f->lo = hc_xoro_next(r);
    f->hi = hc_xoro_next(r);
}

void hc_xoro_from_hash_of(const hc_xoro_fork_t *f, const char *s,
                          hc_xoro_t *out) {
    /* RandomSupport.seedFromHashOf: md5(UTF-8) 를 빅엔디언으로
     * (b[0] 최상위) long 2개로 읽고 factory 시드와 XOR */
    uint8_t d[16];
    hc_md5(s, strlen(s), d);
    uint64_t hlo = 0, hhi = 0;
    for (int i = 0; i < 8; i++) {
        hlo = (hlo << 8) | d[i];
        hhi = (hhi << 8) | d[8 + i];
    }
    xoro_init_raw(out, hlo ^ f->lo, hhi ^ f->hi);
}

int64_t hc_mth_get_seed(int32_t x, int32_t y, int32_t z) {
    /* x*3129871 은 int 곱으로 래핑된 '후' 부호확장, z 는 long 곱, y 는
     * 그대로 XOR. 이어 l*l*42317861 + l*11 (래핑) 후 산술 >>16.
     * signed 오버플로는 C 에서 UB 이므로 전부 무부호로 계산해 재해석한다. */
    uint64_t l = (uint64_t)(int64_t)(int32_t)((uint32_t)x * 3129871u);
    l ^= (uint64_t)((int64_t)z * 116129781LL);
    l ^= (uint64_t)(int64_t)y;
    l = l * l * 42317861u + l * 11u;
    return (int64_t)l >> 16;
}

void hc_xoro_at(const hc_xoro_fork_t *f, int32_t x, int32_t y, int32_t z,
                hc_xoro_t *out) {
    /* hi 는 XOR 하지 않는다 — fromHashOf 와 다른 점 */
    xoro_init_raw(out, (uint64_t)hc_mth_get_seed(x, y, z) ^ f->lo, f->hi);
}

/* --- Legacy 48-bit LCG (java.util.Random == LegacyRandomSource) --- */

#define HC_LCG_MUL  0x5DEECE66DULL
#define HC_LCG_ADD  0xBULL
#define HC_LCG_MASK ((1ULL << 48) - 1)

void hc_lcg_init(hc_lcg_t *r, int64_t seed) {
    r->s = ((uint64_t)seed ^ HC_LCG_MUL) & HC_LCG_MASK;
}

int32_t hc_lcg_next(hc_lcg_t *r, int bits) {
    r->s = (r->s * HC_LCG_MUL + HC_LCG_ADD) & HC_LCG_MASK;
    return (int32_t)(r->s >> (48 - bits));
}

int64_t hc_lcg_next_long(hc_lcg_t *r) {
    /* ((long)next(32) << 32) + next(32) — 하위 절반이 부호확장되어
     * '더해진다'(OR 아님). 합은 무부호로 계산해 랩어라운드를 정의한다. */
    uint64_t hi = (uint64_t)(uint32_t)hc_lcg_next(r, 32) << 32;
    uint64_t lo = (uint64_t)(int64_t)hc_lcg_next(r, 32);
    return (int64_t)(hi + lo);
}

int32_t hc_lcg_next_int(hc_lcg_t *r, int32_t bound) {
    int32_t m = bound - 1;
    if ((bound & m) == 0) /* 2의 거듭제곱 경로: (bound * next(31)) >> 31 */
        return (int32_t)(((int64_t)bound * (int64_t)hc_lcg_next(r, 31)) >> 31);
    int32_t u = hc_lcg_next(r, 31);
    int32_t val = u % bound;
    /* Java 는 u - val + m 의 int 오버플로(<0)로 편향 구간을 판정한다.
     * C 의 signed 오버플로는 UB 이므로 무부호로 계산해 재해석한다. */
    while ((int32_t)((uint32_t)u - (uint32_t)val + (uint32_t)m) < 0) {
        u = hc_lcg_next(r, 31);
        val = u % bound;
    }
    return val;
}

double hc_lcg_next_double(hc_lcg_t *r) {
    /* ((long)next(26) << 27) + next(27), 스케일 2^-53 */
    int64_t hi = (int64_t)hc_lcg_next(r, 26) << 27;
    int64_t lo = (int64_t)hc_lcg_next(r, 27);
    return (double)(hi + lo) * 0x1.0p-53;
}

float hc_lcg_next_float(hc_lcg_t *r) {
    /* (float)next(24) * 5.9604645E-8f — i2f; fmul, 전부 float 연산.
     * 상수는 정확히 2^-24 (비트 0x33800000, A7 §3.3). */
    return (float)hc_lcg_next(r, 24) * 0x1.0p-24f;
}
