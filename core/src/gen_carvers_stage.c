#include "hc_carvers.h"

/* 06_carvers 스테이지 — NoiseBasedChunkGenerator.applyCarvers 26.2
 * (javap) 의 오케스트레이션 절반. 근거는 .hermes/notes/task8-carvers/A1.
 *
 * 바닐라 흐름: WorldgenRandom(LegacyRandomSource) 1개를 스테이지 전체가
 * 재사용하며, (소스 청크, 카버 리스트 인덱스) 쌍마다 setLargeFeatureSeed
 * 로 재시드한다. 생성자 시드는 nanoTime 기반이지만 어떤 드로우도 재시드
 * 전에 일어나지 않으므로 무관 (A1 §4.1).
 *
 * 소스 청크 carverBiome 게이트 (biomeSource.getNoiseBiome(srcX<<2, 0,
 * srcZ<<2) 의 biome.getGenerationSettings().getCarvers()) 는 생략한다:
 * 오버월드 55개 바이옴 전부 (sulfur_caves 포함) 동일한 3-카버 리스트
 * ["cave", "cave_extra_underground", "canyon"] 를 쓰므로 값-중립이다
 * (A1 §5.1/§11 — 26.2 데이터팩 실측). 데이터팩이 바이옴별 카버 리스트를
 * 바꾸면 이 생략이 깨진다 — 그때는 ADR-003 D5 fallback 영역. */

/* WorldgenRandom.setLargeFeatureSeed (A7 §2.3): setSeed(seed) → 2×nextLong
 * → setSeed((long)cx*a ^ (long)cz*b ^ seed). setDecorationSeed 와 달리
 * |1 없음, XOR 결합, void. 랩어라운드 곱은 무부호로 계산한다. */
void hc_lcg_set_large_feature_seed(hc_lcg_t *r, int64_t seed, int32_t cx,
                                   int32_t cz) {
    hc_lcg_init(r, seed);
    int64_t  a = hc_lcg_next_long(r);
    int64_t  b = hc_lcg_next_long(r);
    uint64_t s = (uint64_t)(int64_t)cx * (uint64_t)a ^
                 (uint64_t)(int64_t)cz * (uint64_t)b ^ (uint64_t)seed;
    hc_lcg_init(r, (int64_t)s);
}

void hc_gen_carvers_stage(hc_chunk_t *chunk, hc_noise_chunk_t *nc,
                          hc_surface_t *surf, const hc_biome_view_t *view,
                          int64_t seed, const hc_carver_t *carvers,
                          int32_t n_carvers, uint64_t *mask) {
    hc_mth_trig_init();
    hc_carve_env_t env;
    env.chunk = chunk;
    env.nc = nc;
    env.surf = surf;
    env.view = view;
    env.cv = NULL;
    env.mask = mask;

    /* 17x17 스캔: x 외측 / z 내측, 둘 다 -8..8 오름차순 (A1 §4) */
    hc_lcg_t rng;
    for (int32_t ox = -8; ox <= 8; ox++) {
        for (int32_t oz = -8; oz <= 8; oz++) {
            int32_t scx = chunk->cx + ox;
            int32_t scz = chunk->cz + oz;
            for (int32_t idx = 0; idx < n_carvers; idx++) {
                /* 인덱스는 isStartChunk 결과와 무관하게 증가; 시드
                 * 항은 (worldSeed + idx) 이고 청크 좌표는 믹스에서
                 * 들어간다 (A1 §4.1/§9). ladd 는 mod 2^64 랩 —
                 * INT64_MAX 근방 시드에서 signed UB 가 되지 않도록
                 * 무부호로 더한다 (리뷰 확정: verify:orchestration). */
                hc_lcg_set_large_feature_seed(
                    &rng, (int64_t)((uint64_t)seed + (uint64_t)idx), scx,
                    scz);
                env.cv = &carvers[idx];
                /* isStartChunk: nextFloat() <= probability, 포함 비교
                 * (A3 §2 / A4 §2) */
                if (!(hc_lcg_next_float(&rng) <= env.cv->probability))
                    continue;
                if (env.cv->kind == HC_CARVER_CAVE)
                    hc_cave_carve(&env, &rng, scx, scz);
                else
                    hc_canyon_carve(&env, &rng, scx, scz);
            }
        }
    }
}
