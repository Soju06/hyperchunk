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
     * powder_snow/mud 는 비-풀 형상 특례. 조우 즉시 abort (fail-loud). */
    g_damp[HC_B_ICE] = DAMP_DIE;
    g_damp[HC_B_POWDER_SNOW] = DAMP_DIE;
    g_damp[HC_B_MUD] = DAMP_DIE;

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
    q->w->queue[q->tail++ & (cap - 1)] = e;
}

/* R2 §11: 증가 전용 릴랙세이션. 방향 마스크/stale 드롭은 바닐라 그대로
 * (결과는 lfp 라 어차피 불변 — 엔트리 수 패리티만 위한 보존). */
static void bfs_run(bfs_t *q, int layer) {
    hc_light_world_t *w = q->w;
    uint64_t          cap = 1ull << w->qlog;
    while (q->head != q->tail) {
        uint64_t e = w->queue[q->head++ & (cap - 1)];
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

        for (int d = 0; d < 6; d++) {
            if (!(mask & (1u << d)))
                continue;
            int32_t nx = x + DIR_DX[d], ny = y + DIR_DY[d], nz = z + DIR_DZ[d];
            if (!l_stored_section(w, nx, ny, nz))
                continue; /* 등록 밖 = 벽 */
            int nl = l_stored_get(w, layer, nx, ny, nz);
            if (from - 1 <= nl)
                continue;
            int op = damp_of(l_state(w, nx, ny, nz));
            int newl = from - (op < 1 ? 1 : op);
            if (newl <= nl)
                continue;
            /* shapeOccludes: USO 상태 부재로 항상 false (파일 헤더 주석) */
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
    if (!w->slots || !w->queue)
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

/* ChunkSkyLightSources.fillFrom + findLowestSourceY (R3 §4.3): 위에서
 * 내려오며 첫 dampening!=0 블록에서 멈춘다 (isEdgeOccluded 의 형상 항은
 * USO 부재로 소거). floor = 그 블록 위 y. 없으면 열린 컬럼 센티널. */
static void fill_src_y(hc_light_chunk_t *s) {
    const hc_chunk_t *c = s->chunk;
    /* 섹션별 비-공기 마스크로 전부-공기 구간을 통째로 건너뛴다
     * (vanilla 의 hasOnlyAir 섹션 스킵과 등가) */
    uint32_t nonair = 0;
    for (int32_t sec = -4; sec <= 19; sec++)
        if (section_has_data(c, sec))
            nonair |= 1u << (sec + 4);
    for (int z = 0; z < 16; z++) {
        for (int x = 0; x < 16; x++) {
            int32_t floor_y = HC_LIGHT_OPEN;
            for (int32_t sec = 19; sec >= -4; sec--) {
                if (!(nonair & (1u << (sec + 4))))
                    continue;
                for (int32_t y = sec * 16 + 15; y >= sec * 16; y--) {
                    uint16_t st = c->states[hc_idx(x, y, z)];
                    if (st == HC_B_AIR)
                        continue;
                    if (damp_of(st) != 0) {
                        floor_y = y + 1;
                        goto done;
                    }
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

void hc_light_solve(hc_light_world_t *w) {
    /* --- phase A: 등록 지오메트리 (현재 블록에서 재유도) --- */
    for (int32_t i = 0; i < w->n * w->n; i++) {
        hc_light_chunk_t *s = &w->slots[i];
        if (!s->chunk || !s->in_r)
            continue;
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
                        int32_t ns = sec + dy;
                        t->registered |= 1u << (ns - HC_LIGHT_SEC_MIN);
                        if (ns + 1 > t->top || t->top == HC_LIGHT_NO_TOP)
                            t->top = ns + 1;
                    }
                }
            }
        }
    }
    /* 갭 없는 컬럼 등록 확인 — propagateFromEmptySections(R3 §3.5) 를
     * 미구현으로 남기는 전제. 깨지면 즉사. */
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

    /* --- phase A2: src_y (featured 청크 전부 — 이웃 시드 테이블로 쓰임) --- */
    for (int32_t i = 0; i < w->n * w->n; i++) {
        hc_light_chunk_t *s = &w->slots[i];
        if (s->chunk && s->feat_done)
            fill_src_y(s);
    }

    bfs_t q = {w, 0, 0};

    /* --- phase B: sky 직접 채움 + 시드 (활성 청크; 순서는 lfp 라 무관) --- */
    for (int32_t i = 0; i < w->n * w->n; i++) {
        hc_light_chunk_t *s = &w->slots[i];
        if (!s->chunk || !s->enabled)
            continue;
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
                            q_push(&q, cx * 16 + x, y, cz * 16 + z, 15, mask,
                                   0);
                    }
                    if (low < y0)
                        any_lower = 1;
                }
            }
            if (!any_lower)
                break;
        }
    }
    bfs_run(&q, HC_LIGHT_SKY);

    /* --- phase D: block 시드 (활성 청크의 발광 블록; findBlockLightSources
     * 의 섹션 오름차순/y,z,x 스캔 순서 — 큐 순서만 다르고 lfp 동일) --- */
    q.head = q.tail = 0;
    for (int32_t i = 0; i < w->n * w->n; i++) {
        hc_light_chunk_t *s = &w->slots[i];
        if (!s->chunk || !s->enabled)
            continue;
        int32_t cx = s->chunk->cx, cz = s->chunk->cz;
        for (int32_t y = HC_MIN_Y; y <= HC_MAX_Y; y++) {
            for (int z = 0; z < 16; z++) {
                for (int x = 0; x < 16; x++) {
                    uint16_t st = s->chunk->states[hc_idx(x, y, z)];
                    if (g_emit[st] == 0)
                        continue;
                    q_push(&q, cx * 16 + x, y, cz * 16 + z, g_emit[st],
                           MASK_ALL, 1);
                }
            }
        }
    }
    bfs_run(&q, HC_LIGHT_BLOCK);
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
