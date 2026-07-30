#ifndef HC_BLOCKS_H
#define HC_BLOCKS_H

#include <stdint.h>

/* 내부 블록 id 테이블 — 내부 전용 (core/src). 공개 ABI 아님 (ADR-003 D2).
 *
 * hc_chunk_t.states 의 zero-fill == HC_B_AIR (바닐라 ProtoChunk 초기 상태).
 * 앞쪽 10개는 04_noise 스테이지가 커밋한 순서 그대로다 — 재배열 금지
 * (noise 골든 게이트가 이 순서로 검증됐다). 이후는 05_surface 가 쓸 수
 * 있는 블록 전부 (surface_rule 트리의 result_state + clay bands +
 * SurfaceSystem 상수 SNOW_BLOCK/PACKED_ICE). */
enum {
    HC_B_AIR = 0,
    HC_B_STONE,
    HC_B_WATER, /* minecraft:water[level=0] */
    HC_B_LAVA,  /* minecraft:lava[level=0] */
    HC_B_COPPER_ORE,
    HC_B_RAW_COPPER_BLOCK,
    HC_B_GRANITE,
    HC_B_DEEPSLATE_IRON_ORE,
    HC_B_RAW_IRON_BLOCK,
    HC_B_TUFF,
    /* --- 05_surface --- */
    HC_B_BEDROCK,
    HC_B_DEEPSLATE, /* minecraft:deepslate[axis=y] */
    HC_B_DIRT,
    HC_B_GRASS_BLOCK, /* minecraft:grass_block[snowy=false] */
    HC_B_COARSE_DIRT,
    HC_B_SAND,
    HC_B_SANDSTONE,
    HC_B_RED_SAND,
    HC_B_RED_SANDSTONE,
    HC_B_GRAVEL,
    HC_B_MUD,
    HC_B_MYCELIUM, /* minecraft:mycelium[snowy=false] */
    HC_B_PODZOL,   /* minecraft:podzol[snowy=false] */
    HC_B_SNOW_BLOCK,
    HC_B_POWDER_SNOW,
    HC_B_ICE,
    HC_B_PACKED_ICE,
    HC_B_CALCITE,
    HC_B_TERRACOTTA,
    HC_B_WHITE_TERRACOTTA,
    HC_B_ORANGE_TERRACOTTA,
    HC_B_YELLOW_TERRACOTTA,
    HC_B_BROWN_TERRACOTTA,
    HC_B_RED_TERRACOTTA,
    HC_B_LIGHT_GRAY_TERRACOTTA,
    HC_B_SULFUR,
    HC_B_CINNABAR,
    HC_B_COUNT
};

/* 캐노니컬 직렬화 (BlockStateParser.serialize 형태 — golden 팔레트와
 * 같은 문자열). NULL 없음 — 범위 밖은 호출자 버그다. */
const char *hc_block_name(uint16_t id);

/* 캐노니컬 문자열 → id. 모르면 -1 (호출자가 fail-loud). len 은 name 의
 * 길이 (NUL 종단 불필요 — JSON 문자열이 버퍼 슬라이스다). */
int32_t hc_block_by_name(const char *name, int32_t len);

/* BlockState.isAir / FluidState 비었는지 / blocksMotion.
 * blocksMotion: 유체/공기/가루눈만 false — 가루눈(powder_snow)은 충돌
 * 형상이 비어 MOTION_BLOCKING 계열에서 제외된다 (골든 미커버 — 26.2
 * 바이트코드의 블록 등록 플래그로 재확인 전까지 주의). */
int hc_block_is_air(uint16_t id);
int hc_block_is_fluid(uint16_t id);
int hc_block_blocks_motion(uint16_t id);

#endif /* HC_BLOCKS_H */
