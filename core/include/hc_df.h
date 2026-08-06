#ifndef HC_DF_H
#define HC_DF_H

#include <stdint.h>
#include <stddef.h>

#include "hc_noise.h"

/* --- density function IR ---
 * 트리 순회 대신 평탄 배열. 노드는 위상 정렬되어 있고 각 노드는
 * 자기보다 낮은 인덱스만 참조한다. Phase 2 SIMD 커널이 동일 IR 을
 * 소비한다. 노드 단위 진입점은 공개 ABI 에 올라가지 않는다 (ADR-003 D2).
 *
 * op 시맨틱은 바닐라 26.2 DensityFunctions 바이트코드(javap) 기준이다.
 * 마커 4종(+interpolated)은 6b 에서는 pass-through 지만 6c 의 셀 캐시
 * 시맨틱이 노드 정체성을 요구하므로 별도 op 로 유지한다. */
typedef enum {
    HC_DF_CONST = 0,
    HC_DF_X, HC_DF_Y, HC_DF_Z,

    /* Ap2 (양쪽 다 비상수). MUL 은 arg1==0.0 단락으로 +0.0 을 돌려준다. */
    HC_DF_ADD, HC_DF_MUL, HC_DF_MIN, HC_DF_MAX,
    /* MulOrAdd (한쪽이 상수일 때 바닐라 codec 이 만드는 별도 노드).
     * input OP k0 — 단락 없음. MUL_CONST(input=0.0, k0<0) = -0.0 이라
     * Ap2 MUL 과 비트가 다르다. 컴파일러가 바닐라 create() 규칙 그대로
     * 분기한다. */
    HC_DF_ADD_CONST, HC_DF_MUL_CONST,

    /* Mapped 단항 */
    HC_DF_ABS, HC_DF_SQUARE, HC_DF_CUBE,
    HC_DF_HALF_NEGATIVE, HC_DF_QUARTER_NEGATIVE,
    HC_DF_SQUEEZE, HC_DF_INVERT,

    HC_DF_CLAMP,              /* a, [k0, k1] — Mth.clamp */
    HC_DF_Y_CLAMPED_GRADIENT, /* Mth.clampedMap(y, k0, k1, k2, k3) */
    HC_DF_RANGE_CHOICE,       /* a=input, b=in, c=out, [k0, k1) */

    HC_DF_NOISE,         /* noises[aux].getValue(x*k0, y*k1, z*k0) */
    HC_DF_SHIFTED_NOISE, /* a/b/c = shift_x/y/z 노드, aux, k0=xz k1=y */
    HC_DF_SHIFT_A,       /* noises[aux](x/4, 0, z/4) * 4 */
    HC_DF_SHIFT_B,       /* noises[aux](z/4, x/4, 0) * 4 */
    HC_DF_BLENDED_NOISE, /* blended[aux].compute(x,y,z) */

    HC_DF_SPLINE,          /* (double)splines[aux].apply — float 산술 */
    HC_DF_INTERVAL_SELECT, /* a=input, ipool[aux..]=함수, dpool[aux2..]=경계, c=개수 */
    HC_DF_FIND_TOP_SURFACE, /* a=density, b=upper_bound, k0=lower, k1=cell_h,
                             * ipool[aux..aux+aux2) = density 의존 콘 */

    /* 신규 월드(Blender.empty()) 고정값 — 6c 블렌딩 도입 시 재검토 */
    HC_DF_BLEND_OFFSET,  /* 0.0 */
    HC_DF_BLEND_ALPHA,   /* 1.0 */
    HC_DF_BLEND_DENSITY, /* pass-through(a) */

    /* 마커 — 6b 는 pass-through(a), 6c 가 셀/컬럼 캐시 시맨틱을 준다 */
    HC_DF_INTERPOLATED, HC_DF_FLAT_CACHE, HC_DF_CACHE_2D,
    HC_DF_CACHE_ONCE, HC_DF_CACHE_ALL_IN_CELL,

    HC_DF_OP_COUNT
} hc_df_op_t;

typedef struct {
    uint8_t op;
    int32_t a, b, c;    /* 피연산자 노드 인덱스, 미사용은 -1 */
    int32_t aux, aux2;  /* op별: noise/spline/blended 인덱스, 풀 오프셋 */
    double  k0, k1, k2, k3; /* 상수 파라미터 (op 별 의미는 위 주석) */
} hc_df_node_t;

