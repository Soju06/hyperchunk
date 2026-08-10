#include "hc_biome.h"

#include <assert.h>
#include <stdatomic.h>
#include <string.h>

/* LinearCongruentialGenerator.next: seed *= seed*MULT + INC; seed += salt.
 * 전부 64-bit 랩어라운드 — uint64 로 계산한다. */
static uint64_t lcg_next(uint64_t seed, uint64_t salt) {
    seed *= seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return seed + salt;
}

/* getFiddle: floorMod(l >> 24, 1024)/1024 → [-0.45, 0.45). l>>24 은 자바
 * 산술 시프트지만 하위 10비트만 쓰므로 논리 시프트 + 마스크와 동일하다. */
static double fiddle(uint64_t l) {
    double d = (double)(int32_t)((l >> 24) & 1023) / 1024.0;
    return (d - 0.5) * 0.9;
}

/* getFiddledDistance 의 콘너별 오프셋 (fx,fy,fz) — (seed, 콘너 정수좌표)
 * 만의 순수 함수 (P2-8: 쿼리 블록은 dx/dy/dz 분수만 기여). lcg 시퀀스는
 * 종전 fiddled_distance 본문 그대로다. */
static void corner_fiddle(uint64_t seed, int32_t x, int32_t y, int32_t z,
                          double f[3]) {
    uint64_t l = seed;
    l = lcg_next(l, (uint64_t)(int64_t)x);
    l = lcg_next(l, (uint64_t)(int64_t)y);
    l = lcg_next(l, (uint64_t)(int64_t)z);
    l = lcg_next(l, (uint64_t)(int64_t)x);
    l = lcg_next(l, (uint64_t)(int64_t)y);
    l = lcg_next(l, (uint64_t)(int64_t)z);
    f[0] = fiddle(l);
    l = lcg_next(l, seed);
    f[1] = fiddle(l);
    l = lcg_next(l, seed);
    f[2] = fiddle(l);
}

/* P2-8 쿼트-셀 fiddle 캐시. 한 쿼리의 8콘너는 셀 원점 (q0x,q0y,q0z) 로
 * 결정되고, 콘너 오프셋은 (zoom_seed, 콘너 좌표) 만의 함수라 셀 단위로
 * 캐시 가능하다 — 히트 시 쿼리당 lcg 64회가 3-double×8 조회로 붕괴한다.
 * 값-불변 논거: 캐시는 corner_fiddle 이 계산한 double 을 그대로 돌려주고,
 * 거리 합산식/엄격 비교 순서는 아래 본문이 종전과 동일하게 유지한다
 * (재배열 없음, -ffp-contract=off 로 FMA 없음). 태그에 seed 를 포함해
 * 한 프로세스에서 시드가 바뀌어도 (테스트) 안전하다.
 *
 * 저장소: 정적 풀 + 스레드당 1회 원자 핸드아웃. 큰 _Thread_local 배열은
 * 금물 — PT_TLS 는 이미 ~8.2MB (features_tree 스크래치) 로 기본 8MB
 * 스레드 스택에서 차감되는 구조라, 여기 220KB 를 더하면 체인 워커가
 * 스택 가드를 밟는다 (P2-8 실발화). 풀 슬롯은 소유 스레드 전용 (공유
 * 없음 — TSan 관측면은 relaxed 카운터뿐), 소진 시 무캐시 폴백 (동일
 * 계산 경로라 값 동일). bss 풀은 제로 페이지 — 안 쓴 슬롯 물리 비용 0. */
enum { ZC_LOG = 10, ZC_N = 1 << ZC_LOG, ZC_POOL = 192 };
typedef struct {
    int64_t seed;
    int32_t qx, qy, qz;
    int32_t valid;
    double  f[8][3]; /* 콘너 i (비트 4=x+1, 2=y+1, 1=z+1) 의 (fx,fy,fz) */
} zoom_cell_t;
static zoom_cell_t                 g_zc_pool[ZC_POOL][ZC_N];
static _Atomic int32_t             g_zc_next;
static _Thread_local zoom_cell_t  *g_zc_tab;
static _Thread_local int           g_zc_none; /* 풀 소진 — 폴백 고정 */

static void zoom_cell_fill(int64_t seed, int32_t q0x, int32_t q0y,
                           int32_t q0z, zoom_cell_t *c) {
    c->seed = seed;
    c->qx = q0x;
    c->qy = q0y;
    c->qz = q0z;
    c->valid = 1;
    for (int32_t i = 0; i < 8; i++)
        corner_fiddle((uint64_t)seed, (i & 4) == 0 ? q0x : q0x + 1,
                      (i & 2) == 0 ? q0y : q0y + 1,
                      (i & 1) == 0 ? q0z : q0z + 1, c->f[i]);
}

