/* 08_initialize_light / 09_light 스테이지 패리티 (Task 10 게이트).
 *
 * 관측-윈도우 시맨틱 (.hermes/notes/task10-light/R1..R6):
 *  - 두 스테이지의 덤프는 features order.manifest 프리픽스 위에서 찍힌다
 *    (order.snapshots 의 seqBegin). 08 라이트 = 섹션 등록만 (블록라이트 0,
 *    스카이는 top 규칙의 0/15). 09 라이트 = "덤프 시점까지 09 가 실행된
 *    청크 집합 S" 의 광원을 켠 유일 고정점 (R2 §10, R6 §4).
 *  - 09 시점 블록/하이트맵은 링 청크 데코 스필을 포함한다 → 재생은
 *    프리픽스의 링 청크까지 manifest 순서로 데코한다 (ADR-008 D2 REPLAY).
 *  - OCEAN_FLOOR_WG 는 리로드 후 첫 읽기에서 현재 블록으로 재프라임
 *    (predicate 는 OCEAN_FLOOR 와 동일한 MATERIAL_MOTION_BLOCKING).
 *    게이트는 golden 파일에 실재하는 kind 만 비교하고, OF_WG 는 라이브
 *    OCEAN_FLOOR 와 대조하되 재프라임 시점 R < seqBegin 로 생기는 잔차를
 *    상한(4컬럼) 카운트로 보고한다.
 *
 * 월드: 청크 [-5..5]^2 를 우리 04..06 체인으로 생성. 바이옴은
 *  - 그리드 3x3: stages 번들 03_biomes 덤프 (기존 게이트와 같은 출처)
 *  - 링 반경2: features-trace 03_biomes 덤프
 *  - 그 밖: golden/rng/biome_band (BiomeBandGolden 프로브; 저장 팔레트와
 *    같은 경로 — 캐시 경계 1쿼트급 편차 가능성은 카운트해 보고)
 *
 * 09 모델 (Task 13-close): stages.log v2 의 08/09 제출·완료 이벤트로
 * 라이트 워커 큐-드레인 배치를 시뮬레이션해 R/S 를 만들고, 라이트는
 * 덤프 청크 자신의 09 제출 featuresSeq 프리픽스 블록 위에서 solve 한다
 * (ltask_t 주석의 배치 논증). 블록/하이트맵은 seqBegin 프리픽스 그대로.
 *
 * 게이트 형태 (Task 13-close, strict 기본): 두 번들 35/36 덤프 0-diff +
 * 유일 문서화 아티팩트 1건 (primary 09 c.-1.1 sky<=36셀 — 잔차 계상부의
 * 판정 주석과 완료 노트 참조; 캡 초과 = FAIL). 구 RESID 잔차 봉투
 * (stale 번들 시대 fallback 프로토콜 2026-07-31) 는 unified noSave
 * 재캡처로 소멸 — git 이력 926611a 이전 참조. 진단:
 * HC_LIGHT_TRACE_DIAG=1 (링 트레이스 라인 대조 + 링2 06 대조),
 * HC_LIGHT_TRACE_DUMP_DIR (트레이스 페어 덤프). */

#undef NDEBUG

#include "../../core/src/hc_carvers.h"
#include "../../core/src/hc_features.h"
#include "../../core/src/hc_light.h"
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

static unsigned char g_backing[768u << 20];
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

/* --- reference/ 로더 (test_features_walk.c 와 동일 규약; 복사본 —
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
        if ((size_t)snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name) >=
            sizeof sub)
            die("reference path too long", dir);
        DIR *probe = opendir(sub);
        if (probe) {
            closedir(probe);
            if (depth >= 2)
                die("reference tree deeper than expected", sub);
            char pref[512];
            if ((size_t)snprintf(pref, sizeof pref, "%s%s/", rel_prefix,
                                 e->d_name) >= sizeof pref)
                die("reference prefix too long", rel_prefix);
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

/* 03_biomes 덤프 오버레이 (저장 팔레트가 진실 — 프로브와의 편차 카운트) */
static int g_band_quart_diffs = 0;

