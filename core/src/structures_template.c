/* Task 14 — 템플릿 계열 배치 (작성 중 스텁; hc_structures.h 계약).
 * 최종 구현이 이 파일을 대체한다. 도달 시 die. */
#include "hc_structures.h"
#include "features_internal.h"

const hc_template_t *hc_template_load(hc_arena_t *a, const char *dir,
                                      const char *name) {
    (void)a;
    (void)dir;
    hc_featx_die("hc_template_load stub", name);
    return NULL;
}

void hc_splace_template(hc_sctx_t *sc, hc_feat_region_t *rg,
                        hc_sstart_t *start, hc_spiece_t *p, hc_wgr_t *rng,
                        int32_t cx, int32_t cz) {
    (void)sc; (void)rg; (void)start; (void)p; (void)rng; (void)cx; (void)cz;
    hc_featx_die("hc_splace_template stub", NULL);
}
