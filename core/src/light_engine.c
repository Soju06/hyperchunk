/* 26.2 라이트 엔진 배치 솔버 (Task 10).
 *
 * 시맨틱 = server-26.2.jar 바이트코드 (.hermes/notes/task10-light/R1..R4):
 *  - 등록: 비-공기 섹션이 자신 + 26-이웃 섹션에 DataLayer 를 만든다
 *    (LayerLightSectionStorage.updateSectionStatus, R3 §1). 청크별
 *    topSections = 등록 최고 섹션 y + 1 (SkyDataLayerStorageMap, R3 §2.1).
 *  - sky 시딩: propagateLightSources 가 컬럼별 lowestSourceY(src_y) 위를
 *    15 로 직접 채우고, y==src_y(DOWN) / y<이웃컬럼 src_y(수평) 에서만
 *    확산 엔트리를 넣는다 (SkyLightEngine.propagateLightSources, R3 §3.1).
 *  - block 시딩: 활성 청크의 발광 블록 (findBlockLightSources) 을 emission
 *    엔트리로 넣는다. 자기 위치 self-write 규칙 포함 (R2 §3/§8).
 *  - 전파: 목적지 opacity = max(1, dampening), 증가 단조 → 유일 lfp
 *    (R2 §10). fresh solve 라 decrease 경로는 필요 없다.
 *  - dampening 유도 (R4 §1): solidRender→15, LeavesBlock→1, 유체 비어있지
 *    않음→1(PSD false), 그 외→0. 이 지역에 못 나오는 예외 블록은 die-list.
 *  - shapeOccludes: useShapeForLightOcclusion 상태가 팔레트에 없어 항상
 *    false (R4 §6) — 생략. 스노우 계열이 나타나면 die-list 가 잡는다.
 *
 * 전부 정수 연산 — FMA/부동소수점 무관. */

#include "hc_light.h"

#include "hc_blocks.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { QLOG = 22 }; /* 4M 엔트리 × 8B = 32MB — 관측 최대의 수십 배 여유 */
enum { CHECK_LOG = 17 }; /* checkBlock 펜딩 셋 128K — flush 주기당 수백 관측 */

/* --- per-id 라이트 테이블 (지연 구축) --- */

#define DAMP_DIE 0xFF

static uint8_t g_damp[HC_B_COUNT];
static uint8_t g_emit[HC_B_COUNT];
static int     g_tables_ready = 0;

static void build_tables(void) {
    for (int32_t id = 0; id < HC_B_COUNT; id++) {
        uint8_t d;
        if (hc_block_is_full_cube((uint16_t)id))
            d = 15; /* solidRender */
        else if (hc_block_is_leaves((uint16_t)id))
            d = 1; /* LeavesBlock.getLightDampening 오버라이드 */
        else if (hc_block_fluid_nonempty((uint16_t)id))
            d = 1; /* 유체/워터로그: propagatesSkylightDown false */
        else
            d = 0; /* 비폐색 식물/공기/카펫 등: PSD true */
        g_damp[id] = d;
        g_emit[id] = 0;
    }
    /* F_FULL/유도 규칙이 26.2 값과 어긋나는 (이 지역에 등장 불가한) 블록:
     * ice 는 noOcclusion(솔리드렌더 아님 — F_FULL 근사가 틀림),
     * powder_snow 는 비-풀 형상 특례. 조우 즉시 abort (fail-loud).
     * mud 는 Task 14 에서 등장 (trial chambers 아트리움 데코) — MudBlock
     * 은 collision/support/visual 만 오버라이드하고 outline/occlusion 은
     * 풀 큐브 (javap), canOcclude 기본 → solidRender → damp 15. */
    g_damp[HC_B_ICE] = DAMP_DIE;
    g_damp[HC_B_POWDER_SNOW] = DAMP_DIE;
    g_damp[HC_B_MUD] = 15;
    /* spawner: noOcclusion (solidRender 아님) 인데 getShape 오버라이드가
     * 없어 형상은 풀 큐브 → PSD false → dampening 1 (R4 §3/§5 — F_FULL
     * 유도가 못 잡는 유일 케이스) */
    g_damp[HC_B_SPAWNER] = 1;

    /* emission — Blocks.<clinit> (R4 §4). 팔레트에 없는 발광 블록은 0 유지
     * (redstone_ore lit=false → 0 은 유도 그대로). */
    g_emit[HC_B_LAVA] = 15;
    g_emit[HC_B_MAGMA_BLOCK] = 3;
    for (int i = 0; i < 126; i++) /* 배치 가능한 전 상태가 face>=1 → 7 */
        g_emit[HC_B_GLOW_LICHEN_BASE + i] = 7;
    for (int age = 0; age <= 25; age++) /* berries=true → 14 */
        g_emit[HC_B_CAVE_VINES_BASE + 26 + age] = 14;
    g_emit[HC_B_CAVE_VINES_PLANT_BASE + 1] = 14;
    /* amethyst 싹/클러스터: size 순 1/2/4/5 (facing/waterlogged 무관) */
    static const uint8_t AME_EM[4] = {1, 2, 4, 5};
    for (int size = 0; size < 4; size++)
        for (int i = 0; i < 12; i++)
            g_emit[HC_B_AMETHYST_BUD_BASE + size * 12 + i] = AME_EM[size];
    g_emit[HC_B_FIREFLY_BUSH] = 2; /* R4 §4 lambda$static$410 */

    /* Task 14 확장 블록: damp/emit 는 실측 테이블 (blocks.c T14_*,
     * 26.2 getLightDampening/LIGHT_EMISSION 바이트코드 핀 —
     * R-blockprops*.tsv). 파생 규칙과의 차이 (예: waterlogged 비폐색 = 1,
     * 스테인드글라스/그레이트 = 0) 가 전부 값에 반영돼 있다. */
    for (int32_t id = HC_B_T14_BASE; id < HC_B_COUNT; id++) {
        g_damp[id] = (uint8_t)hc_block_t14_light_damp((uint16_t)id);
        g_emit[id] = (uint8_t)hc_block_t14_light_emission((uint16_t)id);
    }

    g_tables_ready = 1;
}

