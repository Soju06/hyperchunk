#include "hc_surface.h"
#include "hc_counters.h"

#include <assert.h>
#include <math.h>
#include <string.h>

/* SurfaceSystem.buildSurface + SurfaceRules 평가 — 26.2 바이트코드 1:1
 * (.hermes/notes/task7-surface/A1 §7-9, A2, A3).
 *
 * 바닐라의 lazy 조건 memo 는 대부분 순수 함수 위의 값-투명 캐시라 생략
 * 한다. 유일한 예외는 steep — 하이트맵이 스테이지 도중 변할 수 있어
 * (룰이 air 를 쓰면 WORLD_SURFACE_WG 가 재스캔으로 내려간다) '컬럼에서
 * 처음 평가된 시점'이 값을 결정한다. 바닐라에서 steep 은 Context 당
 * 싱글턴 LazyXZCondition 이므로 (5개 JSON 노드가 공유) 여기서도 컨텍스트
 * 싱글턴 memo 로 재현한다. hole/temperature/above_preliminary_surface
 * 싱글턴과 노드별 LazyY 조건들은 전부 (컨텍스트 상태, 위치) 의 순수
 * 함수라 memo 유무가 출력 비트에 영향을 주지 않는다. */

enum { WAY_BELOW_MIN_Y = -32512 }; /* DimensionType.MIN_Y << 4 (A1 §7) */

/* --- Java 수치 헬퍼 (연산 순서 보존) --- */

static double jmin(double a, double b) {
    return a <= b ? a : b; /* Math.min — NaN/±0 특례는 도달 불가 */
}

static int32_t mth_floor(double d) {
    return (int32_t)floor(d);
}

/* JDK Math.round(double) — JDK-8010430 이후의 비트 알고리즘 그대로.
 * floor(v+0.5) 와는 v == 0.49999999999999994 (0x3FDFFFFFFFFFFFFF) 한
 * 비트패턴에서만 다르다 (덧셈의 이중 반올림): floor 형은 1, 진짜
 * Math.round 는 0. 측도 0 이지만 Tier-1 비트정확이 제품이므로 원본
 * 알고리즘을 이식한다 (adversarial 리뷰 발견 — A1 §3 주석 정정). */
static int64_t java_math_round(double a) {
    uint64_t bits;
    memcpy(&bits, &a, 8);
    int64_t biased_exp = (int64_t)((bits & 0x7FF0000000000000ull) >> 52);
    int64_t shift = 1074 - biased_exp; /* SIGNIFICAND_WIDTH-2+EXP_BIAS */
    if ((shift & ~63LL) == 0) {        /* 0 <= shift < 64 */
        int64_t r =
            (int64_t)((bits & 0x000FFFFFFFFFFFFFull) | 0x0010000000000000ull);
        if ((int64_t)bits < 0)
            r = -r;
        /* 자바 >> 는 산술 시프트 — 음수는 보수 트릭으로 구현정의 회피 */
        int64_t sh = r < 0 ? ~((int64_t)(~(uint64_t)r >> shift))
                           : (int64_t)((uint64_t)r >> shift);
        int64_t t = sh + 1;
        return t < 0 ? ~((int64_t)(~(uint64_t)t >> 1))
                     : (int64_t)((uint64_t)t >> 1);
    }
    /* |a| < 0.5 또는 |a| >= 2^52: Java d2l — 우리 값 범위에선 단순 캐스트 */
    return (int64_t)a;
}

static double mlerp(double t, double a, double b) {
    return a + t * (b - a); /* Mth.lerp */
}

/* Mth.map = lerp(inverseLerp(v,a,b), c, d) — 대수 단순화 금지 */
static double mmap(double v, double a, double b, double c, double d) {
    return mlerp((v - a) / (b - a), c, d);
}

/* --- BlockColumn (SurfaceSystem$1): 빌드 높이 밖 read 는 VOID_AIR --- */

static uint16_t col_get(const hc_chunk_t *c, int x, int32_t y, int z) {
    if (y < HC_MIN_Y || y > HC_MAX_Y)
        return HC_B_AIR;
    return c->states[hc_idx(x, y, z)];
}

