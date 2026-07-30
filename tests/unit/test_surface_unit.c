/* SurfaceSystem 구성요소 golden 대조 (Plan Task 7).
 *
 * golden/rng/surface_seed*.txt 의
 *  - surface_noises : 16개 surface 노이즈의 valueFactor / 옥타브 오프셋
 *                     (xo,yo,zo) / NormalNoise.getValue 샘플 — 전부 비트
 *                     단위 (fork-by-string 시딩 + reference 파라미터 검증)
 *  - clay_bands     : generateBands RNG 스크립트 192 엔트리
 *  - surface_depth  : getSurfaceDepth(int) + getSurfaceSecondary(double
 *                     비트) — 48x48 컬럼 (9 골든 청크 전부 커버)
 *
 * 여기가 통과해야 stage 테스트의 diff 가 룰 로직으로 좁혀진다. */

#undef NDEBUG

#include "../../core/src/hc_surface.h"

#include <assert.h>
#include <dirent.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails = 0;

static void die(const char *msg, const char *detail) {
    fprintf(stderr, "GOLDEN/SETUP ERROR: %s%s%s\n", msg, detail ? ": " : "",
            detail ? detail : "");
    exit(2);
}

static unsigned char g_backing[64u << 20];
static hc_arena_t    g_arena;

static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot open", path);
    if (fseek(f, 0, SEEK_END) != 0)
        die("seek failed", path);
    long sz = ftell(f);
    if (sz < 0)
        die("tell failed", path);
    rewind(f);
    char *buf = hc_arena_alloc(&g_arena, (size_t)sz + 1, 1);
    if (!buf)
        die("arena exhausted reading", path);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz)
        die("short read", path);
    fclose(f);
    buf[sz] = '\0';
    if (len_out)
        *len_out = (size_t)sz;
    return buf;
}

static const hc_json_t *parse_file(const char *path) {
    char       *buf = read_file(path, NULL);
    const char *err = NULL;
    size_t      pos = 0;
    hc_json_t  *v = hc_json_parse(buf, &g_arena, &err, &pos);
    if (!v) {
        fprintf(stderr, "JSON parse error in %s at %zu: %s\n", path, pos,
                err ? err : "?");
        exit(2);
    }
    return v;
}

#define MAX_SOURCES 64
static hc_df_source_t g_noises[MAX_SOURCES];
static int32_t        g_n_noises = 0;

static void load_noise_dir(const char *ref_dir) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/noise", ref_dir);
    DIR *d = opendir(dir);
    if (!d)
        die("cannot open reference/noise", dir);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.' || n < 6)
            continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        char *name = hc_arena_alloc(&g_arena, 10 + n + 1, 1);
        if (!name)
            die("arena exhausted (name)", e->d_name);
        sprintf(name, "minecraft:%.*s", (int)(n - 5), e->d_name);
        if (g_n_noises >= MAX_SOURCES)
            die("too many noise sources", name);
        g_noises[g_n_noises].name = name;
        g_noises[g_n_noises].json = parse_file(path);
        g_n_noises++;
    }
    closedir(d);
}

/* --- 비트 대조 헬퍼 --- */

