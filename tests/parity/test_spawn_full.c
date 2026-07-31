/* 10_spawn / 11_full 스테이지 패리티 (Task 11 게이트).
 *
 * 핸드오프 실측 (.hermes/notes/task10-light/A-task10-completion-and-
 * task11-handoff.md) + 이번 런 bytecode 확인 (task11-spawnfull/):
 *  - 09→10 (spawn): 덤프 관측면 변화 0 — blocks/heightmaps/biomes 전부
 *    바이트 동일. C 쪽은 상태 마커만 (hc_gen_spawn_stage).
 *  - 10→11 (full): heightmaps kind 프루닝만. 클라이언트 4종
 *    (WORLD_SURFACE/OCEAN_FLOOR/MOTION_BLOCKING/MOTION_BLOCKING_NO_LEAVES)
 *    생존, 값 바이트 동일 (setRawData 비트 복사 — 전환 시점 프라임/
 *    재계산 없음); *_WG 소멸. blocks/biomes 무변화 (참조 승계). 4종은
 *    FEATURES 진입부 primeHeightmaps 가 항상 프라임해 놓는다 — 재생
 *    쪽도 hc_gen_features_chunk 가 같은 일을 하므로, full 시점 4종
 *    전-프라임은 게이트 불변량으로 검증한다.
 *
 * 게이트 형태: test_light_stages 의 재생 인프라 (order.manifest 프리픽스
 * 재생, seq-9 *_WG 드롭 웨이브, OF_WG 재프라임 모델) 를 그대로 쓰고,
 * order.snapshots 의 10_spawn/11_full 행에서 blocks/heightmaps(존재
 * kind 만)/biomes 를 대조한다. 두 번들 모두. 라이트 덤프는 10/11 에
 * 존재하지 않는다.
 *
 * 잔차 규율: 09 게이트의 링-초목 잔차 (재현 불가능한 기록-서버 autosave
 * 레이스 산물) 가 blocks/hm 에 그대로 이월된다 — RESID 캡은 09 의
 * blocks/hm 캡과 동일 (6 청크덤프), 신규 잔차 0 이 목표 불변량. 어느
 * 칸이든 캡 초과 = FAIL. HC_LIGHT_STRICT=1 = 풀 0-diff (골든 재기록 후
 * 재활성 조건 — 09 게이트와 같은 스위치). */

#undef NDEBUG

#include "../../core/src/hc_carvers.h"
#include "../../core/src/hc_features.h"
#include "../../core/src/hc_sha256.h" /* hc_biome_obfuscate_seed */

#include <assert.h>
#include <dirent.h>
#include <inttypes.h>
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

static unsigned char g_backing[640u << 20];
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

/* --- reference/ 로더 (test_light_stages.c 와 동일 규약; 복사본 —
 * 기존 게이트 파일을 건드리지 않는다) --- */

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

/* --- 쿼트 바이옴 그리드: 밴드 골든 (52x52, 청크 -6..6) --- */

enum { QG_MIN_XZ = -24, QG_NXZ = 52, QG_MIN_Y = -16, QG_NY = 96 };

static uint16_t       g_grid[QG_NY][QG_NXZ][QG_NXZ];
static hc_biome_reg_t g_reg;

static void load_band_grid(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        die("cannot open biome band golden", path);
    char    line[1024];
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
                die("band palette out of order", path);
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
                    die("bad band quart row", path);
                g_grid[cur_qy - QG_MIN_Y][cur_qz][qx] = (uint16_t)pal[v];
                p = end;
            }
            cur_qz++;
            rows++;
        }
    }
    fclose(f);
    if (rows != QG_NY * QG_NXZ)
        die("band quart_biomes incomplete", path);
}

/* biomes 덤프 파서: 03 오버레이(store) 와 10/11 대조(compare) 겸용.
 * store 모드: chunk->biomes 채우고 밴드 그리드 오버레이. compare 모드:
 * chunk->biomes 와의 diff 컬럼 수 반환. */
static int g_band_quart_diffs = 0;