/* setBlock: isInsideBuildHeight 가드 + setBlockState(pos,state,3).
 * 하이트맵 갱신은 ProtoChunk.setBlockState 가 하는 일 — badlands 패스의
 * h2 재읽기와 steep 이 이 갱신을 관측한다. 유체 마킹 (Task 13):
 * SurfaceSystem$1.setBlock @35-53 — 유체 상태를 놓으면 자기 청크에
 * markPosForPostProcessing. */
static void col_set(hc_chunk_t *c, int x, int32_t y, int z, uint16_t state) {
    if (y < HC_MIN_Y || y > HC_MAX_Y)
        return;
    c->states[hc_idx(x, y, z)] = state;
    hc_hm_update_both(c, x, y, z, state);
    if (hc_block_fluid_nonempty(state))
        hc_ppg_mark(c->ppg, (c->cx << 4) + x, y, (c->cz << 4) + z);
}

/* SurfaceSystem.isStone: !isAir && fluidState.isEmpty() */
static int is_stone(uint16_t state) {
    return !hc_block_is_air(state) && !hc_block_is_fluid(state);
}

/* --- SurfaceSystem 노이즈 진입점 (A1 §3-4) --- */

int32_t hc_surface_depth(const hc_surface_t *s, int32_t x, int32_t z) {
    double n =
        hc_normal_noise_value(&s->surface_noise, (double)x, 0.0, (double)z);
    hc_xoro_t r;
    hc_xoro_at(&s->noise_random, x, 0, z, &r);
    /* d2i 절단 (floor 아님) — 합이 음수로 갈 수 있는 극단 노이즈에서도
     * 바닐라와 같은 truncation 이어야 한다 */
    return (int32_t)(n * 2.75 + 3.0 + hc_xoro_next_double(&r) * 0.25);
}

double hc_surface_secondary(const hc_surface_t *s, int32_t x, int32_t z) {
    return hc_normal_noise_value(&s->surface_secondary_noise, (double)x, 0.0,
                                 (double)z);
}

uint16_t hc_surface_band(const hc_surface_t *s, int32_t x, int32_t y,
                         int32_t z) {
    double v = hc_normal_noise_value(&s->clay_bands_offset_noise, (double)x,
                                     0.0, (double)z);
    /* Math.round(double) → long, l2i 절단 */
    int32_t off = (int32_t)java_math_round(v * 4.0);
    int32_t idx = (y + off + HC_SURF_CLAY_BANDS) % HC_SURF_CLAY_BANDS;
    assert(idx >= 0); /* 바닐라도 음수면 AIOOBE — 도달 불가 (A1 §3) */
    return s->clay_bands[idx];
}

/* --- SurfaceRules$Context (A2) --- */

typedef struct {
    hc_surface_t          *s;
    hc_chunk_t            *chunk;
    hc_noise_chunk_t      *nc;
    const hc_biome_view_t *view;

    int64_t last_xz, last_y;
    int32_t block_x, block_z, block_y;
    int32_t surface_depth; /* updateXZ 에서 eager (A2 §1.2) */

    int64_t sec_stamp; /* getSurfaceSecondary memo (per updateXZ) */
    double  sec_val;
    int64_t msl_stamp; /* getMinSurfaceLevel memo (per updateXZ) */
    int32_t msl_val;
    int64_t cell_key; /* preliminary-surface 셀 캐시 (ChunkPos.pack) */
    int32_t cell_cache[4];

    int32_t water_height, stone_depth_above, stone_depth_below;
    int32_t biome; /* -1 = 미해석 (updateY 가 무효화) */

    int64_t steep_stamp; /* 컨텍스트 싱글턴 LazyXZ memo (파일 헤더 참조) */
    uint8_t steep_val;

    /* noise_threshold 샘플러 memo (Context$1/$2) — 바닐라 Context 소속
     * 그대로 청크-로컬 (P2-3: 공유 hc_ssampler_t 에서 이동. 값-불변 —
     * ctx_init 리셋과 스탬프 규약 동일). */
    int64_t sam_stamp[HC_SURF_MAX_SAMPLERS];
    double  sam_val[HC_SURF_MAX_SAMPLERS];
} hc_sctx_t;

