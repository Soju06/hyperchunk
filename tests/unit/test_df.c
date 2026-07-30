/* density function IR 스칼라 평가기 단위 테스트 (Plan Task 5, 6b 확장).
 *
 * 손으로 만든 그래프로 각 op 를 찌른다. 노이즈 노드는 golden 값을
 * 여기 복제하지 않는다 — hc_normal_noise_value 직접 호출과 비트
 * 동일한지만 본다 (노이즈 자체의 golden 검증은 test_noise 담당,
 * 라우터 전체의 golden 검증은 test_router_slots 담당).
 *
 * 상수 op 시맨틱은 바닐라 26.2 와 같아야 한다 (javap 로 확인):
 * - MUL(Ap2): arg1 == 0.0 이면 0.0 (naive a*b 는 0.0 * -x = -0.0)
 * - MUL_CONST(MulOrAdd): 단락 없음 — 0.0 * 음수상수 = -0.0 유지
 * - Y_CLAMPED_GRADIENT: Mth.clampedMap(y, fromY, toY, fromValue, toValue)
 * - MIN/MAX: Math.min/max (±0.0, NaN 처리 포함)
 * - SPLINE: 전 구간 float 산술 (CubicSpline) */

/* 테스트의 assert 는 빌드 플레이버와 무관하게 항상 살아 있어야 한다 —
 * NDEBUG 빌드에서 assert 가 증발하면 테스트가 공허하게 통과한다. */
#undef NDEBUG

#include "hc_df.h"
#include "hc_rng.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static int bits_eq(double a, double b) {
    uint64_t x, y;
    memcpy(&x, &a, sizeof x);
    memcpy(&y, &b, sizeof y);
    return x == y;
}

static int bits_are(double a, uint64_t want) {
    uint64_t x;
    memcpy(&x, &a, sizeof x);
    return x == want;
}

static unsigned char g_backing[1u << 18];

