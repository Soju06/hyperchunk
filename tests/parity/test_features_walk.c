/* 07_features 스테이지 워크 패리티 (Plan Task 9a 게이트).
 *
 * 게이트 3단 (태스크 브리프):
 *  (a) 데코 시드: 두 번들 order.manifest 의 전 라인 (81+81) 에 대해
 *      hc_features_decoration_seed 재계산 == 기록 hex.
 *  (b) 트레이스: PRIMARY 번들 순서로 그리드 9청크를 재생하며 step<=8 의
 *      p/f 이벤트 시퀀스가 golden/features-trace 의 바닐라 트레이스와
 *      정확히 일치 (positions + npos + placed).
 *  (c) 블록: 재생 결과(각 청크는 자기 데코 완료 시점 스냅샷)를 golden 07
 *      과 비교. step 9/10 (초목, 9b) 이 덮어쓴 위치만 잔차로 허용:
 *      g7 이 테이블 밖 블록(9b 팔레트)이거나 {clay, water, dirt} 인
 *      불일치는 카테고리별 카운트, 그 외 불일치는 실패. PRIMARY 와 ALT
 *      번들 모두 재생·비교한다 (서로 다른 두 순서 — ADR-007 D3 의
 *      Tier-2 증거를 ore/blob 패밀리에 대해 선취).
 *
 * 월드 구성: 그리드 9청크는 '우리' 04→(골든 03 바이옴)→05→06 체인
 * (test_carvers_stage.c 와 동일), 링 16청크는 golden/features-trace 의
 * 06_carvers/03_biomes 덤프 로드 (Tier-1 스테이지라 순서 무관·검증됨).
 * 하이트맵 게이트는 9b (FINAL 맵 유지관리와 함께). */

#undef NDEBUG

#include "../../core/src/hc_carvers.h"
#include "../../core/src/hc_features.h"
#include "../../core/src/hc_sha256.h"

#include <assert.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails = 0;

static void die(const char *msg, const char *detail) {
    fprintf(stderr, "GOLDEN/SETUP ERROR: %s%s%s\n", msg, detail ? ": " : "",
            detail ? detail : "");
    exit(2);
}

static unsigned char g_backing[448u << 20];
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

/* --- reference/ 로더 (test_carvers_stage.c 규약; features 는 테이블이
 * 커서 용량만 다르다) --- */

#define MAX_SOURCES 64
#define MAX_FEAT_SOURCES 256

static hc_df_source_t g_dfs[MAX_SOURCES];
static int32_t        g_n_dfs = 0;
static hc_df_source_t g_noises[MAX_SOURCES];
static int32_t        g_n_noises = 0;
static hc_df_source_t g_tags[MAX_SOURCES];
static int32_t        g_n_tags = 0;
static hc_df_source_t g_placed[MAX_FEAT_SOURCES];
static int32_t        g_n_placed = 0;
static hc_df_source_t g_configured[MAX_FEAT_SOURCES];
static int32_t        g_n_configured = 0;

static int has_suffix(const char *s, const char *suf) {
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

static void add_source(hc_df_source_t *tab, int32_t *n, int32_t cap,
                       const char *dir, const char *rel_prefix,
                       const char *fname) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, fname);
    char *name = hc_arena_alloc(
        &g_arena, 10 + strlen(rel_prefix) + strlen(fname) + 1, 1);
    if (!name)
        die("arena exhausted (source name)", fname);
    size_t stem = strlen(fname) - 5;
    sprintf(name, "minecraft:%s%.*s", rel_prefix, (int)stem, fname);
    if (*n >= cap)
        die("too many reference sources", name);
    tab[*n].name = name;
    tab[*n].json = parse_file(path);
    (*n)++;
}

static void load_tree(hc_df_source_t *tab, int32_t *n, int32_t cap,
                      const char *dir, const char *rel_prefix, int depth) {
    DIR *d = opendir(dir);
    if (!d)
        die("cannot open reference dir", dir);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char sub[1024];
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        DIR *probe = opendir(sub);
        if (probe) {
            closedir(probe);
            if (depth >= 2)
                die("reference tree deeper than expected", sub);
            char pref[512];
            snprintf(pref, sizeof pref, "%s%s/", rel_prefix, e->d_name);
            load_tree(tab, n, cap, sub, pref, depth + 1);
        } else if (has_suffix(e->d_name, ".json")) {
            add_source(tab, n, cap, dir, rel_prefix, e->d_name);
        } else {
            die("unexpected file in reference tree", sub);
        }
    }
    closedir(d);
}

/* --- 5x5 쿼트 바이옴 그리드 (surface golden — 청크 -2..2 커버) --- */

enum { QG_MIN_XZ = -8, QG_NXZ = 20, QG_MIN_Y = -16, QG_NY = 96 };