static void ctx_init(hc_sctx_t *c, hc_surface_t *s, hc_chunk_t *chunk,
                     hc_noise_chunk_t *nc, const hc_biome_view_t *view) {
    c->s = s;
    c->chunk = chunk;
    c->nc = nc;
    c->view = view;
    /* 카운터 초기값은 바닐라 상수 (A2 §1.1). memo 스탬프는 카운터-1 —
     * 카운터가 증가만 하므로 첫 조회가 항상 계산된다. */
    c->last_xz = INT64_MIN + 1;
    c->last_y = INT64_MIN + 1;
    c->sec_stamp = INT64_MIN;
    c->sec_val = 0.0;
    c->msl_stamp = INT64_MIN;
    c->msl_val = 0;
    c->cell_key = INT64_MAX; /* Long.MAX_VALUE 센티널 */
    c->block_x = c->block_z = c->block_y = 0;
    c->surface_depth = 0;
    c->water_height = 0;
    c->stone_depth_above = c->stone_depth_below = 0;
    c->biome = -1;
    c->steep_stamp = INT64_MIN;
    c->steep_val = 0;
    /* 샘플러 memo 리셋 — 청크당 새 Context 의 IdentityHashMap 과 동형 */
    for (int32_t i = 0; i < s->n_samplers; i++)
        c->sam_stamp[i] = INT64_MIN;
}

static void ctx_update_xz(hc_sctx_t *c, int32_t x, int32_t z) {
    c->last_xz++;
    c->last_y++; /* updateXZ 는 Y 카운터도 올린다 (A2 §1.2) */
    c->block_x = x;
    c->block_z = z;
    c->surface_depth = hc_surface_depth(c->s, x, z); /* eager — RNG 소비 */
}

static void ctx_update_y(hc_sctx_t *c, int32_t stone_above, int32_t stone_below,
                         int32_t water_height, int32_t y) {
    c->last_y++;
    c->biome = -1;
    c->block_y = y;
    c->water_height = water_height;
    c->stone_depth_below = stone_below;
    c->stone_depth_above = stone_above;
}

static int32_t ctx_biome(hc_sctx_t *c) {
    if (c->biome < 0)
        c->biome = (int32_t)hc_biome_view_get(c->view, c->block_x, c->block_y,
                                              c->block_z);
    return c->biome;
}

static double ctx_surface_secondary(hc_sctx_t *c) {
    if (c->sec_stamp != c->last_xz) {
        HC_CTR_INC(HC_CTR_SURF_SEC_MISS);
        c->sec_stamp = c->last_xz;
        c->sec_val = hc_surface_secondary(c->s, c->block_x, c->block_z);
    }
    return c->sec_val;
}

/* getMinSurfaceLevel (A2 §1.8): 셀 4모서리 preliminarySurfaceLevel →
 * float 분수 bilinear → floor → + surfaceDepth - 8 */
static int32_t ctx_min_surface_level(hc_sctx_t *c) {
    if (c->msl_stamp != c->last_xz) {
        HC_CTR_INC(HC_CTR_SURF_MSL_MISS);
        c->msl_stamp = c->last_xz;
        int32_t cell_x = c->block_x >> 4;
        int32_t cell_z = c->block_z >> 4;
        /* ChunkPos.pack(a,b) = (a & 0xffffffffL) | ((b & 0xffffffffL) << 32) */
        int64_t key = (int64_t)(((uint64_t)(uint32_t)cell_x) |
                                ((uint64_t)(uint32_t)cell_z << 32));
        if (c->cell_key != key) {
            c->cell_key = key;
            /* surfaceCellToBlockCoord = << 4 (음수 UB 회피용 무부호 시프트) */
            int32_t bx0 = (int32_t)((uint32_t)cell_x << 4);
            int32_t bz0 = (int32_t)((uint32_t)cell_z << 4);
            int32_t bx1 = (int32_t)((uint32_t)(cell_x + 1) << 4);
            int32_t bz1 = (int32_t)((uint32_t)(cell_z + 1) << 4);
            c->cell_cache[0] = hc_nc_psl(c->nc, bx0, bz0);
            c->cell_cache[1] = hc_nc_psl(c->nc, bx1, bz0);
            c->cell_cache[2] = hc_nc_psl(c->nc, bx0, bz1);
            c->cell_cache[3] = hc_nc_psl(c->nc, bx1, bz1);
        }
        /* 분수는 float 나눗셈 후 double 승격 (i2f; fdiv 16.0f; f2d) */
        double tx = (double)((float)(c->block_x & 15) / 16.0f);
        double tz = (double)((float)(c->block_z & 15) / 16.0f);
        /* Mth.lerp2(tx,tz,c00,c10,c01,c11) =
         * lerp(tz, lerp(tx,c00,c10), lerp(tx,c01,c11)) */
        double l0 = mlerp(tx, (double)c->cell_cache[0],
                          (double)c->cell_cache[1]);
        double l1 = mlerp(tx, (double)c->cell_cache[2],
                          (double)c->cell_cache[3]);
        c->msl_val = mth_floor(mlerp(tz, l0, l1)) + c->surface_depth - 8;
    }
    return c->msl_val;
}

