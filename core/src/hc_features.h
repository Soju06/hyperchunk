#ifndef HC_FEATURES_H
#define HC_FEATURES_H

#include "hc_biome.h"
#include "hc_blocks.h"
#include "hc_df_compile.h" /* hc_df_source_t (이름→JSON 테이블 규약) */
#include "hc_json.h"

#include "../include/hc_chunk.h"
#include "../include/hc_rng.h"

/* 07_features 스테이지 — 내부 전용 (core/src). 시맨틱은 전부 26.2
 * 바이트코드 기준 (.hermes/notes/task9pre-order/A1..A6 + task9a-features/
 * A1..A6):
 *  - 데코 워크: applyBiomeDecoration 재구성 (task9pre A2) — step 0..10,
 *    3x3 바이옴 합집합 → per-step 정렬 인덱스, setFeatureSeed 회계
 *  - 배치 파이프라인: 9 오프코드 (task9a A2), depth-first 루프 중첩
 *  - 패밀리: ore/spring/underwater_magma/monster_room-검증 (A3/A4/A5)
 *  - 순서: ADR-007 Tier 2 — 호출자가 order.manifest 순서로 청크를 돌린다
 * 파일 I/O 없음 — 호출자가 JSON/order 텍스트를 파싱해 넘긴다 (ADR-003 D1). */

/* --- WorldgenRandom over Xoroshiro (task9pre A3) ---
 *
 * 캐리버 스테이지의 LCG 래퍼와 달리 delegate 가 Xoroshiro 다. next(bits)
 * 는 xoro nextLong 의 상위 bits 비트; nextLong 은 2 드로우; nextInt 는
 * BitRandomSource 기본 구현 (pow2 빠른 경로 + next(31) 거절 루프) —
 * XoroshiroRandomSource.nextInt 의 128bit 곱 경로가 아니다 (A3 §1.4). */
typedef struct {
    hc_xoro_t x;
} hc_wgr_t;

void    hc_wgr_set_seed(hc_wgr_t *r, int64_t seed);
int32_t hc_wgr_next(hc_wgr_t *r, int bits);
int64_t hc_wgr_next_long(hc_wgr_t *r);
int32_t hc_wgr_next_int(hc_wgr_t *r, int32_t bound);
float   hc_wgr_next_float(hc_wgr_t *r);
double  hc_wgr_next_double(hc_wgr_t *r);

/* setDecorationSeed(levelSeed, 16*cx, 16*cz) — 순수 함수 (A3 §2.1).
 * 반환값이 데코 시드; r 은 f(deco) 상태로 남는다. */
int64_t hc_wgr_set_decoration_seed(hc_wgr_t *r, int64_t level_seed,
                                   int32_t min_block_x, int32_t min_block_z);
/* setFeatureSeed = setSeed(deco + index + 10000*step) — 32-bit imul (A3 §2.2) */
void hc_wgr_set_feature_seed(hc_wgr_t *r, int64_t deco_seed, int32_t index,
                             int32_t step);

/* Mth.randomBetweenInclusive: 항상 nextInt(hi-lo+1) 드로우 (lo==hi 포함).
 * Mth.nextInt: lo >= hi 면 드로우 0 으로 lo. 두 헬퍼는 퇴화 케이스가
 * 다르다 — 합치지 말 것 (task9a A2 §3.1). */
int32_t hc_mth_random_between_inclusive(hc_wgr_t *r, int32_t lo, int32_t hi);
int32_t hc_mth_next_int_range(hc_wgr_t *r, int32_t lo, int32_t hi);

/* --- 리전 (WorldGenRegion 대응) ---
 *
 * n x n 청크 창. 쓰기는 center ±1 (blockStateWriteRadius=1, task9pre A4
 * §3.1) — ensureCanWrite soft-fail. 읽기는 리전 전체 (assert). y 범위
 * 밖 읽기는 AIR (VOID_AIR.isAir / BulkSectionAccess 의 범위 밖 AIR 규약,
 * task9a A3 §5 — 술어 결과가 동일). 하이트맵: *_WG 만 읽는다 (frozen,
 * A4 §4.1); FINAL 4종 유지관리는 step 9 가 읽기 시작하는 9b 에서. */
enum { HC_FEAT_REGION_N = 11 }; /* Task 10: 11x11 (radius-5) 재생 월드 */

