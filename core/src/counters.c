/* B-3 연산 카운터 구현 — hc_counters.h 주석이 SoT. */
#include "hc_counters.h"

#include <stdatomic.h>

int                    hc_ctr_on;
_Thread_local uint64_t hc_ctr_tls[HC_CTR_N];

static _Atomic uint64_t g_tot[HC_CTR_N];

static const char *const g_names[HC_CTR_N] = {
    [HC_CTR_ZOOM_Q] = "zoom_q",
    [HC_CTR_ZOOM_MISS] = "zoom_miss",
    [HC_CTR_SURF_COND] = "surf_cond",
    [HC_CTR_SURF_APPLY] = "surf_apply",
    [HC_CTR_SURF_ROOT] = "surf_root",
    [HC_CTR_SURF_TOPMAT] = "surf_topmat",
    [HC_CTR_SURF_YITER] = "surf_yiter",
    [HC_CTR_SURF_SAMP_MISS] = "surf_samp_miss",
    [HC_CTR_SURF_SEC_MISS] = "surf_sec_miss",
    [HC_CTR_SURF_MSL_MISS] = "surf_msl_miss",
    [HC_CTR_SURF_STEEP_MISS] = "surf_steep_miss",
    [HC_CTR_X4_SLICE] = "x4_slice",
    [HC_CTR_X4_CELL] = "x4_cell",
    [HC_CTR_X4_NODE] = "x4_node",
    [HC_CTR_X4_PERLIN] = "x4_perlin",
    [HC_CTR_X4_SCALAR_FB] = "x4_scalar_fb",
    [HC_CTR_X4_RC_MIX] = "x4_rc_mix",
    [HC_CTR_X4_IS_MIX] = "x4_is_mix",
    [HC_CTR_X4_BLEND_MIX] = "x4_blend_mix",
    [HC_CTR_SP_NODE] = "sp_node",
    [HC_CTR_FTS_ITER] = "fts_iter",
    [HC_CTR_DF_AQ_EROSION] = "df_aq_erosion",
    [HC_CTR_DF_AQ_DEPTH] = "df_aq_depth",
    [HC_CTR_DF_AQ_FLOOD] = "df_aq_flood",
    [HC_CTR_DF_AQ_SPREAD] = "df_aq_spread",
    [HC_CTR_DF_AQ_LAVA] = "df_aq_lava",
    [HC_CTR_DF_AQ_BARRIER] = "df_aq_barrier",
    [HC_CTR_AQ_FS_MISS] = "aq_fs_miss",
    [HC_CTR_AQ_SOLID] = "aq_solid",
    [HC_CTR_DF_VEIN_TOGGLE] = "df_vein_toggle",
    [HC_CTR_DF_VEIN_RIDGED] = "df_vein_ridged",
    [HC_CTR_VEIN_PRE_GAP] = "vein_pre_gap",
    [HC_CTR_DF_PSL_MISS] = "df_psl_miss",
    [HC_CTR_L_SKY_SEED] = "l_sky_seed",
    [HC_CTR_L_SKY_POP] = "l_sky_pop",
    [HC_CTR_L_BLK_SEED] = "l_blk_seed",
    [HC_CTR_L_BLK_POP] = "l_blk_pop",
    [HC_CTR_L_FLUSH_DEC] = "l_flush_dec",
    [HC_CTR_L_FLUSH_INC] = "l_flush_inc",
    [HC_CTR_L_CHECK] = "l_check",
    [HC_CTR_L_DIRTY_CH] = "l_dirty_ch",
    [HC_CTR_L_DERIVE_08] = "l_derive_08",
    [HC_CTR_L_DERIVE_FL] = "l_derive_flush",
    [HC_CTR_L_PREP_SKIP] = "l_prep_skip",
    [HC_CTR_L_PREP_SCAN] = "l_prep_scan",
    [HC_CTR_FEAT_SETBLK] = "feat_setblk",
    [HC_CTR_FEAT_ATTEMPT] = "feat_attempt",
    [HC_CTR_STRUCT_STEP] = "struct_step",
};

void hc_ctr_enable(void) {
    hc_ctr_on = 1;
}

void hc_ctr_flush(void) {
    if (!hc_ctr_on)
        return;
    for (int i = 0; i < HC_CTR_N; i++) {
        if (hc_ctr_tls[i]) {
            atomic_fetch_add_explicit(&g_tot[i], hc_ctr_tls[i],
                                      memory_order_relaxed);
            hc_ctr_tls[i] = 0;
        }
    }
}

uint64_t hc_ctr_total(int ev) {
    return atomic_load_explicit(&g_tot[ev], memory_order_relaxed);
}

const char *hc_ctr_name(int ev) {
    return g_names[ev];
}
