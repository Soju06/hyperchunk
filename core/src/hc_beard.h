#ifndef HC_BEARD_H
#define HC_BEARD_H

#include <stdint.h>

/* Beardifier 26.2 (javap/디컴파일 핀) — 구조물 terrain_adaptation 의
 * final_density 기여. NoiseChunk 는 cacheAllInCell(final_density +
 * beardifier) 를 셀 채움에서 평가한다 (noise_chunk.c 의 select_cell_yz).
 *
 * 구성 (Beardifier.forStructuresInChunk):
 *  - 청크 References 의 스타트들 중 terrainAdaptation != NONE 만.
 *  - 피스가 isCloseToChunk(chunk, 12) (XZ, bb 12 인플레이트 교차) 면:
 *    · PoolElementStructurePiece + RIGID projection → rigid (bb,
 *      adjustment, groundLevelDelta) / 비-풀 피스 → rigid (gld=0).
 *    · 풀 피스의 junction 들 중 source 가 (minX-12, minZ-12) 초과 ~
 *      (minX+27, minZ+27) 미만 (양끝 strict) → junction (0.4 beard).
 *  - affectedBox = (rigid bb ∪ junction 점상자) inflatedBy(24);
 *    비어 있으면 EMPTY (compute == 정확히 0.0). */

enum {
    HC_TA_NONE = 0,
    HC_TA_BURY,
    HC_TA_BEARD_THIN,
    HC_TA_BEARD_BOX,
    HC_TA_ENCAPSULATE,
};

typedef struct {
    int32_t bb[6]; /* minX minY minZ maxX maxY maxZ */
    int32_t gld;   /* groundLevelDelta */
    uint8_t adj;   /* HC_TA_* */
} hc_beard_rigid_t;

typedef struct {
    int32_t sx, sgy, sz; /* JigsawJunction source X / groundY / Z */
} hc_beard_junction_t;

typedef struct {
    uint8_t                    has_any; /* affectedBox != null */
    int32_t                    affected[6];
    int32_t                    n_rigids;
    const hc_beard_rigid_t    *rigids;
    int32_t                    n_junctions;
    const hc_beard_junction_t *junctions;
} hc_beard_t;

/* Beardifier.compute — affectedBox 밖/EMPTY 는 정확히 +0.0. 합산 순서 =
 * rigid 리스트 순 → junction 리스트 순 (전부 double). */
double hc_beard_compute(const hc_beard_t *b, int32_t x, int32_t y, int32_t z);

#endif /* HC_BEARD_H */
