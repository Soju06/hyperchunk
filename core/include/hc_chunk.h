#ifndef HC_CHUNK_H
#define HC_CHUNK_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "hc_arena.h"

/* 26.2 오버월드 y 범위 — golden 스테이지 덤프 실측으로 확정
 * (golden/stages/FORMAT.md: minY -64 maxY 319 height 384, ADR-006 Pitfall 4). */
#define HC_MIN_Y    (-64)
#define HC_MAX_Y    319
#define HC_HEIGHT   384
#define HC_CHUNK_XZ 16
#define HC_BLOCKS   (HC_CHUNK_XZ * HC_CHUNK_XZ * HC_HEIGHT)

/* SoA: 블록당 객체를 만들지 않는다. 팔레트 인덱스 평면 배열만 둔다.
 * ADR-003 D3 — 자바는 청크당 ~40,808 객체를 만든다. 여기서는 0 이다. */
typedef struct {
    int32_t   cx, cz;
    uint16_t *states;                     /* HC_BLOCKS 개 팔레트 인덱스 */
    int32_t   heightmap_ws[256];          /* WORLD_SURFACE(_WG), 컬럼당 1 */
    int32_t   heightmap_ocean_floor[256]; /* OCEAN_FLOOR(_WG) */
} hc_chunk_t;

/* states 를 arena 에서 할당하고 전체를 zero-fill 한다. arena 재사용 시
 * stale 데이터가 새 청크로 새는 것이 패리티 버그의 단골 경로다
 * (ADR-003 Pitfall 3) — init 이 항상 지운다. 소진 시 -1 (abort 아님). */
int hc_chunk_init(hc_chunk_t *c, hc_arena_t *a, int32_t cx, int32_t cz);

/* 블록 선형 인덱스. golden 덤프와 같은 레이아웃이다 (FORMAT.md):
 *   ((y - minY) * 16 + z) * 16 + x
 * x/z 는 청크-로컬 [0,16), y 는 월드 좌표 [HC_MIN_Y, HC_MAX_Y].
 * ADR-009 D3: 범위 검사는 debug 빌드 한정 assert — release(NDEBUG) 는
 * zero-cost 다. */
static inline size_t hc_idx(int x, int y, int z) {
    assert(x >= 0 && x < HC_CHUNK_XZ);
    assert(z >= 0 && z < HC_CHUNK_XZ);
    assert(y >= HC_MIN_Y && y <= HC_MAX_Y);
    return (size_t)(((y - HC_MIN_Y) * HC_CHUNK_XZ + z) * HC_CHUNK_XZ + x);
}

/* 하이트맵 컬럼 인덱스: z * 16 + x */
static inline size_t hc_col_idx(int x, int z) {
    assert(x >= 0 && x < HC_CHUNK_XZ);
    assert(z >= 0 && z < HC_CHUNK_XZ);
    return (size_t)(z * HC_CHUNK_XZ + x);
}

#endif /* HC_CHUNK_H */
