/* Task 10 링 프리픽스 본문: disk / seagrass / lake / root_system / geode.
 *
 * 시맨틱 = server-26.2.jar 바이트코드 (본 세션 javap + R5a/R5b/R5c 노트;
 * 인용은 각 함수): 09_light 게이트는 그리드 라이트가 링 청크 상태
 * (±15블록 셸)를 읽기 때문에 프리픽스의 링 데코를 draw-exact 로
 * 재생해야 한다. */

#include "features_internal.h"
#include "hc_noise.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* --- minecraft:disk (DiskFeature.place @0-204 + placeColumn @0-102) ---
 *
 * r = radius.sample (1 드로우); betweenClosed(origin±(r,0,r)) 순회
 * (z 밖, x 안 — y 는 단일 평면), dx^2+dz^2 > r^2 컬럼 스킵;
 * placeColumn: y = origin.y+half_height 부터 origin.y-half_height-1 초과
 * 까지 내려가며 target 술어 통과 셀에 RuleBasedStateProvider.getState
 * (rules 순서 if_true 첫 히트의 then, 아니면 fallback — 드로우는 각
 * sprov 종류에 따름; disk_* 데이터는 전부 simple = 0 드로우) 를 쓴다.
 * markAboveForPostProcessing 은 월드 쓰기가 없다 (10→11 blocks 0-diff,
 * R6 §6) — 생략. */
int hc_featx_disk_place(feat_env_t *e, const hc_disk_cfg_t *c, int32_t x,
                        int32_t y, int32_t z) {
    int32_t top = y + c->half_height;
    int32_t bottom = y - c->half_height - 1;
    int32_t r = hc_featx_iprov_sample(e->rng, &c->radius);
    int     placed = 0;
    for (int32_t dz = -r; dz <= r; dz++)
        for (int32_t dx = -r; dx <= r; dx++) {
            if (dx * dx + dz * dz > r * r)
                continue;
            int32_t px = x + dx, pz = z + dz;
            /* DiskFeature.placeColumn (Task 13): 연속 배치 런의 최상단
             * 블록에서만 markAbove — placedAboveFlag (@70-92): target
             * 불일치가 플래그를 리셋, 배치 직전 플래그 0 이면 mark. */
            int placed_above = 0;
            for (int32_t py = top; py > bottom; py--) {
                if (!hc_featx_bpred_eval(e, &c->target, px, py, pz)) {
                    placed_above = 0;
                    continue;
                }
                const hc_sprov_t *sp = &c->fallback;
                for (int32_t i = 0; i < c->n_rules; i++)
                    if (hc_featx_bpred_eval(e, &c->rules[i].if_true, px, py,
                                            pz)) {
                        sp = &c->rules[i].then;
                        break;
                    }
                uint16_t st = hc_featx_sprov_sample(e->rng, sp);
                hc_feat_set_block(e->rg, px, py, pz, st);
                if (!placed_above)
                    hc_feat_mark_above(e->rg, px, py, pz);
                placed_above = 1;
                placed = 1;
            }
        }
    return placed;
}

/* --- minecraft:seagrass (SeagrassFeature.place @0-300) ---
 *
 * 드로우: nextInt(8)-nextInt(8) (dx), nextInt(8)-nextInt(8) (dz);
 * y = getHeight(OCEAN_FLOOR [FINAL live], ox+dx, oz+dz);
 * 대상이 물이면 nextDouble() < probability 로 tall 여부 (@139-152).
 * canSurvive: 아래 블록 isFaceSturdy(UP) && !#cannot_support_seagrass
 * (Seagrass/TallSeagrassBlock.mayPlaceOn @0-27; tall lower 는 추가로
 * 위치 유체가 물+full — 대상이 이미 물이라 자명). tall 이면 위 칸도
 * 물이어야 lower/upper 를 flag 2 로 쓴다 (@242-254); 아니면 seagrass
 * 단일 (@271-). */
