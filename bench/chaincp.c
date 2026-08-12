/* B-5 체인 임계-경로 하한 도구 (판정 입력 전용 — 게이트 비연결 독립 도구).
 *
 * B-4 잔여 #2 / P2-11 §4 분모 2 의 형식화: x8 커널이 실행하는 실제
 * 스트림 (콘 프로그램) 을 노드 단위로 걷고, 각 노드의 내부 데이터플로
 * 임계 체인 (FMA 금지·순서 보존·정확 나눗셈 패리티 제약 하) 을 Zen5
 * 공개 지연 테이블로 계량해 커널의 구조-유도 사이클 하한을 산출한다.
 * 실측이 아니라 모델이다 — 산출물은 노트 (B-5-final-verdict.md) 가
 * 전제·출처와 함께 소비한다.
 *
 * 셋업은 microbench.c 와 동일 (reference JSON → compile → hc_nc_*),
 * 인보케이션 기하는 noise_chunk.c 의 x8 경로를 그대로 그림자 재현한다
 * (슬라이스: 컬럼당 y 8점 그룹 6개 × 컬럼 5 × 채움 5 = 150 그룹/패스,
 * 셀: select 당 (ix 2 × iz 4) 그룹 16 × 768 select = 12,288 그룹/패스
 * — HC_CTR_X8_SLICE/CELL 카운터와 정확히 일치해야 한다).
 *
 * 값 오라클: 레인별 스칼라 eager 콘 평가 (hc_df_eval_cone — x8 레인과
 * IEEE-754 비트 동일이 df_x8 게이트로 증명된 그 시맨틱). eager 워크는
 * 죽은-레그 노드까지 전부 평가하므로 x8 의 죽은-레인 값 (순수 연산)
 * 과 동일하고, RC/IS/blended over-under 의 8-레인 마스크가 프로덕션과
 * 비트 동일하게 재현된다. 실행은 스칼라 강제 — AVX-512 부재 호스트
 * (claw) 에서 돈다. 스트림/콘은 ISA 무관 (hc_nc_init 이 항상 산출).
 *
 * 집계 2종 (전제 라벨):
 *  - serial: 체인-직렬 전제 (P1) — perlin 본체 (~397 insn ≈ ROB/스레드
 *    224·풀 448 자릿수) 는 한 스트림 안에서 서로 겹치지 않는다 (B-4
 *    실증: 포트 가동률 9-10%, SMT co-run 1.52x, 2-웨이 인터리브가 그
 *    슬랙을 정확히 회수). 본체 내부는 완전 ILP 허용 (관대 방향 = 하한
 *    보수). 인보케이션 간 중첩 불허.
 *  - dfcp: 순수 데이터플로 CP (무한 ILP) — 포트-하한과 같은 "의존성
 *    무시" 병리의 참고선. 도달 불가 하한군의 위치 표시용.
 *
 * 지연 프리셋 (출처: AMD SOG #58455 rev1.00 xlsx / uops.info Zen5 /
 * Agner Fog 2025-09 — 노트에 전거):
 *  - low: 최저 방어 가능치 (하한 보수 방향). fmul 2 (uops 실측),
 *    fadd/fsub 2 (만장일치), fdiv 13, floor(rndscale) 3, cmp→k 4
 *    (ymm 4c 페어 경로가 패리티-유효해 zmm 6 대신 4 허용), blend(k) 1,
 *    vpermt2w/pd 4 (uops), cvtpd2dq 7 (만장일치), 정수 이동/변환 3,
 *    shuffle 4, 정수 ALU 1.
 *  - nom: 문서 공칭치. fmul/fadd 3, cmp 6, blend 2, permt2 5, shuffle 5.
 *  파생 규칙: 2의 거듭제곱 나눗셈은 곱으로 정확-폴딩 (값 보존 — 컴파일러
 *  실동작, P2-11 §9) → 지연은 fmul, 카운트는 양쪽 병기. 테이블/코너
 *  상수/플랫 캐시 로드는 t=0 (사전 적재 가능 — 하한 보수). 스크래치
 *  store→load 왕복 (실기 11c) 은 0 (구현세 — 갭 계상 측에 귀속).
 *
 * 패리티-비용 변형: --fma (mul→add 융합 허용 가정), --reassoc (옥타브
 * 누산 재결합 허용 가정) — 조항 3 폴백 목록 항목의 체인-면 비용 정량화.
 *
 * 출력: stdout `cp <key> <value>` (파서 친화). 검증 앵커:
 *  - 인보케이션/노드/perlin 카운트 == HC_CTR_X8_* (P2-11 §3.4)
 *  - 벡터 FP op/노드 == PMU fp_ops_retired_by_type (B-4 §4.1)
 *  - perlin 정적 46/29/16 (+y분기 47/30/17/1) == B-3 §2 소스 구조 */

#define _POSIX_C_SOURCE 200809L

#include "../core/src/hc_df_compile.h"
#include "../core/src/hc_df_simd.h"
#include "../core/src/hc_gen_noise.h"

#include <dirent.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char *msg, const char *detail) {
    fprintf(stderr, "chaincp: %s%s%s\n", msg, detail ? ": " : "",
            detail ? detail : "");
    exit(2);
}

/* ---------- 셋업 (microbench.c 와 동일 경로) ---------- */

static unsigned char g_backing[160u << 20];
static hc_arena_t    g_arena;

static char *read_file(const char *path) {
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
    return buf;
}

static const hc_json_t *parse_file(const char *path) {
    const char *err = NULL;
    size_t      pos = 0;
    hc_json_t  *v = hc_json_parse(read_file(path), &g_arena, &err, &pos);
    if (!v)
        die("JSON parse error", path);
    return v;
}

