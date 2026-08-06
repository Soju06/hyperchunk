#ifndef HC_STRUCTURES_H
#define HC_STRUCTURES_H

#include "hc_beard.h"
#include "hc_biome.h"
#include "hc_features.h"
#include "hc_nbt.h"
#include "hc_sync.h" /* hc_spin_t (P2-3) */

/* Task 14 구조물 파이프라인 — 내부 전용 (core/src).
 *
 * 범위 (ADR-003 D4: 구조물은 배치까지, 직소 조립 제외):
 *  - in-region starts 4건 (ocean_ruin_warm/shipwreck_beached/
 *    ruined_portal_ocean/trial_chambers) 은 골든 starts NBT 프래그먼트
 *    (golden/structures/c.*.starts.nbt — ADR-008 REPLAY 입력) 를 피스
 *    리스트로 소비하고, 배치 (템플릿/프로세서/BE/RNG) 를 재구현한다.
 *  - mineshaft (비직소) 는 시드에서 조립·배치 전부 생성 (스타트가 리전
 *    밖 — 골든에 피스 리스트가 없다).
 *  - 리전 밖 직소 스타트 (trial_chambers (13,35)) 는 References 멤버십만
 *    골든 실측으로 고정 (references.txt) — 완료 노트에 근거 기록.
 *
 * 시맨틱 출처: .hermes/notes/task14-fullregion/R-placement.md,
 * R-mineshaft-dungeon-placement.md, R-serialization.md, R-fastutil.md
 * (전부 26.2 디컴파일/javap 핀). */

/* 위치시드 RNG: hc_mth_get_seed (Mth.getSeed) 는 hc_rng.h 에 이미 있다
 * — LegacyRandomSource(getSeed(pos)) 일회용 인스턴스가 프로세서/팔레트
 * 선택의 표준 패턴 (R-placement §3.4). */

/* ---------- 구조물 템플릿 (StructureTemplate) ---------- */

/* 로드시 (Y,X,Z) 정렬 3버킷 (full + other + blockEntities) 연결 순서로
 * 저장 (R-placement §3.2). full = nbt 없음 && isCollisionShapeFullBlock. */
typedef struct {
    int32_t         x, y, z;  /* 템플릿-로컬 */
    uint16_t        state;    /* 팔레트 해석 (등록 id) */
    const hc_nbt_t *nbt;      /* BE nbt (없으면 NULL) */
} hc_tblock_t;

enum { HC_TPL_MAX_PALETTES = 8 };

typedef struct {
    int32_t      size[3];
    int32_t      n_palettes;
    int32_t      n_blocks;     /* 팔레트당 블록 수 (전 팔레트 동일) */
    hc_tblock_t *blocks;       /* [palette][n_blocks] 연접 — lazy 해석 */
    const char  *name;         /* "minecraft:shipwreck/..." */
    /* lazy 팔레트 해석 (비선택 팔레트의 상태는 레지스트리에 없을 수
     * 있다 — 미사용 우드 변형): 선택 시점에 해석. */
    const hc_nbt_t *pal_nbt[HC_TPL_MAX_PALETTES];
    uint8_t         pal_resolved[HC_TPL_MAX_PALETTES];
    const int32_t (*raw_pos)[3]; /* [n_blocks] 템플릿-로컬 */
    const int32_t  *raw_state;   /* 팔레트 인덱스 */
    const hc_nbt_t *const *raw_nbt;
} hc_template_t;

/* reference/structure/<path>.nbt (무압축) 로드 + 버킷 정렬. 실패 NULL. */
/* P2-3: 지연-초기화 전역 테이블 소진 (FREE 워커 스폰 전 1회 — REPLAY 는
 * 불필요하지만 무해). hc_structures_init 이 호출한다. */
void hc_template_prewarm(void);
void hc_mineshaft_prewarm(void);

const hc_template_t *hc_template_load(hc_arena_t *a, const char *dir,
                                      const char *name /* mc id */);

/* ---------- 상태 회전/미러 (BlockState.mirror/rotate) ---------- */

enum { HC_ROT_NONE = 0, HC_ROT_CW90, HC_ROT_CW180, HC_ROT_CCW90 };
enum { HC_MIR_NONE = 0, HC_MIR_LEFT_RIGHT, HC_MIR_FRONT_BACK };

/* 프로퍼티 문자열 재작성으로 구현 (blocks.c NAMES 캐노니컬) — 결과 상태
 * 미등재 시 die. mirror 먼저, rotate 나중 (placeInWorld @순서). */
uint16_t hc_state_mirror(uint16_t s, int mir);
uint16_t hc_state_rotate(uint16_t s, int rot);