static inline int damp_of(uint16_t id) {
    uint8_t d = g_damp[id];
    if (d == DAMP_DIE) {
        fprintf(stderr,
                "hc_light: block id %d (%s) reached the light engine but its "
                "26.2 dampening was audited as unreachable here\n",
                id, hc_block_name(id));
        abort();
    }
    return d;
}

/* --- 슬롯/상태 접근 --- */

static inline hc_light_chunk_t *slot(const hc_light_world_t *w, int32_t cx,
                                     int32_t cz) {
    int32_t dx = cx - w->cx0, dz = cz - w->cz0;
    if (dx < 0 || dx >= w->n || dz < 0 || dz >= w->n)
        return NULL;
    hc_light_chunk_t *s = &w->slots[dz * w->n + dx];
    return s->chunk ? s : NULL;
}

/* LightEngine.getState: getChunkForLighting 은 status>=FEATURES 만 돌려주고
 * 없으면 BEDROCK 기본 상태 (R2 §11). y 범위 밖은 AIR (ChunkAccess OOB). */
static inline uint16_t l_state(const hc_light_world_t *w, int32_t x, int32_t y,
                               int32_t z) {
    if (y < HC_MIN_Y || y > HC_MAX_Y)
        return HC_B_AIR;
    const hc_light_chunk_t *s = slot(w, x >> 4, z >> 4);
    if (!s || !s->feat_done)
        return HC_B_BEDROCK;
    return s->chunk->states[hc_idx(x & 15, y, z & 15)];
}

static inline int sec_registered(const hc_light_chunk_t *s, int32_t sec) {
    if (sec < HC_LIGHT_SEC_MIN || sec > HC_LIGHT_SEC_MAX)
        return 0;
    return (s->registered >> (sec - HC_LIGHT_SEC_MIN)) & 1u;
}

static inline size_t cell_idx(int32_t sec, int32_t x, int32_t y, int32_t z) {
    return (size_t)(sec - HC_LIGHT_SEC_MIN) * 4096 +
           (size_t)(((y & 15) << 8) | ((z & 15) << 4) | (x & 15));
}

static inline int l_stored_get(const hc_light_world_t *w, int layer, int32_t x,
                               int32_t y, int32_t z) {
    const hc_light_chunk_t *s = slot(w, x >> 4, z >> 4);
    assert(s);
    return s->light[layer][cell_idx(y >> 4, x, y, z)];
}

static inline void l_stored_set(hc_light_world_t *w, int layer, int32_t x,
                                int32_t y, int32_t z, int v) {
    hc_light_chunk_t *s = slot(w, x >> 4, z >> 4);
    assert(s);
    s->light[layer][cell_idx(y >> 4, x, y, z)] = (uint8_t)v;
}

/* 전파 목적지 게이트: storingLightForSection (R2 §5 @#62) */
static inline int l_stored_section(const hc_light_world_t *w, int32_t x,
                                   int32_t y, int32_t z) {
    const hc_light_chunk_t *s = slot(w, x >> 4, z >> 4);
    return s && sec_registered(s, y >> 4);
}

/* --- BFS 큐: 엔트리 = 한 uint64 ---
 * bits 0..12  x+4096, 13..25 z+4096, 26..35 y+512,
 * 36..39 fromLevel, 40..45 dir mask (D,U,N,S,W,E), 46 emission-seed */
enum { DIR_D = 0, DIR_U, DIR_N, DIR_S, DIR_W, DIR_E };
static const int8_t DIR_DX[6] = {0, 0, 0, 0, -1, 1};
static const int8_t DIR_DY[6] = {-1, 1, 0, 0, 0, 0};
static const int8_t DIR_DZ[6] = {0, 0, -1, 1, 0, 0}; /* N=-z, S=+z */
static const uint8_t DIR_OPP[6] = {DIR_U, DIR_D, DIR_S, DIR_N, DIR_E, DIR_W};
#define MASK_ALL 0x3Fu

typedef struct {
    hc_light_world_t *w;
    uint64_t          head, tail; /* head==tail 비어 있음 */
    uint64_t         *buf;        /* w->queue (증가) 또는 w->queue2 (감소) */
} bfs_t;

static inline void q_push(bfs_t *q, int32_t x, int32_t y, int32_t z, int level,
                          unsigned mask, int emission) {
    uint64_t cap = 1ull << q->w->qlog;
    if (q->tail - q->head >= cap) {
        fprintf(stderr, "hc_light: BFS queue overflow (raise QLOG)\n");
        abort();
    }
    uint64_t e = (uint64_t)(x + 4096) | ((uint64_t)(z + 4096) << 13) |
                 ((uint64_t)(y + 512) << 26) | ((uint64_t)level << 36) |
                 ((uint64_t)mask << 40) | ((uint64_t)(emission ? 1 : 0) << 46);
    q->buf[q->tail++ & (cap - 1)] = e;
}

/* R2 §11: 증가 전용 릴랙세이션. 방향 마스크/stale 드롭은 바닐라 그대로
 * (결과는 lfp 라 어차피 불변 — 엔트리 수 패리티만 위한 보존). */
