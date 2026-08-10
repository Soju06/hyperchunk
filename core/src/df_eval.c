#include "hc_df.h"
#include "hc_counters.h"

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
 * 컴파일러가 FTS 콘 안에 FTS 가 없음을 보장한다. mask 는 라이브-콘
 * 멤버십 (debug assert 전용, NULL 허용) — 마커 폴백이 콘 밖 노드를
 * 읽으면 스테일 scratch 이므로 발화한다. */
static double eval_node(const hc_df_graph_t *g, int32_t idx, double x,
                        double y, double z, double *sc, double *sc2,
                        const hc_df_cellctx_t *cc, const uint8_t *mask);

/* FTS density 콘 재평가: ipool[off..off+len) 은 오름차순 노드 인덱스라
 * 위상 순서가 보존된다. 콘 밖 노드는 건드리지 않는다. mark_deps 가 전
 * 의존을 담아 자족적이므로 mask 검사는 불필요 (NULL). root < 0 이면
 * sc2 만 채운다 (y-불변 프리픽스 패스). */
static double eval_cone(const hc_df_graph_t *g, int32_t off, int32_t len,
                        int32_t root, double x, double y, double z,
                        double *sc2, const hc_df_cellctx_t *cc) {
    for (int32_t j = 0; j < len; j++) {
        int32_t idx = g->ipool[off + j];
        sc2[idx] = eval_node(g, idx, x, y, z, sc2, NULL, cc, NULL);
    }
    return root >= 0 ? sc2[root] : 0.0;
}

static double eval_node(const hc_df_graph_t *g, int32_t idx, double x,
                        double y, double z, double *sc, double *sc2,
                        const hc_df_cellctx_t *cc, const uint8_t *mask) {
    (void)mask; /* NDEBUG 에서는 assert 전용 */
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
        /* 콘은 [nI][y-불변 nI 개][y-가변] 분할 적재 (hc_df.h, P2-2).
         * y-불변부는 정의상 y 를 읽지 않아 (hc_df_mark_y_variant 보수
         * 분류) 어느 y 로 평가해도 비트 동일 — 사다리 진입 시 1회만
         * 평가한다. y-가변 노드의 피연산자 값은 전 스텝 재평가와 동일
         * 하므로 (순수 노드, RNG 없음) 사다리 결과도 비트 동일하다. */
        int32_t n_inv = g->ipool[nd->aux];
        int32_t inv_off = nd->aux + 1;
        eval_cone(g, inv_off, n_inv, -1, x, (double)start, z, sc2, cc);
        for (int32_t yy = start; yy >= lower; yy -= cell) {
            HC_CTR_INC(HC_CTR_FTS_ITER);
            double d = eval_cone(g, inv_off + n_inv, nd->aux2 - n_inv,
                                 nd->a, x, (double)yy, z, sc2, cc);
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
        /* SP pass-through — 콘 평가라면 자식이 콘에 있어야 한다 */
        assert(!mask || mask[nd->a]);
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
        /* 창-밖 폴백 — SP 콘은 자식을 보수 포함한다. CELL/BLOCK 콘은
         * 자식을 컷했으므로 여기 도달하면 콘 계산 버그 (즉시 발화). */
        assert(!mask || mask[nd->a]);
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
        scratch[i] = eval_node(g, i, x, y, z, scratch, sc2, cc, NULL);
    }
    return scratch[g->root];
}

double hc_df_eval(const hc_df_graph_t *g, double x, double y, double z,
                  double *scratch) {
    return hc_df_eval_ex(g, x, y, z, scratch, NULL);
}

/* --- (root×mode) 라이브-콘 산출 + 평가 (P2-1, 규칙은 hc_df.h 주석) --- */

/* spline_sample 은 coord 를 sc 에서 읽는다 — 중첩 스플라인까지 마크.
 * coord/중첩 val 은 스플라인을 참조하는 노드보다 먼저 컴파일되므로
 * (compile_spline_spec 순서) 하향 스윕이 이후에 그 의존을 확장한다. */
static void cone_mark_spline(const hc_df_graph_t *g, int32_t si,
                             uint8_t *mark) {
    const hc_df_spline_t *s = &g->splines[si];
    if (s->n == 0)
        return;
    mark[s->coord] = 1;
    for (int32_t j = 0; j < s->n; j++)
        cone_mark_spline(g, s->val[j], mark);
}