/* 상태 문자열 분해/재조립 (structures.c 구현 — template/processor 계열이
 * waterlogged 재작성·final_state 해석에 공유). build 는 미등재 시 die. */
typedef struct {
    char k[24], v[32];
} hc_skv_t;
int      hc_state_parse(const char *name, char *base, size_t base_cap,
                        hc_skv_t *kv, int cap);
uint16_t hc_state_build(const char *base, hc_skv_t *kv, int n);

/* StructureTemplate.transform(pos, mirror, rot, pivot) — 로컬 좌표 */
void hc_template_transform(int32_t *x, int32_t *y, int32_t *z, int mir,
                           int rot, int32_t pivot_x, int32_t pivot_z);

/* ---------- 블록엔티티 레코더 ----------
 *
 * 월드젠 BE 의 두 부류 (R-serialization §4):
 *  - live: 배치 시점에 실체화 (템플릿 nbt 로드 / setLootTable) — proto
 *    live 맵 (fastutil) 삽입 순서 = 실체화 순서.
 *  - pending(DUMMY): EntityBlock 배치만 되고 안 만져진 것 — full 승격의
 *    postProcessGeneration 이 pending HashMap 순회 순서로 실체화 (live
 *    맵 꼬리에 append).
 * 직렬화 리스트 순서 = fresh HashSet<BlockPos>(live 키들을 LevelChunk
 * live 맵 순회 순서로 삽입) 순회 (Vec3i.hashCode) — chunk_nbt 쪽에서
 * 에뮬레이션. 여기서는 (실체화 종류, 순서) 를 기록한다. */

enum {
    HC_BE_TEMPLATE = 0, /* 템플릿 nbt 로드 (+ 컨테이너면 주입 시드) */
    HC_BE_CHEST_LOOT,   /* setLootTable(name, seed) — monster_room/마커 */
    HC_BE_SPAWNER,      /* SpawnerBlockEntity + entity id */
    HC_BE_DUMMY,        /* EntityBlock 미접촉 (기본 상태 저장) */
};
typedef struct {
    int32_t         x, y, z;
    uint8_t         kind;
    uint8_t         dead;      /* 이후 다른 상태로 덮임 → 제거 */
    uint16_t        state;     /* 배치 상태 (직렬화 타입 판별) */
    const hc_nbt_t *tpl_nbt;   /* TEMPLATE: 템플릿 nbt (읽기 전용) */
    int64_t         loot_seed; /* 컨테이너 주입/설정 시드 */
    const char     *loot;      /* CHEST_LOOT: 루트 테이블 id */
    const char     *entity;    /* SPAWNER: "minecraft:zombie" 등 */
} hc_be_rec_t;

typedef struct hc_be_recorder {
    hc_be_rec_t *recs;
    int32_t      n, cap;
    /* P2-3: FREE 스케줄러 물리 보호 (hc_tick_recorder_t.mu 와 동일 논리
     * — 같은 pos 접근자는 충돌쌍이라 스케줄러가 직렬화; 직렬화가 읽는
     * 청크별 프로젝션은 상대순서만 사용, bes_for_chunk). */
    hc_spin_t    mu;
} hc_be_recorder_t;

int  hc_be_recorder_init(hc_be_recorder_t *r, hc_arena_t *a, int32_t cap);
/* 같은 pos 의 기존 레코드는 dead 처리 후 append (덮어쓰기 시맨틱) */
hc_be_rec_t *hc_be_record(hc_be_recorder_t *r, int32_t x, int32_t y,
                          int32_t z, uint8_t kind, uint16_t state);
/* EntityBlock 이 아닌 상태로 덮인 pos 의 레코드 제거 */
void hc_be_remove(hc_be_recorder_t *r, int32_t x, int32_t y, int32_t z);

/* ---------- 스타트/피스 모델 ---------- */

enum {
    HC_SP_TEMPLATE_SHIPWRECK = 0,
    HC_SP_TEMPLATE_OCEAN_RUIN,
    HC_SP_TEMPLATE_PORTAL,
    HC_SP_JIGSAW, /* trial chambers PoolElementStructurePiece */
    HC_SP_MS_ROOM,
    HC_SP_MS_CORRIDOR,
    HC_SP_MS_CROSSING,
    HC_SP_MS_STAIRS,
};