static void bfs_run(bfs_t *q, int layer) {
    hc_light_world_t *w = q->w;
    uint64_t          cap = 1ull << w->qlog;
    while (q->head != q->tail) {
        uint64_t e = q->buf[q->head++ & (cap - 1)];
        int32_t  x = (int32_t)(e & 0x1FFF) - 4096;
        int32_t  z = (int32_t)((e >> 13) & 0x1FFF) - 4096;
        int32_t  y = (int32_t)((e >> 26) & 0x3FF) - 512;
        int      from = (int)((e >> 36) & 15);
        unsigned mask = (unsigned)((e >> 40) & MASK_ALL);
        int      emission = (int)((e >> 46) & 1);

        int lvl = l_stored_get(w, layer, x, y, z);
        if (emission && lvl < from) { /* self-write (R2 §3 @#48-66) */
            l_stored_set(w, layer, x, y, z, from);
            lvl = from;
        }
        if (lvl != from)
            continue; /* stale */

        /* Task 14: shapeOccludes (LightEngine.java:82-86) — USO 상태
         * (슬랩/스테어) 의 면 슬라이스 합집합이 풀 페이스면 차단.
         * faceShapeOccludes(from.face(d), to.face(opp(d))); 마스크는
         * 월드축 쿼드런트 (hc_block_face_occlusion — 대향면 동일 프레임,
         * R-blockprops-evidence §4/§5). 완전 폐색 큐브는 damp 15 경로. */
        uint32_t focc = hc_block_face_occlusion(l_state(w, x, y, z));

        for (int d = 0; d < 6; d++) {
            if (!(mask & (1u << d)))
                continue;
            int32_t nx = x + DIR_DX[d], ny = y + DIR_DY[d], nz = z + DIR_DZ[d];
            if (!l_stored_section(w, nx, ny, nz))
                continue; /* 등록 밖 = 벽 */
            int nl = l_stored_get(w, layer, nx, ny, nz);
            if (from - 1 <= nl)
                continue;
            uint16_t nst = l_state(w, nx, ny, nz);
            uint32_t nocc = hc_block_face_occlusion(nst);
            if (focc | nocc) {
                unsigned fm = (focc >> (4 * d)) & 0xFu;
                unsigned tm = (nocc >> (4 * DIR_OPP[d])) & 0xFu;
                if ((fm | tm) == 0xFu)
                    continue; /* shapeOccludes */
            }
            int op = damp_of(nst);
            int newl = from - (op < 1 ? 1 : op);
            if (newl <= nl)
                continue;
            l_stored_set(w, layer, nx, ny, nz, newl);
            if (newl > 1)
                q_push(q, nx, ny, nz, newl, MASK_ALL & ~(1u << DIR_OPP[d]), 0);
        }
    }
}

/* --- 공개 API --- */

int hc_light_world_init(hc_light_world_t *w, hc_arena_t *a, int32_t cx0,
                        int32_t cz0, int32_t n) {
    if (!g_tables_ready)
        build_tables();
    w->cx0 = cx0;
    w->cz0 = cz0;
    w->n = n;
    w->qlog = QLOG;
    w->slots = hc_arena_alloc(a, sizeof(hc_light_chunk_t) * (size_t)(n * n),
                              _Alignof(hc_light_chunk_t));
    w->queue = hc_arena_alloc(a, sizeof(uint64_t) << QLOG, 8);
    w->queue2 = hc_arena_alloc(a, sizeof(uint64_t) << QLOG, 8);
    w->check = hc_arena_alloc(a, sizeof(uint64_t) << CHECK_LOG, 8);
    w->dirty = hc_arena_alloc(a, sizeof(int32_t) * (size_t)(n * n), 4);
    w->n_check = 0;
    w->n_dirty = 0;
    if (!w->slots || !w->queue || !w->queue2 || !w->check || !w->dirty)
        return -1;
    memset(w->slots, 0, sizeof(hc_light_chunk_t) * (size_t)(n * n));
    return 0;
}

int hc_light_attach(hc_light_world_t *w, hc_arena_t *a, hc_chunk_t *c) {
    int32_t dx = c->cx - w->cx0, dz = c->cz - w->cz0;
    if (dx < 0 || dx >= w->n || dz < 0 || dz >= w->n) {
        fprintf(stderr, "hc_light_attach: chunk (%d,%d) outside world\n",
                c->cx, c->cz);
        abort();
    }
    hc_light_chunk_t *s = &w->slots[dz * w->n + dx];
    s->chunk = c;
    for (int l = 0; l < 2; l++) {
        s->light[l] = hc_arena_alloc(a, (size_t)HC_LIGHT_NSEC * 4096, 1);
        if (!s->light[l])
            return -1;
    }
    s->top = HC_LIGHT_NO_TOP;
    /* featured-이지만-08-전 이웃이 nb_src 로 읽힌다 (누적 모드) —
     * fillFrom 전의 ChunkSkyLightSources 는 전-열림 센티널 */
    for (int i = 0; i < 256; i++)
        s->src_y[i] = HC_LIGHT_OPEN;
    return 0;
}

void hc_light_reset(hc_light_world_t *w) {
    for (int32_t i = 0; i < w->n * w->n; i++) {
        hc_light_chunk_t *s = &w->slots[i];
        if (!s->chunk)
            continue;
        memset(s->light[0], 0, (size_t)HC_LIGHT_NSEC * 4096);
        memset(s->light[1], 0, (size_t)HC_LIGHT_NSEC * 4096);
        s->registered = 0;
        s->top = HC_LIGHT_NO_TOP;
        s->feat_done = 0;
        s->in_r = 0;
        s->enabled = 0;
        s->seeded = 0;
        for (int c = 0; c < 256; c++)
            s->src_y[c] = HC_LIGHT_OPEN;
    }
}

static hc_light_chunk_t *slot_must(hc_light_world_t *w, int32_t cx,
                                   int32_t cz) {
    hc_light_chunk_t *s = slot(w, cx, cz);
    if (!s) {
        fprintf(stderr, "hc_light: chunk (%d,%d) not attached\n", cx, cz);
        abort();
    }
    return s;
}