int main(void) {
    hc_arena_t arena;
    hc_arena_init(&arena, g_backing, sizeof g_backing);

    /* 아무 시드의 NormalNoise 하나 — 그래프 배관 검증용 */
    hc_xoro_t rand;
    hc_xoro_init(&rand, 1234567890LL);
    hc_normal_noise_t noise;
    const double amps[2] = {1.0, 1.0};
    assert(hc_normal_noise_init(&noise, &arena, &rand, -7, amps, 2) == 0);

    /* 스플라인 풀: [0] 상수 0.5, [1] 상수 2.0, [2] 2매듭 멀티포인트
     * (coordinate = N_X). 경계/보간/선형 연장의 float 산술을 고정한다. */
    float loc[2] = {0.0f, 1.0f};
    float der[2] = {0.0f, 3.0f};
    int32_t val[2] = {0, 1};
    hc_df_spline_t splines[3] = {
        {.coord = -1, .n = 0, .k = 0.5f},
        {.coord = -1, .n = 0, .k = 2.0f},
        {.coord = 0 /* N_X */, .n = 2, .loc = loc, .der = der, .val = val},
    };

    enum { N_X, N_Y, N_Z, N_CONST_A, N_CONST_B, N_ADD, N_MUL, N_MIN, N_MAX,
           N_CLAMP, N_YGRAD, N_YGRAD2, N_NOISE, N_SPLINE, N_ZERO, N_NEG,
           N_MULZ, N_NEGZ, N_MINZ, N_MAXZ, N_MULC, N_ADDC, N_ABS, N_SQUARE,
           N_CUBE, N_HALFN, N_QUARTN, N_SQUEEZE, N_INVERT, N_RANGE,
           N_MARKER, N_COUNT };
    hc_df_node_t nodes[N_COUNT] = {
        [N_X]       = {.op = HC_DF_X, .a = -1, .b = -1, .c = -1},
        [N_Y]       = {.op = HC_DF_Y, .a = -1, .b = -1, .c = -1},
        [N_Z]       = {.op = HC_DF_Z, .a = -1, .b = -1, .c = -1},
        [N_CONST_A] = {.op = HC_DF_CONST, .a = -1, .b = -1, .c = -1, .k0 = 2.5},
        [N_CONST_B] = {.op = HC_DF_CONST, .a = -1, .b = -1, .c = -1, .k0 = 4.0},
        [N_ADD]     = {.op = HC_DF_ADD, .a = N_CONST_A, .b = N_CONST_B, .c = -1},
        [N_MUL]     = {.op = HC_DF_MUL, .a = N_ADD, .b = N_CONST_A, .c = -1},
        [N_MIN]     = {.op = HC_DF_MIN, .a = N_MUL, .b = N_ADD, .c = -1},
        [N_MAX]     = {.op = HC_DF_MAX, .a = N_MUL, .b = N_ADD, .c = -1},
        /* CLAMP(a, k0=min, k1=max) */
        [N_CLAMP]   = {.op = HC_DF_CLAMP, .a = N_MUL, .b = -1, .c = -1,
                       .k0 = 0.0, .k1 = 10.0},
        /* 오버월드 스타일: fromY=-64, toY=320, fromValue=1.5, toValue=-1.5 */
        [N_YGRAD]   = {.op = HC_DF_Y_CLAMPED_GRADIENT, .a = -1, .b = -1,
                       .c = -1, .k0 = -64.0, .k1 = 320.0, .k2 = 1.5,
                       .k3 = -1.5},
        /* 비이진 상수(0.1/0.7)로 lerp '공식' 을 비트로 고정하기 위한 노드 —
         * 대칭 상수에서는 모든 lerp 변형이 비트 동일해 공허하다 */
        [N_YGRAD2]  = {.op = HC_DF_Y_CLAMPED_GRADIENT, .a = -1, .b = -1,
                       .c = -1, .k0 = -64.0, .k1 = 320.0, .k2 = 0.1,
                       .k3 = 0.7},
        /* NOISE: noises[aux].getValue(x*k0, y*k1, z*k0) */
        [N_NOISE]   = {.op = HC_DF_NOISE, .a = -1, .b = -1, .c = -1,
                       .aux = 0, .k0 = 1.0, .k1 = 1.0},
        [N_SPLINE]  = {.op = HC_DF_SPLINE, .a = -1, .b = -1, .c = -1,
                       .aux = 2},
        /* 바닐라 MUL 단락: 0.0 * 음수는 -0.0 이 아니라 0.0 이어야 한다 */
        [N_ZERO]    = {.op = HC_DF_CONST, .a = -1, .b = -1, .c = -1, .k0 = 0.0},
        [N_NEG]     = {.op = HC_DF_CONST, .a = -1, .b = -1, .c = -1, .k0 = -5.0},
        [N_MULZ]    = {.op = HC_DF_MUL, .a = N_ZERO, .b = N_NEG, .c = -1},
        /* Math.min/max 의 ±0.0 규칙: min(+0,-0) = -0, max(-0,+0) = +0.
         * N_NEG*N_ZERO = -0.0 (arg1 이 -5 라 단락 없음) 을 재료로 쓴다. */
        [N_NEGZ]    = {.op = HC_DF_MUL, .a = N_NEG, .b = N_ZERO, .c = -1},
        [N_MINZ]    = {.op = HC_DF_MIN, .a = N_ZERO, .b = N_NEGZ, .c = -1},
        [N_MAXZ]    = {.op = HC_DF_MAX, .a = N_NEGZ, .b = N_ZERO, .c = -1},
        /* MulOrAdd: 단락 없음 — 0.0 * -5.0 = -0.0 이 유지되어야 한다 */
        [N_MULC]    = {.op = HC_DF_MUL_CONST, .a = N_ZERO, .b = -1, .c = -1,
                       .k0 = -5.0},
        [N_ADDC]    = {.op = HC_DF_ADD_CONST, .a = N_CONST_A, .b = -1,
                       .c = -1, .k0 = 0.25},
        [N_ABS]     = {.op = HC_DF_ABS, .a = N_NEG, .b = -1, .c = -1},
        [N_SQUARE]  = {.op = HC_DF_SQUARE, .a = N_CONST_A, .b = -1, .c = -1},
        [N_CUBE]    = {.op = HC_DF_CUBE, .a = N_CONST_A, .b = -1, .c = -1},
        [N_HALFN]   = {.op = HC_DF_HALF_NEGATIVE, .a = N_NEG, .b = -1,
                       .c = -1},
        [N_QUARTN]  = {.op = HC_DF_QUARTER_NEGATIVE, .a = N_NEG, .b = -1,
                       .c = -1},
        /* squeeze(-5) : clamp → -1, -1/2 - (-1)/24 = -0.4583333... */
        [N_SQUEEZE] = {.op = HC_DF_SQUEEZE, .a = N_NEG, .b = -1, .c = -1},
        [N_INVERT]  = {.op = HC_DF_INVERT, .a = N_CONST_B, .b = -1, .c = -1},
        /* range_choice(input=2.5, [0,10)) → in-range 선택 */
        [N_RANGE]   = {.op = HC_DF_RANGE_CHOICE, .a = N_CONST_A, .b = N_NEG,
                       .c = N_CONST_B, .k0 = 0.0, .k1 = 10.0},
        [N_MARKER]  = {.op = HC_DF_INTERPOLATED, .a = N_CONST_A, .b = -1,
                       .c = -1},
    };
    hc_df_graph_t g = {
        .nodes = nodes, .n = N_COUNT,
        .noises = &noise, .n_noises = 1,
        .splines = splines, .n_splines = 3,
        .root = N_COUNT - 1, /* 평가는 [0..root] 프리픽스 — 전 노드 커버 */
    };

    double scratch[2 * N_COUNT]; /* 계약: 최소 2*n (FTS 보조 버퍼) */
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
    assert(bits_eq(scratch[N_NOISE], hc_normal_noise_value(&noise, X, Y, Z)));
    assert(bits_eq(root, scratch[N_COUNT - 1])); /* 반환값 == root 노드 */

    /* 스플라인: X=0.5 는 [0,1] 내부, t=0.5 (float).
     * val0=0.5, val1=2.0, der0=0, der1=3 →
     * p = 0*1 - 1.5 = -1.5, q = -3*1 + 1.5 = -1.5
     * lerp(.5, .5, 2) + .5*.5*lerp(.5, -1.5, -1.5) = 1.25 - 0.375 = 0.875 */
    assert(bits_eq(scratch[N_SPLINE], (double)0.875f));

    /* 단항/이항 신규 op */
    assert(bits_eq(scratch[N_MULC], -0.0)); /* MulOrAdd 는 단락 없음 */
    assert(bits_eq(scratch[N_ADDC], 2.75));
    assert(bits_eq(scratch[N_ABS], 5.0));
    assert(bits_eq(scratch[N_SQUARE], 6.25));
    assert(bits_eq(scratch[N_CUBE], 15.625));
    assert(bits_eq(scratch[N_HALFN], -2.5));
    assert(bits_eq(scratch[N_QUARTN], -1.25));
    /* -1/2 - (-1)^3/24 = -11/24 — 라우터 golden 의 흔한 상수와 동일 비트 */
    assert(bits_are(scratch[N_SQUEEZE], 0xbfdd555555555555ULL));
    assert(bits_eq(scratch[N_INVERT], 0.25));
    assert(bits_eq(scratch[N_RANGE], -5.0));
    assert(bits_eq(scratch[N_MARKER], 2.5)); /* 마커는 pass-through */

    /* y 클램프 양끝 + 범위 밖 */
    hc_df_eval(&g, X, -64.0, Z, scratch);
    assert(bits_eq(scratch[N_YGRAD], 1.5));
    assert(bits_eq(scratch[N_YGRAD2], 0.1)); /* t=0: c + 0*(d-c) == c 정확 */
    hc_df_eval(&g, X, -1000.0, Z, scratch);
    assert(bits_eq(scratch[N_YGRAD], 1.5));
    hc_df_eval(&g, X, 320.0, Z, scratch);
    assert(bits_eq(scratch[N_YGRAD], -1.5));
    hc_df_eval(&g, X, 1000.0, Z, scratch);
    assert(bits_eq(scratch[N_YGRAD], -1.5));
    assert(bits_eq(scratch[N_YGRAD2], 0.7)); /* t>1 은 상수 d 반환 */

    /* lerp 공식 자체를 비트로 고정한다: y=-59 → t=5/384 (비이진) 에서
     * 바닐라 c + t*(d-c) 는 0x3fbb99999999999a, 수학적으로 동치인
     * (1-t)*c + t*d 는 ...99 로 최하위 비트가 다르다. 대칭 상수/중앙점만
     * 찍으면 어떤 공식이든 통과하는 공허한 검사가 된다 (리뷰 실증). */
    hc_df_eval(&g, X, -59.0, Z, scratch);
    assert(bits_are(scratch[N_YGRAD2], 0x3fbb99999999999aULL));

    /* NOISE 의 좌표 스케일: xz_scale 은 x/z 에, y_scale 은 y 에 붙는다 */
    nodes[N_NOISE].k0 = 0.25;
    nodes[N_NOISE].k1 = 2.0;
    hc_df_eval(&g, X, Y, Z, scratch);
    assert(bits_eq(scratch[N_NOISE],
                   hc_normal_noise_value(&noise, X * 0.25, Y * 2.0,
                                         Z * 0.25)));

    /* 스플라인 경계: 아래쪽 선형 연장 (der0=0 → 조기 반환으로 val0 그대로),
     * 위쪽 연장 (der1=3): val1 + 3*(f-1) */
    hc_df_eval(&g, -2.0, Y, Z, scratch);
    assert(bits_eq(scratch[N_SPLINE], (double)0.5f));
    hc_df_eval(&g, 3.0, Y, Z, scratch);
    assert(bits_eq(scratch[N_SPLINE], (double)(2.0f + 3.0f * 2.0f)));
    /* 매듭 정확히 위: f==loc[1] → i==last → 위쪽 연장 경로, f-loc = 0 */
    hc_df_eval(&g, 1.0, Y, Z, scratch);
    assert(bits_eq(scratch[N_SPLINE], (double)2.0f));

    /* 바닐라 MUL 단락과 Math.min/max ±0.0 규칙 (비트 비교) */
    hc_df_eval(&g, X, Y, Z, scratch);
    assert(bits_eq(scratch[N_MULZ], 0.0));  /* -0.0 이면 naive 곱 */
    assert(bits_eq(scratch[N_NEGZ], -0.0)); /* 단락 없음: -5.0 * 0.0 */
    assert(bits_eq(scratch[N_MINZ], -0.0)); /* min(+0,-0) = -0 */
    assert(bits_eq(scratch[N_MAXZ], 0.0));  /* max(-0,+0) = +0 */

    return 0;
}
