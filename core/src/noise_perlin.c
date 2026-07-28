#include "hc_df.h"
#include "hc_rng.h"

#include <math.h>

/* 바닐라 26.2 ImprovedNoise 스칼라 재구현. 시맨틱은 비난독화 클래스
 * (ImprovedNoise / SimplexNoise / Mth) 를 javap 로 확인해 옮겼고
 * (ADR-002 R4: 코드 복사 아님, 알고리즘 이해), 결과는
 * golden/rng/perlin_seed1234567890.txt 로 비트단위 검증된다.
 *
 * FP 연산의 결합 순서를 바닐라와 정확히 같게 유지한다. FMA 접힘은
 * -ffp-contract=off 가 컴파일러 차원에서 막는다 (ADR-004 D3). */

/* SimplexNoise.GRADIENT — gradDot 이 hash & 15 로 16 방향을 선택한다.
 * 12~15 가 0~11 의 중복인 것까지 바닐라 그대로다.
 * ADR-004: 16 double = zmm 2 개, Phase 2 vpermt2pd 대상. */
static const int8_t GRAD[16][3] = {
    { 1,  1,  0}, {-1,  1,  0}, { 1, -1,  0}, {-1, -1,  0},
    { 1,  0,  1}, {-1,  0,  1}, { 1,  0, -1}, {-1,  0, -1},
    { 0,  1,  1}, { 0, -1,  1}, { 0,  1, -1}, { 0, -1, -1},
    { 1,  1,  0}, { 0, -1,  1}, {-1,  1,  0}, { 0, -1, -1},
};

/* Mth.smoothstep: ((t*t)*t) * ((t * (t*6 - 15)) + 10) — 결합 순서 그대로 */
static double smoothstep(double t) {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

/* Mth.lerp: a + t * (b - a) */
static double lerp(double t, double a, double b) {
    return a + t * (b - a);
}

/* ImprovedNoise.p(i) = p[i & 255] & 255. 인덱스를 조회 시점에 마스킹하므로
 * floor 좌표가 음수/255 초과여도 그대로 들어온다 (교과서식 사전 마스킹과
 * 달리 바닐라 구조를 유지한다). */
static int P(const hc_perlin_t *p, int i) {
    return p->perm[i & 255];
}

/* SimplexNoise.dot: ((g0*x) + (g1*y)) + (g2*z) — 좌결합 */
static double grad_dot(const hc_perlin_t *p, int hash,
                       double x, double y, double z) {
    const int8_t *g = GRAD[P(p, hash) & 15];
    return (double)g[0] * x + (double)g[1] * y + (double)g[2] * z;
}

void hc_perlin_init(hc_perlin_t *p, int64_t seed) {
    hc_xoro_t r;
    hc_xoro_init(&r, seed);
    /* 소비 순서 = 바닐라 생성자 순서: xo, yo, zo 먼저 (ADR-002 Pitfall 2) */
    p->xo = hc_xoro_next_double(&r) * 256.0;
    p->yo = hc_xoro_next_double(&r) * 256.0;
    p->zo = hc_xoro_next_double(&r) * 256.0;
    for (int i = 0; i < 256; i++)
        p->perm[i] = (uint8_t)i;
    /* Fisher-Yates 변형: i 오름차순, j = nextInt(256 - i), p[i] <-> p[i+j] */
    for (int i = 0; i < 256; i++) {
        int j = hc_xoro_next_int(&r, 256 - i);
        uint8_t t = p->perm[i];
        p->perm[i] = p->perm[i + j];
        p->perm[i + j] = t;
    }
}

double hc_perlin_sample(const hc_perlin_t *p, double x, double y, double z) {
    /* noise(x,y,z) == noise(x,y,z,0,0): yScale=0 이라 y 양자화가 없고
     * smoothstep 의 y 인자도 같은 fy 다 (sampleAndLerp 의 7번째 인자). */
    double dx = x + p->xo;
    double dy = y + p->yo;
    double dz = z + p->zo;
    /* Mth.floor = (int)Math.floor */
    int ix = (int)floor(dx);
    int iy = (int)floor(dy);
    int iz = (int)floor(dz);
    double fx = dx - (double)ix;
    double fy = dy - (double)iy;
    double fz = dz - (double)iz;

    /* sampleAndLerp 의 해시 체인. iy/iz 는 마스킹 없이 더해진다. */
    int a  = P(p, ix);
    int b  = P(p, ix + 1);
    int aa = P(p, a + iy);
    int ab = P(p, a + iy + 1);
    int ba = P(p, b + iy);
    int bb = P(p, b + iy + 1);

    double v000 = grad_dot(p, aa + iz,     fx,       fy,       fz);
    double v100 = grad_dot(p, ba + iz,     fx - 1.0, fy,       fz);
    double v010 = grad_dot(p, ab + iz,     fx,       fy - 1.0, fz);
    double v110 = grad_dot(p, bb + iz,     fx - 1.0, fy - 1.0, fz);
    double v001 = grad_dot(p, aa + iz + 1, fx,       fy,       fz - 1.0);
    double v101 = grad_dot(p, ba + iz + 1, fx - 1.0, fy,       fz - 1.0);
    double v011 = grad_dot(p, ab + iz + 1, fx,       fy - 1.0, fz - 1.0);
    double v111 = grad_dot(p, bb + iz + 1, fx - 1.0, fy - 1.0, fz - 1.0);

    double sx = smoothstep(fx);
    double sy = smoothstep(fy);
    double sz = smoothstep(fz);

    /* Mth.lerp3: x 최내측, y 중간, z 최외측 */
    double l0 = lerp(sy, lerp(sx, v000, v100), lerp(sx, v010, v110));
    double l1 = lerp(sy, lerp(sx, v001, v101), lerp(sx, v011, v111));
    return lerp(sz, l0, l1);
}