void hc_light_set_featured(hc_light_world_t *w, int32_t cx, int32_t cz) {
    slot_must(w, cx, cz)->feat_done = 1;
}
void hc_light_register(hc_light_world_t *w, int32_t cx, int32_t cz) {
    hc_light_chunk_t *s = slot_must(w, cx, cz);
    if (!s->feat_done) {
        fprintf(stderr, "hc_light: register before featured (%d,%d)\n", cx,
                cz);
        abort();
    }
    s->in_r = 1;
}
void hc_light_enable(hc_light_world_t *w, int32_t cx, int32_t cz) {
    hc_light_chunk_t *s = slot_must(w, cx, cz);
    if (!s->in_r) {
        fprintf(stderr, "hc_light: enable before register (%d,%d)\n", cx, cz);
        abort();
    }
    s->enabled = 1;
}

/* 섹션 sec (월드 섹션 y) 이 비-공기 블록을 갖는가 */
static int section_has_data(const hc_chunk_t *c, int32_t sec) {
    int32_t y0 = sec * 16;
    const uint16_t *base = c->states + hc_idx(0, y0, 0);
    for (int i = 0; i < 4096; i++)
        if (base[i] != HC_B_AIR)
            return 1;
    return 0;
}

/* isEdgeOccluded(above, below) — R3 §4.2: below dampening != 0 이거나
 * above.face(DOWN) ∪ below.face(UP) 이 풀 페이스 (USO 슬랩/스테어). */
static inline int edge_occluded(uint16_t above, uint16_t below) {
    if (damp_of(below) != 0)
        return 1;
    uint32_t tocc = hc_block_face_occlusion(above);
    uint32_t bocc = hc_block_face_occlusion(below);
    if (tocc | bocc) {
        unsigned tm = (tocc >> (4 * DIR_D)) & 0xFu;
        unsigned bm = (bocc >> (4 * DIR_U)) & 0xFu;
        if ((tm | bm) == 0xFu)
            return 1;
    }
    return 0;
}

/* ChunkSkyLightSources.fillFrom + findLowestSourceY (R3 §4.3 + Task 14):
 * 위에서 내려오며 isEdgeOccluded(top, bottom) — bottom dampening != 0
 * 이거나 top.face(DOWN) ∪ bottom.face(UP) 이 풀 페이스 (USO 슬랩/스테어,
 * ChunkSkyLightSources.java:143-147) — 인 첫 페어에서 멈춘다. floor =
 * top 의 y (= bottom 위). 없으면 열린 컬럼 센티널. */
static void fill_src_y(hc_light_chunk_t *s) {
    const hc_chunk_t *c = s->chunk;
    /* 섹션별 비-공기 마스크로 전부-공기 구간을 통째로 건너뛴다
     * (vanilla 의 hasOnlyAir 섹션 스킵과 등가; top 상태는 AIR 로 리셋) */
    uint32_t nonair = 0;
    for (int32_t sec = -4; sec <= 19; sec++)
        if (section_has_data(c, sec))
            nonair |= 1u << (sec + 4);
    for (int z = 0; z < 16; z++) {
        for (int x = 0; x < 16; x++) {
            int32_t  floor_y = HC_LIGHT_OPEN;
            uint16_t top_st = HC_B_AIR;
            for (int32_t sec = 19; sec >= -4; sec--) {
                if (!(nonair & (1u << (sec + 4)))) {
                    top_st = HC_B_AIR;
                    continue;
                }
                for (int32_t y = sec * 16 + 15; y >= sec * 16; y--) {
                    uint16_t st = c->states[hc_idx(x, y, z)];
                    if (edge_occluded(top_st, st)) {
                        floor_y = y + 1;
                        goto done;
                    }
                    top_st = st;
                }
            }
        done:
            s->src_y[hc_col_idx(x, z)] = floor_y;
        }
    }
}

/* 이웃 컬럼 lowestSourceY — getChunkForLighting 규칙: features 미만이면
 * emptyChunkSources (전부 열린 센티널) → 수평 시드가 안 생긴다 (R3 §5.1) */
static inline int32_t nb_src(const hc_light_world_t *w, int32_t cx, int32_t cz,
                             int x, int z) {
    const hc_light_chunk_t *s = slot(w, cx, cz);
    if (!s || !s->feat_done)
        return HC_LIGHT_OPEN;
    return s->src_y[hc_col_idx(x, z)];
}

/* phase A 본체: in_r 청크 하나의 등록 지오메트리 재유도 (단조 — 마스크는
 * 자라기만 한다; 누적 모드에서 updateSectionStatus 진화와 등가).
 *
 * 신규 비트가 기존 등록 위쪽 슬라이스가 nonzero 인 채로 아래에 생기면
 * 바닐라는 repeatFirstLayer 로 그 슬라이스를 복사한다 — 이 지역은 모든
 * 등록 범위가 바닥-고정 ([-5..top+1], 지반이 y=-4 부터 연속) 이라 신규
 * 비트는 항상 상단 확장 = 제로 생성. 전제가 깨지면 die (미구현 관측). */