static uint64_t dbits(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

static void check_bits(const char *what, double ours, uint64_t want_bits) {
    if (dbits(ours) != want_bits) {
        if (++g_fails <= 20)
            fprintf(stderr,
                    "BITS %s: ours %.17g (0x%016" PRIx64 ") golden 0x%016" PRIx64
                    "\n",
                    what, ours, dbits(ours), want_bits);
    }
}

/* 라인에서 마지막 bits=0x... 를 파싱 */
static int parse_bits(const char *line, uint64_t *out) {
    const char *p = strstr(line, "bits=0x");
    if (!p)
        return -1;
    *out = strtoull(p + 7, NULL, 16);
    return 0;
}

/* --- 노이즈 룩업: 9개 시스템 필드 + 샘플러 테이블 --- */

static const hc_normal_noise_t *find_noise(const hc_surface_t *s,
                                           const char *key) {
    static const struct {
        const char *key;
        size_t      off;
    } FIXED[] = {
        {"minecraft:clay_bands_offset",
         offsetof(hc_surface_t, clay_bands_offset_noise)},
        {"minecraft:surface", offsetof(hc_surface_t, surface_noise)},
        {"minecraft:surface_secondary",
         offsetof(hc_surface_t, surface_secondary_noise)},
        {"minecraft:badlands_pillar",
         offsetof(hc_surface_t, badlands_pillar_noise)},
        {"minecraft:badlands_pillar_roof",
         offsetof(hc_surface_t, badlands_pillar_roof_noise)},
        {"minecraft:badlands_surface",
         offsetof(hc_surface_t, badlands_surface_noise)},
        {"minecraft:iceberg_pillar",
         offsetof(hc_surface_t, iceberg_pillar_noise)},
        {"minecraft:iceberg_pillar_roof",
         offsetof(hc_surface_t, iceberg_pillar_roof_noise)},
        {"minecraft:iceberg_surface",
         offsetof(hc_surface_t, iceberg_surface_noise)},
    };
    for (size_t i = 0; i < sizeof FIXED / sizeof FIXED[0]; i++)
        if (strcmp(key, FIXED[i].key) == 0)
            return (const hc_normal_noise_t *)((const char *)s + FIXED[i].off);
    for (int32_t i = 0; i < s->n_samplers; i++)
        if (strcmp(key, s->samplers[i].key) == 0)
            return &s->samplers[i].noise;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
                "usage: test_surface_unit <ref_dir> <surface_seed_golden>\n");
        return 2;
    }
    hc_arena_init(&g_arena, g_backing, sizeof g_backing);
    load_noise_dir(argv[1]);
    if (g_n_noises < 51)
        die("reference/noise incomplete (needs surface params)", argv[1]);

    char path[1024];
    snprintf(path, sizeof path, "%s/overworld-26.2.json", argv[1]);
    const hc_json_t *settings = parse_file(path);

    hc_biome_reg_t reg;
    hc_biome_reg_init(&reg, &g_arena);
    hc_surface_t *s =
        hc_arena_alloc(&g_arena, sizeof *s, _Alignof(hc_surface_t));
    int64_t seed = 1234567890;
    if (!s || hc_surface_init(s, &g_arena, seed, settings, g_noises,
                              g_n_noises, &reg) != 0)
        die("surface init failed", s ? s->err : "arena");
    printf("compiled: %d rules, %d conds, %d samplers, %d biomes interned\n",
           s->n_rules, s->n_conds, s->n_samplers, reg.count);
    assert(s->n_rules > 100 && s->n_conds > 50 && s->n_samplers >= 8);

    FILE *f = fopen(argv[2], "r");
    if (!f)
        die("cannot open surface golden", argv[2]);

    char line[512];
    int  n_noise_checks = 0, n_bands = 0, n_depth = 0;
    const hc_normal_noise_t *cur = NULL;
    char cur_key[128] = "";

    while (fgets(line, sizeof line, f)) {
        int64_t gseed;
        if (sscanf(line, "seed %" SCNd64, &gseed) == 1 && gseed != seed)
            die("golden seed mismatch", argv[2]);

        /* --- surface_depth --- */
        int    dx, dz, ddepth;
        if (sscanf(line, "surface_depth %d %d %d", &dx, &dz, &ddepth) == 3) {
            uint64_t want;
            if (parse_bits(line, &want) != 0)
                die("surface_depth line missing bits", line);
            int32_t ours = hc_surface_depth(s, dx, dz);
            if (ours != ddepth) {
                if (++g_fails <= 20)
                    fprintf(stderr, "DEPTH (%d,%d): ours %d golden %d\n", dx,
                            dz, ours, ddepth);
            }
            check_bits("surface_secondary", hc_surface_secondary(s, dx, dz),
                       want);
            n_depth++;
            continue;
        }

        /* --- clay_bands --- */
        int  bi;
        char bname[96];
        if (sscanf(line, "clay_band %d Block{%95[^}]}", &bi, bname) == 2) {
            assert(bi >= 0 && bi < HC_SURF_CLAY_BANDS);
            const char *ours = hc_block_name(s->clay_bands[bi]);
            if (strcmp(ours, bname) != 0) {
                if (++g_fails <= 20)
                    fprintf(stderr, "BAND %d: ours %s golden %s\n", bi, ours,
                            bname);
            }
            n_bands++;
            continue;
        }

        /* --- surface_noises --- */
        char key[128];
        if (sscanf(line, "noise \"%127[^\"]\"", key) == 1) {
            cur = find_noise(s, key);
            snprintf(cur_key, sizeof cur_key, "%s", key);
            if (!cur)
                die("noise not instantiated on our side", key);
            n_noise_checks++;
            continue;
        }
        if (!cur)
            continue;

        uint64_t want;
        double   gx, gy, gz;
        int      oi;
        char     what[192];
        if (strncmp(line, ".valueFactor ", 13) == 0) {
            if (parse_bits(line, &want) == 0) {
                snprintf(what, sizeof what, "%s.valueFactor", cur_key);
                check_bits(what, cur->value_factor, want);
            }
            continue;
        }
        /* .first/.second octave 오프셋 + null 검증 */
        const hc_octaves_t *oct = NULL;
        const char         *rest = NULL;
        if (strncmp(line, ".first.octave[", 14) == 0) {
            oct = &cur->first;
            rest = line + 14;
        } else if (strncmp(line, ".second.octave[", 15) == 0) {
            oct = &cur->second;
            rest = line + 15;
        }
        if (oct && sscanf(rest, "%d", &oi) == 1) {
            assert(oi >= 0 && oi < oct->count);
            const hc_perlin_t *p = oct->octaves[oi];
            if (strstr(line, " null")) {
                if (p != NULL) {
                    if (++g_fails <= 20)
                        fprintf(stderr, "%s octave[%d]: ours non-null, golden "
                                        "null\n",
                                cur_key, oi);
                }
            } else if (parse_bits(line, &want) == 0) {
                if (!p) {
                    if (++g_fails <= 20)
                        fprintf(stderr,
                                "%s octave[%d]: ours null, golden set\n",
                                cur_key, oi);
                } else {
                    double v = strstr(line, ".xo ")   ? p->xo
                               : strstr(line, ".yo ") ? p->yo
                                                      : p->zo;
                    snprintf(what, sizeof what, "%s octave[%d]", cur_key, oi);
                    check_bits(what, v, want);
                }
            }
            continue;
        }
        /* NormalNoise.getValue 샘플 (.getValue — .first/.second 제외) */
        if (sscanf(line, ".getValue(%lf,%lf,%lf)", &gx, &gy, &gz) == 3 &&
            parse_bits(line, &want) == 0) {
            snprintf(what, sizeof what, "%s.getValue(%g,%g,%g)", cur_key, gx,
                     gy, gz);
            check_bits(what, hc_normal_noise_value(cur, gx, gy, gz), want);
            continue;
        }
    }
    fclose(f);

    if (n_noise_checks != 16 || n_bands != 192 || n_depth != 48 * 48)
        die("golden sections incomplete — malformed file?", argv[2]);
    printf("test_surface_unit: 16 noises, %d bands, %d depth columns, %d "
           "fails\n",
           n_bands, n_depth, g_fails);
    return g_fails == 0 ? 0 : 1;
}
