#ifndef HC_GEN_NOISE_H
#define HC_GEN_NOISE_H

#include "../include/hc_chunk.h"
#include "../include/hc_df.h"
#include "../include/hc_rng.h"
#include "hc_beard.h"
#include "hc_blocks.h"

/* 04_noise 스테이지 — 내부 전용 (core/src). 공개 ABI 는 리전 단위 표면만
 * 유지한다 (ADR-003 D2). 모든 시맨틱은 26.2 바이트코드 (javap) 기준:
 * NoiseChunk / NoiseBasedChunkGenerator.doFill / Aquifer$NoiseBasedAquifer /
 * OreVeinifier. 분석 노트는 커밋 메시지와 tests/parity/test_noise_stage.c
 * 헤더 주석에 요약한다. 블록 id 는 hc_blocks.h 로 이동했다 (Task 7). */

/* ProtoChunk.setBlockState 의 하이트맵 갱신 (OCEAN_FLOOR_WG →
 * WORLD_SURFACE_WG 순). states 기록 후 호출. gen_noise_stage.c 구현 —
 * 05_surface 가 공유한다. */
void hc_hm_update_both(hc_chunk_t *c, int x, int y, int z, uint16_t state);

/* 노이즈 스테이지가 쓰는 라우터 슬롯 루트 (컴파일된 공유 그래프의 노드
 * 인덱스). 호출자(테스트/CLI)가 hc_df_compile_expr 로 채운다. */
typedef struct {
    int32_t final_density;
    int32_t barrier;
    int32_t fluid_level_floodedness;
    int32_t fluid_level_spread;
    int32_t lava;
    int32_t erosion;
    int32_t depth;
    int32_t preliminary_surface_level;
    int32_t vein_toggle;
    int32_t vein_ridged;
    int32_t vein_gap;
} hc_noise_roots_t;

/* Aquifer$FluidStatus: (fluidLevel, fluidType) 레코드. type 은 water/lava. */
typedef struct {
    int32_t level;
    uint8_t type;
} hc_fluid_status_t;

struct hc_noise_chunk_s;

/* NoiseBasedAquifer. 그리드 캐시는 청크당이고 상태는 지연 계산된다. */
typedef struct {
    struct hc_noise_chunk_s *nc;
    hc_xoro_fork_t           fork; /* RandomState.aquiferRandom */
    int32_t min_grid_x, min_grid_y, min_grid_z;
    int32_t grid_size_x, grid_size_z;
    int32_t n_cells;
    int32_t skip_sampling_above_y;
    uint64_t          *loc_cache; /* BlockPos.asLong, INT64_MAX = 미계산 */
    hc_fluid_status_t *fs_cache;
    uint8_t           *fs_set;
    /* shouldScheduleFluidUpdate — 블록 선택에 영향 없음 (문서화 목적).
     * doFill 이 유체 postprocess 마킹에만 읽는다. computeSubstance 의
     * 모든 경로가 이 플래그를 재설정한다. */
    int should_schedule_fluid_update;
} hc_aquifer_t;

/* (root×mode) 라이브 콘 — hc_nc_init 이 IR 위에서 1회 산출 (P2-1).
 * list 는 오름차순 노드 인덱스, mask 는 eval 의 debug assert 용 멤버십. */
typedef struct {
    int32_t        root; /* 디스패치 키 (노드 인덱스), 다중-루트 콘은 -1 */
    int32_t        len;
    const int32_t *list;
    const uint8_t *mask; /* [g->n] */
} hc_nc_cone_t;

enum { HC_NC_N_SP_CONES = 6, HC_NC_N_BLOCK_CONES = 4 };