static uint16_t       g_grid[QG_NY][QG_NXZ][QG_NXZ];
static hc_biome_reg_t g_reg;

static void load_quart_grid(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        die("cannot open surface golden", path);
    char    line[512];
    int     in_section = 0, cur_qy = -9999, cur_qz = 0;
    int32_t pal[64];
    int     n_pal = 0, rows = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "section quart_biomes", 20) == 0) {
            in_section = 1;
            continue;
        }
        if (strncmp(line, "section", 7) == 0) {
            in_section = 0;
            continue;
        }
        if (!in_section)
            continue;
        int  idx;
        char name[64];
        if (sscanf(line, "palette %d %63s", &idx, name) == 2) {
            if (idx != n_pal || n_pal >= 64)
                die("quart_biomes palette out of order", path);
            pal[n_pal] = hc_biome_intern(&g_reg, name, (int32_t)strlen(name));
            if (pal[n_pal] < 0)
                die("biome registry full", name);
            n_pal++;
        } else if (sscanf(line, "qy %d", &cur_qy) == 1) {
            cur_qz = 0;
        } else if (cur_qy != -9999) {
            const char *p = line;
            for (int qx = 0; qx < QG_NXZ; qx++) {
                char *end;
                long  v = strtol(p, &end, 10);
                if (end == p || v < 0 || v >= n_pal)
                    die("bad quart_biomes row", path);
                g_grid[cur_qy - QG_MIN_Y][cur_qz][qx] = (uint16_t)pal[v];
                p = end;
            }
            cur_qz++;
            rows++;
        }
    }
    fclose(f);
    if (rows != QG_NY * QG_NXZ)
        die("quart_biomes section incomplete", path);
}

/* 03_biomes 덤프 → chunk->biomes + 뷰 그리드.
 *
 * 데코가 읽는 진실은 저장된 청크 팔레트다 (union 도 BiomeFilter 의
 * getNoiseBiome 도 — task9a A2 §4). surface golden 의 quart_biomes 는
 * 다른 샘플링 경로의 프로브라 캐시 경계에서 저장값과 1쿼트급 차이가
 * 날 수 있다 (실측: 링에서 24576 중 1) — 그리드 3x3 은 기존 게이트가
 * 검증한 영역이라 strict 교차검증을 유지하고, 링은 덤프 값으로 뷰를
 * 덮어쓰며 차이를 센다 (상한 초과 시 die — fail-loud 유지). */
static int g_ring_quart_diffs = 0;

static void load_chunk_biomes(hc_chunk_t *chunk, const char *dir,
                              int strict) {
    char path[1024];
    snprintf(path, sizeof path, "%s/c.%d.%d/03_biomes.biomes.txt", dir,
             chunk->cx, chunk->cz);
    size_t len = 0;
    char  *buf = read_file(path, &len);
    if (len < 100 || strncmp(buf, "# hyperchunk golden stage dump v1\n", 34))
        die("golden biomes malformed", path);

    int32_t pal[64];
    int     n_pal = 0;
    char   *p = buf;
    int     in_data = 0;
    int     qy = HC_MIN_Y >> 2, qz = 0;
    int     quads = 0;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (!nl)
            break;
        *nl = '\0';
        if (p[0] == '#') {
        } else if (strncmp(p, "palette ", 8) == 0) {
            int  idx;
            char name[64];
            if (sscanf(p, "palette %d %63s", &idx, name) != 2 ||
                idx != n_pal || n_pal >= 64)
                die("bad biome palette line", path);
            pal[n_pal] = hc_biome_intern(&g_reg, name, (int32_t)strlen(name));
            if (pal[n_pal] < 0)
                die("biome registry full", name);
            n_pal++;
        } else if (strcmp(p, "data") == 0) {
            in_data = 1;
        } else if (in_data) {
            const char *q = p;
            for (int qx = 0; qx < 4; qx++) {
                char *end;
                long  v = strtol(q, &end, 10);
                if (end == q || v < 0 || v >= n_pal)
                    die("bad biome data row", path);
                uint16_t id = (uint16_t)pal[v];
                int      gx = chunk->cx * 4 + qx - QG_MIN_XZ;
                int      gz = chunk->cz * 4 + qz - QG_MIN_XZ;
                if (g_grid[qy - QG_MIN_Y][gz][gx] != id) {
                    if (strict)
                        die("quart_biomes vs 03_biomes mismatch", path);
                    g_ring_quart_diffs++;
                    g_grid[qy - QG_MIN_Y][gz][gx] = id; /* 저장값이 진실 */
                }
                chunk->biomes[hc_quart_idx(qx, qy, qz)] = id;
                q = end;
                quads++;
            }
            if (++qz == 4) {
                qz = 0;
                qy++;
            }
        }
        p = nl + 1;
    }
    if (quads != HC_QUARTS)
        die("biome dump incomplete", path);
}

