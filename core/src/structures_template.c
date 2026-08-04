/* Task 14 — 템플릿 계열 구조물 배치 (StructureTemplate.placeInWorld 재구성).
 *
 * 커버: shipwreck_beached / ocean_ruin_warm / ruined_portal_ocean
 * (TemplateStructurePiece, flag 2) + trial_chambers 258 피스
 * (PoolElementStructurePiece → SinglePoolElement.place, flag 18).
 *
 * 시맨틱 근거 (전부 26.2 디컴파일 라인 인용 — 세션 워크플로 추출본):
 *  - StructureTemplate.java 263-431 (placeInWorld), 459-503
 *    (processBlockInfos), 437-457 (updateShapeAtEdge), 633-651
 *    (getBoundingBox), 153-184 (버킷/정렬), 755-775 (loadPalette)
 *  - StructurePlaceSettings.java 111-147 (getRandom/getRandomPalette)
 *  - CappedProcessor.java 38-88, BlockRotProcessor.java 42-54,
 *    BlockAgeProcessor.java 37-117, RuleProcessor.java 26-47,
 *    JigsawReplacementProcessor.java 27-55, AppendLoot.java
 *  - ShipwreckPieces.java 130-187, OceanRuinPieces.java 334-415,
 *    RuinedPortalPiece.java 174-322
 *  - PoolElementStructurePiece.java 90-126, SinglePoolElement.java 95-185,
 *    TemplateStructurePiece.java 80-117
 *  - R-placement.md (요약 노트) — 상충 시 디컴파일 인용이 우선.
 *
 * RNG 회계 (공유 피처 스트림 소비 지점):
 *  - 배치 성공한 RandomizableContainer BE 당 nextLong (ST:313-315)
 *  - shipwreck 높이 조정 nextInt(3) (beached, 최초 1회 — S:180-182)
 *  - shipwreck/ocean-ruin 마커 체스트 nextLong (BE 존재 시)
 *  - ruined portal spreadNetherrack/drip/leaves 드로우 (R:252-288)
 * 그 외 (팔레트 선택/프로세서) 는 위치시드 일회용 LCG — 공유 무영향. */

#include "hc_structures.h"
#include "features_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define die hc_featx_die

/* ================= 자주 쓰는 id (lazy) ================= */

static uint16_t id_of(const char *nm) {
    int32_t id = hc_block_by_name(nm, (int32_t)strlen(nm));
    if (id < 0)
        die("block state not registered", nm);
    return (uint16_t)id;
}

static uint16_t g_barrier, g_susp_sand, g_chest_n, g_chest_n_wl,
    g_netherrack, g_magma, g_cracked_sb, g_mossy_sb, g_crying_obsidian;
static int g_ids_ready = 0;

static void ids_init(void) {
    if (g_ids_ready)
        return;
    g_barrier = id_of("minecraft:barrier");
    g_susp_sand = id_of("minecraft:suspicious_sand[dusted=0]");
    g_chest_n = id_of("minecraft:chest[facing=north,type=single,"
                      "waterlogged=false]");
    g_chest_n_wl = id_of("minecraft:chest[facing=north,type=single,"
                         "waterlogged=true]");
    g_netherrack = id_of("minecraft:netherrack");
    g_magma = HC_B_MAGMA_BLOCK;
    g_cracked_sb = id_of("minecraft:cracked_stone_bricks");
    g_mossy_sb = id_of("minecraft:mossy_stone_bricks");
    g_crying_obsidian = id_of("minecraft:crying_obsidian");
    g_ids_ready = 1;
}

/* ================= 상태 이름 헬퍼 ================= */

static const char *base_of(uint16_t s, size_t *len) {
    const char *nm = hc_block_name(s);
    const char *br = strchr(nm, '[');
    *len = br ? (size_t)(br - nm) : strlen(nm);
    return nm;
}

static int base_is(uint16_t s, const char *b) {
    size_t      n;
    const char *nm = base_of(s, &n);
    return n == strlen(b) && strncmp(nm, b, n) == 0;
}

static int base_ends_with(uint16_t s, const char *suf) {
    size_t      n;
    const char *nm = base_of(s, &n);
    size_t      sl = strlen(suf);
    return n >= sl && strncmp(nm + n - sl, suf, sl) == 0;
}

/* EntityBlock 판별 — 이 리전 템플릿/마커 팔레트의 BE 블록 (R-serial §4).
 * structure_block 은 ignore 프로세서가 항상 걸러 배치 불가. */
static int is_entity_block(uint16_t s) {
    return base_is(s, "minecraft:chest") ||
           base_is(s, "minecraft:trapped_chest") ||
           base_is(s, "minecraft:barrel") ||
           base_is(s, "minecraft:dispenser") ||
           base_is(s, "minecraft:dropper") ||
           base_is(s, "minecraft:hopper") ||
           base_is(s, "minecraft:decorated_pot") ||
           base_is(s, "minecraft:suspicious_sand") ||
           base_is(s, "minecraft:suspicious_gravel") ||
           base_is(s, "minecraft:spawner") ||
           base_is(s, "minecraft:trial_spawner") ||
           base_is(s, "minecraft:vault") || base_is(s, "minecraft:jigsaw");
}

/* RandomizableContainer (ST:313 — LootTableSeed 주입 대상) */
static int is_randomizable(uint16_t s) {
    return base_is(s, "minecraft:chest") ||
           base_is(s, "minecraft:trapped_chest") ||
           base_is(s, "minecraft:barrel") ||
           base_is(s, "minecraft:dispenser") ||
           base_is(s, "minecraft:dropper") ||
           base_is(s, "minecraft:hopper") ||
           base_is(s, "minecraft:shulker_box") ||
           base_is(s, "minecraft:crafter") ||
           base_is(s, "minecraft:decorated_pot");
}

/* ================= 유체 모델 ================= */

enum { PF_NONE = 0, PF_WATER_SRC, PF_LAVA_SRC, PF_FLOWING };

static int fluid_kind(uint16_t s) {
    if (s == HC_B_LAVA)
        return PF_LAVA_SRC;
    if (s >= HC_B_WATER_FLOW_BASE && s < HC_B_WATER_FLOW_BASE + 15)
        return PF_FLOWING;
    if (hc_block_fluid_is_water(s)) /* water[level=0]/waterlogged/수중식생 */
        return PF_WATER_SRC;
    return PF_NONE;
}

/* SimpleWaterloggedBlock 근사 = waterlogged 프로퍼티 보유 */
static int has_waterlogged_prop(uint16_t s) {
    return strstr(hc_block_name(s), "waterlogged=") != NULL;
}

/* waterlogged=false → true 재작성 */
static uint16_t waterlogged_variant(uint16_t s) {
    char     bb[128];
    hc_skv_t kv[16];
    int      n = hc_state_parse(hc_block_name(s), bb, sizeof bb, kv, 16);
    for (int i = 0; i < n; i++)
        if (strcmp(kv[i].k, "waterlogged") == 0) {
            snprintf(kv[i].v, sizeof kv[i].v, "true");
            return hc_state_build(bb, kv, n);
        }
    die("waterlogged property missing", hc_block_name(s));
    return s;
}

/* ================= 템플릿 로드 (캐시) ================= */

enum { TPL_CACHE_MAX = 512 };
static struct {
    const char          *name;
    const hc_template_t *tpl;
} g_tpl_cache[TPL_CACHE_MAX];
static int g_tpl_cache_n = 0;

static char *read_whole(hc_arena_t *a, const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = hc_arena_alloc(a, (size_t)sz + 1, 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz)
        die("template short read", path);
    fclose(f);
    buf[sz] = '\0';
    if (len)
        *len = (size_t)sz;
    return buf;
}

