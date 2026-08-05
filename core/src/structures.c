/* Task 14 구조물 파이프라인 코어 — 스타트 소스/위치계산/References/스텝
 * 디스패치/공유 헬퍼. 배치 본문은 structures_template.c (템플릿 계열) /
 * structures_mineshaft.c (mineshaft). 계약과 범위는 hc_structures.h 헤더
 * 주석, 시맨틱 근거는 .hermes/notes/task14-fullregion/R-*.md.
 *
 * 스타트 소스 3분류 (완료 노트 §구조물 순서 고정 근거):
 *  1. in-region 4건 — 골든 starts NBT (ADR-003 D4 재생 입력; .mca 가
 *     in-region 스타트의 완전한 기록이므로 후보 위치계산의 바이옴 검사를
 *     대체한다 — random_spread 후보 청크 산식은 교차검증으로만 사용).
 *  2. mineshaft — 스타트가 전부 리전 밖 (r.0.-1/r.1.*) 이라 골든 피스
 *     리스트가 없다. 비직소 (ADR 범위 내) → 시드에서 위치계산+조립을
 *     전부 파생하고, in-region References 와 전수 교차검증한다.
 *  3. trial_chambers (13,35) — 리전 밖 직소 스타트 (조립 제외 대상).
 *     피스가 r.0.0 에 블록을 놓지 않음은 실측 (z=31 행 팔레트에 챔버
 *     블록 부재); References 멤버십 (c.7..14.31) 은 골든 실측으로 고정. */

#include "hc_structures.h"

#include "features_internal.h" /* hc_jset (HashSet<BlockPos> 에뮬) */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define die hc_featx_die

/* ================= 상태 회전/미러 (BlockState.mirror/rotate) =================
 *
 * 프로퍼티 문자열 재작성. 커버 대상 = 이 리전의 템플릿/마인샤프트 팔레트
 * (R-blockprops*.tsv 의 상태들). 방향성 프로퍼티:
 *  - facing (n/e/s/w; up/down 불변)
 *  - axis (x<->z 회전; 미러 불변)
 *  - north/east/south/west 불리언 (fence/vine/tripwire/glow_lichen)
 *  - rail shape (north_south<->east_west; 곡선 미등장 — die)
 *  - stairs shape (미러에서 left<->right 스왑; 회전 불변)
 *  - jigsaw orientation / door hinge 미러 — 이 리전 미사용 (도달 die)
 * 그 외 프로퍼티는 불변. 결과 상태 미등재 = die (fail-loud). */

int hc_state_parse(const char *name, char *base, size_t base_cap,
                   hc_skv_t *kv, int cap) {
    const char *br = strchr(name, '[');
    if (!br) {
        snprintf(base, base_cap, "%s", name);
        return 0;
    }
    assert((size_t)(br - name) < base_cap);
    memcpy(base, name, (size_t)(br - name));
    base[br - name] = '\0';
    int         n = 0;
    const char *p = br + 1;
    while (*p != ']') {
        const char *eq = strchr(p, '=');
        const char *end = eq + 1;
        while (*end != ',' && *end != ']')
            end++;
        assert(n < cap);
        assert((size_t)(eq - p) < sizeof kv[0].k);
        assert((size_t)(end - eq - 1) < sizeof kv[0].v);
        memcpy(kv[n].k, p, (size_t)(eq - p));
        kv[n].k[eq - p] = '\0';
        memcpy(kv[n].v, eq + 1, (size_t)(end - eq - 1));
        kv[n].v[end - eq - 1] = '\0';
        n++;
        p = *end == ',' ? end + 1 : end;
    }
    return n;
}

uint16_t hc_state_build(const char *base, hc_skv_t *kv, int n) {
    /* 캐노니컬 = 프로퍼티 알파벳 오름차순 */
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (strcmp(kv[a].k, kv[b].k) > 0) {
                hc_skv_t t = kv[a];
                kv[a] = kv[b];
                kv[b] = t;
            }
    char buf[256];
    int  off = snprintf(buf, sizeof buf, "%s", base);
    if (n) {
        off += snprintf(buf + off, sizeof buf - (size_t)off, "[");
        for (int i = 0; i < n; i++)
            off += snprintf(buf + off, sizeof buf - (size_t)off, "%s%s=%s",
                            i ? "," : "", kv[i].k, kv[i].v);
        off += snprintf(buf + off, sizeof buf - (size_t)off, "]");
    }
    int32_t id = hc_block_by_name(buf, (int32_t)strlen(buf));
    if (id < 0) {
        if (hc_features_survey) {
            /* 서베이: 미등재 상태 열거 (게이트 실행은 항상 die) —
             * 자리 채움은 air (마진 청크 서베이 전용, 직렬화 무관) */
            fprintf(stderr, "hc_structures SURVEY MISSING_STATE %s\n", buf);
            return 0;
        }
        die("rotated/mirrored state not registered", buf);
    }
    return (uint16_t)id;
}

/* facing 값 회전: rot 단계 (CW90 단위) 만큼 n→e→s→w */
static const char *rot_facing(const char *v, int steps) {
    static const char *R[4] = {"north", "east", "south", "west"};
    for (int i = 0; i < 4; i++)
        if (strcmp(v, R[i]) == 0)
            return R[(i + steps) & 3];
    return v; /* up/down 등 불변 */
}

static int rot_steps(int rot) {
    switch (rot) {
    case HC_ROT_CW90:
        return 1;
    case HC_ROT_CW180:
        return 2;
    case HC_ROT_CCW90:
        return 3;
    default:
        return 0;
    }
}

uint16_t hc_state_rotate(uint16_t s, int rot) {
    if (rot == HC_ROT_NONE)
        return s;
    const char *nm = hc_block_name(s);
    char        base[128];
    hc_skv_t    kv[16];
    int         n = hc_state_parse(nm, base, sizeof base, kv, 16);
    if (n == 0)
        return s;
    int steps = rot_steps(rot);
    /* 다면 불리언은 값 재배치 (키 이름이 방향) */
    int  has_dirs = 0;
    char dv[4][sizeof kv[0].v]; /* n,e,s,w 원 값 */
    for (int i = 0; i < n; i++)
        if (strcmp(kv[i].k, "north") == 0 || strcmp(kv[i].k, "east") == 0 ||
            strcmp(kv[i].k, "south") == 0 || strcmp(kv[i].k, "west") == 0)
            has_dirs = 1;
    if (has_dirs) {
        const char *names[4] = {"north", "east", "south", "west"};
        for (int d = 0; d < 4; d++)
            for (int i = 0; i < n; i++)
                if (strcmp(kv[i].k, names[d]) == 0)
                    memcpy(dv[d], kv[i].v, sizeof dv[d]);
        for (int d = 0; d < 4; d++)
            for (int i = 0; i < n; i++)
                if (strcmp(kv[i].k, names[(d + steps) & 3]) == 0)
                    memcpy(kv[i].v, dv[d], sizeof kv[i].v);
    }
    for (int i = 0; i < n; i++) {
        if (strcmp(kv[i].k, "facing") == 0) {
            const char *nv = rot_facing(kv[i].v, steps);
            if (nv != kv[i].v) /* up/down 은 자기 자신 — 중첩 복사 금지 */
                snprintf(kv[i].v, sizeof kv[i].v, "%s", nv);
        } else if (strcmp(kv[i].k, "axis") == 0 && (steps & 1)) {
            if (strcmp(kv[i].v, "x") == 0)
                snprintf(kv[i].v, sizeof kv[i].v, "z");
            else if (strcmp(kv[i].v, "z") == 0)
                snprintf(kv[i].v, sizeof kv[i].v, "x");
        } else if (strcmp(kv[i].k, "shape") == 0) {
            if (strcmp(kv[i].v, "north_south") == 0 ||
                strcmp(kv[i].v, "east_west") == 0) { /* rail */
                if (steps & 1)
                    snprintf(kv[i].v, sizeof kv[i].v, "%s",
                             strcmp(kv[i].v, "north_south") == 0
                                 ? "east_west"
                                 : "north_south");
            }
            /* stairs shape 는 회전 불변 */
        } else if (strcmp(kv[i].k, "orientation") == 0)
            die("jigsaw orientation rotate unreachable", nm);
    }
    return hc_state_build(base, kv, n);
}

