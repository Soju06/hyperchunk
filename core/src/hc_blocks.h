#ifndef HC_BLOCKS_H
#define HC_BLOCKS_H

#include <stdint.h>

/* T14-GEN BEGIN count */
#define HC_B_T14_COUNT 650
/* T14-GEN END count */

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
    /* --- 07_features step 9 (Task 9b: 초목/나무/lush caves) ---
     * 파라미터 블록은 BASE + 오프셋 공식. 오프셋 배치는 blocks.c NAMES 와
     * 1:1 (생성 스크립트 순서) — 재배열 금지. 캐노니컬 문자열은 골든 07
     * 팔레트와 동일 (프로퍼티 알파벳 순, 전 프로퍼티 인쇄). */
    HC_B_SHORT_GRASS,
    HC_B_FERN,
    HC_B_POPPY,
    HC_B_DANDELION,
    HC_B_MOSS_BLOCK,
    HC_B_MOSS_CARPET,
    HC_B_AZALEA,
    HC_B_FLOWERING_AZALEA,
    HC_B_SPORE_BLOSSOM,
    HC_B_TALL_GRASS_LOWER,
    HC_B_TALL_GRASS_UPPER,
    HC_B_OAK_LOG_X, /* axis x,y,z 순 */
    HC_B_OAK_LOG_Y,
    HC_B_OAK_LOG_Z,
    HC_B_JUNGLE_LOG_X,
    HC_B_JUNGLE_LOG_Y,
    HC_B_JUNGLE_LOG_Z,
    /* leaves: BASE + wl*7 + (distance-1), distance 1..7, persistent=false */
    HC_B_OAK_LEAVES_BASE,
    HC_B_JUNGLE_LEAVES_BASE = HC_B_OAK_LEAVES_BASE + 14,
    /* vine: 단일 face — E,N,S,U,W 순 (월드젠은 단면만 쓴다) */
    HC_B_VINE_BASE = HC_B_JUNGLE_LEAVES_BASE + 14,
    /* cocoa: BASE + age*4 + facing (N,E,S,W), age 0..2 */
    HC_B_COCOA_BASE = HC_B_VINE_BASE + 5,
    /* bamboo: BASE + age*6 + leaves*2 + stage; age 0..1, leaves
     * none/small/large, stage 0..1 */
    HC_B_BAMBOO_BASE = HC_B_COCOA_BASE + 12,
    /* cave_vines(머리): BASE + berries*26 + age (0..25) */
    HC_B_CAVE_VINES_BASE = HC_B_BAMBOO_BASE + 12,
    HC_B_CAVE_VINES_PLANT_BASE = HC_B_CAVE_VINES_BASE + 52, /* + berries */
    /* glow_lichen: BASE + wl*63 + (facemask-1); facemask 비트 = down,
     * east, north, south, up, west (LSB→MSB) */
    HC_B_GLOW_LICHEN_BASE = HC_B_CAVE_VINES_PLANT_BASE + 2,
    /* big_dripleaf (tilt=none 고정): BASE + facing*2 + wl */
    HC_B_BIG_DRIPLEAF_BASE = HC_B_GLOW_LICHEN_BASE + 126,
    HC_B_BIG_DRIPLEAF_STEM_BASE = HC_B_BIG_DRIPLEAF_BASE + 8,
    /* small_dripleaf: BASE + facing*4 + half*2 + wl (half lower,upper) */
    HC_B_SMALL_DRIPLEAF_BASE = HC_B_BIG_DRIPLEAF_STEM_BASE + 8,
    /* fallen_tree attached_to_logs 데코레이터 (R2 §11/§13) */
    HC_B_RED_MUSHROOM = HC_B_SMALL_DRIPLEAF_BASE + 16,
    HC_B_BROWN_MUSHROOM,
    /* --- Task 10: 링 프리픽스 본문 (disk/seagrass/lake/root_system/geode) --- */
    HC_B_ROOTED_DIRT,
    HC_B_HANGING_ROOTS,    /* waterlogged=false */
    HC_B_HANGING_ROOTS_WL, /* waterlogged=true */
    HC_B_SEAGRASS,
    HC_B_TALL_SEAGRASS_LOWER,
    HC_B_TALL_SEAGRASS_UPPER,
    HC_B_MELON,
    HC_B_SMOOTH_BASALT,
    HC_B_AMETHYST_BLOCK,
    HC_B_BUDDING_AMETHYST,
    /* azalea 잎: BASE + flowering*14 + wl*7 + (distance-1) — 기존 잎 공식과
     * 동일 (distance 1..7, persistent=false) */
    HC_B_AZALEA_LEAVES_BASE,
    HC_B_FLOWERING_AZALEA_LEAVES_BASE = HC_B_AZALEA_LEAVES_BASE + 14,
    /* amethyst 싹/클러스터: BASE + size*12 + facing*2 + wl;
     * size 0=small_bud,1=medium_bud,2=large_bud,3=cluster;
     * facing 인덱스 = D,U,N,S,W,E (블록 내부 규약) */
    HC_B_AMETHYST_BUD_BASE = HC_B_FLOWERING_AZALEA_LEAVES_BASE + 14,
    /* lake 가 상반부에 cave_air 를 쓴다 (LakeFeature <clinit> — R5b).
     * BlockState.isAir 참: hc_block_is_air 가 포함한다. */
    HC_B_CAVE_AIR = HC_B_AMETHYST_BUD_BASE + 48,
    /* monster_room (던전 — R5d): 방 셸/바닥 + 스포너 + 전리품 상자.
     * chest 는 BASE + facing (N,S,W,E — StructurePiece.reorient 가 고른다;
     * type=single, waterlogged=false 고정: 월드젠은 공기에만 놓는다). */
    HC_B_COBBLESTONE,
    HC_B_MOSSY_COBBLESTONE,
    HC_B_SPAWNER,
    HC_B_CHEST_BASE, /* +0 N, +1 S, +2 W, +3 E */
    /* 밴드 링 초목 (patch_bush/pumpkin/sugar_cane/firefly + kelp_warm):
     * 등록 세그먼트는 Blocks.<clinit> bc 4424(bush)/12468(pumpkin)/
     * 21014(kelp)/21058(kelp_plant)/31766(firefly_bush). bush 만
     * replaceable(); kelp/kelp_plant 는 유체가 항상 물 (KelpBlock
     * .getFluidState — seagrass 와 동류). */
    HC_B_BUSH = HC_B_CHEST_BASE + 4,
    HC_B_PUMPKIN,
    HC_B_SUGAR_CANE,    /* [age=0] — 월드젠 유일 상태 */
    HC_B_FIREFLY_BUSH,  /* 발광 2 (R4 §4 lambda$static$410) */
    HC_B_KELP_BASE,     /* [age=20..23] — KelpFeature nextInt(4)+20 */
    HC_B_KELP_PLANT = HC_B_KELP_BASE + 4,
    /* Task 13 postProcessGeneration 산물 — 월드젠 스테이지는 절대 쓰지
     * 않는다 (worldgen 물은 전부 level=0 소스). FlowingFluid 스프레드가
     * 만든다: level=1..7 흐름 (FluidState amount = 8-level), 8..15 낙하
     * (amount 8, FALLING; LiquidBlock.getFluidState 는 LEVEL 8..15 를
     * 전부 falling-8 로 매핑 — R-D 후속 핀 §12). */
    HC_B_WATER_FLOW_BASE, /* +N-1 = water[level=N], N=1..15 */
    /* BubbleColumnBlock.updateColumn 산물 (postProcess LiquidBlock.tick).
     * getFluidState = 물 소스 (R-D 후속 핀 §13). 26.2 직렬화 프로퍼티명은
     * drag (drag_down 아님 — golden 팔레트 실측, Task 14). */
    HC_B_BUBBLE_COLUMN_DRAG = HC_B_WATER_FLOW_BASE + 15, /* drag=true */
    HC_B_BUBBLE_COLUMN_PUSH,                             /* drag=false */
    /* --- Task 14: 풀 리전 확장 블록 (구조물 팔레트 + 신규 피처) ---
     * 등록은 blocks.c 의 T14-GEN 구간 (생성기:
     * tools/golden/experiments/gen_t14_blocks.py, 속성 근거:
     * .hermes/notes/task14-fullregion/R-blockprops*.tsv — 26.2 바이트코드
     * 핀). 코드는 전부 이름 조회 (init 시 hc_block_by_name) 로 접근한다
     * — 개별 enum 앵커 없음. */
    HC_B_T14_BASE,
    HC_B_COUNT = HC_B_T14_BASE + HC_B_T14_COUNT
};