typedef struct hc_spiece {
    uint8_t kind;
    int32_t bb[6]; /* minX..maxZ (조립/골든 그대로) */
    int8_t  o;     /* orientation get2DDataValue (-1 = null) */
    int32_t gd;
    /* --- 템플릿 계열 --- */
    const hc_template_t *tmpl;
    int32_t tpx, tpy, tpz;
    uint8_t rot, mir;          /* HC_ROT_* / HC_MIR_* */
    uint8_t liquid_ignore;     /* jigsaw: ignore_waterlogging */
    uint8_t known_shape;       /* jigsaw: flag 18 경로 */
    uint8_t procs;             /* HC_PROCS_* (아래) */
    /* jigsaw — beardifier 입력 (R: Beardifier.forStructuresInChunk) */
    uint8_t proj_rigid;        /* pool_element.projection == rigid */
    int32_t gld;               /* ground_level_delta */
    int32_t n_junctions;
    const hc_beard_junction_t *junctions;
    float   integrity;         /* ocean ruin BlockRot */
    /* shipwreck */
    uint8_t is_beached, height_adjusted;
    /* portal properties */
    uint8_t cold, air_pocket, overgrown, has_vines, replace_blackstone;
    float   mossiness;
    const char *vertical_placement; /* "on_ocean_floor" */
    /* --- mineshaft --- */
    uint8_t ms_dir;        /* 생성 방향 (N=0,S=1,W=2,E=3 아님 — 아래 표) */
    uint8_t has_rails, spider, is_two_floored;
    int32_t num_sections;
    uint8_t spider_placed; /* 런타임 (청크 간 공유!) */
    /* room childEntranceBoxes */
    int32_t  n_entrances;
    int32_t (*entrances)[6];
} hc_spiece_t;

/* 프로세서 체인 프리셋 — 이 리전의 조합 4가지 */
enum {
    HC_PROCS_SHIPWRECK = 0, /* [block_ignore STRUCTURE_AND_AIR] */
    HC_PROCS_OCEAN_RUIN,    /* [rot(integrity), ignore, capped(rule sand)] */
    HC_PROCS_PORTAL,        /* [ignore, rule, age, protected, lava_subm] */
    HC_PROCS_JIGSAW_NONE,   /* [ignore STRUCTURE_BLOCK, jigsaw_repl] */
    HC_PROCS_JIGSAW_COPPER, /* + copper bulb degradation rule */
};

typedef struct {
    const char *name; /* 레지스트리 키 ("minecraft:mineshaft" 등) */
    int32_t     scx, scz;
    uint8_t     step;       /* 3 or 4 */
    uint8_t     step_index; /* 스텝 내 구조물 순번 (R-placement §6) */
    const hc_nbt_t *tag;    /* 골든 starts 값 (재방출용; mineshaft NULL) */
    int32_t     bb[6];      /* adjusted bbox (trial +12 인플레이트) */
    int32_t     n_pieces;
    hc_spiece_t *pieces;
} hc_sstart_t;

/* ---------- 컨텍스트 ---------- */

enum { HC_SCTX_MAX_STARTS = 64 };

typedef struct hc_sctx {
    hc_arena_t     *arena;
    int64_t         seed;
    const char     *template_dir;
    const hc_biome_view_t *view;    /* 생 쿼트 밴드 (biome 검사) */
    const hc_biome_reg_t  *biomes;
    /* 태그 마스크 */
    uint64_t mask_features_cannot_replace[(HC_B_COUNT + 63) / 64];
    /* 자주 쓰는 블록 id (init 에서 이름 해석) */
    uint16_t b_cobweb, b_rail_ns, b_rail_ew, b_torch_wall[4], b_chain,
        b_planks, b_fence[16], b_barrier_unused, b_netherrack, b_magma,
        b_suspicious_sand;
    /* copper bulb degradation 룰 (JSON 파싱 결과) */
    struct {
        uint16_t in_block_t14_first; /* waxed_copper_bulb[lit,powered] 후보 */
    } copper;
    hc_sstart_t starts[HC_SCTX_MAX_STARTS];
    int32_t     n_starts;
    hc_be_recorder_t be;
    /* references: 직렬화 시 파생 (radius-8 스캔 + LongOpenHashSet 에뮬) */
} hc_sctx_t;

/* init: 골든 starts 프래그먼트 파스 + mineshaft 배치계산/조립 +
 * references 파생·교차검증 (golden/structures/references.txt 와 fail-loud
 * 대조). structures_dir = golden/structures. err 은 정적 문자열. */
int hc_structures_init(hc_sctx_t *sc, hc_arena_t *a, int64_t seed,
                       const char *structures_dir, const char *template_dir,
                       const hc_json_t *copper_proc_json,
                       const hc_df_source_t *tags, int32_t n_tags,
                       const hc_biome_view_t *view,
                       const hc_biome_reg_t *biomes, const char **err);