/* --- Task 12: 스케줄-틱 레코더 (ProtoChunkTicks 등가, R-D) ---
 *
 * 바닐라 WorldGenTickAccess.schedule → pos 소속 청크의 ProtoChunkTicks:
 *  - 저장 delay 는 스케줄 delay 와 무관하게 0 고정 (ProtoChunkTicks
 *    .schedule @12 iconst_0) — 월드젠 틱의 NBT t 는 전부 0.
 *  - 중복 제거 first-wins, 키 = (블록/유체 타입, pos) — 상태/delay 무관.
 *  - 저장 순서 = 청크별 스케줄 시간순 (ArrayList; LevelChunkTicks.pack 은
 *    subTickOrder 재정렬이라 save/load/FULL 을 지나도 불변).
 * 기록은 리전 전체 시간순 단일 배열 — 직렬화가 청크/종류로 거른다. */
enum {
    HC_TICK_BLOCK = 0,     /* block_ticks — i = 상태 이름의 '[' 앞부분 */
    HC_TICK_WATER,         /* fluid_ticks minecraft:water */
    HC_TICK_FLOWING_WATER, /* minecraft:flowing_water */
    HC_TICK_LAVA,          /* minecraft:lava */
    HC_TICK_FLOWING_LAVA,  /* minecraft:flowing_lava */
};
typedef struct {
    int32_t  x, y, z; /* 절대 블록 좌표 */
    uint16_t block;   /* HC_TICK_BLOCK 전용: 스케줄 주체 상태 id */
    uint8_t  kind;
    int32_t  t;       /* 저장 t (월드젠 0; postProcess 라이브 스케줄 5 등) */
} hc_tick_rec_t;
typedef struct {
    hc_tick_rec_t *recs;
    int32_t        n, cap;
    int32_t       *hset; /* open addressing: recs 인덱스, -1 = 빈 슬롯 */
    uint32_t       hcap; /* 2^k */
} hc_tick_recorder_t;

/* cap 개 기록 + 2^k >= 4*cap 해시를 arena 에서 할당. 실패 -1. */
int hc_tick_recorder_init(hc_tick_recorder_t *tr, hc_arena_t *a, int32_t cap);

typedef struct {
    hc_chunk_t *chunks[HC_FEAT_REGION_N * HC_FEAT_REGION_N]; /* [dz*n+dx] */
    int32_t     cx0, cz0, n;
    int32_t     center_cx, center_cz; /* 지금 데코 중인 청크 */
    /* Task 12: NULL = 기록 끔 (기존 게이트 불변) */
    hc_tick_recorder_t *ticks;
    /* Task 10 (트레이스+덤프 실증): 기록 서버는 manifest seq 9 직전에
     * 전 청크를 저장/언로드했다 — *_WG 하이트맵은 NBT 에 안 실리므로
     * 이후의 *_WG 읽기는 청크·타입별 "현재 블록에서 첫-읽기 재프라임 후
     * 동결" 의미가 된다 (그리드 07: 06 동결 0-diff / 링2 07: 재프라임
     * 16/16 — heightmapsAfter 는 CARVERS 부터 FINAL 4종이라 *_WG 는
     * setBlockState 로는 절대 안 갱신된다). 리플레이어가 entry 9 재생
     * 직전에 1 로 올린다. */
    int         wg_dropped;
} hc_feat_region_t;

hc_chunk_t *hc_feat_region_chunk(const hc_feat_region_t *rg, int32_t cx,
                                 int32_t cz);
uint16_t    hc_feat_get_block(const hc_feat_region_t *rg, int32_t x, int32_t y,
                              int32_t z);
/* ensureCanWrite + states 쓰기 + FINAL 하이트맵 4종 유지 (ProtoChunk
 * .setBlockState 의 flag-2/3 경로 — 지연 프라임 후 증분 update, task9pre
 * A4 §4). ore 본문은 BulkSectionAccess 등가로 이 함수를 우회한다 (맵
 * 무갱신 — 광석은 불투명→불투명 치환이라 순수함수 등가). 성공 1. */
int hc_feat_set_block(hc_feat_region_t *rg, int32_t x, int32_t y, int32_t z,
                      uint16_t id);
/* setBlockKnownShape (flags 19) — 일반 postProcess 마킹 억제 (Task 13) */
int hc_feat_set_block_ks(hc_feat_region_t *rg, int32_t x, int32_t y,
                         int32_t z, uint16_t id);
