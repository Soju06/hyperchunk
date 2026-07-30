/* 최소 JSON 파서 단위 테스트. 대상은 범용 JSON 준수가 아니라
 * worldgen 데이터팩 JSON 파싱의 정확성이다:
 *  - 숫자의 correctly-rounded double 변환 (Java Gson 과 비트 일치 전제)
 *  - 중첩 객체/배열, zero-copy 문자열, 이스케이프
 *  - 실패 경로가 조용히 통과하지 않는 것 */

#include "../../core/src/hc_json.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fails = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_fails++;                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);      \
        }                                                                      \
    } while (0)

static unsigned char backing[1 << 20];

static hc_json_t *parse(const char *s) {
    static hc_arena_t a;
    hc_arena_init(&a, backing, sizeof backing);
    const char *err = NULL;
    size_t pos = 0;
    hc_json_t *v = hc_json_parse(s, &a, &err, &pos);
    return v;
}

int main(void) {
    /* 숫자: 비트 정확 파싱 (worldgen JSON 실물 상수) */
    {
        hc_json_t *v = parse("-0.5037500262260437");
        CHECK(v && v->kind == HC_JSON_NUM, "num parse");
        uint64_t bits;
        memcpy(&bits, &v->num, 8);
        /* python3: struct.pack('<d', -0.5037500262260437).hex() */
        CHECK(bits == UINT64_C(0xbfe01eb860000000), "num bits");
    }
    {
        hc_json_t *v = parse("0.6666666666666666");
        uint64_t bits;
        memcpy(&bits, &v->num, 8);
        CHECK(v && bits == UINT64_C(0x3fe5555555555555), "num bits 2/3");
    }
    {
        hc_json_t *v = parse("-64");
        CHECK(v && v->num == -64.0, "int as double");
    }
    {
        hc_json_t *v = parse("1.5e3");
        CHECK(v && v->num == 1500.0, "exponent form");
    }

    /* 구조 + 조회 */
    {
        hc_json_t *v = parse("{\"type\": \"minecraft:add\", \"argument1\": "
                             "0.25, \"argument2\": {\"a\": [1, 2, 3]}}");
        CHECK(v && v->kind == HC_JSON_OBJ && v->count == 3, "obj parse");
        const hc_json_t *t = hc_json_get(v, "type");
        CHECK(t && hc_json_streq(t, "minecraft:add"), "string member");
        const hc_json_t *a2 = hc_json_get(v, "argument2");
        const hc_json_t *arr = hc_json_get(a2, "a");
        CHECK(arr && arr->kind == HC_JSON_ARR && arr->count == 3, "arr");
        double sum = 0;
        for (const hc_json_t *e = arr->child; e; e = e->next)
            sum += e->num;
        CHECK(sum == 6.0, "arr elements");
        CHECK(hc_json_get(v, "nope") == NULL, "missing key");
    }

    /* 불리언/널 */
    {
        hc_json_t *v = parse("[true, false, null]");
        CHECK(v && v->count == 3, "literals");
        CHECK(v->child->kind == HC_JSON_BOOL && v->child->boolean == 1, "true");
        CHECK(v->child->next->kind == HC_JSON_BOOL &&
                  v->child->next->boolean == 0,
              "false");
        CHECK(v->child->next->next->kind == HC_JSON_NULL, "null");
    }

    /* 이스케이프 */
    {
        hc_json_t *v = parse("\"a\\n\\\"b\\u0041\"");
        CHECK(v && v->kind == HC_JSON_STR && v->slen == 5 &&
                  memcmp(v->s, "a\n\"bA", 5) == 0,
              "escapes");
    }

    /* 실패 경로: 조용한 통과 금지 */
    CHECK(parse("{\"a\": }") == NULL, "reject bad member");
    CHECK(parse("[1, 2") == NULL, "reject unterminated array");
    CHECK(parse("1 2") == NULL, "reject trailing garbage");
    CHECK(parse("\"abc") == NULL, "reject unterminated string");
    CHECK(parse("nulx") == NULL, "reject bad literal");

    /* 실물 파일 급 입력: 커밋된 reference JSON 을 통째로 (경로는 인자로) */
    printf("test_json: %s\n", g_fails ? "FAIL" : "ok");
    return g_fails ? 1 : 0;
}
