#ifndef HC_JSON_H
#define HC_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "hc_arena.h"

/* 최소 JSON DOM 파서 — 내부 전용 (core/src), 의존성 0 (ADR-003 D1).
 *
 * 목적은 범용 JSON 이 아니라 바닐라 worldgen 데이터팩 JSON
 * (density_function / noise / noise_settings) 의 파싱이다. 값 타입은
 * 그 스키마가 쓰는 것만: null/bool/number/string/array/object.
 *
 * 계약:
 *  - 입력 버퍼는 NUL 종단이어야 하고 (strtod 경계), 파스 트리가 살아있는
 *    동안 caller 가 유지해야 한다 (문자열이 버퍼를 가리킨다).
 *  - 모든 노드는 caller 의 arena 에서 할당된다. 실패 시 NULL + *err.
 *  - 숫자는 double 로만 저장한다. strtod 는 glibc 에서 correctly-rounded
 *    라 Java(Gson)의 십진→binary64 결과와 비트 일치한다.
 *  - 이스케이프가 없는 문자열은 zero-copy, 있으면 arena 로 복사·해제한다. */

typedef enum {
    HC_JSON_NULL = 0,
    HC_JSON_BOOL,
    HC_JSON_NUM,
    HC_JSON_STR,
    HC_JSON_ARR,
    HC_JSON_OBJ,
} hc_json_kind_t;

typedef struct hc_json hc_json_t;
struct hc_json {
    uint8_t kind;
    uint8_t boolean;   /* HC_JSON_BOOL */
    double  num;       /* HC_JSON_NUM */
    const char *s;     /* HC_JSON_STR: NUL 종단 아님 — slen 사용 */
    int32_t     slen;
    hc_json_t *child;  /* ARR: 첫 원소, OBJ: 첫 멤버 값 */
    hc_json_t *next;   /* 형제 */
    const char *key;   /* OBJ 멤버일 때 키 (slen 규칙 동일) */
    int32_t     klen;
    int32_t     count; /* ARR/OBJ: 자식 수 */
};

/* 파싱. 실패 시 NULL 을 돌려주고 *err 에 정적 메시지, *err_pos 에
 * 대략적 오프셋을 넣는다 (NULL 허용). */
hc_json_t *hc_json_parse(const char *buf, hc_arena_t *arena,
                         const char **err, size_t *err_pos);

/* OBJ 에서 키 조회. 없으면 NULL. */
const hc_json_t *hc_json_get(const hc_json_t *obj, const char *key);

/* STR 을 C 문자열과 비교 */
int hc_json_streq(const hc_json_t *v, const char *s);

#endif /* HC_JSON_H */