int hc_featx_seagrass_place(feat_env_t *e, const hc_seagrass_cfg_t *c,
                            int32_t x, int32_t y, int32_t z) {
    (void)y;
    /* C 피연산자 평가 순서는 미지정 — 드로우를 명시적으로 순서화 */
    int32_t xa = hc_wgr_next_int(e->rng, 8);
    int32_t xb = hc_wgr_next_int(e->rng, 8);
    int32_t za = hc_wgr_next_int(e->rng, 8);
    int32_t zb = hc_wgr_next_int(e->rng, 8);
    int32_t dx = xa - xb;
    int32_t dz = za - zb;
    int32_t px = x + dx, pz = z + dz;
    int32_t py = hc_feat_height(e->rg, HC_HM_OCEAN_FLOOR, px, pz);
    if (py < HC_MIN_Y || py > HC_MAX_Y)
        return 0;
    if (hc_feat_get_block(e->rg, px, py, pz) != HC_B_WATER)
        return 0;
    int tall = hc_wgr_next_double(e->rng) < (double)c->probability;
    uint16_t below = (py - 1 >= HC_MIN_Y)
                         ? hc_feat_get_block(e->rg, px, py - 1, pz)
                         : HC_B_AIR;
    int may_place = hc_featx_face_sturdy_full(below, /*UP*/ 1) &&
                    !hc_featx_mask_test(e->reg->tag_cannot_support_seagrass,
                                        below);
    if (!may_place)
        return 0;
    if (tall) {
        if (py + 1 > HC_MAX_Y ||
            hc_feat_get_block(e->rg, px, py + 1, pz) != HC_B_WATER)
            return 0;
        hc_feat_set_block(e->rg, px, py, pz, HC_B_TALL_SEAGRASS_LOWER);
        hc_feat_set_block(e->rg, px, py + 1, pz, HC_B_TALL_SEAGRASS_UPPER);
        return 1;
    }
    hc_feat_set_block(e->rg, px, py, pz, HC_B_SEAGRASS);
    return 1;
}

/* ======================= minecraft:lake (R5b) =======================
 *
 * LakeFeature.place @0-1351. 4 패스: blob 그리드 채우기 → 환경 검사
 * (중단 가능, 쓰기 없음) → 굴착 (y>=4 cave_air / y<4 유체) → 배리어 셸
 * (y>=4 는 셀당 nextInt(2) 무조건 드로우). 물 호수 전용 pass 4 (결빙) 는
 * 데이터에 없다 (lake_lava 뿐) — 도달 시 즉사. 전 루프 x 밖 / z 중간 /
 * y 안 순서, 그리드 인덱스 (x*16+z)*8+y (R5b §6). */

static int lake_grid_border(const uint8_t *grid, int32_t x, int32_t z,
                            int32_t y) {
    if (grid[(x * 16 + z) * 8 + y])
        return 0;
    /* 이웃 검사 순서 +x,-x,+z,-z,+y,-y 단락 (@429-594) — 순수 읽기 */
    return (x < 15 && grid[((x + 1) * 16 + z) * 8 + y]) ||
           (x > 0 && grid[((x - 1) * 16 + z) * 8 + y]) ||
           (z < 15 && grid[(x * 16 + z + 1) * 8 + y]) ||
           (z > 0 && grid[(x * 16 + z - 1) * 8 + y]) ||
           (y < 7 && grid[(x * 16 + z) * 8 + y + 1]) ||
           (y > 0 && grid[(x * 16 + z) * 8 + y - 1]);
}