uint16_t hc_state_mirror(uint16_t s, int mir) {
    if (mir == HC_MIR_NONE)
        return s;
    const char *nm = hc_block_name(s);
    char        base[128];
    hc_skv_t    kv[16];
    int         n = hc_state_parse(nm, base, sizeof base, kv, 16);
    if (n == 0)
        return s;
    const char *a = mir == HC_MIR_LEFT_RIGHT ? "north" : "east";
    const char *b = mir == HC_MIR_LEFT_RIGHT ? "south" : "west";
    /* 다면 불리언 스왑 */
    char *va = NULL, *vb = NULL;
    for (int i = 0; i < n; i++) {
        if (strcmp(kv[i].k, a) == 0)
            va = kv[i].v;
        if (strcmp(kv[i].k, b) == 0)
            vb = kv[i].v;
    }
    if (va && vb) {
        char t[32];
        snprintf(t, sizeof t, "%s", va);
        snprintf(va, sizeof kv[0].v, "%s", vb);
        snprintf(vb, sizeof kv[0].v, "%s", t);
    }
    for (int i = 0; i < n; i++) {
        if (strcmp(kv[i].k, "facing") == 0) {
            if (strcmp(kv[i].v, a) == 0)
                snprintf(kv[i].v, sizeof kv[i].v, "%s", b);
            else if (strcmp(kv[i].v, b) == 0)
                snprintf(kv[i].v, sizeof kv[i].v, "%s", a);
        } else if (strcmp(kv[i].k, "shape") == 0 &&
                   strncmp(kv[i].v, "north", 5) != 0 &&
                   strncmp(kv[i].v, "east", 4) != 0) {
            /* stairs: left<->right (StairBlock.mirror) */
            if (strstr(kv[i].v, "_left")) {
                char t[32];
                snprintf(t, sizeof t, "%.*s_right",
                         (int)(strstr(kv[i].v, "_left") - kv[i].v),
                         kv[i].v);
                snprintf(kv[i].v, sizeof kv[i].v, "%s", t);
            } else if (strstr(kv[i].v, "_right")) {
                char t[32];
                snprintf(t, sizeof t, "%.*s_left",
                         (int)(strstr(kv[i].v, "_right") - kv[i].v),
                         kv[i].v);
                snprintf(kv[i].v, sizeof kv[i].v, "%s", t);
            }
        } else if (strcmp(kv[i].k, "hinge") == 0)
            die("door hinge mirror unreachable in this region", nm);
    }
    return hc_state_build(base, kv, n);
}

/* StructureTemplate.transform (디컴파일 @560-585 그대로) */
void hc_template_transform(int32_t *x, int32_t *y, int32_t *z, int mir,
                           int rot, int32_t pivot_x, int32_t pivot_z) {
    (void)y;
    int32_t xx = *x, zz = *z;
    if (mir == HC_MIR_LEFT_RIGHT)
        zz = -zz;
    else if (mir == HC_MIR_FRONT_BACK)
        xx = -xx;
    switch (rot) {
    case HC_ROT_CCW90:
        *x = pivot_x - pivot_z + zz;
        *z = pivot_x + pivot_z - xx;
        return;
    case HC_ROT_CW90:
        *x = pivot_x + pivot_z - zz;
        *z = pivot_z - pivot_x + xx;
        return;
    case HC_ROT_CW180:
        *x = pivot_x + pivot_x - xx;
        *z = pivot_z + pivot_z - zz;
        return;
    default:
        *x = xx;
        *z = zz;
        return;
    }
}

/* ================= BE 레코더 ================= */

int hc_be_recorder_init(hc_be_recorder_t *r, hc_arena_t *a, int32_t cap) {
    r->recs = hc_arena_alloc(a, sizeof(hc_be_rec_t) * (size_t)cap,
                             _Alignof(hc_be_rec_t));
    if (!r->recs)
        return -1;
    r->n = 0;
    r->cap = cap;
    return 0;
}

hc_be_rec_t *hc_be_record(hc_be_recorder_t *r, int32_t x, int32_t y,
                          int32_t z, uint8_t kind, uint16_t state) {
    for (int32_t i = 0; i < r->n; i++)
        if (!r->recs[i].dead && r->recs[i].x == x && r->recs[i].y == y &&
            r->recs[i].z == z)
            r->recs[i].dead = 1; /* 덮어쓰기 */
    if (r->n >= r->cap)
        die("BE recorder capacity exceeded", NULL);
    hc_be_rec_t *e = &r->recs[r->n++];
    memset(e, 0, sizeof *e);
    e->x = x;
    e->y = y;
    e->z = z;
    e->kind = kind;
    e->state = state;
    return e;
}

void hc_be_remove(hc_be_recorder_t *r, int32_t x, int32_t y, int32_t z) {
    for (int32_t i = 0; i < r->n; i++)
        if (!r->recs[i].dead && r->recs[i].x == x && r->recs[i].y == y &&
            r->recs[i].z == z)
            r->recs[i].dead = 1;
}

/* ================= fastutil 에뮬 (R-fastutil, 8.5.18) ================= */

static int64_t fu_mix64(int64_t x) {
    uint64_t h = (uint64_t)x * 0x9E3779B97F4A7C15ull;
    h ^= h >> 32;
    return (int64_t)(h ^ (h >> 16));
}

static uint32_t fu_mix32(int32_t x) {
    uint32_t h = (uint32_t)x * 0x9E3779B9u;
    return h ^ (h >> 16);
}

/* LongOpenHashSet: 기본 n=32(mask 31), 전방 프로브, 방출 = [0(있으면)] +
 * 인덱스 내림차순. 우리 셋은 <=8 원소라 rehash 없음 (assert). */
int32_t hc_longset_to_array(const int64_t *added, int32_t n, int64_t *out) {
    enum { N = 32 };
    int64_t key[N];
    int     used[N];
    memset(used, 0, sizeof used);
    int has_zero = 0;
    int size = 0;
    assert(n <= 24); /* maxFill */
    for (int32_t i = 0; i < n; i++) {
        int64_t k = added[i];
        if (k == 0) {
            has_zero = 1;
            continue;
        }
        int pos = (int)((uint64_t)fu_mix64(k) & (N - 1));
        while (used[pos]) {
            if (key[pos] == k)
                goto dup;
            pos = (pos + 1) & (N - 1);
        }
        used[pos] = 1;
        key[pos] = k;
        size++;
    dup:;
    }
    int32_t m = 0;
    if (has_zero)
        out[m++] = 0;
    for (int pos = N - 1; pos >= 0; pos--)
        if (used[pos])
            out[m++] = key[pos];
    (void)size;
    return m;
}

