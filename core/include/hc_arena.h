#ifndef HC_ARENA_H
#define HC_ARENA_H

#include <stddef.h>
#include <stdint.h>

/* bump 방식 arena 할당자 (ADR-003 D3). 청크 생성 중 힙 할당을 0 으로
 * 만드는 기반이다 — 코어는 malloc 을 모르고, 호출자가 backing 을 준다.
 *
 * 계약:
 * - hc_arena_alloc 은 align (2의 거듭제곱) 배수 '주소' 를 반환한다.
 *   backing 자체의 정렬에 의존하지 않는다.
 * - 소진/오버플로 시 NULL 을 반환한다. abort 하지 않는다 — 배치 중
 *   arena 소진은 호출자가 backing 을 키워 재시도할 수 있는 정상 경로다.
 * - free 는 없다. hc_arena_reset 이 배치 단위로 전체를 되감는다.
 *   reset 은 메모리를 지우지 않으므로 stale 데이터 방어는 소비자 몫이다
 *   (hc_chunk_init 의 zero-fill, ADR-003 Pitfall 3). */

typedef struct {
    unsigned char *base;
    size_t         cap;
    size_t         off;
} hc_arena_t;

void   hc_arena_init(hc_arena_t *a, void *backing, size_t cap);
void  *hc_arena_alloc(hc_arena_t *a, size_t n, size_t align);
void   hc_arena_reset(hc_arena_t *a);
size_t hc_arena_used(const hc_arena_t *a);

#endif /* HC_ARENA_H */