/* NoiseChunk — 셀 상태 기계 + flat_cache 테이블 + psl 메모 + aquifer. */
typedef struct hc_noise_chunk_s {
    const hc_df_graph_t *g;
    hc_noise_roots_t     roots;
    double              *scratch; /* 2 * g->n */

    /* 라이브 콘 (P2-1). 고정 슬롯×모드 조합만 — 디스패치 미스는 종전
     * 프리픽스 워크 폴백 (항상 옳고 느릴 뿐). */
    hc_nc_cone_t cones_sp[HC_NC_N_SP_CONES]; /* psl, erosion, depth,
                                              * floodedness, spread, lava */
    hc_nc_cone_t cones_block[HC_NC_N_BLOCK_CONES]; /* barrier, vein_toggle,
                                                    * vein_ridged, vein_gap */
    hc_nc_cone_t  cone_cell;   /* final_density (CELL) */
    /* interp 자식 union (SP, WINDOW_SAFE — 슬라이스 포인트는 전부 쿼트 창
     * 안이라 flat_cache 자식 컷) 을 y-불변/가변으로 분할한 것 (P2-2).
     * fill_slice 가 컬럼당 inv 1회 + y 마다 var 만 평가한다. 두 콘은
     * mask (전체 콘 멤버십) 를 공유한다. */
    hc_nc_cone_t  cone_slice_inv, cone_slice_var;
    hc_nc_cone_t *cones_flat;  /* [n_flat] flat_cache 자식 (SP, WINDOW_SAFE
                                * — 테이블 구축 포인트도 전부 창 안) */

    /* wrapNew 디스패치 (hc_df_cellctx_t 가 참조) */
    hc_df_cellctx_t cc;
    int32_t        *interp_of, *flat_of; /* [g->n], -1 = 해당 없음 */
    hc_df_interp_t *interp;
    int32_t         n_interp;
    hc_df_flat_t   *flat;
    int32_t         n_flat;

    /* 기하 (26.2 오버월드: cell 4x8, cellCountXZ 4, cellCountY 48) */
    int32_t cx, cz, min_block_x, min_block_z;
    int32_t first_cell_x, first_cell_z;
    int32_t first_noise_x, first_noise_z, noise_size_xz;
    int32_t cell_width, cell_height, cell_count_xz, cell_count_y;
    int32_t cell_noise_min_y;
    int32_t sea_level;

    /* 진행 상태 (doFill 의 커서) */
    int32_t cell_start_x, cell_start_y, cell_start_z;

    /* fullNoiseDensity == cacheAllInCell(final_density + beardifier).
     * selectCellYZ 가 채우고 블록 루프가 in-cell 좌표로 읽는다.
     * beard == NULL 은 Beardifier.EMPTY (+0.0) — init 후 호출자가
     * hc_structures_beard 결과를 붙인다 (구조물 references 없는 게이트는
     * 그대로 NULL). */
    const hc_beard_t *beard;
    double density_cell[4 * 4 * 8];

    /* preliminarySurfaceLevel 컬럼 메모 (Long2IntMap 대응, 값-중립 memo) */
    uint64_t *psl_key;
    int32_t  *psl_val;
    uint8_t  *psl_used;
    int32_t   psl_cap;

    hc_xoro_fork_t ore_fork; /* RandomState.oreRandom */
    hc_aquifer_t   aq;
} hc_noise_chunk_t;

/* 초기화: flat_cache 테이블 구축 (SP 모드, 위상 순서) → aquifer 그리드.
 * roots 는 컴파일된 그래프의 슬롯 루트. 실패(-1)는 arena 소진뿐. */
int hc_nc_init(hc_noise_chunk_t *nc, hc_arena_t *arena,
               const hc_df_graph_t *g, const hc_noise_roots_t *roots,
               int64_t seed, int32_t cx, int32_t cz, int32_t sea_level);

/* NoiseChunk.preliminarySurfaceLevel(x,z) — 쿼트 양자화 + Mth.floor 메모 */
int32_t hc_nc_psl(hc_noise_chunk_t *nc, int32_t x, int32_t z);

/* NoiseChunk.maxPreliminarySurfaceLevel — z 바깥/x 안쪽, step 4, 경계 포함 */
int32_t hc_nc_max_psl_range(hc_noise_chunk_t *nc, int32_t min_x,
                            int32_t min_z, int32_t max_x, int32_t max_z);

/* aquifer 그리드 초기화 (noise_chunk.c 의 hc_nc_init 이 호출) */
int hc_aquifer_init(hc_aquifer_t *aq, hc_arena_t *arena,
                    hc_noise_chunk_t *nc, const hc_xoro_fork_t *fork);

/* 상태 기계 (바닐라 메서드명 대응) */
void hc_nc_initialize_first_cell_x(hc_noise_chunk_t *nc);
void hc_nc_advance_cell_x(hc_noise_chunk_t *nc, int32_t cell_x);
void hc_nc_select_cell_yz(hc_noise_chunk_t *nc, int32_t cell_y,
                          int32_t cell_z);
void hc_nc_update_for_y(hc_noise_chunk_t *nc, int32_t block_y, double t);
void hc_nc_update_for_x(hc_noise_chunk_t *nc, int32_t block_x, double t);
void hc_nc_update_for_z(hc_noise_chunk_t *nc, int32_t block_z, double t);
void hc_nc_swap_slices(hc_noise_chunk_t *nc);

/* 모드별 단일점 평가 (SP = SinglePointContext, BLOCK = 블록 루프 ctx) */
double hc_nc_eval_sp(hc_noise_chunk_t *nc, int32_t root, int32_t x, int32_t y,
                     int32_t z);
double hc_nc_eval_block(hc_noise_chunk_t *nc, int32_t root, int32_t x,
                        int32_t y, int32_t z);

/* Aquifer.computeSubstance: 블록 id 또는 -1 (= solid 유지, 호출자가
 * veinifier → defaultBlock 순으로 진행) */
/* Task 14 진단 전용: 그리드 셀의 소스 위치(positional RNG) + FluidStatus */
void hc_aquifer_debug_cell(hc_aquifer_t *aq, int32_t gx, int32_t gy,
                           int32_t gz, int32_t *sx, int32_t *sy, int32_t *sz,
                           int32_t *level, int32_t *type);

int hc_aquifer_substance(hc_aquifer_t *aq, int32_t x, int32_t y, int32_t z,
                         double density);

/* OreVeinifier filler: 블록 id 또는 -1 (null) */
int hc_ore_vein_block(hc_noise_chunk_t *nc, int32_t x, int32_t y, int32_t z);

/* NoiseBasedChunkGenerator.doFill — chunk 에 블록/하이트맵을 쓴다.
 * chunk 는 hc_chunk_init 직후 상태(전부 air)여야 한다. */
void hc_gen_noise_stage(hc_chunk_t *chunk, hc_noise_chunk_t *nc);

#endif /* HC_GEN_NOISE_H */
