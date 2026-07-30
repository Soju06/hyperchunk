#ifndef HC_BIOME_H
#define HC_BIOME_H

#include <stdint.h>

/* 바이옴 줌 — 내부 전용 (core/src). 시맨틱은 26.2 바이트코드 (javap)
 * BiomeManager.getBiome / getFiddledDistance / getFiddle /
 * LinearCongruentialGenerator.next 기준. 검증:
 * golden/rng/surface_seed*.txt 의 zoomed_biomes 벡터
 * (tests/unit/test_biome_zoom.c).
 *
 * 바이옴 '생성'(multi-noise climate sampler)은 별도 태스크다 — 여기는
 * 저장된 쿼트 바이옴 위의 좌표 변환뿐이다. */

/* BiomeManager.getBiome(pos) 의 좌표 선택: (x-2, y-2, z-2) 기준 쿼트 8개
 * 후보 중 fiddled distance 최소(선착순 동률 우승)를 골라 쿼트 좌표를
 * 돌려준다. 호출자가 그 쿼트를 자기 소스에서 조회한다 (ChunkAccess 의
 * 쿼트 y 클램프는 소스 책임). zoom_seed = hc_biome_obfuscate_seed(seed). */
void hc_biome_zoom(int64_t zoom_seed, int32_t x, int32_t y, int32_t z,
                   int32_t *qx, int32_t *qy, int32_t *qz);

#endif /* HC_BIOME_H */
