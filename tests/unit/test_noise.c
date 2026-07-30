/* Golden-vector tests for the vanilla 26.2 noise substrate (Plan Task 6a).
 *
 * 하나의 바이너리가 모드 인자로 네 검증을 나눠 실행한다:
 *   fork    golden/rng/fork_seed*.txt    — forkPositional/fromHashOf/at/fork
 *   octaves golden/rng/octaves_seed*.txt — PerlinNoise(멀티옥타브) 내부/샘플
 *   normal  golden/rng/octaves_seed*.txt — NormalNoise valueFactor/샘플
 *   blended golden/rng/router_seed*.txt  — BlendedNoise(레거시 시딩) 전 좌표
 *
 * golden 텍스트는 실제 26.2 서버에서 덤프된 값이며 여기서는 파싱만 한다 —
 * 재생성하지 않는다. double 은 십진이 아니라 IEEE-754 비트로 비교하고,
 * 파서는 스코프 안의 미인식 라인에서 즉사한다 (조용한 스킵 = false-PASS
 * 채널, test_perlin.c 와 같은 방침). */

#include "hc_noise.h"
#include "hc_rng.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_fails = 0;

static void die(const char *path, const char *msg) {
    fprintf(stderr, "GOLDEN FORMAT ERROR %s: %s\n", path, msg);
    exit(2);
}

static void die_line(const char *path, const char *line) {
    fprintf(stderr, "GOLDEN FORMAT ERROR %s: unrecognized line: %s", path,
            line);
    exit(2);
}

static int blank(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    return *s == '\0';
}

static void check_u64(const char *what, const char *key, int idx,
                      uint64_t got, uint64_t want) {
    g_checks++;
    if (got != want) {
        g_fails++;
        fprintf(stderr,
                "FAIL %s %s[%d]: got %" PRId64 " want %" PRId64 "\n", what,
                key, idx, (int64_t)got, (int64_t)want);
    }
}

/* ---------------- fork: positional fork / fromHashOf / at ---------------- */

static int run_fork(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot open golden file: %s\n", path);
        exit(2);
    }

    char line[16384];
    int64_t seed = 0;
    int has_seed = 0;
    /* 섹션 선언 개수 vs 실제 검증한 벡터 수 — 공허 통과 방어 */
    int want_hash = -1, want_at = -1, want_fork = -1;
    int n_hash = 0, n_at = 0, n_fork = 0;
    hc_xoro_t fork_base; /* section fork 는 base 를 재생성하지 않고 이어 쓴다 */
    memset(&fork_base, 0, sizeof fork_base);

    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || blank(line))
            continue;

        int64_t sv;
        int x, y, z, idx, n;
        char key[256];
        int64_t v[4];

        if (sscanf(line, "seed %" SCNd64, &sv) == 1) {
            seed = sv;
            has_seed = 1;
        } else if (sscanf(line, "section fromHashOf %d", &n) == 1) {
            want_hash = n;
        } else if (sscanf(line, "section at %d", &n) == 1) {
            want_at = n;
        } else if (sscanf(line, "section fork %d", &n) == 1) {
            want_fork = n;
            hc_xoro_init(&fork_base, seed);
        } else if (sscanf(line,
                          "fromHashOf \"%255[^\"]\" %" SCNd64 " %" SCNd64
                          " %" SCNd64 " %" SCNd64,
                          key, &v[0], &v[1], &v[2], &v[3]) == 5) {
            if (!has_seed)
                die(path, "fromHashOf before seed");
            hc_xoro_t base, r;
            hc_xoro_fork_t fk;
            hc_xoro_init(&base, seed);
            hc_xoro_fork_positional(&base, &fk);
            hc_xoro_from_hash_of(&fk, key, &r);
            for (int i = 0; i < 4; i++)
                check_u64("fromHashOf", key, i, hc_xoro_next(&r),
                          (uint64_t)v[i]);
            n_hash++;
        } else if (sscanf(line,
                          "at(%d,%d,%d) %" SCNd64 " %" SCNd64,
                          &x, &y, &z, &v[0], &v[1]) == 5) {
            if (!has_seed)
                die(path, "at before seed");
            hc_xoro_t base, r;
            hc_xoro_fork_t fk;
            hc_xoro_init(&base, seed);
            hc_xoro_fork_positional(&base, &fk);
            hc_xoro_at(&fk, x, y, z, &r);
            char what[64];
            snprintf(what, sizeof what, "at(%d,%d,%d)", x, y, z);
            for (int i = 0; i < 2; i++)
                check_u64(what, "nextLong", i, hc_xoro_next(&r),
                          (uint64_t)v[i]);
            n_at++;
        } else if (sscanf(line, "fork[%d] %" SCNd64 " %" SCNd64, &idx, &v[0],
                          &v[1]) == 3) {
            if (want_fork < 0)
                die(path, "fork[] before its section header");
            if (idx != n_fork)
                die(path, "fork[] index out of order");
            hc_xoro_t r;
            hc_xoro_fork(&fork_base, &r);
            for (int i = 0; i < 2; i++)
                check_u64("fork", "nextLong", i, hc_xoro_next(&r),
                          (uint64_t)v[i]);
            n_fork++;
        } else {
            die_line(path, line);
        }
    }
    fclose(f);

    if (!has_seed)
        die(path, "missing seed");
    if (want_hash < 1 || n_hash != want_hash)
        die(path, "fromHashOf section missing or count mismatch");
    if (want_at < 1 || n_at != want_at)
        die(path, "at section missing or count mismatch");
    if (want_fork < 1 || n_fork != want_fork)
        die(path, "fork section missing or count mismatch");

    printf("test_noise fork: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
                "usage: test_noise fork|octaves|normal|blended <golden>\n");
        return 2;
    }
    const char *mode = argv[1];
    const char *path = argv[2];

    if (strcmp(mode, "fork") == 0)
        return run_fork(path);
    fprintf(stderr, "unknown mode: %s\n", mode);
    return 2;
}
