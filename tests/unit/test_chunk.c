/* SoA 청크 스토리지 단위 테스트 (Plan Task 4).
 *
 * 핵심은 zero-fill 불변식이다 (ADR-003 Pitfall 3): arena 를 배치 간
 * 재사용할 때 stale 데이터가 새 청크로 새면 산발적 패리티 버그가 된다.
 * backing 을 0xAA 로 오염시킨 뒤 init 이 전부 지우는지 확인한다.
 *
 * hc_idx 레이아웃은 golden 덤프 (golden/stages/FORMAT.md) 와 같아야
 * 스테이지 대조가 인덱스 변환 없이 성립한다. */

#include "hc_chunk.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef NDEBUG
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static _Alignas(64) unsigned char backing[HC_BLOCKS * sizeof(uint16_t) + 4096];

int main(void) {
    hc_arena_t a;
    hc_arena_init(&a, backing, sizeof backing);
    memset(backing, 0xAA, sizeof backing); /* stale arena 재사용 시뮬레이션 */

    hc_chunk_t c;
    assert(hc_chunk_init(&c, &a, 3, -7) == 0);
    assert(c.cx == 3 && c.cz == -7);
    assert(c.states != NULL);

    /* zero-fill 불변식: 블록도 하이트맵도 전부 0 */
    for (size_t i = 0; i < (size_t)HC_BLOCKS; i++)
        assert(c.states[i] == 0);
    for (int i = 0; i < 256; i++) {
        assert(c.heightmap_ws[i] == 0);
        assert(c.heightmap_ocean_floor[i] == 0);
    }

    /* 인덱스 레이아웃 == FORMAT.md: ((y - minY) * 16 + z) * 16 + x */
    assert(hc_idx(0, HC_MIN_Y, 0) == 0);
    assert(hc_idx(15, HC_MAX_Y, 15) == (size_t)HC_BLOCKS - 1);
    assert(hc_idx(5, 0, 9) == (size_t)(((0 - HC_MIN_Y) * 16 + 9) * 16 + 5));
    assert(hc_idx(1, HC_MIN_Y, 0) - hc_idx(0, HC_MIN_Y, 0) == 1);   /* x 최내측 */
    assert(hc_idx(0, HC_MIN_Y, 1) - hc_idx(0, HC_MIN_Y, 0) == 16);  /* z 중간 */
    assert(hc_idx(0, HC_MIN_Y + 1, 0) - hc_idx(0, HC_MIN_Y, 0) == 256); /* y 최외측 */
    assert(hc_col_idx(0, 0) == 0);
    assert(hc_col_idx(15, 15) == 255);
    assert(hc_col_idx(1, 0) == 1);

    /* states 배열이 arena 범위 안에 있고 64바이트 정렬이다 */
    assert((unsigned char *)c.states >= backing);
    assert((unsigned char *)c.states + HC_BLOCKS * sizeof(uint16_t) <=
           backing + sizeof backing);
    assert(((uintptr_t)c.states & 63u) == 0);

    /* 소진된 arena 에서는 -1, abort 아님 */
    hc_arena_t tiny;
    unsigned char small_backing[64];
    hc_arena_init(&tiny, small_backing, sizeof small_backing);
    hc_chunk_t c2;
    assert(hc_chunk_init(&c2, &tiny, 0, 0) == -1);

#ifndef NDEBUG
    /* ADR-009 D3 negative test: 범위 밖 hc_idx 는 debug 빌드에서 assert 로
     * 죽어야 한다. fork 한 자식에서 관찰한다 — assert 는 SIGABRT 이지만,
     * sanitizer 가 abort 를 가로채 비정상 exit 코드로 바꾸는 구성도 있어
     * 둘 다 '죽음' 으로 인정한다. 자식이 0 으로 살아 돌아오는 것만이
     * 실패(= assert 가 없다)다. NDEBUG 빌드에서는 assert 자체가 컴파일
     * 아웃이라 이 테스트가 성립하지 않으므로 통째로 제외한다. */
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        volatile size_t sink = hc_idx(0, HC_MIN_Y - 1, 0); /* y 하한 밖 */
        (void)sink;
        _exit(0); /* assert 미발화 → 부모가 FAIL 판정 */
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    int died_by_abort = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    int died_by_exit  = WIFEXITED(status) && WEXITSTATUS(status) != 0;
    assert(died_by_abort || died_by_exit);
#else
    printf("test_chunk: NDEBUG build — hc_idx negative assert test skipped\n");
#endif

    return 0;
}