/* 팔레트 엔트리 {Name, Properties} → 캐노니컬 상태 id (미등재 die) */
static uint16_t resolve_palette_state(const hc_nbt_t *entry) {
    const hc_nbt_t *nm = hc_nbt_get(entry, "Name");
    const hc_nbt_t *props = hc_nbt_get(entry, "Properties");
    char            bb[128];
    hc_skv_t        kv[16];
    snprintf(bb, sizeof bb, "%s", hc_nbt_str(nm));
    int n = 0;
    if (props) {
        int32_t cnt = hc_nbt_comp_count(props);
        assert(cnt <= 16);
        for (int32_t i = 0; i < cnt; i++) {
            const char     *k = NULL;
            const hc_nbt_t *v = hc_nbt_comp_at(props, i, &k);
            snprintf(kv[n].k, sizeof kv[n].k, "%s", k);
            snprintf(kv[n].v, sizeof kv[n].v, "%s", hc_nbt_str(v));
            n++;
        }
    }
    return hc_state_build(bb, kv, n); /* 미등재 = die (fail-loud) */
}

/* (Y,X,Z) 비교 (ST:173-175) */
static int cmp_yxz(const void *pa, const void *pb) {
    const hc_tblock_t *a = pa, *b = pb;
    if (a->y != b->y)
        return a->y < b->y ? -1 : 1;
    if (a->x != b->x)
        return a->x < b->x ? -1 : 1;
    if (a->z != b->z)
        return a->z < b->z ? -1 : 1;
    return 0;
}

const hc_template_t *hc_template_load(hc_arena_t *a, const char *dir,
                                      const char *name) {
    for (int i = 0; i < g_tpl_cache_n; i++)
        if (strcmp(g_tpl_cache[i].name, name) == 0)
            return g_tpl_cache[i].tpl;
    const char *path_part = name;
    if (strncmp(name, "minecraft:", 10) == 0)
        path_part = name + 10;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.nbt", dir, path_part);
    size_t len = 0;
    char  *buf = read_whole(a, path, &len);
    if (!buf)
        die("template file missing", path);
    hc_nbt_t *root = hc_nbt_parse(a, (const uint8_t *)buf, len);
    if (!root)
        die("template parse failed", path);

    hc_template_t *t =
        hc_arena_alloc(a, sizeof *t, _Alignof(hc_template_t));
    if (!t)
        die("arena exhausted (template)", NULL);
    memset(t, 0, sizeof *t);
    {
        const hc_nbt_t *sz = hc_nbt_get(root, "size");
        assert(hc_nbt_list_count(sz) == 3);
        for (int i = 0; i < 3; i++)
            t->size[i] = (int32_t)hc_nbt_i64(hc_nbt_list_at(sz, i));
    }
    const hc_nbt_t *pals = hc_nbt_get(root, "palettes");
    const hc_nbt_t *pal_single = hc_nbt_get(root, "palette");
    int32_t         n_pal = pals ? hc_nbt_list_count(pals) : 1;
    const hc_nbt_t *blocks = hc_nbt_get(root, "blocks");
    int32_t         nb = hc_nbt_list_count(blocks);
    assert(n_pal >= 1 && n_pal <= HC_TPL_MAX_PALETTES);
    t->n_palettes = n_pal;
    t->n_blocks = nb;
    t->name = name;
    t->blocks = hc_arena_alloc(
        a, sizeof(hc_tblock_t) * (size_t)n_pal * (size_t)nb,
        _Alignof(hc_tblock_t));
    if (!t->blocks)
        die("arena exhausted (template blocks)", NULL);
    for (int32_t p = 0; p < n_pal; p++)
        t->pal_nbt[p] = pals ? hc_nbt_list_at(pals, p) : pal_single;

    /* 블록 공통 파트 (pos/state-index/nbt) — arena 상주 (lazy 해석용) */
    int32_t (*bpos)[3] = hc_arena_alloc(a, sizeof(int32_t[3]) * (size_t)nb,
                                        _Alignof(int32_t));
    int32_t *bstate =
        hc_arena_alloc(a, sizeof(int32_t) * (size_t)nb, _Alignof(int32_t));
    const hc_nbt_t **bnbt = hc_arena_alloc(
        a, sizeof(hc_nbt_t *) * (size_t)nb, _Alignof(hc_nbt_t *));
    if (!bpos || !bstate || !bnbt)
        die("arena exhausted (template raw)", NULL);
    for (int32_t i = 0; i < nb; i++) {
        const hc_nbt_t *bt = hc_nbt_list_at(blocks, i);
        const hc_nbt_t *pv = hc_nbt_get(bt, "pos");
        assert(hc_nbt_list_count(pv) == 3);
        for (int k = 0; k < 3; k++)
            bpos[i][k] = (int32_t)hc_nbt_i64(hc_nbt_list_at(pv, k));
        const hc_nbt_t *sv = hc_nbt_get(bt, "state");
        bstate[i] = sv ? (int32_t)hc_nbt_i64(sv) : 0;
        bnbt[i] = hc_nbt_get(bt, "nbt");
    }
    t->raw_pos = (const int32_t(*)[3])bpos;
    t->raw_state = bstate;
    t->raw_nbt = bnbt;

    if (g_tpl_cache_n >= TPL_CACHE_MAX)
        die("template cache overflow", NULL);
    g_tpl_cache[g_tpl_cache_n].name = name;
    g_tpl_cache[g_tpl_cache_n].tpl = t;
    g_tpl_cache_n++;
    return t;
}

/* 팔레트 p 해석 (최초 사용 시) — 3버킷 분류 (ST:153-166) 후 각각
 * (Y,X,Z) 정렬, full+other+BE 연결 (ST:168-184). 비선택 팔레트 (미사용
 * 우드 변형) 의 상태는 레지스트리에 없을 수 있어 여기서만 해석한다. */
static const hc_tblock_t *ensure_palette(const hc_template_t *tc,
                                         int32_t p) {
    hc_template_t *t = (hc_template_t *)tc;
    assert(p >= 0 && p < t->n_palettes);
    hc_tblock_t *dst = t->blocks + (size_t)p * (size_t)t->n_blocks;
    if (t->pal_resolved[p])
        return dst;
    int32_t  np = hc_nbt_list_count(t->pal_nbt[p]);
    uint16_t ids[256];
    assert(np <= 256);
    for (int32_t i = 0; i < np; i++)
        ids[i] = resolve_palette_state(hc_nbt_list_at(t->pal_nbt[p], i));
    enum { MAXB = 32768 };
    static hc_tblock_t full[MAXB], other[MAXB], bes[MAXB];
    int32_t            nf = 0, no = 0, ne = 0;
    assert(t->n_blocks <= MAXB);
    for (int32_t i = 0; i < t->n_blocks; i++) {
        assert(t->raw_state[i] >= 0 && t->raw_state[i] < np);
        hc_tblock_t e = {t->raw_pos[i][0], t->raw_pos[i][1],
                         t->raw_pos[i][2], ids[t->raw_state[i]],
                         t->raw_nbt[i]};
        if (e.nbt)
            bes[ne++] = e;
        else if (hc_block_collision_full(e.state))
            full[nf++] = e;
        else
            other[no++] = e;
    }
    qsort(full, (size_t)nf, sizeof full[0], cmp_yxz);
    qsort(other, (size_t)no, sizeof other[0], cmp_yxz);
    qsort(bes, (size_t)ne, sizeof bes[0], cmp_yxz);
    memcpy(dst, full, sizeof full[0] * (size_t)nf);
    memcpy(dst + nf, other, sizeof other[0] * (size_t)no);
    memcpy(dst + nf + no, bes, sizeof bes[0] * (size_t)ne);
    t->pal_resolved[p] = 1;
    return dst;
}

/* ================= final_state 해석 (BlockStateParser 부분 지정) =================
 *
 * 지정 안 된 프로퍼티는 디폴트 (jar 실측 final_state 전수: air/tuff_bricks/
 * waxed_copper_block/waxed_oxidized_copper(_grate/_cut)/tripwire/
 * waxed_copper_bulb[lit=true]). 디폴트 표는 필요한 블록만 — 미지 조합 die. */