static void derive_geometry_chunk(hc_light_world_t *w, hc_light_chunk_t *s) {
    int32_t cx = s->chunk->cx, cz = s->chunk->cz;
    for (int32_t sec = -4; sec <= 19; sec++) {
        if (!section_has_data(s->chunk, sec))
            continue;
        for (int dz = -1; dz <= 1; dz++) {
            for (int dx = -1; dx <= 1; dx++) {
                hc_light_chunk_t *t = slot(w, cx + dx, cz + dz);
                if (!t) {
                    fprintf(stderr,
                            "hc_light: registration of (%d,%d) spills "
                            "outside the attached world\n",
                            cx, cz);
                    abort();
                }
                for (int dy = -1; dy <= 1; dy++) {
                    int32_t  ns = sec + dy;
                    uint32_t bit = 1u << (ns - HC_LIGHT_SEC_MIN);
                    if (!(t->registered & bit)) {
                        uint32_t above =
                            t->registered >> (ns - HC_LIGHT_SEC_MIN + 1);
                        if (above) {
                            int32_t asec = ns + 1 + __builtin_ctz(above);
                            const uint8_t *sl =
                                t->light[HC_LIGHT_SKY] +
                                (size_t)(asec - HC_LIGHT_SEC_MIN) * 4096;
                            for (int i = 0; i < 256; i++)
                                if (sl[i]) {
                                    fprintf(stderr,
                                            "hc_light: below-top section "
                                            "creation with nonzero source "
                                            "slice ((%d,%d) sec %d) — "
                                            "repeatFirstLayer unimplemented\n",
                                            cx + dx, cz + dz, ns);
                                    abort();
                                }
                        }
                        t->registered |= bit;
                    }
                    if (ns + 1 > t->top || t->top == HC_LIGHT_NO_TOP)
                        t->top = ns + 1;
                }
            }
        }
    }
}

/* 갭 없는 컬럼 등록 확인 — propagateFromEmptySections(R3 §3.5) 를
 * 미구현으로 남기는 전제. 깨지면 즉사. */
static void check_contiguous(const hc_light_world_t *w) {
    for (int32_t i = 0; i < w->n * w->n; i++) {
        hc_light_chunk_t *s = &w->slots[i];
        if (!s->chunk || !s->registered)
            continue;
        uint32_t m = s->registered;
        uint32_t norm = m >> __builtin_ctz(m);
        if (norm & (norm + 1)) {
            fprintf(stderr,
                    "hc_light: non-contiguous section registration in "
                    "(%d,%d) mask %08x — propagateFromEmptySections needed\n",
                    s->chunk->cx, s->chunk->cz, m);
            abort();
        }
    }
}

/* phase B 본체: 활성 청크 하나의 sky 직접 채움 + 확산 시드 */
static void seed_sky_chunk(hc_light_world_t *w, bfs_t *q,
                           hc_light_chunk_t *s) {
    int32_t cx = s->chunk->cx, cz = s->chunk->cz;
    assert(s->top != HC_LIGHT_NO_TOP);
    for (int32_t sec = s->top - 1; sec >= HC_LIGHT_SEC_MIN; sec--) {
        if (!sec_registered(s, sec))
            continue;
        int32_t y0 = sec * 16, y15 = y0 + 15;
        int     any_lower = 0;
        for (int z = 0; z < 16; z++) {
            for (int x = 0; x < 16; x++) {
                int32_t low = s->src_y[hc_col_idx(x, z)];
                if (low > y15)
                    continue; /* 이 컬럼의 소스는 전부 위쪽 */
                int32_t lowN = (z == 0) ? nb_src(w, cx, cz - 1, x, 15)
                                        : s->src_y[hc_col_idx(x, z - 1)];
                int32_t lowS = (z == 15) ? nb_src(w, cx, cz + 1, x, 0)
                                         : s->src_y[hc_col_idx(x, z + 1)];
                int32_t lowW = (x == 0) ? nb_src(w, cx - 1, cz, 15, z)
                                        : s->src_y[hc_col_idx(x - 1, z)];
                int32_t lowE = (x == 15) ? nb_src(w, cx + 1, cz, 0, z)
                                         : s->src_y[hc_col_idx(x + 1, z)];
                int32_t ymin = low > y0 ? low : y0;
                for (int32_t y = y15; y >= ymin; y--) {
                    s->light[HC_LIGHT_SKY][cell_idx(sec, x, y, z)] = 15;
                    unsigned mask = 0;
                    if (y == low)
                        mask |= 1u << DIR_D;
                    if (y < lowN)
                        mask |= 1u << DIR_N;
                    if (y < lowS)
                        mask |= 1u << DIR_S;
                    if (y < lowW)
                        mask |= 1u << DIR_W;
                    if (y < lowE)
                        mask |= 1u << DIR_E;
                    if (mask)
                        q_push(q, cx * 16 + x, y, cz * 16 + z, 15, mask, 0);
                }
                if (low < y0)
                    any_lower = 1;
            }
        }
        if (!any_lower)
            break;
    }
}

/* phase D 본체: 활성 청크 하나의 발광 블록 시드 (findBlockLightSources
 * 의 섹션 오름차순/y,z,x 스캔 순서 — 큐 순서만 다르고 lfp 동일) */
static void seed_block_chunk(hc_light_world_t *w, bfs_t *q,
                             hc_light_chunk_t *s) {
    int32_t cx = s->chunk->cx, cz = s->chunk->cz;
    (void)w;
    for (int32_t y = HC_MIN_Y; y <= HC_MAX_Y; y++) {
        for (int z = 0; z < 16; z++) {
            for (int x = 0; x < 16; x++) {
                uint16_t st = s->chunk->states[hc_idx(x, y, z)];
                if (g_emit[st] == 0)
                    continue;
                q_push(q, cx * 16 + x, y, cz * 16 + z, g_emit[st], MASK_ALL,
                       1);
            }
        }
    }
}