/* ScheduledTickAccess.scheduleTick 등가 — rg->ticks 없으면 no-op.
 * t 는 저장 t 값 (월드젠 경로 = 0). */
void hc_feat_schedule_tick(hc_feat_region_t *rg, int32_t x, int32_t y,
                           int32_t z, uint16_t block_state, int kind,
                           int32_t t);
/* Task 13 postProcess 마킹 (features.c 주석 참조) */
void hc_feat_mark_above(hc_feat_region_t *rg, int32_t x, int32_t y, int32_t z);
void hc_feat_mark_pos(hc_feat_region_t *rg, int32_t x, int32_t y, int32_t z);
/* ctx.getHeight(type,x,z) = getFirstAvailable (top blocking y + 1).
 * *_WG: wg_dropped 전엔 frozen 저장값, 후엔 청크·타입별 첫-읽기 재프라임
 * (아래 wg_dropped 주석); FINAL 4종은 지연 프라임 (읽기 = 단일 타입
 * 프라임, ChunkAccess.getHeight 의 lazy 경로) — 그래서 region 이
 * non-const. */
int32_t hc_feat_height(hc_feat_region_t *rg, int hm_type, int32_t x,
                       int32_t z);
/* 데코 시작 시 센터 청크 FINAL 4종 재프라임 (ChunkStatusTasks
 * .generateFeatures 진입부의 primeHeightmaps — 블록 순수함수 재계산) */
void hc_feat_prime_final_maps(hc_chunk_t *c);

/* --- 컴파일된 배치 파이프라인 --- */

enum {
    HC_HM_OCEAN_FLOOR_WG = 0,
    HC_HM_WORLD_SURFACE_WG = 1,
    /* FINAL(live) 맵 — hc_chunk_t.heightmap_final[type-2] */
    HC_HM_OCEAN_FLOOR = 2,
    HC_HM_WORLD_SURFACE = 3,
    HC_HM_MOTION_BLOCKING = 4,
    HC_HM_MOTION_BLOCKING_NO_LEAVES = 5,
};

enum {
    HC_IP_CONST = 0,
    HC_IP_UNIFORM,
    HC_IP_TRAPEZOID,     /* [a,b], plateau c — TrapezoidInt (9b R4) */
    HC_IP_WEIGHTED_LIST, /* WeightedListInt — nextInt(total) 픽 후 중첩 샘플 */
    HC_IP_BIASED_TO_BOTTOM, /* min + nextInt(nextInt(b-a+1)+1) — 2 드로우
                             * (BiasedToBottomInt.sample@0-30, sugar_cane) */
    /* clamped_normal 등 (가우시안 드로우) — 그리드 밖 feature 가 참조.
     * 컴파일 허용, 샘플 도달 시 즉사. */
    HC_IP_UNSUPPORTED,
};
typedef struct hc_iprov hc_iprov_t;
typedef struct hc_iprov_entry hc_iprov_entry_t;
struct hc_iprov {
    uint8_t           kind;
    int32_t           a, b, c; /* CONST: a / UNIFORM: [a,b] / TRAP: +plateau c */
    int32_t           n_entries, total_weight; /* WEIGHTED_LIST */
    hc_iprov_entry_t *entries;
};
struct hc_iprov_entry {
    int32_t   weight;
    hc_iprov_t prov;
};

/* --- BlockState provider (step 9 본문용, task9b R3/R4) --- */
enum {
    HC_SP_SIMPLE = 0,     /* 드로우 0 */
    HC_SP_WEIGHTED,       /* WeightedRandom: nextInt(total) 1 드로우 */
    HC_SP_RANDOMIZED_INT, /* source 샘플 후 property 값 IntProvider 샘플 */
};
typedef struct hc_sprov hc_sprov_t;
struct hc_sprov {
    uint8_t kind;
    uint16_t state; /* SIMPLE */
    int32_t n_entries, total_weight; /* WEIGHTED */
    struct {
        int32_t  weight;
        uint16_t state;
    } *entries;
    hc_sprov_t *source; /* RANDOMIZED_INT */
    hc_iprov_t  values;
    /* randomized_int 의 property — 현 데이터는 cave_vines "age" 뿐.
     * 적용은 블록 패밀리별 명시 매핑 (features.c) — 모르면 컴파일 실패. */
    uint8_t     prop_age;
};

