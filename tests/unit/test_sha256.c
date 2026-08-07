/* SHA-256 KAT + 백엔드 이중 대조 (P2-5 SHA-NI).
 *
 * 게이트 판정 해시(canonical)와 바이옴 obfuscateSeed 가 전부 hc_sha256
 * 하나를 쓰므로, SHA-NI 백엔드 추가 후에는 두 경로 모두 상시 검증돼야
 * 한다 (SHA-NI 호스트에서 소프트웨어 폴백이 미검증으로 남는 문제 —
 * test_df_x4 와 동일 논리):
 *  1) FIPS 180-4 KAT — 소프트웨어 강제 상시, SHA-NI 강제는 지원 호스트만
 *  2) 이중 백엔드 배터리 — 패딩/블록 경계 전 위상(길이 0..193) + 대형
 *     버퍼에서 두 백엔드 다이제스트 비트 동일
 *  3) hc_biome_obfuscate_seed 회귀 (golden/rng/surface_seed*.txt 고정값)
 * SHA-NI 부재 호스트에서는 1)(sw)+3) 만 실행 — 공허하지 않으므로
 * SKIP(77) 이 아니라 PASS 로 처리한다. */

#include "../../core/src/hc_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails = 0;

static void hex_of(const uint8_t d[32], char hex[65]) {
    for (int i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", d[i]);
}

static void kat(const char *label, const void *in, size_t len,
                const char *want_hex) {
    uint8_t d[32];
    char    hex[65];
    hc_sha256(in, len, d);
    hex_of(d, hex);
    if (strcmp(hex, want_hex) != 0) {
        g_fails++;
        fprintf(stderr, "FAIL sha256 KAT %s: got %s want %s\n", label, hex,
                want_hex);
    }
}

static void kat_suite(const char *backend) {
    char label[64];
#define KAT(name, in, len, want)                                            \
    do {                                                                    \
        snprintf(label, sizeof label, "%s[%s]", name, backend);             \
        kat(label, in, len, want);                                          \
    } while (0)
    KAT("empty", "", 0,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    KAT("abc", "abc", 3,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    /* NIST 2블록 벡터 (56바이트 — 패딩이 2블록 finalize 로 갈리는 경계) */
    KAT("nist2", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        56,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    /* 1,000,000 x 'a' (NIST 롱 벡터) — 벌크 블록 루프 검증 */
    {
        static char big[1000000];
        memset(big, 'a', sizeof big);
        KAT("million-a", big, sizeof big,
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd"
            "0");
    }
    /* 55/63/64/65 바이트 — 1블록/2블록 패딩 경계 (sw 상호 대조로 고정) */
#undef KAT
}

int main(void) {
    int ni_host = hc_sha256_ni_active();

    /* 1) KAT — 소프트웨어 강제 (폴백 경로 상시 검증) */
    hc_sha256_force(0);
    kat_suite("sw");
    /* SHA-NI 강제 (지원 호스트만 — 강등 없이 실제 ni 로 돌았는지 확인) */
    if (ni_host) {
        hc_sha256_force(1);
        if (!hc_sha256_ni_active()) {
            g_fails++;
            fprintf(stderr, "FAIL: force(1) demoted on sha_ni host\n");
        }
        kat_suite("ni");
    } else {
        fprintf(stderr,
                "note: host lacks SHA-NI — ni backend untested here "
                "(sw KAT still meaningful)\n");
    }

    /* 2) 이중 백엔드 배터리: 길이 0..193 (블록/패딩 전 위상) + 대형.
     * 결정론 LCG 로 채운 버퍼 — 두 백엔드 블록 함수로 같은 다이제스트. */
    if (ni_host) {
        enum { NMAX = 1 << 20 };
        static uint8_t buf[NMAX + 64];
        uint64_t       s = 0x9E3779B97F4A7C15ull;
        for (size_t i = 0; i < sizeof buf; i++) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            buf[i] = (uint8_t)(s >> 56);
        }
        static const size_t extra[] = {4096, 4097, 65535, NMAX, NMAX + 63};
        int checked = 0;
        for (size_t len = 0; len <= 193 + 5; len++) {
            size_t n = len <= 193 ? len : extra[len - 194];
            uint8_t da[32], db[32];
            hc_sha256_force(0);
            hc_sha256(buf, n, da);
            hc_sha256_force(1);
            hc_sha256(buf, n, db);
            if (memcmp(da, db, 32) != 0) {
                g_fails++;
                char ha[65], hb[65];
                hex_of(da, ha);
                hex_of(db, hb);
                fprintf(stderr, "FAIL sw/ni mismatch len=%zu\n  sw %s\n  ni %s\n",
                        n, ha, hb);
            }
            checked++;
        }
        /* 배터리 동결 (df_x4 규약): 194 위상 + 대형 5 */
        if (checked != 199) {
            fprintf(stderr, "FAIL battery count %d != 199\n", checked);
            return 2;
        }
    }

    /* 3) obfuscateSeed 회귀 — golden/rng/surface_seed1234567890.txt:3 */
    hc_sha256_force(-1); /* auto 복원 (프로세스 종료 전 위생) */
    if (hc_biome_obfuscate_seed(1234567890) != 7304622306158335831LL) {
        g_fails++;
        fprintf(stderr, "FAIL obfuscate_seed(1234567890) != golden\n");
    }

    printf("test_sha256: %s (ni_host=%d)\n", g_fails ? "FAIL" : "ok",
           ni_host);
    return g_fails ? 1 : 0;
}