/* 수평 방향 인덱스 (팔레트 오프셋용 내부 규약 — MC Direction 값과의
 * 대응은 각 feature 본문이 명시 테이블로 처리) */
enum { HC_HORIZ_N = 0, HC_HORIZ_E = 1, HC_HORIZ_S = 2, HC_HORIZ_W = 3 };

static inline uint16_t hc_block_leaves(int jungle, int distance, int wl) {
    return (uint16_t)((jungle ? HC_B_JUNGLE_LEAVES_BASE : HC_B_OAK_LEAVES_BASE) +
                      wl * 7 + (distance - 1));
}
static inline uint16_t hc_block_glow_lichen(int facemask, int wl) {
    return (uint16_t)(HC_B_GLOW_LICHEN_BASE + wl * 63 + (facemask - 1));
}

/* 캐노니컬 직렬화 (BlockStateParser.serialize 형태 — golden 팔레트와
 * 같은 문자열). NULL 없음 — 범위 밖은 호출자 버그다. */
const char *hc_block_name(uint16_t id);

/* 캐노니컬 문자열 → id. 모르면 -1 (호출자가 fail-loud). len 은 name 의
 * 길이 (NUL 종단 불필요 — JSON 문자열이 버퍼 슬라이스다). */
int32_t hc_block_by_name(const char *name, int32_t len);