/* Object2ObjectOpenHashMap<BlockPos> 키 순회 (mix32(Vec3i.hashCode)) */
int32_t hc_o2omap_key_order(const int32_t (*pos)[3], int32_t n,
                            int32_t *order_out) {
    enum { NMAX = 4096 };
    static int32_t slot_of[NMAX]; /* 엔트리 인덱스 저장 */
    int32_t        cap = 16;      /* 기본 */
    while (n > cap * 3 / 4)
        cap <<= 1;
    /* fastutil arraySize(expected=16, .75) = 32 가 초기값; put 마다
     * size>=maxFill 이면 배가 (재배치는 n-1..0 스캔) — 여기서는 최종
     * 삽입 시퀀스를 처음부터 재생하며 rehash 를 재현한다. */
    int32_t N = 32;
    while (1) {
        int32_t maxfill = N / 4 * 3;
        if (n <= maxfill)
            break;
        N <<= 1;
    }
    /* 재해시 이력 재현: 삽입 중 rehash 가 일어나면 배치가 달라진다.
     * 우리 사용처 (청크당 BE <=17) 는 기본 32 슬롯에서 rehash 불가
     * (maxFill 24) — 단순 경로만 구현. */
    assert(n <= 24 && N == 32);
    (void)cap;
    int32_t table[32];
    for (int i = 0; i < 32; i++)
        table[i] = -1;
    int has_null = 0; /* BlockPos null 키 없음 */
    (void)has_null;
    for (int32_t i = 0; i < n; i++) {
        /* Vec3i.hashCode = (y + z*31)*31 + x (int 랩) */
        int32_t h = (int32_t)(((uint32_t)((pos[i][1] +
                                           (int32_t)((uint32_t)pos[i][2] *
                                                     31u)) ) *
                               31u) +
                              (uint32_t)pos[i][0]);
        int     p = (int)(fu_mix32(h) & 31u);
        while (table[p] >= 0) {
            const int32_t *q = pos[table[p]];
            if (q[0] == pos[i][0] && q[1] == pos[i][1] && q[2] == pos[i][2])
                goto dup2;
            p = (p + 1) & 31;
        }
        table[p] = i;
    dup2:;
        assert(i < NMAX);
        (void)slot_of;
    }
    int32_t m = 0;
    for (int p = 31; p >= 0; p--)
        if (table[p] >= 0)
            order_out[m++] = table[p];
    return m;
}

/* ================= 골든 starts 로드 ================= */

static char *read_whole(hc_arena_t *a, const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = hc_arena_alloc(a, (size_t)sz + 1, 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz)
        die("short read", path);
    fclose(f);
    buf[sz] = '\0';
    if (len)
        *len = (size_t)sz;
    return buf;
}

static int32_t tag_i32(const hc_nbt_t *c, const char *k) {
    const hc_nbt_t *v = hc_nbt_get(c, k);
    if (!v)
        die("starts tag missing key", k);
    return (int32_t)hc_nbt_i64(v);
}

static const char *tag_str(const hc_nbt_t *c, const char *k) {
    const hc_nbt_t *v = hc_nbt_get(c, k);
    if (!v)
        die("starts tag missing key", k);
    return hc_nbt_str(v);
}

static void tag_bb(const hc_nbt_t *c, int32_t bb[6]) {
    int32_t        n = 0;
    const int32_t *a = hc_nbt_ia(hc_nbt_get(c, "BB"), &n);
    if (n != 6)
        die("BB not int[6]", NULL);
    memcpy(bb, a, sizeof(int32_t) * 6);
}

static uint8_t parse_rot(const char *s) {
    if (strcmp(s, "NONE") == 0)
        return HC_ROT_NONE;
    if (strcmp(s, "CLOCKWISE_90") == 0)
        return HC_ROT_CW90;
    if (strcmp(s, "CLOCKWISE_180") == 0)
        return HC_ROT_CW180;
    if (strcmp(s, "COUNTERCLOCKWISE_90") == 0)
        return HC_ROT_CCW90;
    die("unknown rotation", s);
    return 0;
}

static uint8_t parse_mir(const char *s) {
    if (strcmp(s, "NONE") == 0)
        return HC_MIR_NONE;
    if (strcmp(s, "LEFT_RIGHT") == 0)
        return HC_MIR_LEFT_RIGHT;
    if (strcmp(s, "FRONT_BACK") == 0)
        return HC_MIR_FRONT_BACK;
    die("unknown mirror", s);
    return 0;
}

/* 스텝/순번 (R-placement §6 — 26.2 레지스트리 스냅샷) */
static void step_of(const char *name, uint8_t *step, uint8_t *idx) {
    if (strcmp(name, "minecraft:mineshaft") == 0) {
        *step = 3;
        *idx = 1;
    } else if (strcmp(name, "minecraft:trial_chambers") == 0) {
        *step = 3;
        *idx = 4;
    } else if (strcmp(name, "minecraft:ocean_ruin_warm") == 0) {
        *step = 4;
        *idx = 8;
    } else if (strcmp(name, "minecraft:ruined_portal_ocean") == 0) {
        *step = 4;
        *idx = 15;
    } else if (strcmp(name, "minecraft:shipwreck_beached") == 0) {
        *step = 4;
        *idx = 18;
    } else
        die("structure without step mapping", name);
}