enum { HC_HP_UNIFORM = 0, HC_HP_TRAPEZOID, HC_HP_VERY_BIASED_TO_BOTTOM };
typedef struct {
    uint8_t kind;
    int32_t min_y, max_y; /* 앵커는 컴파일 시 해석 (-64/384 고정, A2 §3.3) */
    int32_t plateau;      /* trapezoid (우리 데이터 전부 0) */
    int32_t inner;        /* very_biased_to_bottom */
} hc_hprov_t;

/* 블록 술어 (task9a A2 §6 + task9b R2/R4) — 전부 RNG 무소비 */
enum {
    HC_BP_MATCHING_FLUIDS_WATER = 0, /* FluidState.is(water) — 소스+waterlogged */
    HC_BP_MATCHING_BLOCK_TAG, /* matching_block_tag + matching_blocks (블록 단위) */
    HC_BP_NOT,
    HC_BP_ALL_OF,
    HC_BP_ANY_OF, /* 단락 OR, 리스트 순 */
    HC_BP_INSIDE_WORLD_BOUNDS,
    HC_BP_SOLID,           /* state.isSolid() */
    HC_BP_WOULD_SURVIVE,   /* state.canSurvive — 블록별 디스패치 (R2) */
    HC_BP_HAS_STURDY_FACE, /* isFaceSturdy(pos, direction) */
    HC_BP_TRUE,
    /* Task 10 링 파이프라인 (patch_melon 등) */
    HC_BP_MATCHING_FLUIDS_EMPTY, /* FluidState.isEmpty */
    HC_BP_REPLACEABLE,           /* state.canBeReplaced */
};
typedef struct hc_bpred hc_bpred_t;
struct hc_bpred {
    uint8_t    kind;
    int8_t     off[3]; /* StateTestingPredicate/inside_world_bounds offset */
    const char *ws_name; /* WOULD_SURVIVE 대상 블록 이름 (arena 사본) */
    int8_t     dir;    /* HAS_STURDY_FACE: 0=down,1=up,2=N,3=S,4=W,5=E */
    uint64_t   tag_mask[(HC_B_COUNT + 63) / 64];
    hc_bpred_t *children;
    int32_t    n_children;
};

enum {
    HC_PM_RARITY_FILTER = 0,
    HC_PM_COUNT,
    HC_PM_IN_SQUARE,
    HC_PM_HEIGHT_RANGE,
    HC_PM_HEIGHTMAP,
    HC_PM_ENV_SCAN,
    HC_PM_SURF_REL_THRESHOLD,
    HC_PM_BLOCK_PRED,
    HC_PM_BIOME,
    HC_PM_RANDOM_OFFSET,
    HC_PM_SURFACE_WATER_DEPTH, /* WS−OF (live) <= max — 드로우 0 (9b) */
    HC_PM_NOISE_THRESHOLD_COUNT, /* BIOME_INFO_NOISE 카운트 — 드로우 0 (9b) */
    HC_PM_NOISE_BASED_COUNT, /* ceil((noise+off)*ratio) 반복 — 드로우 0 (T10) */
    /* 그리드 밖 바이옴 전용 modifier — 컴파일 허용, 실행 도달 시 즉사.
     * (합집합에 든 feature 의 파이프라인은 전부 실행되므로, 합집합 밖
     * feature 만 이 마커를 가질 수 있다.) */
    HC_PM_DIE,
};
typedef struct {
    uint8_t    kind;
    int32_t    chance;    /* rarity */
    hc_iprov_t count;     /* count / random_offset xz_spread */
    hc_iprov_t y_spread;  /* random_offset */
    hc_hprov_t height;    /* height_range */
    uint8_t    hm_type;   /* heightmap / surf_rel */
    int32_t    min_incl, max_incl; /* surf_rel / surface_water_depth(max) */
    int8_t     scan_dy;   /* env_scan: +1 up / -1 down */
    int32_t    max_steps; /* env_scan */
    uint8_t    has_allowed; /* env_scan: allowed_search_condition 존재 */
    hc_bpred_t pred;      /* block_pred / env_scan target */
    hc_bpred_t allowed;   /* env_scan allowed (has_allowed) */
    double     noise_level; /* noise_threshold_count */
    int32_t    below_noise, above_noise;
    double     noise_factor, noise_offset; /* noise_based_count */
    int32_t    noise_ratio;                /* noise_based_count */
    const char *die_what; /* HC_PM_DIE */
} hc_pmod_t;

