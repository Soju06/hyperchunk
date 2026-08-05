/* Beardifier compute 패리티 (Task 14) — BeardProbe.java 가 실서버
 * Beardifier(@VisibleForTesting 생성자) 로 덤프한 29,375 그리드 포인트와
 * hc_beard_compute 를 비트 단위 대조. rigid 4종 adjustment 경로 + junction
 * (커널/fastInvSqrt) + affectedBox 게이팅 + 합산 순서를 전부 커버한다.
 * rigid/junction 목록은 프로브와 동일하게 하드코딩 (동기 유지). */

#undef NDEBUG

#include "../../core/src/hc_beard.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_beard_math <beard_compute.txt>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    /* BeardProbe.java 의 구성 그대로 */
    static const hc_beard_rigid_t RIGIDS[6] = {
        {{0, -28, 0, 30, -10, 18}, 1, HC_TA_ENCAPSULATE},
        {{-9, -35, 7, 12, -22, 40}, -13, HC_TA_ENCAPSULATE},
        {{20, -20, -15, 45, 5, 3}, 0, HC_TA_ENCAPSULATE},
        {{-30, 0, -30, -14, 12, -12}, 2, HC_TA_BURY},
        {{40, -5, 30, 58, 9, 47}, 0, HC_TA_BEARD_THIN},
        {{-25, -60, 25, -5, -45, 44}, 3, HC_TA_BEARD_BOX},
    };
    static const hc_beard_junction_t JUNCTIONS[4] = {
        {5, -27, 9},
        {-2, -34, 33},
        {31, -19, -4},
        {47, -3, 38},
    };
    hc_beard_t b;
    memset(&b, 0, sizeof b);
    b.has_any = 1;
    b.n_rigids = 6;
    b.rigids = RIGIDS;
    b.n_junctions = 4;
    b.junctions = JUNCTIONS;

    char    line[256];
    int64_t n = 0, bad = 0;
    int     have_affected = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') {
            int32_t a[6];
            if (sscanf(line, "# affected %d %d %d %d %d %d", &a[0], &a[1],
                       &a[2], &a[3], &a[4], &a[5]) == 6) {
                memcpy(b.affected, a, sizeof a);
                have_affected = 1;
            }
            continue;
        }
        int32_t            x, y, z;
        unsigned long long bits;
        if (sscanf(line, "%d %d %d %llx", &x, &y, &z, &bits) != 4) {
            fprintf(stderr, "bad line: %s", line);
            return 2;
        }
        assert(have_affected);
        double   v = hc_beard_compute(&b, x, y, z);
        uint64_t got;
        memcpy(&got, &v, 8);
        if (got != (uint64_t)bits) {
            if (bad < 10)
                fprintf(stderr,
                        "MISMATCH (%d,%d,%d): ours %016" PRIx64
                        " golden %016llx\n",
                        x, y, z, got, bits);
            bad++;
        }
        n++;
    }
    fclose(f);
    printf("beard compute: %" PRId64 " points, %" PRId64 " mismatches\n", n,
           bad);
    if (n < 29000 || bad)
        return 1;
    printf("test_beard_math: PASS\n");
    return 0;
}