int hc_featx_lake_place(feat_env_t *e, const hc_lake_cfg_t *c, int32_t ox,
                        int32_t oy, int32_t oz) {
    hc_wgr_t *rng = e->rng;
    /* 가드: origin.y <= minY+4 — 어떤 드로우보다 먼저 (@25-41) */
    if (oy <= HC_MIN_Y + 4)
        return 0;
    /* base = origin + (-8,-4,-8) — 세 축 전부 (@42-52) */
    int32_t bx = ox - 8, by = oy - 4, bz = oz - 8;
    uint8_t grid[2048];
    memset(grid, 0, sizeof grid);
    int32_t blobs = hc_wgr_next_int(rng, 4) + 4; /* DRAW (@60-70) */
    for (int32_t i = 0; i < blobs; i++) {
        /* blob 당 6 nextDouble, 순서 a,b,c,cx,cy,cz; 센터 식은 좌결합
         * 그대로 (@82-220) */
        double a = hc_wgr_next_double(rng) * 6.0 + 3.0;
        double b = hc_wgr_next_double(rng) * 4.0 + 2.0;
        double cc = hc_wgr_next_double(rng) * 6.0 + 3.0;
        double cx = hc_wgr_next_double(rng) * (16.0 - a - 2.0) + 1.0 + a / 2.0;
        double cy = hc_wgr_next_double(rng) * (8.0 - b - 4.0) + 2.0 + b / 2.0;
        double cz =
            hc_wgr_next_double(rng) * (16.0 - cc - 2.0) + 1.0 + cc / 2.0;
        for (int32_t x = 1; x < 15; x++)
            for (int32_t z = 1; z < 15; z++)
                for (int32_t y = 1; y < 7; y++) {
                    double dx = ((double)x - cx) / (a / 2.0);
                    double dy = ((double)y - cy) / (b / 2.0);
                    double dz = ((double)z - cz) / (cc / 2.0);
                    double s = (dx * dx + dy * dy) + dz * dz;
                    if (s < 1.0)
                        grid[(x * 16 + z) * 8 + y] = 1;
                }
    }
    /* PASS 1: 환경 검사 — 쓰기 없음, 중단 가능 (@379-703) */
    for (int32_t x = 0; x < 16; x++)
        for (int32_t z = 0; z < 16; z++)
            for (int32_t y = 0; y < 8; y++) {
                if (!lake_grid_border(grid, x, z, y))
                    continue;
                int32_t wx = bx + x, wy = by + y, wz = bz + z;
                uint16_t st = hc_feat_get_block(e->rg, wx, wy, wz);
                if (y >= 4 && hc_block_is_fluid(st))
                    return 0; /* 공기부 경계에 액체 (@631-646) */
                if (y < 4 && !hc_block_is_solid(st) && st != c->fluid)
                    return 0; /* 유체선 벽 비고체 (@647-669; 참조 동등성) */
                if (!hc_featx_bpred_eval(e, &c->can_place, wx, wy, wz))
                    return 0; /* lake_lava: 상수 true (@670-687) */
            }
    /* PASS 2: 굴착 (@706-865) */
    for (int32_t x = 0; x < 16; x++)
        for (int32_t z = 0; z < 16; z++)
            for (int32_t y = 0; y < 8; y++) {
                if (!grid[(x * 16 + z) * 8 + y])
                    continue;
                int32_t wx = bx + x, wy = by + y, wz = bz + z;
                if (!hc_featx_bpred_eval(e, &c->can_replace_airfluid, wx, wy,
                                         wz))
                    continue;
                hc_feat_set_block(e->rg, wx, wy, wz,
                                  y >= 4 ? HC_B_CAVE_AIR : c->fluid);
                /* y>=4: scheduleTick(CAVE_AIR,0) + markAboveForPostProcessing
                 * (@828-847, R5b §5) */
                if (y >= 4) {
                    hc_feat_schedule_tick(e->rg, wx, wy, wz, HC_B_CAVE_AIR,
                                          HC_TICK_BLOCK, 0);
                    hc_feat_mark_above(e->rg, wx, wy, wz);
                }
            }
    /* PASS 3: 배리어 셸 (@868-1229) — barrier=stone 은 isAir 아님 */
    if (!hc_block_is_air(c->barrier)) {
        for (int32_t x = 0; x < 16; x++)
            for (int32_t z = 0; z < 16; z++)
                for (int32_t y = 0; y < 8; y++) {
                    if (!lake_grid_border(grid, x, z, y))
                        continue;
                    /* y>=4 경계 셀은 블록 읽기 전 무조건 드로우 (@1120-1134);
                     * 0 이면 스킵 */
                    if (y >= 4 && hc_wgr_next_int(rng, 2) == 0)
                        continue;
                    int32_t wx = bx + x, wy = by + y, wz = bz + z;
                    uint16_t st = hc_feat_get_block(e->rg, wx, wy, wz);
                    if (hc_block_is_solid(st) &&
                        hc_featx_bpred_eval(e, &c->can_replace_barrier, wx,
                                            wy, wz)) {
                        hc_feat_set_block(e->rg, wx, wy, wz, c->barrier);
                        /* markAboveForPostProcessing 무조건 (@1207-1211) */
                        hc_feat_mark_above(e->rg, wx, wy, wz);
                    }
                }
    }
    /* PASS 4: 물 호수 결빙 (@1232-1347) — 우리 데이터엔 물 호수 없음 */
    if (hc_block_fluid_is_water(c->fluid))
        hc_featx_die("lake freeze pass (water lake) unimplemented", NULL);
    return 1;
}

/* ================== minecraft:root_system (R5c §2) ==================
 *
 * RootSystemFeature.place @0-85: origin 이 air 아니면 false (0 드로우);
 * placeDirtAndTree (위로 스캔, 중첩 나무 place, 성공 시 placeDirt) →
 * 성공 시 placeRoots. 반환은 origin-air 즉시 true. */