static void load_climate(const char *ref_dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/biome_climate-26.2.json", ref_dir);
    const hc_json_t *tbl = parse_file(path);
    if (tbl->kind != HC_JSON_OBJ || tbl->count < 60)
        die("biome climate table malformed", path);
    for (int32_t id = 0; id < g_reg.count; id++) {
        const hc_json_t *e = hc_json_get(tbl, g_reg.names[id]);
        if (!e || e->kind != HC_JSON_OBJ)
            die("biome missing from climate table", g_reg.names[id]);
        const hc_json_t *t = hc_json_get(e, "temperature");
        const hc_json_t *m = hc_json_get(e, "temperature_modifier");
        if (!t || t->kind != HC_JSON_NUM)
            die("biome climate missing temperature", g_reg.names[id]);
        uint8_t mod = HC_BIOME_TEMP_MOD_NONE;
        if (m && m->kind == HC_JSON_STR && hc_json_streq(m, "frozen"))
            mod = HC_BIOME_TEMP_MOD_FROZEN;
        hc_biome_set_climate(&g_reg, id, (float)t->num, mod);
    }
}

/* --- golden blocks 덤프 로더 (07 비교 + 링 06 로드 겸용) ---
 * 팔레트 항목이 테이블 밖이면 allow_unknown 에 따라 SENTINEL 또는 die. */

enum { B_UNKNOWN = 0xFFFF };

static void load_blocks_dump(const char *path, uint16_t *out,
                             int allow_unknown) {
    size_t len = 0;
    char  *buf = read_file(path, &len);
    if (len < 100 || strncmp(buf, "# hyperchunk golden stage dump v1\n", 34))
        die("golden blocks malformed", path);
    uint16_t pal[512];
    int      n_pal = 0;
    char    *p = buf;
    int      in_data = 0;
    size_t   i = 0;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (!nl)
            break;
        *nl = '\0';
        if (p[0] == '#') {
        } else if (strncmp(p, "palette ", 8) == 0) {
            int  idx;
            char name[128];
            if (sscanf(p, "palette %d %127s", &idx, name) != 2 ||
                idx != n_pal || n_pal >= 512)
                die("bad palette line", path);
            int32_t id = hc_block_by_name(name, (int32_t)strlen(name));
            if (id < 0) {
                if (!allow_unknown)
                    die("block not in hc_blocks table", name);
                pal[n_pal++] = B_UNKNOWN;
            } else {
                pal[n_pal++] = (uint16_t)id;
            }
        } else if (strcmp(p, "data") == 0) {
            in_data = 1;
        } else if (in_data) {
            const char *q = p;
            for (int x = 0; x < 16; x++) {
                char *end;
                long  v = strtol(q, &end, 10);
                if (end == q || v < 0 || v >= n_pal)
                    die("bad block data row", path);
                if (i >= (size_t)HC_BLOCKS)
                    die("block dump overlong", path);
                out[i++] = pal[v];
                q = end;
            }
        }
        p = nl + 1;
    }
    if (i != (size_t)HC_BLOCKS)
        die("block dump incomplete", path);
}

/* 06_carvers.heightmaps.txt → WG 맵 2종 */
static void load_heightmaps_dump(const char *path, hc_chunk_t *c) {
    size_t len = 0;
    char  *buf = read_file(path, &len);
    char  *p = buf;
    int32_t *cur = NULL;
    int      row = 0, filled = 0;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (!nl)
            break;
        *nl = '\0';
        if (p[0] == '#') {
        } else if (strncmp(p, "heightmap ", 10) == 0) {
            if (strcmp(p + 10, "WORLD_SURFACE_WG") == 0)
                cur = c->heightmap_ws;
            else if (strcmp(p + 10, "OCEAN_FLOOR_WG") == 0)
                cur = c->heightmap_ocean_floor;
            else
                cur = NULL; /* FINAL 맵 등 — 무시 */
            row = 0;
        } else if (cur) {
            const char *q = p;
            for (int x = 0; x < 16; x++) {
                char *end;
                long  v = strtol(q, &end, 10);
                if (end == q)
                    die("bad heightmap row", path);
                cur[hc_col_idx(x, row)] = (int32_t)v;
                q = end;
            }
            if (++row == 16) {
                cur = NULL;
                filled++;
            }
        }
        p = nl + 1;
    }
    if (filled < 2)
        die("heightmaps dump missing WG maps", path);
}

/* --- order.manifest 파서 + 게이트 (a) --- */

typedef struct {
    int32_t  cx, cz;
    uint64_t seed_hex;
} manifest_line_t;

