#include "hc_chunk.h"

#include <string.h>

int hc_chunk_init(hc_chunk_t *c, hc_arena_t *a, int32_t cx, int32_t cz) {
    c->cx = cx;
    c->cz = cz;
    /* 64바이트 정렬: 캐시라인 경계 + Phase 2 SIMD 로드 준비 */
    c->states = (uint16_t *)hc_arena_alloc(a, sizeof(uint16_t) * HC_BLOCKS, 64);
    if (!c->states)
        return -1;
    /* 항상 zero-fill. arena 재사용 시 stale 데이터가 새 청크로 새면
     * 산발적 패리티 버그가 된다 (ADR-003 Pitfall 3). */
    memset(c->states, 0, sizeof(uint16_t) * HC_BLOCKS);
    memset(c->heightmap_ws, 0, sizeof c->heightmap_ws);
    memset(c->heightmap_ocean_floor, 0, sizeof c->heightmap_ocean_floor);
    memset(c->heightmap_final, 0, sizeof c->heightmap_final);
    c->hm_final_primed = 0;
    memset(c->heightmap_wg_reprimed, 0, sizeof c->heightmap_wg_reprimed);
    c->wg_reprimed = 0;
    memset(c->biomes, 0, sizeof c->biomes);
    return 0;
}
