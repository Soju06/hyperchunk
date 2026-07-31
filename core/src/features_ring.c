/* Task 10 링 프리픽스 본문: disk / seagrass (이후 lake / root_system /
 * geode 가 여기 합류).
 *
 * 시맨틱 = server-26.2.jar 바이트코드 (본 세션 javap; 인용은 각 함수):
 * 09_light 게이트는 그리드 라이트가 링 청크 상태(±15블록 셸)를 읽기
 * 때문에 프리픽스의 링 데코를 draw-exact 로 재생해야 한다. */

#include "features_internal.h"

#include <stdlib.h>

/* --- minecraft:disk (DiskFeature.place @0-204 + placeColumn @0-102) ---
 *
 * r = radius.sample (1 드로우); betweenClosed(origin±(r,0,r)) 순회
 * (z 밖, x 안 — y 는 단일 평면), dx^2+dz^2 > r^2 컬럼 스킵;
 * placeColumn: y = origin.y+half_height 부터 origin.y-half_height-1 초과
 * 까지 내려가며 target 술어 통과 셀에 RuleBasedStateProvider.getState
 * (rules 순서 if_true 첫 히트의 then, 아니면 fallback — 드로우는 각
 * sprov 종류에 따름; disk_* 데이터는 전부 simple = 0 드로우) 를 쓴다.
 * markAboveForPostProcessing 은 월드 쓰기가 없다 (10→11 blocks 0-diff,
 * R6 §6) — 생략. */
int hc_featx_disk_place(feat_env_t *e, const hc_disk_cfg_t *c, int32_t x,
                        int32_t y, int32_t z) {
    int32_t top = y + c->half_height;
    int32_t bottom = y - c->half_height - 1;
    int32_t r = hc_featx_iprov_sample(e->rng, &c->radius);
    int     placed = 0;
    for (int32_t dz = -r; dz <= r; dz++)
        for (int32_t dx = -r; dx <= r; dx++) {
            if (dx * dx + dz * dz > r * r)
                continue;
            int32_t px = x + dx, pz = z + dz;
            for (int32_t py = top; py > bottom; py--) {
                if (!hc_featx_bpred_eval(e, &c->target, px, py, pz))
                    continue;
                const hc_sprov_t *sp = &c->fallback;
                for (int32_t i = 0; i < c->n_rules; i++)
                    if (hc_featx_bpred_eval(e, &c->rules[i].if_true, px, py,
                                            pz)) {
                        sp = &c->rules[i].then;
                        break;
                    }
                uint16_t st = hc_featx_sprov_sample(e->rng, sp);
                hc_feat_set_block(e->rg, px, py, pz, st);
                placed = 1;
            }
        }
    return placed;
}

/* --- minecraft:seagrass (SeagrassFeature.place @0-300) ---
 *
 * 드로우: nextInt(8)-nextInt(8) (dx), nextInt(8)-nextInt(8) (dz);
 * y = getHeight(OCEAN_FLOOR [FINAL live], ox+dx, oz+dz);
 * 대상이 물이면 nextDouble() < probability 로 tall 여부 (@139-152).
 * canSurvive: 아래 블록 isFaceSturdy(UP) && !#cannot_support_seagrass
 * (Seagrass/TallSeagrassBlock.mayPlaceOn @0-27; tall lower 는 추가로
 * 위치 유체가 물+full — 대상이 이미 물이라 자명). tall 이면 위 칸도
 * 물이어야 lower/upper 를 flag 2 로 쓴다 (@242-254); 아니면 seagrass
 * 단일 (@271-). */
int hc_featx_seagrass_place(feat_env_t *e, const hc_seagrass_cfg_t *c,
                            int32_t x, int32_t y, int32_t z) {
    (void)y;
    int32_t dx = hc_wgr_next_int(e->rng, 8) - hc_wgr_next_int(e->rng, 8);
    int32_t dz = hc_wgr_next_int(e->rng, 8) - hc_wgr_next_int(e->rng, 8);
    int32_t px = x + dx, pz = z + dz;
    int32_t py = hc_feat_height(e->rg, HC_HM_OCEAN_FLOOR, px, pz);
    if (py < HC_MIN_Y || py > HC_MAX_Y)
        return 0;
    if (hc_feat_get_block(e->rg, px, py, pz) != HC_B_WATER)
        return 0;
    int tall = hc_wgr_next_double(e->rng) < (double)c->probability;
    uint16_t below = (py - 1 >= HC_MIN_Y)
                         ? hc_feat_get_block(e->rg, px, py - 1, pz)
                         : HC_B_AIR;
    int may_place = hc_featx_face_sturdy_full(below, /*UP*/ 1) &&
                    !hc_featx_mask_test(e->reg->tag_cannot_support_seagrass,
                                        below);
    if (!may_place)
        return 0;
    if (tall) {
        if (py + 1 > HC_MAX_Y ||
            hc_feat_get_block(e->rg, px, py + 1, pz) != HC_B_WATER)
            return 0;
        hc_feat_set_block(e->rg, px, py, pz, HC_B_TALL_SEAGRASS_LOWER);
        hc_feat_set_block(e->rg, px, py + 1, pz, HC_B_TALL_SEAGRASS_UPPER);
        return 1;
    }
    hc_feat_set_block(e->rg, px, py, pz, HC_B_SEAGRASS);
    return 1;
}