void hc_light_solve(hc_light_world_t *w) {
    /* --- phase A: 등록 지오메트리 (현재 블록에서 재유도) --- */
    for (int32_t i = 0; i < w->n * w->n; i++) {
        hc_light_chunk_t *s = &w->slots[i];
        if (s->chunk && s->in_r)
            derive_geometry_chunk(w, s);
    }
    check_contiguous(w);

    /* --- phase A2: src_y (featured 청크 전부 — 이웃 시드 테이블로 쓰임) --- */
    for (int32_t i = 0; i < w->n * w->n; i++) {
        hc_light_chunk_t *s = &w->slots[i];
        if (s->chunk && s->feat_done)
            fill_src_y(s);
    }

    bfs_t q = {w, 0, 0, w->queue};

    /* --- phase B: sky (활성 청크; 순서는 lfp 라 무관) --- */
    for (int32_t i = 0; i < w->n * w->n; i++) {
        hc_light_chunk_t *s = &w->slots[i];
        if (s->chunk && s->enabled)
            seed_sky_chunk(w, &q, s);
    }
    bfs_run(&q, HC_LIGHT_SKY);

    /* --- phase D: block 시드 --- */
    q.head = q.tail = 0;
    for (int32_t i = 0; i < w->n * w->n; i++) {
        hc_light_chunk_t *s = &w->slots[i];
        if (s->chunk && s->enabled)
            seed_block_chunk(w, &q, s);
    }
    bfs_run(&q, HC_LIGHT_BLOCK);
}

/* --- 누적 (increase-only 이력) 모드 — hc_light.h 헤더 주석 참조 --- */

void hc_light_accum_prepare(hc_light_world_t *w) {
    /* 지오메트리만 — src_y 는 각 청크 08 시점에 동결 (init_chunk).
     * 실측 근거: c.2.26 — 3×3 이웃 데코 전부 09 완료 전인데 골든이
     * 나중-배치 캐노피 아래 sky 15 = 09 직접 채움이 08 시점의 열린
     * 컬럼(fillFrom)을 그대로 썼다. ProtoChunk 쓰기는 updateSectionStatus
     * 만 갱신하고 skyLightSources 는 갱신하지 않는 클래스. */
    for (int32_t i = 0; i < w->n * w->n; i++) {
        hc_light_chunk_t *s = &w->slots[i];
        if (s->chunk && s->in_r)
            derive_geometry_chunk(w, s);
    }
    check_contiguous(w);
}

void hc_light_accum_init_chunk(hc_light_world_t *w, int32_t cx, int32_t cz) {
    hc_light_chunk_t *s = slot_must(w, cx, cz);
    if (!s->feat_done) {
        fprintf(stderr, "hc_light: accum 08 before featured (%d,%d)\n", cx,
                cz);
        abort();
    }
    if (s->in_r) {
        fprintf(stderr, "hc_light: accum repeated 08 (%d,%d)\n", cx, cz);
        abort();
    }
    s->in_r = 1;
    derive_geometry_chunk(w, s);
    check_contiguous(w);
    fill_src_y(s); /* fillFrom — 이 시점 블록으로 동결 */
}

void hc_light_accum_light_chunk(hc_light_world_t *w, int32_t cx, int32_t cz) {
    hc_light_chunk_t *s = slot_must(w, cx, cz);
    if (!s->in_r) {
        fprintf(stderr, "hc_light: accum light before register (%d,%d)\n", cx,
                cz);
        abort();
    }
    if (s->seeded) {
        fprintf(stderr, "hc_light: accum re-seed of (%d,%d)\n", cx, cz);
        abort();
    }
    s->enabled = 1;
    s->seeded = 1;
    bfs_t q = {w, 0, 0, w->queue};
    seed_sky_chunk(w, &q, s);
    bfs_run(&q, HC_LIGHT_SKY);
    q.head = q.tail = 0;
    seed_block_chunk(w, &q, s);
    bfs_run(&q, HC_LIGHT_BLOCK);
}

/* --- 라이브-창 증분 재조명 (checkBlock 기계, Task 14) ---
 * hc_light.h 헤더 주석 + R2 §3/§4/§6, R3 §3.2/§3.4/§4.4 참조. */

/* hasDifferentLightProperties (R2 §9): dampening/emission/USO. USO 플래그
 * 근사 = 면 폐색 마스크 비-영 (마스크 전무 USO 상태는 팔레트에 없음). */
static inline int light_props_differ(uint16_t a, uint16_t b) {
    return g_damp[a] != g_damp[b] || g_emit[a] != g_emit[b] ||
           (hc_block_face_occlusion(a) != 0) !=
               (hc_block_face_occlusion(b) != 0);
}

/* ChunkSkyLightSources.findLowestSourceBelow (R3 §4.4): yb 에서 아래로
 * (above,below) 페어 워크, 첫 폐색에서 above 의 y 반환; 없으면 센티널. */
static int32_t find_lowest_source_below(const hc_light_world_t *w, int32_t x,
                                        int32_t yb, int32_t z) {
    uint16_t above = l_state(w, x, yb, z);
    for (int32_t y = yb; y >= HC_MIN_Y; y--) {
        uint16_t below = l_state(w, x, y - 1, z);
        if (edge_occluded(above, below))
            return y;
        above = below;
    }
    return HC_LIGHT_OPEN;
}

/* ChunkSkyLightSources.updateEdge (R3 §4.4) — 페어 (ya, ya-1) */
static int src_update_edge(hc_light_world_t *w, int32_t *cell, int32_t cur,
                           int32_t x, int32_t ya, int32_t z) {
    uint16_t sa = l_state(w, x, ya, z);
    uint16_t sb = l_state(w, x, ya - 1, z);
    if (edge_occluded(sa, sb)) {
        if (ya > cur) {
            *cell = ya;
            return 1;
        }
    } else if (ya == cur) {
        *cell = find_lowest_source_below(w, x, ya - 1, z);
        return 1;
    }
    return 0;
}

/* ChunkSkyLightSources.update (R3 §4.4) — 쓰기 (x,y,z) 후 (새 블록 반영됨)
 * 컬럼 소스 바닥 증분 갱신. 동결 창에 놓친 이벤트는 재스캔되지 않는다는
 * 이력 의존성이 핵심 — fillFrom 재실행으로 대체 불가. */