static int32_t load_manifest(const char *path, int64_t level_seed,
                             manifest_line_t *out, int32_t cap,
                             const char *label) {
    size_t len = 0;
    char  *buf = read_file(path, &len);
    char  *p = buf;
    int32_t n = 0;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (!nl)
            break;
        *nl = '\0';
        if (p[0] != '#' && p[0] != '\0') {
            long long          seq;
            int32_t            cx, cz;
            unsigned long long hex;
            char               thread[64];
            long long          nanos;
            if (sscanf(p, "%lld %d %d %llx %63s %lld", &seq, &cx, &cz, &hex,
                       thread, &nanos) != 6)
                die("bad manifest line", p);
            if (seq != n)
                die("manifest seq gap", path);
            if (n >= cap)
                die("manifest longer than expected", path);
            out[n].cx = cx;
            out[n].cz = cz;
            out[n].seed_hex = (uint64_t)hex;
            int64_t got = hc_features_decoration_seed(level_seed, cx, cz);
            if ((uint64_t)got != out[n].seed_hex) {
                fprintf(stderr,
                        "GATE(a) FAIL %s seq %d chunk (%d,%d): computed "
                        "%016" PRIx64 " recorded %016" PRIx64 "\n",
                        label, n, cx, cz, (uint64_t)got, out[n].seed_hex);
                g_fails++;
            }
            n++;
        }
        p = nl + 1;
    }
    return n;
}

/* --- 트레이스 싱크: golden 트레이스와 같은 서식의 라인 버퍼 --- */

typedef struct {
    char  *buf;
    size_t len, cap;
} tracebuf_t;

static void tb_printf(tracebuf_t *tb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tb->buf + tb->len, tb->cap - tb->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= tb->cap - tb->len)
        die("trace buffer overflow", NULL);
    tb->len += (size_t)n;
}

static void sink_pos(void *ud, int32_t step, int32_t index, int32_t x,
                     int32_t y, int32_t z, int32_t placed) {
    tb_printf((tracebuf_t *)ud, "p %d %d %d %d %d %d\n", step, index, x, y, z,
              placed);
}

static void sink_feature(void *ud, int32_t step, int32_t index,
                         const char *name, int32_t npos, int32_t placed) {
    tb_printf((tracebuf_t *)ud, "f %d %d %d %d %s\n", step, index, npos,
              placed, name);
}

/* golden 트레이스에서 step<=max 의 p/f 라인만 뽑아 정규화 (p 는 그대로,
 * f 는 같은 필드 순서로) */
static void filter_golden_trace(const char *path, int32_t max_step,
                                uint64_t want_seed, tracebuf_t *tb) {
    size_t len = 0;
    char  *buf = read_file(path, &len);
    char  *p = buf;
    int    saw_begin = 0;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (!nl)
            break;
        *nl = '\0';
        if (strncmp(p, "begin ", 6) == 0) {
            int32_t            cx, cz;
            unsigned long long hex;
            if (sscanf(p, "begin %d %d %llx", &cx, &cz, &hex) != 3 ||
                (uint64_t)hex != want_seed)
                die("trace begin seed mismatch", path);
            saw_begin = 1;
        } else if (p[0] == 'p' && p[1] == ' ') {
            int32_t s, i, x, y, z, pl;
            if (sscanf(p, "p %d %d %d %d %d %d", &s, &i, &x, &y, &z, &pl) != 6)
                die("bad trace p line", path);
            if (s <= max_step)
                tb_printf(tb, "p %d %d %d %d %d %d\n", s, i, x, y, z, pl);
        } else if (p[0] == 'f' && p[1] == ' ') {
            int32_t s, i, np, pl;
            char    name[128];
            if (sscanf(p, "f %d %d %d %d %127s", &s, &i, &np, &pl, name) != 5)
                die("bad trace f line", path);
            if (s <= max_step)
                tb_printf(tb, "f %d %d %d %d %s\n", s, i, np, pl, name);
        } else if (p[0] == 's' && p[1] == ' ') {
            die("structure placement in grid trace — 9a scope broken", path);
        }
        p = nl + 1;
    }
    if (!saw_begin)
        die("trace without begin", path);
}

/* p 라인이 placed01 필드만 다른가? (step,index,x,y,z 동일) — 9a 의 알려진
 * 잔차 클래스: 먼저 데코된 이웃의 step-9(초목) 스필이 바닐라의 후속
 * step<=8 본문이 읽는 셀을 바꾼 경우 (예: moss_block 이 rule test 를
 * 뒤집음). 파이프라인 드로우는 상태 무관이라 위치는 항상 일치해야 하고,
 * placed 비트만 흔들릴 수 있다. 9b 가 step 9 를 구현하면 0 이 된다. */
static int placed_bit_only_diff(const char *a, size_t al, const char *b,
                                size_t bl) {
    if (al != bl || al < 2 || a[0] != 'p' || b[0] != 'p')
        return 0;
    /* 마지막 공백 뒤 한 글자(placed01)만 다르면 참 */
    if (memcmp(a, b, al - 1) != 0)
        return 0;
    return a[al - 1] != b[al - 1];
}