/* CubicSpline. 바닐라 샘플링은 전 구간 float 산술이라 (javap 확인)
 * loc/der/상수도 float 로 저장한다. n == 0 이면 Constant(k). values 는
 * 중첩 스플라인이므로 splines[] 인덱스다 (상수 값도 n==0 엔트리로 통일). */
typedef struct {
    int32_t  coord; /* coordinate DF 노드 인덱스 (n > 0 일 때) */
    int32_t  n;     /* 매듭 수, 0 = Constant */
    float    k;     /* n == 0 일 때 값 */
    float   *loc;   /* [n] */
    float   *der;   /* [n] */
    int32_t *val;   /* [n], splines[] 인덱스 */
} hc_df_spline_t;

typedef struct {
    hc_df_node_t       *nodes;
    int32_t             n;
    hc_normal_noise_t  *noises; /* 레지스트리 키 문자열로 dedup (바닐라
                                 * getOrCreateNoise 캐시와 동형) */
    int32_t             n_noises;
    hc_blended_noise_t *blended;
    int32_t             n_blended;
    hc_df_spline_t     *splines;
    int32_t             n_splines;
    int32_t            *ipool; /* interval_select 함수표, FTS 의존 콘 */
    int32_t             n_ipool;
    double             *dpool; /* interval_select 경계값 */
    int32_t             n_dpool;
    int32_t             root;
} hc_df_graph_t;

/* 스칼라 참조 평가기 (Phase 1 은 이것만 쓴다). scratch 는 호출자 제공,
 * 최소 2*n 개 — 뒤쪽 절반은 FIND_TOP_SURFACE 가 density 콘을 다른 y 로
 * 재평가할 때 쓴다. 평가 후 scratch[i] == 노드 i 의 값이므로 여러 슬롯이
 * 한 그래프를 공유할 때 한 번 평가하고 각 root 를 읽으면 된다. */
double hc_df_eval(const hc_df_graph_t *g, double x, double y, double z,
                  double *scratch);

/* --- NoiseChunk 셀 문맥 (6c) ---
 *
 * 바닐라 NoiseChunk.wrapNew 가 마커를 상태 있는 래퍼로 바꾼 것을, 같은
 * IR 위에 '평가 모드 + 노드별 상태' 로 재현한다. 모드는 바닐라의
 * FunctionContext 정체성에 대응한다:
 *
 *   SP    = SinglePointContext. interpolated/cache_* 는 wrapped 로 폴스루,
 *           flat_cache 만 쿼트 테이블 (위치 기반이라 ctx 무관).
 *           [슬라이스 채움, flat_cache 테이블 구축, aquifer/psl 샘플]
 *   CELL  = ctx==NoiseChunk && fillingCell. interpolated → lerp3(셀 코너).
 *   BLOCK = ctx==NoiseChunk && !fillingCell. interpolated → 점진 lerp 값
 *           (updateForY/X/Z 가 갱신). [블록 루프의 vein/barrier 평가]
 *
 * cache_2d/cache_once 는 전 모드 pass-through: 26.2 오버월드 그래프에서
 * 값-중립임을 바이트코드로 증명했다 (cache_2d 하위 트리는 전부 y-독립,
 * cache_once 는 카운터가 점 단위로 증가하는 문맥에서만 활성). 이 증명은
 * 이 라우터 한정이다 — 데이터팩 일반화(Task 12)에서 재검토할 것. */
typedef enum {
    HC_DF_MODE_SP = 0,
    HC_DF_MODE_CELL,
    HC_DF_MODE_BLOCK
} hc_df_mode_t;

typedef struct {
    int32_t node;           /* HC_DF_INTERPOLATED 노드 인덱스 */
    double *slice0, *slice1; /* [(cellCountXZ+1)*(cellCountY+1)], z*(cy+1)+y */
    /* selectCellYZ 가 고른 셀 코너 (자릿수 = x z y) */
    double  n000, n001, n100, n101, n010, n011, n110, n111;
    /* updateForY/X/Z 점진 상태 */
    double  vxz00, vxz10, vxz01, vxz11, vz0, vz1, value;
} hc_df_interp_t;

typedef struct {
    int32_t node;           /* HC_DF_FLAT_CACHE 노드 인덱스 */
    double *values;         /* [(noiseSizeXZ+1)^2], qx*(n+1)+qz */
} hc_df_flat_t;

