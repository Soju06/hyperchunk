#include "hc_arena.h"

/* 의도적 스텁 (TDD RED) — GREEN 커밋에서 실제 구현으로 교체된다. */

void hc_arena_init(hc_arena_t *a, void *backing, size_t cap) {
    (void)backing;
    (void)cap;
    a->base = 0;
    a->cap  = 0;
    a->off  = 0;
}

void *hc_arena_alloc(hc_arena_t *a, size_t n, size_t align) {
    (void)a;
    (void)n;
    (void)align;
    return 0;
}

void hc_arena_reset(hc_arena_t *a) {
    (void)a;
}

size_t hc_arena_used(const hc_arena_t *a) {
    (void)a;
    return (size_t)-1;
}
