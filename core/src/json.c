#include "hc_json.h"

#include <stdlib.h>
#include <string.h>

/* 재귀 하강 JSON 파서. worldgen JSON 은 중첩이 얕지만 (스플라인 ~10단)
 * 악의적 입력 방어로 깊이 상한을 둔다. */
#define HC_JSON_MAX_DEPTH 64

typedef struct {
    const char *buf;
    const char *p;
    hc_arena_t *arena;
    const char *err;
    int         depth;
} parser_t;

static void set_err(parser_t *ps, const char *msg) {
    if (!ps->err)
        ps->err = msg;
}

static void skip_ws(parser_t *ps) {
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' ||
           *ps->p == '\r')
        ps->p++;
}

static hc_json_t *new_node(parser_t *ps, hc_json_kind_t kind) {
    hc_json_t *n =
        (hc_json_t *)hc_arena_alloc(ps->arena, sizeof(hc_json_t), 16);
    if (!n) {
        set_err(ps, "arena exhausted");
        return NULL;
    }
    memset(n, 0, sizeof *n);
    n->kind = (uint8_t)kind;
    return n;
}

static hc_json_t *parse_value(parser_t *ps);

/* "..." — 이스케이프 없으면 zero-copy, 있으면 arena 로 복사.
 * \uXXXX 는 BMP 만 UTF-8 인코딩한다 (worldgen JSON 에는 사실상 없음). */
static int parse_string_raw(parser_t *ps, const char **out, int32_t *out_len) {
    if (*ps->p != '"') {
        set_err(ps, "expected string");
        return -1;
    }
    ps->p++;
    const char *start = ps->p;
    int has_escape = 0;
    while (*ps->p && *ps->p != '"') {
        if (*ps->p == '\\') {
            has_escape = 1;
            ps->p++;
            if (!*ps->p)
                break;
        }
        ps->p++;
    }
    if (*ps->p != '"') {
        set_err(ps, "unterminated string");
        return -1;
    }
    const char *end = ps->p;
    ps->p++; /* 닫는 따옴표 */

    if (!has_escape) {
        *out = start;
        *out_len = (int32_t)(end - start);
        return 0;
    }

    char *dst =
        (char *)hc_arena_alloc(ps->arena, (size_t)(end - start) + 1, 1);
    if (!dst) {
        set_err(ps, "arena exhausted");
        return -1;
    }
    char *w = dst;
    for (const char *r = start; r < end;) {
        if (*r != '\\') {
            *w++ = *r++;
            continue;
        }
        r++;
        switch (*r) {
        case '"': *w++ = '"'; r++; break;
        case '\\': *w++ = '\\'; r++; break;
        case '/': *w++ = '/'; r++; break;
        case 'b': *w++ = '\b'; r++; break;
        case 'f': *w++ = '\f'; r++; break;
        case 'n': *w++ = '\n'; r++; break;
        case 'r': *w++ = '\r'; r++; break;
        case 't': *w++ = '\t'; r++; break;
        case 'u': {
            r++;
            if (end - r < 4) {
                set_err(ps, "bad \\u escape");
                return -1;
            }
            uint32_t cp = 0;
            for (int i = 0; i < 4; i++) {
                char c = r[i];
                uint32_t d;
                if (c >= '0' && c <= '9')
                    d = (uint32_t)(c - '0');
                else if (c >= 'a' && c <= 'f')
                    d = (uint32_t)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')
                    d = (uint32_t)(c - 'A' + 10);
                else {
                    set_err(ps, "bad \\u escape");
                    return -1;
                }
                cp = (cp << 4) | d;
            }
            r += 4;
            if (cp < 0x80) {
                *w++ = (char)cp;
            } else if (cp < 0x800) {
                *w++ = (char)(0xC0 | (cp >> 6));
                *w++ = (char)(0x80 | (cp & 0x3F));
            } else {
                *w++ = (char)(0xE0 | (cp >> 12));
                *w++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                *w++ = (char)(0x80 | (cp & 0x3F));
            }
            break;
        }
        default:
            set_err(ps, "bad escape");
            return -1;
        }
    }
    *out = dst;
    *out_len = (int32_t)(w - dst);
    return 0;
}