static int64_t walk_biomes_dump(hc_chunk_t *chunk, const char *path,
                                int store) {
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
    int64_t bad = 0;
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
                if (store) {
                    int gx = chunk->cx * 4 + qx - QG_MIN_XZ;
                    int gz = chunk->cz * 4 + qz - QG_MIN_XZ;
                    if (g_grid[qy - QG_MIN_Y][gz][gx] != id) {
                        g_band_quart_diffs++;
                        g_grid[qy - QG_MIN_Y][gz][gx] = id;
                    }
                    chunk->biomes[hc_quart_idx(qx, qy, qz)] = id;
                } else if (chunk->biomes[hc_quart_idx(qx, qy, qz)] != id) {
                    bad++;
                }
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
    return bad;
}

/* 덤프 없는 청크: 밴드 그리드에서 chunk->biomes 채움 */
static void fill_chunk_biomes_from_grid(hc_chunk_t *chunk) {
    for (int qy = HC_MIN_Y >> 2; qy <= HC_MAX_Y >> 2; qy++)
        for (int qz = 0; qz < 4; qz++)
            for (int qx = 0; qx < 4; qx++) {
                int gx = chunk->cx * 4 + qx - QG_MIN_XZ;
                int gz = chunk->cz * 4 + qz - QG_MIN_XZ;
                chunk->biomes[hc_quart_idx(qx, qy, qz)] =
                    g_grid[qy - QG_MIN_Y][gz][gx];
            }
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

/* --- golden 덤프 로더 --- */

static void load_blocks_dump(const char *path, uint16_t *out) {
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
            if (id < 0)
                die("block not in hc_blocks table", name);
            pal[n_pal++] = (uint16_t)id;
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

/* heightmaps 덤프: 파일에 실재하는 kind 만 채운다. 반환 = kind 존재 비트 */
enum { HM6 = 6 };
static const char *const HM_NAMES[HM6] = {
    "WORLD_SURFACE_WG", "WORLD_SURFACE",   "OCEAN_FLOOR_WG",
    "OCEAN_FLOOR",      "MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES",
};
/* 11_full 생존 집합 (ChunkStatus.FINAL_HEIGHTMAPS — 클라이언트 4종) */
static const unsigned HM_CLIENT4 =
    (1u << 1) | (1u << 3) | (1u << 4) | (1u << 5);

static unsigned load_heightmaps(const char *path, int32_t out[HM6][256]) {
    size_t len = 0;
    char  *buf = read_file(path, &len);
    char  *p = buf;
    int32_t *cur = NULL;
    int      row = 0;
    unsigned present = 0;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (!nl)
            break;
        *nl = '\0';
        if (p[0] == '#') {
        } else if (strncmp(p, "heightmap ", 10) == 0) {
            cur = NULL;
            for (int i = 0; i < HM6; i++)
                if (strcmp(p + 10, HM_NAMES[i]) == 0) {
                    cur = out[i];
                    present |= 1u << i;
                }
            if (!cur)
                die("unknown heightmap type in dump", p + 10);
            row = 0;
        } else if (cur) {
            const char *q = p;
            for (int x = 0; x < 16; x++) {
                char *end;
                long  v = strtol(q, &end, 10);
                if (end == q)
                    die("bad heightmap row", path);
                cur[(size_t)(row * 16 + x)] = (int32_t)v;
                q = end;
            }
            if (++row == 16)
                cur = NULL;
        }
        p = nl + 1;
    }
    return present;
}

/* --- order.manifest / order.snapshots --- */

typedef struct {
    int32_t  cx, cz;
    uint64_t seed_hex;
} manifest_line_t;

static int32_t load_manifest(const char *path, int64_t level_seed,
                             manifest_line_t *out, int32_t cap) {
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
            if ((uint64_t)hc_features_decoration_seed(level_seed, cx, cz) !=
                out[n].seed_hex)
                die("decoration seed mismatch (walk gate should cover this)",
                    p);
            n++;
        }
        p = nl + 1;
    }
    return n;
}

