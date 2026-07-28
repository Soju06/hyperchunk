/* density function IR 스칼라 평가기 단위 테스트 (Plan Task 5).
 *
 * 손으로 만든 그래프로 각 op 를 찌른다. 노이즈 노드는 golden 값을
 * 여기 복제하지 않는다 — hc_perlin_sample 직접 호출과 비트 동일한지만
 * 본다 (Perlin 자체의 golden 검증은 test_perlin 담당).
 *
 * 상수 op 시맨틱은 바닐라와 같아야 한다 (javap 로 확인):
 * - MUL: Ap2 는 arg1 == 0.0 이면 0.0 (naive a*b 는 0.0 * -x = -0.0)
 * - Y_CLAMPED_GRADIENT: Mth.clampedMap(y, fromY, toY, fromValue, toValue)
 * - MIN/MAX: Math.min/max (±0.0, NaN 처리 포함) */

/* 테스트의 assert 는 빌드 플레이버와 무관하게 항상 살아 있어야 한다 —
 * NDEBUG 빌드에서 assert 가 증발하면 테스트가 공허하게 통과한다. */
#undef NDEBUG

#include "hc_df.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static int bits_eq(double a, double b) {
    uint64_t x, y;
    memcpy(&x, &a, sizeof x);
    memcpy(&y, &b, sizeof y);
    return x == y;
}

