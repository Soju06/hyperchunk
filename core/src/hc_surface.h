#ifndef HC_SURFACE_H
#define HC_SURFACE_H

#include "../include/hc_chunk.h"
#include "../include/hc_noise.h"
#include "../include/hc_rng.h"
#include "hc_biome.h"
#include "hc_blocks.h"
#include "hc_df_compile.h" /* hc_df_source_t (noise 파라미터 테이블) */
#include "hc_gen_noise.h"  /* hc_noise_chunk_t (preliminarySurfaceLevel) */
#include "hc_json.h"

/* 05_surface 스테이지 — 내부 전용 (core/src). 시맨틱은 전부 26.2
 * 바이트코드 기준 (.hermes/notes/task7-surface/A1..A6):
 *  - SurfaceSystem: buildSurface 워크 순서, getSurfaceDepth 공식,
 *    clay bands RNG 스크립트, badlands/frozen-ocean 확장 패스 (A1)
 *  - SurfaceRules$Context: updateXZ/updateY 카운터, lazy memo,
 *    getMinSurfaceLevel 의 float 분수 bilinear (A2)
 *  - 조건/룰 소스 코덱과 test/tryApply 시맨틱 (A3, A4)
 *  - RandomState: fork-by-string 노이즈/팩토리 시딩 (A5)
 * 파일 I/O 없음 — 호출자가 JSON 을 파싱해 넘긴다 (ADR-003 D1). */

/* 고정 상한 — 초과는 컴파일 에러로 fail-loud. 26.2 오버월드 surface_rule
 * 실측: 노드 468 (condition 149, block 111, sequence 53), 노이즈 8종. */
enum {
    HC_SURF_MAX_RULES = 1024,
    HC_SURF_MAX_CONDS = 512,
    HC_SURF_MAX_CHILDREN = 1024,
    HC_SURF_MAX_SAMPLERS = 32,
    HC_SURF_MAX_SEQ = 64, /* 단일 sequence 의 자식 수 상한 */
    HC_SURF_CLAY_BANDS = 192,
};

/* --- 조건 IR (SurfaceRules$ConditionSource 대응) --- */

enum {
    HC_SC_BIOME = 0,
    HC_SC_NOISE_THRESHOLD,
    HC_SC_VERTICAL_GRADIENT,
    HC_SC_Y_ABOVE,
    HC_SC_WATER,
    HC_SC_TEMPERATURE,
    HC_SC_STEEP,
    HC_SC_NOT,
    HC_SC_HOLE,
    HC_SC_ABOVE_PRELIM,
    HC_SC_STONE_DEPTH,
};

typedef struct {
    uint8_t kind;
    union {
        /* biome_is → 레지스트리 id 비트셋 (HolderSet.contains 와 동형) */
        struct {
            uint64_t bits[(HC_BIOME_MAX + 63) / 64];
        } biome;
        struct {
            int32_t sampler; /* hc_surface_t.samplers 인덱스 */
            double  min_threshold, max_threshold;
        } noise;
        /* 앵커는 컴파일 시 해석 (WorldGenerationContext 상수 — 26.2
         * 오버월드 minGenY -64 / genDepth 384). fork =
         * root.fromHashOf(random_name).forkPositional() (A3 §4b). */
        struct {
            int32_t        true_y, false_y; /* trueAtAndBelow/falseAtAndAbove */
            hc_xoro_fork_t fork;
        } vgrad;
        struct {
            int32_t anchor_y, mult; /* surface_depth_multiplier */
            uint8_t add_stone;      /* add_stone_depth */
        } y_above;
        struct {
            int32_t offset, mult;
            uint8_t add_stone;
        } water;
        struct {
            int32_t inner; /* conds 인덱스 */
        } not_;
        struct {
            int32_t offset, secondary_range;
            uint8_t add_surface, ceiling; /* surface_type == ceiling */
        } stone_depth;
    } u;
} hc_scond_t;

/* --- 룰 IR (SurfaceRules$RuleSource 대응) --- */

enum {
    HC_SR_SEQUENCE = 0,
    HC_SR_BLOCK,
    HC_SR_CONDITION, /* TestRule */
    HC_SR_BANDLANDS,
};

typedef struct {
    uint8_t kind;
    union {
        struct {
            int32_t first, count; /* children 풀의 연속 구간 */
        } seq;
        struct {
            uint16_t block;
        } block;
        struct {
            int32_t cond, then;
        } test;
    } u;
} hc_srule_t;