/* Direction.values(): DOWN, UP, N, S, W, E */
static const int8_t RDIR6[6][3] = {{0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                                   {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};

/* spaceForTree (@0-159, 0 드로우): k=1..required, pos+k 가 air 또는
 * (k+1 <= allowedWater && 물) — levelTestDistance=0 이라 4방향 테스트는
 * 죽은 분기 (컴파일 시 0 검증) */
static int rootsys_space_for_tree(feat_env_t *e, const hc_rootsys_cfg_t *c,
                                  int32_t x, int32_t y, int32_t z) {
    for (int32_t k = 1; k <= c->required_vertical_space; k++) {
        uint16_t s = hc_feat_get_block(e->rg, x, y + k, z);
        if (hc_block_is_air(s))
            continue;
        if (!((k + 1 <= c->allowed_vertical_water) &&
              hc_block_fluid_is_water(s)))
            return 0;
    }
    return 1;
}

/* placeRootedDirt (@0-129): 시도당 4 드로우 무조건 (nextInt(r) 쌍 x/z),
 * 오프셋은 항상 컬럼 셀 기준 (x/z 복원, dy=0) */
static void rootsys_dirt_level(feat_env_t *e, const hc_rootsys_cfg_t *c,
                               int32_t x, int32_t y, int32_t z) {
    for (int32_t a = 0; a < c->root_attempts; a++) {
        int32_t x1 = hc_wgr_next_int(e->rng, c->root_radius);
        int32_t x2 = hc_wgr_next_int(e->rng, c->root_radius);
        int32_t z1 = hc_wgr_next_int(e->rng, c->root_radius);
        int32_t z2 = hc_wgr_next_int(e->rng, c->root_radius);
        int32_t px = x + (x1 - x2), pz = z + (z1 - z2);
        if (hc_featx_mask_test(c->root_replaceable,
                               hc_feat_get_block(e->rg, px, y, pz)))
            hc_feat_set_block(e->rg, px, y, pz, c->root_state);
    }
}

/* placeRoots (@0-158): 시도당 6 드로우 무조건, 베이스는 ORIGIN */
static void rootsys_hanging(feat_env_t *e, const hc_rootsys_cfg_t *c,
                            int32_t ox, int32_t oy, int32_t oz) {
    for (int32_t a = 0; a < c->hanging_attempts; a++) {
        int32_t x1 = hc_wgr_next_int(e->rng, c->hanging_radius);
        int32_t x2 = hc_wgr_next_int(e->rng, c->hanging_radius);
        int32_t y1 = hc_wgr_next_int(e->rng, c->hanging_span);
        int32_t y2 = hc_wgr_next_int(e->rng, c->hanging_span);
        int32_t z1 = hc_wgr_next_int(e->rng, c->hanging_radius);
        int32_t z2 = hc_wgr_next_int(e->rng, c->hanging_radius);
        int32_t px = ox + (x1 - x2), py = oy + (y1 - y2), pz = oz + (z1 - z2);
        if (!hc_block_is_air(hc_feat_get_block(e->rg, px, py, pz)))
            continue;
        /* canSurvive == 명시 isFaceSturdy(DOWN) — 같은 검사 2회 (@106-137):
         * C 에서는 위 블록 face_sturdy(DOWN) 1회 */
        if (hc_featx_face_sturdy_full(
                hc_feat_get_block(e->rg, px, py + 1, pz), /*DOWN*/ 0))
            hc_feat_set_block(e->rg, px, py, pz, c->hanging_state);
    }
}

int hc_featx_rootsys_place(feat_env_t *e, const hc_rootsys_cfg_t *c,
                           int32_t ox, int32_t oy, int32_t oz) {
    if (!hc_block_is_air(hc_feat_get_block(e->rg, ox, oy, oz)))
        return 0; /* place@10-24, 0 드로우 */
    /* placeDirtAndTree (@0-155) */
    int tree_placed = 0;
    int32_t my = oy;
    for (int32_t i = 0; i < c->root_column_max_height; i++) {
        my += 1; /* 첫 후보 = origin.y+1 (천장 블록) (@12-17) */
        if (hc_feat_height(e->rg, HC_HM_WORLD_SURFACE, ox, oz) < my)
            break; /* 하드 중단 (@21-41) */
        if (hc_featx_bpred_eval(e, &c->allowed_tree_position, ox, my, oz) &&
            rootsys_space_for_tree(e, c, ox, my, oz)) {
            uint16_t below = hc_feat_get_block(e->rg, ox, my - 1, oz);
            /* 아래가 용암/비고체면 하드 중단 — continue 아님 (@74-106).
             * 팔레트 유체는 소스뿐 → LAVA 판별은 id (R5c §7.1) */
            if (below == HC_B_LAVA || !hc_block_is_solid(below))
                break;
            if (hc_featx_run_nested(e, c->tree, ox, my, oz)) {
                /* placeDirt: y = origin.y .. origin.y+i-1 (@24-49) */
                for (int32_t yy = oy; yy < oy + i; yy++)
                    rootsys_dirt_level(e, c, ox, yy, oz);
                tree_placed = 1;
                break;
            }
            /* 나무 실패 → 드로우 소비된 채 위로 계속 (@107-124) */
        }
    }
    if (tree_placed)
        rootsys_hanging(e, c, ox, oy, oz);
    return 1; /* origin air 였으면 항상 true (@84) */
}

/* ===================== minecraft:geode (R5a) =====================
 *
 * GeodeFeature.place @0-1433. 내부 노이즈는 월드시드 전용 LCG 스트림
 * (§4.2) — 시드당 1회 구성해 캐시. 셀 스캔은 betweenClosed: x 최속,
 * y 중간, z 최저속 (§5.1). FP: invSqrt = 1.0/sqrt (JOML 1.10.8, §3.1),
 * 누적은 sum + (inv + nv) 내측 우선 (§3.5). */

static struct {
    int64_t           seed;
    int               ready;
    hc_perlin_t       imp1, imp2;
    hc_perlin_t      *arr1[1], *arr2[1];
    double            amps[1];
    hc_normal_noise_t nn;
} g_geode_noise;

/* Java String.hashCode (h = 31*h + c) — "octave_-4" = 440898198 (R5a §4.2) */
static int32_t java_string_hash(const char *s) {
    int32_t h = 0;
    for (; *s; s++)
        h = (int32_t)((uint32_t)h * 31u + (uint32_t)(uint8_t)*s);
    return h;
}

static void geode_octaves_fill(hc_octaves_t *o, hc_perlin_t **arr,
                               double *amps) {
    o->octaves = arr;
    o->amplitudes = amps;
    o->count = 1;
    o->first_octave = -4;
    /* lowestFreqInputFactor = 2^-4; lowestFreqValueFactor =
     * 2^(count-1)/(2^count - 1) = 1.0 (PerlinNoise.<init>@343-381) */
    o->lowest_freq_input = ldexp(1.0, -4);
    o->lowest_freq_value = ldexp(1.0, 0) / (ldexp(1.0, 1) - 1.0);
}

static const hc_normal_noise_t *geode_noise(int64_t level_seed) {
    if (g_geode_noise.ready && g_geode_noise.seed == level_seed)
        return &g_geode_noise.nn;
    /* WorldgenRandom(LegacyRandomSource(worldSeed)) → NormalNoise.create
     * (rand, -4, [1.0]): PerlinNoise 2개 순차, 각각 forkPositional =
     * nextLong 1회 (LegacyRandomSource.forkPositional@0-11 — xoro 의
     * 2 nextLong 과 다름), fromHashOf = (long)hash ^ seed (R5a §4.2) */
    int32_t  h = java_string_hash("octave_-4");
    hc_lcg_t parent;
    hc_lcg_init(&parent, level_seed);
    int64_t  f1 = hc_lcg_next_long(&parent);
    hc_lcg_t r1;
    hc_lcg_init(&r1, (int64_t)h ^ f1);
    hc_perlin_init_from_lcg(&g_geode_noise.imp1, &r1);
    int64_t  f2 = hc_lcg_next_long(&parent);
    hc_lcg_t r2;
    hc_lcg_init(&r2, (int64_t)h ^ f2);
    hc_perlin_init_from_lcg(&g_geode_noise.imp2, &r2);
    g_geode_noise.amps[0] = 1.0;
    g_geode_noise.arr1[0] = &g_geode_noise.imp1;
    g_geode_noise.arr2[0] = &g_geode_noise.imp2;
    geode_octaves_fill(&g_geode_noise.nn.first, g_geode_noise.arr1,
                       g_geode_noise.amps);
    geode_octaves_fill(&g_geode_noise.nn.second, g_geode_noise.arr2,
                       g_geode_noise.amps);
    /* valueFactor = (1/6)/expectedDeviation(0) = (1/6)/(0.1*(1+1/1))
     * (NormalNoise.<init>@149-162) */
    g_geode_noise.nn.value_factor =
        0.16666666666666666 / (0.1 * (1.0 + 1.0 / (double)1));
    g_geode_noise.seed = level_seed;
    g_geode_noise.ready = 1;
    return &g_geode_noise.nn;
}

/* safeSetBlock: 현재 블록이 cannot_replace 태그면 무기록 (Feature.
 * safeSetBlock@0-27) */
static void geode_safe_set(feat_env_t *e, const hc_geode_cfg_t *c, int32_t x,
                           int32_t y, int32_t z, uint16_t st) {
    if (!hc_featx_mask_test(c->cannot_replace,
                            hc_feat_get_block(e->rg, x, y, z)))
        hc_feat_set_block(e->rg, x, y, z, st);
}

enum { GEODE_POT_MAX = 512, GEODE_PTS_MAX = 16 };

int hc_featx_geode_place(feat_env_t *e, const hc_geode_cfg_t *c, int32_t ox,
                         int32_t oy, int32_t oz) {
    hc_wgr_t *rng = e->rng;
    /* DRAW 1: distribution points (place@42-52) */
    int32_t n_points = hc_featx_iprov_sample(rng, &c->dist_points);
    if (n_points > GEODE_PTS_MAX)
        hc_featx_die("geode distribution points overflow", NULL);
    const hc_normal_noise_t *nn = geode_noise(e->level_seed);
    /* d = nPoints / (double)outerWallDistance.maxInclusive (place@98-111) */
    double d = (double)n_points / (double)c->outer_wall_max;
    /* 역거리 임계값 — filling 은 +d 없음 (place@132-186) */
    double l_fill = 1.0 / sqrt(c->layer_fill);
    double l_inner = 1.0 / sqrt(c->layer_inner + d);
    double l_mid = 1.0 / sqrt(c->layer_middle + d);
    double l_outer = 1.0 / sqrt(c->layer_outer + d);
    /* DRAW 2 (rD): 크랙 크기 — 크랙 여부와 무관하게 항상 (place@189-223) */
    double l_crack =
        1.0 / sqrt(c->crack_base + hc_wgr_next_double(rng) / 2.0 +
                   (n_points > 3 ? d : 0.0));
    /* DRAW 3 (rF): 크랙 플래그 — 항상 (place@225-246; f2d 후 dcmpg <) */
    int do_crack = (double)hc_wgr_next_float(rng) < c->crack_chance;
    /* 분포점 + invalid 조기 중단 (place@248-385) */
    struct {
        int32_t x, y, z, off;
    } pts[GEODE_PTS_MAX];
    int32_t invalid = 0;
    for (int32_t i = 0; i < n_points; i++) {
        int32_t wx = hc_featx_iprov_sample(rng, &c->outer_wall);
        int32_t wy = hc_featx_iprov_sample(rng, &c->outer_wall);
        int32_t wz = hc_featx_iprov_sample(rng, &c->outer_wall);
        int32_t px = ox + wx, py = oy + wy, pz = oz + wz;
        uint16_t s = hc_feat_get_block(e->rg, px, py, pz);
        if (hc_block_is_air(s) || hc_featx_mask_test(c->invalid_blocks, s)) {
            if (++invalid > c->invalid_threshold)
                return 0; /* point_offset 미드로우 채 중단 (place@354) */
        }
        pts[i].x = px;
        pts[i].y = py;
        pts[i].z = pz;
        pts[i].off = hc_featx_iprov_sample(rng, &c->point_offset);
    }
    /* 크랙 포인트 (place@388-643) */
    int32_t cps[3][3];
    int32_t n_cp = 0;
    if (do_crack) {
        int32_t k = hc_wgr_next_int(rng, 4); /* DRAW */
        int32_t j = n_points * 2 + 1;
        int32_t jx = (k == 0 || k == 2) ? j : 0;
        int32_t jz = (k == 1 || k == 2) ? j : 0;
        cps[0][0] = ox + jx;
        cps[0][1] = oy + 7;
        cps[0][2] = oz + jz;
        cps[1][0] = ox + jx;
        cps[1][1] = oy + 5;
        cps[1][2] = oz + jz;
        cps[2][0] = ox + jx;
        cps[2][1] = oy + 1;
        cps[2][2] = oz + jz;
        n_cp = 3;
    }
    /* 셀 스캔 (place@667-1237) — x 최속 (§5.1) */
    static int32_t pot[GEODE_POT_MAX][3];
    int32_t n_pot = 0;
    for (int32_t z = oz + c->min_gen; z <= oz + c->max_gen; z++)
        for (int32_t y = oy + c->min_gen; y <= oy + c->max_gen; y++)
            for (int32_t x = ox + c->min_gen; x <= ox + c->max_gen; x++) {
                double nv = hc_normal_noise_value(nn, (double)x, (double)y,
                                                  (double)z) *
                            c->noise_mult;
                double sum = 0.0, csum = 0.0;
                for (int32_t p = 0; p < n_points; p++) {
                    /* distSqr: (dx²+dy²)+dz² (Vec3i.distToLowCornerSqr);
                     * 누적: sum + (invSqrt(..) + nv) (place@788-822) */
                    double dx = (double)x - (double)pts[p].x;
                    double dy = (double)y - (double)pts[p].y;
                    double dz = (double)z - (double)pts[p].z;
                    double ds = (dx * dx + dy * dy) + dz * dz;
                    sum = sum +
                          (1.0 / sqrt(ds + (double)pts[p].off) + nv);
                }
                for (int32_t p = 0; p < n_cp; p++) {
                    double dx = (double)x - (double)cps[p][0];
                    double dy = (double)y - (double)cps[p][1];
                    double dz = (double)z - (double)cps[p][2];
                    double ds = (dx * dx + dy * dy) + dz * dz;
                    csum = csum +
                           (1.0 / sqrt(ds + (double)c->crack_offset) + nv);
                }
                if (sum < l_outer)
                    continue; /* dcmpg ifge (place@887-895) */
                if (do_crack && csum >= l_crack && sum < l_fill) {
                    /* 크랙: 리터럴 AIR + 인접 유체 scheduleTick(np,
                     * fs.getType(), 0) — DIRECTIONS 순 (R5a §7,
                     * place@898-1011) */
                    geode_safe_set(e, c, x, y, z, HC_B_AIR);
                    if (e->rg->ticks) {
                        static const int8_t D6[6][3] = {
                            {0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                            {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};
                        for (int d = 0; d < 6; d++) {
                            int32_t nx = x + D6[d][0], ny = y + D6[d][1],
                                    nz = z + D6[d][2];
                            uint16_t nst = hc_feat_get_block(e->rg, nx, ny,
                                                             nz);
                            if (hc_block_fluid_nonempty(nst))
                                hc_feat_schedule_tick(
                                    e->rg, nx, ny, nz, nst,
                                    hc_block_fluid_is_water(nst)
                                        ? HC_TICK_WATER
                                        : HC_TICK_LAVA,
                                    0);
                        }
                    }
                    continue;
                }
                if (sum >= l_fill) {
                    geode_safe_set(e, c, x, y, z, c->fill);
                    continue;
                }
                if (sum >= l_inner) {
                    /* DRAW (rF): 이 분기 항상 (place@1056-1076) */
                    int alt =
                        (double)hc_wgr_next_float(rng) < c->use_alt;
                    geode_safe_set(e, c, x, y, z,
                                   alt ? c->alt_inner : c->inner);
                    if (!c->require_alt || alt) {
                        /* DRAW (rF): 게이트 통과 시만 (place@1144-1156) */
                        if ((double)hc_wgr_next_float(rng) <
                            c->use_potential) {
                            if (n_pot >= GEODE_POT_MAX)
                                hc_featx_die("geode potential overflow",
                                             NULL);
                            pot[n_pot][0] = x;
                            pot[n_pot][1] = y;
                            pot[n_pot][2] = z;
                            n_pot++;
                        }
                    }
                    continue;
                }
                if (sum >= l_mid) {
                    geode_safe_set(e, c, x, y, z, c->middle);
                    continue;
                }
                geode_safe_set(e, c, x, y, z, c->outer);
            }
    /* 싹/클러스터 (place@1243-1433) */
    for (int32_t p = 0; p < n_pot; p++) {
        /* DRAW: Util.getRandom — 방향 성공 여부와 무관하게 소비 */
        int32_t  k = hc_wgr_next_int(rng, c->n_placements);
        uint16_t base = c->placements[k];
        int32_t  size12 = (base - HC_B_AMETHYST_BUD_BASE) / 12 * 12;
        for (int d6 = 0; d6 < 6; d6++) {
            int32_t nx = pot[p][0] + RDIR6[d6][0];
            int32_t ny = pot[p][1] + RDIR6[d6][1];
            int32_t nz = pot[p][2] + RDIR6[d6][2];
            uint16_t ns = hc_feat_get_block(e->rg, nx, ny, nz);
            /* canClusterGrowAtState: isAir || (블록==물 && isFull) —
             * 팔레트 물은 소스 뿐 (§7.4) */
            if (hc_block_is_air(ns) || ns == HC_B_WATER) {
                /* facing=dir (D,U,N,S,W,E 인덱스 = 레이아웃), waterlogged =
                 * 이웃 유체 isSource (물이면 true) (place@1319-1398) */
                uint16_t bud =
                    (uint16_t)(HC_B_AMETHYST_BUD_BASE + size12 + d6 * 2 +
                               (ns == HC_B_WATER ? 1 : 0));
                geode_safe_set(e, c, nx, ny, nz, bud);
                break; /* 첫 성공 방향에서 중단 (place@1420) */
            }
        }
    }
    return 1; /* place@1432 */
}

/* ===================== minecraft:kelp (본 세션 javap) =====================
 *
 * KelpFeature.place@0-334: y = getHeight(OCEAN_FLOOR) 컬럼이 물이면
 * h = 1 + nextInt(10) (DRAW); i = 0..h 등반: 셀이 물 && 위가 물 &&
 * canSurvive 면 i==h 에 kelp[age=nextInt(4)+20] (DRAW), 아니면
 * kelp_plant; 실패 시 i>0 이면 아래 칸(kelp_plant 였던)을 kelp 톱으로
 * 캡 시도 (canSurvive(아래) && 그 아래가 kelp 아님 → age DRAW) 후 종료
 * — i==0 실패는 계속 등반 (@228-230 ifle). 반환 = 하나라도 놓았는가. */

/* GrowingPlantBlock.canSurvive@0-78 (kelp/kelp_plant 동일: head=KELP,
 * body=KELP_PLANT, growthDirection=UP): 아래 블록이
 * !#cannot_support_kelp(=magma_block 뿐) && (kelp || kelp_plant ||
 * isFaceSturdy(UP)) */
static int kelp_can_survive(feat_env_t *e, int32_t x, int32_t y, int32_t z) {
    uint16_t below = hc_feat_get_block(e->rg, x, y - 1, z);
    if (below == HC_B_MAGMA_BLOCK)
        return 0;
    if ((below >= HC_B_KELP_BASE && below < HC_B_KELP_BASE + 4) ||
        below == HC_B_KELP_PLANT)
        return 1;
    return hc_featx_face_sturdy_full(below, /*UP*/ 1);
}

int hc_featx_kelp_place(feat_env_t *e, int32_t ox, int32_t oy, int32_t oz) {
    (void)oy;
    int     placed = 0;
    int32_t py = hc_feat_height(e->rg, HC_HM_OCEAN_FLOOR, ox, oz);
    if (hc_feat_get_block(e->rg, ox, py, oz) != HC_B_WATER)
        return 0;
    int32_t h = 1 + hc_wgr_next_int(e->rng, 10); /* DRAW */
    for (int32_t i = 0; i <= h; i++) {
        if (hc_feat_get_block(e->rg, ox, py, oz) == HC_B_WATER &&
            hc_feat_get_block(e->rg, ox, py + 1, oz) == HC_B_WATER &&
            kelp_can_survive(e, ox, py, oz)) {
            if (i == h) {
                int32_t age = hc_wgr_next_int(e->rng, 4); /* DRAW */
                hc_feat_set_block(e->rg, ox, py, oz,
                                  (uint16_t)(HC_B_KELP_BASE + age));
                placed++;
            } else {
                hc_feat_set_block(e->rg, ox, py, oz, HC_B_KELP_PLANT);
            }
        } else if (i > 0) {
            /* 캡: 아래 칸(py-1) 에 kelp 톱 — canSurvive(py-1) 는 py-2 를
             * 읽고, py-2 가 kelp 톱이면 스킵 (@233-268) */
            uint16_t bb = hc_feat_get_block(e->rg, ox, py - 2, oz);
            if (kelp_can_survive(e, ox, py - 1, oz) &&
                !(bb >= HC_B_KELP_BASE && bb < HC_B_KELP_BASE + 4)) {
                int32_t age = hc_wgr_next_int(e->rng, 4); /* DRAW */
                hc_feat_set_block(e->rg, ox, py - 1, oz,
                                  (uint16_t)(HC_B_KELP_BASE + age));
                placed++;
            }
            break;
        }
        py++;
    }
    return placed > 0;
}
