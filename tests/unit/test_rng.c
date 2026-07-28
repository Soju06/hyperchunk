/* Golden-vector test for the two vanilla RNGs (Plan Task 3).
 *
 * golden/rng/ 의 텍스트는 실제 26.2 서버 jar 에서 덤프된 값이며 여기서는
 * 파싱만 한다 — 재생성하지 않는다. 각 section 은 같은 시드로 새로 만든
 * 인스턴스에서 시작하므로, 테스트도 section 마다 재시딩한다.
 *
 * double 은 십진 문자열이 아니라 IEEE-754 비트(bits=0x...)로 비교한다. */

#include "hc_rng.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VEC 64

typedef struct {
    int64_t  seed;
    int      has_mixed_state;
    int64_t  state_mixed_lo, state_mixed_hi;
    int      n_longs;
    int64_t  longs[MAX_VEC];
    int      n_ints;
    int32_t  int_bounds[MAX_VEC];
    int32_t  int_vals[MAX_VEC];
    int      n_dbls;
    uint64_t dbl_bits[MAX_VEC];
    int      n_cross;
    int64_t  cross[MAX_VEC];
    /* section 헤더가 선언한 개수. 섹션이 없으면 -1 */
    int      exp_longs, exp_ints, exp_dbls, exp_cross;
} golden_t;

static int g_checks = 0;
static int g_fails = 0;

static void check_u64(const char *file, const char *what, int idx,
                      uint64_t got, int64_t want) {
    g_checks++;
    if (got != (uint64_t)want) {
        g_fails++;
        fprintf(stderr, "FAIL %s %s[%d]: got %" PRId64 " want %" PRId64 "\n",
                file, what, idx, (int64_t)got, want);
    }
}

static void check_i32(const char *file, const char *what, int32_t bound,
                      int32_t got, int32_t want) {
    g_checks++;
    if (got != want) {
        g_fails++;
        fprintf(stderr, "FAIL %s %s(%" PRId32 "): got %" PRId32 " want %" PRId32 "\n",
                file, what, bound, got, want);
    }
}

static void check_dbl_bits(const char *file, const char *what, int idx,
                           double got, uint64_t want_bits) {
    uint64_t got_bits;
    memcpy(&got_bits, &got, sizeof got_bits);
    g_checks++;
    if (got_bits != want_bits) {
        g_fails++;
        fprintf(stderr,
                "FAIL %s %s[%d]: got %.17g (0x%016" PRIx64 ") want bits 0x%016" PRIx64 "\n",
                file, what, idx, got, got_bits, want_bits);
    }
}

static void die(const char *path, const char *msg) {
    fprintf(stderr, "GOLDEN FORMAT ERROR %s: %s\n", path, msg);
    exit(2);
}

static int blank(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    return *s == '\0';
}

static void parse_golden(const char *path, golden_t *g) {
    memset(g, 0, sizeof *g);
    g->exp_longs = g->exp_ints = g->exp_dbls = g->exp_cross = -1;

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot open golden file: %s\n", path);
        exit(2);
    }

    char line[512];
    while (fgets(line, sizeof line, f)) {
        int      idx, n;
        int32_t  bound, ival;
        int64_t  v;
        uint64_t bits;
        char     word[8];

        if (line[0] == '#' || blank(line)) continue;

        if (sscanf(line, "seed %" SCNd64, &v) == 1) {
            g->seed = v;
        } else if (sscanf(line, "state_unmixed_lo %" SCNd64, &v) == 1 ||
                   sscanf(line, "state_unmixed_hi %" SCNd64, &v) == 1) {
            /* unmixed 는 시딩 중간값 — API 로 관측 불가, 문서용으로만 존재 */
        } else if (sscanf(line, "state_mixed_lo %" SCNd64, &v) == 1) {
            g->state_mixed_lo = v;
        } else if (sscanf(line, "state_mixed_hi %" SCNd64, &v) == 1) {
            g->state_mixed_hi = v;
            g->has_mixed_state = 1;
        } else if (sscanf(line, "section nextLong %d", &n) == 1) {
            g->exp_longs = n;
        } else if (sscanf(line, "section nextInt(256-i) %d", &n) == 1) {
            g->exp_ints = n;
        } else if (sscanf(line, "section nextDouble %d", &n) == 1) {
            g->exp_dbls = n;
        } else if (sscanf(line, "section legacyRandomSource_crosscheck %d", &n) == 1) {
            g->exp_cross = n;
        } else if (sscanf(line, "nextLong[%d] %" SCNd64, &idx, &v) == 2) {
            if (idx != g->n_longs || g->n_longs >= MAX_VEC) die(path, "nextLong order/overflow");
            g->longs[g->n_longs++] = v;
        } else if (sscanf(line, "nextInt(%" SCNd32 ") %" SCNd32, &bound, &ival) == 2) {
            if (g->n_ints >= MAX_VEC) die(path, "nextInt overflow");
            g->int_bounds[g->n_ints] = bound;
            g->int_vals[g->n_ints++] = ival;
        } else if (sscanf(line, "nextDouble[%d] %*s bits=0x%" SCNx64, &idx, &bits) == 2) {
            if (idx != g->n_dbls || g->n_dbls >= MAX_VEC) die(path, "nextDouble order/overflow");
            g->dbl_bits[g->n_dbls++] = bits;
        } else if (sscanf(line, "legacy_nextLong[%d] %" SCNd64 " matches_jdk %7s",
                          &idx, &v, word) == 3) {
            if (idx != g->n_cross || g->n_cross >= MAX_VEC) die(path, "crosscheck order/overflow");
            if (strcmp(word, "true") != 0) die(path, "crosscheck matches_jdk != true");
            g->cross[g->n_cross++] = v;
        } else {
            fprintf(stderr, "GOLDEN FORMAT ERROR %s: unrecognized line: %s", path, line);
            exit(2);
        }
    }
    fclose(f);

    /* 섹션 헤더가 선언한 개수와 실제 파싱된 벡터 수가 일치해야 한다.
     * 개수가 0 이면 테스트가 공허하게 통과하므로 하드 에러. */
    if (g->exp_longs <= 0 || g->exp_longs != g->n_longs) die(path, "nextLong count mismatch");
    if (g->exp_ints  <= 0 || g->exp_ints  != g->n_ints)  die(path, "nextInt count mismatch");
    if (g->exp_dbls  <= 0 || g->exp_dbls  != g->n_dbls)  die(path, "nextDouble count mismatch");
    if (g->exp_cross != -1 && g->exp_cross != g->n_cross) die(path, "crosscheck count mismatch");

    /* 섹션명 'nextInt(256-i)' 가 선언한 bound 스케줄을 강제한다. 파일의
     * bound 를 그대로 믿으면 nextInt(1)=0 같은 자명한 벡터로 바꿔치기해도
     * 62 checks 가 그대로 찍히며 rejection 경로 커버리지가 사라진다. */
    for (int i = 0; i < g->n_ints; i++)
        if (g->int_bounds[i] != 256 - i) die(path, "nextInt bound schedule != 256-i");
}