typedef struct {
    hc_df_mode_t mode;
    /* 청크 쿼트 창 (FlatCache) */
    int32_t first_noise_x, first_noise_z, noise_size_xz;
    /* 셀 기하 */
    int32_t cell_width, cell_height;
    /* 현재 셀 내 좌표 (CELL 모드 lerp3 델타) */
    int32_t in_cell_x, in_cell_y, in_cell_z;
    /* 노드 → 상태 디스패치. 길이 g->n, 해당 없으면 -1 */
    const int32_t       *interp_of;
    const int32_t       *flat_of;
    const hc_df_interp_t *interp;
    const hc_df_flat_t   *flat;
} hc_df_cellctx_t;

/* cc == NULL 이면 hc_df_eval 과 동일 (마커 전부 pass-through — 6b 라우터
 * 골든이 이 경로를 고정한다). x/y/z 는 블록 좌표 (정수값이어야 한다 —
 * FlatCache 쿼트 계산이 int 캐스트를 쓴다). */
double hc_df_eval_ex(const hc_df_graph_t *g, double x, double y, double z,
                     double *scratch, const hc_df_cellctx_t *cc);

/* --- (root×mode) 라이브-콘 평가 (P2-1) ---
 *
 * 프리픽스 [0..root] 워크는 root 의 실제 의존 콘 대비 수십~수백 배의
 * 죽은 노드를 평가한다 (P2-0 §5). 콘 리스트(오름차순 노드 인덱스 ==
 * 위상 순서)를 IR 빌드 시 1회 산출해 두고 eval 은 콘만 방문한다.
 * 값-불변 최적화다: 평가 경로에 RNG 가 없고 (시딩은 전부 컴파일 타임)
 * eval_node 는 순수라 죽은 값 생략은 관측 불가. 남는 노드의 연산
 * 순서는 프리픽스 워크와 동일하다 (둘 다 인덱스 오름차순).
 *
 * 모드별 컷 규칙 (hc_df_cellctx_t 모드 시맨틱과 1:1):
 *  - INTERPOLATED: CELL/BLOCK 은 셀 상태(lerp3/점진 lerp)만 읽는다 —
 *    자식 컷. SP 는 pass-through — 자식 포함.
 *  - FLAT_CACHE: CELL/BLOCK 평가 좌표는 청크 안(쿼트 창 안) 보장이라
 *    테이블 히트만 발생 — 자식 컷. SP 는 창-밖 폴백(aquifer 의 청크 밖
 *    컬럼 조회)이 실재하므로 자식을 '보수적으로 포함'하고, eval 의
 *    폴백 경로 mask assert 가 두 가정 모두를 감시한다.
 *  - FIND_TOP_SURFACE: density(a) 는 자체 ipool 콘이 sc2 로 자족
 *    평가된다 — b(upper_bound)만 포함. FTS 는 SP 콘 전용 (바닐라가
 *    fresh SinglePointContext 로 평가) — 비-SP 도달은 -1 fail-loud.
 *
 * 전제: 그래프의 모든 INTERPOLATED/FLAT_CACHE 마커가 셀 상태를 갖는
 * 문맥(cc)에서 쓴다 — hc_nc_init 이 전 마커에 상태를 부여한다 (바닐라
 * mapAll 과 동형). cc == NULL 문맥은 콘을 쓰지 말 것 (마커가 전부
 * pass-through 라 SP 규칙 외 컷이 성립하지 않는다). */

/* roots 의 mode 별 의존 콘을 mark ([g->n], 호출자 0-초기화) 에 마크하고
 * 콘 크기를 돌려준다. -1 = FTS 가 비-SP 콘에 도달 (지원 밖). */
int32_t hc_df_cone_mark(const hc_df_graph_t *g, hc_df_mode_t mode,
                        const int32_t *roots, int32_t n_roots, uint8_t *mark);

/* mark 를 오름차순 리스트로 수집. out 용량 = hc_df_cone_mark 반환값 */
void hc_df_cone_collect(const hc_df_graph_t *g, const uint8_t *mark,
                        int32_t *out);

/* 콘 평가. cone 은 오름차순 (== 위상 순서), scratch 요건은 hc_df_eval
 * 과 동일 (2*n — 뒤 절반은 FTS 용). root < 0 이면 scratch 만 채운다
 * (다중-루트 단일-워크: 호출자가 scratch[각 root] 를 읽는다 —
 * fill_slice). mask 는 debug assert 전용 (NULL 허용): 마커 폴백이 콘
 * 밖 노드(스테일 scratch)를 읽으면 발화한다. */
double hc_df_eval_cone(const hc_df_graph_t *g, const int32_t *cone,
                       int32_t n_cone, int32_t root, double x, double y,
                       double z, double *scratch, const hc_df_cellctx_t *cc,
                       const uint8_t *mask);

#endif /* HC_DF_H */
