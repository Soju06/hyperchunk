#ifndef HC_FEATURES_INTERNAL_H
#define HC_FEATURES_INTERNAL_H

#include "hc_features.h"

/* features.c / features_tree.c 공유 내부 계약 — 코어 밖 비공개. */

typedef struct feat_env {
    hc_feat_region_t      *rg;
    hc_wgr_t              *rng;
    const hc_feat_reg_t   *reg;
    const hc_biome_view_t *view;
    const hc_biome_reg_t  *biomes; /* freeze_top_layer 온도 게이트 */
    int32_t                sea_level;
    const hc_pfeat_t      *pf;
    int32_t                step, index;
    const hc_feat_trace_t *trace;
    int32_t                npos;
    int32_t                placed_any; /* 0/1 */
    int32_t                unknown;    /* 미구현 본문 도달 */
    int32_t                nested; /* PlacedFeature.place 경로 (biome 금지) */
} feat_env_t;

void     hc_featx_die(const char *what, const char *detail);
int      hc_featx_mask_test(const uint64_t *mask, uint16_t id);
int32_t  hc_featx_iprov_sample(hc_wgr_t *r, const hc_iprov_t *p);
uint16_t hc_featx_sprov_sample(hc_wgr_t *r, const hc_sprov_t *p);
/* 중첩 placed feature 실행 (PlacedFeature.place — 트레이스 없음) */
int hc_featx_run_nested(feat_env_t *e, const hc_pfeat_t *pf, int32_t x,
                        int32_t y, int32_t z);

/* features_tree.c — R2 본문 */
int hc_featx_tree_place(feat_env_t *e, int32_t x, int32_t y, int32_t z);
int hc_featx_ftree_place(feat_env_t *e, int32_t x, int32_t y, int32_t z);

#endif /* HC_FEATURES_INTERNAL_H */
