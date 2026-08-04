#include "hc_carvers.h" /* hc_mth_trig_init */
#include "hc_features.h"

#include <string.h>

/* 07_features 스테이지 — 데코 워크 (ChunkGenerator.applyBiomeDecoration
 * 26.2 재구성, task9pre A2).
 *
 * 순서 계약 (ADR-007 Tier 2 / ADR-008 D2): 호출자가 order.manifest 의
 * seq 순서로 hc_gen_features_chunk 를 부른다. 청크 간 상호작용(3x3 창의
 * 이웃 읽기/스필오버 쓰기)은 그 순서 하에서만 바닐라와 일치한다.
 *
 * 구조물 스텝: 이 그리드에 스타트가 없음을 golden 트레이스가 증명한다
 * (s-라인 0개, task9a A1 §6) — 구조물 루프는 setFeatureSeed 만 하고
 * 아무것도 그리지 않으며 feature RNG 는 아이템마다 재시드라 생략이
 * 무영향이다. 구조물이 있는 리전은 9b+ 스코프. */

int64_t hc_features_decoration_seed(int64_t level_seed, int32_t cx,
                                    int32_t cz) {
    hc_wgr_t r;
    return hc_wgr_set_decoration_seed(&r, level_seed, cx * 16, cz * 16);
}

void hc_gen_features_chunk(hc_feat_region_t *rg, int32_t cx, int32_t cz,
                           int64_t level_seed, const hc_feat_reg_t *reg,
                           const hc_biome_view_t *view,
                           const hc_biome_reg_t *biomes, int32_t sea_level,
                           int32_t walk_max_step,
                           const hc_feat_trace_t *trace) {
    hc_mth_trig_init();
    rg->center_cx = cx;
    rg->center_cz = cz;

    /* ChunkStatusTasks.generateFeatures 진입부: 센터 청크 FINAL 하이트맵
     * 4종을 현재 블록에서 재프라임 (task9pre A4 §4.2 — 이웃 데코가 남긴
     * 증분 상태는 여기서 재계산으로 대체된다; 결과는 등가지만 바닐라
     * 순서를 그대로 둔다). */
    hc_feat_prime_final_maps(hc_feat_region_chunk(rg, cx, cz));

    hc_wgr_t rng;
    int64_t deco = hc_wgr_set_decoration_seed(&rng, level_seed, cx * 16,
                                              cz * 16);

    /* 3x3 이웃의 바이옴 집합 (섹션 팔레트 getAll 의 저장-쿼트 등가 —
     * task9a A1 §7 의 팔레트-vs-저장 열린 질문은 step<=8 게이트에
     * 무영향, 9b 핸드오프 항목) */
    uint64_t bset[(HC_BIOME_MAX + 63) / 64];
    memset(bset, 0, sizeof bset);
    for (int32_t dz = -1; dz <= 1; dz++)
        for (int32_t dx = -1; dx <= 1; dx++) {
            const hc_chunk_t *c =
                hc_feat_region_chunk(rg, cx + dx, cz + dz);
            for (size_t q = 0; q < (size_t)HC_QUARTS; q++) {
                uint16_t b = c->biomes[q];
                bset[b >> 6] |= 1ull << (b & 63);
            }
        }

    int32_t origin_x = cx * 16, origin_y = HC_MIN_Y, origin_z = cz * 16;

    for (int32_t step = 0; step <= walk_max_step && step < HC_FEAT_STEPS;
         step++) {
        /* Task 14: 구조물이 feature 보다 먼저 (R-placement §1) */
        if (rg->struct_step)
            rg->struct_step(rg->struct_ud, rg, cx, cz, deco, step);
        int32_t nf = reg->counts[step];
        if (nf == 0)
            continue;
        /* IntSet 합집합 + 오름차순 (task9pre A2 §2 (b)) */
        uint64_t idxset[(256 + 63) / 64];
        memset(idxset, 0, sizeof idxset);
        for (int32_t bw = 0; bw < (HC_BIOME_MAX + 63) / 64; bw++) {
            uint64_t bits = bset[bw];
            while (bits) {
                int32_t b = bw * 64 + __builtin_ctzll(bits);
                bits &= bits - 1;
                const uint64_t *row =
                    &reg->member[step][(size_t)b * (size_t)reg->words[step]];
                for (int32_t w = 0; w < reg->words[step]; w++)
                    idxset[w] |= row[w];
            }
        }
        for (int32_t i = 0; i < nf; i++) {
            if (!((idxset[i >> 6] >> (i & 63)) & 1u))
                continue;
            hc_wgr_set_feature_seed(&rng, deco, i, step);
            hc_feat_run_placed(rg, &rng, level_seed, reg, view, biomes,
                               sea_level, &reg->steps[step][i], step, i,
                               origin_x, origin_y, origin_z, trace);
        }
    }
}
