#ifndef HC_DF_H
#define HC_DF_H

#include <stdint.h>
#include <stddef.h>

/* --- 바닐라 26.2 ImprovedNoise (스칼라 Perlin) ---
 *
 * 시맨틱은 26.2 비난독화 클래스(tools/golden/work/server)를 javap 로
 * 확인했고, golden/rng/perlin_seed1234567890.txt 로 비트단위 검증된다
 * (tests/unit/test_perlin.c).
 *
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
/* ImprovedNoise.noise(x,y,z) == noise(x,y,z,0,0): yScale=0 경로 */
double hc_perlin_sample(const hc_perlin_t *p, double x, double y, double z);

/* --- density function IR ---
 * 트리 순회 대신 평탄 배열. 노드는 위상 정렬되어 있고 각 노드는
 * 자기보다 낮은 인덱스만 참조한다. Phase 2 SIMD 커널이 동일 IR 을
 * 소비한다. 노드 단위 진입점은 공개 ABI 에 올라가지 않는다 (ADR-003 D2). */
typedef enum {
    HC_DF_CONST = 0,
    HC_DF_X, HC_DF_Y, HC_DF_Z,
    HC_DF_NOISE,
    HC_DF_ADD, HC_DF_MUL, HC_DF_MIN, HC_DF_MAX,
    HC_DF_CLAMP,
    HC_DF_Y_CLAMPED_GRADIENT,
    HC_DF_SPLINE,
    HC_DF_OP_COUNT
} hc_df_op_t;

typedef struct {
    uint8_t op;
    int32_t a, b;            /* 피연산자 노드 인덱스, 미사용은 -1 */
    double  k0, k1, k2, k3;  /* 상수 파라미터 (op 별 의미는 df_eval.c) */
    int32_t noise_id;        /* HC_DF_NOISE 일 때 noises[] 인덱스 */
} hc_df_node_t;

typedef struct {
    hc_df_node_t *nodes;
    int32_t       n;
    hc_perlin_t  *noises;
    int32_t       n_noises;
    int32_t       root;
} hc_df_graph_t;

/* 스칼라 참조 평가기 (Phase 1 은 이것만 쓴다). scratch 는 호출자 제공,
 * 노드 수 이상. 평가 후 scratch[i] == 노드 i 의 값. */
double hc_df_eval(const hc_df_graph_t *g, double x, double y, double z,
                  double *scratch);

#endif /* HC_DF_H */