static uint16_t resolve_final_state(const char *str) {
    int32_t direct = hc_block_by_name(str, (int32_t)strlen(str));
    if (direct >= 0)
        return (uint16_t)direct;
    char     bb[128];
    hc_skv_t kv[16];
    int      n = hc_state_parse(str, bb, sizeof bb, kv, 16);
    /* 디폴트 프로퍼티 병합 */
    struct {
        const char *base;
        const char *defs; /* "k=v,k=v" */
    } DEFS[] = {
        {"minecraft:waxed_copper_bulb", "lit=false,powered=false"},
        {"minecraft:waxed_exposed_copper_bulb", "lit=false,powered=false"},
        {"minecraft:waxed_weathered_copper_bulb",
         "lit=false,powered=false"},
        {"minecraft:waxed_oxidized_copper_bulb",
         "lit=false,powered=false"},
        {"minecraft:waxed_oxidized_copper_grate", "waterlogged=false"},
        {"minecraft:tripwire", "attached=false,disarmed=false,east=false,"
                               "north=false,powered=false,south=false,"
                               "west=false"},
    };
    for (size_t d = 0; d < sizeof DEFS / sizeof DEFS[0]; d++) {
        if (strcmp(bb, DEFS[d].base) != 0)
            continue;
        hc_skv_t    merged[16];
        int         m = 0;
        const char *p = DEFS[d].defs;
        while (*p) {
            const char *eq = strchr(p, '=');
            const char *end = strchr(eq, ',');
            if (!end)
                end = eq + strlen(eq);
            snprintf(merged[m].k, sizeof merged[m].k, "%.*s",
                     (int)(eq - p), p);
            snprintf(merged[m].v, sizeof merged[m].v, "%.*s",
                     (int)(end - eq - 1), eq + 1);
            m++;
            p = *end ? end + 1 : end;
        }
        for (int i = 0; i < n; i++) /* 지정값 덮기 */
            for (int j = 0; j < m; j++)
                if (strcmp(merged[j].k, kv[i].k) == 0)
                    snprintf(merged[j].v, sizeof merged[j].v, "%s",
                             kv[i].v);
        return hc_state_build(bb, merged, m);
    }
    die("final_state unresolvable", str);
    return 0;
}

/* ================= 배치 환경 ================= */

enum { HC_MAX_PROCESSED = 16384, HC_MAX_PLACED = 8192 };

typedef struct {
    int32_t         wx, wy, wz; /* 변환된 월드 좌표 */
    uint16_t        state;      /* 프로세싱 후 상태 (미러/회전 전) */
    const hc_nbt_t *nbt;        /* 템플릿 BE nbt */
    const char     *loot;       /* AppendLoot 산출 (nbt 표현 대체) */
    int64_t         loot_seed;
} pb_t;

typedef struct {
    hc_sctx_t        *sc;
    hc_feat_region_t *rg;
    feat_env_t        fe; /* edge 디스패치 (rg/reg) */
    hc_wgr_t         *rng;
    hc_sstart_t      *start;
    hc_spiece_t      *p;
    int32_t           bb[6]; /* settings BB (포탈은 encapsulate 확장) */
    int32_t           pvx, pvz; /* rotation pivot (shipwreck 4,15; 그 외 0) */
    int32_t           sea;
    int               flag_ks;     /* 본배치 flag 16비트 (jigsaw 18) */
    int               apply_water; /* APPLY_WATERLOGGING */
    int               known_shape;
    /* placeInWorld 산출 */
    pb_t   *processed;
    int32_t n_processed;
    int32_t placed[HC_MAX_PLACED][3];
    const pb_t *placed_pb[HC_MAX_PLACED];
    int32_t     n_placed;
    int32_t     mn[3], mx[3];
} tpl_env_t;

static int bb_inside(const int32_t bb[6], int32_t x, int32_t y, int32_t z) {
    return x >= bb[0] && x <= bb[3] && z >= bb[2] && z <= bb[5] &&
           y >= bb[1] && y <= bb[4];
}

/* setBlock — flag2 경로(hc_feat_set_block: 마킹 포함) / flag16 경로.
 * 성공 시 비-BE 상태로 덮인 pos 의 BE 레코드 제거 (ProtoChunk 대응). */
static int tpl_set(tpl_env_t *e, int32_t x, int32_t y, int32_t z, uint16_t s,
                   int ks) {
    int ok = ks ? hc_feat_set_block_ks(e->rg, x, y, z, s)
                : hc_feat_set_block(e->rg, x, y, z, s);
    if (ok && !is_entity_block(s))
        hc_be_remove(&e->sc->be, x, y, z);
    return ok;
}

/* getBoundingBox (ST:643-651): fromCorners(transform(0), transform(size-1))
 * .move(pos) */
static void template_bb(const hc_template_t *t, int mir, int rot,
                        int32_t pvx, int32_t pvz, int32_t tx, int32_t ty,
                        int32_t tz, int32_t out[6]) {
    int32_t x0 = 0, y0 = 0, z0 = 0;
    int32_t x1 = t->size[0] - 1, y1 = t->size[1] - 1, z1 = t->size[2] - 1;
    hc_template_transform(&x0, &y0, &z0, mir, rot, pvx, pvz);
    hc_template_transform(&x1, &y1, &z1, mir, rot, pvx, pvz);
    out[0] = (x0 < x1 ? x0 : x1) + tx;
    out[1] = (y0 < y1 ? y0 : y1) + ty;
    out[2] = (z0 < z1 ? z0 : z1) + tz;
    out[3] = (x0 > x1 ? x0 : x1) + tx;
    out[4] = (y0 > y1 ? y0 : y1) + ty;
    out[5] = (z0 > z1 ? z0 : z1) + tz;
}

/* BoundingBox.getCenter (BB:257-259) */
static void bb_center(const int32_t bb[6], int32_t c[3]) {
    c[0] = bb[0] + (bb[3] - bb[0] + 1) / 2;
    c[1] = bb[1] + (bb[4] - bb[1] + 1) / 2;
    c[2] = bb[2] + (bb[5] - bb[2] + 1) / 2;
}

/* ================= 프로세서 체인 ================= */

/* RandomBlockMatchTest: is(block) && nextFloat() < prob (매치시만 드로우) */
static int rule_random_match(hc_lcg_t *r, uint16_t s, const char *block,
                             float prob) {
    if (!base_is(s, block))
        return 0;
    return hc_lcg_next_float(r) < prob;
}

/* BlockAgeProcessor.getRandomFacingStairs — nextInt(4) facing[N,E,S,W] +
 * nextInt(2) half[top,bottom] (BAP:106-109; Half.values()=[TOP,BOTTOM]) */
static uint16_t age_random_stairs(hc_lcg_t *r, const char *base) {
    static const char *F[4] = {"north", "east", "south", "west"};
    const char        *f = F[hc_lcg_next_int(r, 4)];
    const char        *h = hc_lcg_next_int(r, 2) == 0 ? "top" : "bottom";
    char buf[160];
    snprintf(buf, sizeof buf,
             "%s[facing=%s,half=%s,shape=straight,waterlogged=false]", base,
             f, h);
    return id_of(buf);
}

/* BlockAgeProcessor.processBlock (BAP:37-104) — 위치시드 일회용 LCG.
 * 반환: 교체 상태 또는 s (드롭 없음). */
static uint16_t age_process(uint16_t s, int32_t wx, int32_t wy, int32_t wz,
                            float mossiness) {
    hc_lcg_t r;
    hc_lcg_init(&r, hc_mth_get_seed(wx, wy, wz));
    if (base_is(s, "minecraft:stone_bricks") ||
        base_is(s, "minecraft:stone") ||
        base_is(s, "minecraft:chiseled_stone_bricks")) {
        if (hc_lcg_next_float(&r) >= 0.5f)
            return s;
        uint16_t nm_stairs =
            age_random_stairs(&r, "minecraft:stone_brick_stairs");
        uint16_t m_stairs =
            age_random_stairs(&r, "minecraft:mossy_stone_brick_stairs");
        int      mossy = hc_lcg_next_float(&r) < mossiness;
        int      pick = hc_lcg_next_int(&r, 2);
        if (mossy)
            return pick == 0 ? g_mossy_sb : m_stairs;
        return pick == 0 ? g_cracked_sb : nm_stairs;
    }
    if (base_ends_with(s, "_stairs"))
        die("age processor stairs input unreachable in this region",
            hc_block_name(s));
    if (base_ends_with(s, "_slab")) {
        if (hc_lcg_next_float(&r) < mossiness) {
            /* MOSSY_STONE_BRICK_SLAB.withPropertiesOf — type/waterlogged
             * 복사 */
            char     bb[128];
            hc_skv_t kv[16];
            int n = hc_state_parse(hc_block_name(s), bb, sizeof bb, kv, 16);
            snprintf(bb, sizeof bb, "minecraft:mossy_stone_brick_slab");
            return hc_state_build(bb, kv, n);
        }
        return s;
    }
    if (base_ends_with(s, "_wall"))
        die("age processor wall input unreachable in this region",
            hc_block_name(s));
    if (base_is(s, "minecraft:obsidian"))
        return hc_lcg_next_float(&r) < 0.15f ? g_crying_obsidian : s;
    return s;
}

