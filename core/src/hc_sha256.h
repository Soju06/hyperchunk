#ifndef HC_SHA256_H
#define HC_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* FIPS 180-4 SHA-256. 내부 전용 (core/src) — 공개 ABI 아님.
 *
 * 용도는 암호화가 아니라 (a) 바닐라 BiomeManager.obfuscateSeed 재현:
 * Guava Hashing.sha256().hashLong(seed).asLong() — 시드 8바이트를
 * 리틀엔디언으로 해시하고 다이제스트 앞 8바이트를 리틀엔디언 long 으로
 * 읽는다. 여기가 어긋나면 바이옴 줌(지형 표면의 바이옴 경계)이 전부
 * 어긋난다. 검증: golden/rng/surface_seed*.txt 의 obfuscated_seed.
 * (b) 벤치/게이트의 canonical 판정 해시 (~47MB/런 — P2-5 SHA-NI 대상).
 *
 * P2-5: 블록 압축은 cpuid 디스패치 (소프트웨어 / SHA-NI). 두 백엔드는
 * 임의 입력에서 비트 동일 — test_sha256 (KAT+이중 배터리) 와
 * check_sha_equiv.sh 가 판정한다. */
void hc_sha256(const void *data, size_t len, uint8_t out[32]);

/* BiomeManager.obfuscateSeed(seed) */
int64_t hc_biome_obfuscate_seed(int64_t seed);

/* ---- 내부 백엔드 (직접 호출 금지 — 디스패치는 hc_sha256 이 한다) ----
 * 64바이트 블록열 압축 (패딩 없음). _ni 는 sha256_ni.c (-msha TU 격리,
 * 비-x86/부재 호스트에서 호출 금지 — abort 스텁). */
void hc_sha256_blocks_sw(uint32_t h[8], const uint8_t *p, size_t nblocks);
void hc_sha256_blocks_ni(uint32_t h[8], const uint8_t *p, size_t nblocks);

/* SHA-NI 가용 여부 (cpuid + _Atomic 1회 캐시 — df_isa.c 패턴) */
int hc_sha256_ni_active(void);

/* 테스트/벤치 전용 강제: -1 auto 재검출, 0 소프트웨어, 1 SHA-NI
 * (미지원 호스트에서 1 은 0 으로 강등 — hc_isa_force 와 동일 규약).
 * 생성 스레드 기동 전 호출 전제. */
void hc_sha256_force(int backend);

#endif /* HC_SHA256_H */
