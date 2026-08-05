/* WorldgenRandom-over-Xoroshiro 단위 게이트 (Plan Task 9a).
 *
 * 벡터 출처:
 *  - .hermes/notes/task9pre-order/A3 §6 — jar 실행 + 독립 파이썬 모델이
 *    비트 일치로 상호 검증한 교차 벡터 (데코 시드, 리시드 후 드로우,
 *    nextInt/nextFloat 연속성)
 *  - golden/rng/jdk_sincos.txt — 이 머신의 JDK 25 HotSpot Math.sin/cos
 *    스텁 출력. OreFeature 의 각도 sin/cos 는 JDK Math 호출이라 (task9a
 *    A3 §1) 우리 스텁 이식 hc_jdk_sin/cos (core/src/jdk_trig.c) 가 이
 *    덤프와 비트 일치해야 광맥 중심이 맞는다. */

#undef NDEBUG

#include "../../core/src/hc_features.h"
#include "../../core/src/hc_jdk_trig.h"

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails = 0;

#define CHECK(cond, ...)                          \
    do {                                          \
        if (!(cond)) {                            \
            fprintf(stderr, "FAIL: " __VA_ARGS__); \
            fprintf(stderr, "\n");                \
            g_fails++;                            \
        }                                         \
    } while (0)

static void test_decoration_seeds(void) {
    /* A3 §6.1 — 시드 1234567890, Xoroshiro 델리게이트 */
    static const struct {
        int32_t cx, cz;
        int64_t want;
    } V[] = {
        {0, 0, 1234567890LL},
        {1, 0, -7060597322161768638LL},
        {-1, 0, 7060597322161768610LL},
        {0, 1, 1296893547007966818LL},
        {1, 1, -5763703776066719598LL},
        {-1, -1, 5763703776066719506LL},
        {10, -20, -4310123779316098254LL},
    };
    for (size_t i = 0; i < sizeof V / sizeof V[0]; i++) {
        int64_t got =
            hc_features_decoration_seed(1234567890LL, V[i].cx, V[i].cz);
        CHECK(got == V[i].want,
              "deco seed (%d,%d): got %" PRId64 " want %" PRId64, V[i].cx,
              V[i].cz, got, V[i].want);
    }
}

static void test_post_reseed_draws(void) {
    /* A3 §6.4 — setDecorationSeed(1234567890,16,0) 후 nextLong 3개 */
    hc_wgr_t r;
    int64_t  deco = hc_wgr_set_decoration_seed(&r, 1234567890LL, 16, 0);
    CHECK(deco == -7060597322161768638LL, "deco(16,0)");
    static const int64_t WANT[3] = {654184706891912386LL,
                                    2989823146729223195LL,
                                    7180858201083779893LL};
    for (int i = 0; i < 3; i++) {
        int64_t got = hc_wgr_next_long(&r);
        CHECK(got == WANT[i], "post-reseed nextLong[%d]: got %" PRId64, i,
              got);
    }
    /* setFeatureSeed(deco, 3, 6) → nextInt(16)=3, nextInt(16)=0,
     * nextFloat=0.6951269f */
    hc_wgr_set_feature_seed(&r, -7060597322161768638LL, 3, 6);
    int32_t a = hc_wgr_next_int(&r, 16);
    int32_t b = hc_wgr_next_int(&r, 16);
    float   f = hc_wgr_next_float(&r);
    CHECK(a == 3 && b == 0, "feature-seeded nextInt(16) pair: %d %d", a, b);
    CHECK(f == 0.6951269f, "feature-seeded nextFloat: %.9g", (double)f);
}

static void test_mth_helpers(void) {
    /* randomBetweenInclusive 는 lo==hi 에서도 드로우, Mth.nextInt 는 0
     * 드로우 — 상태 전진량으로 구분을 검증한다 */
    hc_wgr_t r1, r2;
    hc_wgr_set_seed(&r1, 42);
    hc_wgr_set_seed(&r2, 42);
    int32_t v = hc_mth_random_between_inclusive(&r1, 7, 7);
    CHECK(v == 7, "randomBetweenInclusive degenerate value");
    (void)hc_wgr_next_long(&r1);
    int32_t w = hc_mth_next_int_range(&r2, 7, 7);
    CHECK(w == 7, "Mth.nextInt degenerate value");
    (void)hc_wgr_next_int(&r2, 1); /* r1 의 nextInt(1) 만큼 따라잡기 */
    (void)hc_wgr_next_long(&r2);
    CHECK(hc_wgr_next_long(&r1) == hc_wgr_next_long(&r2),
          "draw accounting: randomBetweenInclusive(7,7) must burn exactly "
          "one nextInt(1)");
}

