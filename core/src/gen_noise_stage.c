#include "hc_gen_noise.h"

/* NoiseBasedChunkGenerator.doFill 26.2 (javap) — 루프 중첩/순서 그대로:
 * cellX 오름 → cellZ 오름 → cellY 내림 → inCellY 내림 → inCellX 오름 →
 * inCellZ 오름. swapSlices 는 매 cellX 뒤 (마지막 포함).
 *
 * 블록 결정 = MaterialRuleList[aquifer, veinifier] 첫 non-null,
 * 전부 null 이면 defaultBlock(stone). air 는 쓰지 않는다 (참조 비교
 * state != AIR — ProtoChunk 초기 상태가 air 라 zero-fill 과 동형).
 *
 * 하이트맵: OCEAN_FLOOR_WG(blocksMotion) → WORLD_SURFACE_WG(!isAir) 순.
 * 물/용암은 blocksMotion=false (LiquidBlock 충돌 형상 empty). */

/* Heightmap.update — 비활성 브랜치(하향 재스캔)까지 완전 이식.
 * pred: 0 = MATERIAL_MOTION_BLOCKING(blocksMotion), 1 = NOT_AIR */
static void hm_update(hc_chunk_t *c, int32_t *hm, int x, int y, int z,
                      uint16_t state, int pred) {
    size_t  col = hc_col_idx(x, z);
    int32_t first = hm[col];
    if (y <= first - 2)
        return;
    int opaque =
        pred ? !hc_block_is_air(state) : hc_block_blocks_motion(state);
    if (opaque) {
        if (y >= first)
            hm[col] = y + 1;
    } else if (first - 1 == y) {
        /* doFill 에서는 도달 불가 (y 단조 감소 + 단일 기록). surface 가
         * 기존 블록을 비-불투명으로 교체할 때(air/water 룰) 활성화된다. */
        for (int yy = y - 1; yy >= HC_MIN_Y; yy--) {
            uint16_t s = c->states[hc_idx(x, yy, z)];
            if (pred ? !hc_block_is_air(s) : hc_block_blocks_motion(s)) {
                hm[col] = yy + 1;
                return;
            }
        }
        hm[col] = HC_MIN_Y;
    }
}

/* ProtoChunk.setBlockState 의 하이트맵 갱신 순서: OCEAN_FLOOR_WG →
 * WORLD_SURFACE_WG (primeHeightmaps 등록 순서). */
void hc_hm_update_both(hc_chunk_t *c, int x, int y, int z, uint16_t state) {
    hm_update(c, c->heightmap_ocean_floor, x, y, z, state, 0);
    hm_update(c, c->heightmap_ws, x, y, z, state, 1);
}

void hc_gen_noise_stage(hc_chunk_t *chunk, hc_noise_chunk_t *nc) {
    /* getOrCreateHeightmapUnprimed: zero-init 스토리지 == 전 컬럼 minY */
    for (int i = 0; i < 256; i++) {
        chunk->heightmap_ocean_floor[i] = HC_MIN_Y;
        chunk->heightmap_ws[i] = HC_MIN_Y;
    }

    hc_nc_initialize_first_cell_x(nc);
    int32_t cw = nc->cell_width;   /* 4 */
    int32_t ch = nc->cell_height;  /* 8 */
    int32_t cells_xz = 16 / cw;    /* 4 */

    for (int32_t cell_x = 0; cell_x < cells_xz; cell_x++) {
        hc_nc_advance_cell_x(nc, cell_x);
        for (int32_t cell_z = 0; cell_z < cells_xz; cell_z++) {
            for (int32_t cell_y = nc->cell_count_y - 1; cell_y >= 0;
                 cell_y--) {
                hc_nc_select_cell_yz(nc, cell_y, cell_z);
                for (int32_t iy = ch - 1; iy >= 0; iy--) {
                    int32_t block_y =
                        (nc->cell_noise_min_y + cell_y) * ch + iy;
                    hc_nc_update_for_y(nc, block_y,
                                       (double)iy / (double)ch);
                    for (int32_t ix = 0; ix < cw; ix++) {
                        int32_t block_x =
                            nc->min_block_x + cell_x * cw + ix;
                        int32_t local_x = block_x & 15;
                        hc_nc_update_for_x(nc, block_x,
                                           (double)ix / (double)cw);
                        for (int32_t iz = 0; iz < cw; iz++) {
                            int32_t block_z =
                                nc->min_block_z + cell_z * cw + iz;
                            int32_t local_z = block_z & 15;
                            hc_nc_update_for_z(nc, block_z,
                                               (double)iz / (double)cw);

                            /* getInterpolatedState → MaterialRuleList:
                             * 1) aquifer.computeSubstance(ctx,
                             *    fullNoiseDensity.compute(ctx)) —
                             *    density 는 selectCellYZ 가 채운
                             *    cacheAllInCell 조회 */
                            double density =
                                nc->density_cell[((ch - 1 -
                                                   nc->cc.in_cell_y) *
                                                      cw +
                                                  nc->cc.in_cell_x) *
                                                     cw +
                                                 nc->cc.in_cell_z];
                            int b = hc_aquifer_substance(
                                &nc->aq, block_x, block_y, block_z, density);
                            if (b < 0) {
                                /* 2) OreVeinifier, 3) null → defaultBlock */
                                b = hc_ore_vein_block(nc, block_x, block_y,
                                                      block_z);
                                if (b < 0)
                                    b = HC_B_STONE;
                            }
                            if (b != HC_B_AIR) {
                                chunk->states[hc_idx(local_x, block_y,
                                                     local_z)] =
                                    (uint16_t)b;
                                hc_hm_update_both(chunk, local_x, block_y,
                                                  local_z, (uint16_t)b);
                                /* doFill 마킹 (Task 13):
                                 * aquifer.shouldScheduleFluidUpdate()
                                 * && !state.getFluidState().isEmpty()
                                 * (NoiseBasedChunkGenerator.doFill
                                 * @455-492) — 자기 청크, 쓴 좌표 그대로 */
                                if (nc->aq.should_schedule_fluid_update &&
                                    hc_block_fluid_nonempty((uint16_t)b))
                                    hc_ppg_mark(chunk->ppg, block_x, block_y,
                                                block_z);
                            }
                        }
                    }
                }
            }
        }
        hc_nc_swap_slices(nc);
    }
    /* stopInterpolation(): 플래그 전용 — 상태 없음 */
}
