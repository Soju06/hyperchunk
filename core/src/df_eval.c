#include "hc_df.h"

#include <assert.h>
#include <math.h>
#include <string.h>

/* density function IR 스칼라 평가기. 노드는 위상 정렬 전제 (a, b, c < i) —
 * 위반은 그래프 빌더 버그이므로 debug assert 로 잡는다 (ADR-009 D3).
 *
 * 모든 op 의 FP 시맨틱은 바닐라 26.2 (javap 확인) 와 동일하게 맞춘다.
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

/* Mth.clamp(v, lo, hi): v < lo 면 lo, 아니면 Math.min(v, hi)
 * — 상한이 Math.min 을 통과하는 것까지 바이트코드 그대로 */
static double mth_clamp(double v, double lo, double hi) {
    return v < lo ? lo : jmin(v, hi);
}

/* --- CubicSpline 샘플링 (전 구간 float 산술, javap 확인) --- */

/* Mth.lerp(float): start + delta * (end - start) */
static float lerp_f(float delta, float start, float end) {
    return start + delta * (end - start);
}

/* Mth.binarySearch(0, n, i -> coord < loc[i]) - 1
 * == loc[i] <= coord 인 최대 i, coord < loc[0] 이면 -1.
 * NaN coord 는 술어가 전부 false 라 n-1 로 떨어진다 (바닐라 fcmpg 동일). */
static int32_t find_interval_start(const float *loc, int32_t n, float coord) {
    int32_t lo = 0, span = n;
    while (span > 0) {
        int32_t half = span / 2;
        int32_t mid = lo + half;
        if (coord < loc[mid]) {
            span = half;
        } else {
            lo = mid + 1;
            span -= half + 1;
        }
    }
    return lo - 1;
}

/* CubicSpline.linearExtend — der == 0.0f 조기 반환까지 그대로
 * (value + 0.0f 는 value 가 -0.0f 일 때 비트를 바꾼다). */
static float linear_extend(float f, const float *loc, float value,
                           const float *der, int32_t idx) {
    float d = der[idx];
    if (d == 0.0f)
        return value;
    return value + d * (f - loc[idx]);
}

/* CubicSpline.sample. 내부 구간은 양 끝 값 스플라인을 항상 둘 다 평가한다.
 * coordinate DF 는 위상 순서상 이미 평가돼 있어 scratch 에서 읽는다. */
static float spline_sample(const hc_df_graph_t *g, int32_t si,
                           const double *sc) {
    const hc_df_spline_t *s = &g->splines[si];
    if (s->n == 0)
        return s->k;

    float   f = (float)sc[s->coord];
    int32_t i = find_interval_start(s->loc, s->n, f);
    int32_t last = s->n - 1;

    if (i < 0)
        return linear_extend(f, s->loc, spline_sample(g, s->val[0], sc),
                             s->der, 0);
    if (i == last)
        return linear_extend(f, s->loc, spline_sample(g, s->val[last], sc),
                             s->der, last);

    float loc0 = s->loc[i];
    float loc1 = s->loc[i + 1];
    float t = (f - loc0) / (loc1 - loc0);
    float der0 = s->der[i];
    float der1 = s->der[i + 1];
    float val0 = spline_sample(g, s->val[i], sc);
    float val1 = spline_sample(g, s->val[i + 1], sc);
    float p = der0 * (loc1 - loc0) - (val1 - val0);
    float q = (-der1) * (loc1 - loc0) + (val1 - val0);
    return lerp_f(t, val0, val1) + (t * (1.0f - t)) * lerp_f(t, p, q);
}

/* --- NoiseChunk 셀 시맨틱 (6c) --- */

/* Mth.lerp(double): a + t*(b-a) — FMA 금지 하에 mul→add 순서 그대로 */
static double lerp_d(double t, double a, double b) {
    return a + t * (b - a);
}

/* Mth.lerp2/lerp3 — 셀 채움 경로의 중첩 순서 (x 안쪽, y 중간, z 바깥).
 * 블록 루프의 updateForY→X→Z (y 먼저) 와 FP 순서가 다르다 — 두 경로를
 * 섞으면 비트가 어긋난다. */
static double lerp2_d(double dx, double dy, double a, double b, double c,
                      double d) {
    return lerp_d(dy, lerp_d(dx, a, b), lerp_d(dx, c, d));
}