static void run_xoro(const char *path) {
    golden_t g;
    parse_golden(path, &g);
    if (!g.has_mixed_state) die(path, "missing state_mixed_lo/hi");

    hc_xoro_t r;

    /* 시딩(mixStafford13 업그레이드)이 맞는지 출력 이전에 검증 */
    hc_xoro_init(&r, g.seed);
    check_u64(path, "state_mixed_lo", 0, r.lo, g.state_mixed_lo);
    check_u64(path, "state_mixed_hi", 0, r.hi, g.state_mixed_hi);

    hc_xoro_init(&r, g.seed);
    for (int i = 0; i < g.n_longs; i++)
        check_u64(path, "nextLong", i, hc_xoro_next(&r), g.longs[i]);

    hc_xoro_init(&r, g.seed);
    for (int i = 0; i < g.n_ints; i++)
        check_i32(path, "nextInt", g.int_bounds[i],
                  hc_xoro_next_int(&r, g.int_bounds[i]), g.int_vals[i]);

    hc_xoro_init(&r, g.seed);
    for (int i = 0; i < g.n_dbls; i++)
        check_dbl_bits(path, "nextDouble", i, hc_xoro_next_double(&r), g.dbl_bits[i]);
}

static void run_lcg(const char *path) {
    golden_t g;
    parse_golden(path, &g);
    if (g.exp_cross <= 0) die(path, "missing legacyRandomSource_crosscheck section");

    hc_lcg_t r;

    hc_lcg_init(&r, g.seed);
    for (int i = 0; i < g.n_longs; i++)
        check_u64(path, "nextLong", i, (uint64_t)hc_lcg_next_long(&r), g.longs[i]);

    hc_lcg_init(&r, g.seed);
    for (int i = 0; i < g.n_ints; i++)
        check_i32(path, "nextInt", g.int_bounds[i],
                  hc_lcg_next_int(&r, g.int_bounds[i]), g.int_vals[i]);

    hc_lcg_init(&r, g.seed);
    for (int i = 0; i < g.n_dbls; i++)
        check_dbl_bits(path, "nextDouble", i, hc_lcg_next_double(&r), g.dbl_bits[i]);

    /* LegacyRandomSource == java.util.Random 스트림 동일성 벡터 */
    hc_lcg_init(&r, g.seed);
    for (int i = 0; i < g.n_cross; i++)
        check_u64(path, "legacy_nextLong", i, (uint64_t)hc_lcg_next_long(&r), g.cross[i]);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "golden/rng";
    char path[4096];

    snprintf(path, sizeof path, "%s/xoroshiro_seed1234567890.txt", dir);
    run_xoro(path);
    snprintf(path, sizeof path, "%s/lcg_seed1234567890.txt", dir);
    run_lcg(path);

    printf("test_rng: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
