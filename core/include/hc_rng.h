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

typedef struct { uint64_t s; } hc_lcg_t;

void    hc_lcg_init(hc_lcg_t *r, int64_t seed);
int32_t hc_lcg_next(hc_lcg_t *r, int bits);             /* java.util.Random.next(bits) */
int64_t hc_lcg_next_long(hc_lcg_t *r);
int32_t hc_lcg_next_int(hc_lcg_t *r, int32_t bound);    /* nextInt(bound), bound > 0 */
double  hc_lcg_next_double(hc_lcg_t *r);

#endif /* HC_RNG_H */