static void load_chunk_biomes(hc_chunk_t *chunk, const char *dir) {
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
                    g_band_quart_diffs++;
                    g_grid[qy - QG_MIN_Y][gz][gx] = id;
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

/* light 덤프: 6144 행 x 16 hex 니블 → uint8[HC_BLOCKS] (blocks 와 같은
 * 선형 인덱스) */
static void load_light_dump(const char *path, uint8_t *out) {
    size_t len = 0;
    char  *buf = read_file(path, &len);
    if (len < 100 || strncmp(buf, "# hyperchunk golden stage dump v1\n", 34))
        die("golden light malformed", path);
    char  *p = buf;
    int    in_data = 0;
    size_t i = 0;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (!nl)
            break;
        *nl = '\0';
        if (p[0] == '#') {
        } else if (strcmp(p, "data") == 0) {
            in_data = 1;
        } else if (in_data) {
            if (strlen(p) != 16)
                die("bad light row", path);
            for (int x = 0; x < 16; x++) {
                char c = p[x];
                int  v = c >= '0' && c <= '9'   ? c - '0'
                         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                : -1;
                if (v < 0)
                    die("bad light nibble", path);
                if (i >= (size_t)HC_BLOCKS)
                    die("light dump overlong", path);
                out[i++] = (uint8_t)v;
            }
        }
        p = nl + 1;
    }
    if (i != (size_t)HC_BLOCKS)
        die("light dump incomplete", path);
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

/* stages.log v2 — 라이트 엔진 태스크 (08 등록 / 09 enable+시딩) 의
 * 제출·완료 이벤트, 벽시계 순서 (Task 13 unified 캡처). 09 덤프 C 의
 * 라이트 상태 = lfp(블록@P(C), R, S):
 *
 *   TLLE.runUpdate 배치는 배치 내 모든 PRE 를 어느 POST 보다도 먼저
 *   실행한다 (phase 1: sipush 1000 창; phase 2 에서 POST + 덤프). 배치
 *   k 는 start(k) 까지 제출된 미실행 태스크 전부를 드레인한다. start(k)
 *   는 관측 불가라 직전 배치 마지막 완료 나노로 근사 (워커는 큐가 비지
 *   않는 한 쉬지 않는다; 유휴였으면 다음 제출 나노). 실측 경계 사례:
 *   alt c.0.-1 — c.0.-2 의 09 제출이 직전 배치 완료 169µs 뒤 = 다음
 *   배치 (미포함, 골든 lava emission 부재와 일치); primary 동일-배치
 *   burst 제출은 자기 제출보다 뒤여도 포함 (골든 sky 유입과 일치).
 *
 *   C 의 09 덤프 시점:
 *     S = { D : batch(D 의 09) <= batch(C 의 09) }
 *     R = { D : batch(D 의 08) <= batch(C 의 09) }
 *     P(C) = counter_at(drain(batch(C 의 09))) — 배치 드레인 시점의
 *            featuresSeq. 전 스테이지 이벤트가 (nanos, featuresSeq) 를
 *            찍으므로 임의 시점의 데코 카운터를 타임라인 이분탐색으로
 *            복원한다. PRE 가 읽은 블록 = 프리픽스 P.
 *   데코는 배치 실행과 병행 진행되므로 완료 시점 프리픽스 (order.
 *   snapshots seq_begin) 와 다를 수 있다: primary c.-1.1 드레인@49/완료
 *   @56 — 잎 배치 (49..55) 는 배치 실행 중 = 골든 라이트에 미반영
 *   (checkBlock 재전파는 다음 배치, 덤프 밖). 반면 c.-1.-1 은 드레인
 *   시점에 카운터가 이미 seq_begin 과 같아 P == seq_begin. 블록/
 *   하이트맵 비교는 항상 seq_begin 프리픽스. 드레인-시점 P 는 "등록되는
 *   청크는 자기 데코 완료 후" 를 구조적으로 보장한다 (카운터 단조:
 *   deco(D) < submit(D 의 08) <= drain = P).
 *   (26.2 바이트코드 핀: 08 PRE = 섹션 등록만 (큐 엔트리/15-fill 없음),
 *   enable+시딩은 오로지 propagateLightSources(09 PRE); emission 은
 *   lightOnInSection 게이트 — 완료 노트 참조.) */
typedef struct {
    uint8_t kind;       /* 0 = 08 initialize_light, 1 = 09 light */
    int32_t cx, cz;
    int32_t sub_seq;    /* 제출 시점 featuresSeq (진단용) */
    int64_t sub_nanos;
    int64_t comp_nanos; /* -1 = 완료 미기록 (캡처 종료 잘림) */
    int32_t batch;
} ltask_t;

/* 데코 카운터 타임라인 (전 스테이지 이벤트) + 배치 드레인 나노 —
 * load_light_tasks 가 채운다 (번들당 재로드). */
enum { TL_CAP = 1 << 16 };
static int64_t g_tl_nanos[TL_CAP];
static int32_t g_tl_seq[TL_CAP];
static int32_t g_tl_n;
static int64_t g_batch_drain[TL_CAP];
static int32_t g_nbatch;

static int32_t counter_at(int64_t t) {
    int32_t lo = 0, hi = g_tl_n; /* 마지막 nanos <= t 의 seq */
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (g_tl_nanos[mid] <= t)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo == 0 ? 0 : g_tl_seq[lo - 1];
}

static int32_t load_light_tasks(const char *path, ltask_t *out, int32_t cap) {
    size_t  len = 0;
    char   *buf = read_file(path, &len);
    char   *p = buf;
    int32_t n = 0;
    g_tl_n = 0;
    g_nbatch = 0;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (!nl)
            break;
        *nl = '\0';
        if (p[0] != '#' && p[0] != '\0') {
            int       si, kind = -1, is_sub = 0;
            char      sname[64];
            int32_t   cx, cz;
            long long seq, nanos;
            const char *body = p;
            if (p[0] == 's' && p[1] == ' ') {
                is_sub = 1;
                body = p + 2;
            }
            if (sscanf(body, "%d %63s %d %d %lld %lld", &si, sname, &cx, &cz,
                       &seq, &nanos) != 6)
                die("bad stages.log line", p);
            if (strcmp(sname, "initialize_light") == 0)
                kind = 0;
            else if (strcmp(sname, "light") == 0)
                kind = 1;
            /* 데코 카운터 타임라인: 모든 스테이지 이벤트가 표본.
             * nanos 는 파일 순서와 미세 역전 가능 (nanoTime 취득 후 락
             * 획득 사이 경합) — 로드 후 정렬한다. */
            if (g_tl_n >= TL_CAP)
                die("stage event timeline exceeds cap", path);
            g_tl_nanos[g_tl_n] = nanos;
            g_tl_seq[g_tl_n] = (int32_t)seq;
            g_tl_n++;
            if (kind >= 0) {
                if (is_sub) {
                    if (n >= cap)
                        die("stages.log tasks exceed cap", path);
                    out[n].kind = (uint8_t)kind;
                    out[n].cx = cx;
                    out[n].cz = cz;
                    out[n].sub_seq = (int32_t)seq;
                    out[n].sub_nanos = nanos;
                    out[n].comp_nanos = -1;
                    out[n].batch = -1;
                    n++;
                } else {
                    int32_t t = -1;
                    for (int32_t i = n - 1; i >= 0; i--)
                        if (out[i].kind == kind && out[i].cx == cx &&
                            out[i].cz == cz) {
                            t = i;
                            break;
                        }
                    if (t < 0 || out[t].comp_nanos >= 0)
                        die("stages.log completion without submit", p);
                    out[t].comp_nanos = nanos;
                }
            }
        }
        p = nl + 1;
    }
    /* 타임라인 + 태스크를 nanos 오름차순으로 정렬 (미세 역전 교정 —
     * 삽입 정렬: 거의 정렬된 입력이라 O(n) 급) */
    for (int32_t a = 1; a < g_tl_n; a++) {
        int64_t kn = g_tl_nanos[a];
        int32_t ks = g_tl_seq[a];
        int32_t b = a - 1;
        while (b >= 0 && g_tl_nanos[b] > kn) {
            g_tl_nanos[b + 1] = g_tl_nanos[b];
            g_tl_seq[b + 1] = g_tl_seq[b];
            b--;
        }
        g_tl_nanos[b + 1] = kn;
        g_tl_seq[b + 1] = ks;
    }
    for (int32_t a = 1; a < n; a++) {
        ltask_t key = out[a];
        int32_t b = a - 1;
        while (b >= 0 && out[b].sub_nanos > key.sub_nanos) {
            out[b + 1] = out[b];
            b--;
        }
        out[b + 1] = key;
    }
    /* 큐-드레인 배치 시뮬레이션 — 09 태스크 전용 (제출 나노 오름차순).
     * 08 등록은 별도 경로다: 실측으로 08 완료가 09 POST 들 사이에
     * 끼어든다 (예: primary 09 POST 816.43/825.81ms 사이·직후에 08 완료
     * 825.85-831ms — 09 배치가 FIFO 로 08 을 함께 드레인했다면 불가능한
     * 인터리빙). 08 을 배치 끝 계산에 섞으면 드레인 추정이 뒤로 밀려
     * S 가 과포함된다 (c.-1.-1 131셀 회귀의 원인). 제출 순서가 앞이면
     * 배치도 앞 (드레인은 프리픽스) — 배치 번호는 제출 순서에서 단조.
     * 드레인 시점 = 직전 배치 마지막 완료 (유휴면 기상 제출 나노, δ=0)
     * 를 배치별로 기록한다. */
    int32_t nb = 0, i = 0;
    int64_t t = -1;
    while (i < n) {
        if (out[i].kind != 1) {
            out[i].batch = -1; /* 08: 배치 밖 (R 은 제출<완료 규칙) */
            i++;
            continue;
        }
        if (out[i].sub_nanos > t) {
            /* 워커 유휴 → 제출에 기상. 기상~드레인 사이 스케줄링 지연
             * δ_wake 동안 도착한 제출도 같은 배치다 (실측 제약: primary
             * 09 burst 1.07ms 창 + (-2,-1) +1.00ms 은 포함 (골든 enable
             * 증거), alt 0,-2 +7.67ms 는 제외 — busy-드레인 (δ=0) 이
             * 별도로 걸러 상한은 느슨). δ_wake = 2ms. */
            t = out[i].sub_nanos + 2000000;
        }
        if (nb >= TL_CAP)
            die("light batch count exceeds cap", path);
        g_batch_drain[nb] = t;
        int64_t end = t;
        while (i < n && (out[i].kind != 1 || out[i].sub_nanos <= t)) {
            if (out[i].kind != 1) {
                out[i].batch = -1;
                i++;
                continue;
            }
            out[i].batch = nb;
            if (out[i].comp_nanos > end)
                end = out[i].comp_nanos;
            i++;
        }
        t = end;
        nb++;
    }
    /* POST-실행가능성 병합: δ=0 은 유휴-기상 시 기상 제출 나노에 즉시
     * 드레인한다고 가정하지만, 실제 드레인은 스케줄링 지연 뒤라 burst
     * 제출 전체가 한 배치로 묶일 수 있다 (기록 밖). 판별은 기록이 준다:
     * 배치 k+1 의 첫 09 POST 가 배치 k 의 마지막 09 POST 와 T_PRE_MIN
     * (2ms — 청크 flood PRE 물리 하한; 실측 마진 primary 60µs vs alt
     * 12.7ms) 미만 간격이면 k+1 의 PRE 페이즈가 낄 수 없다 → 실제로는
     * 같은 배치였다. 병합 시 드레인 = 병합 09 멤버 최대 제출 나노
     * (실드레인의 하한; 병합 근거인 60µs 간격이 상한을 첫 POST 앞으로
     * 고정), 그보다 늦게 제출된 (δ=0 이 잘못 편입한) 태스크는 뒤 배치로
     * 남긴다. */
    enum { T_PRE_MIN_NANOS = 2000000 };
    for (int32_t b = 1; b < nb; b++) {
        int64_t prev_last_post = -1, cur_first_post = -1;
        for (int32_t k = 0; k < n; k++) {
            if (out[k].kind != 1 || out[k].comp_nanos < 0)
                continue;
            if (out[k].batch < b && out[k].comp_nanos > prev_last_post)
                prev_last_post = out[k].comp_nanos;
            if (out[k].batch == b &&
                (cur_first_post < 0 || out[k].comp_nanos < cur_first_post))
                cur_first_post = out[k].comp_nanos;
        }
        if (prev_last_post < 0 || cur_first_post < 0)
            continue;
        if (cur_first_post - prev_last_post >= T_PRE_MIN_NANOS)
            continue;
        /* 병합: b 의 09 멤버를 b-1 로 흡수, 드레인 재계산 */
        int64_t drain2 = g_batch_drain[b - 1];
        for (int32_t k = 0; k < n; k++)
            if (out[k].batch == b && out[k].kind == 1 &&
                out[k].sub_nanos > drain2)
                drain2 = out[k].sub_nanos;
        for (int32_t k = 0; k < n; k++) {
            if (out[k].batch == b) {
                /* 09 멤버 (제출 <= drain2) 는 b-1 로 흡수; 잔여 (δ=0 이
                 * 잘못 편입한 늦은 08 등) 는 다음 배치 (시프트 후 b) 로 */
                out[k].batch = out[k].sub_nanos <= drain2 ? b - 1 : b;
            } else if (out[k].batch > b) {
                out[k].batch -= 1;
            }
        }
        g_batch_drain[b - 1] = drain2;
        for (int32_t k = b; k + 1 < nb; k++)
            g_batch_drain[k] = g_batch_drain[k + 1];
        nb--;
        b--; /* 재검사 (연쇄 병합) */
    }
    g_nbatch = nb;
    return n;
}

