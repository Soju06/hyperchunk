#include "hc_nbt.h"

#include <assert.h>
#include <string.h>

/* 컴파운드 엔트리 — 삽입 순서 연결 리스트. 방출 순서는 write 시점에
 * hc_nbt_java_map_order 로 계산한다 (hc_nbt.h 헤더 주석). */
typedef struct hc_nbt_ent {
    const char        *key;
    hc_nbt_t          *val;
    struct hc_nbt_ent *next;
} hc_nbt_ent_t;

typedef struct hc_nbt_item {
    hc_nbt_t           *val;
    struct hc_nbt_item *next;
} hc_nbt_item_t;

struct hc_nbt {
    uint8_t tag;
    uint8_t etag; /* 리스트 전용: 엘리먼트 태그 (빈 리스트 = End 0) */
    int32_t n;    /* 배열 길이 / 리스트·컴파운드 자식 수 */
    union {
        int64_t        i;
        const char    *s;
        const uint8_t *ba;
        const int64_t *la;
        struct {
            hc_nbt_ent_t *head, *tail;
        } comp;
        struct {
            hc_nbt_item_t *head, *tail;
        } list;
    } v;
};

/* 컴파운드 하나의 최대 키 수. 청크 스키마 최대는 루트 15키다. */
#define HC_NBT_MAX_KEYS 64

/* ------------------------------------------------------------------ */
/* Java HashMap 순회 순서                                              */
/* ------------------------------------------------------------------ */

/* java.lang.String.hashCode — 31진 다항, int32 랩어라운드 */
static uint32_t java_str_hash(const char *s) {
    uint32_t h = 0;
    for (; *s; s++)
        h = h * 31u + (uint8_t)*s;
    return h;
}

void hc_nbt_java_map_order(const char *const *keys, int n, uint8_t *perm) {
    assert(n >= 0 && n <= HC_NBT_MAX_KEYS);
    /* 최종 캐퍼시티: 기본 16, size > (int)(cap*0.75) 가 되는 put 에서
     * 2배 리사이즈 — n 키 삽입 후 cap 은 n <= cap*3/4 인 최소 2^k(>=16). */
    uint32_t cap = 16;
    while ((uint32_t)n > cap / 4u * 3u)
        cap <<= 1;
    /* 버킷 오름차순 + 버킷 내 삽입 순서 = 안정 카운팅 소트 */
    uint32_t bucket[HC_NBT_MAX_KEYS];
    for (int i = 0; i < n; i++) {
        uint32_t h = java_str_hash(keys[i]);
        h ^= h >> 16; /* HashMap.hash() spread */
        bucket[i] = h & (cap - 1u);
    }
    int out = 0;
    for (uint32_t b = 0; b < cap; b++) {
        int chain = 0;
        for (int i = 0; i < n; i++) {
            if (bucket[i] == b) {
                perm[out++] = (uint8_t)i;
                chain++;
            }
        }
        /* 트리화(빈 8개 & cap>=64) 는 이 스키마에서 도달 불가 */
        assert(chain < 8);
    }
    assert(out == n);
}

/* ------------------------------------------------------------------ */
/* 노드 생성                                                           */
/* ------------------------------------------------------------------ */

static hc_nbt_t *node(hc_arena_t *a, uint8_t tag) {
    hc_nbt_t *t = hc_arena_alloc(a, sizeof *t, _Alignof(hc_nbt_t));
    if (!t)
        return NULL;
    memset(t, 0, sizeof *t);
    t->tag = tag;
    return t;
}

hc_nbt_t *hc_nbt_byte(hc_arena_t *a, int32_t v) {
    hc_nbt_t *t = node(a, HC_NBT_BYTE);
    if (t)
        t->v.i = (int8_t)v;
    return t;
}

hc_nbt_t *hc_nbt_short(hc_arena_t *a, int32_t v) {
    hc_nbt_t *t = node(a, HC_NBT_SHORT);
    if (t)
        t->v.i = (int16_t)v;
    return t;
}