int32_t hc_df_cone_mark(const hc_df_graph_t *g, hc_df_mode_t mode,
                        const int32_t *roots, int32_t n_roots,
                        uint8_t *mark) {
    return hc_df_cone_mark_ex(g, mode, roots, n_roots, mark, 0);
}

int32_t hc_df_cone_mark_ex(const hc_df_graph_t *g, hc_df_mode_t mode,
                           const int32_t *roots, int32_t n_roots,
                           uint8_t *mark, uint32_t flags) {
    /* 하향 스윕: 위상 정렬(피연산자 인덱스 < 노드 인덱스)이라 i 를
     * 내림차순으로 지나며 마크된 노드의 피연산자를 마크하면 전이 닫힘이
     * 완성된다 — 재귀 없음, O(top). */
    int32_t top = -1;
    for (int32_t r = 0; r < n_roots; r++) {
        assert(roots[r] >= 0 && roots[r] < g->n);
        mark[roots[r]] = 1;
        if (roots[r] > top)
            top = roots[r];
    }
    int32_t len = 0;
    for (int32_t i = top; i >= 0; i--) {
        if (!mark[i])
            continue;
        len++;
        const hc_df_node_t *nd = &g->nodes[i];
        switch (nd->op) {
        case HC_DF_INTERPOLATED:
            /* CELL/BLOCK: 셀 상태만 읽는다 — 자식 컷.
             * SP: pass-through — 자식 포함. */
            if (mode == HC_DF_MODE_SP)
                mark[nd->a] = 1;
            break;
        case HC_DF_FLAT_CACHE:
            /* CELL/BLOCK: 테이블만 읽는다 — 자식 컷.
             * SP: 창-밖 폴백 대비 자식 보수 포함 — 단, 창-안 보장 문맥
             * (WINDOW_SAFE) 은 항상 테이블 히트라 자식 컷 (hc_df.h). */
            if (mode == HC_DF_MODE_SP && !(flags & HC_DF_CONE_WINDOW_SAFE))
                mark[nd->a] = 1;
            break;
        case HC_DF_FIND_TOP_SURFACE:
            /* density(a) 는 자체 ipool 콘이 sc2 로 자족 — b 만.
             * FTS 는 fresh SinglePointContext 시맨틱이라 SP 콘 전용. */
            if (mode != HC_DF_MODE_SP) {
                assert(!"find_top_surface in non-SP cone");
                return -1;
            }
            mark[nd->b] = 1;
            break;
        case HC_DF_INTERVAL_SELECT: {
            /* 선택은 런타임 값 — 함수 전부 포함 (FTS build_cone 동형) */
            mark[nd->a] = 1;
            int32_t nf = g->ipool[nd->aux];
            for (int32_t k = 0; k < nf; k++)
                mark[g->ipool[nd->aux + 1 + k]] = 1;
            break;
        }
        case HC_DF_SPLINE:
            cone_mark_spline(g, nd->aux, mark);
            break;
        default:
            if (nd->a >= 0)
                mark[nd->a] = 1;
            if (nd->b >= 0)
                mark[nd->b] = 1;
            if (nd->c >= 0)
                mark[nd->c] = 1;
        }
    }
    return len;
}

void hc_df_cone_collect(const hc_df_graph_t *g, const uint8_t *mark,
                        int32_t *out) {
    int32_t k = 0;
    for (int32_t i = 0; i < g->n; i++)
        if (mark[i])
            out[k++] = i;
}

double hc_df_eval_cone(const hc_df_graph_t *g, const int32_t *cone,
                       int32_t n_cone, int32_t root, double x, double y,
                       double z, double *scratch, const hc_df_cellctx_t *cc,
                       const uint8_t *mask) {
    double *sc2 = scratch + g->n;
    for (int32_t j = 0; j < n_cone; j++) {
        int32_t i = cone[j];
        assert(j == 0 || cone[j - 1] < i); /* 오름차순 == 위상 순서 */
        scratch[i] = eval_node(g, i, x, y, z, scratch, sc2, cc, mask);
    }
    assert(root < 0 || !mask || mask[root]);
    return root >= 0 ? scratch[root] : 0.0;
}

