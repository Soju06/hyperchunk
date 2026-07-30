/* 바이옴 줌 golden 대조 (Plan Task 7).
 *
 * golden/rng/surface_seed*.txt 의
 *  - obfuscated_seed : hc_biome_obfuscate_seed (sha256 경로)
 *  - quart_biomes    : 쿼트 저장소 (chunks [-2..2]^2, 검증 입력)
 *  - zoomed_biomes   : BiomeManager.getBiome 결과 12,288 벡터 — 우리
 *                      hc_biome_zoom + 쿼트 클램프 조회가 전부 일치해야
 *                      한다. 여기가 어긋나면 surface 의 biome 조건이
 *                      통째로 어긋난다. */

#undef NDEBUG

#include "../../core/src/hc_biome.h"
#include "../../core/src/hc_sha256.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* quart_biomes 그리드: qx,qz in [-8..11], qy in [-16..79] */
enum { Q_MIN_XZ = -8, Q_MAX_XZ = 11, Q_MIN_Y = -16, Q_MAX_Y = 79 };
enum { Q_NXZ = Q_MAX_XZ - Q_MIN_XZ + 1, Q_NY = Q_MAX_Y - Q_MIN_Y + 1 };

static uint8_t g_grid[Q_NY][Q_NXZ][Q_NXZ]; /* [qy][qz][qx] 팔레트 인덱스 */
static char    g_pal[64][64];
static int     g_n_pal = 0;

static const char *quart_lookup(int32_t qx, int32_t qy, int32_t qz) {
    /* ChunkAccess.getNoiseBiome 은 쿼트 y 를 청크 범위로 클램프한다.
     * x/z 는 실제 청크로 라우팅되므로 그리드 범위 안이어야 한다. */
    if (qy < Q_MIN_Y) qy = Q_MIN_Y;
    if (qy > Q_MAX_Y) qy = Q_MAX_Y;
    assert(qx >= Q_MIN_XZ && qx <= Q_MAX_XZ);
    assert(qz >= Q_MIN_XZ && qz <= Q_MAX_XZ);
    return g_pal[g_grid[qy - Q_MIN_Y][qz - Q_MIN_XZ][qx - Q_MIN_XZ]];
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_biome_zoom <surface_seed_golden.txt>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    char    line[512];
    int64_t seed = 0, obf_golden = 0;
    int     fails = 0, zoom_checked = 0;
    int     in_quarts = 0, cur_qy = -9999, cur_qz = 0;

    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "seed %" SCNd64, &seed) == 1)
            continue;
        if (sscanf(line, "obfuscated_seed %" SCNd64, &obf_golden) == 1)
            continue;
        if (strncmp(line, "section quart_biomes", 20) == 0) {
            in_quarts = 1;
            continue;
        }
        if (strncmp(line, "section", 7) == 0) {
            in_quarts = 0;
            continue;
        }
        if (in_quarts) {
            int idx;
            char name[64];
            if (sscanf(line, "palette %d %63s", &idx, name) == 2) {
                assert(idx == g_n_pal && g_n_pal < 64);
                strcpy(g_pal[g_n_pal++], name);
            } else if (sscanf(line, "qy %d", &cur_qy) == 1) {
                cur_qz = 0;
            } else {
                const char *p = line;
                for (int qx = 0; qx < Q_NXZ; qx++) {
                    char *end;
                    long  v = strtol(p, &end, 10);
                    assert(end != p && v >= 0 && v < g_n_pal);
                    g_grid[cur_qy - Q_MIN_Y][cur_qz][qx] = (uint8_t)v;
                    p = end;
                }
                cur_qz++;
            }
            continue;
        }
        int  x, y, z;
        char want[64];
        if (sscanf(line, "zoomed %d %d %d %63s", &x, &y, &z, want) == 4) {
            int32_t qx, qy, qz;
            hc_biome_zoom(hc_biome_obfuscate_seed(seed), x, y, z, &qx, &qy,
                          &qz);
            const char *got = quart_lookup(qx, qy, qz);
            zoom_checked++;
            if (strcmp(got, want) != 0) {
                if (++fails <= 10)
                    fprintf(stderr,
                            "zoom mismatch at (%d,%d,%d): ours %s golden %s "
                            "(quart %d,%d,%d)\n",
                            x, y, z, got, want, qx, qy, qz);
            }
        }
    }
    fclose(f);

    if (obf_golden == 0 || seed == 0) {
        fprintf(stderr, "golden file missing seed/obfuscated_seed\n");
        return 2;
    }
    int64_t obf = hc_biome_obfuscate_seed(seed);
    if (obf != obf_golden) {
        fprintf(stderr, "obfuscateSeed mismatch: ours %" PRId64 " golden %" PRId64 "\n",
                obf, obf_golden);
        fails++;
    }
    if (zoom_checked < 10000) {
        fprintf(stderr, "too few zoom vectors (%d) — golden malformed?\n",
                zoom_checked);
        return 2;
    }
    printf("test_biome_zoom: %d zoom vectors, %d fails\n", zoom_checked,
           fails);
    return fails ? 1 : 0;
}
