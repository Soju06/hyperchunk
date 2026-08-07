#include "hc_sha256.h"

#include <stdatomic.h>
#include <string.h>

/* FIPS 180-4 상수 (fractional parts of cube roots of primes 2..311) */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(uint32_t h[8], const uint8_t p[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void hc_sha256_blocks_sw(uint32_t h[8], const uint8_t *p, size_t nblocks) {
    while (nblocks--) {
        sha256_block(h, p);
        p += 64;
    }
}

/* ---- 백엔드 디스패치 (P2-5) — df_isa.c 와 동일 패턴: 검출은 멱등이라
 * 중복 실행 무해, relaxed 원자 접근만 보장 (TSan 클린). ---- */

#if defined(__x86_64__) || defined(__i386__)
static int detect_sha_ni(void) {
    /* 커널 구조: shuffle_epi8/alignr(SSSE3) + blend_epi16(SSE4.1) 사용 —
     * sha 지원 CPU 는 전부 갖지만 검출은 보수적으로 둘 다 본다. */
    return __builtin_cpu_supports("sha") && __builtin_cpu_supports("sse4.1");
}
#else
static int detect_sha_ni(void) { return 0; }
#endif

static _Atomic int g_sha_ni = -1; /* -1 = 미검출/auto */

int hc_sha256_ni_active(void) {
    int v = atomic_load_explicit(&g_sha_ni, memory_order_relaxed);
    if (v < 0) {
        v = detect_sha_ni();
        atomic_store_explicit(&g_sha_ni, v, memory_order_relaxed);
    }
    return v;
}

void hc_sha256_force(int backend) {
    if (backend < 0) {
        atomic_store_explicit(&g_sha_ni, -1, memory_order_relaxed);
        return;
    }
    if (backend && !detect_sha_ni())
        backend = 0; /* SIGILL 방지 강등 (hc_isa_force 규약) */
    atomic_store_explicit(&g_sha_ni, backend, memory_order_relaxed);
}

void hc_sha256(const void *data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    void (*blocks)(uint32_t[8], const uint8_t *, size_t) =
        hc_sha256_ni_active() ? hc_sha256_blocks_ni : hc_sha256_blocks_sw;
    const uint8_t *p = data;
    size_t         rem = len;
    size_t         nb = rem / 64;
    if (nb) {
        blocks(h, p, nb);
        p += nb * 64;
        rem -= nb * 64;
    }
    uint8_t tail[128];
    memcpy(tail, p, rem);
    tail[rem] = 0x80;
    size_t pad = (rem + 1 + 8 <= 64) ? 64 : 128;
    memset(tail + rem + 1, 0, pad - rem - 1 - 8);
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        tail[pad - 1 - i] = (uint8_t)(bits >> (8 * i));
    blocks(h, tail, pad / 64);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

/* Guava HashFunction.hashLong 은 리틀엔디언 8바이트를 해시하고,
 * HashCode.asLong 은 다이제스트 앞 8바이트를 리틀엔디언으로 읽는다. */
int64_t hc_biome_obfuscate_seed(int64_t seed) {
    uint8_t in[8], dg[32];
    for (int i = 0; i < 8; i++)
        in[i] = (uint8_t)((uint64_t)seed >> (8 * i));
    hc_sha256(in, 8, dg);
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | dg[i];
    return (int64_t)v;
}
