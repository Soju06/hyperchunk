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
    HC_DF_FIND_TOP_SURFACE, /* a=density, b=upper_bound, k0=lower, k1=cell_h.
                             * density 의존 콘은 y-불변/가변 분할 적재 (P2-2):
                             * ipool[aux] = y-불변 노드 수 nI,
                             * ipool[aux+1 .. aux+1+aux2) = 콘 노드 aux2 개
                             * (앞 nI 개 = y-불변, 뒤 = y-가변, 각각 오름차순).
                             * y-불변부는 사다리 진입 시 1회, y-가변부만 매
                             * y 재평가 — 분류는 hc_df_mark_y_variant. */

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

/* hc_df_cone_mark 의 flags 판:
 *  - HC_DF_CONE_WINDOW_SAFE: 모든 평가 좌표가 청크 쿼트 창 안임이 보장된
 *    문맥 (fill_slice 포인트, flat_cache 테이블 구축 포인트) 전용. SP 모드
 *    에서도 FLAT_CACHE 자식을 컷한다 — 창 안은 항상 테이블 히트라 자식
 *    값이 죽은 값이다. 창-밖 폴백이 실재하는 문맥 (psl/erosion/depth 의
 *    aquifer 청크-밖 컬럼 조회) 에 쓰면 안 된다 — eval 의 폴백 mask
 *    assert 가 debug 에서 위반을 잡는다. */
enum { HC_DF_CONE_WINDOW_SAFE = 1u };
int32_t hc_df_cone_mark_ex(const hc_df_graph_t *g, hc_df_mode_t mode,
                           const int32_t *roots, int32_t n_roots,
                           uint8_t *mark, uint32_t flags);

/* --- y-분산 분류 (P2-2) ---
 *
 * yv[i] = 1 이면 노드 i 의 값이 (x,z 고정, y 만 변화) 에서 비트가 달라질
 * 수 있다. 0 이면 어느 y 에서 평가해도 비트 동일 — 컬럼당 1회 평가로
 * 승격 가능 (fill_slice y-루프, FTS 사다리). 분류는 보수적이다:
 *  - 무조건 가변: Y, Y_CLAMPED_GRADIENT, BLENDED_NOISE, FIND_TOP_SURFACE,
 *    맨 NOISE (y_scale==0 이어도 y*0.0 = ±0.0 이 노이즈 인자로 들어가
 *    부호가 y 부호를 따른다 — 비트 불변 증명 불가).
 *  - SHIFTED_NOISE: y_scale k1 == 0.0 이고 shift_y(b) 가 CONST +0.0 이며
 *    shift_x(a)/shift_z(c) 가 y-불변일 때만 불변 — y 인자 = y*±0.0 + (+0.0)
 *    = +0.0 항상 (y 는 유한 블록 좌표 전제). 그 외 가변.
 *  - SPLINE/INTERVAL_SELECT 는 coord/중첩 val/함수 전부 전파.
 *  - 마커(INTERPOLATED/FLAT_CACHE/CACHE_*)는 pass-through 로 자식 전파 —
 *    창-안 테이블 히트는 y-불변이지만 폴백(자식) 기준의 보수 분류가 전
 *    문맥(cc==NULL 포함)에서 안전하다. */
void hc_df_mark_y_variant(const hc_df_graph_t *g, uint8_t *yv);

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

/* --- 콘 프로그램: lazy 브랜치 평가 (P2-4) ---
 *
 * 바닐라 RangeChoice/WeirdScaledSampler(interval_select)는 트리 워크라
 * '선택된 브랜치만' 평가한다. 콘 리스트의 선형 워크는 전 브랜치를 eager
 * 평가한다 — 26.2 슬라이스 콘 실측으로 펄린 샘플의 ~40% 가 죽은-브랜치
 * 값이다. 콘 프로그램은 콘을 세그먼트 구조로 재배열해 그 낭비를 없앤다:
 *
 *  - 브랜치-배타 서브콘 = "choice→branch 엣지 하나를 지우면 루트들에서
 *    도달 불가해지는 콘 노드들". 배타 노드는 그 브랜치가 선택될 때만
 *    평가하면 되고, 비-배타 노드(다른 경로로도 읽힘)는 무조건 평가한다.
 *  - 중첩 (한 배타 집합 안의 choice) 은 집합 포함 관계가 되므로, 각
 *    노드를 '가장 작은 배타 집합' (innermost) 세그먼트에 귀속시키면
 *    세그먼트가 트리로 짜인다. 세그먼트 내부는 노드 오름차순 (위상 순서).
 *  - 값-불변 근거: eval_node 는 순수 (평가 경로 RNG 0회) 이고, 스킵되는
 *    노드의 값은 이 점에서 아무도 읽지 않는다 (도달성 정의). 읽는 노드가
 *    하나라도 있으면 그 노드는 배타가 아니므로 평가된다. 도달성 엣지는
 *    eval_node 가 읽는 엣지의 상위집합 (마커 pass-through 포함 — 과대
 *    연결은 배타 집합을 줄일 뿐, 보수 방향).
 *
 * 스트림 인코딩 (i32):
 *   v >= 0                          : 노드 v 평가
 *   [-1, ch, wt, we][then][else]    : RANGE_CHOICE ch — 선택 세그먼트만
 *                                     실행 후 ch 평가 (wt/we = 워드 수)
 *   [-2, ch, nf, w0..w_{nf-1}][seg0]..[seg_{nf-1}]
 *                                   : INTERVAL_SELECT ch — 동일
 * 선택 판정은 eval_node 의 해당 케이스와 동일 식이다 (같은 d, 같은 경계
 * → 같은 선택). 세그먼트 실행 후 ch 평가가 선택된 값 sc[브랜치] 를 읽고,
 * 비선택 브랜치의 sc 는 스테일이지만 정의상 읽히지 않는다. */

/* 프로그램 산출. cone/roots 는 hc_df_cone_mark(_ex) 산출물과 그 루트.
 * 브랜치 구조가 없거나 이득이 없으면 *prog_out = NULL (플레인 콘 워크
 * 사용). 실패(-1)는 arena 소진뿐. b==c 인 range_choice, 함수표에 같은
 * 노드가 중복 등장하는 interval_select 는 세그먼트화하지 않는다 (엣지
 * 삭제 모델이 성립하지 않는 케이스 — eager 로 두면 항상 옳다). */
int hc_df_cone_program(const hc_df_graph_t *g, const int32_t *roots,
                       int32_t n_roots, const int32_t *cone, int32_t len,
                       hc_arena_t *arena, const int32_t **prog_out,
                       int32_t *words_out);

/* 프로그램 평가 — hc_df_eval_cone 과 같은 계약 (scratch 2*n, root < 0
 * 이면 scratch 만 채움, mask 는 debug assert 전용). 결과는 같은 콘의
 * 플레인 워크와 비트 동일 (df_cones 게이트가 판정). */
double hc_df_eval_prog(const hc_df_graph_t *g, const int32_t *prog,
                       int32_t words, int32_t root, double x, double y,
                       double z, double *scratch, const hc_df_cellctx_t *cc,
                       const uint8_t *mask);

#endif /* HC_DF_H */
