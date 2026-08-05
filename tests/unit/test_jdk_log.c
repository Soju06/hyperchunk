/* JDK Math.log 스텁 이식 단위 게이트.
 *
 * 벡터 출처:
 *  - golden/rng/jdk_log.txt — 이 머신의 JDK 25 HotSpot Math.log 스텁
 *    (StubRoutines::dlog) 출력. MarsagliaPolarGaussian 의
 *    Math.log(r²), r² ∈ (0,1) 은 JDK Math 호출이라 glibc log() 의 1-ulp
 *    차이가 월드젠 비트 패리티를 깬다 (sin/cos 와 동일 교훈, MEMORY.md
 *    jdk-sincos-not-libm) — 스텁 이식 hc_jdk_log (core/src/jdk_log.c) 가
 *    이 덤프와 전 코퍼스 (특수값 포함) 비트 일치해야 한다. */

#undef NDEBUG

#include "../../core/src/hc_jdk_log.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_jdk_log <golden_jdk_log_txt>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        fprintf(stderr, "SETUP: cannot open %s\n", argv[1]);
        return 2;
    }
    char line[256];
    int  n = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        unsigned long long in_bits, log_bits;
        if (sscanf(line, "in=0x%llx log=0x%llx", &in_bits, &log_bits) != 2) {
            fprintf(stderr, "SETUP: bad jdk_log line: %s", line);
            fclose(f);
            return 2;
        }
        double in;
        memcpy(&in, &in_bits, 8);
        double   got = hc_jdk_log(in);
        uint64_t gb;
        memcpy(&gb, &got, 8);
        if (gb != (uint64_t)log_bits) {
            if (bad < 8)
                fprintf(stderr,
                        "FAIL: port vs JDK in=%016llx got %016" PRIx64
                        " want %016llx\n",
                        in_bits, gb, log_bits);
            bad++;
        }
        n++;
    }
    fclose(f);
    if (bad)
        fprintf(stderr,
                "FAIL: hc_jdk_log mismatches JDK stub on %d/%d vectors — "
                "jdk_log.c port broken\n",
                bad, n);
    printf("jdk_log: %d vectors, %d mismatches\n", n, bad);
    return bad == 0 ? 0 : 1;
}
