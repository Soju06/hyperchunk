/* Task 13 — LevelChunk.postProcessGeneration(ServerLevel) 재현.
 *
 * 시맨틱 출처: 26.2 서버 jar javap 핀 (완료 노트 .hermes/notes/task13-close/
 * 에 인용 원문). 요지:
 *  - 드레인: 섹션 오름차순 × ShortList append 순, 중복 유지. per pos:
 *    fluid 비면 아니면 FluidState.tick (fall-through); LiquidBlock 이면
 *    BlockState.tick (버블 컬럼 전용); 아니면 updateFromNeighbourShapes 후
 *    참조-부등 시 setBlock(pos, new, 276).
 *  - 라이브 setBlock(flags): LevelChunk 쓰기(+FINAL 하이트맵) → onPlace
 *    (flags&512==0) → updateNeighborsAt (flags&1; W,E,D,U,N,S;
 *    CollectingNeighborUpdater: LIFO 스택 + 같은-레이어 역순 push =
 *    레이어 내 FIFO, MultiNeighborUpdate 는 runNext 당 한 방향) →
 *    updateNeighbourShapes (flags&16==0; W,E,N,S,D,U; 중첩 flags &-34).
 *  - FlowingFluid 물: tick/spread/getNewLiquid/spreadToSides(EnumMap
 *    ordinal 적용 순 N,S,W,E; 평가 순 N,E,S,W)/경사 DFS (조기 return,
 *    프로브 N,E,S,W, slopeFindDistance 4)/canPassThroughWall (충돌 형상
 *    full/empty 식별; 부분 형상 조우는 die).
 *  - 라이브 스케줄: Level.scheduleTick → 청크 컨테이너 first-wins dedup
 *    (월드젠 pendingTicks 포함) — hc_feat_schedule_tick 의 dedup 과 동일
 *    키 (Fluid 객체 = kind, Block = 베이스명). t = 지연값 (물 5, 모래 2,
 *    잎 1, 용암 30, 버블 예약 20) — 스케줄 게임타임 == 저장 게임타임
 *    (캡처 검증: 승격 gameTime 전부 == LastUpdate).
 *
 * 게이트 밖 리스크는 die 로 fail-loud: 부분 충돌 형상, 용암 상태 변화/
 * 스프레드, 미모델 반응 블록의 지지 상실. */

#include "hc_postprocess.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hc_blocks.h"
#include "features_internal.h" /* hc_featx_can_survive (Task 14) */

static _Noreturn void pp_die(const char *what, int32_t x, int32_t y,
                             int32_t z) {
    fprintf(stderr, "hc_postprocess FATAL: %s at (%d,%d,%d)\n", what, x, y, z);
    abort();
}

static int32_t fdiv16(int32_t v) {
    return v >> 4;
}

/* ---------- 유체 상태 (FluidState) ---------- */

enum { FL_NONE = 0, FL_WATER = 1, FL_LAVA = 2 };
typedef struct {
    uint8_t type, amount, source, falling;
} fl_t;

static fl_t fluid_of(uint16_t id) {
    fl_t f = {FL_NONE, 0, 0, 0};
    if (id == HC_B_WATER) {
        f.type = FL_WATER;
        f.amount = 8;
        f.source = 1;
    } else if (id == HC_B_LAVA) {
        f.type = FL_LAVA;
        f.amount = 8;
        f.source = 1;
    } else if (id >= HC_B_WATER_FLOW_BASE && id < HC_B_WATER_FLOW_BASE + 15) {
        int level = id - HC_B_WATER_FLOW_BASE + 1;
        f.type = FL_WATER;
        if (level >= 8) {
            f.amount = 8;
            f.falling = 1;
        } else {
            f.amount = (uint8_t)(8 - level);
        }
    } else if (hc_block_is_waterlogged(id)) {
        /* waterlogged=true 상태 + kelp/seagrass/bubble_column — 소스 물 */
        f.type = FL_WATER;
        f.amount = 8;
        f.source = 1;
    }
    return f;
}

static int fl_eq(fl_t a, fl_t b) {
    return a.type == b.type && a.amount == b.amount && a.source == b.source &&
           a.falling == b.falling;
}

/* createLegacyBlock: source→level 0; else level = (8-min(amount,8)) +
 * falling*8 (getLegacyLevel @0-44) */
static uint16_t water_legacy(fl_t f) {
    if (f.type != FL_WATER)
        pp_die("legacy block for non-water fluid", 0, 0, 0);
    if (f.source)
        return HC_B_WATER;
    int level = (8 - (f.amount > 8 ? 8 : f.amount)) + (f.falling ? 8 : 0);
    return (uint16_t)(HC_B_WATER_FLOW_BASE + level - 1);
}

static fl_t water_flowing(int amount, int falling) {
    fl_t f = {FL_WATER, (uint8_t)amount, 0, (uint8_t)falling};
    return f;
}

/* ---------- 충돌 형상 클래스 (canPassThroughWall 용) ---------- */

enum { COLL_EMPTY = 0, COLL_FULL = 1, COLL_PARTIAL = 2 };

static int coll_class(uint16_t id) {
    if (!hc_block_blocks_motion(id))
        return COLL_EMPTY; /* 유체/식물/공기 — 충돌 없음 */
    if (hc_block_is_full_cube(id) || hc_block_is_leaves(id) ||
        id == HC_B_SPAWNER)
        return COLL_FULL;
    return COLL_PARTIAL; /* chest/자수정 클러스터 등 — 조우 시 die */
}

/* ---------- waterloggable 컨테이너 (LiquidBlockContainer) ----------
 * NAMES 스캔으로 1회 구축: "waterlogged=false" 를 지닌 상태 = 미로그
 * 컨테이너, 대응 wl=true 상태를 이름 치환으로 찾는다. seagrass/kelp 류는
 * canPlaceLiquid=false 컨테이너 (스프레드 유입 불가). */

static uint8_t  g_wl_ready;
static int32_t  g_wl_true_of[HC_B_COUNT]; /* -1 = 컨테이너 아님/불가 */

