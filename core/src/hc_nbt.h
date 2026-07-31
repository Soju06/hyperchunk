#ifndef HC_NBT_H
#define HC_NBT_H

#include <stddef.h>
#include <stdint.h>

#include "../include/hc_arena.h"

/* 빅엔디언 NBT 라이터 — 내부 전용 (core/src), 의존성 0 (ADR-003 D1).
 *
 * 용도는 범용 NBT 가 아니라 26.2 청크 직렬화의 바이트 재현이다 (Task 12,
 * canonical payload 게이트). 바이트 정확성에 필요한 바닐라 규칙:
 *
 *  - 컴파운드 키 방출 순서 = java.util.HashMap 순회 순서. CompoundTag 는
 *    기본 생성자 HashMap(cap 16, LF 0.75) 이고, 순회는 "버킷 오름차순,
 *    버킷 내 삽입 순서" 다 (Java 8+ 의 리사이즈 split 은 체인 상대 순서를
 *    보존하므로 최종 캐퍼시티만 알면 된다). 이 에뮬레이션은 golden
 *    r.0.0.mca 전 청크(1024)의 모든 컴파운드 키 순서를 재현함을 실측
 *    검증했다 (.hermes/notes/task12-region/). 따라서 hc_nbt_put 호출
 *    순서는 바닐라의 put 순서와 같아야 한다 — 같은 버킷에 충돌하는 키들
 *    (예: zPos 와 block_entities, BlockLight 와 Y) 의 상대 순서가 여기서
 *    결정된다.
 *  - 루트는 무명 컴파운드: id 0x0A + 길이 0 문자열 + 페이로드.
 *  - 빈 리스트의 엘리먼트 태그 = TAG_End(0).
 *  - 문자열은 modified UTF-8 — 이 파이프라인의 문자열은 전부 ASCII 라
 *    UTF-8 과 동일하다. 비 ASCII 는 조우 즉시 assert (die-list 관례).
 *
 * 노드/키/배열은 arena 수명이다. 스칼라 외 배열 페이로드는 복사하지
 * 않는다 (호출자가 수명 보장). 실패 규약: 생성자는 NULL, put/add/write
 * 는 -1 (arena 소진 / 버퍼 초과 — abort 아님). */

enum {
    HC_NBT_END = 0,
    HC_NBT_BYTE = 1,
    HC_NBT_SHORT = 2,
    HC_NBT_INT = 3,
    HC_NBT_LONG = 4,
    HC_NBT_BYTE_ARRAY = 7,
    HC_NBT_STRING = 8,
    HC_NBT_LIST = 9,
    HC_NBT_COMPOUND = 10,
    HC_NBT_INT_ARRAY = 11,
    HC_NBT_LONG_ARRAY = 12,
};

typedef struct hc_nbt hc_nbt_t;

hc_nbt_t *hc_nbt_byte(hc_arena_t *a, int32_t v);
hc_nbt_t *hc_nbt_short(hc_arena_t *a, int32_t v);
hc_nbt_t *hc_nbt_int(hc_arena_t *a, int32_t v);
hc_nbt_t *hc_nbt_long(hc_arena_t *a, int64_t v);
/* s 는 NUL 종단, 복사하지 않음 */
hc_nbt_t *hc_nbt_string(hc_arena_t *a, const char *s);
/* [s, s+n) 을 arena 로 복사 (부분 문자열용 — 팔레트 프로퍼티 파싱) */
hc_nbt_t *hc_nbt_string_n(hc_arena_t *a, const char *s, size_t n);
hc_nbt_t *hc_nbt_byte_array(hc_arena_t *a, const uint8_t *d, int32_t n);
/* 호스트 순서 long 배열 — 직렬화 시 빅엔디언 변환 */
hc_nbt_t *hc_nbt_long_array(hc_arena_t *a, const int64_t *d, int32_t n);
hc_nbt_t *hc_nbt_compound(hc_arena_t *a);
hc_nbt_t *hc_nbt_list(hc_arena_t *a);

/* key 는 복사하지 않음 (정적 문자열 또는 arena 수명). 중복 키 금지
 * (assert). 반환 0/-1. */
int hc_nbt_put(hc_arena_t *a, hc_nbt_t *comp, const char *key, hc_nbt_t *v);
/* 리스트 append. 엘리먼트 태그 균질성 assert. 반환 0/-1. */
int hc_nbt_add(hc_arena_t *a, hc_nbt_t *list, hc_nbt_t *v);

/* 무명 루트로 직렬화. 성공 시 페이로드 길이, 초과/오류 시 -1. */
ptrdiff_t hc_nbt_write(const hc_nbt_t *root, uint8_t *out, size_t cap);

/* HashMap 순회 순서 에뮬레이터 (테스트/검증 노출용).
 * keys[0..n) 를 삽입 순서로 보고 perm[i] = i번째로 방출되는 키의 삽입
 * 인덱스를 쓴다. n <= 64. */
void hc_nbt_java_map_order(const char *const *keys, int n, uint8_t *perm);

#endif /* HC_NBT_H */
