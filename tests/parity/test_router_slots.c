/* 26.2 오버월드 noise_router 15슬롯 비트정확 패리티 (Plan Task 6b 게이트).
 *
 * 커밋된 reference/ worldgen JSON 클로저를 파싱해 df_compile 로 평탄 IR 을
 * 만들고, golden/rng/router_seed*.txt 의 모든 (x,y,z) 벡터를 평가해
 * IEEE-754 비트로 대조한다. golden 은 실제 26.2 서버가 SinglePointContext
 * 로 라우터 슬롯을 덤프한 값이다 — 재생성하지 않는다.
 *
 * false-PASS 방어 (test_noise.c 방침 동일):
 *  - 스코프 안의 미인식 라인 즉사
 *  - 슬롯별 기대 벡터 수를 동결 golden 의 상수로 고정 (꼬리 절단 방어)
 *  - 16개 섹션(라우터 15 + old_blended_noise) 전부 존재해야 통과
 *  - old_blended_noise 는 test_noise blended 가 담당 — 여기서는 수만 센다 */

#undef NDEBUG

#include "../../core/src/hc_df_compile.h"

#include <assert.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_fails = 0;

static void die(const char *msg, const char *detail) {
    fprintf(stderr, "GOLDEN/SETUP ERROR: %s%s%s\n", msg, detail ? ": " : "",
            detail ? detail : "");
    exit(2);
}

static int blank(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    return *s == '\0';
}

/* 컴파일러 arena. JSON DOM 전체 + IR + 노이즈 인스턴스가 들어간다. */
static unsigned char g_backing[24u << 20];
static hc_arena_t g_arena;

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

/* --- reference/ 로더 ---
 * density_function/<rel>.json → "minecraft:<rel>" (최대 2단 중첩),
 * noise/<name>.json → "minecraft:<name>". 이름은 arena 에 복사한다. */

#define MAX_SOURCES 64

static hc_df_source_t g_dfs[MAX_SOURCES];
static int32_t        g_n_dfs = 0;
static hc_df_source_t g_noises[MAX_SOURCES];
static int32_t        g_n_noises = 0;

static int has_suffix(const char *s, const char *suf) {
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

static void add_source(hc_df_source_t *tab, int32_t *n, const char *dir,
                       const char *rel_prefix, const char *fname) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, fname);

    char *name = hc_arena_alloc(
        &g_arena, 10 + strlen(rel_prefix) + strlen(fname) + 1, 1);
    if (!name)
        die("arena exhausted (source name)", fname);
    /* "minecraft:" + prefix + fname(마지막 .json 제거) */
    size_t stem = strlen(fname) - 5;
    sprintf(name, "minecraft:%s%.*s", rel_prefix, (int)stem, fname);

    if (*n >= MAX_SOURCES)
        die("too many reference sources", name);
    tab[*n].name = name;
    tab[*n].json = parse_file(path);
    (*n)++;
}

static void load_tree(hc_df_source_t *tab, int32_t *n, const char *dir,
                      const char *rel_prefix, int depth) {
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
        if (probe) { /* 하위 디렉터리 */
            closedir(probe);
            if (depth >= 2)
                die("reference tree deeper than expected", sub);
            char pref[512];
            snprintf(pref, sizeof pref, "%s%s/", rel_prefix, e->d_name);
            load_tree(tab, n, sub, pref, depth + 1);
        } else if (has_suffix(e->d_name, ".json")) {
            add_source(tab, n, dir, rel_prefix, e->d_name);
        } else {
            die("unexpected file in reference tree", sub);
        }
    }
    closedir(d);
}

/* --- 슬롯 테이블: 동결 golden 의 섹션/벡터 수와 일치해야 한다 --- */

typedef struct {
    const char *name;
    int         want_vecs;
    int         is_router; /* 0 = old_blended_noise (test_noise 담당) */
    int32_t     root;
    int         seen_vecs;
    int         seen_slot;
} slot_t;

static slot_t g_slots[] = {
    {"final_density", 680, 1, -1, 0, 0},
    {"preliminary_surface_level", 40, 1, -1, 0, 0},
    {"barrier", 200, 1, -1, 0, 0},
    {"fluid_level_floodedness", 200, 1, -1, 0, 0},
    {"fluid_level_spread", 200, 1, -1, 0, 0},
    {"lava", 200, 1, -1, 0, 0},
    {"depth", 200, 1, -1, 0, 0},
    {"continents", 40, 1, -1, 0, 0},
    {"erosion", 40, 1, -1, 0, 0},
    {"ridges", 40, 1, -1, 0, 0},
    {"temperature", 40, 1, -1, 0, 0},
    {"vegetation", 40, 1, -1, 0, 0},
    {"vein_toggle", 200, 1, -1, 0, 0},
    {"vein_ridged", 200, 1, -1, 0, 0},
    {"vein_gap", 200, 1, -1, 0, 0},
    {"old_blended_noise", 680, 0, -1, 0, 0},
};
#define N_SLOTS ((int)(sizeof g_slots / sizeof g_slots[0]))