/* --- 컴파일된 configured feature 본문 --- */

typedef struct hc_pfeat hc_pfeat_t;

enum {
    HC_CF_ORE = 0,           /* minecraft:ore (task9a A3) */
    HC_CF_SPRING,            /* minecraft:spring_feature (A4, 드로우 0) */
    HC_CF_UNDERWATER_MAGMA,  /* minecraft:underwater_magma (A4) */
    HC_CF_MONSTER_ROOM,      /* 검증 단계만 — 성공 시 fail-loud */
    /* --- step 9 (task9b R2/R3/R4) --- */
    HC_CF_TREE,              /* minecraft:tree */
    HC_CF_FALLEN_TREE,
    HC_CF_RANDOM_SELECTOR,
    HC_CF_RANDOM_BOOLEAN,
    HC_CF_SIMPLE_RANDOM_SELECTOR,
    HC_CF_VEGETATION_PATCH,  /* +waterlogged 플래그 */
    HC_CF_BLOCK_COLUMN,
    HC_CF_MULTIFACE_GROWTH,
    HC_CF_SIMPLE_BLOCK,
    HC_CF_VINES,
    HC_CF_BAMBOO,
    HC_CF_FREEZE_TOP_LAYER,
    /* --- Task 10: 링 프리픽스 본문 --- */
    HC_CF_DISK,        /* minecraft:disk (disk_sand/gravel/clay) */
    HC_CF_SEAGRASS,    /* minecraft:seagrass (seagrass_river 등) */
    HC_CF_LAKE,        /* minecraft:lake (lake_lava_underground) */
    HC_CF_ROOT_SYSTEM, /* minecraft:root_system (rooted_azalea_tree) */
    HC_CF_GEODE,       /* minecraft:geode (amethyst_geode) */
    HC_CF_KELP,        /* minecraft:kelp (kelp_warm/cold — config 없음) */
    HC_CF_UNIMPLEMENTED,     /* 파이프라인만 — 본문 도달 시 placed=-1 */
};

enum { HC_ORE_MAX_TARGETS = 2 };
typedef struct {
    int32_t size;
    float   discard_on_air;
    int32_t n_targets;
    struct {
        uint64_t rule_mask[(HC_B_COUNT + 63) / 64]; /* tag_match 확장 */
        uint16_t state;
    } targets[HC_ORE_MAX_TARGETS];
} hc_ore_cfg_t;

typedef struct {
    uint16_t fluid_block;     /* water[level=0] / lava[level=0] */
    uint8_t  requires_block_below; /* codec 기본 true */
    int32_t  rock_count;      /* 기본 4 */
    int32_t  hole_count;      /* 기본 1 */
    uint64_t valid_mask[(HC_B_COUNT + 63) / 64];
} hc_spring_cfg_t;

typedef struct {
    int32_t floor_search_range;
    int32_t placement_radius;
    float   placement_prob;
} hc_umagma_cfg_t;

/* --- step 9 본문 config (arena 할당, 포인터로 연결) --- */

typedef struct {
    int32_t n_entries;
    struct {
        float       chance;
        hc_pfeat_t *pf; /* 중첩 placed feature (place() 경로 — biome 금지) */
    } *entries;
    hc_pfeat_t *dflt;
} hc_rsel_cfg_t;

typedef struct {
    hc_pfeat_t *on_true, *on_false;
} hc_rbool_cfg_t;

typedef struct {
    int32_t      n;
    hc_pfeat_t **feats;
} hc_srsel_cfg_t;

typedef struct {
    uint8_t    waterlogged;     /* waterlogged_vegetation_patch */
    uint8_t    surface_ceiling; /* surface: 0 floor / 1 ceiling */
    hc_iprov_t xz_radius, depth;
    float      extra_bottom_chance, extra_edge_chance, veg_chance;
    int32_t    vertical_range;
    uint64_t   replaceable[(HC_B_COUNT + 63) / 64];
    hc_sprov_t ground;
    hc_pfeat_t *vegetation;
} hc_vpatch_cfg_t;

typedef struct {
    int32_t n_layers;
    struct {
        hc_iprov_t height;
        hc_sprov_t prov;
    } *layers;
    int8_t     dir_dy; /* +1 up / -1 down */
    uint8_t    prioritize_tip;
    hc_bpred_t allowed;
} hc_bcol_cfg_t;