#define MAX_SOURCES 64
static hc_df_source_t g_dfs[MAX_SOURCES], g_noises[MAX_SOURCES];
static int32_t        g_n_dfs = 0, g_n_noises = 0;

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
            load_tree(tab, n, sub, pref, depth + 1);
            continue;
        }
        size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".json") != 0)
            die("unexpected file in reference tree", sub);
        char *name =
            hc_arena_alloc(&g_arena, 10 + strlen(rel_prefix) + l + 1, 1);
        if (!name)
            die("arena exhausted (source name)", e->d_name);
        sprintf(name, "minecraft:%s%.*s", rel_prefix, (int)(l - 5),
                e->d_name);
        if (*n >= MAX_SOURCES)
            die("too many reference sources", name);
        tab[*n].name = name;
        tab[*n].json = parse_file(sub);
        (*n)++;
    }
    closedir(d);
}

static hc_df_graph_t    g_graph;
static hc_noise_roots_t g_roots;

static void df_setup(const char *repo, int64_t seed) {
    char sub[1024];
    snprintf(sub, sizeof sub, "%s/reference/density_function", repo);
    load_tree(g_dfs, &g_n_dfs, sub, "", 0);
    snprintf(sub, sizeof sub, "%s/reference/noise", repo);
    load_tree(g_noises, &g_n_noises, sub, "", 0);
    if (g_n_dfs < 19 || g_n_noises < 35)
        die("reference closure incomplete", repo);
    snprintf(sub, sizeof sub, "%s/reference/overworld-26.2.json", repo);
    const hc_json_t *settings = parse_file(sub);
    const hc_json_t *router = hc_json_get(settings, "noise_router");
    if (!router || router->kind != HC_JSON_OBJ || router->count != 15)
        die("noise_router missing or not 15 slots", sub);

    hc_df_compiler_t comp;
    if (hc_df_compiler_init(&comp, &g_graph, &g_arena, seed, g_dfs, g_n_dfs,
                            g_noises, g_n_noises) != 0)
        die("compiler init failed", NULL);
    memset(&g_roots, -1, sizeof g_roots);
    for (const hc_json_t *m = router->child; m; m = m->next) {
        int32_t r = hc_df_compile_expr(&comp, m);
        if (r < 0)
            die("compile failed", comp.err ? comp.err : "?");
#define S(k)                                                                   \
    if ((int32_t)strlen(#k) == m->klen &&                                      \
        memcmp(m->key, #k, (size_t)m->klen) == 0)                              \
    g_roots.k = r
        S(final_density);
        S(barrier);
        S(fluid_level_floodedness);
        S(fluid_level_spread);
        S(lava);
        S(erosion);
        S(depth);
        S(preliminary_surface_level);
        S(vein_toggle);
        S(vein_ridged);
        S(vein_gap);
#undef S
    }
    g_graph.root = g_roots.final_density;
    if (g_roots.final_density < 0)
        die("router slot missing", NULL);
}

/* ---------- 지연 테이블 ---------- */

typedef struct {
    double fadd, fmul, fdiv, ffma; /* FP */
    double frnd;                   /* vrndscalepd (floor) */
    double cmp;                    /* vcmppd → k */
    double blendk;                 /* vblendm{pd,w} (k 소비 포함) */
    double permt2;                 /* vpermt2{w,pd} */
    double cvtpd2dq;               /* vcvtpd2dq y←z (2 uop) */
    double cvtint;                 /* vpmov* 계열 폭 변환 */
    double shuf;                   /* vshufi32x4 / vextracti32x4 */
    double insert;                 /* vinserti128 */
    double ipadd;                  /* 정수 ALU (vpaddw/vpsrlw/vpand) */
    int    fma;     /* 1 = mul→add 융합 허용 가정 (패리티 위반 변형) */
    int    reassoc; /* 1 = 누산 재결합 허용 가정 (패리티 위반 변형) */
} lat_t;

static lat_t lat_low(void) {
    lat_t L = {.fadd = 2, .fmul = 2, .fdiv = 13, .ffma = 3, .frnd = 3,
               .cmp = 4, .blendk = 1, .permt2 = 4, .cvtpd2dq = 7,
               .cvtint = 3, .shuf = 4, .insert = 1, .ipadd = 1};
    return L;
}

static lat_t lat_nom(void) {
    lat_t L = {.fadd = 3, .fmul = 3, .fdiv = 13, .ffma = 4, .frnd = 3,
               .cmp = 6, .blendk = 2, .permt2 = 5, .cvtpd2dq = 7,
               .cvtint = 4, .shuf = 5, .insert = 3, .ipadd = 1};
    return L;
}

/* FP op 카운트 (벡터 insn 단위 — PMU fp_ops_retired_by_type 의 op 단위와
 * 동일). div_folded: 2^k 나눗셈이 곱으로 폴딩된 계상 (mul 로 이동). */
typedef struct {
    uint64_t mul, add, sub, div;
    uint64_t mul_folded, div_real; /* 폴딩 병기용 */
} fpops_t;

static fpops_t g_fp; /* 전역 누적 (인보케이션 전체) */

static int is_pow2_d(double v) {
    if (!(v > 0.0) || isinf(v))
        return 0;
    int    e;
    double m = frexp(v, &e);
    return m == 0.5;
}

/* ---------- 커널 내부 체인 서브모델 (df_simd_avx512.c 소스 구조) ---------- */

/* wrap_x8: d - floor(d/2^25 + 0.5)*2^25. /2^25 는 정확-폴딩 → fmul.
 * 체인 = fmul + fadd + frnd + fmul + fsub. */
static double L_wrap(const lat_t *L) {
    return L->fmul + L->fadd + L->frnd + L->fmul + L->fadd; /* fsub==fadd */
}

static void fp_wrap(void) { /* 축 1개 분 */
    g_fp.mul += 2; /* q-mul (폴딩) + fl*2^25 */
    g_fp.mul_folded += 1;
    g_fp.div_real += 1; /* 소스는 div — 미폴딩 계상 병기 */
    g_fp.add += 1;
    g_fp.sub += 1;
}

/* perlin_x8 내부 CP (좌표 입력 t=0 기준) + 정적 FP 카운트.
 * y_branch: yscale != 0 (blended 경로). 소스 구조는 df_simd_avx512.c
 * perlin_x8 — 코너 해시 3단 vpermt2w 룩업, vpermt2pd 코너 픽,
 * smoothstep, 7-lerp 사다리. */
static double L_perlin(const lat_t *L, int y_branch) {
    double t_d = L->fadd;             /* dx = x + xo */
    double t_fd = t_d + L->frnd;      /* fdx = floor(dx) */
    double t_f = t_fd + L->fadd;      /* fx = dx - fdx (sub) */
    /* 해시: fdx → cvtpd2dq → and → 워드화 → insert → 3단 룩업 */
    double t_ix = t_fd + L->cvtpd2dq + L->ipadd + L->cvtint;
    double t_ixp = t_ix + L->insert + L->ipadd; /* +1 페어, &255 */
    double lkup = L->permt2 + 3.0 * L->blendk;  /* perm_lookup32 */
    double t_ab = t_ixp + lkup;
    double t_l2 = t_ab + L->shuf + L->ipadd + lkup;  /* [aa..bb] */
    double t_gh = t_l2 + L->shuf + L->ipadd + lkup + L->ipadd; /* &15 */
    double t_vi = t_gh + L->shuf + L->cvtint;        /* extract+zx */
    double t_pick = t_vi + L->permt2;                /* vpermt2pd */
    /* gy (y_branch): blend(use_ymax) → div(t/ys) → +eps → floor → *ys → sub */
    double t_gy = t_f;
    if (y_branch)
        t_gy = t_f + L->cmp + L->blendk + L->fdiv + L->fadd + L->frnd +
               L->fmul + L->fadd;
    /* 코너 dot: max(픽, 좌표) + mul + add + add (fma 변형: mul+2fma) */
    double t_in = t_f > t_gy ? t_f : t_gy;
    if (t_pick > t_in)
        t_in = t_pick;
    double t_corner =
        L->fma ? t_in + L->fmul + 2.0 * L->ffma
               : t_in + L->fmul + 2.0 * L->fadd;
    /* smoothstep (fx/fy/fz 는 양자화 전 값): inner 4-체인 ∥ t³ 2-체인 */
    double ss_inner = L->fma ? 2.0 * L->ffma
                             : L->fmul + L->fadd + L->fmul + L->fadd;
    double ss_t3 = 2.0 * L->fmul;
    double t_ss = t_f + (ss_inner > ss_t3 ? ss_inner : ss_t3) + L->fmul;
    /* 7-lerp 사다리 3층: lerp = sub+mul+add (fma: sub+fma) */
    double lerp = L->fma ? L->fadd + L->ffma
                         : L->fadd + L->fmul + L->fadd;
    double t_l1 = (t_corner > t_ss ? t_corner : t_ss) + lerp;
    double t_lv2 = (t_l1 > t_ss ? t_l1 : t_ss) + lerp;
    return (t_lv2 > t_ss ? t_lv2 : t_ss) + lerp;
}

static void fp_perlin(int y_branch) {
    /* B-3 §2 소스 구조: 46 mul + 29 add + 16 sub (y분기 +1 mul/add/sub/div) */
    g_fp.mul += 46;
    g_fp.add += 29;
    g_fp.sub += 16;
    if (y_branch) {
        g_fp.mul += 1;
        g_fp.add += 1;
        g_fp.sub += 1;
        g_fp.div += 1; /* t/yscale — 임의 배율, 실나눗셈 */
    }
}

/* 체인-직렬 전제 (P1) 커서: perlin 본체는 한 스트림 안에서 서로 겹치지
 * 않는다. serial==0 이면 순수 데이터플로. */
typedef struct {
    double ser;    /* 직렬 커서 (인보케이션 로컬) */
    int    serial; /* P1 on/off */
} sched_t;

static double body(sched_t *s, double ready, double cp) {
    if (s->serial) {
        double st = ready > s->ser ? ready : s->ser;
        s->ser = st + cp;
        return s->ser;
    }
    return ready + cp;
}

static uint64_t g_perlin_calls;

/* PerlinNoise.getValue (octaves_x8): 옥타브별 wrap 입력 + perlin 본체 +
 * 누산 d += (amp*g)*f. yscale==0 전제 (NOISE 경로). */
static double L_octaves(const lat_t *L, sched_t *s, const hc_octaves_t *o,
                        double t_in, int y_branch, double yscale) {
    double acc = 0.0;
    double cmax = 0.0;
    int    nacc = 0;
    double cp = L_perlin(L, y_branch && yscale != 0.0);
    for (int32_t i = 0; i < o->count; i++) {
        if (!o->octaves[i])
            continue;
        double t_wrap = t_in + L->fmul + L_wrap(L); /* x*e → wrap */
        double t_body = body(s, t_wrap, cp);
        g_perlin_calls++;
        fp_perlin(y_branch && yscale != 0.0);
        g_fp.mul += 3; /* x*e, y*e, z*e */
        g_fp.mul += 1; /* ymax*e (항상 계산됨) */
        fp_wrap();
        fp_wrap();
        fp_wrap();
        /* d += (amp*g)*f — fma 변형: t=amp*g 후 d=fma(t,f,d) */
        double contrib = L->fma ? t_body + L->fmul : t_body + 2.0 * L->fmul;
        double step = L->fma ? L->ffma : L->fadd;
        g_fp.mul += 2;
        g_fp.add += 1;
        if (L->reassoc) {
            if (contrib > cmax)
                cmax = contrib;
            nacc++;
        } else {
            double r = contrib > acc ? contrib : acc;
            acc = r + step;
        }
    }
    if (L->reassoc && nacc > 0) {
        double depth = ceil(log2((double)nacc));
        acc = cmax + depth * L->fadd;
    }
    return acc;
}

/* NormalNoise.getValue: first(x,y,z) + second(x·F,y·F,z·F) → (a+b)*factor */
static double L_normal(const lat_t *L, sched_t *s, const hc_normal_noise_t *n,
                       double t_in) {
    double a = L_octaves(L, s, &n->first, t_in, 0, 0.0);
    double b = L_octaves(L, s, &n->second, t_in + L->fmul, 0, 0.0);
    g_fp.mul += 3; /* x2/y2/z2 = ·INPUT_FACTOR */
    g_fp.add += 1;
    g_fp.mul += 1; /* ·value_factor */
    return (a > b ? a : b) + L->fadd + L->fmul;
}

/* BlendedNoise.compute — 소스는 noise_blended.c / df_simd_avx512.c
 * blended_x8. over/under 마스크는 레인별 q 를 스칼라 재계산으로 판정
 * (hc_blended_compute 의 q 섹션 복제 — 값은 비트 동일). */
static const hc_perlin_t *oct_at(const hc_octaves_t *o, int32_t i) {
    return o->octaves[o->count - 1 - i];
}

static uint64_t g_blend_execs, g_blend_onesided, g_blend_mix;
static uint64_t g_interp_cell;
static uint64_t g_ops[HC_DF_OP_COUNT];

static double L_blended(const lat_t *L, sched_t *s,
                        const hc_blended_noise_t *b, const double *lx,
                        const double *ly, const double *lz) {
    /* 레인별 q (스칼라 복제 — noise_blended.c:53-78) */
    int over_all = 1, under_all = 1, over_any = 0, under_any = 0;
    for (int l = 0; l < 8; l++) {
        double d = (double)(int32_t)lx[l] * b->xz_mult;
        double e = (double)(int32_t)ly[l] * b->y_mult;
        double f = (double)(int32_t)lz[l] * b->xz_mult;
        double gx = d / b->xz_factor;
        double gy = e / b->y_factor;
        double gz = f / b->xz_factor;
        double smear_g = b->y_mult * b->smear / b->y_factor;
        double n = 0.0;
        double o = 1.0;
        for (int p = 0; p < 8; p++) {
            const hc_perlin_t *oct = oct_at(&b->main_noise, p);
            if (oct)
                n += hc_perlin_sample_scaled(
                         oct, hc_octaves_wrap(gx * o), hc_octaves_wrap(gy * o),
                         hc_octaves_wrap(gz * o), smear_g * o, gy * o) /
                     o;
            o /= 2.0;
        }
        double q = (n / 10.0 + 1.0) / 2.0;
        int    ov = q >= 1.0, un = q <= 0.0;
        over_all &= ov;
        under_all &= un;
        over_any |= ov;
        under_any |= un;
    }
    g_blend_execs++;
    /* 분류는 파티션이 아니다: 전-레인 내부 (over/under 어느 레인도 없음)
     * 는 양쪽 다 아님 — execs > onesided + mix 가 정상 (진단 계수) */
    if (over_all || under_all)
        g_blend_onesided++;
    if ((over_any && !over_all) || (under_any && !under_all))
        g_blend_mix++;

    /* 체인 모델. d/e/f: fmul; gx/gy/gz: 실나눗셈 (xz/y_factor 는 임의) */
    double t_def = L->fmul;
    double t_g = t_def + (is_pow2_d(b->xz_factor) ? L->fmul : L->fdiv);
    g_fp.mul += 3;
    g_fp.div += 3;
    /* main 8 옥타브: 입력 wrap(gx·o), yscale = smear_g·o ≠ 0 */
    double cp_y = L_perlin(L, 1);
    double n_t = 0.0;
    for (int p = 0; p < 8; p++) {
        if (!oct_at(&b->main_noise, p))
            continue;
        double t_wrap = t_g + L->fmul + L_wrap(L);
        double t_body = body(s, t_wrap, cp_y);
        g_perlin_calls++;
        fp_perlin(1);
        g_fp.mul += 4; /* gx·o ×3 + gy·o (ymax) */
        fp_wrap();
        fp_wrap();
        fp_wrap();
        /* n += s/o — o 는 2^-p, 정확-폴딩 → fmul */
        double contrib = t_body + L->fmul;
        g_fp.mul += 1;
        g_fp.mul_folded += 1;
        g_fp.div_real += 1;
        g_fp.add += 1;
        double r = contrib > n_t ? contrib : n_t;
        n_t = r + L->fadd;
    }
    /* q = (n/10 + 1)/2 : 실나눗셈 + add + 폴딩곱 */
    double t_q = n_t + L->fdiv + L->fadd + L->fmul;
    g_fp.div += 1;
    g_fp.add += 1;
    g_fp.mul += 1;
    g_fp.mul_folded += 1;
    g_fp.div_real += 1;

    /* lo/hi 뱅크 16 옥타브 (편측-생략은 마스크 판정대로) */
    double lo_t = 0.0, hi_t = 0.0;
    double cp_b = L_perlin(L, 1);
    for (int r = 0; r < 16; r++) {
        const hc_perlin_t *ol = over_all ? NULL : oct_at(&b->min_limit, r);
        const hc_perlin_t *oh = under_all ? NULL : oct_at(&b->max_limit, r);
        if (!ol && !oh)
            continue;
        /* s/t/u = wrap(d·o) 등 — 뱅크 공유 */
        double t_stu = t_def + L->fmul + L_wrap(L);
        g_fp.mul += 3;
        fp_wrap();
        fp_wrap();
        fp_wrap();
        if (ol) {
            double t_body = body(s, t_stu, cp_b);
            g_perlin_calls++;
            fp_perlin(1);
            g_fp.mul += 1; /* e·o (ymax) */
            double contrib = t_body + L->fmul; /* /o 폴딩 */
            g_fp.mul += 1;
            g_fp.mul_folded += 1;
            g_fp.div_real += 1;
            g_fp.add += 1;
            double rr = contrib > lo_t ? contrib : lo_t;
            lo_t = rr + L->fadd;
        }
        if (oh) {
            double t_body = body(s, t_stu, cp_b);
            g_perlin_calls++;
            fp_perlin(1);
            g_fp.mul += 1;
            double contrib = t_body + L->fmul;
            g_fp.mul += 1;
            g_fp.mul_folded += 1;
            g_fp.div_real += 1;
            g_fp.add += 1;
            double rr = contrib > hi_t ? contrib : hi_t;
            hi_t = rr + L->fadd;
        }
    }
    /* maskz + clampedLerp(q, lo/512, hi/512) + /128 — /512,/128 폴딩곱 */
    double t_band = (lo_t > hi_t ? lo_t : hi_t) + L->blendk + L->fmul;
    double lerp = L->fma ? L->fadd + L->ffma : L->fadd + L->fmul + L->fadd;
    double t_res = (t_q > t_band ? t_q : t_band) + lerp + 2.0 * L->blendk +
                   L->fmul;
    g_fp.mul += 4; /* lo/512, hi/512, /128 (폴딩) + lerp mul */
    g_fp.mul_folded += 3;
    g_fp.div_real += 3;
    g_fp.add += 1;
    g_fp.sub += 1;
    return t_res;
}

/* ---------- 스트림 타이밍 워커 (x8_run 미러) ---------- */

enum { MAXN = HC_DFC_MAX_NODES };

typedef struct {
    const hc_df_graph_t   *g;
    const lat_t           *L;
    sched_t                sch;
    double                 t[MAXN];   /* 노드 완료 시각 */
    double                 tmax;      /* 인보케이션 CP */
    const double          *val[8];    /* 레인별 오라클 scratch (2n) */
    const hc_df_cellctx_t *cc;
    const double          *lx, *ly, *lz; /* 레인 좌표 */
    uint64_t               nodes;     /* HC_CTR_X8_NODE 대응 (v>=0 만) */
    uint64_t               rc_mix, is_mix;
} sim_t;

static void bump(sim_t *S, int32_t idx, double t) {
    S->t[idx] = t;
    if (t > S->tmax)
        S->tmax = t;
    if (S->sch.serial && S->sch.ser > S->tmax)
        S->tmax = S->sch.ser;
}

static double tin1(sim_t *S, int32_t a) { return S->t[a]; }
static double tin2(sim_t *S, int32_t a, int32_t b) {
    return S->t[a] > S->t[b] ? S->t[a] : S->t[b];
}

static void sim_node(sim_t *S, int32_t idx) {
    const hc_df_node_t *nd = &S->g->nodes[idx];
    const lat_t        *L = S->L;
    double              t = 0.0;
    switch (nd->op) {
    case HC_DF_CONST:
    case HC_DF_X:
    case HC_DF_Y:
    case HC_DF_Z:
    case HC_DF_BLEND_OFFSET:
    case HC_DF_BLEND_ALPHA:
        t = 0.0;
        break;

    case HC_DF_ADD:
        t = tin2(S, nd->a, nd->b) + L->fadd;
        g_fp.add += 1;
        break;
    case HC_DF_MUL: { /* maskz mul: a 의 ±0 마스크가 체인에 얹힌다 */
        double tk = tin1(S, nd->a) + L->cmp;
        t = (S->t[nd->b] > tk ? S->t[nd->b] : tk) + L->fmul;
        g_fp.mul += 1;
        break;
    }
    case HC_DF_MIN:
    case HC_DF_MAX: /* cmp + 3 blend (Java NaN/±0 시맨틱 체인) */
        t = tin2(S, nd->a, nd->b) + L->cmp + 3.0 * L->blendk;
        break;
    case HC_DF_ADD_CONST:
        t = tin1(S, nd->a) + L->fadd;
        g_fp.add += 1;
        break;
    case HC_DF_MUL_CONST:
        t = tin1(S, nd->a) + L->fmul;
        g_fp.mul += 1;
        break;

    case HC_DF_ABS:
        t = tin1(S, nd->a) + L->ipadd; /* andnot — 비트 연산 */
        break;
    case HC_DF_SQUARE:
        t = tin1(S, nd->a) + L->fmul;
        g_fp.mul += 1;
        break;
    case HC_DF_CUBE:
        t = tin1(S, nd->a) + 2.0 * L->fmul;
        g_fp.mul += 2;
        break;
    case HC_DF_HALF_NEGATIVE:
    case HC_DF_QUARTER_NEGATIVE: /* cmp → masked mul */
        t = tin1(S, nd->a) + L->cmp + L->fmul;
        g_fp.mul += 1;
        break;
    case HC_DF_SQUEEZE: {
        /* clamp → max(d/2 [폴딩곱], d³/24 [실나눗셈]) → sub */
        double c = tin1(S, nd->a) + L->cmp + 4.0 * L->blendk;
        double p1 = c + L->fmul;
        double p2 = c + 2.0 * L->fmul + L->fdiv;
        t = (p2 > p1 ? p2 : p1) + L->fadd;
        g_fp.mul += 3;
        g_fp.mul_folded += 1;
        g_fp.div_real += 1;
        g_fp.div += 1; /* /24 실나눗셈 */
        g_fp.sub += 1;
        break;
    }
    case HC_DF_INVERT:
        t = tin1(S, nd->a) + L->fdiv;
        g_fp.div += 1;
        break;

    case HC_DF_CLAMP:
        t = tin1(S, nd->a) + L->cmp + 4.0 * L->blendk;
        break;

    case HC_DF_Y_CLAMPED_GRADIENT: {
        double dv = is_pow2_d(fabs(nd->k1 - nd->k0)) ? L->fmul : L->fdiv;
        double lerp =
            L->fma ? L->fadd + L->ffma : L->fadd + L->fmul + L->fadd;
        t = L->fadd + dv + lerp + 2.0 * L->blendk;
        g_fp.sub += 2;
        g_fp.div += 1;
        g_fp.mul += 1;
        g_fp.add += 1;
        break;
    }

    case HC_DF_RANGE_CHOICE: /* 평탄 노드 판 (비-세그먼트) */
        t = tin1(S, nd->a) + L->cmp + L->ipadd;
        if (S->t[nd->b] > t)
            t = S->t[nd->b];
        if (S->t[nd->c] > t)
            t = S->t[nd->c];
        t += L->blendk;
        break;

    case HC_DF_NOISE: {
        sched_t *sc = &S->sch;
        double   t0 = L->fmul; /* x·k0 등 */
        g_fp.mul += 3;
        t = L_normal(L, sc, &S->g->noises[nd->aux], t0);
        break;
    }
    case HC_DF_BLENDED_NOISE:
        t = L_blended(L, &S->sch, &S->g->blended[nd->aux], S->lx, S->ly,
                      S->lz);
        break;

    case HC_DF_INTERVAL_SELECT: { /* 평탄 노드 판 — eager 전 함수 계산 완 */
        int32_t nf = S->g->ipool[nd->aux];
        t = tin1(S, nd->a) + L->cmp;
        for (int32_t j = 0; j < nf; j++) {
            double tf = S->t[S->g->ipool[nd->aux + 1 + j]];
            if (tf > t)
                t = tf;
        }
        t += L->blendk;
        break;
    }

    case HC_DF_INTERPOLATED:
        if (S->cc && S->cc->interp_of[idx] >= 0 &&
            S->cc->mode == HC_DF_MODE_CELL) {
            double lerp =
                L->fma ? L->fadd + L->ffma : L->fadd + L->fmul + L->fadd;
            t = 3.0 * lerp; /* 코너/델타는 t=0 (사전 적재) */
            /* 1층 lerp 4개의 (b-a) 는 broadcast−broadcast — GCC 가
             * vsubsd 로 스칼라-폴딩 (디스어셈 실증: vsubsd+vbroadcastsd
             * 페어) → 벡터 sub 는 2/3층 3개만 (PMU vector_sub 대응) */
            g_fp.mul += 7;
            g_fp.add += 7;
            g_fp.sub += 3;
            g_interp_cell++;
        } else {
            t = tin1(S, nd->a); /* SP pass-through */
        }
        break;

    case HC_DF_FLAT_CACHE:
        if (S->cc && S->cc->flat_of[idx] >= 0)
            t = 0.0; /* 테이블 히트 — 사전 적재 (하한 보수) */
        else
            t = tin1(S, nd->a);
        break;

    case HC_DF_BLEND_DENSITY:
    case HC_DF_CACHE_2D:
    case HC_DF_CACHE_ONCE:
    case HC_DF_CACHE_ALL_IN_CELL:
        t = tin1(S, nd->a);
        break;

    default:
        die("unsupported op in x8 stream (whitelist violation?)", NULL);
    }
    S->nodes++;
    g_ops[nd->op]++;
    bump(S, idx, t);
}

static void sim_run(sim_t *S, const int32_t *p, int32_t words) {
    const int32_t *end = p + words;
    while (p < end) {
        int32_t v = *p++;
        if (v >= 0) {
            sim_node(S, v);
            continue;
        }
        if (v == -1) { /* PROG_RC */
            int32_t             ch = p[0], wt = p[1], we = p[2];
            const hc_df_node_t *nd = &S->g->nodes[ch];
            int                 m = 0;
            for (int l = 0; l < 8; l++) {
                double d = S->val[l][nd->a];
                if (d >= nd->k0 && d < nd->k1)
                    m |= 1 << l;
            }
            if (m != 0 && m != 0xFF)
                S->rc_mix++;
            if (m != 0)
                sim_run(S, p + 3, wt);
            if (m != 0xFF)
                sim_run(S, p + 3 + wt, we);
            double t = S->t[nd->a] + S->L->cmp + S->L->ipadd;
            if (m != 0 && S->t[nd->b] > t)
                t = S->t[nd->b];
            if (m != 0xFF && S->t[nd->c] > t)
                t = S->t[nd->c];
            bump(S, ch, t + S->L->blendk);
            p += 3 + wt + we;
        } else { /* PROG_IS */
            int32_t             ch = p[0], nf = p[1];
            const int32_t      *w = p + 2;
            const hc_df_node_t *nd = &S->g->nodes[ch];
            int32_t             sel[8];
            for (int l = 0; l < 8; l++) {
                double  dv = S->val[l][nd->a];
                int32_t sl = nf - 1;
                for (int32_t j = 0; j < nf - 1; j++)
                    if (dv < S->g->dpool[nd->aux2 + j]) {
                        sl = j;
                        break;
                    }
                sel[l] = sl;
            }
            int mix = 0;
            for (int l = 1; l < 8; l++)
                mix |= (sel[l] != sel[0]);
            if (mix)
                S->is_mix++;
            const int32_t *q = p + 2 + nf;
            double         t = S->t[nd->a] + S->L->cmp;
            for (int32_t k = 0; k < nf; k++) {
                int need = 0;
                for (int l = 0; l < 8; l++)
                    need |= (sel[l] == k);
                if (need && w[k] > 0)
                    sim_run(S, q, w[k]);
                q += w[k];
            }
            for (int l = 0; l < 8; l++) {
                double tf = S->t[S->g->ipool[nd->aux + 1 + sel[l]]];
                if (tf > t)
                    t = tf;
            }
            bump(S, ch, t + S->L->blendk);
            p = q;
        }
    }
}

/* ---------- 인보케이션 시뮬 (오라클 + 타이밍) ---------- */

static double g_orc[8][2 * MAXN]; /* 레인별 오라클 scratch */
static double g_seed[2 * MAXN];   /* 컬럼 inv 시드 */

typedef struct {
    uint64_t inv, nodes, rc_mix, is_mix;
    double   sum; /* Σ 인보케이션 시간 */
} agg_t;

static void sim_invocation(const hc_df_graph_t *g, const lat_t *L, int serial,
                           const int32_t *stream, int32_t words,
                           const int32_t *list, int32_t len, int seeded,
                           const hc_df_cellctx_t *cc_base, const double *lx,
                           const double *ly, const double *lz, agg_t *out) {
    for (int l = 0; l < 8; l++) {
        if (seeded)
            memcpy(g_orc[l], g_seed, sizeof(double) * 2 * (size_t)g->n);
        hc_df_eval_cone(g, list, len, -1, lx[l], ly[l], lz[l], g_orc[l],
                        cc_base, NULL);
    }
    static sim_t S; /* 큰 배열 — 스택 회피 */
    memset(S.t, 0, sizeof(double) * (size_t)g->n);
    S.g = g;
    S.L = L;
    S.sch.ser = 0.0;
    S.sch.serial = serial;
    S.tmax = 0.0;
    for (int l = 0; l < 8; l++)
        S.val[l] = g_orc[l];
    S.cc = cc_base;
    S.lx = lx;
    S.ly = ly;
    S.lz = lz;
    S.nodes = 0;
    S.rc_mix = 0;
    S.is_mix = 0;
    sim_run(&S, stream, words);
    out->inv += 1;
    out->nodes += S.nodes;
    out->rc_mix += S.rc_mix;
    out->is_mix += S.is_mix;
    out->sum += S.tmax;
}

/* CELL 모드 오라클은 cc.in_cell_* 를 레인별로 설정해야 한다 (eval_node
 * INTERPOLATED CELL 이 델타를 cc 에서 읽는다) — 전용 래퍼. */
static void sim_cell_invocation(hc_noise_chunk_t *nc, const lat_t *L,
                                int serial, const int32_t *stream,
                                int32_t words, int32_t iy, int32_t ix0,
                                agg_t *out) {
    const hc_df_graph_t *g = nc->g;
    double               lx[8], ly[8], lz[8];
    int32_t              by = nc->cell_start_y + iy;
    for (int l = 0; l < 8; l++) {
        lx[l] = (double)(nc->cell_start_x + ix0 + (l >> 2));
        ly[l] = (double)by;
        lz[l] = (double)(nc->cell_start_z + (l & 3));
    }
    hc_df_cellctx_t cc = nc->cc;
    cc.mode = HC_DF_MODE_CELL;
    cc.in_cell_y = iy;
    for (int l = 0; l < 8; l++) {
        cc.in_cell_x = ix0 + (l >> 2);
        cc.in_cell_z = l & 3;
        hc_df_eval_cone(g, nc->cone_cell.list, nc->cone_cell.len, -1, lx[l],
                        ly[l], lz[l], g_orc[l], &cc, NULL);
    }
    static sim_t S;
    memset(S.t, 0, sizeof(double) * (size_t)g->n);
    S.g = g;
    S.L = L;
    S.sch.ser = 0.0;
    S.sch.serial = serial;
    S.tmax = 0.0;
    for (int l = 0; l < 8; l++)
        S.val[l] = g_orc[l];
    S.cc = &cc; /* mode CELL — INTERPOLATED lerp3 지연 */
    S.lx = lx;
    S.ly = ly;
    S.lz = lz;
    S.nodes = 0;
    S.rc_mix = 0;
    S.is_mix = 0;
    sim_run(&S, stream, words);
    out->inv += 1;
    out->nodes += S.nodes;
    out->rc_mix += S.rc_mix;
    out->is_mix += S.is_mix;
    out->sum += S.tmax;
}

/* 슬라이스 채움의 x8 그룹 시뮬 — noise_chunk.c nc_fill_slice x8 경로의
 * 기하 (y 8점 그룹, 꼬리 49번째 점은 스칼라라 제외) 재현. */
static void sim_fill_slice(hc_noise_chunk_t *nc, const lat_t *L, int serial,
                           int32_t abs_cell_x, agg_t *out) {
    const hc_df_graph_t *g = nc->g;
    int32_t              csx = abs_cell_x * nc->cell_width;
    const int32_t *stream = nc->cone_slice_var.prog ? nc->cone_slice_var.prog
                                                    : nc->cone_slice_var.list;
    int32_t swords = nc->cone_slice_var.prog ? nc->cone_slice_var.prog_words
                                             : nc->cone_slice_var.len;
    hc_df_cellctx_t cc = nc->cc;
    cc.mode = HC_DF_MODE_SP;
    for (int32_t j = 0; j <= nc->cell_count_xz; j++) {
        int32_t bz = (nc->first_cell_z + j) * nc->cell_width;
        /* 컬럼 inv 콘 1회 (순수 — 실제 fill_slice 와 같은 값) */
        hc_df_eval_cone(g, nc->cone_slice_inv.list, nc->cone_slice_inv.len,
                        -1, (double)csx,
                        (double)(nc->cell_noise_min_y * nc->cell_height),
                        (double)bz, g_seed, &cc, NULL);
        for (int32_t i = 0; i + 7 <= nc->cell_count_y; i += 8) {
            double lx[8], ly[8], lz[8];
            for (int l = 0; l < 8; l++) {
                lx[l] = (double)csx;
                ly[l] = (double)((i + l + nc->cell_noise_min_y) *
                                 nc->cell_height);
                lz[l] = (double)bz;
            }
            sim_invocation(g, L, serial, stream, swords,
                           nc->cone_slice_var.list, nc->cone_slice_var.len,
                           1, &cc, lx, ly, lz, out);
        }
    }
}

/* ---------- 메인 ---------- */

int main(int argc, char **argv) {
    const char *repo = ".";
    const char *preset = "low";
    int         n_coords = 0, want_fma = 0, want_reassoc = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--repo") && i + 1 < argc)
            repo = argv[++i];
        else if (!strcmp(argv[i], "--lat") && i + 1 < argc)
            preset = argv[++i];
        else if (!strcmp(argv[i], "--coords") && i + 1 < argc)
            n_coords = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fma"))
            want_fma = 1;
        else if (!strcmp(argv[i], "--reassoc"))
            want_reassoc = 1;
        else
            die("unknown arg", argv[i]);
    }
    lat_t L;
    if (!strcmp(preset, "low"))
        L = lat_low();
    else if (!strcmp(preset, "nom"))
        L = lat_nom();
    else {
        die("unknown --lat (low|nom)", preset);
        return 2;
    }
    L.fma = want_fma;
    L.reassoc = want_reassoc;

    int64_t seed = 1234567890;
    hc_arena_init(&g_arena, g_backing, sizeof g_backing);
    hc_isa_force(HC_ISA_SCALAR); /* 시뮬 전용 — 커널 실행 없음 */
    df_setup(repo, seed);
    if (g_graph.n > MAXN)
        die("graph larger than MAXN", NULL);

    /* microbench 와 동일한 r.0.0 산개 8좌표 */
    static const int32_t coords[][2] = {{0, 0},   {31, 31}, {5, 17},
                                        {17, 5},  {11, 28}, {28, 11},
                                        {23, 23}, {8, 8}};
    int max_coords = (int)(sizeof coords / sizeof coords[0]);
    if (n_coords < 1 || n_coords > max_coords)
        n_coords = max_coords;

    printf("cp mode chaincp\n");
    printf("cp lat %s fma %d reassoc %d\n", preset, want_fma, want_reassoc);
    printf("cp coords %d\n", n_coords);
    printf("cp graph_nodes %d\n", g_graph.n);
    printf("cp perlin_cp_y0 %.1f\n", L_perlin(&L, 0));
    printf("cp perlin_cp_y1 %.1f\n", L_perlin(&L, 1));
    printf("cp wrap_cp %.1f\n", L_wrap(&L));

    size_t snap = g_arena.off;
    for (int serial = 1; serial >= 0; serial--) {
        agg_t slice, cell;
        memset(&slice, 0, sizeof slice);
        memset(&cell, 0, sizeof cell);
        memset(&g_fp, 0, sizeof g_fp);
        memset(g_ops, 0, sizeof g_ops);
        g_perlin_calls = 0;
        g_interp_cell = 0;
        g_blend_execs = g_blend_onesided = g_blend_mix = 0;

        for (int c = 0; c < n_coords; c++) {
            g_arena.off = snap;
            hc_noise_chunk_t *nc = hc_arena_alloc(&g_arena, sizeof *nc,
                                                  _Alignof(hc_noise_chunk_t));
            if (!nc || hc_nc_init(nc, &g_arena, &g_graph, &g_roots, seed,
                                  coords[c][0], coords[c][1], 63))
                die("hc_nc_init failed", NULL);
            if (!nc->cone_slice_var.x4_ok || !nc->cone_cell.x4_ok)
                die("stream not x4/x8-eligible (unexpected)", NULL);

            /* 그림자 패스 — microbench kernel_pass 의 nc 시퀀스 + 시뮬 */
            hc_nc_initialize_first_cell_x(nc);
            sim_fill_slice(nc, &L, serial, nc->first_cell_x, &slice);
            int32_t cells_xz = 16 / nc->cell_width;
            const int32_t *cstream = nc->cone_cell.prog ? nc->cone_cell.prog
                                                        : nc->cone_cell.list;
            int32_t cwords = nc->cone_cell.prog ? nc->cone_cell.prog_words
                                                : nc->cone_cell.len;
            for (int32_t cell_x = 0; cell_x < cells_xz; cell_x++) {
                hc_nc_advance_cell_x(nc, cell_x);
                sim_fill_slice(nc, &L, serial,
                               nc->first_cell_x + cell_x + 1, &slice);
                for (int32_t cell_z = 0; cell_z < cells_xz; cell_z++)
                    for (int32_t cell_y = nc->cell_count_y - 1; cell_y >= 0;
                         cell_y--) {
                        hc_nc_select_cell_yz(nc, cell_y, cell_z);
                        for (int32_t iy = nc->cell_height - 1; iy >= 0; iy--) {
                            sim_cell_invocation(nc, &L, serial, cstream,
                                                cwords, iy, 0, &cell);
                            sim_cell_invocation(nc, &L, serial, cstream,
                                                cwords, iy, 2, &cell);
                        }
                    }
                hc_nc_swap_slices(nc);
            }
            fprintf(stderr, "  coord (%d,%d) done (serial=%d)\n",
                    coords[c][0], coords[c][1], serial);
        }

        const char *tag = serial ? "serial" : "dfcp";
        uint64_t    nodes = slice.nodes + cell.nodes;
        double      total = slice.sum + cell.sum;
        printf("cp %s_inv_slice %" PRIu64 "\n", tag, slice.inv);
        printf("cp %s_inv_cell %" PRIu64 "\n", tag, cell.inv);
        printf("cp %s_nodes_slice %" PRIu64 "\n", tag, slice.nodes);
        printf("cp %s_nodes_cell %" PRIu64 "\n", tag, cell.nodes);
        printf("cp %s_nodes_total %" PRIu64 "\n", tag, nodes);
        printf("cp %s_cyc_slice %.0f\n", tag, slice.sum);
        printf("cp %s_cyc_cell %.0f\n", tag, cell.sum);
        printf("cp %s_cyc_total %.0f\n", tag, total);
        printf("cp %s_cyc_per_node %.3f\n", tag, total / (double)nodes);
        printf("cp %s_cyc_per_node_w2 %.3f\n", tag,
               total / (double)nodes / 2.0);
        printf("cp %s_rc_mix %" PRIu64 " is_mix %" PRIu64 "\n", tag,
               slice.rc_mix + cell.rc_mix, slice.is_mix + cell.is_mix);
        printf("cp %s_perlin_calls %" PRIu64 " per_node %.4f\n", tag,
               g_perlin_calls, (double)g_perlin_calls / (double)nodes);
        printf("cp %s_blend_execs %" PRIu64 " onesided %" PRIu64
               " mix %" PRIu64 "\n",
               tag, g_blend_execs, g_blend_onesided, g_blend_mix);
        printf("cp %s_fp_per_node mul %.3f add %.3f sub %.3f div %.3f "
               "(mul_unfolded %.3f div_unfolded %.3f)\n",
               tag, (double)g_fp.mul / (double)nodes,
               (double)g_fp.add / (double)nodes,
               (double)g_fp.sub / (double)nodes,
               (double)g_fp.div / (double)nodes,
               (double)(g_fp.mul - g_fp.mul_folded) / (double)nodes,
               (double)(g_fp.div + g_fp.div_real) / (double)nodes);
        printf("cp %s_interp_cell_execs %" PRIu64 "\n", tag, g_interp_cell);
        if (serial) { /* 히스토그램은 1회면 충분 */
            for (int op = 0; op < HC_DF_OP_COUNT; op++)
                if (g_ops[op])
                    printf("cp op %d count %" PRIu64 "\n", op, g_ops[op]);
        }
    }
    return 0;
}