int main(void) {
    hc_perlin_t noise;
    hc_perlin_init(&noise, 1234567890LL);

    enum { N_CONST_A, N_CONST_B, N_X, N_Y, N_Z, N_ADD, N_MUL, N_MIN, N_MAX,
           N_CLAMP, N_YGRAD, N_NOISE, N_SPLINE, N_ZERO, N_NEG, N_MULZ,
           N_NEGZ, N_MINZ, N_MAXZ, N_COUNT };
    hc_df_node_t nodes[N_COUNT] = {
        [N_CONST_A] = {.op = HC_DF_CONST, .a = -1, .b = -1, .k0 = 2.5},
        [N_CONST_B] = {.op = HC_DF_CONST, .a = -1, .b = -1, .k0 = 4.0},
        [N_X]       = {.op = HC_DF_X, .a = -1, .b = -1},
        [N_Y]       = {.op = HC_DF_Y, .a = -1, .b = -1},
        [N_Z]       = {.op = HC_DF_Z, .a = -1, .b = -1},
        [N_ADD]     = {.op = HC_DF_ADD, .a = N_CONST_A, .b = N_CONST_B},
        [N_MUL]     = {.op = HC_DF_MUL, .a = N_ADD, .b = N_CONST_A},
        [N_MIN]     = {.op = HC_DF_MIN, .a = N_MUL, .b = N_ADD},
        [N_MAX]     = {.op = HC_DF_MAX, .a = N_MUL, .b = N_ADD},
        /* CLAMP(a, k0=min, k1=max) */
        [N_CLAMP]   = {.op = HC_DF_CLAMP, .a = N_MUL, .b = -1, .k0 = 0.0, .k1 = 10.0},
        /* 오버월드 스타일: fromY=-64, toY=320, fromValue=1.5, toValue=-1.5 */
        [N_YGRAD]   = {.op = HC_DF_Y_CLAMPED_GRADIENT, .a = -1, .b = -1,
                       .k0 = -64.0, .k1 = 320.0, .k2 = 1.5, .k3 = -1.5},
        /* NOISE(noise_id, 좌표 스케일 k0,k1,k2) */
        [N_NOISE]   = {.op = HC_DF_NOISE, .a = -1, .b = -1,
                       .k0 = 1.0, .k1 = 1.0, .k2 = 1.0, .noise_id = 0},
        [N_SPLINE]  = {.op = HC_DF_SPLINE, .a = -1, .b = -1},
        /* 바닐라 MUL 단락: 0.0 * 음수는 -0.0 이 아니라 0.0 이어야 한다 */
        [N_ZERO]    = {.op = HC_DF_CONST, .a = -1, .b = -1, .k0 = 0.0},
        [N_NEG]     = {.op = HC_DF_CONST, .a = -1, .b = -1, .k0 = -5.0},
        [N_MULZ]    = {.op = HC_DF_MUL, .a = N_ZERO, .b = N_NEG},
        /* Math.min/max 의 ±0.0 규칙: min(+0,-0) = -0, max(-0,+0) = +0.
         * N_NEG*N_ZERO = -0.0 (arg1 이 -5 라 단락 없음) 을 재료로 쓴다. */
        [N_NEGZ]    = {.op = HC_DF_MUL, .a = N_NEG, .b = N_ZERO},
        [N_MINZ]    = {.op = HC_DF_MIN, .a = N_ZERO, .b = N_NEGZ},
        [N_MAXZ]    = {.op = HC_DF_MAX, .a = N_NEGZ, .b = N_ZERO},
    };
    hc_df_graph_t g = {
        .nodes = nodes, .n = N_COUNT,
        .noises = &noise, .n_noises = 1,
        .root = N_NOISE,
    };

    double scratch[N_COUNT];
    const double X = 0.5, Y = 128.0, Z = 0.125;
    double root = hc_df_eval(&g, X, Y, Z, scratch);

    /* 좌표 / 상수 / 산술 */
    assert(bits_eq(scratch[N_CONST_A], 2.5));
    assert(bits_eq(scratch[N_X], X));
    assert(bits_eq(scratch[N_Y], Y));
    assert(bits_eq(scratch[N_Z], Z));
    assert(bits_eq(scratch[N_ADD], 6.5));
    assert(bits_eq(scratch[N_MUL], 16.25));
    assert(bits_eq(scratch[N_MIN], 6.5));
    assert(bits_eq(scratch[N_MAX], 16.25));
    assert(bits_eq(scratch[N_CLAMP], 10.0));

    /* y_clamped_gradient: 중앙에서 정확히 0, 클램프 양끝 확인은 아래 재평가 */
    assert(bits_eq(scratch[N_YGRAD], 0.0)); /* t=(128+64)/384=0.5 → 1.5+0.5*(-3) */

    /* 노이즈 노드 == 직접 샘플 (그래프 배관 검증) */
    assert(bits_eq(scratch[N_NOISE], hc_perlin_sample(&noise, X, Y, Z)));
    assert(bits_eq(root, scratch[N_NOISE]));

    /* 미구현 SPLINE 은 0.0 (Plan Task 6 전까지) */
    assert(bits_eq(scratch[N_SPLINE], 0.0));

    /* y 클램프 양끝 + 범위 밖 */
    hc_df_eval(&g, X, -64.0, Z, scratch);
    assert(bits_eq(scratch[N_YGRAD], 1.5));
    hc_df_eval(&g, X, -1000.0, Z, scratch);
    assert(bits_eq(scratch[N_YGRAD], 1.5));
    hc_df_eval(&g, X, 320.0, Z, scratch);
    assert(bits_eq(scratch[N_YGRAD], -1.5));
    hc_df_eval(&g, X, 1000.0, Z, scratch);
    assert(bits_eq(scratch[N_YGRAD], -1.5));

    /* NOISE 의 좌표 스케일: k 를 바꾸면 스케일된 좌표의 직접 샘플과 같다 */
    nodes[N_NOISE].k0 = 0.25;
    nodes[N_NOISE].k1 = 2.0;
    nodes[N_NOISE].k2 = -1.0;
    hc_df_eval(&g, X, Y, Z, scratch);
    assert(bits_eq(scratch[N_NOISE],
                   hc_perlin_sample(&noise, X * 0.25, Y * 2.0, Z * -1.0)));

    /* 바닐라 MUL 단락과 Math.min/max ±0.0 규칙 (비트 비교) */
    hc_df_eval(&g, X, Y, Z, scratch);
    assert(bits_eq(scratch[N_MULZ], 0.0));  /* -0.0 이면 naive 곱 */
    assert(bits_eq(scratch[N_NEGZ], -0.0)); /* 단락 없음: -5.0 * 0.0 */
    assert(bits_eq(scratch[N_MINZ], -0.0)); /* min(+0,-0) = -0 */
    assert(bits_eq(scratch[N_MAXZ], 0.0));  /* max(-0,+0) = +0 */

    return 0;
}