/* --- 콘 프로그램: lazy 브랜치 평가 (P2-4, 규칙은 hc_df.h 주석이 SoT) --- */

enum { PROG_RC = -1, PROG_IS = -2 };

static void prog_reach_spline(const hc_df_graph_t *g, int32_t si,
                              const uint8_t *in_cone, uint8_t *out) {
    const hc_df_spline_t *s = &g->splines[si];
    if (s->n == 0)
        return;
    if (in_cone[s->coord])
        out[s->coord] = 1;
    for (int32_t j = 0; j < s->n; j++)
        prog_reach_spline(g, s->val[j], in_cone, out);
}

/* 콘-내부 도달성 (위상 역방향 스윕, hc_df_cone_mark_ex 와 같은 기법).
 * skip_from >= 0 이면 그 choice 노드의 '브랜치' 엣지 (RANGE_CHOICE 의
 * b/c, INTERVAL_SELECT 의 함수표 엔트리) 중 대상이 skip_to 인 것만
 * 제외한다 — 입력(a) 엣지는 항상 유지. 엣지 집합은 eval_node 가 실제로
 * 읽는 엣지의 상위집합이다 (CELL/BLOCK 마커의 자식 엣지도 따라가지만
 * 그 자식은 콘 밖이라 in_cone 필터에 걸린다; FTS 의 a 는 sc2 자족이지만
 * 따라가도 배타 집합이 줄어들 뿐 — 보수 방향). */
static void prog_reach(const hc_df_graph_t *g, const uint8_t *in_cone,
                       const int32_t *roots, int32_t n_roots,
                       int32_t skip_from, int32_t skip_to, uint8_t *out) {
    memset(out, 0, (size_t)g->n);
    int32_t top = -1;
    for (int32_t r = 0; r < n_roots; r++)
        if (in_cone[roots[r]]) {
            out[roots[r]] = 1;
            if (roots[r] > top)
                top = roots[r];
        }
    for (int32_t i = top; i >= 0; i--) {
        if (!out[i])
            continue;
        const hc_df_node_t *nd = &g->nodes[i];
        const int32_t       ops[3] = {nd->a, nd->b, nd->c};
        for (int k = 0; k < 3; k++) {
            int32_t t = ops[k];
            if (t < 0 || !in_cone[t])
                continue;
            if (i == skip_from && t == skip_to && k != 0 &&
                nd->op == HC_DF_RANGE_CHOICE)
                continue;
            out[t] = 1;
        }
        if (nd->op == HC_DF_INTERVAL_SELECT) {
            int32_t nf = g->ipool[nd->aux];
            for (int32_t k = 0; k < nf; k++) {
                int32_t t = g->ipool[nd->aux + 1 + k];
                if (in_cone[t] && !(i == skip_from && t == skip_to))
                    out[t] = 1;
            }
        } else if (nd->op == HC_DF_SPLINE) {
            prog_reach_spline(g, nd->aux, in_cone, out);
        }
    }
}

typedef struct {
    const hc_df_graph_t *g;
    const int32_t       *cone;
    int32_t              len;
    const int32_t       *home_ch, *home_br; /* [g->n] */
    const uint8_t       *has_seg;           /* [g->n] */
    int32_t             *out;
    int32_t              pos, cap;
    int32_t              n_emitted, n_controls;
    int                  fail;
} prog_emitter_t;

