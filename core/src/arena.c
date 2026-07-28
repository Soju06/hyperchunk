#include "hc_arena.h"

#include <assert.h>

void hc_arena_init(hc_arena_t *a, void *backing, size_t cap) {
    a->base = (unsigned char *)backing;
    a->cap  = cap;
    a->off  = 0;
}

void *hc_arena_alloc(hc_arena_t *a, size_t n, size_t align) {
    /* align 은 2의 거듭제곱이어야 한다. 위반은 호출자 버그라 debug 에서
     * assert 로 잡고, release 에서는 NULL 로 방어한다. */
    assert(align != 0 && (align & (align - 1)) == 0);
    if (align == 0 || (align & (align - 1)) != 0)
        return NULL;

    /* 정렬은 오프셋이 아니라 주소 기준이다 — backing 이 align 미만으로
     * 정렬돼 있어도 반환 주소는 align 배수여야 한다. */
    uintptr_t cur     = (uintptr_t)a->base + a->off;
    uintptr_t aligned = (cur + (align - 1)) & ~((uintptr_t)align - 1);
    if (aligned < cur)
        return NULL; /* 주소공간 상단 랩어라운드 */

    size_t p = (size_t)(aligned - (uintptr_t)a->base);
    /* `p + n > cap` 형태는 p + n 랩어라운드로 뚫린다. 뺄셈으로 비교한다. */
    if (p > a->cap || n > a->cap - p)
        return NULL; /* 소진: NULL, abort 아님 — 호출자가 복구 가능 */

    a->off = p + n;
    return a->base + p;
}

void hc_arena_reset(hc_arena_t *a) {
    /* 메모리는 지우지 않는다. stale 방어는 소비자의 zero-fill 몫이다
     * (ADR-003 Pitfall 3, hc_chunk_init). */
    a->off = 0;
}

size_t hc_arena_used(const hc_arena_t *a) {
    return a->off;
}
