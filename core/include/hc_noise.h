#ifndef HC_NOISE_H
#define HC_NOISE_H

#include <stdint.h>

#include "hc_arena.h"
#include "hc_rng.h"

/* 바닐라 26.2 노이즈 스택 (synth 패키지) 의 스칼라 재구현.
 * 시맨틱은 비난독화 클래스를 javap 로 확인해 옮겼고 (ADR-002 R4),
 * golden/rng/{perlin,fork,octaves,router}_seed*.txt 로 비트단위 검증된다.
 *
 * 계층:  ImprovedNoise (hc_perlin_t)
 *      → PerlinNoise   (hc_octaves_t, 멀티옥타브 + fork-by-string 시딩)
 *      → NormalNoise   (hc_normal_noise_t, first/second 쌍)
 *      → BlendedNoise  (hc_blended_noise_t, 레거시 3중 옥타브) */

/* --- ImprovedNoise ---
 * 바닐라는 perm 을 byte[256] 로 두고 p(i) = p[i & 255] & 255 로 조회
 * 시점에 마스킹한다 — 교과서식 512 복제 테이블이 아니며, floor 좌표도
 * 마스킹 없이 그대로 들어간다. 여기서도 같은 구조를 쓴다. */
typedef struct {
    double  xo, yo, zo;
    uint8_t perm[256];
} hc_perlin_t;

/* 시딩: 같은 Xoroshiro 인스턴스에서 xo, yo, zo (nextDouble()*256) 를
 * 순서대로 소비한 뒤, i=0..255 에 대해 nextInt(256-i) 로 perm 을 셔플한다.
 * 이 소비 순서가 어긋나면 이후 모든 노이즈가 어긋난다 (ADR-002 Pitfall 2). */
void   hc_perlin_init(hc_perlin_t *p, int64_t seed);
void   hc_perlin_init_from(hc_perlin_t *p, hc_xoro_t *r); /* 주어진 RNG 소비 */
/* LegacyRandomSource(LCG) 플레이버 — geode 내부 노이즈 (Task 10 R5a §4.2):
 * 소비 순서는 xoro 판과 동일 (xo,yo,zo 그리고 Fisher-Yates). */
void   hc_perlin_init_from_lcg(hc_perlin_t *p, hc_lcg_t *r);
/* ImprovedNoise.noise(x,y,z) == noise(x,y,z,0,0): yScale=0 경로 */
double hc_perlin_sample(const hc_perlin_t *p, double x, double y, double z);
/* noise(x,y,z,yScale,yMax): yScale != 0 이면 y 분수를 yScale 격자로 양자화
 * (+float 1e-7 승격 상수) 하되 smoothstep fade 는 원래 분수를 쓴다 */
double hc_perlin_sample_scaled(const hc_perlin_t *p, double x, double y,
                               double z, double yscale, double ymax);

/* --- PerlinNoise (멀티옥타브) --- */
typedef struct {
    hc_perlin_t **octaves; /* count 개, 진폭 0 옥타브는 NULL */
    double       *amplitudes;
    int32_t       count;
    int32_t       first_octave;
    double        lowest_freq_input; /* 2^first_octave */
    double        lowest_freq_value; /* 2^(count-1) / (2^count - 1) */
} hc_octaves_t;

/* MODERN 초기화 (26.2 데이터팩 노이즈 전부):
 * rand.forkPositional() 로 nextLong 2회 소비 후, 진폭 != 0.0 인 옥타브만
 * fromHashOf("octave_<n>") 으로 시딩한다. 진폭 0 은 아무것도 소비 안 함. */
int hc_octaves_init(hc_octaves_t *o, hc_arena_t *a, hc_xoro_t *rand,
                    int32_t first_octave, const double *amps, int32_t count);

/* LEGACY 초기화 (BlendedNoise 전용): 인덱스 j = -first_octave (옥타브 0)
 * 의 ImprovedNoise 를 저장 여부와 무관하게 항상 먼저 생성(=소비)하고,
 * j-1..0 (옥타브 -1..first_octave) 을 내림차순으로 하나의 RNG 에서 '순차'
 * 소비한다. 내림차순 구간에서 진폭 0 인 자리는 262 draw 를 건너뛴다.
 * j < count-1 (양수 옥타브) 는 바닐라가 throw 하므로 -1 을 돌려준다. */
int hc_octaves_init_legacy(hc_octaves_t *o, hc_arena_t *a, hc_xoro_t *rand,
                           int32_t first_octave, const double *amps,
                           int32_t count);

/* PerlinNoise.getValue(x,y,z[,yScale,yMax]) */
double hc_octaves_value(const hc_octaves_t *o, double x, double y, double z,
                        double yscale, double ymax);

/* PerlinNoise.wrap: d - lfloor(d/2^25 + 0.5)*2^25 */
double hc_octaves_wrap(double d);

/* --- NormalNoise --- */
typedef struct {
    hc_octaves_t first, second;
    double       value_factor;
} hc_normal_noise_t;

/* first/second 를 같은 rand 에서 순차로 초기화한다 (forkPositional 2회
 * == nextLong 4회). value_factor = (1/6) / expectedDeviation(maxIdx-minIdx),
 * expectedDeviation(n) = 0.1 * (1 + 1/(n+1)), 인덱스는 진폭 != 0 범위. */
int hc_normal_noise_init(hc_normal_noise_t *n, hc_arena_t *a, hc_xoro_t *rand,
                         int32_t first_octave, const double *amps,
                         int32_t count);

double hc_normal_noise_value(const hc_normal_noise_t *n, double x, double y,
                             double z);

/* --- BlendedNoise (old_blended_noise, 레거시 지형 노이즈) --- */
typedef struct {
    hc_octaves_t min_limit;  /* 옥타브 -15..0, 16개 */
    hc_octaves_t max_limit;  /* 옥타브 -15..0, 16개 */
    hc_octaves_t main_noise; /* 옥타브 -7..0, 8개 */
    double       xz_mult, y_mult; /* 684.412 * scale */
    double       xz_factor, y_factor, smear; /* JSON 파라미터 */
} hc_blended_noise_t;

/* rand 에서 min_limit(16) → max_limit(16) → main(8) 순으로 레거시 소비.
 * 26.2 는 fromHashOf("minecraft:terrain") 으로 rand 를 만든다 (RandomState). */
int hc_blended_init(hc_blended_noise_t *b, hc_arena_t *a, hc_xoro_t *rand,
                    double xz_scale, double y_scale, double xz_factor,
                    double y_factor, double smear);

/* BlendedNoise.compute — 블록 좌표는 int (FunctionContext) */
double hc_blended_compute(const hc_blended_noise_t *b, int32_t x, int32_t y,
                          int32_t z);

#endif /* HC_NOISE_H */
