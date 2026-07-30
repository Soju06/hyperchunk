/* RFC 1321 부록 A.5 테스트 벡터 + 긴 입력(패딩 2블록 경계) 검증.
 * hc_md5 는 RandomSupport.seedFromHashOf (fork-by-string 시딩) 의
 * 기반이다 — 여기가 틀리면 모든 노이즈 인스턴스가 틀린다. */

#include "../../core/src/hc_md5.h"

#include <stdio.h>
#include <string.h>

static int g_fails = 0;

static void check(const char *in, size_t len, const char *want_hex) {
    uint8_t d[16];
    char hex[33];
    hc_md5(in, len, d);
    for (int i = 0; i < 16; i++)
        snprintf(hex + 2 * i, 3, "%02x", d[i]);
    if (strcmp(hex, want_hex) != 0) {
        g_fails++;
        fprintf(stderr, "FAIL md5(\"%.40s\"...): got %s want %s\n", in, hex,
                want_hex);
    }
}

int main(void) {
    check("", 0, "d41d8cd98f00b204e9800998ecf8427e");
    check("a", 1, "0cc175b9c0f1b6a831c399e269772661");
    check("abc", 3, "900150983cd24fb0d6963f7d28e17f72");
    check("message digest", 14, "f96b697d7cb7938d525a2f31aaf161d0");
    check("abcdefghijklmnopqrstuvwxyz", 26, "c3fcd3d76192e4007dfb496cca67e13b");
    check("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
          62, "d174ab98d277d9f5a5611c2c9f419d9f");
    check("1234567890123456789012345678901234567890"
          "1234567890123456789012345678901234567890",
          80, "57edf4a22be3c955ac49da2e2107b67a");
    /* 55/56/64 바이트 — 패딩이 1블록/2블록으로 갈리는 경계 */
    {
        char buf[64];
        memset(buf, 'x', sizeof buf);
        check(buf, 55, "04364420e25c512fd958a70738aa8f72");
        check(buf, 56, "668a72d5ba17f08e62dabcafad6db14b");
        check(buf, 64, "c1bb4f81d892b2d57947682aeb252456");
    }
    /* worldgen 노이즈 키 실물 — md5sum 대조로 고정 */
    check("minecraft:temperature", 21, "5c7e6b29735f0d7ff7d86f1bbc734988");

    printf("test_md5: %s\n", g_fails ? "FAIL" : "ok");
    return g_fails ? 1 : 0;
}