typedef struct {
    int32_t  search_range;
    uint8_t  can_place_on_floor, can_place_on_ceiling, can_place_on_wall;
    float    chance_of_spreading;
    uint64_t can_place_on[(HC_B_COUNT + 63) / 64];
} hc_mface_cfg_t;

typedef struct {
    hc_sprov_t to_place;
} hc_sblock_cfg_t;

typedef struct {
    float probability;
} hc_bamboo_cfg_t;

/* --- tree / fallen_tree (task9b R2) --- */

enum {
    HC_TRUNK_STRAIGHT = 0,
    HC_TRUNK_MEGA_JUNGLE,
    HC_TRUNK_FANCY,
    HC_TRUNK_BENDING, /* azalea (Task 10 R5c §5) */
};
enum {
    HC_FOL_BLOB = 0,
    HC_FOL_BUSH,
    HC_FOL_MEGA_JUNGLE,
    HC_FOL_FANCY,
    HC_FOL_RANDOM_SPREAD, /* azalea (Task 10 R5c §6) */
};
enum {
    HC_TDEC_COCOA = 0,
    HC_TDEC_TRUNK_VINE,
    HC_TDEC_LEAVE_VINE,
    HC_TDEC_ATTACHED_TO_LOGS,
};
typedef struct {
    uint8_t    kind;
    float      prob;
    hc_sprov_t provider; /* attached_to_logs block_provider */
    int8_t     dir;      /* attached_to_logs 단일 direction (Direction ord) */
} hc_tdec_t;

typedef struct hc_tree_cfg {
    uint8_t    trunk_kind, fol_kind;
    int32_t    base_height, rand_a, rand_b;
    hc_iprov_t fol_radius, fol_offset;
    int32_t    fol_height;    /* random_spread 는 foliage_height */
    int32_t    leaf_attempts; /* random_spread leaf_placement_attempts */
    int32_t    min_height_for_leaves; /* bending (기본 1) */
    hc_iprov_t bend_length;          /* bending */
    uint16_t   trunk_state; /* simple provider (jungle/oak log axis=y) */
    hc_sprov_t foliage; /* simple 또는 weighted; 전 상태 leaves[d7,wl=false] */
    /* below_trunk rule_based: if !mask(cannot_replace…) → below_state */
    uint64_t   below_not_mask[(HC_B_COUNT + 63) / 64];
    uint16_t   below_state;
    uint8_t    ignore_vines;
    int32_t    ts_limit, ts_lower, ts_upper, ts_min_clipped; /* -1 = 없음 */
    int32_t    n_decorators;
    hc_tdec_t *decorators;
} hc_tree_cfg_t;

typedef struct hc_ftree_cfg {
    uint16_t   trunk_state;
    hc_iprov_t log_length;
    int32_t    n_stump_dec, n_log_dec;
    hc_tdec_t *stump_dec, *log_dec;
} hc_ftree_cfg_t;

/* minecraft:disk (DiskFeature + RuleBasedStateProvider — Task 10) */
typedef struct {
    hc_bpred_t if_true; /* 셀 위치에서 평가 (오프셋 포함) */
    hc_sprov_t then;
} hc_disk_rule_t;
typedef struct {
    int32_t         half_height;
    hc_iprov_t      radius;
    hc_bpred_t      target;
    hc_sprov_t      fallback;
    int32_t         n_rules;
    hc_disk_rule_t *rules;
} hc_disk_cfg_t;

/* minecraft:seagrass (SeagrassFeature — ProbabilityFeatureConfiguration) */
typedef struct {
    float probability;
} hc_seagrass_cfg_t;

/* minecraft:lake (LakeFeature — Task 10 R5b) */
typedef struct {
    uint16_t   fluid;   /* simple provider — lava[level=0] */
    uint16_t   barrier; /* simple provider — stone */
    hc_bpred_t can_place;            /* can_place_feature */
    hc_bpred_t can_replace_airfluid; /* can_replace_with_air_or_fluid */
    hc_bpred_t can_replace_barrier;  /* can_replace_with_barrier */
} hc_lake_cfg_t;