typedef struct {
    int      stage; /* 10 또는 11 */
    int32_t  cx, cz;
    int32_t  seq_begin, seq_end;
    int64_t  nanos;
} snap_t;

static int32_t load_snapshots(const char *path, snap_t *out, int32_t cap) {
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
            char      stage[64], thread[64];
            int32_t   cx, cz, sb, se;
            long long nanos;
            if (sscanf(p, "%63s %d %d %d %d %63s %lld", stage, &cx, &cz, &sb,
                       &se, thread, &nanos) != 7)
                die("bad snapshot line", p);
            int st = -1;
            if (strcmp(stage, "10_spawn") == 0)
                st = 10;
            else if (strcmp(stage, "11_full") == 0)
                st = 11;
            if (st > 0) {
                if (n >= cap)
                    die("too many snapshots", path);
                out[n].stage = st;
                out[n].cx = cx;
                out[n].cz = cz;
                out[n].seq_begin = sb;
                out[n].seq_end = se;
                out[n].nanos = nanos;
                n++;
            }
        }
        p = nl + 1;
    }
    /* nanos 오름차순 정렬 (실행 순서) — 삽입 정렬이면 충분 */
    for (int32_t i = 1; i < n; i++) {
        snap_t  key = out[i];
        int32_t j = i - 1;
        while (j >= 0 && out[j].nanos > key.nanos) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return n;
}

/* --- fail-loud 트레이스 싱크 (test_light_stages.c 와 동일) --- */

static void sink_pos_nop(void *ud, int32_t step, int32_t index, int32_t x,
                         int32_t y, int32_t z, int32_t placed) {
    (void)ud;
    (void)step;
    (void)index;
    (void)x;
    (void)y;
    (void)z;
    (void)placed;
}

static void sink_feature_guard(void *ud, int32_t step, int32_t index,
                               const char *name, int32_t npos,
                               int32_t placed) {
    (void)ud;
    if (npos > 0 && placed < 0) {
        char buf[192];
        snprintf(buf, sizeof buf, "%s (step %d index %d, npos %d)", name,
                 step, index, npos);
        die("UNIMPLEMENTED feature body fired in prefix replay", buf);
    }
}

static const hc_feat_trace_t g_guard_sink = {sink_pos_nop,
                                             sink_feature_guard, NULL};

/* --- 월드 --- */

enum { WR = 5, WN = 2 * WR + 1, WORLD_CHUNKS = WN * WN }; /* 청크 -5..5 */

typedef struct {
    hc_chunk_t chunks[WORLD_CHUNKS];
    uint16_t  *pristine[WORLD_CHUNKS];
} world_t;

static world_t g_world;

static int widx(int32_t cx, int32_t cz) {
    assert(cx >= -WR && cx <= WR && cz >= -WR && cz <= WR);
    return (cz + WR) * WN + (cx + WR);
}

static int is_grid(int32_t cx, int32_t cz) {
    return cx >= -1 && cx <= 1 && cz >= -1 && cz <= 1;
}
static int is_ring2(int32_t cx, int32_t cz) {
    int32_t r = abs(cx) > abs(cz) ? abs(cx) : abs(cz);
    return r == 2;
}

/* FINAL 4종 지연-프라임 보장 (09 게이트와 동일 헬퍼) — 10 덤프 대조 전에
 * 필요. 11 은 hc_gen_full_stage 가 이미 프라임한다. */
static void ensure_final_primed(hc_feat_region_t *rg, hc_chunk_t *c) {
    if ((c->hm_final_primed & 0xF) == 0xF)
        return;
    rg->center_cx = c->cx;
    rg->center_cz = c->cz;
    (void)hc_feat_height(rg, HC_HM_OCEAN_FLOOR, c->cx * 16, c->cz * 16);
    (void)hc_feat_height(rg, HC_HM_WORLD_SURFACE, c->cx * 16, c->cz * 16);
    (void)hc_feat_height(rg, HC_HM_MOTION_BLOCKING, c->cx * 16, c->cz * 16);
    (void)hc_feat_height(rg, HC_HM_MOTION_BLOCKING_NO_LEAVES, c->cx * 16,
                         c->cz * 16);
}