static void src_y_update(hc_light_world_t *w, hc_light_chunk_t *s, int32_t x,
                         int32_t y, int32_t z) {
    int32_t *cell = &s->src_y[hc_col_idx(x & 15, z & 15)];
    int32_t  cur = *cell;
    if (y + 1 < cur)
        return; /* 바닥보다 엄격히 아래: 무효과 (cur==OPEN 이면 항상 통과) */
    if (src_update_edge(w, cell, cur, x, y + 1, z))
        return;
    src_update_edge(w, cell, cur, x, y, z);
}

void hc_light_accum_write(hc_light_world_t *w, int32_t x, int32_t y, int32_t z,
                          uint16_t old_id, uint16_t new_id) {
    if (y < HC_MIN_Y || y > HC_MAX_Y || old_id == new_id)
        return;
    if (!light_props_differ(old_id, new_id))
        return;
    hc_light_chunk_t *s = slot_must(w, x >> 4, z >> 4);
    if (!s->in_r) {
        fprintf(stderr, "hc_light: live write into pre-08 chunk (%d,%d)\n",
                x >> 4, z >> 4);
        abort();
    }
    src_y_update(w, s, x, y, z);
    if (w->n_check >= (1 << CHECK_LOG)) {
        fprintf(stderr, "hc_light: checkBlock set overflow\n");
        abort();
    }
    /* 중복 미제거 (바닐라는 해시셋) — 중복 checkNode 는 두 번째가 PULL 로
     * 축약돼 결과 동일 (멱등) */
    w->check[w->n_check++] = (uint64_t)(x + 4096) |
                             ((uint64_t)(z + 4096) << 13) |
                             ((uint64_t)(y + 512) << 26);
    if (!s->geo_dirty) {
        s->geo_dirty = 1;
        w->dirty[w->n_dirty++] = (int32_t)(s - w->slots);
    }
}

/* SkyLightEngine.updateSourcesInColumn (R3 §3.2) — 직접-채움 재동기.
 * remove: low-1 부터 아래로 연속 15 런 클리어 (최상단 = REMOVE_TOP 전방향,
 * 나머지 = REMOVE skip-UP). add: low 부터 top 까지 15 채움 + §3.1 시드. */
static void sky_resync_column(hc_light_world_t *w, bfs_t *dq, bfs_t *iq,
                              hc_light_chunk_t *s, int32_t x, int32_t z,
                              int32_t low) {
    int32_t cx = s->chunk->cx, cz = s->chunk->cz;
    if (low != HC_LIGHT_OPEN) {
        for (int32_t y = low - 1;; y--) {
            if (!l_stored_section(w, x, y, z))
                break;
            if (l_stored_get(w, HC_LIGHT_SKY, x, y, z) != 15)
                break;
            l_stored_set(w, HC_LIGHT_SKY, x, y, z, 0);
            q_push(dq, x, y, z, 15,
                   y == low - 1 ? MASK_ALL : (MASK_ALL & ~(1u << DIR_U)), 0);
        }
    }
    int32_t lx = x & 15, lz = z & 15;
    int32_t lowN = (lz == 0) ? nb_src(w, cx, cz - 1, lx, 15)
                             : s->src_y[hc_col_idx(lx, lz - 1)];
    int32_t lowS = (lz == 15) ? nb_src(w, cx, cz + 1, lx, 0)
                              : s->src_y[hc_col_idx(lx, lz + 1)];
    int32_t lowW = (lx == 0) ? nb_src(w, cx - 1, cz, 15, lz)
                             : s->src_y[hc_col_idx(lx - 1, lz)];
    int32_t lowE = (lx == 15) ? nb_src(w, cx + 1, cz, 0, lz)
                              : s->src_y[hc_col_idx(lx + 1, lz)];
    int32_t start = low == HC_LIGHT_OPEN ? HC_LIGHT_SEC_MIN * 16 : low;
    int32_t top_y = s->top == HC_LIGHT_NO_TOP ? start - 1 : s->top * 16 - 1;
    for (int32_t y = start; y <= top_y; y++) {
        if (!l_stored_section(w, x, y, z))
            continue;
        l_stored_set(w, HC_LIGHT_SKY, x, y, z, 15);
        unsigned m = 0;
        if (y == low)
            m |= 1u << DIR_D;
        if (y < lowN)
            m |= 1u << DIR_N;
        if (y < lowS)
            m |= 1u << DIR_S;
        if (y < lowW)
            m |= 1u << DIR_W;
        if (y < lowE)
            m |= 1u << DIR_E;
        if (m)
            q_push(iq, x, y, z, 15, m, 0);
    }
}

/* checkNode (R2 §4 block / R3 §3.2 sky) */
static void check_node(hc_light_world_t *w, bfs_t *dq, bfs_t *iq, int layer,
                       int32_t x, int32_t y, int32_t z) {
    hc_light_chunk_t *s = slot(w, x >> 4, z >> 4);
    if (layer == HC_LIGHT_SKY) {
        int32_t low = (s && s->enabled)
                          ? s->src_y[hc_col_idx(x & 15, z & 15)]
                          : INT32_MAX;
        if (low != INT32_MAX)
            sky_resync_column(w, dq, iq, s, x, z, low);
        if (!l_stored_section(w, x, y, z))
            return;
        if (y >= low) {
            /* REMOVE_SKY_SOURCE + ADD_SKY_SOURCE (둘 다 skip-UP, 15) */
            q_push(dq, x, y, z, 15, MASK_ALL & ~(1u << DIR_U), 0);
            q_push(iq, x, y, z, 15, MASK_ALL & ~(1u << DIR_U), 0);
        } else {
            int lvl = l_stored_get(w, HC_LIGHT_SKY, x, y, z);
            if (lvl > 0) {
                l_stored_set(w, HC_LIGHT_SKY, x, y, z, 0);
                q_push(dq, x, y, z, lvl, MASK_ALL, 0);
            } else {
                q_push(dq, x, y, z, 1, MASK_ALL, 0); /* PULL_LIGHT_IN */
            }
        }
    } else {
        if (!l_stored_section(w, x, y, z))
            return;
        uint16_t st = l_state(w, x, y, z);
        int em = (s && s->enabled) ? g_emit[st] : 0; /* getEmission 게이트 */
        int stored = l_stored_get(w, HC_LIGHT_BLOCK, x, y, z);
        if (em < stored) {
            l_stored_set(w, HC_LIGHT_BLOCK, x, y, z, 0);
            q_push(dq, x, y, z, stored, MASK_ALL, 0);
        } else {
            q_push(dq, x, y, z, 1, MASK_ALL, 0); /* PULL_LIGHT_IN */
        }
        if (em > 0)
            q_push(iq, x, y, z, em, MASK_ALL, 1);
    }
}

