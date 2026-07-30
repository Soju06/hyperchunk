/* Mth.sin/cos(double)→float 단위 게이트 (Task 8).
 *
 * golden/rng/mth_sin_table.txt (tools/golden/MthGolden.java — 실제 26.2
 * 서버 Mth.SIN 을 리플렉션으로 덤프) 와 대조:
 *  - sin_table: 65536 엔트리 전부 비트 일치 (glibc sin 의 d2f 결과가
 *    HotSpot libm 스텁과 같음을 증명 — A7 §4.1 OPEN 클로즈)
 *  - sin_probes/cos_probes: d2l 절단 인덱스 경로 (음수 각도 포함) */

#undef NDEBUG

#include "../../core/src/hc_carvers.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails = 0;

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_carvers_unit <mth_sin_table.txt>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    hc_mth_trig_init();
    const float *tab = hc_mth_sin_table();

    char line[256];
    int  n_tab = 0, n_sin = 0, n_cos = 0;
    int  section = 0; /* 1=sin_table, 2=sin_probes, 3=cos_probes */
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "section sin_table", 17) == 0) {
            section = 1;
            continue;
        }
        if (strncmp(line, "section sin_probes", 18) == 0) {
            section = 2;
            continue;
        }
        if (strncmp(line, "section cos_probes", 18) == 0) {
            section = 3;
            continue;
        }
        if (line[0] == '#')
            continue;
        if (section == 1) {
            int      i;
            uint32_t bits;
            if (sscanf(line, "%d bits=0x%x", &i, &bits) != 2)
                continue;
            assert(i >= 0 && i < 65536);
            uint32_t ours;
            memcpy(&ours, &tab[i], 4);
            if (ours != bits) {
                if (g_fails < 10)
                    fprintf(stderr, "SIN[%d]: ours=0x%08x golden=0x%08x\n", i,
                            ours, bits);
                g_fails++;
            }
            n_tab++;
        } else if (section == 2 || section == 3) {
            uint64_t abits;
            uint32_t rbits;
            if (sscanf(line, "0x%lx -> bits=0x%x", &abits, &rbits) != 2)
                continue;
            double a;
            memcpy(&a, &abits, 8);
            float    r = section == 2 ? hc_mth_sin(a) : hc_mth_cos(a);
            uint32_t ours;
            memcpy(&ours, &r, 4);
            if (ours != rbits) {
                fprintf(stderr, "%s(%.17g): ours=0x%08x golden=0x%08x\n",
                        section == 2 ? "sin" : "cos", a, ours, rbits);
                g_fails++;
            }
            if (section == 2)
                n_sin++;
            else
                n_cos++;
        }
    }
    fclose(f);

    if (n_tab != 65536 || n_sin < 20 || n_cos < 20) {
        fprintf(stderr, "golden incomplete: table=%d sin=%d cos=%d\n", n_tab,
                n_sin, n_cos);
        return 2;
    }
    printf("test_carvers_unit: %d table + %d sin + %d cos probes, %d fails\n",
           n_tab, n_sin, n_cos, g_fails);
    return g_fails == 0 ? 0 : 1;
}