static void parse_start_tag(hc_sctx_t *sc, hc_sstart_t *st,
                            const hc_nbt_t *root_starts, int32_t scx,
                            int32_t scz) {
    if (hc_nbt_comp_count(root_starts) != 1)
        die("multi-structure starts chunk unexpected", NULL);
    const char     *name = NULL;
    const hc_nbt_t *tag = hc_nbt_comp_at(root_starts, 0, &name);
    st->name = name;
    st->scx = scx;
    st->scz = scz;
    st->tag = root_starts;
    step_of(name, &st->step, &st->step_index);
    if (tag_i32(tag, "ChunkX") != scx || tag_i32(tag, "ChunkZ") != scz)
        die("starts chunk pos mismatch", name);
    const hc_nbt_t *children = hc_nbt_get(tag, "Children");
    int32_t         np = hc_nbt_list_count(children);
    st->n_pieces = np;
    st->pieces = hc_arena_alloc(sc->arena, sizeof(hc_spiece_t) * (size_t)np,
                                _Alignof(hc_spiece_t));
    if (!st->pieces)
        die("arena exhausted (pieces)", NULL);
    int32_t sbb[6] = {INT32_MAX, INT32_MAX, INT32_MAX,
                      INT32_MIN, INT32_MIN, INT32_MIN};
    for (int32_t i = 0; i < np; i++) {
        const hc_nbt_t *c = hc_nbt_list_at(children, i);
        hc_spiece_t    *p = &st->pieces[i];
        memset(p, 0, sizeof *p);
        tag_bb(c, p->bb);
        p->o = (int8_t)tag_i32(c, "O");
        p->gd = tag_i32(c, "GD");
        const char *pid = tag_str(c, "id");
        if (strcmp(pid, "minecraft:shipwreck") == 0) {
            p->kind = HC_SP_TEMPLATE_SHIPWRECK;
            p->tpx = tag_i32(c, "TPX");
            p->tpy = tag_i32(c, "TPY");
            p->tpz = tag_i32(c, "TPZ");
            p->rot = parse_rot(tag_str(c, "Rot"));
            p->mir = HC_MIR_NONE;
            p->is_beached = (uint8_t)tag_i32(c, "isBeached");
            /* 골든은 배치 후 기록 (height_adjusted=1, TPY 조정 완료).
             * 재생은 이벤트 재현: 래치 0 에서 시작 — 배치가 재계산 후
             * 골든 TPY 와 일치를 assert (structures_template.c). */
            p->height_adjusted = 0;
            p->procs = HC_PROCS_SHIPWRECK;
            p->tmpl =
                hc_template_load(sc->arena, sc->template_dir, tag_str(c, "Template"));
        } else if (strcmp(pid, "minecraft:orp") == 0) {
            p->kind = HC_SP_TEMPLATE_OCEAN_RUIN;
            p->tpx = tag_i32(c, "TPX");
            p->tpy = tag_i32(c, "TPY");
            p->tpz = tag_i32(c, "TPZ");
            p->rot = parse_rot(tag_str(c, "Rot"));
            p->mir = HC_MIR_NONE;
            p->integrity = hc_nbt_f32(hc_nbt_get(c, "Integrity"));
            p->procs = HC_PROCS_OCEAN_RUIN;
            p->tmpl =
                hc_template_load(sc->arena, sc->template_dir, tag_str(c, "Template"));
        } else if (strcmp(pid, "minecraft:rupo") == 0) {
            p->kind = HC_SP_TEMPLATE_PORTAL;
            p->tpx = tag_i32(c, "TPX");
            p->tpy = tag_i32(c, "TPY");
            p->tpz = tag_i32(c, "TPZ");
            p->rot = parse_rot(tag_str(c, "Rotation"));
            p->mir = parse_mir(tag_str(c, "Mirror"));
            p->vertical_placement = tag_str(c, "VerticalPlacement");
            const hc_nbt_t *props = hc_nbt_get(c, "Properties");
            p->cold = (uint8_t)tag_i32(props, "cold");
            p->mossiness = hc_nbt_f32(hc_nbt_get(props, "mossiness"));
            p->air_pocket = (uint8_t)tag_i32(props, "air_pocket");
            p->overgrown = (uint8_t)tag_i32(props, "overgrown");
            p->has_vines = (uint8_t)tag_i32(props, "vines");
            p->replace_blackstone =
                (uint8_t)tag_i32(props, "replace_with_blackstone");
            p->procs = HC_PROCS_PORTAL;
            p->tmpl =
                hc_template_load(sc->arena, sc->template_dir, tag_str(c, "Template"));
        } else if (strcmp(pid, "minecraft:jigsaw") == 0) {
            p->kind = HC_SP_JIGSAW;
            p->tpx = tag_i32(c, "PosX");
            p->tpy = tag_i32(c, "PosY");
            p->tpz = tag_i32(c, "PosZ");
            p->rot = parse_rot(tag_str(c, "rotation"));
            p->mir = HC_MIR_NONE;
            p->known_shape = 1;
            /* beardifier 입력 (Beardifier.forStructuresInChunk) */
            p->gld = tag_i32(c, "ground_level_delta");
            const hc_nbt_t *jl = hc_nbt_get(c, "junctions");
            p->n_junctions = jl ? hc_nbt_list_count(jl) : 0;
            if (p->n_junctions) {
                hc_beard_junction_t *js = hc_arena_alloc(
                    sc->arena,
                    sizeof(hc_beard_junction_t) * (size_t)p->n_junctions,
                    _Alignof(hc_beard_junction_t));
                if (!js)
                    die("arena exhausted (junctions)", NULL);
                for (int32_t j = 0; j < p->n_junctions; j++) {
                    const hc_nbt_t *jc = hc_nbt_list_at(jl, j);
                    js[j].sx = tag_i32(jc, "source_x");
                    js[j].sgy = tag_i32(jc, "source_ground_y");
                    js[j].sz = tag_i32(jc, "source_z");
                }
                p->junctions = js;
            }
            const hc_nbt_t *ls = hc_nbt_get(c, "liquid_settings");
            p->liquid_ignore =
                ls && strcmp(hc_nbt_str(ls), "ignore_waterlogging") == 0;
            const hc_nbt_t *pe = hc_nbt_get(c, "pool_element");
            const hc_nbt_t *et = hc_nbt_get(pe, "element_type");
            if (strcmp(hc_nbt_str(et), "minecraft:single_pool_element") != 0)
                die("non-single pool element", NULL);
            const char *proj = tag_str(pe, "projection");
            if (strcmp(proj, "rigid") == 0)
                p->proj_rigid = 1;
            else if (strcmp(proj, "terrain_matching") != 0)
                die("unknown pool projection", proj);
            const hc_nbt_t *procs = hc_nbt_get(pe, "processors");
            if (hc_nbt_tag(procs) == HC_NBT_STRING) {
                if (strcmp(hc_nbt_str(procs),
                           "minecraft:trial_chambers_copper_bulb_"
                           "degradation") != 0)
                    die("unknown processor list ref", hc_nbt_str(procs));
                p->procs = HC_PROCS_JIGSAW_COPPER;
            } else {
                /* 인라인 {processors: []} */
                const hc_nbt_t *lst = hc_nbt_get(procs, "processors");
                if (!lst || hc_nbt_list_count(lst) != 0)
                    die("inline processor list not empty", NULL);
                p->procs = HC_PROCS_JIGSAW_NONE;
            }
            p->tmpl = hc_template_load(sc->arena, sc->template_dir,
                                       tag_str(pe, "location"));
        } else
            die("unknown piece id", pid);
        for (int k = 0; k < 3; k++) {
            if (p->bb[k] < sbb[k])
                sbb[k] = p->bb[k];
            if (p->bb[k + 3] > sbb[k + 3])
                sbb[k + 3] = p->bb[k + 3];
        }
    }
    memcpy(st->bb, sbb, sizeof sbb);
    if (strcmp(name, "minecraft:trial_chambers") == 0)
        for (int k = 0; k < 3; k++) { /* terrain_adaptation → +12 인플레 */
            st->bb[k] -= 12;
            st->bb[k + 3] += 12;
        }
}

/* ================= 바이옴 태그 해석 (has_structure/mineshaft) ================= */

enum { MAX_TAG_BIOMES = 96 };

typedef struct {
    uint16_t ids[MAX_TAG_BIOMES];
    int32_t  n;
} biome_set_t;

static void tag_add(biome_set_t *s, uint16_t id) {
    for (int32_t i = 0; i < s->n; i++)
        if (s->ids[i] == id)
            return;
    if (s->n >= MAX_TAG_BIOMES)
        die("biome tag set overflow", NULL);
    s->ids[s->n++] = id;
}

static void resolve_biome_tag(hc_arena_t *a, const hc_biome_reg_t *reg,
                              const char *tags_dir, const char *tag_name,
                              biome_set_t *out, int depth) {
    if (depth > 4)
        die("biome tag recursion too deep", tag_name);
    char path[512];
    snprintf(path, sizeof path, "%s/%s.json", tags_dir, tag_name);
    size_t len = 0;
    char  *buf = read_whole(a, path, &len);
    if (!buf)
        die("cannot open biome tag", path);
    const char *err = NULL;
    size_t      pos = 0;
    hc_json_t  *j = hc_json_parse(buf, a, &err, &pos);
    if (!j)
        die("biome tag parse failed", path);
    const hc_json_t *vals = hc_json_get(j, "values");
    for (const hc_json_t *v = vals->child; v; v = v->next) {
        char nm[128];
        snprintf(nm, sizeof nm, "%.*s", (int)v->slen, v->s);
        if (nm[0] == '#') {
            /* "#minecraft:is_ocean" → is_ocean.json */
            const char *sub = strchr(nm, ':');
            resolve_biome_tag(a, reg, tags_dir, sub + 1, out, depth + 1);
        } else {
            int32_t id = hc_biome_find((hc_biome_reg_t *)reg, nm,
                                       (int32_t)strlen(nm));
            /* 밴드에 없는 바이옴 (사막 등) 은 무시 — 검사 대상 좌표에
             * 등장할 수 없다 */
            if (id >= 0)
                tag_add(out, (uint16_t)id);
        }
    }
}

static int biome_in(const biome_set_t *s, uint16_t id) {
    for (int32_t i = 0; i < s->n; i++)
        if (s->ids[i] == id)
            return 1;
    return 0;
}

