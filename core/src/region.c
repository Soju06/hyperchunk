#include "hc_region.h"

#include <assert.h>
#include <string.h>

#define SECTOR 4096u

/* RFC 1950 adler32 — 자체 구현 (의존성 0, ADR-003 D1) */
static uint32_t adler32(const uint8_t *d, size_t n) {
    uint32_t a = 1, b = 0;
    while (n > 0) {
        /* 5552 = adler32 모듈러 지연 한계 (zlib 관례) */
        size_t block = n > 5552 ? 5552 : n;
        n -= block;
        while (block--) {
            a += *d++;
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

/* zlib 컨테이너 + stored-block DEFLATE. 반환: 쓴 길이, 초과 시 -1.
 * 출력 상한: 2(헤더) + n + 5*ceil(n/65535) + 4(adler) + 5(빈 입력 블록) */
static ptrdiff_t zlib_store(const uint8_t *in, size_t n, uint8_t *out,
                            size_t cap) {
    size_t blocks = n / 65535u + 1; /* 마지막 (부분/빈) 블록 포함 */
    size_t need = 2 + n + 5 * blocks + 4;
    if (cap < need)
        return -1;
    uint8_t *p = out;
    /* CMF=0x78 (deflate, 32K 윈도), FLG=0x01 ((CMF<<8|FLG)%31==0, 레벨 0) */
    *p++ = 0x78;
    *p++ = 0x01;
    size_t off = 0;
    do {
        size_t   chunk = n - off > 65535u ? 65535u : n - off;
        uint16_t len = (uint16_t)chunk;
        int      final = (off + chunk == n);
        *p++ = (uint8_t)(final ? 0x01 : 0x00); /* BFINAL | BTYPE=00 (stored) */
        *p++ = (uint8_t)(len & 0xFF);          /* LEN 리틀엔디언 */
        *p++ = (uint8_t)(len >> 8);
        *p++ = (uint8_t)(~len & 0xFF); /* NLEN = ~LEN */
        *p++ = (uint8_t)(~len >> 8);
        memcpy(p, in + off, chunk);
        p += chunk;
        off += chunk;
    } while (off < n);
    uint32_t ad = adler32(in, n);
    *p++ = (uint8_t)(ad >> 24);
    *p++ = (uint8_t)(ad >> 16);
    *p++ = (uint8_t)(ad >> 8);
    *p++ = (uint8_t)ad;
    return p - out;
}

static void put_u32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

ptrdiff_t hc_region_write(const hc_region_chunk_t *chunks, int n,
                          uint32_t timestamp, uint8_t *out, size_t cap) {
    if (cap < 2 * SECTOR)
        return -1;
    memset(out, 0, 2 * SECTOR);
    size_t off = 2 * SECTOR;
    for (int i = 0; i < n; i++) {
        const hc_region_chunk_t *c = &chunks[i];
        assert(c->x >= 0 && c->x < 32 && c->z >= 0 && c->z < 32);
        int idx = c->x + c->z * 32;
        if (off + 5 > cap)
            return -1;
        ptrdiff_t zn = zlib_store(c->payload, c->len, out + off + 5,
                                  cap - off - 5);
        if (zn < 0)
            return -1;
        put_u32be(out + off, (uint32_t)zn + 1); /* length = 압축길이 + 1 */
        out[off + 4] = 2;                       /* compression = zlib */
        size_t entry = 5 + (size_t)zn;
        size_t sectors = (entry + SECTOR - 1) / SECTOR;
        assert(sectors <= 255);
        put_u32be(out + idx * 4,
                  (uint32_t)(off / SECTOR) << 8 | (uint32_t)sectors);
        put_u32be(out + SECTOR + idx * 4, timestamp);
        size_t padded = off + sectors * SECTOR;
        if (padded > cap)
            return -1;
        memset(out + off + entry, 0, padded - off - entry);
        off = padded;
    }
    return (ptrdiff_t)off;
}