/* BlockState.isAir / FluidState 비었는지 / blocksMotion.
 * 플래그 값의 출처는 26.2 Blocks.<clinit> 등록 체인 + BlockStateBase 캐시
 * 재구성 (.hermes/notes/task9b-features/R1) — blocks.c FLAGS 테이블. */
int hc_block_is_air(uint16_t id);
int hc_block_is_fluid(uint16_t id); /* 소스 water/lava 블록만 (level=0) */
int hc_block_blocks_motion(uint16_t id);

/* BlockState.isSolid (legacySolid) — MonsterRoom/vegetation_patch 등이
 * 읽는다. */
int hc_block_is_solid(uint16_t id);

/* getFaceOcclusionShape(면) 이 완전 폐색(full block)인지 — 면 무관 근사.
 * 부분 형상인데 일부 면만 폐색인 블록(계단 등)은 월드젠 palette 에 없다. */
int hc_block_is_full_cube(uint16_t id);

/* FluidState 가 비어있지 않은가 — 소스 유체 + waterlogged=true 상태
 * (MOTION_BLOCKING 하이트맵 술어, matching_fluids 의 물 판정) */
int hc_block_fluid_nonempty(uint16_t id);
/* FluidState 가 (소스) 물인가 — water[level=0] + waterlogged=true 상태 */
int hc_block_fluid_is_water(uint16_t id);
/* 상태 문자열에 waterlogged=true (Task 12 — 잎 updateShape 유체 틱) */
int hc_block_is_waterlogged(uint16_t id);
/* LeavesBlock 인가 (MOTION_BLOCKING_NO_LEAVES 술어) */
int hc_block_is_leaves(uint16_t id);
/* BlockState.canBeReplaced() — 나무/초목 배치가 읽는다 */
int hc_block_is_replaceable(uint16_t id);

/* --- Task 14: T14 블록 광학/형상 테이블 (id >= HC_B_T14_BASE 전용;
 * 값 근거 R-blockprops*.tsv — 26.2 getLightDampening/LIGHT_EMISSION/
 * getFaceOcclusionShape/isCollisionShapeFullBlock) --- */

/* getLightBlock (0..15). id < T14_BASE 는 -1 (호출자가 기존 파생 유지) */
int hc_block_t14_light_damp(uint16_t id);
/* getLightEmission. id < T14_BASE 는 -1 */
int hc_block_t14_light_emission(uint16_t id);
/* useShapeForLightOcclusion 상태의 6면 폐색 슬라이스 — 니블 순서 (LSB→)
 * D,U,N,S,W,E, 면당 2x2 쿼드런트 4비트 (R-blockprops-evidence §4 규약).
 * 0 = 라이트 폐색 형상 없음 (isEmptyShape). */
uint32_t hc_block_face_occlusion(uint16_t id);
/* isCollisionShapeFullBlock — StructureTemplate full 버킷 분류.
 * 전 id 유효 (pre-T14 는 F_FULL/F_LEAVES/spawner 파생). */
int hc_block_collision_full(uint16_t id);

#endif /* HC_BLOCKS_H */