/* Context$1/$2 노이즈 샘플러: 2d 는 (x,0,z)/lastUpdateXZ, 3d 는
 * (x,y,z)/lastUpdateY memo (A2 §2-3) */
static double sampler_value(hc_sctx_t *c, int32_t si) {
    const hc_ssampler_t *sp = &c->s->samplers[si];
    int64_t              stamp = sp->is3d ? c->last_y : c->last_xz;
    if (c->sam_stamp[si] != stamp) {
        HC_CTR_INC(HC_CTR_SURF_SAMP_MISS);
        c->sam_val[si] = hc_normal_noise_value(
            &sp->noise, (double)c->block_x,
            sp->is3d ? (double)c->block_y : 0.0, (double)c->block_z);
        c->sam_stamp[si] = stamp;
    }
    return c->sam_val[si];
}

/* SteepMaterialCondition (A2 §7): WORLD_SURFACE_WG, 청크-로컬 클램프.
 * 바닐라 getHeight = firstAvailable - 1 이지만 비교 양변이 같은 -1 을
 * 받으므로 저장값(firstAvailable)으로 비교해도 동치다. */
static int steep_compute(const hc_sctx_t *c) {
    int lx = c->block_x & 15;
    int lz = c->block_z & 15;
    int zn = lz - 1 > 0 ? lz - 1 : 0;
    int zp = lz + 1 < 15 ? lz + 1 : 15;
    const int32_t *hm = c->chunk->heightmap_ws;
    int32_t hn = hm[hc_col_idx(lx, zn)];
    int32_t hs = hm[hc_col_idx(lx, zp)];
    if (hs >= hn + 4)
        return 1;
    int xn = lx - 1 > 0 ? lx - 1 : 0;
    int xp = lx + 1 < 15 ? lx + 1 : 15;
    int32_t hw = hm[hc_col_idx(xn, lz)];
    int32_t he = hm[hc_col_idx(xp, lz)];
    return hw >= he + 4;
}

/* --- 조건 평가 (A3) --- */