static void prog_emit_ctx(prog_emitter_t *e, int32_t ctx_ch, int32_t ctx_br) {
    for (int32_t j = 0; j < e->len && !e->fail; j++) {
        int32_t i = e->cone[j];
        if (e->home_ch[i] != ctx_ch ||
            (ctx_ch >= 0 && e->home_br[i] != ctx_br))
            continue;
        const hc_df_node_t *nd = &e->g->nodes[i];
        if (e->has_seg[i] && nd->op == HC_DF_RANGE_CHOICE) {
            int32_t h = e->pos;
            if (h + 4 > e->cap) {
                e->fail = 1;
                return;
            }
            e->out[h] = PROG_RC;
            e->out[h + 1] = i;
            e->pos = h + 4;
            prog_emit_ctx(e, i, 0);
            e->out[h + 2] = e->pos - (h + 4); /* wt */
            prog_emit_ctx(e, i, 1);
            e->out[h + 3] = e->pos - (h + 4) - e->out[h + 2]; /* we */
            e->n_emitted++;
            e->n_controls++;
        } else if (e->has_seg[i] && nd->op == HC_DF_INTERVAL_SELECT) {
            int32_t nf = e->g->ipool[nd->aux];
            int32_t h = e->pos;
            if (h + 3 + nf > e->cap) {
                e->fail = 1;
                return;
            }
            e->out[h] = PROG_IS;
            e->out[h + 1] = i;
            e->out[h + 2] = nf;
            e->pos = h + 3 + nf;
            int32_t seg0 = e->pos;
            for (int32_t k = 0; k < nf; k++) {
                int32_t before = e->pos;
                prog_emit_ctx(e, i, k);
                e->out[h + 3 + k] = e->pos - before;
            }
            (void)seg0;
            e->n_emitted++;
            e->n_controls++;
        } else {
            if (e->pos + 1 > e->cap) {
                e->fail = 1;
                return;
            }
            e->out[e->pos++] = i;
            e->n_emitted++;
        }
    }
}

int hc_df_cone_program(const hc_df_graph_t *g, const int32_t *roots,
                       int32_t n_roots, const int32_t *cone, int32_t len,
                       hc_arena_t *arena, const int32_t **prog_out,
                       int32_t *words_out) {
    *prog_out = NULL;
    *words_out = 0;
    if (len <= 0)
        return 0;

    /* choice 스캔 — 없으면 프로그램 불필요 (플레인 콘 워크와 동일) */
    int32_t n_choice = 0;
    for (int32_t j = 0; j < len; j++) {
        uint8_t op = g->nodes[cone[j]].op;
        n_choice += (op == HC_DF_RANGE_CHOICE || op == HC_DF_INTERVAL_SELECT);
    }
    if (n_choice == 0)
        return 0;

    uint8_t *in_cone = hc_arena_alloc(arena, (size_t)g->n, 1);
    uint8_t *red = hc_arena_alloc(arena, (size_t)g->n, 1);
    uint8_t *has_seg = hc_arena_alloc(arena, (size_t)g->n, 1);
    int32_t *home_ch = hc_arena_alloc(arena, sizeof(int32_t) * (size_t)g->n, 4);
    int32_t *home_br = hc_arena_alloc(arena, sizeof(int32_t) * (size_t)g->n, 4);
    int32_t *best = hc_arena_alloc(arena, sizeof(int32_t) * (size_t)g->n, 4);
    if (!in_cone || !red || !has_seg || !home_ch || !home_br || !best)
        return -1;
    memset(in_cone, 0, (size_t)g->n);
    memset(has_seg, 0, (size_t)g->n);
    for (int32_t j = 0; j < len; j++) {
        in_cone[cone[j]] = 1;
        home_ch[cone[j]] = -1;
        best[cone[j]] = INT32_MAX;
    }

#ifndef NDEBUG
    /* 도달성 엣지가 콘 산출 엣지의 상위집합이므로 (그리고 in_cone 제한
     * 이므로) 루트-도달성 == 콘 이 항상 성립해야 한다. */
    prog_reach(g, in_cone, roots, n_roots, -1, -1, red);
    {
        int32_t cnt = 0;
        for (int32_t i = 0; i < g->n; i++)
            cnt += red[i];
        assert(cnt == len && "cone program: reach != cone");
    }
#endif

    /* 브랜치별 배타 집합 → innermost home 귀속 */
    for (int32_t j = 0; j < len; j++) {
        int32_t             c = cone[j];
        const hc_df_node_t *nd = &g->nodes[c];
        int32_t             brs[16];
        int32_t             n_br = 0;
        if (nd->op == HC_DF_RANGE_CHOICE) {
            if (nd->b == nd->c)
                continue; /* 엣지-삭제 모델 밖 — eager 유지 */
            brs[0] = nd->b;
            brs[1] = nd->c;
            n_br = 2;
        } else if (nd->op == HC_DF_INTERVAL_SELECT) {
            int32_t nf = g->ipool[nd->aux];
            if (nf > 16)
                continue; /* 상한 밖 — eager 유지 */
            int dup = 0;
            for (int32_t a2 = 0; a2 < nf && !dup; a2++)
                for (int32_t b2 = a2 + 1; b2 < nf; b2++)
                    if (g->ipool[nd->aux + 1 + a2] ==
                        g->ipool[nd->aux + 1 + b2]) {
                        dup = 1;
                        break;
                    }
            if (dup)
                continue; /* 중복 함수표 — eager 유지 */
            for (int32_t k = 0; k < nf; k++)
                brs[k] = g->ipool[nd->aux + 1 + k];
            n_br = nf;
        } else {
            continue;
        }
        int any = 0;
        for (int32_t k = 0; k < n_br; k++) {
            prog_reach(g, in_cone, roots, n_roots, c, brs[k], red);
            int32_t excl = 0;
            for (int32_t j2 = 0; j2 < len; j2++)
                excl += !red[cone[j2]];
            if (excl == 0)
                continue;
            any = 1;
            for (int32_t j2 = 0; j2 < len; j2++) {
                int32_t m = cone[j2];
                if (!red[m] && excl < best[m]) {
                    best[m] = excl;
                    home_ch[m] = c;
                    home_br[m] = k;
                }
            }
        }
        has_seg[c] = (uint8_t)any;
    }
    {
        int any_seg = 0;
        for (int32_t j = 0; j < len; j++)
            any_seg |= has_seg[cone[j]];
        if (!any_seg)
            return 0; /* 전 브랜치 배타 0 — 프로그램 이득 없음 */
    }

    /* 방출. 상한: 노드당 1 워드 + 컨트롤당 헤더 (RC 4, IS 3+nf ≤ 19) */
    int32_t  cap = len + n_choice * 20;
    int32_t *out =
        hc_arena_alloc(arena, sizeof(int32_t) * (size_t)cap, 4);
    if (!out)
        return -1;
    prog_emitter_t e = {
        .g = g,
        .cone = cone,
        .len = len,
        .home_ch = home_ch,
        .home_br = home_br,
        .has_seg = has_seg,
        .out = out,
        .pos = 0,
        .cap = cap,
        .n_emitted = 0,
        .n_controls = 0,
        .fail = 0,
    };
    prog_emit_ctx(&e, -1, -1);
    /* cap 은 구조적 상한이라 실패는 산정 버그 — fail-loud */
    assert(!e.fail);
    if (e.fail)
        return 0; /* release: 프로그램 포기 (플레인 콘 워크가 항상 옳다) */
    /* 모든 콘 노드가 정확히 1회 방출됐는가 (플레인 or 컨트롤 ch) */
    assert(e.n_emitted == len);
    *prog_out = out;
    *words_out = e.pos;
    return 0;
}