static void build_wl_table(void) {
    for (int i = 0; i < HC_B_COUNT; i++)
        g_wl_true_of[i] = -1;
    for (int i = 0; i < HC_B_COUNT; i++) {
        const char *n = hc_block_name((uint16_t)i);
        const char *p = strstr(n, "waterlogged=false");
        if (!p)
            continue;
        char want[160];
        size_t pre = (size_t)(p - n);
        if (pre + 32 >= sizeof want)
            continue;
        memcpy(want, n, pre);
        memcpy(want + pre, "waterlogged=true", 16);
        strcpy(want + pre + 16, p + 17);
        int32_t wl = hc_block_by_name(want, (int32_t)strlen(want));
        g_wl_true_of[i] = wl; /* -1 = wl 변형 미등재 → placeLiquid 시 die */
    }
    g_wl_ready = 1;
}

static int is_container(uint16_t id) {
    /* strstr("waterlogged=") 보유 상태 전부 + kelp/seagrass 류 (항상 물) */
    if (strstr(hc_block_name(id), "waterlogged="))
        return 1;
    if ((id >= HC_B_KELP_BASE && id <= HC_B_KELP_PLANT) ||
        id == HC_B_SEAGRASS || id == HC_B_TALL_SEAGRASS_LOWER ||
        id == HC_B_TALL_SEAGRASS_UPPER)
        return 1;
    return 0;
}

/* ---------- 지역 접근 ---------- */

typedef struct {
    hc_feat_region_t *rg;
} pp_t;

static uint16_t ppget(pp_t *pp, int32_t x, int32_t y, int32_t z) {
    return hc_feat_get_block(pp->rg, x, y, z);
}

/* ---------- 스케줄 ---------- */

static void sched_fluid(pp_t *pp, int32_t x, int32_t y, int32_t z, fl_t f,
                        int32_t delay) {
    int kind;
    if (f.type == FL_WATER)
        kind = f.source ? HC_TICK_WATER : HC_TICK_FLOWING_WATER;
    else
        kind = f.source ? HC_TICK_LAVA : HC_TICK_FLOWING_LAVA;
    hc_feat_schedule_tick(pp->rg, x, y, z, 0, kind, delay);
}

static void sched_block(pp_t *pp, int32_t x, int32_t y, int32_t z,
                        uint16_t block, int32_t delay) {
    hc_feat_schedule_tick(pp->rg, x, y, z, block, HC_TICK_BLOCK, delay);
}