static double interp_lerp3(const hc_df_cellctx_t *cc,
                           const hc_df_interp_t *it) {
    double dx = (double)cc->in_cell_x / (double)cc->cell_width;
    double dy = (double)cc->in_cell_y / (double)cc->cell_height;
    double dz = (double)cc->in_cell_z / (double)cc->cell_width;
    /* NoiseInterpolator.compute(fillingCell): lerp3(dx,dy,dz,
     *   n000,n100,n010,n110,n001,n101,n011,n111) — 자릿수는 x z y */
    return lerp_d(dz,
                  lerp2_d(dx, dy, it->n000, it->n100, it->n010, it->n110),
                  lerp2_d(dx, dy, it->n001, it->n101, it->n011, it->n111));
}

/* 노드 하나 평가. sc 는 활성 scratch (피연산자 값). sc2 는
 * FIND_TOP_SURFACE 전용 보조 버퍼 — NULL 이면 FTS 금지 문맥(콘 내부)이다.
 * 컴파일러가 FTS 콘 안에 FTS 가 없음을 보장한다. */
static double eval_node(const hc_df_graph_t *g, int32_t idx, double x,
                        double y, double z, double *sc, double *sc2,
                        const hc_df_cellctx_t *cc);

/* FTS density 콘 재평가: ipool[off..off+len) 은 오름차순 노드 인덱스라
 * 위상 순서가 보존된다. 콘 밖 노드는 건드리지 않는다. */
static double eval_cone(const hc_df_graph_t *g, int32_t off, int32_t len,
                        int32_t root, double x, double y, double z,
                        double *sc2, const hc_df_cellctx_t *cc) {
    for (int32_t j = 0; j < len; j++) {
        int32_t idx = g->ipool[off + j];
        sc2[idx] = eval_node(g, idx, x, y, z, sc2, NULL, cc);
    }
    return sc2[root];
}