int main(int argc, char **argv) {
    if (argc != 8) {
        fprintf(stderr,
                "usage: test_spawn_full <ref_dir> <stages_seed_dir> "
                "<stages_alt_seed_dir> <trace_seed_dir> <surface_golden> "
                "<band_golden> <seed>\n");
        return 2;
    }
    const char *ref_dir = argv[1];
    const char *stages_dir = argv[2];
    const char *alt_dir = argv[3];
    const char *trace_dir = argv[4];
    /* argv[5] surface_golden 미사용 (인자 유지: CMake 일관성) */
    const char *band_golden = argv[6];
    int64_t     seed = strtoll(argv[7], NULL, 10);

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

    load_band_grid(band_golden);

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
                         g_n_tags, &g_reg, /*walk_max_step=*/10, &ferr) != 0)
        die(ferr ? ferr : "feature registry init failed", NULL);

    load_climate(ref_dir);

    /* --- 월드 구성: 11x11 전부 우리 04..06 체인 (09 게이트와 동일) --- */
    hc_biome_view_t view;
    view.qx0 = QG_MIN_XZ;
    view.qz0 = QG_MIN_XZ;
    view.nxz = QG_NXZ;
    view.qy0 = QG_MIN_Y;
    view.ny = QG_NY;
    view.ids = &g_grid[0][0][0];
    view.zoom_seed = hc_biome_obfuscate_seed(seed);

    for (int32_t cz = -WR; cz <= WR; cz++)
        for (int32_t cx = -WR; cx <= WR; cx++) {
            hc_chunk_t *c = &g_world.chunks[widx(cx, cz)];
            if (hc_chunk_init(c, &g_arena, cx, cz) != 0)
                die("arena exhausted (chunk)", NULL);
            char bpath[1024];
            if (is_grid(cx, cz)) {
                snprintf(bpath, sizeof bpath, "%s/c.%d.%d/03_biomes.biomes.txt",
                         stages_dir, cx, cz);
                walk_biomes_dump(c, bpath, /*store=*/1);
            } else if (is_ring2(cx, cz)) {
                snprintf(bpath, sizeof bpath, "%s/c.%d.%d/03_biomes.biomes.txt",
                         trace_dir, cx, cz);
                walk_biomes_dump(c, bpath, /*store=*/1);
            }
        }
    if (g_band_quart_diffs > 8)
        die("too many band-vs-stored quart diffs — probe drift?", NULL);
    for (int32_t cz = -WR; cz <= WR; cz++)
        for (int32_t cx = -WR; cx <= WR; cx++) {
            hc_chunk_t *c = &g_world.chunks[widx(cx, cz)];
            if (!is_grid(cx, cz) && !is_ring2(cx, cz))
                fill_chunk_biomes_from_grid(c);
            g_world.pristine[widx(cx, cz)] = hc_arena_alloc(
                &g_arena, sizeof(uint16_t) * (size_t)HC_BLOCKS, 2);
            if (!g_world.pristine[widx(cx, cz)])
                die("arena exhausted (pristine)", NULL);
            size_t            arena_mark = g_arena.off;
            hc_noise_chunk_t *nc = hc_arena_alloc(
                &g_arena, sizeof *nc, _Alignof(hc_noise_chunk_t));
            if (!nc || hc_nc_init(nc, &g_arena, &graph, &roots, seed, cx, cz,
                                  (int32_t)sea->num) != 0)
                die("arena exhausted (noise chunk)", NULL);
            hc_gen_noise_stage(c, nc);
            hc_gen_surface_stage(c, nc, surf, &view);
            static uint64_t mask[HC_CARVING_MASK_WORDS];
            memset(mask, 0, sizeof mask);
            hc_gen_carvers_stage(c, nc, surf, &view, seed, carvers, 3, mask);
            g_arena.off = arena_mark;
            memcpy(g_world.pristine[widx(cx, cz)], c->states,
                   sizeof(uint16_t) * (size_t)HC_BLOCKS);
        }
    printf("world: %d chunks chained 04..06 (band quart overlay diffs: %d)\n",
           WORLD_CHUNKS, g_band_quart_diffs);

    /* features 리전 */
    hc_feat_region_t rg;
    memset(&rg, 0, sizeof rg);
    rg.cx0 = -WR;
    rg.cz0 = -WR;
    rg.n = WN;
    for (int32_t cz = -WR; cz <= WR; cz++)
        for (int32_t cx = -WR; cx <= WR; cx++)
            rg.chunks[(cz + WR) * WN + (cx + WR)] =
                &g_world.chunks[widx(cx, cz)];

    /* --- 번들 재생 --- */
    enum { MAX_MANIFEST = 128, MAX_SNAPS = 32 };
    static manifest_line_t man[MAX_MANIFEST];
    static snap_t          snaps[MAX_SNAPS];

    for (int bundle = 0; bundle < 2; bundle++) {
        const char *bname = bundle == 0 ? "primary" : "alt";
        const char *bdir = bundle == 0 ? stages_dir : alt_dir;

        char mpath[1024];
        snprintf(mpath, sizeof mpath, "%s/order.manifest", bdir);
        int32_t n_man = load_manifest(mpath, seed, man, MAX_MANIFEST);
        snprintf(mpath, sizeof mpath, "%s/order.snapshots", bdir);
        int32_t n_snap = load_snapshots(mpath, snaps, MAX_SNAPS);
        if (n_man < 81 || n_snap != 18)
            die("manifest/snapshots unexpectedly short", bname);

        int32_t max_prefix = 0;
        for (int32_t i = 0; i < n_snap; i++)
            if (snaps[i].seq_begin > max_prefix)
                max_prefix = snaps[i].seq_begin;
        if (max_prefix >= n_man)
            die("snapshot prefix beyond manifest", bname);

        /* 06 상태로 리셋 (09 게이트와 동일 + promoted) */
        for (int i = 0; i < WORLD_CHUNKS; i++) {
            memcpy(g_world.chunks[i].states, g_world.pristine[i],
                   sizeof(uint16_t) * (size_t)HC_BLOCKS);
            memset(g_world.chunks[i].heightmap_final, 0,
                   sizeof g_world.chunks[i].heightmap_final);
            g_world.chunks[i].hm_final_primed = 0;
            g_world.chunks[i].wg_reprimed = 0;
            g_world.chunks[i].promoted = 0;
        }
        rg.wg_dropped = 0;

        int32_t next_snap = 0;
        int64_t bundle_fails = 0;
        int32_t bundle_resid = 0;

        for (int32_t pos = 0; pos <= max_prefix; pos++) {
            while (next_snap < n_snap && snaps[next_snap].seq_begin == pos) {
                const snap_t *sn = &snaps[next_snap++];
                hc_chunk_t   *c = &g_world.chunks[widx(sn->cx, sn->cz)];
                char gpath[1024];
                const char *sname =
                    sn->stage == 10 ? "10_spawn" : "11_full";

                /* 스테이지 본문 — 부기 불변량 fail-loud */
                if (sn->stage == 10) {
                    if (c->promoted != 0)
                        die("spawn on already-promoted chunk", sname);
                    hc_gen_spawn_stage(c);
                } else {
                    if (c->promoted != 10)
                        die("full without spawn", sname);
                    hc_gen_full_stage(c);
                }

                /* (1) blocks */
                static uint16_t gblocks[HC_BLOCKS];
                snprintf(gpath, sizeof gpath, "%s/c.%d.%d/%s.blocks.txt",
                         bdir, sn->cx, sn->cz, sname);
                load_blocks_dump(gpath, gblocks);
                int64_t bbad = 0;
                for (size_t i = 0; i < (size_t)HC_BLOCKS; i++)
                    if (c->states[i] != gblocks[i]) {
                        if (bbad < 12) {
                            int32_t y = (int32_t)(i / 256) + HC_MIN_Y;
                            int32_t z = (int32_t)((i / 16) % 16);
                            int32_t x = (int32_t)(i % 16);
                            fprintf(stderr,
                                    "BLOCKS %s %s c.%d.%d (%d,%d,%d): ours=%s "
                                    "golden=%s\n",
                                    bname, sname, sn->cx, sn->cz,
                                    sn->cx * 16 + x, y, sn->cz * 16 + z,
                                    hc_block_name(c->states[i]),
                                    hc_block_name(gblocks[i]));
                        }
                        bbad++;
                    }

                /* (2) biomes — 03 이후 무변화 (pure passthrough) */
                snprintf(gpath, sizeof gpath, "%s/c.%d.%d/%s.biomes.txt",
                         bdir, sn->cx, sn->cz, sname);
                int64_t iobad = walk_biomes_dump(c, gpath, /*store=*/0);
                if (iobad)
                    fprintf(stderr, "BIOMES %s %s c.%d.%d: %" PRId64
                                    " quart diffs\n",
                            bname, sname, sn->cx, sn->cz, iobad);

                /* (3) heightmaps — golden 에 실재하는 kind 만.
                 * 11_full: kind 집합이 정확히 클라이언트 4종이어야 한다
                 * (프루닝 게이트 — golden 쪽도 여기서 검증된다). */
                static int32_t ghm[HM6][256];
                snprintf(gpath, sizeof gpath, "%s/c.%d.%d/%s.heightmaps.txt",
                         bdir, sn->cx, sn->cz, sname);
                unsigned present = load_heightmaps(gpath, ghm);
                if (sn->stage == 11) {
                    if (present != HM_CLIENT4)
                        die("11_full heightmap kinds != client 4", gpath);
                    /* 생존 집합 = 프라임된 FINAL 비트 — FEATURES 가 4종
                     * 전부 프라임했어야 golden 의 4 kind 와 집합 일치 */
                    if (c->promoted != 11 ||
                        (c->hm_final_primed & 0xF) != 0xF)
                        die("full: FINAL kinds not all primed by features",
                            gpath);
                }
                ensure_final_primed(&rg, c);
                int64_t hmbad = 0, ofwg_resid = 0;
                for (int t = 0; t < HM6; t++) {
                    if (!(present & (1u << t)))
                        continue;
                    const int32_t *ours = NULL;
                    switch (t) {
                    case 0: ours = c->heightmap_ws; break;
                    case 1:
                        ours = c->heightmap_final[HC_HMF_WORLD_SURFACE];
                        break;
                    case 2: ours = c->heightmap_ocean_floor; break;
                    case 3:
                        ours = c->heightmap_final[HC_HMF_OCEAN_FLOOR];
                        break;
                    case 4:
                        ours = c->heightmap_final[HC_HMF_MOTION_BLOCKING];
                        break;
                    case 5:
                        ours = c->heightmap_final
                                   [HC_HMF_MOTION_BLOCKING_NO_LEAVES];
                        break;
                    }
                    /* 리로드된 청크(WS_WG 부재)의 OF_WG: 09 게이트와 동일한
                     * 재프라임 모델 대조 + 잔차 상한 4 (R<seqBegin 랙). */
                    int is_reprimed_ofwg = t == 2 && !(present & 1u);
                    if (is_reprimed_ofwg)
                        ours = (c->wg_reprimed & 1u)
                                   ? c->heightmap_wg_reprimed[0]
                                   : c->heightmap_final[HC_HMF_OCEAN_FLOOR];
                    for (int col = 0; col < 256; col++) {
                        if (ours[col] == ghm[t][col])
                            continue;
                        if (is_reprimed_ofwg) {
                            ofwg_resid++;
                            continue;
                        }
                        if (hmbad < 8)
                            fprintf(stderr,
                                    "HM %s %s c.%d.%d %s (%d,%d): ours=%d "
                                    "golden=%d\n",
                                    bname, sname, sn->cx, sn->cz, HM_NAMES[t],
                                    col & 15, col >> 4, ours[col],
                                    ghm[t][col]);
                        hmbad++;
                    }
                }
                if (ofwg_resid > 4) {
                    fprintf(stderr,
                            "HM %s %s c.%d.%d: OF_WG reprime residual %" PRId64
                            " columns (> cap 4)\n",
                            bname, sname, sn->cx, sn->cz, ofwg_resid);
                    hmbad += ofwg_resid;
                } else if (ofwg_resid) {
                    printf("note %s %s c.%d.%d: OF_WG reprime residual %" PRId64
                           " column(s) (R<seqBegin lag, documented)\n",
                           bname, sname, sn->cx, sn->cz, ofwg_resid);
                }

                int64_t total = bbad + iobad + hmbad;
                /* --- 이월 잔차 게이트 (09 RESID 의 blocks/hm 캡 그대로 —
                 * 6 청크덤프의 링-초목 잔차가 10/11 로 순수 이월된다는
                 * 핸드오프 불변량. biomes 는 캡 없음 (항상 0 요구).
                 * 캡 초과 = FAIL, 신규 잔차 청크 = FAIL. --- */
                static const struct {
                    int8_t  bundle, cx, cz;
                    int32_t cap[2]; /* blocks, hm — 09 RESID 와 동일 */
                } RESID[] = {
                    {0, -1, -1, {1, 1}},
                    {0, 0, -1, {78, 39}},
                    {0, 1, -1, {11, 10}},
                    {1, -1, -1, {1, 1}},
                    {1, 0, -1, {87, 39}},
                    {1, 1, -1, {53, 37}},
                };
                int64_t counted = total;
                if (total && !iobad && !getenv("HC_LIGHT_STRICT")) {
                    for (size_t ri = 0; ri < sizeof RESID / sizeof *RESID;
                         ri++) {
                        if (RESID[ri].bundle != bundle ||
                            RESID[ri].cx != sn->cx || RESID[ri].cz != sn->cz)
                            continue;
                        if (bbad <= RESID[ri].cap[0] &&
                            hmbad <= RESID[ri].cap[1])
                            counted = 0; /* 문서화 잔차 이내 */
                        break;
                    }
                }
                bundle_fails += counted;
                if (!counted && total)
                    bundle_resid++;
                printf("%s %s c.%d.%d @seq%d: blocks %" PRId64
                       " biomes %" PRId64 " hm %" PRId64 "%s\n",
                       bname, sname, sn->cx, sn->cz, sn->seq_begin, bbad,
                       iobad, hmbad,
                       counted ? "  <-- FAIL"
                       : total ? "  (within inherited 09 residual caps)"
                               : "");
            }

            /* manifest 엔트리 pos 적용. seq 9 직전 = 기록 서버의 저장/
             * 언로드 웨이브 — *_WG 하이트맵 드롭 (09 게이트와 동일). */
            if (pos == 9)
                rg.wg_dropped = 1;
            if (pos < max_prefix)
                hc_gen_features_chunk(&rg, man[pos].cx, man[pos].cz, seed,
                                      freg, &view, &g_reg, (int32_t)sea->num,
                                      /*walk_max_step=*/10, &g_guard_sink);
        }
        assert(next_snap == n_snap);
        if (bundle_fails) {
            fprintf(stderr, "BUNDLE %s: %" PRId64 " total cell mismatches\n",
                    bname, bundle_fails);
            g_fails += (int)(bundle_fails > 100000 ? 100000 : bundle_fails);
        } else if (bundle_resid) {
            printf("BUNDLE %s: %d/18 dump gates 0-diff, %d within inherited "
                   "09 residual caps (see RESID table)\n",
                   bname, 18 - bundle_resid, bundle_resid);
        } else {
            printf("BUNDLE %s: all 18 dump gates clean\n", bname);
        }
    }

    printf("test_spawn_full: %d fails\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