static int g_placed_bit_drifts = 0;

static int diff_traces(const char *what, const char *ours, const char *golden) {
    int line = 1, local = 0;
    const char *a = ours, *b = golden;
    while (*a || *b) {
        const char *an = strchr(a, '\n');
        const char *bn = strchr(b, '\n');
        size_t al = an ? (size_t)(an - a) : strlen(a);
        size_t bl = bn ? (size_t)(bn - b) : strlen(b);
        if (al != bl || memcmp(a, b, al) != 0) {
            if (placed_bit_only_diff(a, al, b, bl)) {
                fprintf(stderr,
                        "GATE(b) placed-bit drift %s line %d (9b step-9 "
                        "spill class): %.*s vs vanilla %.*s\n",
                        what, line, (int)al, a, (int)bl, b);
                g_placed_bit_drifts++;
            } else {
                if (local < 8)
                    fprintf(stderr,
                            "GATE(b) DIFF %s line %d:\n  ours:   %.*s\n  "
                            "vanilla: %.*s\n",
                            what, line, (int)al, *a ? a : "<EOF>", (int)bl,
                            *b ? b : "<EOF>");
                local++;
            }
        }
        a = an ? an + 1 : a + al;
        b = bn ? bn + 1 : b + bl;
        line++;
    }
    if (local)
        fprintf(stderr, "GATE(b) %s: %d differing lines\n", what, local);
    return local;
}

/* --- 월드 상태 --- */

enum { WN = 5, WORLD_CHUNKS = WN * WN }; /* 청크 -2..2 */

typedef struct {
    hc_chunk_t chunks[WORLD_CHUNKS];
    uint16_t  *pristine[WORLD_CHUNKS]; /* 06 상태 스냅샷 (재생 리셋용) */
} world_t;

static int widx(int32_t cx, int32_t cz) {
    assert(cx >= -2 && cx <= 2 && cz >= -2 && cz <= 2);
    return (cz + 2) * WN + (cx + 2);
}

