/* P2-3 — hc_sched 속성 게이트.
 *
 * 검증 속성 (FREE 정책의 정확성 = 이 둘이 전부):
 *  P1. 충돌쌍 순서: 셀을 공유하는 두 이벤트 (a<b) 는 a 의 종료 스탬프 <
 *      b 의 시작 스탬프 (겹침 없음 + base 순서).
 *  P2. 배리어: 전역 이벤트 g 는 모든 e<g 종료 후 시작하고, 모든 e>g 는
 *      g 종료 후 시작한다.
 * 스탬프는 전역 원자 카운터 — 실행이 실제로 병렬이어도 순서 관측은
 * 선형화된다. REPLAY 정책은 완전 순차 (스탬프 = base 순서) 를 확인.
 *
 * TSan 게이트의 스케줄러 스트레스이기도 하다 (build-tsan 에서 같은
 * 바이너리 실행 — 워커/완료 부기의 레이스 검출). */

#define _POSIX_C_SOURCE 200809L

#include "../../core/include/hc_sched.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { GRID = 16, NCELLS = GRID * GRID, MAXEV = 4096, MAXC = 9 };

static _Atomic int32_t g_clock;
static int32_t g_start[MAXEV], g_end[MAXEV];

typedef struct {
    int32_t idx;
    int32_t spin; /* 실행 시간 흔들기 — 인터리브 다양화 */
} ev_ud_t;

static void ev_exec(void *ud, int32_t worker) {
    (void)worker;
    ev_ud_t *e = ud;
    g_start[e->idx] =
        atomic_fetch_add_explicit(&g_clock, 1, memory_order_relaxed);
    volatile int32_t acc = 0;
    for (int32_t i = 0; i < e->spin; i++)
        acc += i;
    (void)acc;
    g_end[e->idx] =
        atomic_fetch_add_explicit(&g_clock, 1, memory_order_relaxed);
}

/* xorshift — 재현 가능한 결정론 시드 */
static uint64_t g_rng;
static uint32_t rnd(uint32_t bound) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng % bound);
}

static int32_t build_events(hc_sched_ev_t *evs, ev_ud_t *uds,
                            int32_t (*cells)[MAXC], int32_t n,
                            int global_every) {
    for (int32_t i = 0; i < n; i++) {
        uds[i].idx = i;
        uds[i].spin = (int32_t)rnd(2000);
        evs[i].exec = ev_exec;
        evs[i].ud = &uds[i];
        if (global_every && i > 0 && (int32_t)rnd((uint32_t)global_every) == 0) {
            evs[i].cells = NULL;
            evs[i].n_cells = 0;
            continue;
        }
        /* 무작위 중심 + 반경 0..1 박스 (1 또는 9셀) — 실제 사용 형태 */
        int32_t cx = (int32_t)rnd(GRID), cz = (int32_t)rnd(GRID);
        int32_t r = (int32_t)rnd(2);
        int32_t m = 0;
        for (int32_t dz = -r; dz <= r; dz++)
            for (int32_t dx = -r; dx <= r; dx++) {
                int32_t x = cx + dx, z = cz + dz;
                if (x < 0 || x >= GRID || z < 0 || z >= GRID)
                    continue;
                cells[i][m++] = z * GRID + x;
            }
        evs[i].cells = cells[i];
        evs[i].n_cells = m;
    }
    return n;
}

static int share_cell(const hc_sched_ev_t *a, const hc_sched_ev_t *b) {
    for (int32_t i = 0; i < a->n_cells; i++)
        for (int32_t j = 0; j < b->n_cells; j++)
            if (a->cells[i] == b->cells[j])
                return 1;
    return 0;
}

static void check_props(const hc_sched_ev_t *evs, int32_t n) {
    for (int32_t b = 0; b < n; b++)
        for (int32_t a = 0; a < b; a++) {
            int conflict = evs[a].n_cells == 0 || evs[b].n_cells == 0 ||
                           share_cell(&evs[a], &evs[b]);
            if (!conflict)
                continue;
            if (!(g_end[a] < g_start[b])) {
                fprintf(stderr,
                        "FAIL: 충돌쌍 (%d,%d) 순서 위반 — end[a]=%d "
                        "start[b]=%d\n",
                        a, b, g_end[a], g_start[b]);
                exit(1);
            }
        }
}

int main(void) {
    static hc_sched_ev_t evs[MAXEV];
    static ev_ud_t       uds[MAXEV];
    static int32_t       cells[MAXEV][MAXC];

    size_t      backing_sz = 64u << 20;
    void       *backing = malloc(backing_sz);
    hc_arena_t  arena;
    assert(backing);

    /* 시드×구성 스윕: 배리어 없음 / 드문 배리어 / 촘촘한 배리어 */
    static const struct {
        int32_t n;
        int     global_every;
        int32_t nthreads;
    } CFG[] = {
        {2000, 0, 8}, {2000, 200, 8}, {1000, 40, 16},
        {512, 0, 2},  {64, 8, 8},     {1, 0, 4},
    };
    for (size_t c = 0; c < sizeof CFG / sizeof CFG[0]; c++) {
        for (uint64_t seed = 1; seed <= 3; seed++) {
            g_rng = seed * 0x9E3779B97F4A7C15ull + c;
            int32_t n = build_events(evs, uds, cells, CFG[c].n,
                                     CFG[c].global_every);
            atomic_store(&g_clock, 0);
            memset(g_start, -1, sizeof g_start);
            memset(g_end, -1, sizeof g_end);
            hc_arena_init(&arena, backing, backing_sz);
            if (hc_sched_run(evs, n, NCELLS, HC_SCHED_FREE, CFG[c].nthreads,
                             &arena) != 0) {
                fprintf(stderr, "FAIL: hc_sched_run (arena)\n");
                return 1;
            }
            for (int32_t i = 0; i < n; i++)
                if (g_start[i] < 0 || g_end[i] < 0) {
                    fprintf(stderr, "FAIL: 이벤트 %d 미실행\n", i);
                    return 1;
                }
            check_props(evs, n);
        }
    }

    /* REPLAY: 스탬프가 base 순서 그대로 */
    g_rng = 42;
    int32_t n = build_events(evs, uds, cells, 300, 30);
    atomic_store(&g_clock, 0);
    hc_arena_init(&arena, backing, backing_sz);
    assert(hc_sched_run(evs, n, NCELLS, HC_SCHED_REPLAY, 1, &arena) == 0);
    for (int32_t i = 1; i < n; i++)
        if (!(g_end[i - 1] < g_start[i])) {
            fprintf(stderr, "FAIL: REPLAY 순차성 위반 (%d)\n", i);
            return 1;
        }

    printf("test_sched: PASS (FREE 충돌쌍/배리어 속성 %zu 구성 × 3시드 + "
           "REPLAY 순차성)\n",
           sizeof CFG / sizeof CFG[0]);
    free(backing);
    return 0;
}