static void test_jdk_sincos(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "SETUP: cannot open %s\n", path);
        exit(2);
    }
    char line[256];
    int  n = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        long long          k;
        unsigned long long in_bits, sin_bits, cos_bits;
        if (sscanf(line, "k %lld in=0x%llx sin=0x%llx cos=0x%llx", &k,
                   &in_bits, &sin_bits, &cos_bits) != 4) {
            fprintf(stderr, "SETUP: bad jdk_sincos line: %s", line);
            exit(2);
        }
        double in;
        memcpy(&in, &in_bits, 8);
        /* 재현: 입력 도메인 자체도 우리 float 경로와 일치해야 한다 */
        double our_in = (double)(((float)k * 0x1p-24f) * 3.1415927f);
        uint64_t our_in_bits;
        memcpy(&our_in_bits, &our_in, 8);
        CHECK(our_in_bits == in_bits, "sincos input domain k=%lld", k);
        double s = hc_jdk_sin(in), c = hc_jdk_cos(in);
        uint64_t sb, cb;
        memcpy(&sb, &s, 8);
        memcpy(&cb, &c, 8);
        if (sb != sin_bits || cb != cos_bits) {
            if (bad < 5)
                fprintf(stderr,
                        "FAIL: port vs JDK k=%lld sin %016" PRIx64
                        " vs %016llx cos %016" PRIx64 " vs %016llx\n",
                        k, sb, sin_bits, cb, cos_bits);
            bad++;
        }
        n++;
    }
    fclose(f);
    if (bad) {
        fprintf(stderr,
                "FAIL: hc_jdk_sin/cos mismatches JDK stub on %d/%d vectors — "
                "jdk_trig.c port broken\n",
                bad, n);
        g_fails += bad;
    }
    printf("jdk_sincos: %d vectors, %d mismatches\n", n, bad);
}

/* golden/rng/wgr_gaussian.txt — 실서버 WorldgenRandom.nextGaussian 399
 * 드로우 (Task 14, make_wgr_gaussian_golden.sh). 리시드 패턴이 홀수 드로우
 * 캐시를 물고 넘어가므로 Marsaglia 캐시의 setSeed-비리셋 지속 시맨틱 +
 * hc_jdk_log 경로까지 함께 게이트한다. */
static void test_wgr_gaussian(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "FAIL: cannot open %s\n", path);
        g_fails++;
        return;
    }
    hc_wgr_t r;
    int64_t deco = hc_wgr_set_decoration_seed(&r, 1234567890LL, 16, 32);
    char line[128];
    int  n = 0, bad = 0, i = 0, k = 0, per = 1;
    hc_wgr_set_feature_seed(&r, deco, 0, 7);
    while (fgets(line, sizeof line, f)) {
        unsigned long long want;
        if (sscanf(line, "g=0x%llx", &want) != 1)
            continue; /* 헤더 */
        if (k == per) { /* 다음 feature index 로 재시드 */
            i++;
            k = 0;
            per = 1 + (i % 3);
            hc_wgr_set_feature_seed(&r, deco, i, 7);
        }
        double   g = hc_wgr_next_gaussian(&r);
        uint64_t b;
        memcpy(&b, &g, 8);
        if (b != (uint64_t)want) {
            if (bad < 5)
                fprintf(stderr,
                        "FAIL: gaussian i=%d k=%d %016" PRIx64
                        " vs %016llx\n",
                        i, k, b, want);
            bad++;
        }
        k++;
        n++;
    }
    fclose(f);
    if (n != 399) {
        fprintf(stderr, "FAIL: gaussian golden has %d vectors (want 399)\n",
                n);
        g_fails++;
    }
    g_fails += bad;
    printf("wgr_gaussian: %d vectors, %d mismatches\n", n, bad);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_features_rng <golden_rng_dir>\n");
        return 2;
    }
    test_decoration_seeds();
    test_post_reseed_draws();
    test_mth_helpers();
    char path[1024];
    snprintf(path, sizeof path, "%s/jdk_sincos.txt", argv[1]);
    test_jdk_sincos(path);
    snprintf(path, sizeof path, "%s/wgr_gaussian.txt", argv[1]);
    test_wgr_gaussian(path);
    printf("test_features_rng: %d fails\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