static int cond_test(hc_sctx_t *c, int32_t ci) {
    HC_CTR_INC_HOT(HC_CTR_SURF_COND);
    const hc_scond_t *k = &c->s->conds[ci];
    switch (k->kind) {
    case HC_SC_BIOME: {
        int32_t id = ctx_biome(c);
        return (k->u.biome.bits[id >> 6] >> (id & 63)) & 1u;
    }
    case HC_SC_NOISE_THRESHOLD: {
        double v = sampler_value(c, k->u.noise.sampler);
        /* 양쪽 포함, NaN 은 양방향 실패 (C 비교도 NaN → false) */
        return v >= k->u.noise.min_threshold && v <= k->u.noise.max_threshold;
    }
    case HC_SC_VERTICAL_GRADIENT: {
        int32_t y = c->block_y;
        if (y <= k->u.vgrad.true_y)
            return 1;
        if (y >= k->u.vgrad.false_y)
            return 0;
        double g = mmap((double)y, (double)k->u.vgrad.true_y,
                        (double)k->u.vgrad.false_y, 1.0, 0.0);
        hc_xoro_t r;
        hc_xoro_at(&k->u.vgrad.fork, c->block_x, y, c->block_z, &r);
        /* nextFloat (nextDouble 아님), f2d 승격, 엄격 < */
        return (double)hc_xoro_next_float(&r) < g;
    }
    case HC_SC_Y_ABOVE:
        return c->block_y +
                   (k->u.y_above.add_stone ? c->stone_depth_above : 0) >=
               k->u.y_above.anchor_y + c->surface_depth * k->u.y_above.mult;
    case HC_SC_WATER:
        return c->water_height == INT32_MIN ||
               c->block_y + (k->u.water.add_stone ? c->stone_depth_above : 0) >=
                   c->water_height + k->u.water.offset +
                       c->surface_depth * k->u.water.mult;
    case HC_SC_TEMPERATURE:
        return hc_biome_cold_enough_to_snow(c->s->biomes, ctx_biome(c),
                                            c->block_x, c->block_y, c->block_z,
                                            c->s->sea_level);
    case HC_SC_STEEP:
        if (c->steep_stamp != c->last_xz) { /* stamp-before-compute */
            HC_CTR_INC(HC_CTR_SURF_STEEP_MISS);
            c->steep_stamp = c->last_xz;
            c->steep_val = (uint8_t)steep_compute(c);
        }
        return c->steep_val;
    case HC_SC_NOT:
        return !cond_test(c, k->u.not_.inner);
    case HC_SC_HOLE:
        return c->surface_depth <= 0;
    case HC_SC_ABOVE_PRELIM:
        return c->block_y >= ctx_min_surface_level(c);
    case HC_SC_STONE_DEPTH: {
        int32_t depth = k->u.stone_depth.ceiling ? c->stone_depth_below
                                                 : c->stone_depth_above;
        int32_t surf = k->u.stone_depth.add_surface ? c->surface_depth : 0;
        int32_t sec = 0;
        if (k->u.stone_depth.secondary_range != 0)
            /* d2i 절단 (floor 아님) — Mth.map 결과 [0, range] */
            sec = (int32_t)mmap(ctx_surface_secondary(c), -1.0, 1.0, 0.0,
                                (double)k->u.stone_depth.secondary_range);
        return depth <= 1 + k->u.stone_depth.offset + surf + sec;
    }
    }
    assert(0 && "unknown condition kind");
    return 0;
}

/* --- 룰 평가 (A4): tryApply — -1 == null --- */

static int32_t rule_apply(hc_sctx_t *c, int32_t ri, int32_t x, int32_t y,
                          int32_t z) {
    HC_CTR_INC_HOT(HC_CTR_SURF_APPLY);
    const hc_srule_t *r = &c->s->rules[ri];
    switch (r->kind) {
    case HC_SR_SEQUENCE:
        for (int32_t i = 0; i < r->u.seq.count; i++) {
            int32_t v = rule_apply(c, c->s->children[r->u.seq.first + i], x, y,
                                   z);
            if (v >= 0) /* 첫 non-null 승리 */
                return v;
        }
        return -1;
    case HC_SR_BLOCK:
        return r->u.block.block;
    case HC_SR_CONDITION:
        if (!cond_test(c, r->u.test.cond))
            return -1;
        return rule_apply(c, r->u.test.then, x, y, z);
    case HC_SR_BANDLANDS:
        return hc_surface_band(c->s, x, y, z);
    }
    assert(0 && "unknown rule kind");
    return -1;
}

/* SurfaceSystem.topMaterial (task7 A1 §11, task8 A5 §1) — 카버 전용 진입점.
 * 바닐라는 호출마다 새 SurfaceRules.Context 를 만든다 (possibleBiomes=null):
 * 모든 lazy memo 가 초기화된 상태에서 updateXZ → updateY(1, 1,
 * hasFluid ? y+1 : Integer.MIN_VALUE, y) → rule.tryApply(x,y,z).
 * 반환 -1 == Optional.empty (교체 없음). x/z 는 절대 블록 좌표. */
int32_t hc_surface_top_material(hc_surface_t *s, hc_chunk_t *chunk,
                                hc_noise_chunk_t *nc,
                                const hc_biome_view_t *view, int32_t x,
                                int32_t y, int32_t z, int has_fluid) {
    hc_sctx_t ctx;
    ctx_init(&ctx, s, chunk, nc, view);
    ctx_update_xz(&ctx, x, z);
    ctx_update_y(&ctx, 1, 1, has_fluid ? y + 1 : INT32_MIN, y);
    HC_CTR_INC(HC_CTR_SURF_TOPMAT);
    return rule_apply(&ctx, s->root_rule, x, y, z);
}

/* --- erodedBadlandsExtension (A1 §8) — 룰 패스 전, RNG 없음 --- */

