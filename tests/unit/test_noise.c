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

static void check_bits(const char *label, double got, uint64_t want_bits) {
    uint64_t got_bits;
    memcpy(&got_bits, &got, sizeof got_bits);
    g_checks++;
    if (got_bits != want_bits) {
        g_fails++;
        fprintf(stderr,
                "FAIL %s: got %.17g (0x%016" PRIx64 ") want bits 0x%016" PRIx64
                "\n",
                label, got, got_bits, want_bits);
    }
}

/* 노이즈 인스턴스 시딩용 arena — 블록마다 reset 해 재사용한다 */
static unsigned char g_backing[1u << 20];
static hc_arena_t g_arena;

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

/* --------- octaves/normal: NormalNoise 내부 + 샘플 (octaves golden) --------- */

#define MAX_OCT     16
#define MAX_SAMPLES 8

typedef struct {
    int      has_fo, fo;
    int      n_amp;
    double   amp[MAX_OCT];
    int      has_lfif, has_lfvf;
    uint64_t lfif_bits, lfvf_bits;
    int      oct_count; /* .octave_count 선언, 없으면 -1 */
    int      oct_seen[MAX_OCT]; /* 비트: 1 xo, 2 yo, 4 zo, 8 null */
    uint64_t oct_bits[MAX_OCT][3];
    int      n_val;
    double   vx[MAX_SAMPLES], vy[MAX_SAMPLES], vz[MAX_SAMPLES];
    uint64_t vbits[MAX_SAMPLES];
} perlin_golden_t;

typedef struct {
    char            key[128];
    int             has_vf;
    uint64_t        vf_bits;
    perlin_golden_t first, second;
    int             n_val; /* NormalNoise.getValue 샘플 */
    double          vx[MAX_SAMPLES], vy[MAX_SAMPLES], vz[MAX_SAMPLES];
    uint64_t        vbits[MAX_SAMPLES];
} noise_golden_t;

/* rest 는 ".first"/".second" 프리픽스를 제거한 나머지 (".firstOctave ..."). */
static int parse_perlin_line(const char *path, const char *rest,
                             perlin_golden_t *pg) {
    int      iv, oi;
    uint64_t bits;
    double   x, y, z;
    char     tag[8];

    if (sscanf(rest, ".firstOctave %d", &iv) == 1) {
        pg->fo = iv;
        pg->has_fo = 1;
    } else if (strncmp(rest, ".amplitudes [", 13) == 0) {
        const char *s = rest + 13;
        char       *end;
        for (;;) {
            double v = strtod(s, &end);
            if (end == s)
                break;
            if (pg->n_amp >= MAX_OCT)
                die(path, "amplitude overflow");
            pg->amp[pg->n_amp++] = v;
            s = end;
            while (*s == ',' || *s == ' ')
                s++;
        }
        if (*s != ']')
            die(path, "amplitudes not terminated by ]");
        if (pg->n_amp < 1)
            die(path, "empty amplitudes");
    } else if (sscanf(rest, ".lowestFreqInputFactor %*s bits=0x%" SCNx64,
                      &bits) == 1) {
        pg->lfif_bits = bits;
        pg->has_lfif = 1;
    } else if (sscanf(rest, ".lowestFreqValueFactor %*s bits=0x%" SCNx64,
                      &bits) == 1) {
        pg->lfvf_bits = bits;
        pg->has_lfvf = 1;
    } else if (sscanf(rest, ".octave_count %d", &iv) == 1) {
        pg->oct_count = iv;
    } else if (sscanf(rest, ".octave[%d].xo %*s bits=0x%" SCNx64, &oi,
                      &bits) == 2) {
        if (oi < 0 || oi >= MAX_OCT)
            die(path, "octave index out of range");
        pg->oct_bits[oi][0] = bits;
        pg->oct_seen[oi] |= 1;
    } else if (sscanf(rest, ".octave[%d].yo %*s bits=0x%" SCNx64, &oi,
                      &bits) == 2) {
        if (oi < 0 || oi >= MAX_OCT)
            die(path, "octave index out of range");
        pg->oct_bits[oi][1] = bits;
        pg->oct_seen[oi] |= 2;
    } else if (sscanf(rest, ".octave[%d].zo %*s bits=0x%" SCNx64, &oi,
                      &bits) == 2) {
        if (oi < 0 || oi >= MAX_OCT)
            die(path, "octave index out of range");
        pg->oct_bits[oi][2] = bits;
        pg->oct_seen[oi] |= 4;
    } else if (sscanf(rest, ".octave[%d] %7s", &oi, tag) == 2 &&
               strcmp(tag, "null") == 0) {
        if (oi < 0 || oi >= MAX_OCT)
            die(path, "octave index out of range");
        pg->oct_seen[oi] |= 8;
    } else if (sscanf(rest, ".getValue(%lf,%lf,%lf) %*s bits=0x%" SCNx64, &x,
                      &y, &z, &bits) == 4) {
        if (pg->n_val >= MAX_SAMPLES)
            die(path, "getValue sample overflow");
        pg->vx[pg->n_val] = x;
        pg->vy[pg->n_val] = y;
        pg->vz[pg->n_val] = z;
        pg->vbits[pg->n_val++] = bits;
    } else {
        return -1;
    }
    return 0;
}