hc_nbt_t *hc_nbt_int(hc_arena_t *a, int32_t v) {
    hc_nbt_t *t = node(a, HC_NBT_INT);
    if (t)
        t->v.i = v;
    return t;
}

hc_nbt_t *hc_nbt_long(hc_arena_t *a, int64_t v) {
    hc_nbt_t *t = node(a, HC_NBT_LONG);
    if (t)
        t->v.i = v;
    return t;
}

hc_nbt_t *hc_nbt_string(hc_arena_t *a, const char *s) {
    hc_nbt_t *t = node(a, HC_NBT_STRING);
    if (t)
        t->v.s = s;
    return t;
}

hc_nbt_t *hc_nbt_string_n(hc_arena_t *a, const char *s, size_t n) {
    char *copy = hc_arena_alloc(a, n + 1, 1);
    if (!copy)
        return NULL;
    memcpy(copy, s, n);
    copy[n] = '\0';
    return hc_nbt_string(a, copy);
}

hc_nbt_t *hc_nbt_byte_array(hc_arena_t *a, const uint8_t *d, int32_t n) {
    assert(n >= 0);
    hc_nbt_t *t = node(a, HC_NBT_BYTE_ARRAY);
    if (t) {
        t->v.ba = d;
        t->n = n;
    }
    return t;
}

hc_nbt_t *hc_nbt_long_array(hc_arena_t *a, const int64_t *d, int32_t n) {
    assert(n >= 0);
    hc_nbt_t *t = node(a, HC_NBT_LONG_ARRAY);
    if (t) {
        t->v.la = d;
        t->n = n;
    }
    return t;
}

hc_nbt_t *hc_nbt_compound(hc_arena_t *a) {
    return node(a, HC_NBT_COMPOUND);
}

hc_nbt_t *hc_nbt_list(hc_arena_t *a) {
    return node(a, HC_NBT_LIST); /* etag = 0 (End) — 빈 리스트 규약 */
}

int hc_nbt_put(hc_arena_t *a, hc_nbt_t *comp, const char *key, hc_nbt_t *v) {
    assert(comp && comp->tag == HC_NBT_COMPOUND);
    if (!v)
        return -1;
#ifndef NDEBUG
    for (const hc_nbt_ent_t *e = comp->v.comp.head; e; e = e->next)
        assert(strcmp(e->key, key) != 0); /* 중복 키 금지 */
#endif
    hc_nbt_ent_t *e = hc_arena_alloc(a, sizeof *e, _Alignof(hc_nbt_ent_t));
    if (!e)
        return -1;
    e->key = key;
    e->val = v;
    e->next = NULL;
    if (comp->v.comp.tail)
        comp->v.comp.tail->next = e;
    else
        comp->v.comp.head = e;
    comp->v.comp.tail = e;
    comp->n++;
    assert(comp->n <= HC_NBT_MAX_KEYS);
    return 0;
}

int hc_nbt_add(hc_arena_t *a, hc_nbt_t *list, hc_nbt_t *v) {
    assert(list && list->tag == HC_NBT_LIST);
    if (!v)
        return -1;
    assert(list->etag == HC_NBT_END || list->etag == v->tag);
    list->etag = v->tag;
    hc_nbt_item_t *it = hc_arena_alloc(a, sizeof *it, _Alignof(hc_nbt_item_t));
    if (!it)
        return -1;
    it->val = v;
    it->next = NULL;
    if (list->v.list.tail)
        list->v.list.tail->next = it;
    else
        list->v.list.head = it;
    list->v.list.tail = it;
    list->n++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 직렬화                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *p;
    uint8_t *end;
    int      overflow;
} wr_t;

static void put_bytes(wr_t *w, const void *d, size_t n) {
    if (w->overflow || (size_t)(w->end - w->p) < n) {
        w->overflow = 1;
        return;
    }
    memcpy(w->p, d, n);
    w->p += n;
}

static void put_u8(wr_t *w, uint8_t v) {
    put_bytes(w, &v, 1);
}

