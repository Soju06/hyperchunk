#ifndef HC_CARVERS_H
#define HC_CARVERS_H

#include "hc_surface.h"

/* 06_carvers 스테이지 — 내부 전용 (core/src). 시맨틱은 전부 26.2
 * 바이트코드 기준 (.hermes/notes/task8-carvers/A1..A7):
 *  - 오케스트레이션: 17x17 소스 청크 스캔 + setLargeFeatureSeed (A1 §4)
 *  - WorldCarver 베이스: carveEllipsoid/carveBlock/canReach/getCarveState (A2)
 *  - CaveWorldCarver (A3) / CanyonWorldCarver (A4)
 *  - 카버-aquifer: computeSubstance(SinglePointContext, 0.0) — barrier 서브
 *    트리에 ctx-민감 래퍼가 없어 SP==BLOCK 비트 동일 (A5 §4)
 *  - CarvingMask 비트 레이아웃 + ProtoChunk.setBlockState 하이트맵 (A6)
 * 파일 I/O 없음 — 호출자가 JSON 을 파싱해 넘긴다 (ADR-003 D1). */

/* --- FloatProvider (A7 §5) — 카버 설정이 쓰는 3종만 --- */

enum { HC_FP_CONST = 0, HC_FP_UNIFORM, HC_FP_TRAPEZOID };

typedef struct {
    uint8_t kind;
    float   a, b, c; /* const: a / uniform: [a,b) / trapezoid: (a,b,plateau c) */
} hc_fprov_t;

/* --- 컴파일된 ConfiguredWorldCarver --- */

enum { HC_CARVER_CAVE = 0, HC_CARVER_CANYON };

typedef struct {
    uint8_t kind;
    float   probability;
    /* HeightProvider (설정 4종 전부 minecraft:uniform — A7 §6.1). 앵커는
     * 컴파일 시 해석 (오버월드 minGenY -64 / genDepth 384, A7 §6.2/6.3). */
    int32_t    y_min, y_max;
    hc_fprov_t y_scale;
    int32_t    lava_level; /* resolveY 결과 (above_bottom 8 → -56) */
    /* replaceable 태그 → 내부 블록 id 비트셋. 테이블에 없는 블록은 우리
     * 스테이지가 생성할 수 없으므로 멤버십이 무의미 — 건너뛴다. */
    uint64_t replaceable[(HC_B_COUNT + 63) / 64];
    /* cave (A3 §8) */
    hc_fprov_t horiz_radius_mult, vert_radius_mult, floor_level;
    /* canyon (A4 §8) */
    hc_fprov_t vertical_rotation;
    hc_fprov_t distance_factor, thickness, horiz_radius_factor;
    int32_t    width_smoothness;
    float      vert_radius_default_factor, vert_radius_center_factor;
} hc_carver_t;

/* configured_carver JSON ({"type", "config"}) 하나를 컴파일. tags 는
 * reference/tags/block 테이블 ("minecraft:이름" → JSON, "#..." 재귀 확장).
 * 실패 -1 + *err (정적 문자열). */
int hc_carver_init(hc_carver_t *cv, const hc_json_t *carver_json,
                   const hc_df_source_t *tags, int32_t n_tags,
                   const char **err);

/* --- CarvingMask (A6 §1): BitSet(256*height), 인덱스
 * x | z<<4 | (y-minY)<<8. 워드 수 = HC_BLOCKS/64. --- */
enum { HC_CARVING_MASK_WORDS = HC_BLOCKS / 64 };

/* Mth.sin/cos(double)→float — 65536 float 테이블 (A7 §4.1). 테이블은
 * 첫 사용 전에 hc_mth_trig_init 으로 생성한다 (idempotent). 단위 게이트:
 * golden/rng/mth_sin_table.txt (tests/unit/test_carvers_unit.c). */
void         hc_mth_trig_init(void);
float        hc_mth_sin(double x);
float        hc_mth_cos(double x);
const float *hc_mth_sin_table(void); /* [65536], 테스트 대조용 */

/* WorldgenRandom.setLargeFeatureSeed (A1 §4.1): setSeed(seed) → a=nextLong
 * → b=nextLong → setSeed(cx*a ^ cz*b ^ seed). 단위 게이트 노출. */
void hc_lcg_set_large_feature_seed(hc_lcg_t *r, int64_t seed, int32_t cx,
                                   int32_t cz);

/* 카버 실행 환경 — carvers.c(알고리즘) 와 gen_carvers_stage.c(17x17
 * 오케스트레이션) 가 공유하는 인자 다발. cv 는 루프가 카버마다 바꾼다. */
typedef struct {
    hc_chunk_t            *chunk; /* 타깃 (중앙) 청크 */
    hc_noise_chunk_t      *nc;    /* 같은 청크의 noise chunk (aquifer) */
    hc_surface_t          *surf;  /* topMaterial (grass 복구) */
    const hc_biome_view_t *view;
    const hc_carver_t     *cv;
    uint64_t              *mask; /* CarvingMask 워드 (HC_CARVING_MASK_WORDS) */
} hc_carve_env_t;

/* CaveWorldCarver.carve / CanyonWorldCarver.carve — rng 는
 * setLargeFeatureSeed + isStartChunk 를 지난 외부 WorldgenRandom 상태.
 * (scx, scz) = 소스 청크 (A1 §4 의 sourcePos). */
void hc_cave_carve(hc_carve_env_t *e, hc_lcg_t *rng, int32_t scx,
                   int32_t scz);
void hc_canyon_carve(hc_carve_env_t *e, hc_lcg_t *rng, int32_t scx,
                     int32_t scz);

/* NoiseBasedChunkGenerator.applyCarvers — chunk 는 05_surface 완료 상태.
 * carvers 는 carverIndex 순 (오버월드 전 바이옴 공통: cave,
 * cave_extra_underground, canyon — 소스 청크 바이옴 게이트는 값-중립이라
 * 생략, A1 §5.1/§11). nc 는 같은 청크의 noise chunk (aquifer 재사용,
 * A5 §2). surf/view 는 grass 복구 경로 (topMaterial). mask 는 호출자
 * 제공, zero-init 필수 (Task 9 가 이어받는다). */
void hc_gen_carvers_stage(hc_chunk_t *chunk, hc_noise_chunk_t *nc,
                          hc_surface_t *surf, const hc_biome_view_t *view,
                          int64_t seed, const hc_carver_t *carvers,
                          int32_t n_carvers, uint64_t *mask);

#endif /* HC_CARVERS_H */