/* noise_threshold 의 공유 샘플러 (Context$1/$2 대응): (키, is_3d) 로
 * dedup, memo 는 2d=lastUpdateXZ / 3d=lastUpdateY 스탬프 (A2 §0.5).
 * memo 필드는 buildSurface 진입 시 리셋된다 — 청크당 새 Context 와 동형.
 * Phase 2 스레딩 시 memo 를 컨텍스트로 옮겨야 한다. */
typedef struct {
    hc_normal_noise_t noise;
    const char       *key;
    uint8_t           is3d;
    int64_t           memo_stamp;
    double            memo_val;
} hc_ssampler_t;

/* --- SurfaceSystem + 컴파일된 룰 트리 --- */

typedef struct {
    uint16_t default_block; /* settings.defaultBlock() — 참조 동일성 게이트 */
    int32_t  sea_level;
    int      legacy_random; /* useLegacyRandomSource (26.2 오버월드 false) */

    hc_xoro_fork_t noise_random; /* ROOT positional factory (A5 §1.3) */

    /* ctor 가 만드는 9개 노이즈 (A1 §1.1) */
    hc_normal_noise_t clay_bands_offset_noise;
    hc_normal_noise_t surface_noise, surface_secondary_noise;
    hc_normal_noise_t badlands_pillar_noise, badlands_pillar_roof_noise,
        badlands_surface_noise;
    hc_normal_noise_t iceberg_pillar_noise, iceberg_pillar_roof_noise,
        iceberg_surface_noise;
    uint16_t clay_bands[HC_SURF_CLAY_BANDS];

    hc_srule_t    *rules;
    int32_t        n_rules;
    hc_scond_t    *conds;
    int32_t        n_conds;
    int32_t       *children;
    int32_t        n_children;
    hc_ssampler_t *samplers;
    int32_t        n_samplers;
    int32_t        root_rule;

    const hc_biome_reg_t *biomes;
    /* 확장 패스 트리거 (레지스트리에 없으면 -1 — 어떤 컬럼도 매치 불가) */
    int32_t biome_eroded_badlands, biome_frozen_ocean, biome_deep_frozen_ocean;

    const char *err;
    char        errbuf[256];
} hc_surface_t;

/* settings = overworld-26.2.json 루트 (default_block / sea_level /
 * legacy_random_source / surface_rule 을 읽는다). noise_params 는
 * reference/noise 테이블 (df 컴파일러와 같은 규약). biomes 레지스트리에
 * 룰 트리의 biome_is 이름들을 intern 한다 (기후는 호출자가 설정).
 * 실패 -1 + s->err. */
int hc_surface_init(hc_surface_t *s, hc_arena_t *arena, int64_t seed,
                    const hc_json_t *settings,
                    const hc_df_source_t *noise_params,
                    int32_t n_noise_params, hc_biome_reg_t *biomes);

/* 단위 게이트용 내부 노출 (golden/rng/surface_seed*.txt 대조):
 * getSurfaceDepth / getSurfaceSecondary / getBand (A1 §3-4) */
int32_t  hc_surface_depth(const hc_surface_t *s, int32_t x, int32_t z);
double   hc_surface_secondary(const hc_surface_t *s, int32_t x, int32_t z);
uint16_t hc_surface_band(const hc_surface_t *s, int32_t x, int32_t y,
                         int32_t z);

/* SurfaceSystem.buildSurface — chunk 는 04_noise 완료 상태 (blocks +
 * WG 하이트맵), nc 는 같은 청크의 noise chunk (preliminarySurfaceLevel
 * 메모 공유), view 는 3x3 이상 이웃을 덮는 쿼트 바이옴 뷰.
 * s 는 샘플러 memo 때문에 non-const (진입 시 리셋). */
void hc_gen_surface_stage(hc_chunk_t *chunk, hc_noise_chunk_t *nc,
                          hc_surface_t *s, const hc_biome_view_t *view);

/* SurfaceSystem.topMaterial — 06_carvers 의 grass 복구 경로 전용
 * (task7 A1 §11). 호출마다 새 Context (모든 memo 리셋). 반환 블록 id
 * 또는 -1 (Optional.empty). */
int32_t hc_surface_top_material(hc_surface_t *s, hc_chunk_t *chunk,
                                hc_noise_chunk_t *nc,
                                const hc_biome_view_t *view, int32_t x,
                                int32_t y, int32_t z, int has_fluid);

#endif /* HC_SURFACE_H */
