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
    /* --- 07_features (Task 9a: ore/blob 패밀리 + underwater_magma) --- */
    HC_B_ANDESITE,
    HC_B_DIORITE,
    HC_B_CLAY,
    HC_B_COAL_ORE,
    HC_B_DEEPSLATE_COAL_ORE,
    HC_B_IRON_ORE, /* deepslate_iron_ore 는 04_noise 구간에 이미 있다 */
    HC_B_GOLD_ORE,
    HC_B_DEEPSLATE_GOLD_ORE,
    HC_B_REDSTONE_ORE,           /* minecraft:redstone_ore[lit=false] */
    HC_B_DEEPSLATE_REDSTONE_ORE, /* minecraft:deepslate_redstone_ore[lit=false] */
    HC_B_DIAMOND_ORE,
    HC_B_DEEPSLATE_DIAMOND_ORE,
    HC_B_LAPIS_ORE,
    HC_B_DEEPSLATE_LAPIS_ORE,
    HC_B_DEEPSLATE_COPPER_ORE, /* copper_ore 는 04_noise 구간에 이미 있다 */
    HC_B_EMERALD_ORE,
    HC_B_DEEPSLATE_EMERALD_ORE,
    HC_B_MAGMA_BLOCK,
    HC_B_INFESTED_STONE,     /* ore_infested (step 7, 산악) — 그리드 밖 */
    HC_B_INFESTED_DEEPSLATE, /* minecraft:infested_deepslate[axis=y] */
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

/* BlockState.isSolid (legacySolid) — features 검증 스캔(MonsterRoom)이
 * 읽는다. 유체/공기/가루눈만 false — 테이블의 나머지는 전부 solid 광물
 * 블록이다. 26.2 에서 비-solid 인 비유체 블록(잎, glow_lichen, 덩굴 등)
 * 이 테이블에 들어오는 9b 시점에 개별 재검토 필요. */
int hc_block_is_solid(uint16_t id);

/* getFaceOcclusionShape(면) 이 완전 폐색(full block)인지 — 면 무관 근사.
 * 테이블의 블록은 유체/공기/가루눈 제외 전부 완전 큐브다 (underwater_magma
 * 유효성 검사가 읽는다). 부분 형상 블록(dripstone, 버섯 등)이 들어오면
 * 면별 테이블로 승격해야 한다. */
int hc_block_is_full_cube(uint16_t id);

#endif /* HC_BLOCKS_H */