/* 블록 완결성 검사 — 필수 섹션이 빠진 golden 은 공허 통과를 만들므로 즉사 */
static void validate_perlin_golden(const char *path, const char *key,
                                   const perlin_golden_t *pg) {
    if (!pg->has_fo || !pg->has_lfif || !pg->has_lfvf)
        die(path, "perlin block missing firstOctave/lowestFreq factors");
    if (pg->oct_count != pg->n_amp)
        die(path, "octave_count != amplitude count");
    if (pg->n_val < 3)
        die(path, "fewer than 3 perlin getValue samples");
    for (int i = 0; i < pg->n_amp; i++) {
        if (pg->oct_seen[i] != 7 && pg->oct_seen[i] != 8) {
            fprintf(stderr, "GOLDEN FORMAT ERROR %s: %s octave[%d] "
                            "incomplete (seen=%d)\n",
                    path, key, i, pg->oct_seen[i]);
            exit(2);
        }
    }
}

static void validate_noise_golden(const char *path, const noise_golden_t *b) {
    validate_perlin_golden(path, b->key, &b->first);
    validate_perlin_golden(path, b->key, &b->second);
    if (!b->has_vf)
        die(path, "missing .valueFactor");
    if (b->n_val < 4)
        die(path, "fewer than 4 NormalNoise getValue samples");
    if (b->first.fo != b->second.fo || b->first.n_amp != b->second.n_amp ||
        memcmp(b->first.amp, b->second.amp,
               sizeof(double) * (size_t)b->first.n_amp) != 0)
        die(path, "first/second parameter mismatch");
}

/* 구성한 hc_octaves_t 를 golden 의 first/second 서브블록과 대조한다 */
static void check_octaves(const char *key, const char *sub,
                          const perlin_golden_t *pg, const hc_octaves_t *o) {
    char label[256];

    snprintf(label, sizeof label, "%s%s.lowestFreqInputFactor", key, sub);
    check_bits(label, o->lowest_freq_input, pg->lfif_bits);
    snprintf(label, sizeof label, "%s%s.lowestFreqValueFactor", key, sub);
    check_bits(label, o->lowest_freq_value, pg->lfvf_bits);

    for (int i = 0; i < pg->n_amp; i++) {
        int want_null = (pg->oct_seen[i] == 8);
        g_checks++;
        if ((o->octaves[i] == NULL) != want_null) {
            g_fails++;
            fprintf(stderr, "FAIL %s%s.octave[%d]: got %s want %s\n", key,
                    sub, i, o->octaves[i] ? "instance" : "null",
                    want_null ? "null" : "instance");
            continue;
        }
        if (want_null)
            continue;
        static const char *axis[3] = {"xo", "yo", "zo"};
        const double      *got[3] = {&o->octaves[i]->xo, &o->octaves[i]->yo,
                                     &o->octaves[i]->zo};
        for (int c = 0; c < 3; c++) {
            snprintf(label, sizeof label, "%s%s.octave[%d].%s", key, sub, i,
                     axis[c]);
            check_bits(label, *got[c], pg->oct_bits[i][c]);
        }
    }
    for (int i = 0; i < pg->n_val; i++) {
        snprintf(label, sizeof label, "%s%s.getValue(%g,%g,%g)", key, sub,
                 pg->vx[i], pg->vy[i], pg->vz[i]);
        check_bits(label,
                   hc_octaves_value(o, pg->vx[i], pg->vy[i], pg->vz[i], 0.0,
                                    0.0),
                   pg->vbits[i]);
    }
}

