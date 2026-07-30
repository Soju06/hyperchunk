#include "hc_md5.h"

#include <string.h>

/* RFC 1321 MD5 — 자체 구현 (의존성 0, ADR-003 D1).
 * 단일 호출 API 만 노출한다. 입력은 노이즈 키 문자열 수십 바이트라
 * 스트리밍 컨텍스트가 불필요하다. */

static uint32_t rotl32(uint32_t x, int k) {
    return (x << k) | (x >> (32 - k));
}

/* 라운드별 시프트 양 */
static const uint8_t S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

/* K[i] = floor(2^32 * |sin(i+1)|) — RFC 1321 상수표 */
static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static void md5_block(uint32_t st[4], const uint8_t blk[64]) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
        m[i] = (uint32_t)blk[4 * i] | ((uint32_t)blk[4 * i + 1] << 8) |
               ((uint32_t)blk[4 * i + 2] << 16) |
               ((uint32_t)blk[4 * i + 3] << 24);

    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        uint32_t t = d;
        d = c;
        c = b;
        b = b + rotl32(a + f + K[i] + m[g], S[i]);
        a = t;
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
}

void hc_md5(const void *data, size_t len, uint8_t out[16]) {
    uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const uint8_t *p = (const uint8_t *)data;
    size_t rem = len;

    while (rem >= 64) {
        md5_block(st, p);
        p += 64;
        rem -= 64;
    }

    /* 패딩: 0x80, 0*, 길이(비트, LE 64) — 마지막 1~2 블록 */
    uint8_t tail[128];
    memset(tail, 0, sizeof tail);
    memcpy(tail, p, rem);
    tail[rem] = 0x80;
    size_t tail_len = (rem < 56) ? 64 : 128;
    uint64_t bits = (uint64_t)len << 3;
    for (int i = 0; i < 8; i++)
        tail[tail_len - 8 + i] = (uint8_t)(bits >> (8 * i));
    md5_block(st, tail);
    if (tail_len == 128)
        md5_block(st, tail + 64);

    for (int i = 0; i < 4; i++) {
        out[4 * i] = (uint8_t)st[i];
        out[4 * i + 1] = (uint8_t)(st[i] >> 8);
        out[4 * i + 2] = (uint8_t)(st[i] >> 16);
        out[4 * i + 3] = (uint8_t)(st[i] >> 24);
    }
}