/* ================= mineshaft 위치계산 ================= */

/* setLargeFeatureSeed (LegacyRandomSource — R-mineshaft §0) */
static void lcg_large_feature_seed(hc_lcg_t *r, int64_t seed, int32_t cx,
                                   int32_t cz) {
    hc_lcg_init(r, seed);
    int64_t a = hc_lcg_next_long(r);
    int64_t b = hc_lcg_next_long(r);
    hc_lcg_init(r, (int64_t)cx * a ^ (int64_t)cz * b ^ seed);
}

/* legacy_type_3 (C.2): nextDouble < (double)0.004F */
static int mineshaft_freq_pass(int64_t seed, int32_t cx, int32_t cz) {
    hc_lcg_t r;
    lcg_large_feature_seed(&r, seed, cx, cz);
    return hc_lcg_next_double(&r) < (double)0.004f;
}

/* ================= init ================= */

int hc_structures_init(hc_sctx_t *sc, hc_arena_t *a, int64_t seed,
                       const char *structures_dir, const char *template_dir,
                       const hc_json_t *copper_proc_json,
                       const hc_df_source_t *tags, int32_t n_tags,
                       const hc_biome_view_t *view,
                       const hc_biome_reg_t *biomes, const char **err) {
    (void)copper_proc_json;
    memset(sc, 0, sizeof *sc);
    sc->arena = a;
    sc->seed = seed;
    sc->template_dir = template_dir;
    sc->view = view;
    sc->biomes = biomes;
    if (hc_be_recorder_init(&sc->be, a, 4096) != 0) {
        *err = "arena exhausted (be recorder)";
        return -1;
    }
    /* features_cannot_replace 마스크 (블록 태그 테이블에서) */
    for (int32_t i = 0; i < n_tags; i++)
        if (strcmp(tags[i].name, "minecraft:features_cannot_replace") == 0) {
            const hc_json_t *vals = hc_json_get(tags[i].json, "values");
            for (const hc_json_t *v = vals->child; v; v = v->next)
                for (int32_t id = 0; id < HC_B_COUNT; id++) {
                    const char *nm = hc_block_name((uint16_t)id);
                    const char *br = strchr(nm, '[');
                    size_t      bl = br ? (size_t)(br - nm) : strlen(nm);
                    if ((size_t)v->slen == bl &&
                        strncmp(nm, v->s, bl) == 0)
                        sc->mask_features_cannot_replace[id >> 6] |=
                            1ull << (id & 63);
                }
        }

    /* --- 골든 starts 4건 --- */
    static const int32_t GS[4][2] = {{20, 1}, {9, 5}, {22, 6}, {16, 14}};
    for (int i = 0; i < 4; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/c.%d.%d.starts.nbt", structures_dir,
                 GS[i][0], GS[i][1]);
        size_t len = 0;
        char  *buf = read_whole(a, path, &len);
        if (!buf) {
            *err = "golden starts fragment missing (run "
                   "tools/golden/extract_structures.py)";
            return -1;
        }
        hc_nbt_t *root = hc_nbt_parse(a, (const uint8_t *)buf, len);
        if (!root) {
            *err = "starts fragment parse failed";
            return -1;
        }
        hc_sstart_t *st = &sc->starts[sc->n_starts++];
        parse_start_tag(sc, st, root, GS[i][0], GS[i][1]);
    }

    /* --- mineshaft 파생 (스캔 창 -12..43 — 헤더 주석) --- */
    biome_set_t ms_biomes;
    memset(&ms_biomes, 0, sizeof ms_biomes);
    {
        /* tags_dir: reference/tags/worldgen_biome (has_structure/ 하위) */
        char tags_dir[512];
        snprintf(tags_dir, sizeof tags_dir, "%s/../tags/worldgen_biome",
                 template_dir); /* reference/structure → reference/tags */
        resolve_biome_tag(a, biomes, tags_dir, "has_structure/mineshaft",
                          &ms_biomes, 0);
        if (ms_biomes.n < 5) {
            *err = "mineshaft biome tag resolution too small";
            return -1;
        }
    }
    int32_t ms_found = 0;
    for (int32_t scz = -12; scz <= 43; scz++)
        for (int32_t scx = -12; scx <= 43; scx++) {
            if (!mineshaft_freq_pass(seed, scx, scz))
                continue;
            /* 가중 선택 (C.3): [mineshaft, mineshaft_mesa] — mesa 는
             * badlands 전용이라 이 창에서 항상 biome 실패. 어느 쪽이
             * 먼저 뽑히든 normal 의 조립 RNG 는 독립 (fresh 인스턴스). */
            hc_spiece_t *pieces = NULL;
            int32_t      sx, sy, sz;
            int32_t np = hc_mineshaft_assemble(a, seed, scx, scz, &pieces,
                                               &sx, &sy, &sz);
            if (np <= 0)
                continue;
            /* stub 바이옴 (raw quart — C.4) */
            int32_t qx = sx >> 2, qy = sy >> 2, qz = sz >> 2;
            if (qy < view->qy0)
                qy = view->qy0;
            if (qy >= view->qy0 + view->ny)
                qy = view->qy0 + view->ny - 1;
            uint16_t b = hc_biome_view_get(view, qx * 4, qy * 4, qz * 4);
            if (!biome_in(&ms_biomes, b))
                continue;
            if (sc->n_starts >= HC_SCTX_MAX_STARTS)
                die("too many derived starts", NULL);
            hc_sstart_t *st = &sc->starts[sc->n_starts++];
            memset(st, 0, sizeof *st);
            st->name = "minecraft:mineshaft";
            st->scx = scx;
            st->scz = scz;
            st->step = 3;
            st->step_index = 1;
            st->tag = NULL;
            st->n_pieces = np;
            st->pieces = pieces;
            int32_t bb[6] = {INT32_MAX, INT32_MAX, INT32_MAX,
                             INT32_MIN, INT32_MIN, INT32_MIN};
            for (int32_t i = 0; i < np; i++)
                for (int k = 0; k < 3; k++) {
                    if (pieces[i].bb[k] < bb[k])
                        bb[k] = pieces[i].bb[k];
                    if (pieces[i].bb[k + 3] > bb[k + 3])
                        bb[k + 3] = pieces[i].bb[k + 3];
                }
            memcpy(st->bb, bb, sizeof bb);
            ms_found++;
        }
    fprintf(stderr, "hc_structures: %d mineshaft starts derived in scan "
                    "window (-12..43)\n",
            ms_found);

    /* --- 리전 밖 이웃 스타트 — 같은 캡처 런의 이웃 리전 .mca 에서
     * 추출한 골든 starts 프래그먼트 (tools/golden/
     * extract_neighbor_start.py — coherence 가드가 r.0.0 해시 대조).
     * 포함 기준 (실측, 완료 노트 참조): 데코 창 (-4..34) 청크에 피스가
     * 닿거나 r.0.0 에 beard/references 가 도달하는 비-mineshaft 스타트.
     *  - trial_chambers (13,35): 피스 union z=512..612 — r.0.0 블록
     *    배치는 없고, (a) cz=31 행 beardifier, (b) 마진 청크 (cz>=32)
     *    데코 배치 (라이트/엣지 전파가 r.0.0 도달), (c) References
     *    멤버십 c.7..14.31 (아래 교차검증 fail-loud).
     *  - shipwreck_beached (32,17): 피스 BB x=512.. — 마진 청크
     *    c.32.17/18 배치가 x=511 로 엣지 틱/스카이라이트 전파 (실측).
     *  - 제외 실측: trial (40,4) 피스 x>=560 (데코 창 밖, 지하 밀도만
     *    — 관측 불가), jungle_pyramid (5,42)/원거리 보물·난파선 등
     *    (데코 창 밖). --- */
    {
        static const int32_t NS[][2] = {{13, 35}, {32, 17}};
        for (size_t i = 0; i < sizeof NS / sizeof NS[0]; i++) {
            char path[512];
            snprintf(path, sizeof path, "%s/c.%d.%d.starts.nbt",
                     structures_dir, NS[i][0], NS[i][1]);
            size_t len = 0;
            char  *buf = read_whole(a, path, &len);
            if (!buf) {
                *err = "neighbor starts fragment missing (run "
                       "tools/golden/extract_neighbor_start.py)";
                return -1;
            }
            hc_nbt_t *root = hc_nbt_parse(a, (const uint8_t *)buf, len);
            if (!root) {
                *err = "neighbor starts fragment parse failed";
                return -1;
            }
            hc_sstart_t *st = &sc->starts[sc->n_starts++];
            parse_start_tag(sc, st, root, NS[i][0], NS[i][1]);
            st->tag = NULL; /* r.0.0 직렬화 재방출 대상 아님 (리전 밖) */
        }
    }

    /* --- References 교차검증 (골든 references.txt 전수) --- */
    {
        char path[512];
        snprintf(path, sizeof path, "%s/references.txt", structures_dir);
        size_t len = 0;
        char  *buf = read_whole(a, path, &len);
        if (!buf) {
            *err = "golden references.txt missing";
            return -1;
        }
        /* 골든: (cx,cz,name) 별 롱 목록 (파일 순서) */
        int64_t bad = 0;
        char   *save = buf;
        /* 우리 파생과 대조: 청크·구조물별로 파생 배열을 만들고 파일과
         * 순서까지 비교 */
        for (int32_t cz = 0; cz < 32; cz++)
            for (int32_t cx = 0; cx < 32; cx++) {
                const char    *names[8];
                const int64_t *arrays[8];
                int32_t        lens[8];
                hc_arena_t     scratch = *a; /* 지역 스냅샷 (버림) */
                int32_t        nr = hc_structures_references(
                    sc, &scratch, cx, cz, names, arrays, lens, 8);
                /* 골든 라인 수집 */
                for (int32_t s = 0; s < nr; s++) {
                    for (int32_t k = 0; k < lens[s]; k++) {
                        char want[160];
                        snprintf(want, sizeof want, "%d %d %s %016llx", cx,
                                 cz, names[s],
                                 (unsigned long long)arrays[s][k]);
                        if (!strstr(save, want)) {
                            if (bad < 12)
                                fprintf(stderr,
                                        "REFS derived-not-in-golden: %s\n",
                                        want);
                            bad++;
                        }
                    }
                }
            }
        /* 역방향: 골든 라인마다 파생에 존재+순서 검증 */
        char *p = buf;
        int32_t cur_cx = -1, cur_cz = -1;
        const char *cur_name = NULL;
        int32_t     cur_k = 0;
        while (*p) {
            char *nl = strchr(p, '\n');
            if (!nl)
                break;
            *nl = '\0';
            if (p[0] != '#' && p[0]) {
                int32_t            gcx, gcz;
                char               nm[80];
                unsigned long long hex;
                if (sscanf(p, "%d %d %79s %llx", &gcx, &gcz, nm, &hex) != 4)
                    die("bad references.txt line", p);
                if (gcx != cur_cx || gcz != cur_cz || !cur_name ||
                    strcmp(cur_name, nm) != 0) {
                    cur_cx = gcx;
                    cur_cz = gcz;
                    cur_k = 0;
                }
                const char    *names[8];
                const int64_t *arrays[8];
                int32_t        lens[8];
                hc_arena_t     scratch = *a;
                int32_t        nr = hc_structures_references(
                    sc, &scratch, gcx, gcz, names, arrays, lens, 8);
                int found = 0;
                for (int32_t s = 0; s < nr; s++)
                    if (strcmp(names[s], nm) == 0) {
                        found = cur_k < lens[s] &&
                                (unsigned long long)arrays[s][cur_k] == hex;
                        cur_name = names[s];
                    }
                if (!found) {
                    if (bad < 12)
                        fprintf(stderr, "REFS golden-not-derived: %s\n", p);
                    bad++;
                }
                cur_k++;
            }
            p = nl + 1;
        }
        if (bad) {
            *err = "derived references diverge from golden";
            return -1;
        }
    }
    return 0;
}