/* ---------- 방향 ---------- */
/* 바닐라 ordinal: 0=D,1=U,2=N,3=S,4=W,5=E */
static const int32_t DSTEP[6][3] = {{0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                                    {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};
static const int UPDATE_ORDER[6] = {4, 5, 0, 1, 2, 3};       /* W,E,D,U,N,S */
static const int UPDATE_SHAPE_ORDER[6] = {4, 5, 2, 3, 0, 1}; /* W,E,N,S,D,U */
static const int HORIZ_EVAL[4] = {2, 5, 3, 4}; /* N,E,S,W (Plane 배열 순) */
static const int HORIZ_APPLY[4] = {2, 3, 4, 5}; /* N,S,W,E (EnumMap ordinal) */

/* ---------- CollectingNeighborUpdater ---------- */

enum { UPD_MULTI = 0, UPD_SHAPE = 1 };
typedef struct {
    uint8_t  kind;
    int8_t   next_dir; /* MULTI 진행 인덱스 */
    int8_t   dir;      /* SHAPE: 갱신 방향 (수신자 관점) */
    int32_t  x, y, z;  /* MULTI: 원점 / SHAPE: 수신 pos */
    int32_t  nx, ny, nz; /* SHAPE: neighborPos */
    uint16_t aux;        /* MULTI: sourceBlock / SHAPE: neighborState */
    uint8_t  flags;      /* SHAPE 전용 */
} upd_t;

enum { UPD_STACK_CAP = 8192, UPD_LAYER_CAP = 1024 };
static upd_t   g_stack[UPD_STACK_CAP];
static int32_t g_sp;
static upd_t   g_layer[UPD_LAYER_CAP];
static int32_t g_ln;
static int64_t g_count; /* CollectingNeighborUpdater.count */

static int  run_next(pp_t *pp, upd_t *e);
static void pp_set_block(pp_t *pp, int32_t x, int32_t y, int32_t z,
                         uint16_t ns, int flags);

static void run_updates(pp_t *pp) {
    while (g_sp > 0 || g_ln > 0) {
        for (int32_t i = g_ln - 1; i >= 0; i--) {
            if (g_sp >= UPD_STACK_CAP)
                pp_die("neighbor updater stack overflow", 0, 0, 0);
            g_stack[g_sp++] = g_layer[i];
        }
        g_ln = 0;
        if (g_sp == 0)
            continue;
        while (g_ln == 0) {
            if (!run_next(pp, &g_stack[g_sp - 1])) {
                g_sp--;
                break;
            }
        }
    }
    g_count = 0;
}

static void add_and_run(pp_t *pp, upd_t e) {
    int already = g_count > 0;
    g_count++;
    if (already) {
        if (g_ln >= UPD_LAYER_CAP)
            pp_die("neighbor updater layer overflow", 0, 0, 0);
        g_layer[g_ln++] = e;
    } else {
        g_stack[g_sp++] = e;
    }
    if (!already)
        run_updates(pp);
}

/* ---------- neighborChanged 디스패치 ---------- */

static int is_liquid_block(uint16_t id) {
    return id == HC_B_WATER || id == HC_B_LAVA ||
           (id >= HC_B_WATER_FLOW_BASE && id < HC_B_WATER_FLOW_BASE + 15);
}

/* LiquidBlock.shouldSpreadLiquid — 물은 항상 true; 용암 상호작용은
 * 게이트 데이터에 없다 (리전 전체 t=30/변환 0건) — 조우 시 die */
static int should_spread_liquid(pp_t *pp, int32_t x, int32_t y, int32_t z,
                                uint16_t state) {
    fl_t f = fluid_of(state);
    if (f.type != FL_LAVA)
        return 1;
    /* 프로브 UP,N,S,W,E — 물 접촉 = obsidian/cobblestone 변환 경로 */
    static const int probe[5] = {1, 2, 3, 4, 5};
    for (int i = 0; i < 5; i++) {
        uint16_t q = ppget(pp, x + DSTEP[probe[i]][0], y + DSTEP[probe[i]][1],
                           z + DSTEP[probe[i]][2]);
        if (fluid_of(q).type == FL_WATER)
            pp_die("lava-water interaction in postprocess (unmodeled)", x, y,
                   z);
    }
    return 1;
}

/* LiquidBlock.tryScheduleBubbleBlockColumn: 아래가 drag/push 태그 →
 * 블록 틱 delay 20 (magma_block 만 해당 — soul_sand 미등재) */
static void try_sched_bubble(pp_t *pp, int32_t x, int32_t y, int32_t z,
                             uint16_t self_block, uint16_t below) {
    if (below == HC_B_MAGMA_BLOCK)
        sched_block(pp, x, y, z, self_block, 20);
}

static int bubble_can_occupy(uint16_t s) {
    /* bubble_column || (물 태그 && LiquidBlock && 소스 && amount>=8) */
    if (s == HC_B_BUBBLE_COLUMN_DRAG || s == HC_B_BUBBLE_COLUMN_PUSH)
        return 1;
    return s == HC_B_WATER;
}

static int should_bubble_occupy(uint16_t s) {
    /* 자기 유체가 물 소스 && full && LiquidBlock — water[level=0] 만 */
    return s == HC_B_WATER;
}

static void neighbor_changed(pp_t *pp, int32_t x, int32_t y, int32_t z,
                             uint16_t src_block) {
    (void)src_block; /* orientation 없음, movedByPiston=false (핀 §5) */
    uint16_t state = ppget(pp, x, y, z);
    if (is_liquid_block(state)) {
        fl_t f = fluid_of(state);
        if (should_spread_liquid(pp, x, y, z, state))
            sched_fluid(pp, x, y, z, f, f.type == FL_LAVA ? 30 : 5);
        if (should_bubble_occupy(state))
            try_sched_bubble(pp, x, y, z, state,
                             ppget(pp, x, y - 1, z));
    }
    /* 그 외 블록: BlockBehaviour.neighborChanged 기본 no-op */
}

/* ---------- updateShape 디스패치 ----------
 * 수신자 s@ (x,y,z), 방향 dir (수신자→이웃), 이웃 상태 ns.
 * 반환 = 새 상태 (참조 동치 = 무변화). 스케줄은 여기서 바로 기록. */

static int is_leaf_family(uint16_t s) {
    return hc_block_is_leaves(s);
}

static int leaves_distance(uint16_t s) {
    /* 잎 인코딩: BASE + [flowering*14 +] wl*7 + (distance-1) —
     * 모든 잎 패밀리에서 (s - base) % 7 + 1 (blocks.c 공식) */
    if (s >= HC_B_OAK_LEAVES_BASE && s < HC_B_OAK_LEAVES_BASE + 14)
        return (s - HC_B_OAK_LEAVES_BASE) % 7 + 1;
    if (s >= HC_B_JUNGLE_LEAVES_BASE && s < HC_B_JUNGLE_LEAVES_BASE + 14)
        return (s - HC_B_JUNGLE_LEAVES_BASE) % 7 + 1;
    if (s >= HC_B_AZALEA_LEAVES_BASE && s < HC_B_AZALEA_LEAVES_BASE + 14)
        return (s - HC_B_AZALEA_LEAVES_BASE) % 7 + 1;
    if (s >= HC_B_FLOWERING_AZALEA_LEAVES_BASE &&
        s < HC_B_FLOWERING_AZALEA_LEAVES_BASE + 14)
        return (s - HC_B_FLOWERING_AZALEA_LEAVES_BASE) % 7 + 1;
    {
        /* acacia — T14 [base, base+14), wl*7+(d-1) (Task 14) */
        uint16_t ac = hc_block_acacia_leaves_base();
        if (s >= ac && s < ac + 14)
            return (s - ac) % 7 + 1;
    }
    return 7;
}

static int is_log_like(uint16_t s) {
    /* #prevents_nearby_leaf_decay ≈ 로그 — 이름 베이스 "_log"/"_wood" */
    const char *n = hc_block_name(s);
    const char *br = strchr(n, '[');
    size_t      len = br ? (size_t)(br - n) : strlen(n);
    if (len >= 4 && (memcmp(n + len - 4, "_log", 4) == 0))
        return 1;
    if (len >= 5 && (memcmp(n + len - 5, "_wood", 5) == 0))
        return 1;
    return 0;
}

static int optional_dist(uint16_t ns) {
    if (is_log_like(ns))
        return 0;
    if (is_leaf_family(ns))
        return leaves_distance(ns);
    return -1; /* empty → 7 */
}

static int is_falling_block(uint16_t s) {
    return s == HC_B_SAND || s == HC_B_RED_SAND || s == HC_B_GRAVEL;
}

/* 미모델 반응 패밀리 진단 카운터 (완료 노트에 기록) */
static int64_t g_unmodeled_veg_eval;

static int is_plantish(uint16_t s) {
    /* 충돌 없음 + 비공기 + 비유체 + 비-lichen — 지지 상실 시 파괴되는
     * 초목 근사 클래스 */
    if (hc_block_is_air(s) || fluid_of(s).type != FL_NONE)
        return 0;
    if (hc_block_blocks_motion(s))
        return 0;
    if (s >= HC_B_GLOW_LICHEN_BASE && s < HC_B_GLOW_LICHEN_BASE + 126)
        return 0;
    return 1;
}

static int is_lichen_state(uint16_t s) {
    return s >= HC_B_GLOW_LICHEN_BASE && s < HC_B_GLOW_LICHEN_BASE + 126;
}

/* lichen face bit ↔ 방향 (blocks.c 규약: down,east,north,south,up,west) */
static const int LICHEN_BIT[6] = {0 /*D*/, 4 /*U*/, 2 /*N*/,
                                  3 /*S*/, 5 /*W*/, 1 /*E*/};

static uint16_t update_shape(pp_t *pp, uint16_t s, int32_t x, int32_t y,
                             int32_t z, int dir, uint16_t ns) {
    if (hc_block_is_air(s))
        return s;
    if (is_liquid_block(s)) {
        /* LiquidBlock.updateShape @0-85 */
        fl_t f = fluid_of(s), nf = fluid_of(ns);
        if (f.source || nf.source)
            sched_fluid(pp, x, y, z, f, f.type == FL_LAVA ? 30 : 5);
        if (dir == 0 && should_bubble_occupy(s))
            try_sched_bubble(pp, x, y, z, s, ns);
        return s;
    }
    if (is_leaf_family(s)) {
        /* LeavesBlock.updateShape (R-D §1) — postProcess 는 라이브 지연 */
        if (hc_block_is_waterlogged(s)) {
            fl_t wf = {FL_WATER, 8, 1, 0};
            sched_fluid(pp, x, y, z, wf, 5);
        }
        int od = optional_dist(ns);
        int i = (od < 0 ? 7 : od) + 1;
        int d = leaves_distance(s);
        if (!(i == 1 && d == 1))
            sched_block(pp, x, y, z, s, 1);
        return s;
    }
    if (is_falling_block(s)) {
        sched_block(pp, x, y, z, s, 2); /* FallingBlock @0-8, 무조건 delay 2 */
        return s;
    }
    if (s == HC_B_BUBBLE_COLUMN_DRAG || s == HC_B_BUBBLE_COLUMN_PUSH) {
        fl_t wf = {FL_WATER, 8, 1, 0};
        sched_fluid(pp, x, y, z, wf, 5); /* 항상 물 틱 */
        uint16_t below = ppget(pp, x, y - 1, z);
        int can_survive = below == HC_B_MAGMA_BLOCK || below == s ||
                          below == HC_B_BUBBLE_COLUMN_DRAG ||
                          below == HC_B_BUBBLE_COLUMN_PUSH;
        if (!can_survive || dir == 0 ||
            (dir == 1 && ns != s && bubble_can_occupy(ns)))
            sched_block(pp, x, y, z, s, 5);
        return s;
    }
    if (is_lichen_state(s)) {
        /* MultifaceBlock.updateShape: wl → 물 틱; dir 면 보유 && 부착
         * 불가 → removeFace; 면 0 → AIR */
        int mask = (s - HC_B_GLOW_LICHEN_BASE) % 63 + 1;
        int wl = (s - HC_B_GLOW_LICHEN_BASE) / 63;
        if (wl) {
            fl_t wf = {FL_WATER, 8, 1, 0};
            sched_fluid(pp, x, y, z, wf, 5);
        }
        int bit = LICHEN_BIT[dir];
        if ((mask >> bit) & 1) {
            int attach =
                hc_block_is_full_cube(ns) || hc_block_is_leaves(ns);
            if (!attach) {
                mask &= ~(1 << bit);
                if (mask == 0)
                    return wl ? HC_B_WATER : HC_B_AIR;
                return (uint16_t)(HC_B_GLOW_LICHEN_BASE + wl * 63 +
                                  (mask - 1));
            }
        }
        return s;
    }
    {
        /* 수생 식물/워터로그 일반 — updateShape 물 틱 (26.2 디컴파일 핀,
         * task14-decomp/blocks2). fluid_of 가 물인 비유체 블록:
         *  - tall_seagrass: TallSeagrassBlock 은 물 틱을 걸지 않는다.
         *  - seagrass: super(Vegetation) 폴드 생존 시 (결과 비-air) 틱.
         *  - kelp/kelp_plant (GrowingPlant head/body, scheduleFluidTicks
         *    =true): head↔body 변환·지지 상실 경로는 이 리전 골든에
         *    부재 — 발생하면 die (커버리지 경계). 그 외 무조건 틱.
         *  - 기타 (big_dripleaf(_stem) 포함 SimpleWaterlogged 공통):
         *    waterlogged=true 무조건 틱. */
        fl_t sf = fluid_of(s);
        if (sf.type == FL_WATER) {
            const char *nm = hc_block_name(s);
            if (strncmp(nm, "minecraft:tall_seagrass", 23) == 0)
                return s;
            fl_t wf = {FL_WATER, 8, 1, 0};
            if (s == HC_B_SEAGRASS) {
                uint16_t below = ppget(pp, x, y - 1, z);
                if (hc_block_is_air(below) ||
                    fluid_of(below).type != FL_NONE)
                    return HC_B_AIR; /* canSurvive 실패 → AIR, 틱 없음 */
                sched_fluid(pp, x, y, z, wf, 5);
                return s;
            }
            int is_kelp_head = strncmp(nm, "minecraft:kelp[", 15) == 0;
            int is_kelp_body = strcmp(nm, "minecraft:kelp_plant") == 0;
            if (is_kelp_head || is_kelp_body) {
                const char *nn = hc_block_name(ns);
                int ns_fam = strncmp(nn, "minecraft:kelp[", 15) == 0 ||
                             strcmp(nn, "minecraft:kelp_plant") == 0;
                if (dir == 1 && is_kelp_body && !ns_fam)
                    pp_die("kelp body->head conversion unmodeled", x, y, z);
                if (dir == 1 && is_kelp_head && ns_fam)
                    pp_die("kelp head->body conversion unmodeled", x, y, z);
                if (dir == 0) {
                    uint16_t below = ppget(pp, x, y - 1, z);
                    if (hc_block_is_air(below) ||
                        is_liquid_block(below))
                        pp_die("kelp support loss unmodeled", x, y, z);
                }
                sched_fluid(pp, x, y, z, wf, 5);
                return s;
            }
            sched_fluid(pp, x, y, z, wf, 5);
            return s;
        }
    }
    if (is_plantish(s)) {
        /* VegetationBlock/MushroomBlock.updateShape (26.2 javap): 방향
         * 무관 canSurvive 폴드 — 실패 시 AIR. 실측 클래스 (Task 14):
         * 이웃 disk 가 아래를 sand/gravel 로 치환한 마크 셀의 초지 42+9
         * 셀 (supports_vegetation 탈락). 매핑 패밀리만 정확 평가. */
        if (pp->rg->survive_reg &&
            (s == HC_B_SHORT_GRASS || s == HC_B_FERN ||
             s == HC_B_BROWN_MUSHROOM || s == HC_B_RED_MUSHROOM)) {
            if (!hc_featx_can_survive(pp->rg->survive_reg, pp->rg, s, x, y,
                                      z))
                return HC_B_AIR;
            return s;
        }
        /* 지지 상실 근사 (완료 노트 참조): 아래가 공기/유체가 되면 파괴
         * (스프레드 유발 지지 제거를 정확히 커버); 그 외엔 생존 no-op.
         * 측면 규칙(수련잎 등) 미모델 — 진단 카운트. */
        g_unmodeled_veg_eval++;
        if (dir == 0) {
            if (hc_block_is_air(ns) || fluid_of(ns).type != FL_NONE)
                return HC_B_AIR; /* updateOrDestroy 가 유체 복원 처리 */
        }
        return s;
    }
    return s; /* 기본 (돌/광석/이끼/점토 등): 상태 유지, 스케줄 없음 */
}

/* ---------- updateOrDestroy / destroyBlock ---------- */

static void update_or_destroy(pp_t *pp, uint16_t cur, uint16_t ns, int32_t x,
                              int32_t y, int32_t z, int flags) {
    if (ns == cur)
        return;
    if (hc_block_is_air(ns)) {
        /* Level.destroyBlock: 유체 잔존 상태로 치환, flags 3 (drops 무시) */
        fl_t f = fluid_of(cur);
        uint16_t repl = HC_B_AIR;
        if (f.type == FL_WATER)
            repl = water_legacy(f);
        else if (f.type == FL_LAVA)
            pp_die("lava-bearing block destroyed in postprocess", x, y, z);
        pp_set_block(pp, x, y, z, repl, 3);
    } else {
        pp_set_block(pp, x, y, z, ns, flags & ~32);
    }
}

static int run_next(pp_t *pp, upd_t *e) {
    if (e->kind == UPD_MULTI) {
        if (e->next_dir >= 6)
            return 0;
        int dir = UPDATE_ORDER[e->next_dir];
        int32_t qx = e->x + DSTEP[dir][0], qy = e->y + DSTEP[dir][1],
                qz = e->z + DSTEP[dir][2];
        e->next_dir++;
        neighbor_changed(pp, qx, qy, qz, e->aux);
        return e->next_dir < 6;
    }
    /* SHAPE: executeShapeUpdate @0-123 */
    uint16_t cur = ppget(pp, e->x, e->y, e->z);
    uint16_t ns =
        update_shape(pp, cur, e->x, e->y, e->z, e->dir, e->aux);
    update_or_destroy(pp, cur, ns, e->x, e->y, e->z, e->flags);
    return 0;
}

/* ---------- Level.setBlock (라이브) ---------- */

static void on_place(pp_t *pp, int32_t x, int32_t y, int32_t z,
                     uint16_t state) {
    if (is_liquid_block(state)) {
        fl_t f = fluid_of(state);
        if (should_spread_liquid(pp, x, y, z, state))
            sched_fluid(pp, x, y, z, f, f.type == FL_LAVA ? 30 : 5);
        if (should_bubble_occupy(state))
            try_sched_bubble(pp, x, y, z, state, ppget(pp, x, y - 1, z));
    }
    /* FallingBlock.onPlace 도 delay-2 스케줄이지만 postProcess 는 낙하
     * 블록을 새로 놓지 않는다 (스프레드 산물은 유체뿐) */
    if (is_falling_block(state))
        sched_block(pp, x, y, z, state, 2);
}

static void pp_set_block(pp_t *pp, int32_t x, int32_t y, int32_t z,
                         uint16_t ns, int flags) {
    uint16_t old = ppget(pp, x, y, z);
    if (old == ns)
        return; /* LevelChunk.setBlockState 동일-참조 null 경로 */
    if (!hc_feat_set_block(pp->rg, x, y, z, ns))
        pp_die("postprocess write outside region window", x, y, z);
    /* onPlace (flags&512==0 — 3/276/2 전부 실행) */
    on_place(pp, x, y, z, ns);
    /* updateNeighborsAt (flags&1): old 블록이 source 인자 */
    if (flags & 1) {
        upd_t e;
        memset(&e, 0, sizeof e);
        e.kind = UPD_MULTI;
        e.x = x;
        e.y = y;
        e.z = z;
        e.aux = old;
        add_and_run(pp, e);
    }
    /* updateNeighbourShapes (flags&16==0): W,E,N,S,D,U; 중첩 flags &-34
     * (Java -34 == ~33 — UPDATE_NEIGHBORS(1)|UPDATE_SUPPRESS_DROPS(32)
     * 클리어; ~34 로 옮기면 비트 2 를 지우는 오역이 된다) */
    if (!(flags & 16)) {
        int k = (flags & -34) & 0xFF;
        for (int i = 0; i < 6; i++) {
            int dir = UPDATE_SHAPE_ORDER[i];
            int32_t qx = x + DSTEP[dir][0], qy = y + DSTEP[dir][1],
                    qz = z + DSTEP[dir][2];
            upd_t e;
            memset(&e, 0, sizeof e);
            e.kind = UPD_SHAPE;
            /* 수신자 = 이웃; 방향 = dir.getOpposite() (이웃→우리) */
            e.dir = (int8_t)(dir ^ 1);
            e.x = qx;
            e.y = qy;
            e.z = qz;
            e.nx = x;
            e.ny = y;
            e.nz = z;
            e.aux = ns;
            e.flags = (uint8_t)k;
            add_and_run(pp, e);
        }
    }
}

/* ---------- FlowingFluid (물; 용암은 변화 시 die) ---------- */

static int can_pass_through_wall(pp_t *pp, int dir, uint16_t s1, int32_t x1,
                                 int32_t y1, int32_t z1, uint16_t s2,
                                 int32_t x2, int32_t y2, int32_t z2) {
    (void)pp;
    (void)dir;
    int c2 = coll_class(s2);
    if (c2 == COLL_FULL)
        return 0;
    int c1 = coll_class(s1);
    if (c1 == COLL_FULL)
        return 0;
    if (c1 == COLL_EMPTY && c2 == COLL_EMPTY)
        return 1;
    pp_die("partial collision shape in canPassThroughWall", x2, y2, z2);
    (void)x1;
    (void)y1;
    (void)z1;
}

static int can_hold_any_fluid(uint16_t s) {
    if (is_container(s))
        return 1;
    if (hc_block_blocks_motion(s))
        return 0;
    if (s == HC_B_SUGAR_CANE || s == HC_B_BUBBLE_COLUMN_DRAG ||
        s == HC_B_BUBBLE_COLUMN_PUSH)
        return 0;
    return 1;
}

static int can_hold_specific(uint16_t s, int fl_type) {
    if (!g_wl_ready)
        build_wl_table();
    if (is_container(s)) {
        /* SimpleWaterlogged 기본: 물 && waterlogged=false */
        if (fl_type != FL_WATER)
            return 0;
        if ((s >= HC_B_KELP_BASE && s <= HC_B_KELP_PLANT) ||
            s == HC_B_SEAGRASS || s == HC_B_TALL_SEAGRASS_LOWER ||
            s == HC_B_TALL_SEAGRASS_UPPER)
            return 0; /* canPlaceLiquid=false 컨테이너 */
        return g_wl_true_of[s] >= 0 &&
               strstr(hc_block_name(s), "waterlogged=false") != NULL;
    }
    return 1;
}

static int is_source_of_type(fl_t f, int type) {
    return f.type == type && f.source;
}

static int can_maybe_pass(pp_t *pp, int32_t x, int32_t y, int32_t z,
                          uint16_t s, int dir, int32_t qx, int32_t qy,
                          int32_t qz, uint16_t qs, fl_t qf, int fl_type) {
    return !is_source_of_type(qf, fl_type) && can_hold_any_fluid(qs) &&
           can_pass_through_wall(pp, dir, s, x, y, z, qs, qx, qy, qz);
}

static fl_t get_new_liquid(pp_t *pp, int32_t x, int32_t y, int32_t z,
                           uint16_t bs, int fl_type);

static int can_be_replaced_with(fl_t existing, int incoming_type, int dir) {
    if (existing.type == FL_NONE)
        return 1;
    if (existing.type == FL_WATER)
        return dir == 0 && incoming_type != FL_WATER;
    /* lava: height >= 4/9 && incoming water — 미조우 경로 */
    return 0;
}

static int can_hold_fluid(uint16_t s, int fl_type) {
    return can_hold_any_fluid(s) && can_hold_specific(s, fl_type);
}

static int is_water_hole(pp_t *pp, int32_t x, int32_t y, int32_t z,
                         uint16_t s, int32_t bx, int32_t by, int32_t bz,
                         uint16_t bstate, int fl_type) {
    if (!can_pass_through_wall(pp, 0, s, x, y, z, bstate, bx, by, bz))
        return 0;
    if (fluid_of(bstate).type == fl_type)
        return 1;
    return can_hold_fluid(bstate, fl_type);
}

static int slope_distance(pp_t *pp, int32_t x, int32_t y, int32_t z,
                          uint16_t s, int depth, int exclude, int fl_type) {
    int best = 1000;
    for (int i = 0; i < 4; i++) {
        int dir = HORIZ_EVAL[i];
        if (dir == exclude)
            continue;
        int32_t qx = x + DSTEP[dir][0], qy = y, qz = z + DSTEP[dir][2];
        uint16_t qs = ppget(pp, qx, qy, qz);
        fl_t     qf = fluid_of(qs);
        /* canPassThrough = canMaybePassThrough && canHoldSpecific */
        if (!can_maybe_pass(pp, x, y, z, s, dir, qx, qy, qz, qs, qf, fl_type))
            continue;
        if (!can_hold_specific(qs, fl_type))
            continue;
        uint16_t below = ppget(pp, qx, qy - 1, qz);
        if (is_water_hole(pp, qx, qy, qz, qs, qx, qy - 1, qz, below, fl_type))
            return depth; /* 조기 return — 프로브 순 N,E,S,W */
        if (depth < 4 /* water slopeFindDistance */) {
            int d2 = slope_distance(pp, qx, qy, qz, qs, depth + 1, dir ^ 1,
                                    fl_type);
            if (d2 < best)
                best = d2;
        }
    }
    return best;
}

static void spread_to(pp_t *pp, int32_t x, int32_t y, int32_t z, uint16_t qs,
                      fl_t nf) {
    if (nf.type != FL_WATER)
        pp_die("non-water spreadTo in postprocess", x, y, z);
    if (is_container(qs) && can_hold_specific(qs, FL_WATER)) {
        /* LiquidBlockContainer.placeLiquid — waterlogged=true 로 전환 */
        if (!g_wl_ready)
            build_wl_table();
        int32_t wl = g_wl_true_of[qs];
        if (wl < 0)
            pp_die("placeLiquid target without wl=true state", x, y, z);
        pp_set_block(pp, x, y, z, (uint16_t)wl, 3);
        return;
    }
    /* beforeDestroyingBlock (drops) 는 관측 무영향 */
    pp_set_block(pp, x, y, z, water_legacy(nf), 3);
}

static fl_t get_new_liquid(pp_t *pp, int32_t x, int32_t y, int32_t z,
                           uint16_t bs, int fl_type) {
    int max_a = 0, sources = 0;
    for (int i = 0; i < 4; i++) {
        int dir = HORIZ_EVAL[i];
        int32_t qx = x + DSTEP[dir][0], qz = z + DSTEP[dir][2];
        uint16_t qs = ppget(pp, qx, y, qz);
        fl_t     qf = fluid_of(qs);
        if (qf.type != fl_type)
            continue;
        if (!can_pass_through_wall(pp, dir, bs, x, y, z, qs, qx, y, qz))
            continue;
        if (qf.source)
            sources++;
        if (qf.amount > max_a)
            max_a = qf.amount;
    }
    if (sources >= 2 && fl_type == FL_WATER /* WATER_SOURCE_CONVERSION */) {
        uint16_t below = ppget(pp, x, y - 1, z);
        fl_t     bf = fluid_of(below);
        if (hc_block_is_solid(below) || is_source_of_type(bf, fl_type)) {
            fl_t src = {FL_WATER, 8, 1, 0};
            return src;
        }
    }
    {
        uint16_t above = ppget(pp, x, y + 1, z);
        fl_t     af = fluid_of(above);
        if (af.type == fl_type &&
            can_pass_through_wall(pp, 1, bs, x, y, z, above, x, y + 1, z))
            return water_flowing(8, 1);
    }
    int k = max_a - 1 /* water dropOff */;
    if (k <= 0) {
        fl_t none = {FL_NONE, 0, 0, 0};
        return none;
    }
    return water_flowing(k, 0);
}

static void spread_to_sides(pp_t *pp, int32_t x, int32_t y, int32_t z,
                            fl_t f, uint16_t bs) {
    int strength = f.amount - 1; /* getDropOff(water)=1 */
    if (f.falling)
        strength = 7;
    if (strength <= 0)
        return;
    /* getSpread: 평가 N,E,S,W; 적용 EnumMap ordinal N,S,W,E */
    int     have[6] = {0, 0, 0, 0, 0, 0};
    fl_t    put[6];
    int32_t min_dist = 1000;
    for (int i = 0; i < 4; i++) {
        int dir = HORIZ_EVAL[i];
        int32_t qx = x + DSTEP[dir][0], qz = z + DSTEP[dir][2];
        uint16_t qs = ppget(pp, qx, y, qz);
        fl_t     qf = fluid_of(qs);
        if (!can_maybe_pass(pp, x, y, z, bs, dir, qx, y, qz, qs, qf,
                            FL_WATER))
            continue;
        fl_t nl = get_new_liquid(pp, qx, y, qz, qs, FL_WATER);
        if (nl.type != FL_NONE && !can_hold_specific(qs, nl.type))
            continue;
        uint16_t below = ppget(pp, qx, y - 1, qz);
        int d = is_water_hole(pp, qx, y, qz, qs, qx, y - 1, qz, below,
                              FL_WATER)
                    ? 0
                    : slope_distance(pp, qx, y, qz, qs, 1, dir ^ 1, FL_WATER);
        if (d < min_dist) {
            for (int j = 0; j < 6; j++)
                have[j] = 0;
        }
        if (d <= min_dist) {
            /* getNewLiquid 가 EMPTY 를 내는 측면 spread 는 이 창의 역학
             * 으로 도달 불가 (틱 중인 자기 셀 amount>=2 가 항상 이웃) —
             * canBeReplacedWith(EMPTY) 경로는 미모델, 조우 시 die */
            if (nl.type == FL_NONE)
                pp_die("empty getNewLiquid in side-spread (unmodeled)", qx, y,
                       qz);
            if (can_be_replaced_with(qf, nl.type, dir)) {
                /* 바닐라는 nl(getNewLiquid 결과) 를 그대로 put 한다 */
                have[dir] = 1;
                put[dir] = nl;
            }
            min_dist = d; /* put 스킵이어도 갱신 (@222-224) */
        }
    }
    for (int i = 0; i < 4; i++) {
        int dir = HORIZ_APPLY[i];
        if (!have[dir])
            continue;
        int32_t qx = x + DSTEP[dir][0], qz = z + DSTEP[dir][2];
        spread_to(pp, qx, y, qz, ppget(pp, qx, y, qz), put[dir]);
    }
}

static void fluid_spread(pp_t *pp, int32_t x, int32_t y, int32_t z,
                         uint16_t bs, fl_t f) {
    if (f.type == FL_NONE)
        return;
    if (f.type == FL_LAVA) {
        /* 스프레드 성립 여부만 평가 — 성립하면 미모델 (die).
         * 아래로: canMaybePassThrough(DOWN) && 교체 가능 && 수용 가능 */
        uint16_t below = ppget(pp, x, y - 1, z);
        fl_t     bf = fluid_of(below);
        if (!is_source_of_type(bf, FL_LAVA) && can_hold_any_fluid(below) &&
            coll_class(below) == COLL_EMPTY && coll_class(bs) != COLL_FULL)
            pp_die("lava spread (down) in postprocess window", x, y, z);
        /* 옆: 소스가 아니면 홀 검사 게이트 — 등가 평가는 물 경로와 동일
         * 구조라 보수적으로: 4 방향 중 하나라도 유입 가능하면 die */
        for (int i = 0; i < 4; i++) {
            int dir = HORIZ_EVAL[i];
            int32_t qx = x + DSTEP[dir][0], qz = z + DSTEP[dir][2];
            uint16_t qs = ppget(pp, qx, y, qz);
            fl_t     qf = fluid_of(qs);
            if (is_source_of_type(qf, FL_LAVA))
                continue;
            if (qf.type == FL_NONE && can_hold_any_fluid(qs) &&
                coll_class(qs) == COLL_EMPTY && coll_class(bs) != COLL_FULL)
                pp_die("lava spread (side) in postprocess window", x, y, z);
        }
        return;
    }
    /* water spread @0-161 */
    uint16_t below = ppget(pp, x, y - 1, z);
    fl_t     bf = fluid_of(below);
    if (can_maybe_pass(pp, x, y, z, bs, 0, x, y - 1, z, below, bf,
                       FL_WATER)) {
        fl_t nl = get_new_liquid(pp, x, y - 1, z, below, FL_WATER);
        /* 아래 셀의 getNewLiquid 는 위(=틱 중인 물) 를 항상 보므로 EMPTY
         * 불가 — canBeReplacedWith(EMPTY, DOWN)=true 의 물기둥-소거
         * 경로는 미모델, 조우 시 die */
        if (nl.type == FL_NONE)
            pp_die("empty getNewLiquid in down-spread (unmodeled)", x, y - 1,
                   z);
        if (can_be_replaced_with(bf, nl.type, 0) &&
            can_hold_specific(below, nl.type)) {
            spread_to(pp, x, y - 1, z, below, nl);
            int nsrc = 0;
            for (int i = 0; i < 4; i++) {
                int dir = HORIZ_EVAL[i];
                fl_t qf = fluid_of(ppget(pp, x + DSTEP[dir][0], y,
                                         z + DSTEP[dir][2]));
                if (is_source_of_type(qf, FL_WATER))
                    nsrc++;
            }
            if (nsrc >= 3)
                spread_to_sides(pp, x, y, z, f, bs);
            return;
        }
    }
    if (f.source || !is_water_hole(pp, x, y, z, bs, x, y - 1, z, below,
                                   FL_WATER))
        spread_to_sides(pp, x, y, z, f, bs);
}

static void fluid_tick(pp_t *pp, int32_t x, int32_t y, int32_t z,
                       uint16_t bs0, fl_t f0) {
    uint16_t bs = bs0;
    fl_t     f = f0;
    if (!f.source) {
        if (f.type == FL_LAVA)
            pp_die("non-source lava tick in postprocess window", x, y, z);
        uint16_t cur = ppget(pp, x, y, z);
        fl_t     nf = get_new_liquid(pp, x, y, z, cur, f.type);
        if (nf.type == FL_NONE) {
            f = nf;
            bs = HC_B_AIR;
            pp_set_block(pp, x, y, z, HC_B_AIR, 3);
        } else if (!fl_eq(nf, f)) {
            f = nf;
            bs = water_legacy(nf);
            pp_set_block(pp, x, y, z, bs, 3);
            sched_fluid(pp, x, y, z, nf, 5 /* water getSpreadDelay */);
        }
    }
    fluid_spread(pp, x, y, z, bs, f);
}

/* ---------- LiquidBlock.tick → BubbleColumnBlock.updateColumn ---------- */

static void liquid_block_tick(pp_t *pp, int32_t x, int32_t y, int32_t z,
                              uint16_t state) {
    if (!should_bubble_occupy(state))
        return;
    uint16_t below = ppget(pp, x, y - 1, z);
    /* updateColumn(BUBBLE, level, pos, below) — 5-arg @0-83 */
    uint16_t cur = ppget(pp, x, y, z);
    if (!bubble_can_occupy(cur))
        return;
    uint16_t cs;
    if (below == HC_B_BUBBLE_COLUMN_DRAG || below == HC_B_BUBBLE_COLUMN_PUSH)
        cs = below;
    else if (below == HC_B_MAGMA_BLOCK)
        cs = HC_B_BUBBLE_COLUMN_DRAG;
    else if (cur == HC_B_BUBBLE_COLUMN_DRAG || cur == HC_B_BUBBLE_COLUMN_PUSH)
        cs = HC_B_WATER;
    else
        cs = cur; /* no-op */
    pp_set_block(pp, x, y, z, cs, 2); /* 동일 상태면 내부 no-op */
    int32_t my = y + 1;
    while (bubble_can_occupy(ppget(pp, x, my, z))) {
        uint16_t before = ppget(pp, x, my, z);
        pp_set_block(pp, x, my, z, cs, 2);
        if (ppget(pp, x, my, z) == before && before != cs)
            return; /* setBlock 실패 등가 */
        if (before == cs)
            return; /* 바닐라 setBlock=false (동일 참조) → return */
        my++;
    }
}

/* ---------- 드레인 본체 ---------- */

void hc_postprocess_chunk(hc_feat_region_t *rg, int32_t cx, int32_t cz,
                          const hc_ppg_recorder_t *marks) {
    if (!marks->frozen)
        pp_die("postprocess drain on unfrozen recorder (live marks would "
               "corrupt the log)",
               cx, 0, cz);
    if (!g_wl_ready)
        build_wl_table();
    pp_t pp = {rg};
    rg->center_cx = cx;
    rg->center_cz = cz;
    g_sp = 0;
    g_ln = 0;
    g_count = 0;
    /* 섹션 오름차순 × append 순 (stable) — 24 패스 필터 */
    for (int sec = 0; sec < HC_HEIGHT / 16; sec++) {
        for (int32_t i = 0; i < marks->n; i++) {
            const hc_ppg_rec_t *r = &marks->recs[i];
            if (fdiv16(r->x) != cx || fdiv16(r->z) != cz)
                continue;
            if ((r->y - HC_MIN_Y) >> 4 != sec)
                continue;
            uint16_t state = ppget(&pp, r->x, r->y, r->z);
            fl_t     f = fluid_of(state);
            if (f.type != FL_NONE)
                fluid_tick(&pp, r->x, r->y, r->z, state, f);
            if (is_liquid_block(state)) {
                liquid_block_tick(&pp, r->x, r->y, r->z, state);
            } else {
                /* Block.updateFromNeighbourShapes @0-80: W,E,N,S,D,U 접기 */
                uint16_t folded = state;
                for (int di = 0; di < 6; di++) {
                    int dir = UPDATE_SHAPE_ORDER[di];
                    int32_t qx = r->x + DSTEP[dir][0],
                            qy = r->y + DSTEP[dir][1],
                            qz = r->z + DSTEP[dir][2];
                    folded = update_shape(&pp, folded, r->x, r->y, r->z, dir,
                                          ppget(&pp, qx, qy, qz));
                }
                if (folded != state)
                    pp_set_block(&pp, r->x, r->y, r->z, folded, 276);
            }
        }
    }
}

int64_t hc_postprocess_unmodeled_veg_evals(void) {
    return g_unmodeled_veg_eval;
}