/* 포탈 RuleProcessor (R:141-172, on_ocean_floor / !cold 스냅샷) —
 * 룰 순서: gold(0.3→AIR), lava(→MAGMA 결정적), netherrack(0.07→MAGMA). */
static void portal_rules(pb_t *b) {
    hc_lcg_t r;
    hc_lcg_init(&r, hc_mth_get_seed(b->wx, b->wy, b->wz));
    if (rule_random_match(&r, b->state, "minecraft:gold_block", 0.3f)) {
        b->state = HC_B_AIR;
        return;
    }
    if (base_is(b->state, "minecraft:lava")) {
        b->state = g_magma; /* ON_OCEAN_FLOOR: BlockMatch(LAVA)→MAGMA */
        return;
    }
    if (rule_random_match(&r, b->state, "minecraft:netherrack", 0.07f)) {
        b->state = g_magma;
        return;
    }
}

/* copper bulb degradation (datapack JSON — reference/worldgen/
 * processor_list/trial_chambers_copper_bulb_degradation.json).
 * 출력 상태는 JSON 리터럴 그대로 [lit=true,powered=false]. */
static void copper_rules(pb_t *b) {
    if (!base_is(b->state, "minecraft:waxed_copper_bulb"))
        return;
    hc_lcg_t r;
    hc_lcg_init(&r, hc_mth_get_seed(b->wx, b->wy, b->wz));
    if (hc_lcg_next_float(&r) < 0.1f) {
        b->state = id_of("minecraft:waxed_oxidized_copper_bulb[lit=true,"
                         "powered=false]");
        return;
    }
    if (hc_lcg_next_float(&r) < 0.33333334f) {
        b->state = id_of("minecraft:waxed_weathered_copper_bulb[lit=true,"
                         "powered=false]");
        return;
    }
    if (hc_lcg_next_float(&r) < 0.5f) {
        b->state = id_of("minecraft:waxed_exposed_copper_bulb[lit=true,"
                         "powered=false]");
        return;
    }
}

/* 프로세서 체인 1블록 — 반환 0 = 탈락 */
static int chain_process(tpl_env_t *e, pb_t *b) {
    switch (e->p->procs) {
    case HC_PROCS_SHIPWRECK:
        /* [BlockIgnore STRUCTURE_AND_AIR] */
        if (b->state == HC_B_AIR || base_is(b->state, "minecraft:structure_block"))
            return 0;
        return 1;
    case HC_PROCS_OCEAN_RUIN: {
        /* [BlockRot(integrity), Ignore AIR+SB, Capped(passthrough)] */
        hc_lcg_t r;
        hc_lcg_init(&r, hc_mth_get_seed(b->wx, b->wy, b->wz));
        if (!(hc_lcg_next_float(&r) <= e->p->integrity))
            return 0; /* BRP:51-53 — <= 유지, > 탈락 */
        if (b->state == HC_B_AIR ||
            base_is(b->state, "minecraft:structure_block"))
            return 0;
        return 1;
    }
    case HC_PROCS_PORTAL: {
        /* [Ignore AIR+SB(air_pocket=0), Rule, Age, Protected, LavaSubm] */
        if (b->state == HC_B_AIR ||
            base_is(b->state, "minecraft:structure_block"))
            return 0;
        portal_rules(b);
        b->state = age_process(b->state, b->wx, b->wy, b->wz,
                               e->p->mossiness);
        uint16_t world = hc_feat_get_block(e->rg, b->wx, b->wy, b->wz);
        if (hc_featx_mask_test(e->sc->mask_features_cannot_replace, world))
            return 0; /* ProtectedBlockProcessor */
        if (world == HC_B_LAVA && !hc_block_collision_full(b->state)) {
            b->state = HC_B_LAVA; /* LavaSubmergedBlockProcessor */
            b->nbt = NULL;
        }
        return 1;
    }
    case HC_PROCS_JIGSAW_NONE:
    case HC_PROCS_JIGSAW_COPPER: {
        /* [Ignore STRUCTURE_BLOCK, JigsawReplacement, (copper), (protected)] */
        if (base_is(b->state, "minecraft:structure_block"))
            return 0;
        if (base_is(b->state, "minecraft:jigsaw")) {
            if (!b->nbt)
                return 1; /* JRP:38-40 — 경고 후 통과 */
            const hc_nbt_t *fs = hc_nbt_get(b->nbt, "final_state");
            const char *str = fs ? hc_nbt_str(fs) : "minecraft:air";
            if (strcmp(str, "minecraft:structure_void") == 0)
                return 0;
            b->state = resolve_final_state(str);
            b->nbt = NULL;
            /* 이어서 체인 계속 (copper 룰이 final_state 산출물을 볼 수
             * 있음 — copper bulb final_state 존재) */
        }
        if (e->p->procs == HC_PROCS_JIGSAW_COPPER) {
            copper_rules(b);
            uint16_t world = hc_feat_get_block(e->rg, b->wx, b->wy, b->wz);
            if (hc_featx_mask_test(e->sc->mask_features_cannot_replace,
                                   world))
                return 0; /* protected_blocks (datapack 리스트 2번째) */
        }
        return 1;
    }
    default:
        die("unknown processor preset", NULL);
        return 0;
    }
}

/* CappedProcessor.finalizeProcessing (CP:43-88) — ocean ruin 전용.
 * random = LegacySingleThreaded(worldSeed).forkPositional().at(tp). */
static void capped_finalize(tpl_env_t *e) {
    if (e->p->procs != HC_PROCS_OCEAN_RUIN)
        return;
    int32_t n = e->n_processed;
    if (n == 0)
        return;
    hc_lcg_t seedr;
    hc_lcg_init(&seedr, e->sc->seed);
    int64_t factory = hc_lcg_next_long(&seedr);
    hc_lcg_t r;
    hc_lcg_init(&r, hc_mth_get_seed(e->p->tpx, e->p->tpy, e->p->tpz) ^
                        factory);
    int32_t max_repl = 5 < n ? 5 : n; /* ConstantInt(5).sample — 0드로우 */
    /* Util.toShuffledList(0..n-1): Fisher-Yates 하강 (U:967-998) */
    static int32_t idx[HC_MAX_PROCESSED];
    for (int32_t i = 0; i < n; i++)
        idx[i] = i;
    for (int32_t i = n; i > 1; i--) {
        int32_t j = hc_lcg_next_int(&r, i);
        int32_t t = idx[i - 1];
        idx[i - 1] = idx[j];
        idx[j] = t;
    }
    int32_t replaced = 0;
    for (int32_t k = 0; k < n && replaced < max_repl; k++) {
        pb_t *b = &e->processed[idx[k]];
        /* delegate = Rule([BlockMatch(SAND) → SUSPICIOUS_SAND +
         * AppendLoot(ocean_ruin_warm archaeology)]) — WARM 스냅샷 */
        if (!base_is(b->state, "minecraft:sand"))
            continue; /* 매치 실패 = 동일 객체 → 캡 미집계, 드로우 0 */
        hc_lcg_t br;
        hc_lcg_init(&br, hc_mth_get_seed(b->wx, b->wy, b->wz));
        b->state = g_susp_sand;
        b->loot = "minecraft:archaeology/ocean_ruin_warm";
        b->loot_seed = hc_lcg_next_long(&br); /* AppendLoot */
        b->nbt = NULL;
        replaced++;
    }
}