/* ================= References 파생 ================= */

int32_t hc_structures_references(const hc_sctx_t *sc, hc_arena_t *scratch,
                                 int32_t cx, int32_t cz, const char **names,
                                 const int64_t **arrays, int32_t *lens,
                                 int32_t cap) {
    (void)scratch;
    /* 구조물별 삽입 리스트 (스캔 순서: sx 외측, sz 내측 — C.6) */
    enum { MAX_PER = 24 };
    struct {
        const char *name;
        int64_t     added[MAX_PER];
        int32_t     n;
    } acc[8];
    int32_t n_acc = 0;
    int32_t bx0 = cx * 16, bz0 = cz * 16, bx1 = bx0 + 15, bz1 = bz0 + 15;
    for (int32_t sx = cx - 8; sx <= cx + 8; sx++)
        for (int32_t sz = cz - 8; sz <= cz + 8; sz++)
            for (int32_t i = 0; i < sc->n_starts; i++) {
                const hc_sstart_t *st = &sc->starts[i];
                if (st->scx != sx || st->scz != sz)
                    continue;
                /* 2D XZ intersects (BoundingBox.java:135-137) */
                if (st->bb[3] < bx0 || st->bb[0] > bx1 || st->bb[5] < bz0 ||
                    st->bb[2] > bz1)
                    continue;
                int32_t k = -1;
                for (int32_t j = 0; j < n_acc; j++)
                    if (strcmp(acc[j].name, st->name) == 0)
                        k = j;
                if (k < 0) {
                    if (n_acc >= 8)
                        die("too many structures per chunk", NULL);
                    k = n_acc++;
                    acc[k].name = st->name;
                    acc[k].n = 0;
                }
                if (acc[k].n >= MAX_PER)
                    die("too many references per structure", NULL);
                acc[k].added[acc[k].n++] =
                    (int64_t)((uint64_t)(uint32_t)sx |
                              ((uint64_t)(uint32_t)sz << 32));
            }
    /* 방출: 구조물 키 순서는 소스 맵 (HashMap<Structure identity>) —
     * 컴파운드 키 방출은 nbt.c 의 HashMap(String) 에뮬이 처리하므로
     * 여기서는 put 순서만 결정하면 된다. 우리 청크는 전부 단일 구조물
     * (골든 실측) — 복수면 die (identity-order 재현 불가). */
    if (n_acc > 1)
        die("multi-structure references in one chunk — identity order "
            "unpinned",
            NULL);
    int32_t out = 0;
    for (int32_t j = 0; j < n_acc && out < cap; j++, out++) {
        names[out] = acc[j].name;
        int64_t *arr = hc_arena_alloc(sc->arena, sizeof(int64_t) * MAX_PER,
                                      _Alignof(int64_t));
        int32_t  m = hc_longset_to_array(acc[j].added, acc[j].n, arr);
        arrays[out] = arr;
        lens[out] = m;
    }
    return out;
}

/* ================= Beardifier 입력 수집 ================= */

/* terrain_adaptation — 26.2 data/minecraft/worldgen/structure JSON 실측:
 * trial_chambers 만 "encapsulate", mineshaft/shipwreck_beached/
 * ocean_ruin_warm/ruined_portal_ocean 은 필드 부재 (코덱 기본 none). */
