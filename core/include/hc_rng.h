#ifndef HC_RNG_H
#define HC_RNG_H

#include <stdint.h>

/* 바닐라의 두 RNG 를 비트단위로 재현한다 (ADR-002 Pitfall 2).
 *
 * - Xoroshiro128++ : 1.18+ worldgen. XoroshiroRandomSource(seed) ==
 *   Xoroshiro128PlusPlus(upgradeSeedTo128bit(seed)), 시딩은 mixStafford13.
 * - 48-bit LCG     : java.util.Random. LegacyRandomSource 와 스트림이
 *   동일함은 golden 의 legacyRandomSource_crosscheck 섹션이 증명한다.
 *
 * 이 헤더는 코어 내부용이다. 공개 ABI 는 hyperchunk.h 의 리전 단위
 * 표면뿐이며 노드/RNG 단위 진입점은 거기 올라가지 않는다 (ADR-003 D2). */

typedef struct { uint64_t lo, hi; } hc_xoro_t;

void     hc_xoro_init(hc_xoro_t *r, int64_t seed);
uint64_t hc_xoro_next(hc_xoro_t *r);                    /* nextLong */
int32_t  hc_xoro_next_int(hc_xoro_t *r, int32_t bound); /* nextInt(bound), bound > 0 */
double   hc_xoro_next_double(hc_xoro_t *r);
float    hc_xoro_next_float(hc_xoro_t *r);

/* --- positional fork (XoroshiroPositionalRandomFactory) ---
 *
 * 바닐라 26.2 바이트코드 확인 사항 (비트정확 필수 지점):
 *  - forkPositional()/fork() 는 nextLong 을 정확히 2회 소비한다
 *    (첫 번째가 lo, 두 번째가 hi). 이때 mixStafford13 재믹싱은 없고
 *    all-zero 가드만 적용된다.
 *  - fromHashOf(s) 는 md5(UTF-8(s)) 16바이트를 빅엔디언 long 2개로 읽어
 *    (해시가 상위) factory 시드와 XOR 한다: (h_lo^lo, h_hi^hi).
 *  - at(x,y,z) 는 Mth.getSeed 를 lo 에만 XOR 한다: (seed^lo, hi).
 * 검증: golden/rng/fork_seed*.txt (tests/unit/test_octave.c). */
typedef struct { uint64_t lo, hi; } hc_xoro_fork_t;

void hc_xoro_fork(hc_xoro_t *r, hc_xoro_t *out);              /* RandomSource.fork() */
void hc_xoro_fork_positional(hc_xoro_t *r, hc_xoro_fork_t *f);
void hc_xoro_from_hash_of(const hc_xoro_fork_t *f, const char *s, hc_xoro_t *out);
void hc_xoro_at(const hc_xoro_fork_t *f, int32_t x, int32_t y, int32_t z,
                hc_xoro_t *out);

/* Mth.getSeed(x,y,z) — x*3129871 은 int32 로 래핑 후 부호확장된다 */
int64_t hc_mth_get_seed(int32_t x, int32_t y, int32_t z);

typedef struct { uint64_t s; } hc_lcg_t;

void    hc_lcg_init(hc_lcg_t *r, int64_t seed);
int32_t hc_lcg_next(hc_lcg_t *r, int bits);             /* java.util.Random.next(bits) */
int64_t hc_lcg_next_long(hc_lcg_t *r);
int32_t hc_lcg_next_int(hc_lcg_t *r, int32_t bound);    /* nextInt(bound), bound > 0 */
double  hc_lcg_next_double(hc_lcg_t *r);
float   hc_lcg_next_float(hc_lcg_t *r);                 /* next(24) * 2^-24f */

#endif /* HC_RNG_H */