static void prog_run(const hc_df_graph_t *g, const int32_t *p, int32_t words,
                     double x, double y, double z, double *sc, double *sc2,
                     const hc_df_cellctx_t *cc, const uint8_t *mask) {
    const int32_t *end = p + words;
    while (p < end) {
        int32_t v = *p++;
        if (v >= 0) {
            HC_CTR_INC_HOT(HC_CTR_SP_NODE);
            sc[v] = eval_node(g, v, x, y, z, sc, sc2, cc, mask);
            continue;
        }
        if (v == PROG_RC) {
            int32_t             ch = p[0], wt = p[1], we = p[2];
            const hc_df_node_t *nd = &g->nodes[ch];
            double              d = sc[nd->a];
            /* 선택식은 eval_node 의 RANGE_CHOICE 와 동일 */
            if (d >= nd->k0 && d < nd->k1)
                prog_run(g, p + 3, wt, x, y, z, sc, sc2, cc, mask);
            else
                prog_run(g, p + 3 + wt, we, x, y, z, sc, sc2, cc, mask);
            sc[ch] = eval_node(g, ch, x, y, z, sc, sc2, cc, mask);
            p += 3 + wt + we;
        } else { /* PROG_IS */
            int32_t             ch = p[0], nf = p[1];
            const int32_t      *w = p + 2;
            const hc_df_node_t *nd = &g->nodes[ch];
            double              d = sc[nd->a];
            /* 선택식은 eval_node 의 INTERVAL_SELECT 와 동일 */
            int32_t sel = nf - 1;
            for (int32_t j = 0; j < nf - 1; j++)
                if (d < g->dpool[nd->aux2 + j]) {
                    sel = j;
                    break;
                }
            const int32_t *q = p + 2 + nf;
            int32_t        total = 0;
            for (int32_t k = 0; k < sel; k++)
                q += w[k];
            for (int32_t k = 0; k < nf; k++)
                total += w[k];
            prog_run(g, q, w[sel], x, y, z, sc, sc2, cc, mask);
            sc[ch] = eval_node(g, ch, x, y, z, sc, sc2, cc, mask);
            p += 2 + nf + total;
        }
    }
}

