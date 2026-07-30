#ifndef HC_DF_H
#define HC_DF_H

#include <stdint.h>
#include <stddef.h>

#include "hc_noise.h" /* hc_perlin_t 등 노이즈 스택 (Task 6 에서 분리) */

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