/* minecraft:root_system (RootSystemFeature — Task 10 R5c §7.1) */
typedef struct {
    hc_pfeat_t *tree; /* 인라인 placed ("feature", placement []) */
    hc_bpred_t allowed_tree_position;
    int32_t    required_vertical_space; /* 3 */
    int32_t    allowed_vertical_water;  /* 2 */
    int32_t    root_radius, root_attempts, root_column_max_height;
    uint64_t   root_replaceable[(HC_B_COUNT + 63) / 64];
    uint16_t   root_state;    /* simple rooted_dirt */
    int32_t    hanging_radius, hanging_span, hanging_attempts;
    uint16_t   hanging_state; /* simple hanging_roots[wl=false] */
} hc_rootsys_cfg_t;

/* minecraft:geode (GeodeFeature — Task 10 R5a) */
typedef struct {
    uint16_t   fill, inner, alt_inner, middle, outer; /* simple providers */
    uint16_t   placements[8]; /* inner_placements (기본 상태) */
    int32_t    n_placements;
    uint64_t   cannot_replace[(HC_B_COUNT + 63) / 64];
    uint64_t   invalid_blocks[(HC_B_COUNT + 63) / 64];
    double     layer_fill, layer_inner, layer_middle, layer_outer;
    double     crack_chance, crack_base;
    int32_t    crack_offset;
    double     use_potential, use_alt;
    int32_t    require_alt;
    hc_iprov_t outer_wall, dist_points, point_offset;
    int32_t    outer_wall_max; /* outer_wall_distance.maxInclusive */
    int32_t    min_gen, max_gen;
    double     noise_mult;
    int32_t    invalid_threshold;
} hc_geode_cfg_t;

struct hc_pfeat {
    const char *name; /* "minecraft:ore_dirt" (arena 사본; 인라인은 NULL) */
    const char *unimpl_why; /* UNIMPLEMENTED 로 강등된 이유 (진단용) */
    int32_t     n_mods;
    hc_pmod_t  *mods;
    uint8_t     cf_kind;
    union {
        hc_ore_cfg_t    ore;
        hc_spring_cfg_t spring;
        hc_umagma_cfg_t umagma;
        hc_rsel_cfg_t   *rsel;
        hc_rbool_cfg_t  *rbool;
        hc_srsel_cfg_t  *srsel;
        hc_vpatch_cfg_t *vpatch;
        hc_bcol_cfg_t   *bcol;
        hc_mface_cfg_t  *mface;
        hc_sblock_cfg_t *sblock;
        hc_bamboo_cfg_t *bamboo;
        hc_tree_cfg_t   *tree;
        hc_ftree_cfg_t  *ftree;
        hc_disk_cfg_t   *disk;
        hc_seagrass_cfg_t seagrass;
        hc_lake_cfg_t    *lake;
        hc_rootsys_cfg_t *rootsys;
        hc_geode_cfg_t   *geode;
    } cf;
};

/* --- feature 레지스트리 (FeatureSorter 테이블 + 바이옴 멤버십) --- */

enum { HC_FEAT_STEPS = 11 };

typedef struct {
    int32_t    counts[HC_FEAT_STEPS];
    hc_pfeat_t *steps[HC_FEAT_STEPS];   /* walk_max_step 초과 step 은
                                         * 파이프라인 미컴파일 (mods NULL) */
    /* 본문 canSurvive 가 읽는 태그 마스크 (reg_init 이 확장) */
    uint64_t tag_supports_vegetation[(HC_B_COUNT + 63) / 64];
    uint64_t tag_supports_bamboo[(HC_B_COUNT + 63) / 64];
    uint64_t tag_podzol_replaceable[(HC_B_COUNT + 63) / 64];
    uint64_t tag_supports_azalea[(HC_B_COUNT + 63) / 64];
    uint64_t tag_supports_small_dripleaf[(HC_B_COUNT + 63) / 64];
    uint64_t tag_supports_big_dripleaf[(HC_B_COUNT + 63) / 64];
    uint64_t tag_supports_cocoa[(HC_B_COUNT + 63) / 64];
    uint64_t tag_supports_sugar_cane[(HC_B_COUNT + 63) / 64];
    /* tree 본문 (R2): validTreePos/isFree/getOptionalDistanceAt */
    uint64_t tag_replaceable_by_trees[(HC_B_COUNT + 63) / 64];
    uint64_t tag_logs[(HC_B_COUNT + 63) / 64];
    uint64_t tag_prevents_leaf_decay[(HC_B_COUNT + 63) / 64];
    /* Task 10 링 본문 */
    uint64_t tag_cannot_support_seagrass[(HC_B_COUNT + 63) / 64];
    uint64_t tag_features_cannot_replace[(HC_B_COUNT + 63) / 64];
    /* 멤버십 비트셋: member[step][biome_id * words + w]. biome_id 는
     * hc_biome_reg_t 인턴 id — reg_init 이 biome_features 의 전 바이옴을
     * 인턴한다. */
    uint64_t *member[HC_FEAT_STEPS];
    int32_t   words[HC_FEAT_STEPS];
    int32_t   n_biomes;
} hc_feat_reg_t;