double hc_df_eval_prog(const hc_df_graph_t *g, const int32_t *prog,
                       int32_t words, int32_t root, double x, double y,
                       double z, double *scratch, const hc_df_cellctx_t *cc,
                       const uint8_t *mask) {
    double *sc2 = scratch + g->n;
    prog_run(g, prog, words, x, y, z, scratch, sc2, cc, mask);
    assert(root < 0 || !mask || mask[root]);
    return root >= 0 ? scratch[root] : 0.0;
}

/* --- y-분산 분류 (P2-2, 규칙은 hc_df.h 주석이 SoT) --- */

/* 스플라인 값 경로: coord 나 중첩 val 이 y-가변이면 가변. 중첩 스플라인의
 * coord/val 노드는 스플라인을 참조하는 노드보다 먼저 컴파일되므로
 * (compile_spline_spec 순서) yv 는 이미 확정돼 있다. */
static int spline_y_variant(const hc_df_graph_t *g, int32_t si,
                            const uint8_t *yv) {
    const hc_df_spline_t *s = &g->splines[si];
    if (s->n == 0)
        return 0;
    if (yv[s->coord])
        return 1;
    for (int32_t j = 0; j < s->n; j++)
        if (spline_y_variant(g, s->val[j], yv))
            return 1;
    return 0;
}

void hc_df_mark_y_variant(const hc_df_graph_t *g, uint8_t *yv) {
    for (int32_t i = 0; i < g->n; i++) {
        const hc_df_node_t *nd = &g->nodes[i];
        int                 v;
        switch (nd->op) {
        case HC_DF_Y:
        case HC_DF_Y_CLAMPED_GRADIENT:
        case HC_DF_BLENDED_NOISE:
        case HC_DF_FIND_TOP_SURFACE:
        case HC_DF_NOISE:
            /* 맨 NOISE 는 y_scale==0 이어도 y*0.0 = ±0.0 의 부호가 y 를
             * 따라 들어간다 — 보수적으로 가변 (hc_df.h). */
            v = 1;
            break;
        case HC_DF_SHIFTED_NOISE: {
            /* y 인자 = y*k1 + sc[b]. k1 == 0.0 이고 b 가 CONST +0.0
             * (비트 0) 이면 y*±0.0 ∈ {+0.0,-0.0} 에 +0.0 을 더해 항상
             * +0.0 — y 는 유한 블록 좌표라 비트 불변이다. */
            const hc_df_node_t *bn = &g->nodes[nd->b];
            uint64_t            kb = 0;
            if (bn->op == HC_DF_CONST)
                memcpy(&kb, &bn->k0, sizeof kb);
            if (nd->k1 == 0.0 && bn->op == HC_DF_CONST && kb == 0)
                v = yv[nd->a] || yv[nd->c];
            else
                v = 1;
            break;
        }
        case HC_DF_SPLINE:
            v = spline_y_variant(g, nd->aux, yv);
            break;
        case HC_DF_INTERVAL_SELECT: {
            v = yv[nd->a];
            int32_t nf = g->ipool[nd->aux];
            for (int32_t k = 0; k < nf && !v; k++)
                v = yv[g->ipool[nd->aux + 1 + k]];
            break;
        }
        default:
            /* CONST/X/Z/SHIFT_A/SHIFT_B/BLEND_OFFSET/BLEND_ALPHA 는
             * 피연산자가 없어 불변으로 떨어진다. 마커 포함 나머지는
             * 피연산자 전파 (위상 정렬이라 yv[<i] 확정). */
            v = (nd->a >= 0 && yv[nd->a]) || (nd->b >= 0 && yv[nd->b]) ||
                (nd->c >= 0 && yv[nd->c]);
        }
        yv[i] = (uint8_t)v;
    }
}
