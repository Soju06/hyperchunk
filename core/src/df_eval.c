#include "hc_df.h"

#include <assert.h>
#include <string.h>

/* density function IR 스칼라 평가기. 노드는 위상 정렬 전제 (a, b < i) —
 * 위반은 그래프 빌더 버그이므로 debug assert 로 잡는다 (ADR-009 D3).
 *
 * 상수 op 의 FP 시맨틱은 바닐라 26.2 (javap 확인) 와 동일하게 맞춘다.
 * 스칼라 값이 같아도 비트가 다르면 (±0.0) 하류 덤프 대조가 깨진다. */

/* java.lang.Math.min/max — ±0.0 과 NaN 규칙까지 그대로.
 * C 의 `a < b ? a : b` 는 min(+0.0, -0.0) 에서 +0.0 을 돌려줘 어긋난다. */
static double jmin(double a, double b) {
    if (a != a)
        return a; /* NaN 은 a 우선 */
    if (a == 0.0 && b == 0.0) {
        uint64_t bb;
        memcpy(&bb, &b, sizeof bb);
        if (bb >> 63)
            return b; /* b 가 -0.0 이면 b */
    }
    return a <= b ? a : b;
}

static double jmax(double a, double b) {
    if (a != a)
        return a;
    if (a == 0.0 && b == 0.0) {
        uint64_t ab;
        memcpy(&ab, &a, sizeof ab);
        if (ab >> 63)
            return b; /* a 가 -0.0 이면 b */
    }
    return a >= b ? a : b;
}

double hc_df_eval(const hc_df_graph_t *g, double x, double y, double z,
                  double *scratch) {
    assert(g->n > 0 && g->root >= 0 && g->root < g->n);
    for (int32_t i = 0; i < g->n; i++) {
        const hc_df_node_t *nd = &g->nodes[i];
        double r;
        switch (nd->op) {
        case HC_DF_CONST:
            r = nd->k0;
            break;
        case HC_DF_X:
            r = x;
            break;
        case HC_DF_Y:
            r = y;
            break;
        case HC_DF_Z:
            r = z;
            break;
        case HC_DF_NOISE:
            /* k0/k1/k2 = xz/y 좌표 스케일 (라우터가 채운다, Task 6) */
            assert(nd->noise_id >= 0 && nd->noise_id < g->n_noises);
            r = hc_perlin_sample(&g->noises[nd->noise_id],
                                 x * nd->k0, y * nd->k1, z * nd->k2);
            break;
        case HC_DF_ADD:
            assert(nd->a >= 0 && nd->a < i && nd->b >= 0 && nd->b < i);
            r = scratch[nd->a] + scratch[nd->b];
            break;
        case HC_DF_MUL:
            assert(nd->a >= 0 && nd->a < i && nd->b >= 0 && nd->b < i);
            /* 바닐라 Ap2 MUL 단락: arg1 == 0.0 이면 0.0.
             * naive 곱은 0.0 * 음수 = -0.0 으로 비트가 달라진다. */
            r = scratch[nd->a] == 0.0 ? 0.0 : scratch[nd->a] * scratch[nd->b];
            break;
        case HC_DF_MIN:
            /* 바닐라의 minValue() 단락은 arg2 '평가 생략' 최적화일 뿐
             * 결과는 Math.min 과 동일하다 — 평탄 IR 은 어차피 전 노드를
             * 평가하므로 Math.min 시맨틱만 맞추면 된다. */
            assert(nd->a >= 0 && nd->a < i && nd->b >= 0 && nd->b < i);
            r = jmin(scratch[nd->a], scratch[nd->b]);
            break;
        case HC_DF_MAX:
            assert(nd->a >= 0 && nd->a < i && nd->b >= 0 && nd->b < i);
            r = jmax(scratch[nd->a], scratch[nd->b]);
            break;
        case HC_DF_CLAMP: {
            /* Mth.clamp(v, k0, k1) */
            assert(nd->a >= 0 && nd->a < i);
            double t = scratch[nd->a];
            r = t < nd->k0 ? nd->k0 : (t > nd->k1 ? nd->k1 : t);
            break;
        }
        case HC_DF_Y_CLAMPED_GRADIENT: {
            /* YClampedGradient = Mth.clampedMap(y, fromY k0, toY k1,
             * fromValue k2, toValue k3): inverseLerp 후 clampedLerp */
            double t = (y - nd->k0) / (nd->k1 - nd->k0);
            if (t < 0.0)
                r = nd->k2;
            else if (t > 1.0)
                r = nd->k3;
            else
                r = nd->k2 + t * (nd->k3 - nd->k2);
            break;
        }
        case HC_DF_SPLINE: /* Task 6 에서 구현 — 그때까지 0.0 (플랜 명시) */
        default:
            r = 0.0;
            break;
        }
        scratch[i] = r;
    }
    return scratch[g->root];
}