static slot_t *find_slot(const char *name) {
    for (int i = 0; i < N_SLOTS; i++)
        if (strcmp(g_slots[i].name, name) == 0)
            return &g_slots[i];
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: test_router_slots <router_golden> <ref_dir>\n");
        return 2;
    }
    const char *golden_path = argv[1];
    const char *ref_dir = argv[2];

    hc_arena_init(&g_arena, g_backing, sizeof g_backing);

    /* reference 클로저 적재 */
    char sub[1024];
    snprintf(sub, sizeof sub, "%s/density_function", ref_dir);
    load_tree(g_dfs, &g_n_dfs, sub, "", 0);
    snprintf(sub, sizeof sub, "%s/noise", ref_dir);
    load_tree(g_noises, &g_n_noises, sub, "", 0);
    if (g_n_dfs < 19 || g_n_noises < 35)
        die("reference closure incomplete", ref_dir);

    snprintf(sub, sizeof sub, "%s/overworld-26.2.json", ref_dir);
    const hc_json_t *settings = parse_file(sub);
    const hc_json_t *router = hc_json_get(settings, "noise_router");
    if (!router || router->kind != HC_JSON_OBJ || router->count != 15)
        die("noise_router missing or not 15 slots", sub);

    /* golden 스트리밍: seed 를 읽은 뒤 컴파일하고 벡터를 대조한다 */
    FILE *f = fopen(golden_path, "r");
    if (!f)
        die("cannot open golden file", golden_path);

    hc_df_compiler_t comp;
    hc_df_graph_t    graph;
    double          *scratch = NULL;
    int              compiled = 0;
    slot_t          *cur = NULL;
    char             line[16384];
    int64_t          seed = 0;
    int              has_seed = 0;

    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || blank(line))
            continue;

        int64_t  sv;
        int      x, y, z, n;
        char     name[128];
        uint64_t bits;

        if (sscanf(line, "seed %" SCNd64, &sv) == 1) {
            seed = sv;
            has_seed = 1;

            /* 시드 확보 → 라우터 15슬롯을 하나의 공유 그래프로 컴파일 */
            if (hc_df_compiler_init(&comp, &graph, &g_arena, seed, g_dfs,
                                    g_n_dfs, g_noises, g_n_noises) != 0)
                die("compiler init failed (arena)", NULL);
            for (const hc_json_t *m = router->child; m; m = m->next) {
                char key[128];
                if (m->klen >= (int32_t)sizeof key)
                    die("slot name too long", NULL);
                memcpy(key, m->key, (size_t)m->klen);
                key[m->klen] = '\0';
                slot_t *s = find_slot(key);
                if (!s || !s->is_router)
                    die("unexpected router slot in JSON", key);
                if (s->root >= 0)
                    die("duplicate router slot in JSON", key);
                s->root = hc_df_compile_expr(&comp, m);
                if (s->root < 0) {
                    fprintf(stderr, "compile failed for slot %s: %s\n", key,
                            comp.err ? comp.err : "?");
                    return 2;
                }
            }
            for (int i = 0; i < N_SLOTS; i++)
                if (g_slots[i].is_router && g_slots[i].root < 0)
                    die("router slot missing from JSON", g_slots[i].name);
            scratch = hc_arena_alloc(&g_arena,
                                     sizeof(double) * 2 * (size_t)graph.n,
                                     _Alignof(double));
            if (!scratch)
                die("arena exhausted (scratch)", NULL);
            compiled = 1;
        } else if (sscanf(line, "slot %127s", name) == 1) {
            if (!compiled)
                die("slot before seed", name);
            cur = find_slot(name);
            if (!cur)
                die("unknown slot in golden", name);
            if (cur->seen_slot)
                die("duplicate slot section in golden", name);
            cur->seen_slot = 1;
        } else if (sscanf(line, "v %d %d %d %*s bits=0x%" SCNx64, &x, &y, &z,
                          &bits) == 4) {
            if (!cur)
                die("vector before slot header", NULL);
            cur->seen_vecs++;
            if (!cur->is_router)
                continue; /* old_blended_noise: test_noise blended 담당 */
            graph.root = cur->root;
            double got = hc_df_eval(&graph, (double)x, (double)y, (double)z,
                                    scratch);
            uint64_t got_bits;
            memcpy(&got_bits, &got, sizeof got_bits);
            g_checks++;
            if (got_bits != bits) {
                g_fails++;
                if (g_fails <= 40) /* 홍수 방지 — 카운트는 전부 센다 */
                    fprintf(stderr,
                            "FAIL %s(%d,%d,%d): got %.17g (0x%016" PRIx64
                            ") want bits 0x%016" PRIx64 "\n",
                            cur->name, x, y, z, got, got_bits, bits);
            }
        } else if (sscanf(line, "blended_noise_instances %d", &n) == 1) {
            if (n != 1)
                die("blended_noise_instances != 1", NULL);
        } else {
            fprintf(stderr, "GOLDEN FORMAT ERROR %s: unrecognized line: %s",
                    golden_path, line);
            return 2;
        }
    }
    fclose(f);

    if (!has_seed)
        die("missing seed", golden_path);
    for (int i = 0; i < N_SLOTS; i++) {
        if (!g_slots[i].seen_slot)
            die("slot section missing from golden", g_slots[i].name);
        if (g_slots[i].seen_vecs != g_slots[i].want_vecs) {
            fprintf(stderr, "GOLDEN FORMAT ERROR: slot %s has %d vectors, "
                            "want %d\n",
                    g_slots[i].name, g_slots[i].seen_vecs,
                    g_slots[i].want_vecs);
            return 2;
        }
    }

    printf("test_router_slots: %d nodes, %d noises, %d splines, %d blended; "
           "%d checks, %d failures\n",
           graph.n, graph.n_noises, graph.n_splines, graph.n_blended,
           g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