static const zoom_cell_t *zoom_cell(int64_t seed, int32_t q0x, int32_t q0y,
                                    int32_t q0z, zoom_cell_t *fallback) {
    zoom_cell_t *tab = g_zc_tab;
    if (!tab) {
        if (g_zc_none) {
            zoom_cell_fill(seed, q0x, q0y, q0z, fallback);
            return fallback;
        }
        int32_t slot = atomic_fetch_add_explicit(&g_zc_next, 1,
                                                 memory_order_relaxed);
        if (slot >= ZC_POOL) {
            g_zc_none = 1;
            zoom_cell_fill(seed, q0x, q0y, q0z, fallback);
            return fallback;
        }
        tab = g_zc_tab = g_zc_pool[slot];
    }
    /* splitmix64 마무리 믹스 — 분포만 중요 (값 무관) */
    uint64_t k = (uint64_t)(uint32_t)q0x * 0x9E3779B97F4A7C15ULL ^
                 (uint64_t)(uint32_t)q0y * 0xBF58476D1CE4E5B9ULL ^
                 (uint64_t)(uint32_t)q0z * 0x94D049BB133111EBULL ^
                 (uint64_t)seed;
    k ^= k >> 31;
    k *= 0xD6E8FEB86659FD93ULL;
    k ^= k >> 27;
    zoom_cell_t *c = &tab[k & (ZC_N - 1)];
    if (c->valid && c->seed == seed && c->qx == q0x && c->qy == q0y &&
        c->qz == q0z)
        return c;
    zoom_cell_fill(seed, q0x, q0y, q0z, c);
    return c;
}

void hc_biome_zoom(int64_t zoom_seed, int32_t x, int32_t y, int32_t z,
                   int32_t *qx, int32_t *qy, int32_t *qz) {
    int32_t bx = x - 2, by = y - 2, bz = z - 2;
    int32_t q0x = bx >> 2, q0y = by >> 2, q0z = bz >> 2;
    double  fx = (double)(bx & 3) / 4.0;
    double  fy = (double)(by & 3) / 4.0;
    double  fz = (double)(bz & 3) / 4.0;

    zoom_cell_t        local;
    const zoom_cell_t *cell = zoom_cell(zoom_seed, q0x, q0y, q0z, &local);

    int32_t best = 0;
    double  best_dist = 1.0 / 0.0; /* +Inf — 첫 후보가 항상 이긴다 */
    for (int32_t i = 0; i < 8; i++) {
        int lo_x = (i & 4) == 0, lo_y = (i & 2) == 0, lo_z = (i & 1) == 0;
        double dx = lo_x ? fx : fx - 1.0;
        double dy = lo_y ? fy : fy - 1.0;
        double dz = lo_z ? fz : fz - 1.0;
        /* getFiddledDistance 합산 순서 z, y, x — 종전 식 그대로 */
        const double *f = cell->f[i];
        double        d = (dz + f[2]) * (dz + f[2]) +
                   (dy + f[1]) * (dy + f[1]) + (dx + f[0]) * (dx + f[0]);
        if (best_dist > d) { /* 엄격 비교 — 동률은 선착 후보 유지 */
            best = i;
            best_dist = d;
        }
    }
    *qx = (best & 4) == 0 ? q0x : q0x + 1;
    *qy = (best & 2) == 0 ? q0y : q0y + 1;
    *qz = (best & 1) == 0 ? q0z : q0z + 1;
}

/* --- 레지스트리 --- */

void hc_biome_reg_init(hc_biome_reg_t *r, hc_arena_t *arena) {
    r->arena = arena;
    r->count = 0;
    for (int i = 0; i < HC_BIOME_MAX; i++) {
        r->names[i] = 0;
        r->temperature[i] = 0.0f / 0.0f; /* NaN — 기후 미설정 감시 */
        r->temp_modifier[i] = HC_BIOME_TEMP_MOD_NONE;
    }
}

int32_t hc_biome_find(const hc_biome_reg_t *r, const char *name, int32_t len) {
    for (int32_t i = 0; i < r->count; i++)
        if ((int32_t)strlen(r->names[i]) == len &&
            memcmp(r->names[i], name, (size_t)len) == 0)
            return i;
    return -1;
}

int32_t hc_biome_intern(hc_biome_reg_t *r, const char *name, int32_t len) {
    int32_t i = hc_biome_find(r, name, len);
    if (i >= 0)
        return i;
    if (r->count >= HC_BIOME_MAX)
        return -1;
    char *copy = hc_arena_alloc(r->arena, (size_t)len + 1, 1);
    if (!copy)
        return -1;
    memcpy(copy, name, (size_t)len);
    copy[len] = '\0';
    r->names[r->count] = copy;
    return r->count++;
}

void hc_biome_set_climate(hc_biome_reg_t *r, int32_t id, float temperature,
                          uint8_t temp_modifier) {
    assert(id >= 0 && id < r->count);
    r->temperature[id] = temperature;
    r->temp_modifier[id] = temp_modifier;
}

/* --- 리전 뷰: BiomeManager.getBiome → ChunkAccess.getNoiseBiome ---
 * 쿼트 y 만 청크 범위로 클램프한다 (A5 §7.2 — x/z 는 마스킹으로 실제
 * 청크에 라우팅될 뿐이므로 뷰 범위 안이어야 한다). */
uint16_t hc_biome_view_get(const hc_biome_view_t *v, int32_t x, int32_t y,
                           int32_t z) {
    int32_t qx, qy, qz;
    hc_biome_zoom(v->zoom_seed, x, y, z, &qx, &qy, &qz);
    if (qy < v->qy0)
        qy = v->qy0;
    if (qy > v->qy0 + v->ny - 1)
        qy = v->qy0 + v->ny - 1;
    assert(qx >= v->qx0 && qx < v->qx0 + v->nxz);
    assert(qz >= v->qz0 && qz < v->qz0 + v->nxz);
    return v->ids[((size_t)(qy - v->qy0) * (size_t)v->nxz +
                   (size_t)(qz - v->qz0)) *
                      (size_t)v->nxz +
                  (size_t)(qx - v->qx0)];
}