static double eval_node(const hc_df_graph_t *g, int32_t idx, double x,
                        double y, double z, double *sc, double *sc2,
                        const hc_df_cellctx_t *cc) {
    const hc_df_node_t *nd = &g->nodes[idx];
    switch (nd->op) {
    case HC_DF_CONST:
        return nd->k0;
    case HC_DF_X:
        return x;
    case HC_DF_Y:
        return y;
    case HC_DF_Z:
        return z;

    case HC_DF_ADD:
        return sc[nd->a] + sc[nd->b];
    case HC_DF_MUL:
        /* 바닐라 Ap2 MUL 단락: arg1 == 0.0 이면 0.0.
         * naive 곱은 0.0 * 음수 = -0.0 으로 비트가 달라진다. */
        return sc[nd->a] == 0.0 ? 0.0 : sc[nd->a] * sc[nd->b];
    case HC_DF_MIN:
        /* 바닐라의 minValue() 단락은 arg2 '평가 생략' 최적화일 뿐
         * 결과는 Math.min 과 동일하다 — 평탄 IR 은 어차피 전 노드를
         * 평가하므로 Math.min 시맨틱만 맞추면 된다. */
        return jmin(sc[nd->a], sc[nd->b]);
    case HC_DF_MAX:
        return jmax(sc[nd->a], sc[nd->b]);

    case HC_DF_ADD_CONST:
        return sc[nd->a] + nd->k0;
    case HC_DF_MUL_CONST:
        /* MulOrAdd 는 단락이 없다 — input 0.0 * 음수상수 = -0.0 유지 */
        return sc[nd->a] * nd->k0;

    case HC_DF_ABS:
        return fabs(sc[nd->a]);
    case HC_DF_SQUARE:
        return sc[nd->a] * sc[nd->a];
    case HC_DF_CUBE:
        return sc[nd->a] * sc[nd->a] * sc[nd->a];
    case HC_DF_HALF_NEGATIVE:
        return sc[nd->a] > 0.0 ? sc[nd->a] : sc[nd->a] * 0.5;
    case HC_DF_QUARTER_NEGATIVE:
        return sc[nd->a] > 0.0 ? sc[nd->a] : sc[nd->a] * 0.25;
    case HC_DF_SQUEEZE: {
        double d = mth_clamp(sc[nd->a], -1.0, 1.0);
        return d / 2.0 - d * d * d / 24.0;
    }
    case HC_DF_INVERT:
        return 1.0 / sc[nd->a]; /* Mapped INVERT — 가드 없음 (javap) */

    case HC_DF_CLAMP:
        return mth_clamp(sc[nd->a], nd->k0, nd->k1);

    case HC_DF_Y_CLAMPED_GRADIENT: {
        /* Mth.clampedMap(y, fromY k0, toY k1, fromValue k2, toValue k3):
         * inverseLerp 후 clampedLerp */
        double t = (y - nd->k0) / (nd->k1 - nd->k0);
        if (t < 0.0)
            return nd->k2;
        if (t > 1.0)
            return nd->k3;
        return nd->k2 + t * (nd->k3 - nd->k2);
    }

    case HC_DF_RANGE_CHOICE: {
        double d = sc[nd->a];
        return (d >= nd->k0 && d < nd->k1) ? sc[nd->b] : sc[nd->c];
    }

    case HC_DF_NOISE:
        return hc_normal_noise_value(&g->noises[nd->aux], x * nd->k0,
                                     y * nd->k1, z * nd->k0);
    case HC_DF_SHIFTED_NOISE:
        return hc_normal_noise_value(&g->noises[nd->aux],
                                     x * nd->k0 + sc[nd->a],
                                     y * nd->k1 + sc[nd->b],
                                     z * nd->k0 + sc[nd->c]);
    case HC_DF_SHIFT_A:
        /* ShiftNoise.compute(x, 0, z): getValue(x/4, 0, z/4) * 4 */
        return hc_normal_noise_value(&g->noises[nd->aux], x * 0.25, 0.0,
                                     z * 0.25) *
               4.0;
    case HC_DF_SHIFT_B:
        /* ShiftNoise.compute(z, x, 0): getValue(z/4, x/4, 0) * 4 */
        return hc_normal_noise_value(&g->noises[nd->aux], z * 0.25, x * 0.25,
                                     0.0) *
               4.0;
    case HC_DF_BLENDED_NOISE:
        /* FunctionContext 블록 좌표는 int — 호출자가 정수 좌표를 넘긴다 */
        return hc_blended_compute(&g->blended[nd->aux], (int32_t)x,
                                  (int32_t)y, (int32_t)z);

    case HC_DF_SPLINE:
        return (double)spline_sample(g, nd->aux, sc);

    case HC_DF_INTERVAL_SELECT: {
        /* ipool[aux] = 함수 수 n, ipool[aux+1..aux+n] = 함수 노드,
         * dpool[aux2..aux2+n-1) = 오름차순 경계. d < 경계면 그 구간 함수,
         * 경계와 같으면 다음 구간 (바닐라 선형 스캔 + 엄격 미만). */
        double  d = sc[nd->a];
        int32_t n = g->ipool[nd->aux];
        int32_t sel = n - 1;
        for (int32_t j = 0; j < n - 1; j++) {
            if (d < g->dpool[nd->aux2 + j]) {
                sel = j;
                break;
            }
        }
        return sc[g->ipool[nd->aux + 1 + sel]];
    }

    case HC_DF_FIND_TOP_SURFACE: {
        /* 26.2 FindTopSurface.compute (javap):
         *   start = Mth.floor(upper/cellHeight) * cellHeight  (절대 격자 정렬)
         *   start <= lowerBound 면 density 평가 없이 lowerBound
         *   y = start..lowerBound (step -cellHeight): density > 0.0 이면 y
         *   미발견이면 lowerBound.
         * density 는 (blockX, y, blockZ) 새 컨텍스트 — 콘만 sc2 로
         * 재평가한다. upper_bound 는 들어온 컨텍스트로 이미 평가됐다. */
        assert(sc2 != NULL); /* FTS 콘 안의 FTS 는 컴파일러가 거부 */
        int32_t lower = (int32_t)nd->k0;
        int32_t cell = (int32_t)nd->k1;
        /* Java (int) 캐스트는 saturating, Mth.floor 는 그 위의 보정 */
        double  q = sc[nd->b] / (double)cell;
        int32_t qi;
        if (q != q)
            qi = 0;
        else if (q >= 2147483647.0)
            qi = INT32_MAX;
        else if (q <= -2147483648.0)
            qi = INT32_MIN;
        else
            qi = (int32_t)q;
        if (q < (double)qi)
            qi = (int32_t)((uint32_t)qi - 1u); /* Java int 감산 래핑 */
        /* Java int 곱은 래핑 — int64 곱 후 절단으로 재현 (UB 회피) */
        int32_t start = (int32_t)(uint32_t)((int64_t)qi * cell);
        if (start <= lower)
            return (double)lower;
        for (int32_t yy = start; yy >= lower; yy -= cell) {
            double d = eval_cone(g, nd->aux, nd->aux2, nd->a, x, (double)yy,
                                 z, sc2, cc);
            if (d > 0.0)
                return (double)yy;
        }
        return (double)lower;
    }

    case HC_DF_BLEND_OFFSET:
        return 0.0; /* BlendOffset.compute 상수 (javap) */
    case HC_DF_BLEND_ALPHA:
        return 1.0; /* BlendAlpha.compute 상수 (javap) */

    case HC_DF_INTERPOLATED:
        /* 바닐라 NoiseInterpolator.compute:
         *  - SinglePointContext (SP): noiseFiller.compute(ctx) — 하위 트리는
         *    위상 순서로 이미 평가돼 있으므로 pass-through 가 그 값이다.
         *  - fillingCell (CELL): 셀 코너 lerp3.
         *  - 블록 루프 (BLOCK): updateForY/X/Z 의 점진 lerp 값. */
        if (cc && cc->interp_of[idx] >= 0) {
            const hc_df_interp_t *it = &cc->interp[cc->interp_of[idx]];
            if (cc->mode == HC_DF_MODE_CELL)
                return interp_lerp3(cc, it);
            if (cc->mode == HC_DF_MODE_BLOCK)
                return it->value;
        }
        return sc[nd->a];

    case HC_DF_FLAT_CACHE: {
        /* FlatCache.compute 는 ctx 종류와 무관하게 위치 기반이다:
         * 쿼트 창 안이면 y=0 쿼트-정렬 좌표로 미리 계산한 테이블, 밖이면
         * 원 좌표로 wrapped 신선 평가 (== pass-through). aquifer 가 청크
         * 밖 컬럼을 조회할 때 창 밖 경로가 실제로 발생하고 값이 달라진다
         * (쿼트 정렬 vs 원좌표) — 창 검사는 정확히 재현해야 한다. */
        if (cc && cc->flat_of[idx] >= 0) {
            int32_t qx = (int32_t)x >> 2; /* QuartPos.fromBlock */
            int32_t qz = (int32_t)z >> 2;
            int32_t i = qx - cc->first_noise_x;
            int32_t j = qz - cc->first_noise_z;
            int32_t size = cc->noise_size_xz + 1;
            if (i >= 0 && j >= 0 && i < size && j < size)
                return cc->flat[cc->flat_of[idx]].values[i + j * size];
        }
        return sc[nd->a];
    }

    case HC_DF_BLEND_DENSITY: /* 26.2 에선 마커 타입 — 신규 월드 identity */
    case HC_DF_CACHE_2D:
    case HC_DF_CACHE_ONCE:
        /* 전 모드 pass-through — 값-중립 증명은 hc_df.h 셀 문맥 주석 참조 */
        return sc[nd->a];

    case HC_DF_CACHE_ALL_IN_CELL:
        /* 26.2 오버월드 JSON 에는 인스턴스가 없다. 바닐라가 쓰는 유일한
         * cacheAllInCell(final_density+beardifier) 은 noise_chunk.c 가
         * density_cell 배열로 직접 소유한다. */
        return sc[nd->a];

    default:
        assert(!"unknown df op");
        return 0.0;
    }
}

double hc_df_eval_ex(const hc_df_graph_t *g, double x, double y, double z,
                     double *scratch, const hc_df_cellctx_t *cc) {
    assert(g->n > 0 && g->root >= 0 && g->root < g->n);
    double *sc2 = scratch + g->n;
    /* 위상 정렬이라 [0..root] 프리픽스가 의존성 닫힘이다 — 여러 슬롯이
     * 한 그래프를 공유할 때 root 만 바꿔 불필요한 꼬리를 건너뛴다. */
    for (int32_t i = 0; i <= g->root; i++) {
        assert(g->nodes[i].a < i && g->nodes[i].b < i && g->nodes[i].c < i);
        scratch[i] = eval_node(g, i, x, y, z, scratch, sc2, cc);
    }
    return scratch[g->root];
}

double hc_df_eval(const hc_df_graph_t *g, double x, double y, double z,
                  double *scratch) {
    return hc_df_eval_ex(g, x, y, z, scratch, NULL);
}