static uint8_t terrain_adaptation_of(const char *name) {
    if (strcmp(name, "minecraft:trial_chambers") == 0)
        return HC_TA_ENCAPSULATE;
    return HC_TA_NONE;
}

/* StructurePiece.isCloseToChunk(chunk, 12) — XZ 교차 (12 인플레이트) */
static int piece_close_to_chunk(const int32_t bb[6], int32_t bx0,
                                int32_t bz0) {
    return bb[0] <= bx0 + 15 + 12 && bb[3] >= bx0 - 12 &&
           bb[2] <= bz0 + 15 + 12 && bb[5] >= bz0 - 12;
}

int hc_structures_beard(const hc_sctx_t *sc, hc_arena_t *arena, int32_t cx,
                        int32_t cz, hc_beard_t *out) {
    memset(out, 0, sizeof *out);
    int32_t bx0 = cx * 16, bz0 = cz * 16;
    /* startsForStructure(chunk, ta != NONE): References 멤버십과 동일한
     * 스캔 + LongOpenHashSet 순회 순서 (hc_structures_step 과 같은 산식).
     * 이 리전은 청크당 trial 스타트가 최대 1개지만 순서 규약은 유지. */
    hc_sstart_t *hits[24];
    int64_t      added[24];
    int32_t      n_hits = 0;
    for (int32_t sx = cx - 8; sx <= cx + 8; sx++)
        for (int32_t sz = cz - 8; sz <= cz + 8; sz++)
            for (int32_t i = 0; i < sc->n_starts; i++) {
                const hc_sstart_t *st = &sc->starts[i];
                if (st->scx != sx || st->scz != sz)
                    continue;
                if (terrain_adaptation_of(st->name) == HC_TA_NONE)
                    continue;
                if (st->bb[3] < bx0 || st->bb[0] > bx0 + 15 ||
                    st->bb[5] < bz0 || st->bb[2] > bz0 + 15)
                    continue;
                assert(n_hits < 24);
                hits[n_hits] = (hc_sstart_t *)st;
                added[n_hits] = (int64_t)((uint64_t)(uint32_t)sx |
                                          ((uint64_t)(uint32_t)sz << 32));
                n_hits++;
            }
    if (n_hits == 0)
        return 0;
    int64_t ordered[24];
    int32_t m = hc_longset_to_array(added, n_hits, ordered);

    /* 1패스: 개수 (rigid = 피스 필터 통과, junction = 창 필터 통과) */
    enum { R_CAP = 512, J_CAP = 2048 };
    static _Thread_local hc_beard_rigid_t    rig[R_CAP];
    static _Thread_local hc_beard_junction_t jun[J_CAP];
    int32_t n_r = 0, n_j = 0;
    int32_t u[6];   /* anyPieceBoundingBox (encapsulating union) */
    int     has_u = 0;
    for (int32_t k = 0; k < m; k++) {
        hc_sstart_t *st = NULL;
        for (int32_t i = 0; i < n_hits; i++)
            if (added[i] == ordered[k])
                st = hits[i];
        assert(st);
        uint8_t ta = terrain_adaptation_of(st->name);
        for (int32_t i = 0; i < st->n_pieces; i++) {
            const hc_spiece_t *p = &st->pieces[i];
            if (!piece_close_to_chunk(p->bb, bx0, bz0))
                continue;
            if (p->kind == HC_SP_JIGSAW) {
                if (p->proj_rigid) {
                    if (n_r >= R_CAP)
                        die("beard rigid overflow", NULL);
                    memcpy(rig[n_r].bb, p->bb, sizeof p->bb);
                    rig[n_r].gld = p->gld;
                    rig[n_r].adj = ta;
                    n_r++;
                    for (int c3 = 0; c3 < 3; c3++) {
                        if (!has_u || p->bb[c3] < u[c3])
                            u[c3] = p->bb[c3];
                        if (!has_u || p->bb[c3 + 3] > u[c3 + 3])
                            u[c3 + 3] = p->bb[c3 + 3];
                    }
                    has_u = 1;
                }
                for (int32_t j = 0; j < p->n_junctions; j++) {
                    const hc_beard_junction_t *jj = &p->junctions[j];
                    if (jj->sx > bx0 - 12 && jj->sz > bz0 - 12 &&
                        jj->sx < bx0 + 15 + 12 && jj->sz < bz0 + 15 + 12) {
                        if (n_j >= J_CAP)
                            die("beard junction overflow", NULL);
                        jun[n_j++] = *jj;
                        int32_t jb[6] = {jj->sx, jj->sgy, jj->sz,
                                         jj->sx, jj->sgy, jj->sz};
                        for (int c3 = 0; c3 < 3; c3++) {
                            if (!has_u || jb[c3] < u[c3])
                                u[c3] = jb[c3];
                            if (!has_u || jb[c3 + 3] > u[c3 + 3])
                                u[c3 + 3] = jb[c3 + 3];
                        }
                        has_u = 1;
                    }
                }
            } else {
                /* 비-풀 피스 (이 리전의 trial 은 전부 jigsaw — 도달 시
                 * 규약대로 rigid(gld=0) 처리) */
                if (n_r >= R_CAP)
                    die("beard rigid overflow", NULL);
                memcpy(rig[n_r].bb, p->bb, sizeof p->bb);
                rig[n_r].gld = 0;
                rig[n_r].adj = ta;
                n_r++;
                for (int c3 = 0; c3 < 3; c3++) {
                    if (!has_u || p->bb[c3] < u[c3])
                        u[c3] = p->bb[c3];
                    if (!has_u || p->bb[c3 + 3] > u[c3 + 3])
                        u[c3 + 3] = p->bb[c3 + 3];
                }
                has_u = 1;
            }
        }
    }
    if (!has_u)
        return 0; /* Beardifier.EMPTY */
    out->has_any = 1;
    for (int c3 = 0; c3 < 3; c3++) { /* inflatedBy(24) */
        out->affected[c3] = u[c3] - 24;
        out->affected[c3 + 3] = u[c3 + 3] + 24;
    }
    out->n_rigids = n_r;
    out->n_junctions = n_j;
    if (n_r) {
        hc_beard_rigid_t *pr = hc_arena_alloc(
            arena, sizeof(hc_beard_rigid_t) * (size_t)n_r,
            _Alignof(hc_beard_rigid_t));
        if (!pr)
            die("arena exhausted (beard rigids)", NULL);
        memcpy(pr, rig, sizeof(hc_beard_rigid_t) * (size_t)n_r);
        out->rigids = pr;
    }
    if (n_j) {
        hc_beard_junction_t *pj = hc_arena_alloc(
            arena, sizeof(hc_beard_junction_t) * (size_t)n_j,
            _Alignof(hc_beard_junction_t));
        if (!pj)
            die("arena exhausted (beard junctions)", NULL);
        memcpy(pj, jun, sizeof(hc_beard_junction_t) * (size_t)n_j);
        out->junctions = pj;
    }
    return 1;
}

/* ================= 스텝 디스패치 ================= */

const hc_nbt_t *hc_structures_starts_tag(const hc_sctx_t *sc, int32_t cx,
                                         int32_t cz) {
    for (int32_t i = 0; i < sc->n_starts; i++)
        if (sc->starts[i].tag && sc->starts[i].scx == cx &&
            sc->starts[i].scz == cz)
            return sc->starts[i].tag;
    return NULL;
}

