#include "hc_df.h"

/* 의도적 스텁 (TDD RED) — GREEN 커밋에서 실제 구현으로 교체된다. */

double hc_df_eval(const hc_df_graph_t *g, double x, double y, double z,
                  double *scratch) {
    (void)x;
    (void)y;
    (void)z;
    for (int32_t i = 0; i < g->n; i++)
        scratch[i] = 0.0;
    return scratch[g->root];
}