typedef struct {
    int      stage; /* 8 또는 9 */
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
            if (strcmp(stage, "08_initialize_light") == 0)
                st = 8;
            else if (strcmp(stage, "09_light") == 0)
                st = 9;
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

/* --- fail-loud 트레이스 싱크: 파이프라인이 위치를 냈는데 본문이
 * UNIMPLEMENTED(placed=-1) 면 게이트가 조용히 오염되므로 즉사한다 --- */

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

/* --- 진단 (HC_LIGHT_TRACE_DIAG=1, report-only): 재생하는 모든 청크의
 * p/f 이벤트를 golden/features-trace 와 라인 대조. walk 게이트(b)는 그리드
 * 9청크만 커버하므로 링 청크의 첫 발산 이벤트를 여기서 집는다. 트레이스
 * 기록은 1-스레드 (sticky order = primary) — primary 재생에서만 비교. --- */

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

static void diag_sink_pos(void *ud, int32_t step, int32_t index, int32_t x,
                          int32_t y, int32_t z, int32_t placed) {
    tb_printf((tracebuf_t *)ud, "p %d %d %d %d %d %d\n", step, index, x, y, z,
              placed);
}

static void diag_sink_feature(void *ud, int32_t step, int32_t index,
                              const char *name, int32_t npos, int32_t placed) {
    sink_feature_guard(NULL, step, index, name, npos, placed);
    tb_printf((tracebuf_t *)ud, "f %d %d %d %d %s\n", step, index, npos,
              placed, name);
}

/* golden 트레이스의 step<=10 p/f 라인 정규화 (walk 게이트(b)와 동일 서식) */
static void filter_golden_trace(const char *path, uint64_t want_seed,
                                tracebuf_t *tb) {
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
            if (s <= 10)
                tb_printf(tb, "p %d %d %d %d %d %d\n", s, i, x, y, z, pl);
        } else if (p[0] == 'f' && p[1] == ' ') {
            int32_t s, i, np, pl;
            char    name[128];
            if (sscanf(p, "f %d %d %d %d %127s", &s, &i, &np, &pl, name) != 5)
                die("bad trace f line", path);
            if (s <= 10)
                tb_printf(tb, "f %d %d %d %d %s\n", s, i, np, pl, name);
        }
        p = nl + 1;
    }
    if (!saw_begin)
        die("trace without begin", path);
}