/* 바닐라 RandomState.getOrCreateNoise 재현: 노이즈별 RNG 는
 * XoroshiroRandomSource(seed).forkPositional().fromHashOf(key) 다. */
static void seed_noise_rand(int64_t seed, const char *key, hc_xoro_t *out) {
    hc_xoro_t      base;
    hc_xoro_fork_t fk;
    hc_xoro_init(&base, seed);
    hc_xoro_fork_positional(&base, &fk);
    hc_xoro_from_hash_of(&fk, key, out);
}

/* NormalNoise 소비 순서 재현: first/second PerlinNoise 를 같은 rand 에서
 * 순차 초기화한다 (forkPositional 2회 == nextLong 4회 소비). */
static void verify_noise_block(const char *path, int64_t seed,
                               const noise_golden_t *b, int mode) {
    hc_xoro_t rand;
    seed_noise_rand(seed, b->key, &rand);
    hc_arena_reset(&g_arena);

    if (mode == 0) {
        hc_octaves_t first, second;
        if (hc_octaves_init(&first, &g_arena, &rand, b->first.fo,
                            b->first.amp, b->first.n_amp) != 0 ||
            hc_octaves_init(&second, &g_arena, &rand, b->first.fo,
                            b->first.amp, b->first.n_amp) != 0)
            die(path, "hc_octaves_init failed (arena exhausted?)");
        check_octaves(b->key, ".first", &b->first, &first);
        check_octaves(b->key, ".second", &b->second, &second);
    } else {
        die(path, "normal mode not implemented yet");
    }
}

/* mode: 0 = octaves (first/second 내부 + 샘플), 1 = normal (valueFactor +
 * 결합 샘플). 같은 파일을 두 게이트로 나눠 실패 계층을 격리한다. */
static int run_octaves_file(const char *path, int mode) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot open golden file: %s\n", path);
        exit(2);
    }
    hc_arena_init(&g_arena, g_backing, sizeof g_backing);

    char           line[16384];
    int64_t        seed = 0;
    int            has_seed = 0;
    int            want_count = -1, n_blocks = 0;
    noise_golden_t blk;
    int            in_block = 0;

    memset(&blk, 0, sizeof blk);
    blk.first.oct_count = blk.second.oct_count = -1;

    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || blank(line))
            continue;

        int64_t  sv;
        int      n;
        uint64_t bits;
        double   x, y, z;
        char     key[128];

        if (sscanf(line, "seed %" SCNd64, &sv) == 1) {
            seed = sv;
            has_seed = 1;
        } else if (sscanf(line, "noise_count %d", &n) == 1) {
            want_count = n;
        } else if (sscanf(line, "noise \"%127[^\"]\"", key) == 1) {
            if (in_block) {
                validate_noise_golden(path, &blk);
                verify_noise_block(path, seed, &blk, mode);
                n_blocks++;
            }
            if (!has_seed)
                die(path, "noise block before seed");
            memset(&blk, 0, sizeof blk);
            blk.first.oct_count = blk.second.oct_count = -1;
            snprintf(blk.key, sizeof blk.key, "%s", key);
            in_block = 1;
        } else if (!in_block) {
            die_line(path, line);
        } else if (strncmp(line, ".first.", 7) == 0) {
            if (parse_perlin_line(path, line + 6, &blk.first) != 0)
                die_line(path, line);
        } else if (strncmp(line, ".second.", 8) == 0) {
            if (parse_perlin_line(path, line + 7, &blk.second) != 0)
                die_line(path, line);
        } else if (sscanf(line, ".valueFactor %*s bits=0x%" SCNx64, &bits) ==
                   1) {
            blk.vf_bits = bits;
            blk.has_vf = 1;
        } else if (sscanf(line, ".getValue(%lf,%lf,%lf) %*s bits=0x%" SCNx64,
                          &x, &y, &z, &bits) == 4) {
            if (blk.n_val >= MAX_SAMPLES)
                die(path, "getValue sample overflow");
            blk.vx[blk.n_val] = x;
            blk.vy[blk.n_val] = y;
            blk.vz[blk.n_val] = z;
            blk.vbits[blk.n_val++] = bits;
        } else {
            die_line(path, line);
        }
    }
    fclose(f);

    if (in_block) {
        validate_noise_golden(path, &blk);
        verify_noise_block(path, seed, &blk, mode);
        n_blocks++;
    }
    if (!has_seed)
        die(path, "missing seed");
    if (want_count < 1 || n_blocks != want_count)
        die(path, "noise_count missing or block count mismatch");

    printf("test_noise %s: %d noises, %d checks, %d failures\n",
           mode == 0 ? "octaves" : "normal", n_blocks, g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}

