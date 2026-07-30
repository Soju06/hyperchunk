#ifndef HC_SHA256_H
#define HC_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* FIPS 180-4 SHA-256. 내부 전용 (core/src) — 공개 ABI 아님.
 *
 * 용도는 암호화가 아니라 바닐라 BiomeManager.obfuscateSeed 재현이다:
 * Guava Hashing.sha256().hashLong(seed).asLong() — 시드 8바이트를
 * 리틀엔디언으로 해시하고 다이제스트 앞 8바이트를 리틀엔디언 long 으로
 * 읽는다. 여기가 어긋나면 바이옴 줌(지형 표면의 바이옴 경계)이 전부
 * 어긋난다. 검증: golden/rng/surface_seed*.txt 의 obfuscated_seed. */
void hc_sha256(const void *data, size_t len, uint8_t out[32]);

/* BiomeManager.obfuscateSeed(seed) */
int64_t hc_biome_obfuscate_seed(int64_t seed);

#endif /* HC_SHA256_H */