static hc_json_t *parse_object(parser_t *ps) {
    hc_json_t *obj = new_node(ps, HC_JSON_OBJ);
    if (!obj)
        return NULL;
    ps->p++; /* '{' */
    skip_ws(ps);
    if (*ps->p == '}') {
        ps->p++;
        return obj;
    }
    hc_json_t *tail = NULL;
    for (;;) {
        skip_ws(ps);
        const char *key;
        int32_t klen;
        if (parse_string_raw(ps, &key, &klen) != 0)
            return NULL;
        skip_ws(ps);
        if (*ps->p != ':') {
            set_err(ps, "expected ':'");
            return NULL;
        }
        ps->p++;
        hc_json_t *val = parse_value(ps);
        if (!val)
            return NULL;
        val->key = key;
        val->klen = klen;
        if (tail)
            tail->next = val;
        else
            obj->child = val;
        tail = val;
        obj->count++;
        skip_ws(ps);
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == '}') {
            ps->p++;
            return obj;
        }
        set_err(ps, "expected ',' or '}'");
        return NULL;
    }
}

static hc_json_t *parse_array(parser_t *ps) {
    hc_json_t *arr = new_node(ps, HC_JSON_ARR);
    if (!arr)
        return NULL;
    ps->p++; /* '[' */
    skip_ws(ps);
    if (*ps->p == ']') {
        ps->p++;
        return arr;
    }
    hc_json_t *tail = NULL;
    for (;;) {
        hc_json_t *val = parse_value(ps);
        if (!val)
            return NULL;
        if (tail)
            tail->next = val;
        else
            arr->child = val;
        tail = val;
        arr->count++;
        skip_ws(ps);
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == ']') {
            ps->p++;
            return arr;
        }
        set_err(ps, "expected ',' or ']'");
        return NULL;
    }
}

static hc_json_t *parse_value(parser_t *ps) {
    if (++ps->depth > HC_JSON_MAX_DEPTH) {
        set_err(ps, "nesting too deep");
        return NULL;
    }
    skip_ws(ps);
    hc_json_t *n = NULL;
    switch (*ps->p) {
    case '{':
        n = parse_object(ps);
        break;
    case '[':
        n = parse_array(ps);
        break;
    case '"': {
        n = new_node(ps, HC_JSON_STR);
        if (n && parse_string_raw(ps, &n->s, &n->slen) != 0)
            n = NULL;
        break;
    }
    case 't':
        if (strncmp(ps->p, "true", 4) == 0) {
            n = new_node(ps, HC_JSON_BOOL);
            if (n)
                n->boolean = 1;
            ps->p += 4;
        } else
            set_err(ps, "bad literal");
        break;
    case 'f':
        if (strncmp(ps->p, "false", 5) == 0) {
            n = new_node(ps, HC_JSON_BOOL);
            ps->p += 5;
        } else
            set_err(ps, "bad literal");
        break;
    case 'n':
        if (strncmp(ps->p, "null", 4) == 0) {
            n = new_node(ps, HC_JSON_NULL);
            ps->p += 4;
        } else
            set_err(ps, "bad literal");
        break;
    default: {
        /* number — strtod 는 correctly-rounded (glibc), Java 와 비트 일치 */
        char *endp;
        double v = strtod(ps->p, &endp);
        if (endp == ps->p) {
            set_err(ps, "expected value");
            break;
        }
        n = new_node(ps, HC_JSON_NUM);
        if (n)
            n->num = v;
        ps->p = endp;
        break;
    }
    }
    ps->depth--;
    return n;
}

hc_json_t *hc_json_parse(const char *buf, hc_arena_t *arena, const char **err,
                         size_t *err_pos) {
    parser_t ps = {buf, buf, arena, NULL, 0};
    hc_json_t *root = parse_value(&ps);
    if (root) {
        skip_ws(&ps);
        if (*ps.p != '\0') {
            set_err(&ps, "trailing garbage");
            root = NULL;
        }
    }
    if (!root) {
        if (err)
            *err = ps.err ? ps.err : "parse error";
        if (err_pos)
            *err_pos = (size_t)(ps.p - ps.buf);
    }
    return root;
}

const hc_json_t *hc_json_get(const hc_json_t *obj, const char *key) {
    if (!obj || obj->kind != HC_JSON_OBJ)
        return NULL;
    size_t klen = strlen(key);
    for (const hc_json_t *m = obj->child; m; m = m->next)
        if ((size_t)m->klen == klen && memcmp(m->key, key, klen) == 0)
            return m;
    return NULL;
}

int hc_json_streq(const hc_json_t *v, const char *s) {
    if (!v || v->kind != HC_JSON_STR)
        return 0;
    size_t n = strlen(s);
    return (size_t)v->slen == n && memcmp(v->s, s, n) == 0;
}
