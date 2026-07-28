/* Arena 할당자 단위 테스트 (Plan Task 4).
 *
 * 계약 검증: 요청 정렬의 '주소' 반환, used 추적, reset, 그리고 소진 시
 * NULL (abort 금지). 오버플로 안전성은 SIZE_MAX 근처 요청으로 찌른다 —
 * naive 한 `off + n` 비교는 여기서 랩어라운드해 가짜 성공을 낸다. */

/* 테스트의 assert 는 빌드 플레이버와 무관하게 항상 살아 있어야 한다 —
 * NDEBUG 빌드에서 assert 가 증발하면 테스트가 공허하게 통과한다. */
#undef NDEBUG

#include "hc_arena.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    static _Alignas(64) unsigned char backing[4096];
    hc_arena_t a;

    hc_arena_init(&a, backing, sizeof backing);
    assert(hc_arena_used(&a) == 0);

    /* 기본 할당 + 정렬 */
    void *p = hc_arena_alloc(&a, 100, 16);
    assert(p != NULL);
    assert(((uintptr_t)p & 15u) == 0);
    memset(p, 0x5A, 100); /* 반환 영역이 실제로 쓰기 가능해야 한다 */

    size_t used_before = hc_arena_used(&a);
    assert(used_before >= 100);

    /* 연속 할당은 겹치지 않고 전진한다 */
    unsigned char *q = (unsigned char *)hc_arena_alloc(&a, 1, 1);
    assert(q != NULL);
    assert(q >= (unsigned char *)p + 100);
    assert(hc_arena_used(&a) > used_before);

    /* 홀수 오프셋 직후의 큰 정렬 요구도 주소 기준으로 맞아야 한다 */
    void *r = hc_arena_alloc(&a, 8, 64);
    assert(r != NULL);
    assert(((uintptr_t)r & 63u) == 0);

    /* reset 은 전체를 되감는다 */
    hc_arena_reset(&a);
    assert(hc_arena_used(&a) == 0);

    /* 용량 초과는 NULL 이고, 실패한 할당은 상태를 오염시키지 않는다 */
    assert(hc_arena_alloc(&a, 1u << 20, 16) == NULL);
    assert(hc_arena_used(&a) == 0);

    /* 산술 오버플로: off + (align-1) 또는 p + n 이 랩어라운드해도 NULL */
    assert(hc_arena_alloc(&a, SIZE_MAX, 16) == NULL);
    assert(hc_arena_alloc(&a, SIZE_MAX - 8, 16) == NULL);
    assert(hc_arena_used(&a) == 0);

    /* 정확히 남은 용량은 성공, 그 위로 1바이트는 실패 */
    void *full = hc_arena_alloc(&a, sizeof backing, 1);
    assert(full == backing);
    assert(hc_arena_used(&a) == sizeof backing);
    assert(hc_arena_alloc(&a, 1, 1) == NULL);

    /* n=0 도 유효한 정렬 주소를 반환한다 (전진 없음이어도 무방) */
    hc_arena_reset(&a);
    void *z = hc_arena_alloc(&a, 0, 32);
    assert(z != NULL);
    assert(((uintptr_t)z & 31u) == 0);

    return 0;
}