/* order_txt: reference/features_order-26.2.txt 내용 (NUL 종단).
 * biome_features: reference/biome_features-26.2.json 파스 트리.
 * placed/configured/tags: reference 트리 로드 테이블.
 * walk_max_step 이하의 step 에 등장하는 feature 는 파이프라인+본문을
 * 컴파일하고, 모르는 modifier/본문이면 실패(-1 + *err). 그 밖의 step 은
 * 이름/카운트만 채운다. */
int hc_feat_reg_init(hc_feat_reg_t *reg, hc_arena_t *arena,
                     const char *order_txt, const hc_json_t *biome_features,
                     const hc_df_source_t *placed, int32_t n_placed,
                     const hc_df_source_t *configured, int32_t n_configured,
                     const hc_df_source_t *tags, int32_t n_tags,
                     hc_biome_reg_t *biomes, int32_t walk_max_step,
                     const char **err);

/* --- 데코 워크 (gen_features_stage.c) --- */

/* 트레이스 싱크 — golden/features-trace FORMAT.md 의 p/f 라인 대응.
 * placed: 1/0, 본문 미구현으로 알 수 없으면 -1. */
typedef struct {
    void (*on_pos)(void *ud, int32_t step, int32_t index, int32_t x, int32_t y,
                   int32_t z, int32_t placed);
    void (*on_feature)(void *ud, int32_t step, int32_t index, const char *name,
                       int32_t npos, int32_t placed);
    void *ud;
} hc_feat_trace_t;

/* 데코 시드 — 순수 함수 (manifest 시드 열 검증용) */
int64_t hc_features_decoration_seed(int64_t level_seed, int32_t cx, int32_t cz);

/* 한 placed feature 의 파이프라인+본문 실행 (features.c — walk 내부용).
 * biomes/sea_level 은 freeze_top_layer 온도 게이트가 읽는다. */
void hc_feat_run_placed(hc_feat_region_t *rg, hc_wgr_t *rng,
                        int64_t level_seed, const hc_feat_reg_t *reg,
                        const hc_biome_view_t *view,
                        const hc_biome_reg_t *biomes, int32_t sea_level,
                        const hc_pfeat_t *pf, int32_t step, int32_t index,
                        int32_t origin_x, int32_t origin_y, int32_t origin_z,
                        const hc_feat_trace_t *trace);

/* 한 청크의 데코 적용. 호출자가 manifest seq 순서로 부른다 (ADR-008 D2
 * REPLAY). rg->center 는 이 함수가 설정. walk_max_step: 이 step 까지만
 * 파이프라인 실행 (RNG 는 feature 마다 재시드라 상위 step 생략이
 * 하위에 영향 없음 — task9a A5 §"skipping is RNG-safe"). trace NULL 허용. */
void hc_gen_features_chunk(hc_feat_region_t *rg, int32_t cx, int32_t cz,
                           int64_t level_seed, const hc_feat_reg_t *reg,
                           const hc_biome_view_t *view,
                           const hc_biome_reg_t *biomes, int32_t sea_level,
                           int32_t walk_max_step,
                           const hc_feat_trace_t *trace);

/* --- 10_spawn / 11_full 스테이지 (Task 11, gen_spawn_full_stages.c) ---
 *
 * 실측 (golden 09→10→11 diff) + 바이트코드 (task11-spawnfull/): spawn 은
 * 덤프 관측면 순수 pass-through (NaturalSpawner 부기만), full 은 하이트맵
 * kind 프루닝만 — proto 맵 중 FINAL 4종만 setRawData 비트 복사로 생존,
 * *_WG 소멸, 전환 시점 프라임 없음. 블록/바이옴/하이트맵 값 연산 없음. */
void hc_gen_spawn_stage(hc_chunk_t *c);
void hc_gen_full_stage(hc_chunk_t *c);

#endif /* HC_FEATURES_H */