static void eroded_badlands_ext(hc_surface_t *s, hc_chunk_t *chunk, int lx,
                                int lz, int32_t x, int32_t z, int32_t h1) {
    double pillar =
        jmin(fabs(hc_normal_noise_value(&s->badlands_surface_noise, (double)x,
                                        0.0, (double)z) *
                  8.25),
             hc_normal_noise_value(&s->badlands_pillar_noise, (double)x * 0.2,
                                   0.0, (double)z * 0.2) *
                 15.0);
    if (pillar <= 0.0)
        return;
    double roof = fabs(hc_normal_noise_value(&s->badlands_pillar_roof_noise,
                                             (double)x * 0.75, 0.0,
                                             (double)z * 0.75) *
                       1.5);
    double top = 64.0 + jmin(pillar * pillar * 2.5, ceil(roof * 50.0) + 24.0);
    int32_t top_y = mth_floor(top);
    if (h1 > top_y)
        return;
    /* scan 1: defaultBlock 의 '블록' 을 만나면 중단, 물이 먼저면 포기.
     * 우리 표현에서 stone 블록 == stone 상태 단일이라 id 비교와 동치. */
    for (int32_t k = top_y; k >= HC_MIN_Y; k--) {
        uint16_t st = col_get(chunk, lx, k, lz);
        if (st == s->default_block)
            break;
        if (st == HC_B_WATER)
            return;
    }
    /* scan 2: 공기를 defaultBlock 으로 채움 (첫 비-공기에서 중단) */
    for (int32_t k = top_y;
         k >= HC_MIN_Y && hc_block_is_air(col_get(chunk, lx, k, lz)); k--)
        col_set(chunk, lx, k, lz, s->default_block);
}

/* --- frozenOceanExtension (A1 §9) — 룰 패스 후 --- */

static void frozen_ocean_ext(hc_sctx_t *c, hc_chunk_t *chunk, int lx, int lz,
                             int32_t x, int32_t z, int32_t h1,
                             int32_t biome_id) {
    hc_surface_t *s = c->s;
    double iceberg =
        jmin(fabs(hc_normal_noise_value(&s->iceberg_surface_noise, (double)x,
                                        0.0, (double)z) *
                  8.25),
             hc_normal_noise_value(&s->iceberg_pillar_noise, (double)x * 1.28,
                                   0.0, (double)z * 1.28) *
                 15.0);
    if (iceberg <= 1.8)
        return;
    double roof = fabs(hc_normal_noise_value(&s->iceberg_pillar_roof_noise,
                                             (double)x * 1.17, 0.0,
                                             (double)z * 1.17) *
                       1.5);
    double max_top = jmin(iceberg * iceberg * 1.2, ceil(roof * 40.0) + 14.0);
    if (hc_biome_should_melt_iceberg(s->biomes, biome_id, x, s->sea_level, z,
                                     s->sea_level))
        max_top -= 2.0;
    double bottom, top;
    if (max_top > 2.0) {
        bottom = (double)s->sea_level - max_top - 7.0;
        top = max_top + (double)s->sea_level;
    } else {
        top = 0.0;
        bottom = 0.0;
    }
    /* 같은 위치시드에서 새 소스 — getSurfaceDepth 의 fork 와 시드는 같고
     * 시퀀스는 0 부터 다시 시작한다 (A1 §9) */
    hc_xoro_t r;
    hc_xoro_at(&s->noise_random, x, 0, z, &r);
    int32_t max_snow = 2 + hc_xoro_next_int(&r, 4);
    int32_t min_snow_y = s->sea_level + 18 + hc_xoro_next_int(&r, 10);
    int32_t snow_placed = 0;

    int32_t start = h1 > (int32_t)top + 1 ? h1 : (int32_t)top + 1;
    int32_t floor_y = ctx_min_surface_level(c);
    for (int32_t y = start; y >= floor_y; y--) {
        int place = 0;
        uint16_t st = col_get(chunk, lx, y, lz);
        /* 가드 순서 그대로: A 의 앞 두 가드 통과 후에만 roll 소비; A 의
         * roll 실패는 B 로 낙하하지만 air != water 라 B roll 은 없다 */
        if (hc_block_is_air(st) && y < (int32_t)top &&
            hc_xoro_next_double(&r) > 0.01) {
            place = 1;
        } else if (col_get(chunk, lx, y, lz) == HC_B_WATER &&
                   y > (int32_t)bottom && y < s->sea_level && bottom != 0.0 &&
                   hc_xoro_next_double(&r) > 0.15) {
            place = 1;
        }
        if (place) {
            if (snow_placed <= max_snow && y > min_snow_y) {
                col_set(chunk, lx, y, lz, HC_B_SNOW_BLOCK);
                snow_placed++;
            } else {
                col_set(chunk, lx, y, lz, HC_B_PACKED_ICE);
            }
        }
    }
}