static void put_u16(wr_t *w, uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    put_bytes(w, b, 2);
}

static void put_i32(wr_t *w, int32_t v) {
    uint32_t u = (uint32_t)v;
    uint8_t  b[4] = {(uint8_t)(u >> 24), (uint8_t)(u >> 16), (uint8_t)(u >> 8),
                     (uint8_t)u};
    put_bytes(w, b, 4);
}

static void put_i64(wr_t *w, int64_t v) {
    uint64_t u = (uint64_t)v;
    uint8_t  b[8];
    for (int i = 0; i < 8; i++)
        b[i] = (uint8_t)(u >> (56 - 8 * i));
    put_bytes(w, b, 8);
}

/* DataOutput.writeUTF — modified UTF-8. ASCII 전제 (헤더 주석). */
static void put_utf(wr_t *w, const char *s) {
    size_t n = strlen(s);
    assert(n <= 0xFFFF);
#ifndef NDEBUG
    for (size_t i = 0; i < n; i++)
        assert((uint8_t)s[i] >= 0x01 && (uint8_t)s[i] <= 0x7F);
#endif
    put_u16(w, (uint16_t)n);
    put_bytes(w, s, n);
}

static void write_payload(wr_t *w, const hc_nbt_t *t);

static void write_compound_payload(wr_t *w, const hc_nbt_t *t) {
    const char     *keys[HC_NBT_MAX_KEYS] = {0};
    const hc_nbt_t *vals[HC_NBT_MAX_KEYS] = {0};
    uint8_t         perm[HC_NBT_MAX_KEYS];
    int           n = 0;
    for (const hc_nbt_ent_t *e = t->v.comp.head; e; e = e->next) {
        keys[n] = e->key;
        vals[n] = e->val;
        n++;
    }
    assert(n == t->n);
    hc_nbt_java_map_order(keys, n, perm);
    for (int i = 0; i < n; i++) {
        const hc_nbt_t *v = vals[perm[i]];
        put_u8(w, v->tag);
        put_utf(w, keys[perm[i]]);
        write_payload(w, v);
    }
    put_u8(w, HC_NBT_END);
}

static void write_payload(wr_t *w, const hc_nbt_t *t) {
    switch (t->tag) {
    case HC_NBT_BYTE:
        put_u8(w, (uint8_t)t->v.i);
        break;
    case HC_NBT_SHORT:
        put_u16(w, (uint16_t)t->v.i);
        break;
    case HC_NBT_INT:
        put_i32(w, (int32_t)t->v.i);
        break;
    case HC_NBT_LONG:
        put_i64(w, t->v.i);
        break;
    case HC_NBT_BYTE_ARRAY:
        put_i32(w, t->n);
        put_bytes(w, t->v.ba, (size_t)t->n);
        break;
    case HC_NBT_STRING:
        put_utf(w, t->v.s);
        break;
    case HC_NBT_LIST:
        put_u8(w, t->etag);
        put_i32(w, t->n);
        for (const hc_nbt_item_t *it = t->v.list.head; it; it = it->next)
            write_payload(w, it->val);
        break;
    case HC_NBT_COMPOUND:
        write_compound_payload(w, t);
        break;
    case HC_NBT_LONG_ARRAY:
        put_i32(w, t->n);
        for (int32_t i = 0; i < t->n; i++)
            put_i64(w, t->v.la[i]);
        break;
    default:
        assert(0 && "unsupported NBT tag");
    }
}

ptrdiff_t hc_nbt_write(const hc_nbt_t *root, uint8_t *out, size_t cap) {
    assert(root && root->tag == HC_NBT_COMPOUND);
    wr_t w = {out, out + cap, 0};
    /* 무명 루트: id + 빈 이름 + 컴파운드 페이로드 */
    put_u8(&w, HC_NBT_COMPOUND);
    put_u16(&w, 0);
    write_compound_payload(&w, root);
    return w.overflow ? -1 : (ptrdiff_t)(w.p - out);
}
