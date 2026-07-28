/* Golden-vector test for vanilla 26.2 ImprovedNoise (Plan Task 5).
 *
 * golden/rng/perlin_seed1234567890.txt 는 실제 26.2 서버에서 덤프된 값이며
 * 여기서는 파싱만 한다 — 재생성하지 않는다. 샘플 좌표도 파일에서 읽는다.
 *
 * 검증 순서가 진단력을 만든다:
 *   1) perm 테이블 256 엔트리 전부  — RNG 소비 순서/시딩 버그를 격리
 *   2) xo/yo/zo 비트정확            — nextDouble()*256 소비 3회
 *   3) 노이즈 샘플 비트정확          — 여기'만' 어긋나면 샘플링 수학
 *      (floor/lerp/smoothstep/gradient) 버그다. 시딩이 아니다.
 *
 * double 은 십진 문자열이 아니라 IEEE-754 비트(bits=0x...)로 비교한다. */

#include "hc_df.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PERM_N      256
#define MAX_SAMPLES 16
#define MIN_SAMPLES 3 /* golden 은 세 좌표를 싣는다 — 그 미만이면 공허 */

typedef struct {
    int      has_seed;
    int64_t  seed;
    int      has_xo, has_yo, has_zo;
    uint64_t xo_bits, yo_bits, zo_bits;
    int      n_samples;
    double   sx[MAX_SAMPLES], sy[MAX_SAMPLES], sz[MAX_SAMPLES];
    uint64_t sample_bits[MAX_SAMPLES];
    int      perm_len; /* 선언된 길이. 없으면 -1 */
    int      n_perm;
    int      perm[PERM_N];
} golden_t;

static int g_checks = 0;
static int g_fails = 0;

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
    g->perm_len = -1;

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot open golden file: %s\n", path);
        exit(2);
    }

    /* perm_table 라인은 256 엔트리 한 줄 (~1KB) — 넉넉히 잡는다 */
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        int64_t  sv;
        uint64_t bits;
        double   x, y, z;
        int      n;

        if (line[0] == '#' || blank(line)) continue;

        if (sscanf(line, "seed %" SCNd64, &sv) == 1) {
            g->seed = sv;
            g->has_seed = 1;
        } else if (sscanf(line, "xo %*s bits=0x%" SCNx64, &bits) == 1) {
            g->xo_bits = bits;
            g->has_xo = 1;
        } else if (sscanf(line, "yo %*s bits=0x%" SCNx64, &bits) == 1) {
            g->yo_bits = bits;
            g->has_yo = 1;
        } else if (sscanf(line, "zo %*s bits=0x%" SCNx64, &bits) == 1) {
            g->zo_bits = bits;
            g->has_zo = 1;
        } else if (sscanf(line, "noise(%lf, %lf, %lf) %*s bits=0x%" SCNx64,
                          &x, &y, &z, &bits) == 4) {
            if (g->n_samples >= MAX_SAMPLES) die(path, "sample overflow");
            g->sx[g->n_samples] = x;
            g->sy[g->n_samples] = y;
            g->sz[g->n_samples] = z;
            g->sample_bits[g->n_samples++] = bits;
        } else if (sscanf(line, "perm_table_length %d", &n) == 1) {
            g->perm_len = n;
        } else if (strncmp(line, "perm_table ", 11) == 0) {
            const char *s = line + 11;
            char *end;
            for (;;) {
                long v = strtol(s, &end, 10);
                if (end == s) break;
                if (v < 0 || v > 255) die(path, "perm value out of [0,255]");
                if (g->n_perm >= PERM_N) die(path, "perm overflow");
                g->perm[g->n_perm++] = (int)v;
                s = end;
            }
        } else {
            fprintf(stderr, "GOLDEN FORMAT ERROR %s: unrecognized line: %s",
                    path, line);
            exit(2);
        }
    }
    fclose(f);

    /* 공허 통과 방어: 필수 섹션 존재 + 선언 길이 일치 + 값이 실제 순열 */
    if (!g->has_seed) die(path, "missing seed");
    if (!g->has_xo || !g->has_yo || !g->has_zo) die(path, "missing xo/yo/zo");
    if (g->perm_len != PERM_N) die(path, "perm_table_length != 256");
    if (g->n_perm != PERM_N) die(path, "perm_table entry count != 256");
    if (g->n_samples < MIN_SAMPLES) die(path, "fewer than 3 noise samples");
    {
        unsigned char seen[PERM_N] = {0};
        for (int i = 0; i < PERM_N; i++) {
            if (seen[g->perm[i]]) die(path, "perm_table is not a permutation");
            seen[g->perm[i]] = 1;
        }
    }
}

static void check_dbl_bits(const char *path, const char *what, int idx,
                           double got, uint64_t want_bits, const char *hint) {
    uint64_t got_bits;
    memcpy(&got_bits, &got, sizeof got_bits);
    g_checks++;
    if (got_bits != want_bits) {
        g_fails++;
        fprintf(stderr,
                "FAIL %s %s[%d]: got %.17g (0x%016" PRIx64 ") want bits 0x%016" PRIx64 "%s%s\n",
                path, what, idx, got, got_bits, want_bits,
                hint && *hint ? "\n      " : "", hint ? hint : "");
    }
}

int main(int argc, char **argv) {
    const char *path =
        argc > 1 ? argv[1] : "golden/rng/perlin_seed1234567890.txt";

    golden_t g;
    parse_golden(path, &g);

    hc_perlin_t p;
    hc_perlin_init(&p, g.seed);

    /* 1) perm 먼저. 여기가 어긋나면 시딩/RNG 소비 순서 버그다 —
     *    샘플 결과를 보기 전에 원인 계층을 확정한다. */
    int perm_fails = 0;
    for (int i = 0; i < PERM_N; i++) {
        g_checks++;
        if ((int)p.perm[i] != g.perm[i]) {
            g_fails++;
            if (perm_fails++ < 8)
                fprintf(stderr, "FAIL %s perm[%d]: got %d want %d\n",
                        path, i, (int)p.perm[i], g.perm[i]);
        }
    }
    if (perm_fails)
        fprintf(stderr,
                "HINT %s: perm mismatch (%d entries) => seeding / RNG "
                "consumption-order bug (xo,yo,zo then nextInt(256-i)), "
                "not sampling math\n",
                path, perm_fails);

    /* 2) 원점 오프셋 */
    check_dbl_bits(path, "xo", 0, p.xo, g.xo_bits, NULL);
    check_dbl_bits(path, "yo", 0, p.yo, g.yo_bits, NULL);
    check_dbl_bits(path, "zo", 0, p.zo, g.zo_bits, NULL);
    int seeding_ok = (g_fails == 0);

    /* 3) 샘플. perm/오프셋이 전부 맞은 상태에서 여기만 어긋나면
     *    범인은 샘플링 수학이다 — 미래의 디버깅을 위해 명시한다. */
    for (int i = 0; i < g.n_samples; i++) {
        double got = hc_perlin_sample(&p, g.sx[i], g.sy[i], g.sz[i]);
        check_dbl_bits(path, "noise", i, got, g.sample_bits[i],
                       seeding_ok
                           ? "perm + xo/yo/zo all matched => bug is in the "
                             "sampling math (floor/lerp/smoothstep/gradient "
                             "table), NOT in seeding/RNG order"
                           : "");
    }

    printf("test_perlin: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