int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr,
                "usage: test_features_walk <ref_dir> <stages_seed_dir> "
                "<stages_alt_seed_dir> <trace_seed_dir> <surface_golden> "
                "<seed>\n");
        return 2;
    }
    const char *ref_dir = argv[1];
    const char *stages_dir = argv[2];
    const char *alt_dir = argv[3];
    const char *trace_dir = argv[4];
    const char *surface_golden = argv[5];
    int64_t     seed = strtoll(argv[6], NULL, 10);

    hc_arena_init(&g_arena, g_backing, sizeof g_backing);
    hc_biome_reg_init(&g_reg, &g_arena);

    char sub[1024];
    snprintf(sub, sizeof sub, "%s/density_function", ref_dir);
    load_tree(g_dfs, &g_n_dfs, MAX_SOURCES, sub, "", 0);
    snprintf(sub, sizeof sub, "%s/noise", ref_dir);
    load_tree(g_noises, &g_n_noises, MAX_SOURCES, sub, "", 0);
    snprintf(sub, sizeof sub, "%s/tags/block", ref_dir);
    load_tree(g_tags, &g_n_tags, MAX_SOURCES, sub, "", 0);
    snprintf(sub, sizeof sub, "%s/placed_feature", ref_dir);
    load_tree(g_placed, &g_n_placed, MAX_FEAT_SOURCES, sub, "", 0);
    snprintf(sub, sizeof sub, "%s/configured_feature", ref_dir);
    load_tree(g_configured, &g_n_configured, MAX_FEAT_SOURCES, sub, "", 0);
    if (g_n_dfs < 19 || g_n_noises < 51 || g_n_tags < 30 ||
        g_n_placed < 200 || g_n_configured < 160)
        die("reference closure incomplete", ref_dir);

    snprintf(sub, sizeof sub, "%s/overworld-26.2.json", ref_dir);
    const hc_json_t *settings = parse_file(sub);
    const hc_json_t *router = hc_json_get(settings, "noise_router");
    if (!router || router->kind != HC_JSON_OBJ || router->count != 15)
        die("noise_router missing", sub);
    const hc_json_t *sea = hc_json_get(settings, "sea_level");
    if (!sea || sea->kind != HC_JSON_NUM)
        die("sea_level missing", sub);

    static const char *CARVER_FILES[3] = {"cave", "cave_extra_underground",
                                          "canyon"};
    hc_carver_t        carvers[3];
    for (int i = 0; i < 3; i++) {
        snprintf(sub, sizeof sub, "%s/carver/%s.json", ref_dir,
                 CARVER_FILES[i]);
        const char *err = NULL;
        if (hc_carver_init(&carvers[i], parse_file(sub), g_tags, g_n_tags,
                           &err) != 0)
            die(err ? err : "carver compile failed", sub);
    }

    hc_df_compiler_t comp;
    hc_df_graph_t    graph;
    if (hc_df_compiler_init(&comp, &graph, &g_arena, seed, g_dfs, g_n_dfs,
                            g_noises, g_n_noises) != 0)
        die("compiler init failed (arena)", NULL);

    hc_noise_roots_t roots;
    memset(&roots, -1, sizeof roots);
    struct {
        const char *name;
        int32_t    *dst;
    } slot_map[] = {
        {"final_density", &roots.final_density},
        {"barrier", &roots.barrier},
        {"fluid_level_floodedness", &roots.fluid_level_floodedness},
        {"fluid_level_spread", &roots.fluid_level_spread},
        {"lava", &roots.lava},
        {"erosion", &roots.erosion},
        {"depth", &roots.depth},
        {"preliminary_surface_level", &roots.preliminary_surface_level},
        {"vein_toggle", &roots.vein_toggle},
        {"vein_ridged", &roots.vein_ridged},
        {"vein_gap", &roots.vein_gap},
    };
    for (const hc_json_t *m = router->child; m; m = m->next) {
        char key[128];
        if (m->klen >= (int32_t)sizeof key)
            die("slot name too long", NULL);
        memcpy(key, m->key, (size_t)m->klen);
        key[m->klen] = '\0';
        int32_t root = hc_df_compile_expr(&comp, m);
        if (root < 0)
            die("router slot compile failed", key);
        for (size_t i = 0; i < sizeof slot_map / sizeof slot_map[0]; i++)
            if (strcmp(slot_map[i].name, key) == 0)
                *slot_map[i].dst = root;
    }
    for (size_t i = 0; i < sizeof slot_map / sizeof slot_map[0]; i++)
        if (*slot_map[i].dst < 0)
            die("router slot missing from JSON", slot_map[i].name);

    hc_surface_t *surf =
        hc_arena_alloc(&g_arena, sizeof *surf, _Alignof(hc_surface_t));
    if (!surf || hc_surface_init(surf, &g_arena, seed, settings, g_noises,
                                 g_n_noises, &g_reg) != 0)
        die("surface init failed", surf ? surf->err : "arena");

    load_quart_grid(surface_golden);

    /* features 레지스트리 (walk step<=8) — 바이옴 인턴 포함 */
    snprintf(sub, sizeof sub, "%s/features_order-26.2.txt", ref_dir);
    char *order_txt = read_file(sub, NULL);
    snprintf(sub, sizeof sub, "%s/biome_features-26.2.json", ref_dir);
    const hc_json_t *biome_features = parse_file(sub);
    hc_feat_reg_t *freg =
        hc_arena_alloc(&g_arena, sizeof *freg, _Alignof(hc_feat_reg_t));
    const char *ferr = NULL;
    if (!freg ||
        hc_feat_reg_init(freg, &g_arena, order_txt, biome_features, g_placed,
                         g_n_placed, g_configured, g_n_configured, g_tags,
                         g_n_tags, &g_reg, 8, &ferr) != 0)
        die(ferr ? ferr : "feature registry init failed", NULL);
    if (freg->counts[6] != 34 || freg->counts[8] != 3)
        die("feature order table sanity failed", NULL);

    load_climate(ref_dir);

    /* --- 게이트 (a): 두 번들 manifest 전 라인 시드 재계산 --- */
    enum { MAX_MANIFEST = 256 };
    static manifest_line_t man_pri[MAX_MANIFEST], man_alt[MAX_MANIFEST];
    char mpath[1024];
    snprintf(mpath, sizeof mpath, "%s/order.manifest", stages_dir);
    int32_t n_pri = load_manifest(mpath, seed, man_pri, MAX_MANIFEST,
                                  "primary");
    snprintf(mpath, sizeof mpath, "%s/order.manifest", alt_dir);
    int32_t n_alt = load_manifest(mpath, seed, man_alt, MAX_MANIFEST, "alt");
    if (n_pri < 81 || n_alt < 81)
        die("manifest unexpectedly short", NULL);
    printf("gate(a): decoration seeds verified: primary %d/%d, alt %d/%d "
           "lines\n",
           n_pri, n_pri, n_alt, n_alt);

    /* --- 월드 구성: 그리드 = 우리 04..06 체인, 링 = golden 로드 --- */
    hc_biome_view_t view;
    view.qx0 = QG_MIN_XZ;
    view.qz0 = QG_MIN_XZ;
    view.nxz = QG_NXZ;
    view.qy0 = QG_MIN_Y;
    view.ny = QG_NY;
    view.ids = &g_grid[0][0][0];
    view.zoom_seed = hc_biome_obfuscate_seed(seed);

    static world_t w;
    for (int32_t cz = -2; cz <= 2; cz++) {
        for (int32_t cx = -2; cx <= 2; cx++) {
            hc_chunk_t *c = &w.chunks[widx(cx, cz)];
            if (hc_chunk_init(c, &g_arena, cx, cz) != 0)
                die("arena exhausted (chunk)", NULL);
            int ring = (cx < -1 || cx > 1 || cz < -1 || cz > 1);
            if (!ring) {
                hc_noise_chunk_t *nc = hc_arena_alloc(
                    &g_arena, sizeof *nc, _Alignof(hc_noise_chunk_t));
                if (!nc ||
                    hc_nc_init(nc, &g_arena, &graph, &roots, seed, cx, cz,
                               (int32_t)sea->num) != 0)
                    die("arena exhausted (noise chunk)", NULL);
                hc_gen_noise_stage(c, nc);
                load_chunk_biomes(c, stages_dir, /*strict=*/1);
                hc_gen_surface_stage(c, nc, surf, &view);
                static uint64_t mask[HC_CARVING_MASK_WORDS];
                memset(mask, 0, sizeof mask);
                hc_gen_carvers_stage(c, nc, surf, &view, seed, carvers, 3,
                                     mask);
            } else {
                load_chunk_biomes(c, trace_dir, /*strict=*/0);
                char bpath[1024];
                snprintf(bpath, sizeof bpath,
                         "%s/c.%d.%d/06_carvers.blocks.txt", trace_dir, cx,
                         cz);
                load_blocks_dump(bpath, c->states, /*allow_unknown=*/0);
                snprintf(bpath, sizeof bpath,
                         "%s/c.%d.%d/06_carvers.heightmaps.txt", trace_dir,
                         cx, cz);
                load_heightmaps_dump(bpath, c);
            }
            w.pristine[widx(cx, cz)] = hc_arena_alloc(
                &g_arena, sizeof(uint16_t) * (size_t)HC_BLOCKS, 2);
            if (!w.pristine[widx(cx, cz)])
                die("arena exhausted (pristine)", NULL);
            memcpy(w.pristine[widx(cx, cz)], c->states,
                   sizeof(uint16_t) * (size_t)HC_BLOCKS);
        }
    }
    if (g_ring_quart_diffs > 4)
        die("too many quart_biomes vs stored-palette diffs in the ring — "
            "biome source drift?",
            NULL);
    printf("world: 9 grid chunks chained 04..06, 16 ring chunks loaded "
           "(ring quart diffs vs surface-golden probe: %d)\n",
           g_ring_quart_diffs);

    /* --- 재생 + 게이트 (b)/(c), 두 번들 --- */
    hc_feat_region_t rg;
    memset(&rg, 0, sizeof rg);
    rg.cx0 = -2;
    rg.cz0 = -2;
    rg.n = WN;
    /* hc_feat_region_t 의 chunks 는 [dz*n+dx] — n=5 로 채운다 */
    for (int32_t cz = -2; cz <= 2; cz++)
        for (int32_t cx = -2; cx <= 2; cx++)
            rg.chunks[(cz + 2) * WN + (cx + 2)] = &w.chunks[widx(cx, cz)];

    int64_t total_placed_blocks = 0;

    for (int bundle = 0; bundle < 2; bundle++) {
        const char            *bname = bundle == 0 ? "primary" : "alt";
        const char            *bdir = bundle == 0 ? stages_dir : alt_dir;
        const manifest_line_t *man = bundle == 0 ? man_pri : man_alt;

        /* 06 상태로 리셋 */
        for (int i = 0; i < WORLD_CHUNKS; i++)
            memcpy(w.chunks[i].states, w.pristine[i],
                   sizeof(uint16_t) * (size_t)HC_BLOCKS);

        /* seq 0..8 = 그리드 9청크 (양 번들 공통 — NOTES.md) */
        static uint16_t snap[9][HC_BLOCKS];
        int32_t         snap_cx[9], snap_cz[9];
        for (int32_t s = 0; s < 9; s++) {
            int32_t cx = man[s].cx, cz = man[s].cz;
            if (cx < -1 || cx > 1 || cz < -1 || cz > 1)
                die("manifest seq 0..8 not the 3x3 grid", bname);

            tracebuf_t tb;
            tb.cap = 1u << 20;
            tb.buf = hc_arena_alloc(&g_arena, tb.cap, 1);
            tb.len = 0;
            if (!tb.buf)
                die("arena exhausted (trace buffer)", NULL);
            tb.buf[0] = '\0';
            hc_feat_trace_t sink = {sink_pos, sink_feature, &tb};

            hc_gen_features_chunk(&rg, cx, cz, seed, freg, &view, &g_reg, 8,
                                  bundle == 0 ? &sink : NULL);
            tb.buf[tb.len] = '\0';

            /* 게이트 (b): PRIMARY 만 (트레이스 골든이 그 순서로 기록됨) */
            if (bundle == 0) {
                tracebuf_t gt;
                gt.cap = 1u << 20;
                gt.buf = hc_arena_alloc(&g_arena, gt.cap, 1);
                gt.len = 0;
                if (!gt.buf)
                    die("arena exhausted (golden trace)", NULL);
                char tpath[1024];
                snprintf(tpath, sizeof tpath, "%s/traces/c.%d.%d.trace.txt",
                         trace_dir, cx, cz);
                filter_golden_trace(tpath, 8, man[s].seed_hex, &gt);
                gt.buf[gt.len] = '\0';
                char what[64];
                snprintf(what, sizeof what, "c.%d.%d", cx, cz);
                g_fails += diff_traces(what, tb.buf, gt.buf);
            }

            /* 자기 데코 완료 시점 스냅샷 (07 덤프 시맨틱) */
            memcpy(snap[s], w.chunks[widx(cx, cz)].states,
                   sizeof(uint16_t) * (size_t)HC_BLOCKS);
            snap_cx[s] = cx;
            snap_cz[s] = cz;
        }

        /* 게이트 (c): 스냅샷 vs golden 07 — 9b 잔차 카테고리만 허용 */
        int64_t hard = 0, res_unknown = 0, res_clay = 0, res_water = 0,
                res_dirt = 0, changed = 0;
        for (int32_t s = 0; s < 9; s++) {
            static uint16_t g7[HC_BLOCKS];
            char            gpath[1024];
            snprintf(gpath, sizeof gpath, "%s/c.%d.%d/07_features.blocks.txt",
                     bdir, snap_cx[s], snap_cz[s]);
            load_blocks_dump(gpath, g7, /*allow_unknown=*/1);
            const uint16_t *g6 =
                w.pristine[widx(snap_cx[s], snap_cz[s])];
            for (size_t i = 0; i < (size_t)HC_BLOCKS; i++) {
                if (snap[s][i] != g6[i])
                    changed++;
                if (snap[s][i] == g7[i])
                    continue;
                /* step 9/10 (초목, 9b) 의 덮어쓰기만 허용 */
                if (g7[i] == B_UNKNOWN) {
                    res_unknown++;
                } else if (g7[i] == HC_B_CLAY) {
                    res_clay++;
                } else if (g7[i] == HC_B_WATER) {
                    res_water++;
                } else if (g7[i] == HC_B_DIRT) {
                    res_dirt++;
                } else {
                    if (hard < 8) {
                        int32_t y = (int32_t)(i / 256) + HC_MIN_Y;
                        int32_t z = (int32_t)((i / 16) % 16);
                        int32_t x = (int32_t)(i % 16);
                        fprintf(stderr,
                                "GATE(c) FAIL %s c.%d.%d (%d,%d,%d): ours=%s "
                                "golden07=%s (06=%s)\n",
                                bname, snap_cx[s], snap_cz[s],
                                snap_cx[s] * 16 + x, y, snap_cz[s] * 16 + z,
                                hc_block_name(snap[s][i]),
                                hc_block_name(g7[i]), hc_block_name(g6[i]));
                    }
                    hard++;
                }
            }
        }
        if (hard) {
            fprintf(stderr, "GATE(c) %s: %" PRId64 " hard mismatches\n",
                    bname, hard);
            g_fails += (int)(hard > 1000 ? 1000 : hard);
        }
        if (changed < 5000)
            die("suspiciously few placed blocks — gate vacuous?", bname);
        total_placed_blocks += changed;
        printf("gate(c) %s: %" PRId64 " blocks placed by walk; residuals "
               "(9b overwrites): %" PRId64 " new-block, %" PRId64
               " clay, %" PRId64 " water, %" PRId64 " dirt; hard fails %"
               PRId64 "\n",
               bname, changed, res_unknown, res_clay, res_water, res_dirt,
               hard);
    }

    /* placed-bit drift 상한: 이 그리드의 실측 잔차는 1 (c.1.1 ore_iron_small
     * — 이웃 step-9 moss 스필). 늘어나면 회귀다. */
    if (g_placed_bit_drifts > 2) {
        fprintf(stderr,
                "GATE(b) FAIL: %d placed-bit drifts (> 2) — new state "
                "divergence beyond the known step-9 spill class\n",
                g_placed_bit_drifts);
        g_fails += g_placed_bit_drifts;
    }
    printf("test_features_walk: %" PRId64 " blocks placed across bundles, "
           "%d placed-bit drifts (9b), %d fails\n",
           total_placed_blocks, g_placed_bit_drifts, g_fails);
    return g_fails == 0 ? 0 : 1;
}
