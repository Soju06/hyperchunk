#ifndef HC_BIOME_H
#define HC_BIOME_H

#include <stdint.h>

#include "../include/hc_arena.h"

/* 바이옴 줌/레지스트리/온도 — 내부 전용 (core/src). 시맨틱은 26.2
 * 바이트코드 (javap) 기준: BiomeManager.getBiome / getFiddledDistance /
 * getFiddle / LinearCongruentialGenerator.next (A6 §1-2),
 * Biome.getHeightAdjustedTemperature / coldEnoughToSnow (A6 §4),
 * PerlinSimplexNoise / SimplexNoise (A6 §5-6). 검증:
 * golden/rng/surface_seed*.txt 의 zoomed_biomes 벡터
 * (tests/unit/test_biome_zoom.c).
 *
 * 바이옴 '생성'(multi-noise climate sampler)은 별도 태스크다 — 여기는
 * 저장된 쿼트 바이옴 위의 좌표 변환 + 이름/기후 테이블뿐이다. */

/* BiomeManager.getBiome(pos) 의 좌표 선택: (x-2, y-2, z-2) 기준 쿼트 8개
 * 후보 중 fiddled distance 최소(선착순 동률 우승)를 골라 쿼트 좌표를
 * 돌려준다. 호출자가 그 쿼트를 자기 소스에서 조회한다 (ChunkAccess 의
 * 쿼트 y 클램프는 소스 책임). zoom_seed = hc_biome_obfuscate_seed(seed). */
void hc_biome_zoom(int64_t zoom_seed, int32_t x, int32_t y, int32_t z,
                   int32_t *qx, int32_t *qy, int32_t *qz);

/* --- 바이옴 레지스트리 (intern id ↔ 이름 + 기후) ---
 *
 * hc_chunk_t.biomes / hc_biome_view_t.ids 가 담는 uint16 id 의 원장.
 * Holder 정체성 비교(HolderSet.contains)가 레지스트리 id 동일성과 1:1 인
 * 전제 (A3 §2 — 바닐라 홀더는 인턴됨). 기후는 temperature 조건과
 * frozen-ocean melt 검사가 읽는다 (reference/biome_climate-26.2.json). */

enum { HC_BIOME_MAX = 128 };
enum { HC_BIOME_TEMP_MOD_NONE = 0, HC_BIOME_TEMP_MOD_FROZEN = 1 };

typedef struct {
    hc_arena_t *arena;
    const char *names[HC_BIOME_MAX]; /* "minecraft:jungle" (arena 사본) */
    float       temperature[HC_BIOME_MAX]; /* NaN = 기후 미설정 (fail-loud) */
    uint8_t     temp_modifier[HC_BIOME_MAX];
    int32_t     count;
} hc_biome_reg_t;

void hc_biome_reg_init(hc_biome_reg_t *r, hc_arena_t *arena);
/* get-or-add. len 은 name 길이 (NUL 종단 불필요). 실패(-1)는 arena 소진
 * 또는 HC_BIOME_MAX 초과뿐. */
int32_t hc_biome_intern(hc_biome_reg_t *r, const char *name, int32_t len);
/* 조회 전용 — 없으면 -1 */
int32_t hc_biome_find(const hc_biome_reg_t *r, const char *name, int32_t len);
void    hc_biome_set_climate(hc_biome_reg_t *r, int32_t id, float temperature,
                             uint8_t temp_modifier);

/* --- 리전 쿼트 바이옴 뷰 (WorldGenRegion + BiomeManager 대응) ---
 *
 * surface 스테이지의 biomeGetter: 줌 → 쿼트 y 클램프 → 그리드 조회.
 * ids 레이아웃은 [qy][qz][qx] (biomes 덤프/quart_biomes 섹션과 동일).
 * x/z 는 그리드 범위 안이어야 한다 (줌은 블록에서 ±1 청크를 벗어나지
 * 않으므로 호출자가 3x3 이상을 공급하면 충분) — 범위 밖은 assert. */
typedef struct {
    int32_t         qx0, qz0, nxz; /* 월드 쿼트 원점 + 한 변 크기 */
    int32_t         qy0, ny;       /* 26.2 오버월드: -16, 96 */
    const uint16_t *ids;           /* ny * nxz * nxz */
    int64_t         zoom_seed;     /* hc_biome_obfuscate_seed(world seed) */
} hc_biome_view_t;

uint16_t hc_biome_view_get(const hc_biome_view_t *v, int32_t x, int32_t y,
                           int32_t z);

/* --- Biome 온도 (coldEnoughToSnow / melt 검사) ---
 *
 * Biome.getHeightAdjustedTemperature: y > seaLevel+17 에서 float 경로의
 * TEMPERATURE_NOISE(PerlinSimplexNoise, LegacyRandomSource(1234), 옥타브
 * {0}) 를 더한다. FROZEN modifier 는 FROZEN_TEMPERATURE_NOISE(3456,
 * {-2,-1,0}) + BIOME_INFO_NOISE(2345, {0}) 를 쓴다. 전부 월드 시드와
 * 무관한 클래스-정적 노이즈다 (biome_temp.c 가 지연 초기화).
 * 바닐라의 1024-엔트리 온도 캐시는 값-투명이라 생략한다 (A6 §4.3). */
float hc_biome_temperature(const hc_biome_reg_t *r, int32_t id, int32_t x,
                           int32_t y, int32_t z, int32_t sea_level);
/* coldEnoughToSnow = !(getTemperature >= 0.15f) */
int hc_biome_cold_enough_to_snow(const hc_biome_reg_t *r, int32_t id,
                                 int32_t x, int32_t y, int32_t z,
                                 int32_t sea_level);
/* shouldMeltFrozenOceanIcebergSlightly = getTemperature > 0.1f (엄격) */
int hc_biome_should_melt_iceberg(const hc_biome_reg_t *r, int32_t id,
                                 int32_t x, int32_t y, int32_t z,
                                 int32_t sea_level);

/* Biome.BIOME_INFO_NOISE.getValue(x, z, false) — noise_threshold_count
 * 배치 modifier 가 읽는 클래스-정적 PerlinSimplexNoise (시드 2345,
 * 옥타브 {0}; biome_temp.c 의 g_info_noise 재사용) */
double hc_biome_info_noise(double x, double z);

#endif /* HC_BIOME_H */