/* -------- blended: old_blended_noise 슬롯 (router golden) -------- */

/* # blended 설정 문자열의 옥타브 튜플 (xo/yo/zo 는 %.3f, p0/p255 는
 * 부호 있는 byte). 비트정확 게이트는 아래 v 벡터가 담당하고, 이 튜플은
 * 컴퓨트가 어긋났을 때 시딩 계층 (어느 PerlinNoise 의 어느 옥타브) 을
 * 격리하는 진단용이다. 순서: min[0..15], max[0..15], main[0..7]. */
#define BLENDED_TUPLES (16 + 16 + 8)

typedef struct {
    double xo, yo, zo;
    int    p0, p255;
} blended_tuple_t;

static void check_blended_tuple(const char *who, int idx,
                                const blended_tuple_t *tu,
                                const hc_perlin_t *oct) {
    char label[128];
    g_checks++;
    if (!oct) {
        g_fails++;
        fprintf(stderr, "FAIL blended %s.octave[%d]: got null\n", who, idx);
        return;
    }
    /* %.3f 반올림 오차 한계. 비트 비교는 v 벡터가 담당한다. */
    const double  tol = 5.1e-4;
    const double  got[3] = {oct->xo, oct->yo, oct->zo};
    const double  want[3] = {tu->xo, tu->yo, tu->zo};
    static const char *axis[3] = {"xo", "yo", "zo"};
    for (int c = 0; c < 3; c++) {
        double diff = got[c] - want[c];
        g_checks++;
        if (diff < -tol || diff > tol) {
            g_fails++;
            snprintf(label, sizeof label, "blended %s.octave[%d].%s", who,
                     idx, axis[c]);
            fprintf(stderr, "FAIL %s: got %.6f want ~%.3f\n", label, got[c],
                    want[c]);
        }
    }
    check_u64("blended perm head", who, idx, (uint64_t)(int8_t)oct->perm[0],
              (uint64_t)tu->p0);
    check_u64("blended perm tail", who, idx,
              (uint64_t)(int8_t)oct->perm[255], (uint64_t)tu->p255);
}