/* processBlockInfos (ST:459-503) */
static void build_processed(tpl_env_t *e, const hc_tblock_t *blocks,
                            int32_t nb, pb_t *out) {
    int process_only =
        e->p->procs != HC_PROCS_OCEAN_RUIN; /* Capped 만 전피스 (ST:468) */
    e->n_processed = 0;
    for (int32_t i = 0; i < nb; i++) {
        int32_t x = blocks[i].x, y = blocks[i].y, z = blocks[i].z;
        hc_template_transform(&x, &y, &z, e->p->mir, e->p->rot, e->pvx,
                              e->pvz);
        x += e->p->tpx;
        y += e->p->tpy;
        z += e->p->tpz;
        if (process_only && !bb_inside(e->bb, x, y, z))
            continue;
        pb_t b = {x, y, z, blocks[i].state, blocks[i].nbt, NULL, 0};
        if (!chain_process(e, &b))
            continue;
        assert(e->n_processed < HC_MAX_PROCESSED);
        out[e->n_processed++] = b;
    }
    e->processed = out;
    capped_finalize(e);
}

/* ================= updateShapeAtEdge / fold ================= */

static const int8_t DIR_STEP[6][3] = {{0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                                      {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};

/* BitSetDiscreteVoxelShape.forAllFaces — Z 패스, Y 패스, X 패스 (R5 §3;
 * features_tree.c update_shape_at_edge 와 동일 순서) */
static void tpl_update_shape_at_edge(tpl_env_t *e) {
    int32_t sx = e->mx[0] - e->mn[0] + 1;
    int32_t sy = e->mx[1] - e->mn[1] + 1;
    int32_t sz = e->mx[2] - e->mn[2] + 1;
    enum { SMAX = 48 };
    if (sx > SMAX || sy > SMAX || sz > SMAX)
        die("edge shape too large", NULL);
    static uint8_t fillv[SMAX][SMAX][SMAX]; /* [y][x][z] */
    memset(fillv, 0, sizeof fillv);
    for (int32_t i = 0; i < e->n_placed; i++)
        fillv[e->placed[i][1] - e->mn[1]][e->placed[i][0] - e->mn[0]]
             [e->placed[i][2] - e->mn[2]] = 1;
#define FULLV(X, Y, Z)                                                        \
    ((X) >= 0 && (X) < sx && (Y) >= 0 && (Y) < sy && (Z) >= 0 &&              \
     (Z) < sz && fillv[Y][X][Z])
    /* Z 패스: rising→NORTH(2), falling→SOUTH(3) */
    for (int32_t x = 0; x < sx; x++)
        for (int32_t y = 0; y < sy; y++) {
            int prev = 0;
            for (int32_t z = 0; z <= sz; z++) {
                int cur = z != sz && FULLV(x, y, z);
                if (!prev && cur)
                    hc_featx_edge_face(&e->fe, e->mn[0] + x, e->mn[1] + y,
                                       e->mn[2] + z, 2);
                if (prev && !cur)
                    hc_featx_edge_face(&e->fe, e->mn[0] + x, e->mn[1] + y,
                                       e->mn[2] + z - 1, 3);
                prev = cur;
            }
        }
    /* Y 패스: rising→DOWN(0), falling→UP(1) */
    for (int32_t z = 0; z < sz; z++)
        for (int32_t x = 0; x < sx; x++) {
            int prev = 0;
            for (int32_t y = 0; y <= sy; y++) {
                int cur = y != sy && FULLV(x, y, z);
                if (!prev && cur)
                    hc_featx_edge_face(&e->fe, e->mn[0] + x, e->mn[1] + y,
                                       e->mn[2] + z, 0);
                if (prev && !cur)
                    hc_featx_edge_face(&e->fe, e->mn[0] + x,
                                       e->mn[1] + y - 1, e->mn[2] + z, 1);
                prev = cur;
            }
        }
    /* X 패스: rising→WEST(4), falling→EAST(5) */
    for (int32_t y = 0; y < sy; y++)
        for (int32_t z = 0; z < sz; z++) {
            int prev = 0;
            for (int32_t x = 0; x <= sx; x++) {
                int cur = x != sx && FULLV(x, y, z);
                if (!prev && cur)
                    hc_featx_edge_face(&e->fe, e->mn[0] + x, e->mn[1] + y,
                                       e->mn[2] + z, 4);
                if (prev && !cur)
                    hc_featx_edge_face(&e->fe, e->mn[0] + x - 1,
                                       e->mn[1] + y, e->mn[2] + z, 5);
                prev = cur;
            }
        }
#undef FULLV
}

/* Block.updateFromNeighbourShapes (Block:213-223) — W,E,N,S,D,U 접기;
 * 변화시 setBlock (updateMode & -2 | 16 = flag 16 경로) */
static void tpl_fold_neighbour_shapes(tpl_env_t *e) {
    static const int FOLD[6] = {4, 5, 2, 3, 0, 1};
    for (int32_t i = 0; i < e->n_placed; i++) {
        int32_t  x = e->placed[i][0], y = e->placed[i][1],
                z = e->placed[i][2];
        uint16_t s0 = hc_feat_get_block(e->rg, x, y, z);
        uint16_t cur = s0;
        for (int d = 0; d < 6; d++) {
            int      dir = FOLD[d];
            uint16_t ns = hc_feat_get_block(e->rg, x + DIR_STEP[dir][0],
                                            y + DIR_STEP[dir][1],
                                            z + DIR_STEP[dir][2]);
            hc_featx_edge_ticks(&e->fe, cur, x, y, z, dir, ns);
            cur = hc_featx_edge_state(&e->fe, cur, x, y, z, dir, ns);
        }
        if (cur != s0)
            tpl_set(e, x, y, z, cur, 1 /* flag 18 — 마킹 억제 */);
    }
}

/* ================= placeInWorld 본체 (ST:263-431) ================= */

static int place_in_world(tpl_env_t *e) {
    const hc_template_t *t = e->p->tmpl;
    if (t->n_palettes == 0 || t->size[0] < 1 || t->size[1] < 1 ||
        t->size[2] < 1)
        return 0;
    /* 팔레트 선택 — RandomSource.create(Mth.getSeed(position)) 일회용
     * (SPS:111-147); 팔레트 1개여도 드로우 */
    hc_lcg_t pr;
    hc_lcg_init(&pr, hc_mth_get_seed(e->p->tpx, e->p->tpy, e->p->tpz));
    int32_t pal = hc_lcg_next_int(&pr, t->n_palettes);
    if (t->n_blocks == 0)
        return 0;
    const hc_tblock_t *blocks = ensure_palette(t, pal);

    static pb_t processed[HC_MAX_PROCESSED];
    build_processed(e, blocks, t->n_blocks, processed);

    /* 워터로깅 리스트 */
    enum { WLMAX = 4096 };
    static int32_t locked[WLMAX][3], tofill[WLMAX][3];
    int32_t        n_locked = 0, n_tofill = 0;

    e->n_placed = 0;
    e->mn[0] = e->mn[1] = e->mn[2] = INT32_MAX;
    e->mx[0] = e->mx[1] = e->mx[2] = INT32_MIN;

    for (int32_t i = 0; i < e->n_processed; i++) {
        pb_t *b = &e->processed[i];
        if (!bb_inside(e->bb, b->wx, b->wy, b->wz))
            continue;
        int prev = e->apply_water
                       ? fluid_kind(hc_feat_get_block(e->rg, b->wx, b->wy,
                                                      b->wz))
                       : -1; /* -1 = 워터로깅 스킵 */
        uint16_t s = b->state;
        if (e->p->mir != HC_MIR_NONE)
            s = hc_state_mirror(s, e->p->mir);
        if (e->p->rot != HC_ROT_NONE)
            s = hc_state_rotate(s, e->p->rot);
        int has_nbt = b->nbt != NULL || b->loot != NULL;
        if (has_nbt) /* BARRIER 선배치 flag 820 (bit16 → ks 경로) */
            tpl_set(e, b->wx, b->wy, b->wz, g_barrier, 1);
        if (!tpl_set(e, b->wx, b->wy, b->wz, s, e->flag_ks))
            continue;
        if (b->wx < e->mn[0]) e->mn[0] = b->wx;
        if (b->wy < e->mn[1]) e->mn[1] = b->wy;
        if (b->wz < e->mn[2]) e->mn[2] = b->wz;
        if (b->wx > e->mx[0]) e->mx[0] = b->wx;
        if (b->wy > e->mx[1]) e->mx[1] = b->wy;
        if (b->wz > e->mx[2]) e->mx[2] = b->wz;
        assert(e->n_placed < HC_MAX_PLACED);
        e->placed[e->n_placed][0] = b->wx;
        e->placed[e->n_placed][1] = b->wy;
        e->placed[e->n_placed][2] = b->wz;
        e->placed_pb[e->n_placed] = b;
        e->n_placed++;
        if (is_entity_block(s)) {
            if (has_nbt) {
                int64_t inj = 0;
                if (is_randomizable(s))
                    inj = hc_wgr_next_long(e->rng); /* ST:313-315 */
                hc_be_rec_t *rec = hc_be_record(&e->sc->be, b->wx, b->wy,
                                                b->wz, HC_BE_TEMPLATE, s);
                rec->tpl_nbt = b->nbt;
                rec->loot = b->loot;
                rec->loot_seed = b->loot ? b->loot_seed : inj;
            } else {
                hc_be_record(&e->sc->be, b->wx, b->wy, b->wz, HC_BE_DUMMY,
                             s);
            }
        }
        if (prev >= 0) {
            uint16_t now = s; /* 재작성 추적 */
            if (fluid_kind(now) == PF_WATER_SRC ||
                fluid_kind(now) == PF_LAVA_SRC) {
                assert(n_locked < WLMAX);
                locked[n_locked][0] = b->wx;
                locked[n_locked][1] = b->wy;
                locked[n_locked][2] = b->wz;
                n_locked++;
            } else if (has_waterlogged_prop(now)) {
                /* placeLiquid (SimpleWaterloggedBlock:24-36) */
                if (!hc_block_is_waterlogged(now) && prev == PF_WATER_SRC) {
                    uint16_t wl = waterlogged_variant(now);
                    tpl_set(e, b->wx, b->wy, b->wz, wl, 0); /* flag 3 */
                    hc_feat_schedule_tick(e->rg, b->wx, b->wy, b->wz, 0,
                                          HC_TICK_WATER, 0);
                }
                if (prev != PF_WATER_SRC && prev != PF_LAVA_SRC) {
                    assert(n_tofill < WLMAX);
                    tofill[n_tofill][0] = b->wx;
                    tofill[n_tofill][1] = b->wy;
                    tofill[n_tofill][2] = b->wz;
                    n_tofill++;
                }
            }
        }
    }

    /* 유체 플러드필 (ST:337-365) — 방향 UP,N,E,S,W */
    static const int FLOOD[5] = {1, 2, 5, 3, 4};
    int              filled = 1;
    while (filled && n_tofill > 0) {
        filled = 0;
        int32_t w = 0;
        for (int32_t i = 0; i < n_tofill; i++) {
            int32_t px = tofill[i][0], py = tofill[i][1], pz = tofill[i][2];
            int     k = fluid_kind(hc_feat_get_block(e->rg, px, py, pz));
            int     src = k == PF_WATER_SRC || k == PF_LAVA_SRC;
            for (int d = 0; d < 5 && !src; d++) {
                int32_t nx = px + DIR_STEP[FLOOD[d]][0];
                int32_t ny = py + DIR_STEP[FLOOD[d]][1];
                int32_t nz = pz + DIR_STEP[FLOOD[d]][2];
                int nk = fluid_kind(hc_feat_get_block(e->rg, nx, ny, nz));
                if (nk != PF_WATER_SRC && nk != PF_LAVA_SRC)
                    continue;
                int lockedp = 0;
                for (int32_t l = 0; l < n_locked; l++)
                    if (locked[l][0] == nx && locked[l][1] == ny &&
                        locked[l][2] == nz) {
                        lockedp = 1;
                        break;
                    }
                if (!lockedp) {
                    src = 1;
                    k = nk;
                }
            }
            if (src) {
                uint16_t st = hc_feat_get_block(e->rg, px, py, pz);
                if (has_waterlogged_prop(st)) {
                    if (!hc_block_is_waterlogged(st) && k == PF_WATER_SRC) {
                        tpl_set(e, px, py, pz, waterlogged_variant(st), 0);
                        hc_feat_schedule_tick(e->rg, px, py, pz, 0,
                                              HC_TICK_WATER, 0);
                    }
                }
                filled = 1;
                /* iterator.remove() — 순서 보존 압축 */
            } else {
                tofill[w][0] = px;
                tofill[w][1] = py;
                tofill[w][2] = pz;
                w++;
            }
        }
        n_tofill = w;
    }

    if (e->mn[0] <= e->mx[0] && !e->known_shape) {
        tpl_update_shape_at_edge(e);
        tpl_fold_neighbour_shapes(e);
    }
    /* placeEntities: 이 리전 템플릿 전수 entities=0 (실측) — no-op */
    return 1;
}

/* ================= 마커/직소 패스 (TemplateStructurePiece) ================= */

typedef void (*marker_fn)(tpl_env_t *e, const char *meta, int32_t x,
                          int32_t y, int32_t z);

/* filterBlocks(tp, settings, 타입) — 선택 팔레트에서 타입 매치·BB 컬링,
 * (Y,X,Z) 버킷 순서 그대로 (ST:232-249) */
static void marker_pass(tpl_env_t *e, const char *type_base, int want_data,
                        marker_fn fn) {
    const hc_template_t *t = e->p->tmpl;
    hc_lcg_t             pr;
    hc_lcg_init(&pr, hc_mth_get_seed(e->p->tpx, e->p->tpy, e->p->tpz));
    int32_t            pal = hc_lcg_next_int(&pr, t->n_palettes);
    const hc_tblock_t *blocks = ensure_palette(t, pal);
    for (int32_t i = 0; i < t->n_blocks; i++) {
        const hc_tblock_t *b = &blocks[i];
        if (!base_is(b->state, type_base) || !b->nbt)
            continue;
        int32_t x = b->x, y = b->y, z = b->z;
        hc_template_transform(&x, &y, &z, e->p->mir, e->p->rot, e->pvx,
                              e->pvz);
        x += e->p->tpx;
        y += e->p->tpy;
        z += e->p->tpz;
        if (!bb_inside(e->bb, x, y, z))
            continue;
        if (want_data) {
            const hc_nbt_t *mode = hc_nbt_get(b->nbt, "mode");
            if (!mode || strcmp(hc_nbt_str(mode), "DATA") != 0)
                continue;
            const hc_nbt_t *meta = hc_nbt_get(b->nbt, "metadata");
            fn(e, meta ? hc_nbt_str(meta) : "", x, y, z);
        } else {
            /* 직소 패스: final_state 파싱 → setBlock flag 3 (T:102-115) */
            const hc_nbt_t *fs = hc_nbt_get(b->nbt, "final_state");
            uint16_t        st =
                resolve_final_state(fs ? hc_nbt_str(fs) : "minecraft:air");
            tpl_set(e, x, y, z, st, 0);
        }
    }
}

/* shipwreck 마커 (S:130-137): 아래 블록 BE 가 RandomizableContainer 면
 * setLootTable(loot, nextLong) */
static void shipwreck_marker(tpl_env_t *e, const char *meta, int32_t x,
                             int32_t y, int32_t z) {
    const char *loot = NULL;
    if (strcmp(meta, "map_chest") == 0)
        loot = "minecraft:chests/shipwreck_map";
    else if (strcmp(meta, "treasure_chest") == 0)
        loot = "minecraft:chests/shipwreck_treasure";
    else if (strcmp(meta, "supply_chest") == 0)
        loot = "minecraft:chests/shipwreck_supply";
    if (!loot)
        return;
    /* pos.below() 의 라이브 BE — 레코더에서 조회 (배치 순서 불변 갱신) */
    hc_be_recorder_t *rec = &e->sc->be;
    for (int32_t i = 0; i < rec->n; i++) {
        hc_be_rec_t *r = &rec->recs[i];
        if (r->dead || r->x != x || r->y != y - 1 || r->z != z)
            continue;
        if (!is_randomizable(r->state))
            return;
        r->loot = loot;
        r->loot_seed = hc_wgr_next_long(e->rng); /* RC:44-50 */
        return;
    }
}

/* ocean ruin 마커 (O:334-358) */
static void ruin_marker(tpl_env_t *e, const char *meta, int32_t x, int32_t y,
                        int32_t z) {
    if (strcmp(meta, "chest") == 0) {
        int wk = fluid_kind(hc_feat_get_block(e->rg, x, y, z));
        int wl = wk == PF_WATER_SRC || wk == PF_FLOWING;
        uint16_t st = wl ? g_chest_n_wl : g_chest_n;
        if (tpl_set(e, x, y, z, st, 0)) { /* flag 2 */
            /* getBlockEntity → 신선 ChestBlockEntity → setLootTable.
             * IsLarge=0 (골든) → small. */
            hc_be_rec_t *r = hc_be_record(&e->sc->be, x, y, z,
                                          HC_BE_CHEST_LOOT, st);
            r->loot = "minecraft:chests/underwater_ruin_small";
            r->loot_seed = hc_wgr_next_long(e->rng);
        }
        return;
    }
    if (strcmp(meta, "drowned") == 0) {
        /* 엔티티 생성/finalizeSpawn 은 엔티티 자체 랜덤 — 블록/공유
         * 스트림 무영향 (O:344-357). 저장물은 setBlock 만. */
        uint16_t st = y > e->sea ? HC_B_AIR : HC_B_WATER;
        tpl_set(e, x, y, z, st, 0);
        return;
    }
}

static void portal_marker(tpl_env_t *e, const char *meta, int32_t x,
                          int32_t y, int32_t z) {
    (void)e;
    (void)meta;
    (void)x;
    (void)y;
    (void)z; /* RuinedPortalPiece.handleDataMarker — no-op (R:203-206) */
}

/* TemplateStructurePiece.postProcess (T:80-117) 공통부 */
static void tsp_post_process(tpl_env_t *e, marker_fn fn) {
    /* BB 재계산 (T:91) — 골든 BB 와 일치 검증 (fail-loud) */
    int32_t nbb[6];
    template_bb(e->p->tmpl, e->p->mir, e->p->rot, e->pvx, e->pvz, e->p->tpx,
                e->p->tpy, e->p->tpz, nbb);
    if (memcmp(nbb, e->p->bb, sizeof nbb) != 0) {
        static char msg[256];
        snprintf(msg, sizeof msg,
                 "recomputed [%d,%d,%d,%d,%d,%d] golden [%d,%d,%d,%d,%d,%d]",
                 nbb[0], nbb[1], nbb[2], nbb[3], nbb[4], nbb[5], e->p->bb[0],
                 e->p->bb[1], e->p->bb[2], e->p->bb[3], e->p->bb[4],
                 e->p->bb[5]);
        die("template piece BB mismatch vs golden", msg);
    }
    if (!place_in_world(e))
        return;
    marker_pass(e, "minecraft:structure_block", 1, fn);
    marker_pass(e, "minecraft:jigsaw", 0, NULL);
}

/* ================= 피스별 postProcess ================= */

/* shipwreck (S:139-187) */
static void shipwreck_post(tpl_env_t *e) {
    hc_spiece_t *p = e->p;
    int too_big = p->tmpl->size[0] > 32 || p->tmpl->size[1] > 32;
    if (!p->height_adjusted && !too_big) {
        int32_t min_y = HC_MAX_Y + 1; /* level.getMaxY()+1 (S:149) */
        int32_t mean = 0;
        int     hm = p->is_beached ? HC_HM_WORLD_SURFACE_WG
                                   : HC_HM_OCEAN_FLOOR_WG;
        int32_t base = p->tmpl->size[0] * p->tmpl->size[2];
        if (base == 0)
            mean = hc_feat_height(e->rg, hm, p->tpx, p->tpz);
        else {
            /* betweenClosed(tp, tp+size-1): x 최속, z 최외 (단일 y) */
            for (int32_t z = p->tpz; z <= p->tpz + p->tmpl->size[2] - 1;
                 z++)
                for (int32_t x = p->tpx; x <= p->tpx + p->tmpl->size[0] - 1;
                     x++) {
                    int32_t h = hc_feat_height(e->rg, hm, x, z);
                    mean += h;
                    if (h < min_y)
                        min_y = h;
                }
            mean /= base;
        }
        int32_t newy =
            p->is_beached
                ? min_y - p->tmpl->size[1] / 2 - hc_wgr_next_int(e->rng, 3)
                : mean;
        p->height_adjusted = 1;
        if (newy != p->tpy) {
            static char msg[128];
            snprintf(msg, sizeof msg, "recomputed %d golden %d", newy,
                     p->tpy);
            die("shipwreck height adjust mismatch vs golden TPY", msg);
        }
        p->tpy = newy;
    }
    tsp_post_process(e, shipwreck_marker);
}

/* ocean ruin (O:360-415) — 래치 없음, 청크마다 재계산 */
static void ruin_post(tpl_env_t *e) {
    hc_spiece_t *p = e->p;
    int32_t      anchor =
        hc_feat_height(e->rg, HC_HM_OCEAN_FLOOR_WG, p->tpx, p->tpz);
    /* corner = transform((sx-1,0,sz-1), NONE, rot, ZERO) + (tpx,anchor,tpz) */
    int32_t cx = p->tmpl->size[0] - 1, cy = 0, cz = p->tmpl->size[2] - 1;
    hc_template_transform(&cx, &cy, &cz, HC_MIR_NONE, p->rot, 0, 0);
    cx += p->tpx;
    cz += p->tpz;
    /* getHeight (O:382-415) */
    int32_t newy = anchor;
    int32_t min_floor = 512;
    int32_t top_y = newy - 1;
    int32_t area = 0;
    int32_t x0 = p->tpx < cx ? p->tpx : cx, x1 = p->tpx < cx ? cx : p->tpx;
    int32_t z0 = p->tpz < cz ? p->tpz : cz, z1 = p->tpz < cz ? cz : p->tpz;
    for (int32_t z = z0; z <= z1; z++)
        for (int32_t x = x0; x <= x1; x++) {
            int32_t  fy = anchor - 1;
            uint16_t st = hc_feat_get_block(e->rg, x, fy, z);
            while ((hc_block_is_air(st) ||
                    fluid_kind(st) == PF_WATER_SRC ||
                    fluid_kind(st) == PF_FLOWING ||
                    base_is(st, "minecraft:ice") ||
                    base_is(st, "minecraft:packed_ice") ||
                    base_is(st, "minecraft:blue_ice") ||
                    base_is(st, "minecraft:frosted_ice")) &&
                   fy > HC_MIN_Y + 1) {
                fy--;
                st = hc_feat_get_block(e->rg, x, fy, z);
            }
            if (fy < min_floor)
                min_floor = fy;
            if (fy < top_y - 2)
                area++;
        }
    int32_t width = p->tpx - cx;
    if (width < 0)
        width = -width;
    if (top_y - min_floor > 2 && area > width - 2)
        newy = min_floor + 1;
    if (newy != p->tpy) {
        static char msg[128];
        snprintf(msg, sizeof msg, "recomputed %d golden %d", newy, p->tpy);
        die("ocean ruin reposition mismatch vs golden TPY", msg);
    }
    p->tpy = newy;
    tsp_post_process(e, ruin_marker);
}

/* ---------- ruined portal 후처리 (R:174-322) ---------- */

/* canBlockBeReplacedByNetherrackOrMagma (R:290-299) */
static int portal_can_replace(tpl_env_t *e, uint16_t st) {
    if (st == HC_B_AIR) /* 정확히 minecraft:air (cave_air 는 교체 가능) */
        return 0;
    if (base_is(st, "minecraft:obsidian"))
        return 0;
    if (hc_featx_mask_test(e->sc->mask_features_cannot_replace, st))
        return 0;
    if (st == HC_B_LAVA) /* vp != IN_NETHER */
        return 0;
    return 1;
}

/* placeNetherrackOrMagma (R:301-307) — !cold 만 nextFloat 1드로우 */
static void portal_place_nrm(tpl_env_t *e, int32_t x, int32_t y, int32_t z) {
    uint16_t s = g_netherrack;
    if (!e->p->cold && hc_wgr_next_float(e->rng) < 0.07f)
        s = g_magma;
    tpl_set(e, x, y, z, s, 0); /* flag 3 — magma 는 위 칸 자동 마킹 */
}

/* maybeAddLeavesAbove (R:223-227) — nextFloat 는 항상 드로우 */
static void portal_maybe_leaves(tpl_env_t *e, int32_t x, int32_t y,
                                int32_t z) {
    if (hc_wgr_next_float(e->rng) < 0.5f &&
        hc_feat_get_block(e->rg, x, y, z) == g_netherrack &&
        hc_block_is_air(hc_feat_get_block(e->rg, x, y + 1, z)))
        tpl_set(e, x, y + 1, z,
                id_of("minecraft:jungle_leaves[distance=7,persistent=true,"
                      "waterlogged=false]"),
                0); /* overgrown 전용 — 이 리전 미도달 (lazy id) */
}

/* addNetherrackDripColumn (R:240-250) */
static void portal_drip_column(tpl_env_t *e, int32_t x, int32_t y,
                               int32_t z) {
    portal_place_nrm(e, x, y, z);
    int cap = 8;
    while (cap > 0 && hc_wgr_next_float(e->rng) < 0.5f) {
        y--;
        cap--;
        portal_place_nrm(e, x, y, z);
    }
}

static void portal_post(tpl_env_t *e) {
    hc_spiece_t *p = e->p;
    if (p->overgrown || p->has_vines || p->replace_blackstone ||
        p->air_pocket || p->cold ||
        strcmp(p->vertical_placement, "on_ocean_floor") != 0)
        die("ruined portal properties outside region snapshot", NULL);
    /* 포탈 피벗 = (sizeX/2, 0, sizeZ/2) (R-placement §4.3) — 이 리전은
     * rot/mir NONE 이라 항등이지만, 스냅샷 밖 회전은 fail-loud. */
    if (p->rot != HC_ROT_NONE || p->mir != HC_MIR_NONE)
        die("ruined portal rotation outside region snapshot", NULL);
    int32_t lbb[6];
    template_bb(p->tmpl, p->mir, p->rot, e->pvx, e->pvz, p->tpx, p->tpy,
                p->tpz, lbb);
    int32_t c[3];
    bb_center(lbb, c);
    if (!bb_inside(e->bb, c[0], c[1], c[2]))
        return; /* 센터 미포함 청크 — 완전 no-op (R:184) */
    /* chunkBB.encapsulate(boundingBox) (R:185) */
    for (int k = 0; k < 3; k++) {
        if (lbb[k] < e->bb[k])
            e->bb[k] = lbb[k];
        if (lbb[k + 3] > e->bb[k + 3])
            e->bb[k + 3] = lbb[k + 3];
    }
    tsp_post_process(e, portal_marker);
    /* spreadNetherrack (R:252-288) — this.boundingBox = 재계산 BB = 골든 */
    const int32_t *bb = p->bb;
    int32_t        cc[3];
    bb_center(bb, cc);
    int32_t xspan = bb[3] - bb[0] + 1, zspan = bb[5] - bb[2] + 1;
    int32_t avg_w = (xspan + zspan) / 2;
    int32_t adj_bound = 8 - avg_w / 2;
    if (adj_bound < 1)
        adj_bound = 1;
    int32_t adj = hc_wgr_next_int(e->rng, adj_bound);
    static const float PROB[14] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                   0.9f, 0.9f, 0.8f, 0.7f, 0.6f, 0.4f, 0.2f};
    for (int32_t x = cc[0] - 14; x <= cc[0] + 14; x++)
        for (int32_t z = cc[2] - 14; z <= cc[2] + 14; z++) {
            int32_t dist = (x > cc[0] ? x - cc[0] : cc[0] - x) +
                           (z > cc[2] ? z - cc[2] : cc[2] - z);
            int32_t ad = dist + adj;
            if (ad < 0)
                ad = 0;
            if (ad >= 14)
                continue;
            if (!(hc_wgr_next_double(e->rng) < (double)PROB[ad]))
                continue;
            /* getSurfaceY: on_ocean_floor → OCEAN_FLOOR_WG - 1 */
            int32_t sy =
                hc_feat_height(e->rg, HC_HM_OCEAN_FLOOR_WG, x, z) - 1;
            int32_t y = sy; /* followGroundSurface (on_ocean_floor) */
            int32_t dy = y - bb[1];
            if (dy < 0)
                dy = -dy;
            if (dy > 3)
                continue;
            uint16_t st = hc_feat_get_block(e->rg, x, y, z);
            if (!portal_can_replace(e, st))
                continue;
            portal_place_nrm(e, x, y, z);
            if (e->p->overgrown) /* 이 리전 0 — 가드가 위에서 die */
                portal_maybe_leaves(e, x, y, z);
            portal_drip_column(e, x, y - 1, z);
        }
    /* addNetherrackDripColumnsBelowPortal (R:229-238) — 내부 전용 범위 */
    for (int32_t x = bb[0] + 1; x < bb[3]; x++)
        for (int32_t z = bb[2] + 1; z < bb[5]; z++) {
            if (hc_feat_get_block(e->rg, x, bb[1], z) == g_netherrack)
                portal_drip_column(e, x, bb[1] - 1, z);
        }
    /* vines/overgrown 패스 — 프로퍼티 0 (위 가드) */
}

/* jigsaw (PoolElementStructurePiece → SinglePoolElement.place) */
static void jigsaw_post(tpl_env_t *e) {
    /* getSettings: BB=chunkBB, rot, knownShape=1, liquid override,
     * placeInWorld flag 18 (SPE:152-165). 마커 pass 는 handleDataMarker
     * 디폴트 no-op — 공유 드로우 없음 (SPEbase:76-84) → 생략. */
    place_in_world(e);
}

/* ================= 진입점 ================= */

void hc_splace_template(hc_sctx_t *sc, hc_feat_region_t *rg,
                        const hc_feat_reg_t *freg, int32_t sea,
                        hc_sstart_t *start, hc_spiece_t *p, hc_wgr_t *rng,
                        int32_t cx, int32_t cz) {
    ids_init();
    tpl_env_t e;
    memset(&e, 0, sizeof e);
    e.sc = sc;
    e.rg = rg;
    e.rng = rng;
    e.start = start;
    e.p = p;
    e.sea = sea;
    e.fe.rg = rg;
    e.fe.reg = freg;
    /* getWritableArea (R-placement §1): 센터 청크 16x16, y [minY+1,maxY] */
    e.bb[0] = cx * 16;
    e.bb[1] = HC_MIN_Y + 1;
    e.bb[2] = cz * 16;
    e.bb[3] = cx * 16 + 15;
    e.bb[4] = HC_MAX_Y;
    e.bb[5] = cz * 16 + 15;
    if (p->kind == HC_SP_TEMPLATE_SHIPWRECK) {
        e.pvx = 4; /* ShipwreckPieces PIVOT (4,0,15) — S:33 */
        e.pvz = 15;
    } else if (p->kind == HC_SP_TEMPLATE_PORTAL) {
        e.pvx = p->tmpl->size[0] / 2;
        e.pvz = p->tmpl->size[2] / 2;
    } /* ocean ruin / jigsaw: BlockPos.ZERO (디폴트) */
    switch (p->kind) {
    case HC_SP_TEMPLATE_SHIPWRECK:
        e.flag_ks = 0; /* flag 2 */
        e.apply_water = 1;
        e.known_shape = 0;
        shipwreck_post(&e);
        break;
    case HC_SP_TEMPLATE_OCEAN_RUIN:
        e.flag_ks = 0;
        e.apply_water = 1;
        e.known_shape = 0;
        ruin_post(&e);
        break;
    case HC_SP_TEMPLATE_PORTAL:
        e.flag_ks = 0;
        e.apply_water = 1;
        e.known_shape = 0;
        portal_post(&e);
        break;
    case HC_SP_JIGSAW:
        e.flag_ks = 1; /* flag 18 */
        e.apply_water = !p->liquid_ignore;
        e.known_shape = 1;
        jigsaw_post(&e);
        break;
    default:
        die("unknown template piece kind", NULL);
    }
}