/* Beardifier.forStructuresInChunk — (cx,cz) 의 references 스타트 중
 * terrainAdaptation != NONE (이 리전에선 trial_chambers=encapsulate 뿐;
 * 나머지 4종은 26.2 worldgen/structure JSON 에 terrain_adaptation 부재
 * = none, 실측 핀) 을 beard 입력으로 수집. rigids/junctions 는 arena
 * 할당. 반환 = out->has_any (EMPTY 면 0). */
int hc_structures_beard(const hc_sctx_t *sc, hc_arena_t *arena, int32_t cx,
                        int32_t cz, hc_beard_t *out);

/* 데코 워크 스텝 훅 — step 진입 시 (feature 들보다 먼저) 이 청크를
 * 참조하는 스타트들을 스텝 순번대로 배치. rng 회계: 구조물마다
 * setFeatureSeed(deco, step_index, step) 후 피스들 공유 스트림.
 * freg = updateShape 디스패치의 태그 마스크, sea = 드라운드 마커. */
void hc_structures_step(hc_sctx_t *sc, hc_feat_region_t *rg,
                        const hc_feat_reg_t *freg, int32_t sea,
                        int32_t cx, int32_t cz, int64_t deco_seed,
                        int32_t step);

/* ---------- 배치 구현 (structures_template.c / structures_mineshaft.c) ---------- */

/* 템플릿 계열 피스 하나를 chunkBB (cx,cz 청크) 에 배치. rng = 공유 피처
 * 스트림. start 는 shipwreck 높이 래치 갱신용. freg/sea 는 엣지
 * 디스패치·마커가 소비. */
void hc_splace_template(hc_sctx_t *sc, hc_feat_region_t *rg,
                        const hc_feat_reg_t *freg, int32_t sea,
                        hc_sstart_t *start, hc_spiece_t *p, hc_wgr_t *rng,
                        int32_t cx, int32_t cz);

/* mineshaft 피스 postProcess (A.4) */
void hc_splace_mineshaft(hc_sctx_t *sc, hc_feat_region_t *rg,
                         hc_sstart_t *start, hc_spiece_t *p, hc_wgr_t *rng,
                         int32_t cx, int32_t cz);

/* mineshaft 조립 (A.1/A.2): (scx,scz) 스타트의 피스 리스트 생성.
 * 반환: 피스 수 (arena 할당), stub 좌표를 *stub_* 에 (biome 검사용). */
int32_t hc_mineshaft_assemble(hc_arena_t *a, int64_t seed, int32_t scx,
                              int32_t scz, hc_spiece_t **out,
                              int32_t *stub_x, int32_t *stub_y,
                              int32_t *stub_z);

/* ---------- 직렬화 소비 (chunk_nbt 확장이 사용) ---------- */

/* 이 청크의 starts 컴파운드 (골든 파스 트리 재방출) — 없으면 NULL */
const hc_nbt_t *hc_structures_starts_tag(const hc_sctx_t *sc, int32_t cx,
                                         int32_t cz);

/* 이 청크의 References — name/longs 배열 (LongOpenHashSet toLongArray
 * 순서). 반환 = 구조물 수. 호출자 버퍼 cap. */
int32_t hc_structures_references(const hc_sctx_t *sc, hc_arena_t *scratch,
                                 int32_t cx, int32_t cz, const char **names,
                                 const int64_t **arrays, int32_t *lens,
                                 int32_t cap);

/* 이 청크의 BE 리스트 — 직렬화 순서 (LevelChunk live 맵 → fresh
 * HashSet<BlockPos> 순회 에뮬, R-serialization §4.5). recs_out 에 인덱스. */
int32_t hc_structures_chunk_bes(const hc_sctx_t *sc, int32_t cx, int32_t cz,
                                const hc_be_rec_t **recs_out, int32_t cap);

/* ---------- fastutil 에뮬 (R-fastutil, jmaps.c) ---------- */

/* LongOpenHashSet: 기본 cap 16 (n=32); add 순서로 삽입 후 toLongArray
 * 순서 (0 먼저(있으면) + 인덱스 내림차순) 를 out 에 쓴다. */
int32_t hc_longset_to_array(const int64_t *added, int32_t n, int64_t *out);

/* Object2ObjectOpenHashMap<BlockPos> keys() 순회: 삽입 순서 → 방출 순서
 * (mix32(hashCode)&mask 슬롯, 전방 프로브, 인덱스 내림차순 방출). */
int32_t hc_o2omap_key_order(const int32_t (*pos)[3], int32_t n,
                            int32_t *order_out);

/* java HashSet<BlockPos> 순회 (jset.c 재사용 규약) — chunk_nbt 쪽 사용 */

#endif /* HC_STRUCTURES_H */