void hc_structures_step(hc_sctx_t *sc, hc_feat_region_t *rg,
                        const hc_feat_reg_t *freg, int32_t sea,
                        int32_t cx, int32_t cz, int64_t deco_seed,
                        int32_t step) {
    /* 이 청크의 references 에 있는 스타트들만, 스텝 내 순번 순.
     * 같은 순번의 스타트 여러 개 (mineshaft) 는 references LongSet 순회
     * 순서 (hc_structures_references 산출 순서와 동일 산식). */
    int32_t bx0 = cx * 16, bz0 = cz * 16, bx1 = bx0 + 15, bz1 = bz0 + 15;
    /* chunkBB (getWritableArea): y [minY+1, maxY] */
    for (int idx = 0; idx <= 25; idx++) {
        /* 이 (step, idx) 조합의 스타트 수집 (refs 산식과 같은 스캔) */
        hc_sstart_t *hits[24];
        int32_t      n_hits = 0;
        int64_t      added[24];
        for (int32_t sx = cx - 8; sx <= cx + 8; sx++)
            for (int32_t sz = cz - 8; sz <= cz + 8; sz++)
                for (int32_t i = 0; i < sc->n_starts; i++) {
                    hc_sstart_t *st = &sc->starts[i];
                    if (st->scx != sx || st->scz != sz ||
                        st->step != step || st->step_index != idx)
                        continue;
                    if (st->bb[3] < bx0 || st->bb[0] > bx1 ||
                        st->bb[5] < bz0 || st->bb[2] > bz1)
                        continue;
                    assert(n_hits < 24);
                    hits[n_hits] = st;
                    added[n_hits] =
                        (int64_t)((uint64_t)(uint32_t)sx |
                                  ((uint64_t)(uint32_t)sz << 32));
                    n_hits++;
                }
        if (n_hits == 0)
            continue;
        /* LongOpenHashSet 순회 순서로 재배열 */
        int64_t ordered[24];
        int32_t m = hc_longset_to_array(added, n_hits, ordered);
        hc_wgr_t rng = {0}; /* 가우시안 캐시 위생 — 구조물 경로는 미사용 */
        hc_wgr_set_feature_seed(&rng, deco_seed, idx, step);
        for (int32_t k = 0; k < m; k++) {
            hc_sstart_t *st = NULL;
            for (int32_t i = 0; i < n_hits; i++)
                if (added[i] == ordered[k])
                    st = hits[i];
            assert(st);
            /* placeInChunk: 피스 순서, 피스 bb ∩ chunkBB(3D) */
            for (int32_t i = 0; i < st->n_pieces; i++) {
                hc_spiece_t *p = &st->pieces[i];
                if (p->bb[3] < bx0 || p->bb[0] > bx1 || p->bb[5] < bz0 ||
                    p->bb[2] > bz1)
                    continue;
                if (p->bb[4] < HC_MIN_Y + 1 || p->bb[1] > HC_MAX_Y)
                    continue;
                if (p->kind >= HC_SP_MS_ROOM)
                    hc_splace_mineshaft(sc, rg, st, p, &rng, cx, cz);
                else
                    hc_splace_template(sc, rg, freg, sea, st, p, &rng, cx,
                                       cz);
            }
        }
    }
}

/* ================= 직렬화: 청크 BE 목록 ================= */

int32_t hc_structures_chunk_bes(const hc_sctx_t *sc, int32_t cx, int32_t cz,
                                const hc_be_rec_t **recs_out, int32_t cap) {
    /* 실체화 순서 재구성 (R-serialization §4.4/4.5):
     *  - live (TEMPLATE/CHEST_LOOT/SPAWNER): 기록 순서 = 실체화 순서 →
     *    proto live 맵 (fastutil) 에 그 순서로 put.
     *  - DUMMY: postProcessGeneration 이 pending HashMap<BlockPos> 순회
     *    순서로 승격 → live 맵 꼬리에 그 순서로 put.
     *  - 저장 리스트 = fresh HashSet<BlockPos>(live 키 삽입 = live 맵
     *    fastutil 순회 순서) 의 순회 (jset).
     * live 맵 자체도 fastutil — LevelChunk 생성자가 proto 순회 순서로
     * 복사하지만, 동일 키 집합을 같은 순서로 재삽입하면 순회가 보존된다
     * (오픈 어드레싱 전방 프로브 — 슬롯 충돌쌍의 상대 순서 유지). */
    enum { MAXB = 64 };
    int32_t idx_live[MAXB], n_live = 0;
    int32_t idx_dummy[MAXB], n_dummy = 0;
    for (int32_t i = 0; i < sc->be.n; i++) {
        const hc_be_rec_t *r = &sc->be.recs[i];
        if (r->dead || (r->x >> 4) != cx || (r->z >> 4) != cz)
            continue;
        if (r->kind == HC_BE_DUMMY) {
            assert(n_dummy < MAXB);
            idx_dummy[n_dummy++] = i;
        } else {
            assert(n_live < MAXB);
            idx_live[n_live++] = i;
        }
    }
    if (n_live + n_dummy == 0)
        return 0;
    /* pending HashMap<BlockPos> 순회 (DUMMY 승격 순서) — jset 에뮬 */
    int32_t dummy_sorted[MAXB];
    {
        hc_jset_t *js = malloc(sizeof *js);
        if (!js)
            die("oom (jset)", NULL);
        hc_jset_init(js);
        for (int32_t i = 0; i < n_dummy; i++) {
            const hc_be_rec_t *r = &sc->be.recs[idx_dummy[i]];
            hc_jset_add(js, r->x, r->y, r->z);
        }
        hc_jit_t it;
        int32_t  m = 0;
        for (hc_jit_begin(&it, js); hc_jit_valid(&it); hc_jit_next(&it)) {
            int32_t x, y, z;
            hc_jit_pos(&it, &x, &y, &z);
            for (int32_t i = 0; i < n_dummy; i++) {
                const hc_be_rec_t *r = &sc->be.recs[idx_dummy[i]];
                if (r->x == x && r->y == y && r->z == z)
                    dummy_sorted[m++] = idx_dummy[i];
            }
        }
        assert(m == n_dummy);
        free(js);
    }
    /* live 맵 put 순서: live (기록순) + dummy (승격순) → fastutil 순회 */
    int32_t all[MAXB * 2];
    int32_t n_all = 0;
    for (int32_t i = 0; i < n_live; i++)
        all[n_all++] = idx_live[i];
    for (int32_t i = 0; i < n_dummy; i++)
        all[n_all++] = dummy_sorted[i];
    int32_t pos[MAXB * 2][3];
    for (int32_t i = 0; i < n_all; i++) {
        const hc_be_rec_t *r = &sc->be.recs[all[i]];
        pos[i][0] = r->x;
        pos[i][1] = r->y;
        pos[i][2] = r->z;
    }
    int32_t fu_order[MAXB * 2];
    int32_t n_fu = hc_o2omap_key_order(pos, n_all, fu_order);
    assert(n_fu == n_all);
    /* 저장 순서: fresh HashSet<BlockPos> 에 fastutil 순회 순서로 삽입 후
     * 순회 (jset) */
    hc_jset_t *js = malloc(sizeof *js);
    if (!js)
        die("oom (jset2)", NULL);
    hc_jset_init(js);
    for (int32_t i = 0; i < n_fu; i++)
        hc_jset_add(js, pos[fu_order[i]][0], pos[fu_order[i]][1],
                    pos[fu_order[i]][2]);
    hc_jit_t it;
    int32_t  out = 0;
    for (hc_jit_begin(&it, js); hc_jit_valid(&it) && out < cap;
         hc_jit_next(&it)) {
        int32_t x, y, z;
        hc_jit_pos(&it, &x, &y, &z);
        for (int32_t i = 0; i < n_all; i++) {
            const hc_be_rec_t *r = &sc->be.recs[all[i]];
            if (r->x == x && r->y == y && r->z == z)
                recs_out[out++] = r;
        }
    }
    free(js);
    return out;
}
