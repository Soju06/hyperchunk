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
    c->promoted = 0;
    c->ppg = 0;
    return 0;
}

/* --- Task 13: postProcess 마킹 레코더 (hc_chunk.h 주석 참조) --- */

#include <stdio.h>
#include <stdlib.h>

int hc_ppg_recorder_init(hc_ppg_recorder_t *r, hc_arena_t *a, int32_t cap) {
    r->recs = (hc_ppg_rec_t *)hc_arena_alloc(
        a, sizeof(hc_ppg_rec_t) * (size_t)cap, _Alignof(hc_ppg_rec_t));
    if (!r->recs)
        return -1;
    r->n = 0;
    r->cap = cap;
    r->frozen = 0;
    return 0;
}

void hc_ppg_mark(hc_ppg_recorder_t *r, int32_t x, int32_t y, int32_t z) {
    if (!r || r->frozen)
        return; /* 기록 끔 / 라이브 단계 (LevelChunk.markPos = no-op) */
    if (y < HC_MIN_Y || y > HC_MAX_Y)
        return; /* isInsideBuildHeight 밖 드롭 (ProtoChunk @2-5) */
    if (r->n >= r->cap) {
        fprintf(stderr, "hc_ppg_mark: recorder capacity exceeded\n");
        abort();
    }
    r->recs[r->n].x = x;
    r->recs[r->n].y = y;
    r->recs[r->n].z = z;
    r->n++;
}