static int run_blended(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot open golden file: %s\n", path);
        exit(2);
    }
    hc_arena_init(&g_arena, g_backing, sizeof g_backing);

    char            line[16384];
    int64_t         seed = 0;
    int             has_seed = 0, instances = -1;
    int             collecting = 0, n_vec = 0, n_tuples = 0;
    blended_tuple_t tuples[BLENDED_TUPLES];
    /* 벡터는 스트리밍 검증한다 — 먼저 인스턴스를 만들어야 하므로 seed
     * 확인 후 lazy-init 한다. */
    hc_blended_noise_t bn;
    int                bn_ready = 0;

    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "# blended ", 10) == 0) {
            const char *pos = line + 10;
            while ((pos = strstr(pos, "xo=")) != NULL) {
                if (n_tuples >= BLENDED_TUPLES)
                    die(path, "too many blended octave tuples");
                blended_tuple_t *tu = &tuples[n_tuples];
                if (sscanf(pos, "xo=%lf, yo=%lf, zo=%lf, p0=%d, p255=%d",
                           &tu->xo, &tu->yo, &tu->zo, &tu->p0,
                           &tu->p255) != 5)
                    die(path, "malformed blended octave tuple");
                n_tuples++;
                pos += 3;
            }
            continue;
        }
        if (line[0] == '#' || blank(line))
            continue;

        int64_t sv;
        int     x, y, z, n;
        char    name[128];
        uint64_t bits;

        if (sscanf(line, "seed %" SCNd64, &sv) == 1) {
            seed = sv;
            has_seed = 1;
        } else if (sscanf(line, "slot %127s", name) == 1) {
            collecting = (strcmp(name, "old_blended_noise") == 0);
            if (collecting && !bn_ready) {
                if (!has_seed)
                    die(path, "old_blended_noise slot before seed");
                /* RandomState: BlendedNoise.withNewRandom(
                 *   forkPositional().fromHashOf("minecraft:terrain")).
                 * 파라미터는 26.2 오버월드 base_3d_noise JSON
                 * (reference/density_function/overworld/base_3d_noise.json):
                 * xz_scale=0.25 y_scale=0.125 xz_factor=80 y_factor=160
                 * smear_scale_multiplier=8. */
                hc_xoro_t rand;
                seed_noise_rand(seed, "minecraft:terrain", &rand);
                if (hc_blended_init(&bn, &g_arena, &rand, 0.25, 0.125, 80.0,
                                    160.0, 8.0) != 0)
                    die(path, "hc_blended_init failed");
                bn_ready = 1;
            }
        } else if (sscanf(line, "v %d %d %d %*s bits=0x%" SCNx64, &x, &y, &z,
                          &bits) == 4) {
            if (!collecting)
                continue; /* 다른 슬롯 값은 6b (라우터 평가) 범위 */
            char label[64];
            snprintf(label, sizeof label, "old_blended_noise(%d,%d,%d)", x,
                     y, z);
            check_bits(label, hc_blended_compute(&bn, x, y, z), bits);
            n_vec++;
        } else if (sscanf(line, "blended_noise_instances %d", &n) == 1) {
            instances = n;
        } else {
            die_line(path, line);
        }
    }
    fclose(f);

    if (!has_seed)
        die(path, "missing seed");
    if (instances != 1)
        die(path, "blended_noise_instances != 1");
    if (!bn_ready || n_vec < 100)
        die(path, "old_blended_noise slot missing or too few vectors");
    if (n_tuples != BLENDED_TUPLES)
        die(path, "blended config tuples != 40");

    /* 시딩 계층 진단: 설정 문자열 순서 = min[0..15], max[0..15], main[0..7]
     * (배열 인덱스 순서, getOctaveNoise 역순 아님) */
    for (int i = 0; i < 16; i++)
        check_blended_tuple("minLimit", i, &tuples[i],
                            bn.min_limit.octaves[i]);
    for (int i = 0; i < 16; i++)
        check_blended_tuple("maxLimit", i, &tuples[16 + i],
                            bn.max_limit.octaves[i]);
    for (int i = 0; i < 8; i++)
        check_blended_tuple("main", i, &tuples[32 + i],
                            bn.main_noise.octaves[i]);

    printf("test_noise blended: %d vectors, %d checks, %d failures\n", n_vec,
           g_checks, g_fails);
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
    if (strcmp(mode, "octaves") == 0)
        return run_octaves_file(path, 0);
    if (strcmp(mode, "blended") == 0)
        return run_blended(path);
    fprintf(stderr, "unknown mode: %s\n", mode);
    return 2;
}