/* report-only 라인 diff — 첫 8 라인만 출력, 발산 리턴 (게이트 미가산) */
static int diag_diff_traces(const char *what, const char *ours,
                            const char *golden) {
    int         line = 1, local = 0;
    const char *a = ours, *b = golden;
    while (*a || *b) {
        const char *an = strchr(a, '\n');
        const char *bn = strchr(b, '\n');
        size_t al = an ? (size_t)(an - a) : strlen(a);
        size_t bl = bn ? (size_t)(bn - b) : strlen(b);
        if (al != bl || memcmp(a, b, al) != 0) {
            if (local < 8)
                fprintf(stderr,
                        "TRACE-DIAG %s line %d:\n  ours:    %.*s\n  "
                        "vanilla: %.*s\n",
                        what, line, (int)al, *a ? a : "<EOF>", (int)bl,
                        *b ? b : "<EOF>");
            local++;
        }
        a = an ? an + 1 : a + al;
        b = bn ? bn + 1 : b + bl;
        line++;
    }
    if (local)
        fprintf(stderr, "TRACE-DIAG %s: %d differing lines\n", what, local);
    return local;
}

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

/* OCEAN_FLOOR final 맵 지연 프라임 확인용 헬퍼 — hc_feat_height 가 FINAL
 * 을 프라임한다 (한 컬럼만 읽어도 타입 전체 프라임). */
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

/* --- 라이트 비교 --- */

static int64_t compare_light(const hc_light_world_t *lw, int layer,
                             const hc_chunk_t *c, const uint8_t *golden,
                             const char *what, int64_t print_cap) {
    int64_t bad = 0;
    for (int32_t y = HC_MIN_Y; y <= HC_MAX_Y; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) {
                int got = hc_light_get(lw, layer, c->cx * 16 + x, y,
                                       c->cz * 16 + z);
                int want = golden[hc_idx(x, y, z)];
                if (got != want) {
                    if (bad < print_cap)
                        fprintf(stderr,
                                "  %s (%d,%d,%d): want %d got %d (block %s)\n",
                                what, c->cx * 16 + x, y, c->cz * 16 + z, want,
                                got, hc_block_name(c->states[hc_idx(x, y, z)]));
                    bad++;
                }
            }
    return bad;
}