/* propagateDecrease 드레인 (R2 §6 block / R3 §3.4 sky): 감소는 opacity/
 * 폐색 무시 초과-클리어, 경계 생존자 (nl >= from) 는 재-flood 프론티어. */
static void run_decreases(bfs_t *dq, bfs_t *iq, int layer) {
    hc_light_world_t *w = dq->w;
    uint64_t          cap = 1ull << w->qlog;
    while (dq->head != dq->tail) {
        uint64_t e = dq->buf[dq->head++ & (cap - 1)];
        int32_t  x = (int32_t)(e & 0x1FFF) - 4096;
        int32_t  z = (int32_t)((e >> 13) & 0x1FFF) - 4096;
        int32_t  y = (int32_t)((e >> 26) & 0x3FF) - 512;
        int      from = (int)((e >> 36) & 15);
        unsigned mask = (unsigned)((e >> 40) & MASK_ALL);
        for (int d = 0; d < 6; d++) {
            if (!(mask & (1u << d)))
                continue;
            int32_t nx = x + DIR_DX[d], ny = y + DIR_DY[d],
                    nz = z + DIR_DZ[d];
            if (!l_stored_section(w, nx, ny, nz))
                continue; /* propagateFromEmptySections — 연속 등록 가드 */
            int nl = l_stored_get(w, layer, nx, ny, nz);
            if (nl == 0)
                continue;
            if (nl <= from - 1) {
                uint16_t          nst = l_state(w, nx, ny, nz);
                hc_light_chunk_t *ns = slot(w, nx >> 4, nz >> 4);
                int em = (layer == HC_LIGHT_BLOCK && ns && ns->enabled)
                             ? g_emit[nst]
                             : 0;
                l_stored_set(w, layer, nx, ny, nz, 0);
                if (em < nl)
                    q_push(dq, nx, ny, nz, nl, MASK_ALL & ~(1u << DIR_OPP[d]),
                           0);
                if (em > 0)
                    q_push(iq, nx, ny, nz, em, MASK_ALL, 1);
            } else {
                q_push(iq, nx, ny, nz, nl, 1u << DIR_OPP[d], 0);
            }
        }
    }
}

void hc_light_accum_flush(hc_light_world_t *w) {
    if (w->n_check == 0)
        return;
    /* updateSectionStatus 라이브 등가 — 쓰인 청크만 재유도 */
    for (int32_t i = 0; i < w->n_dirty; i++) {
        hc_light_chunk_t *s = &w->slots[w->dirty[i]];
        s->geo_dirty = 0;
        derive_geometry_chunk(w, s);
    }
    if (w->n_dirty)
        check_contiguous(w);
    w->n_dirty = 0;
    /* LevelLightEngine.runLightUpdates: block 엔진 → sky 엔진; 각각
     * checkNode 전체 → 감소 완전 드레인 → 증가 드레인 (R2 §3). */
    for (int pass = 0; pass < 2; pass++) {
        int   layer = pass == 0 ? HC_LIGHT_BLOCK : HC_LIGHT_SKY;
        bfs_t dq = {w, 0, 0, w->queue2};
        bfs_t iq = {w, 0, 0, w->queue};
        for (int32_t i = 0; i < w->n_check; i++) {
            uint64_t e = w->check[i];
            check_node(w, &dq, &iq, layer, (int32_t)(e & 0x1FFF) - 4096,
                       (int32_t)((e >> 26) & 0x3FF) - 512,
                       (int32_t)((e >> 13) & 0x1FFF) - 4096);
        }
        run_decreases(&dq, &iq, layer);
        bfs_run(&iq, layer);
    }
    w->n_check = 0;
}

int hc_light_get(const hc_light_world_t *w, int layer, int32_t x, int32_t y,
                 int32_t z) {
    const hc_light_chunk_t *s = slot(w, x >> 4, z >> 4);
    int32_t                 sec = y >> 4;
    if (layer == HC_LIGHT_SKY) {
        /* topSections 미스 (등록 전무) = default currentLowestY → 15 */
        if (!s || s->top == HC_LIGHT_NO_TOP)
            return 15;
        if (sec >= s->top)
            return 15;
        if (!sec_registered(s, sec)) {
            /* 갭: 위쪽 첫 레이어의 바닥 슬라이스 (R3 §2.2) */
            do {
                sec++;
                if (sec >= s->top)
                    return 15;
            } while (!sec_registered(s, sec));
            return s->light[layer][cell_idx(sec, x, sec * 16, z)];
        }
        return s->light[layer][cell_idx(sec, x, y, z)];
    }
    if (!s || !sec_registered(s, sec))
        return 0;
    return s->light[layer][cell_idx(sec, x, y, z)];
}