/* --- buildSurface (A1 §7) --- */

void hc_gen_surface_stage(hc_chunk_t *chunk, hc_noise_chunk_t *nc,
                          hc_surface_t *s, const hc_biome_view_t *view) {
    hc_sctx_t ctx;
    ctx_init(&ctx, s, chunk, nc, view);

    /* ChunkPos.getMinBlockX/Z = chunkX << 4 (무부호 시프트로 UB 회피) */
    int32_t min_x = (int32_t)((uint32_t)chunk->cx << 4);
    int32_t min_z = (int32_t)((uint32_t)chunk->cz << 4);

    for (int lx = 0; lx < 16; lx++) {     /* X 바깥 */
        for (int lz = 0; lz < 16; lz++) { /* Z 안쪽 */
            int32_t x = min_x + lx;
            int32_t z = min_z + lz;
            size_t  col = hc_col_idx(lx, lz);

            /* (1) h1 = getHeight(WORLD_SURFACE_WG)+1 == firstAvailable
             * == 우리 저장값 */
            int32_t h1 = chunk->heightmap_ws[col];

            /* (3) 컬럼 바이옴 — y 는 legacyRandom ? 0 : h1 */
            int32_t biome_col = (int32_t)hc_biome_view_get(
                view, x, s->legacy_random ? 0 : h1, z);

            /* (4) eroded badlands 프리패스 — 하이트맵을 올릴 수 있다 */
            if (biome_col == s->biome_eroded_badlands)
                eroded_badlands_ext(s, chunk, lx, lz, x, z, h1);

            /* (5) h2 — 프리패스 후 재읽기 */
            int32_t h2 = chunk->heightmap_ws[col];
            HC_CTR_ADD(HC_CTR_SURF_YITER,
                       h2 >= HC_MIN_Y ? h2 - HC_MIN_Y + 1 : 0);

            /* (6) updateXZ — eager getSurfaceDepth (positional RNG) */
            ctx_update_xz(&ctx, x, z);

            /* (7) top-down 상태 기계 */
            int32_t stone_above = 0;
            int32_t water_height = INT32_MIN;
            int32_t stone_bottom = INT32_MAX;

            for (int32_t y = h2; y >= HC_MIN_Y; y--) {
                uint16_t st = col_get(chunk, lx, y, lz);
                if (hc_block_is_air(st)) {
                    stone_above = 0;
                    water_height = INT32_MIN;
                    continue;
                }
                if (hc_block_is_fluid(st)) {
                    if (water_height == INT32_MIN)
                        water_height = y + 1;
                    continue;
                }
                if (stone_bottom >= y) {
                    /* 하향 재스캔 — minY-1 포함 (VOID_AIR → not stone) */
                    stone_bottom = WAY_BELOW_MIN_Y;
                    for (int32_t k = y - 1; k >= HC_MIN_Y - 1; k--) {
                        if (!is_stone(col_get(chunk, lx, k, lz))) {
                            stone_bottom = k + 1;
                            break;
                        }
                    }
                }
                stone_above++;
                int32_t stone_below = y - stone_bottom + 1;
                ctx_update_y(&ctx, stone_above, stone_below, water_height, y);

                /* 참조 동일성 게이트: 정확히 defaultBlock 상태만 교체 대상 */
                if (st != s->default_block)
                    continue;
                HC_CTR_INC(HC_CTR_SURF_ROOT);
                int32_t res = rule_apply(&ctx, s->root_rule, x, y, z);
                if (res >= 0)
                    col_set(chunk, lx, y, lz, (uint16_t)res);
            }

            /* (8) frozen ocean 포스트패스 — h1 (프리 badlands) 사용 */
            if (biome_col == s->biome_frozen_ocean ||
                biome_col == s->biome_deep_frozen_ocean)
                frozen_ocean_ext(&ctx, chunk, lx, lz, x, z, h1, biome_col);
        }
    }
}