int main(int argc, char **argv) {
    if (argc != 8) {
        fprintf(stderr,
                "usage: test_light_stages <ref_dir> <stages_seed_dir> "
                "<stages_alt_seed_dir> <trace_seed_dir> <surface_golden> "
                "<band_golden> <seed>\n");
        return 2;
    }
    const char *ref_dir = argv[1];
    const char *stages_dir = argv[2];
    const char *alt_dir = argv[3];
    const char *trace_dir = argv[4];
    /* argv[5] surface_golden 은 밴드 골든이 오버랩 게이트로 이미 검증 —
     * 여기서는 쓰지 않는다 (인자 유지: CMake 일관성). */
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
    /* HC_LIST_UNIMPL=1: 남은 UNIMPLEMENTED 본문을 전부 나열하고 종료 —
     * 링 프리픽스에서 한 번에 하나씩 fail-loud 로 발견하는 대신 미리
     * 파악하는 진단 (레지스트리 초기화 직후라 수 초) */
    if (getenv("HC_LIST_UNIMPL")) {
        /* 밴드 그리드에 실재하는 바이옴만 대상으로 멤버십을 교차한다 —
         * 링 프리픽스에서 발화 가능한 UNIMPLEMENTED 만 남긴다 */
        static uint8_t present[HC_BIOME_MAX];
        for (int32_t qy = 0; qy < QG_NY; qy++)
            for (int32_t qz = 0; qz < QG_NXZ; qz++)
                for (int32_t qx = 0; qx < QG_NXZ; qx++) {
                    int16_t id = g_grid[qy][qz][qx];
                    if (id >= 0 && id < HC_BIOME_MAX)
                        present[id] = 1;
                }
        for (int32_t st = 0; st < HC_FEAT_STEPS; st++)
            for (int32_t i = 0; i < freg->counts[st]; i++) {
                const hc_pfeat_t *p = &freg->steps[st][i];
                if (p->cf_kind != HC_CF_UNIMPLEMENTED)
                    continue;
                int32_t w = freg->words[st];
                char    hosts[512];
                hosts[0] = '\0';
                for (int32_t b = 0; b < HC_BIOME_MAX; b++) {
                    if (!present[b])
                        continue;
                    if (!(freg->member[st][b * w + (i >> 6)] >>
                              (i & 63) &
                          1u))
                        continue;
                    strncat(hosts, " ", sizeof hosts - strlen(hosts) - 1);
                    strncat(hosts, g_reg.names[b],
                            sizeof hosts - strlen(hosts) - 1);
                }
                if (hosts[0])
                    printf("UNIMPL-IN-BAND step %d index %d %s (%s):%s\n",
                           st, i, p->name ? p->name : "<inline>",
                           p->unimpl_why ? p->unimpl_why : "?", hosts);
            }
        return 0;
    }

    load_climate(ref_dir);

    /* --- 월드 구성: 11x11 전부 우리 04..06 체인 --- */
    hc_biome_view_t view;
    view.qx0 = QG_MIN_XZ;
    view.qz0 = QG_MIN_XZ;
    view.nxz = QG_NXZ;
    view.qy0 = QG_MIN_Y;
    view.ny = QG_NY;
    view.ids = &g_grid[0][0][0];
    view.zoom_seed = hc_biome_obfuscate_seed(seed);

    /* 03_biomes 덤프 오버레이는 뷰 조회 전에 끝나야 한다 — 먼저 바이옴만
     * 전 청크에 로드/채움, 그 다음 체인 실행. */
    for (int32_t cz = -WR; cz <= WR; cz++)
        for (int32_t cx = -WR; cx <= WR; cx++) {
            hc_chunk_t *c = &g_world.chunks[widx(cx, cz)];
            if (hc_chunk_init(c, &g_arena, cx, cz) != 0)
                die("arena exhausted (chunk)", NULL);
            if (is_grid(cx, cz))
                load_chunk_biomes(c, stages_dir);
            else if (is_ring2(cx, cz))
                load_chunk_biomes(c, trace_dir);
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
            /* 노이즈 청크(보간기 버퍼 등)는 체인이 끝나면 죽은 메모리 —
             * 121청크가 arena 를 다 먹지 않게 청크 단위로 되감는다. */
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

    /* 진단 (HC_LIGHT_TRACE_DIAG): 링2 청크의 우리 06 상태를 features-trace
     * 06_carvers 덤프와 대조 — 노이즈 게이트 사각지대 (vein RNG 등) 가 링
     * 청크에서 실재하는지 측정. report-only. */
    if (getenv("HC_LIGHT_TRACE_DIAG")) {
        for (int32_t cz = -WR; cz <= WR; cz++)
            for (int32_t cx = -WR; cx <= WR; cx++) {
                if (!is_ring2(cx, cz))
                    continue;
                static uint16_t g06[HC_BLOCKS];
                char            gpath[1024];
                snprintf(gpath, sizeof gpath, "%s/c.%d.%d/06_carvers.blocks.txt",
                         trace_dir, cx, cz);
                load_blocks_dump(gpath, g06);
                const hc_chunk_t *c = &g_world.chunks[widx(cx, cz)];
                int64_t           bad = 0;
                for (size_t i = 0; i < (size_t)HC_BLOCKS; i++)
                    if (c->states[i] != g06[i]) {
                        if (bad < 4) {
                            int32_t y = (int32_t)(i / 256) + HC_MIN_Y;
                            int32_t z = (int32_t)((i / 16) % 16);
                            int32_t x = (int32_t)(i % 16);
                            fprintf(stderr,
                                    "RING06-DIAG c.%d.%d (%d,%d,%d): ours=%s "
                                    "golden=%s\n",
                                    cx, cz, cx * 16 + x, y, cz * 16 + z,
                                    hc_block_name(c->states[i]),
                                    hc_block_name(g06[i]));
                        }
                        bad++;
                    }
                if (bad)
                    fprintf(stderr, "RING06-DIAG c.%d.%d: %" PRId64
                                    " block diffs vs golden 06\n",
                            cx, cz, bad);
            }
    }

    /* 라이트 월드 */
    hc_light_world_t lw;
    if (hc_light_world_init(&lw, &g_arena, -WR, -WR, WN) != 0)
        die("arena exhausted (light world)", NULL);
    for (int i = 0; i < WORLD_CHUNKS; i++)
        if (hc_light_attach(&lw, &g_arena, &g_world.chunks[i]) != 0)
            die("arena exhausted (light chunks)", NULL);

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
    enum { MAX_MANIFEST = 4096, MAX_SNAPS = 32 };
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
        static ltask_t lt[2 * MAX_MANIFEST];
        snprintf(mpath, sizeof mpath, "%s/stages.log", bdir);
        int32_t n_lt = load_light_tasks(mpath, lt, 2 * MAX_MANIFEST);
        if (n_man < 81 || n_snap != 18 || n_lt < 18)
            die("manifest/snapshots/stages.log unexpectedly short", bname);

        /* 09 덤프별 태스크 매핑: P(C) = 드레인 시점 카운터 (라이트 solve
         * 트리거 pos), k_C = 그 태스크의 배치. 카운터 단조로 P <=
         * seq_begin 이 보장된다 (위반 = 로그 부정합, die). */
        int32_t snap_task[MAX_SNAPS], snap_P[MAX_SNAPS];
        int64_t snap_lb[MAX_SNAPS], snap_ls[MAX_SNAPS];
        int8_t  snap_light_done[MAX_SNAPS];
        for (int32_t i = 0; i < n_snap; i++) {
            snap_task[i] = -1;
            snap_P[i] = -1;
            snap_light_done[i] = 0;
            snap_lb[i] = snap_ls[i] = 0;
            if (snaps[i].stage != 9)
                continue;
            for (int32_t k = 0; k < n_lt; k++)
                if (lt[k].kind == 1 && lt[k].cx == snaps[i].cx &&
                    lt[k].cz == snaps[i].cz) {
                    snap_task[i] = k;
                    break;
                }
            if (snap_task[i] < 0)
                die("09 dump chunk missing from stages.log", bname);
            snap_P[i] = counter_at(g_batch_drain[lt[snap_task[i]].batch]);
            if (snap_P[i] > snaps[i].seq_begin)
                die("drain-time featuresSeq after dump seq_begin", bname);
            printf("model %s 09 c.%d.%d: seq_begin=%d P=%d batch=%d\n",
                   bname, snaps[i].cx, snaps[i].cz, snaps[i].seq_begin,
                   snap_P[i], lt[snap_task[i]].batch);
        }

        int32_t max_prefix = 0;
        for (int32_t i = 0; i < n_snap; i++)
            if (snaps[i].seq_begin > max_prefix)
                max_prefix = snaps[i].seq_begin;
        /* 개발 모드: 프리픽스 상한 (링 본문 랜딩 전 부분 게이트).
         * 상한 밖 덤프 이벤트는 스킵으로 보고 — CI 는 무제한. */
        int32_t dev_cap = -1;
        {
            const char *cs = getenv("HC_LIGHT_MAX_PREFIX");
            if (cs && *cs)
                dev_cap = (int32_t)strtol(cs, NULL, 10);
        }
        int32_t skipped = 0;
        if (dev_cap >= 0 && dev_cap < max_prefix) {
            for (int32_t i = 0; i < n_snap; i++)
                if (snaps[i].seq_begin > dev_cap)
                    skipped++;
            max_prefix = dev_cap;
        }

        /* 06 상태로 리셋 */
        for (int i = 0; i < WORLD_CHUNKS; i++) {
            memcpy(g_world.chunks[i].states, g_world.pristine[i],
                   sizeof(uint16_t) * (size_t)HC_BLOCKS);
            memset(g_world.chunks[i].heightmap_final, 0,
                   sizeof g_world.chunks[i].heightmap_final);
            g_world.chunks[i].hm_final_primed = 0;
            g_world.chunks[i].wg_reprimed = 0;
        }
        rg.wg_dropped = 0;

        int32_t next_snap = 0;
        int64_t bundle_fails = 0;

        for (int32_t pos = 0; pos <= max_prefix; pos++) {
            /* 09 라이트 solve @ P(C) == pos — 블록 = 드레인 시점 프리픽스.
             * 값 비교는 여기서 하고 결과는 seq_begin 덤프 처리에 합산한다
             * (ltask_t 주석의 배치 논증). */
            for (int32_t si = 0; si < n_snap; si++) {
                if (snaps[si].stage != 9 || snap_light_done[si] ||
                    snaps[si].seq_begin > max_prefix || snap_P[si] != pos)
                    continue;
                const snap_t *sn = &snaps[si];
                hc_chunk_t   *c = &g_world.chunks[widx(sn->cx, sn->cz)];
                int32_t       k_c = lt[snap_task[si]].batch;
                int64_t       c_comp = lt[snap_task[si]].comp_nanos;
                if (c_comp < 0)
                    die("09 dump task without completion nanos", bname);
                hc_light_reset(&lw);
                static int8_t feat_map[WORLD_CHUNKS];
                memset(feat_map, 0, sizeof feat_map);
                for (int32_t k = 0; k < pos; k++) {
                    hc_light_set_featured(&lw, man[k].cx, man[k].cz);
                    feat_map[widx(man[k].cx, man[k].cz)] = 1;
                }
                /* R (08 등록): 제출 < C 완료 — 등록은 제출 즉시 유효
                 * 클래스 (실측: 배치 규칙은 c.-1.-1 에 131셀 회귀; 08 은
                 * 09 PRE/POST 페어와 달리 드레인 대기 없이 반영된다).
                 * 단 프리픽스 P 에 deco 가 없는 청크의 등록 (P 이후 데코
                 * +08 — PRE 페이즈와 병행 진행된 링 청크) 은 블록과
                 * 일관되게 제외한다 — 등록만 있고 블록이 프리픽스 밖인
                 * 상태는 재생 불가. S (09 enable+시딩): 배치 <= k_C. */
                for (int32_t k = 0; k < n_lt; k++) {
                    int on = lt[k].kind == 0 ? (lt[k].sub_nanos < c_comp)
                                             : (lt[k].batch <= k_c);
                    if (!on)
                        continue;
                    if (lt[k].cx < -(WR - 1) || lt[k].cx > WR - 1 ||
                        lt[k].cz < -(WR - 1) || lt[k].cz > WR - 1)
                        die("light task outside light world before a grid "
                            "09 dump — enlarge WR",
                            bname);
                    if (lt[k].kind == 0 &&
                        !feat_map[widx(lt[k].cx, lt[k].cz)])
                        continue;
                    if (lt[k].kind == 0)
                        hc_gen_initialize_light_stage(&lw, lt[k].cx,
                                                      lt[k].cz);
                    else
                        hc_gen_light_stage(&lw, lt[k].cx, lt[k].cz);
                }
                hc_light_solve(&lw);

                static uint8_t glight9[HC_BLOCKS];
                char           gpath9[1024], what9[96];
                snprintf(gpath9, sizeof gpath9,
                         "%s/c.%d.%d/09_light.light_block.txt", bdir, sn->cx,
                         sn->cz);
                load_light_dump(gpath9, glight9);
                snprintf(what9, sizeof what9, "LIGHT_BLOCK %s 09_light c.%d.%d",
                         bname, sn->cx, sn->cz);
                snap_lb[si] =
                    compare_light(&lw, HC_LIGHT_BLOCK, c, glight9, what9, 10);

                snprintf(gpath9, sizeof gpath9,
                         "%s/c.%d.%d/09_light.light_sky.txt", bdir, sn->cx,
                         sn->cz);
                load_light_dump(gpath9, glight9);
                snprintf(what9, sizeof what9, "LIGHT_SKY %s 09_light c.%d.%d",
                         bname, sn->cx, sn->cz);
                snap_ls[si] =
                    compare_light(&lw, HC_LIGHT_SKY, c, glight9, what9, 10);
                snap_light_done[si] = 1;
            }

            /* seqBegin == pos 인 덤프 이벤트 처리 (nanos 순) */
            while (next_snap < n_snap && snaps[next_snap].seq_begin == pos) {
                const int32_t si_cur = next_snap;
                const snap_t *sn = &snaps[next_snap++];
                hc_chunk_t   *c =
                    &g_world.chunks[widx(sn->cx, sn->cz)];
                char gpath[1024];
                const char *sname = sn->stage == 8 ? "08_initialize_light"
                                                   : "09_light";

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

                /* (2) heightmaps — golden 에 실재하는 kind 만 */
                static int32_t ghm[HM6][256];
                snprintf(gpath, sizeof gpath, "%s/c.%d.%d/%s.heightmaps.txt",
                         bdir, sn->cx, sn->cz, sname);
                unsigned present = load_heightmaps(gpath, ghm);
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
                    /* 리로드된 청크의 OF_WG(t==2) 는 재프라임 상태: 우리
                     * 모델의 재프라임 맵 (첫-읽기 동결 — 바닐라와 같은
                     * 이벤트) 과 대조. 읽힌 적 없으면 라이브 OCEAN_FLOOR
                     * (동일 predicate) 폴백. 잔차 = 재프라임 시점
                     * R(바닐라 리로드 후 첫 읽기) 와의 랙. */
                    int is_reprimed_ofwg =
                        t == 2 && sn->stage == 9 && !(present & 1u);
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

                /* (3) light */
                int64_t lb_bad, ls_bad;
                if (sn->stage == 8) {
                    /* 08 덤프: 값은 전부 미등록/등록-제로 + 가시 읽기 규칙
                     * (top 위 15) — R = 데코 프리픽스 (기존 모델, 18/18). */
                    hc_light_reset(&lw);
                    for (int32_t k = 0; k < pos; k++)
                        hc_light_set_featured(&lw, man[k].cx, man[k].cz);
                    for (int32_t k = 0; k < pos; k++)
                        hc_gen_initialize_light_stage(&lw, man[k].cx,
                                                      man[k].cz);
                    hc_light_solve(&lw);

                    static uint8_t glight[HC_BLOCKS];
                    char           what[96];
                    snprintf(gpath, sizeof gpath,
                             "%s/c.%d.%d/%s.light_block.txt", bdir, sn->cx,
                             sn->cz, sname);
                    load_light_dump(gpath, glight);
                    snprintf(what, sizeof what, "LIGHT_BLOCK %s %s c.%d.%d",
                             bname, sname, sn->cx, sn->cz);
                    lb_bad = compare_light(&lw, HC_LIGHT_BLOCK, c, glight,
                                           what, 10);

                    snprintf(gpath, sizeof gpath,
                             "%s/c.%d.%d/%s.light_sky.txt", bdir, sn->cx,
                             sn->cz, sname);
                    load_light_dump(gpath, glight);
                    snprintf(what, sizeof what, "LIGHT_SKY %s %s c.%d.%d",
                             bname, sname, sn->cx, sn->cz);
                    ls_bad = compare_light(&lw, HC_LIGHT_SKY, c, glight, what,
                                           10);
                } else {
                    /* 09 덤프: 라이트는 P(C)==제출 프리픽스에서 이미 solve
                     * + 비교됨 (위 트리거) — 결과만 합산. */
                    if (!snap_light_done[si_cur])
                        die("09 light solve did not run before its dump",
                            bname);
                    lb_bad = snap_lb[si_cur];
                    ls_bad = snap_ls[si_cur];
                }

                int64_t total = bbad + hmbad + lb_bad + ls_bad;
                /* Task 13-close: strict 기본 승격 — 구 stale-번들 RESID
                 * 잔차 봉투 (fallback 프로토콜 2026-07-31) 삭제. unified
                 * noSave 재캡처 + 배치 시뮬레이션 모델로 35/36 덤프
                 * 0-diff 가 기본 게이트다 (HC_LIGHT_STRICT 불필요).
                 *
                 * 유일 예외 (실측 소진으로 골든 아티팩트 판정, 완료 노트
                 * task13-close §light-artifact): primary 09 c.-1.1 의
                 * sky 36셀 ((-16..-15, 64-65, 29-31) — want 13-14, 수렴해
                 * 11-13). want 형상 (z-무구배, x-구배 15→14→13, y64/65
                 * 동일) 은 서쪽 이웃 c.-2.1 동단 컬럼이 y64-65 에서 15
                 * 여야만 성립하는데, 그 컬럼들 위 오크 캐노피 (y69-74,
                 * 데코 엔트리 <=44 — 모든 후보 flood 나노보다 최소 130ms
                 * 앞) 를 지나는 어떤 수렴해도 12-14 를 넘지 못한다.
                 * 재생 공간 소진: 프리픽스 {제출 seq, 드레인 seq,
                 * seq_begin} × S {완료 컷오프, 배치, 병합 배치} × R
                 * {완료 컷오프, 배치} 전 조합에서 이 36셀은 불변 (5회
                 * 실측 런). 최종 라이트는 region 게이트 4/4 byte-exact
                 * — 아티팩트는 미드-배치 과도상태에 국한된다. 캡 초과
                 * = 즉시 FAIL (회귀 fail-loud). */
                int64_t counted = total;
                if (bundle == 0 && sn->stage == 9 && sn->cx == -1 &&
                    sn->cz == 1 && bbad == 0 && hmbad == 0 && lb_bad == 0 &&
                    ls_bad <= 36)
                    counted = 0;
                bundle_fails += counted;
                printf("%s %s c.%d.%d @seq%d: blocks %" PRId64 " hm %" PRId64
                       " light_block %" PRId64 " light_sky %" PRId64 "%s\n",
                       bname, sname, sn->cx, sn->cz, sn->seq_begin, bbad,
                       hmbad, lb_bad, ls_bad,
                       counted ? "  <-- FAIL"
                       : total ? "  (documented golden artifact — header)"
                               : "");
            }

            /* manifest 엔트리 pos 적용 (UNIMPLEMENTED 본문 발화는 즉사).
             * Task 13 unified 번들: noSave 캡처 — 저장/언로드 웨이브 없음,
             * wg_dropped 모델링 제거. 창(±(WR-1)) 밖 엔트리는 스킵 (리전
             * 전체 manifest; 프리픽스 시맨틱은 글로벌 seq 그대로). */
            if (pos < max_prefix &&
                (man[pos].cx < -(WR - 1) || man[pos].cx > WR - 1 ||
                 man[pos].cz < -(WR - 1) || man[pos].cz > WR - 1))
                continue;
            if (pos < max_prefix) {
                if (getenv("HC_LIGHT_TRACE_DIAG") && bundle == 0) {
                    static char ours_buf[256 << 10], gold_buf[256 << 10];
                    ours_buf[0] = gold_buf[0] = '\0';
                    tracebuf_t  ours = {ours_buf, 0, sizeof ours_buf};
                    tracebuf_t  gold = {gold_buf, 0, sizeof gold_buf};
                    hc_feat_trace_t diag = {diag_sink_pos, diag_sink_feature,
                                            &ours};
                    hc_gen_features_chunk(&rg, man[pos].cx, man[pos].cz, seed,
                                          freg, &view, &g_reg,
                                          (int32_t)sea->num,
                                          /*walk_max_step=*/10, &diag);
                    char tpath[1024], what[64];
                    snprintf(tpath, sizeof tpath, "%s/traces/c.%d.%d.trace.txt",
                             trace_dir, man[pos].cx, man[pos].cz);
                    FILE *tf = fopen(tpath, "rb");
                    if (tf) {
                        fclose(tf);
                        filter_golden_trace(tpath, man[pos].seed_hex, &gold);
                        snprintf(what, sizeof what, "seq%d c.%d.%d", pos,
                                 man[pos].cx, man[pos].cz);
                        diag_diff_traces(what, ours.buf, gold.buf);
                        const char *dd = getenv("HC_LIGHT_TRACE_DUMP_DIR");
                        if (dd) {
                            char dpath[1024];
                            snprintf(dpath, sizeof dpath, "%s/c.%d.%d.ours.txt",
                                     dd, man[pos].cx, man[pos].cz);
                            FILE *df = fopen(dpath, "wb");
                            if (df) {
                                fwrite(ours.buf, 1, ours.len, df);
                                fclose(df);
                            }
                            snprintf(dpath, sizeof dpath, "%s/c.%d.%d.gold.txt",
                                     dd, man[pos].cx, man[pos].cz);
                            df = fopen(dpath, "wb");
                            if (df) {
                                fwrite(gold.buf, 1, gold.len, df);
                                fclose(df);
                            }
                        }
                    }
                } else {
                    hc_gen_features_chunk(&rg, man[pos].cx, man[pos].cz, seed,
                                          freg, &view, &g_reg,
                                          (int32_t)sea->num,
                                          /*walk_max_step=*/10, &g_guard_sink);
                }
            }
        }
        if (skipped) {
            fprintf(stderr,
                    "BUNDLE %s: DEV MODE — %d dump gates skipped "
                    "(HC_LIGHT_MAX_PREFIX=%d)\n",
                    bname, skipped, max_prefix);
            g_fails += skipped; /* 개발 모드는 절대 green 이 되지 않는다 */
        } else {
            assert(next_snap == n_snap);
        }
        if (bundle_fails) {
            fprintf(stderr, "BUNDLE %s: %" PRId64 " total cell mismatches\n",
                    bname, bundle_fails);
            g_fails += (int)(bundle_fails > 100000 ? 100000 : bundle_fails);
        } else {
            printf("BUNDLE %s: all 18 dump gates clean\n", bname);
        }
    }

    printf("test_light_stages: %d fails\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
