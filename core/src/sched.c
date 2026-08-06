/* P2-3 — 이중-정책 스케줄러 (hc_sched.h 계약 주석 참조).
 *
 * FREE 구현 = 셀별 FIFO: 셀마다 그 셀을 접촉하는 이벤트를 base 순서로
 * 나열하고, 이벤트는 자신이 모든 접촉 셀의 "머리"가 됐을 때만 실행
 * 가능하다. 이는 정확히 "충돌쌍(셀 공유)의 상대순서 = base 순서" 를
 * 강제하고, 그 외 어떤 순서 제약도 두지 않는다. 전역 배리어(n_cells==0)
 * 는 완료 카운터로 게이트한다.
 *
 * 정확성 논거 (완료 노트 §설계): 충돌쌍은 시간상 겹치지 않으므로
 * (완료 후에야 후속이 ready) 같은 상태를 만지는 접근은 전부 base
 * 순서로 직렬화되고, pthread mutex/cond 가 happens-before 를 부여한다
 * — 셀 신고가 풋프린트를 덮는 한 데이터 레이스도 순서 재량도 없다.
 *
 * 스레딩은 이 TU 에만 (pthread — 기존 체인 병렬과 동일 기준). */

#define _POSIX_C_SOURCE 200809L

#include "../include/hc_sched.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const hc_sched_ev_t *evs;
    int32_t              n;

    /* 셀별 이벤트 리스트 (base 순서) — CSR 레이아웃 */
    int32_t *cell_off;  /* [n_cell_space+1] */
    int32_t *cell_evs;  /* [총 접촉 수] */
    int32_t *cell_cur;  /* [n_cell_space] 진행 커서 */

    int32_t *wait;      /* [n] 아직 머리가 아닌 접촉 셀 수 */
    uint8_t *parked;    /* [n] wait==0 인데 배리어에 막힘 */

    int32_t *globals;   /* 전역 배리어 이벤트 인덱스 (오름차순) */
    int32_t  n_globals, next_global;

    int32_t *ready;     /* 링 버퍼 [n] */
    int32_t  r_head, r_tail;

    int32_t completed;
    int     error;

    pthread_mutex_t mu;
    pthread_cond_t  cv;
} sched_t;

static int32_t barrier_limit(const sched_t *s) {
    return s->next_global < s->n_globals ? s->globals[s->next_global] : s->n;
}

static void push_ready(sched_t *s, int32_t e) {
    s->ready[s->r_tail++ % s->n] = e;
}

/* wait==0 이 된 이벤트의 승인 검사 (mu 보유 하에) */
static void admit(sched_t *s, int32_t e) {
    if (e < barrier_limit(s))
        push_ready(s, e);
    else
        s->parked[e] = 1;
}

/* 완료 부기 (mu 보유 하에) */
static void complete(sched_t *s, int32_t e) {
    const hc_sched_ev_t *ev = &s->evs[e];
    s->completed++;
    for (int32_t k = 0; k < ev->n_cells; k++) {
        int32_t c = ev->cells[k];
        int32_t cur = ++s->cell_cur[c];
        if (cur < s->cell_off[c + 1] - s->cell_off[c]) {
            int32_t h = s->cell_evs[s->cell_off[c] + cur];
            if (--s->wait[h] == 0)
                admit(s, h);
        }
    }
    if (ev->n_cells == 0) {
        /* 배리어 통과 — 다음 배리어까지의 parked 승인 */
        s->next_global++;
        int32_t lim = barrier_limit(s);
        for (int32_t i = e + 1; i < lim; i++)
            if (s->parked[i]) {
                s->parked[i] = 0;
                push_ready(s, i);
            }
    }
    /* 다음 전역 배리어가 준비됐나 (자기 앞 전부 완료) */
    if (s->next_global < s->n_globals) {
        int32_t g = s->globals[s->next_global];
        if (s->completed == g)
            push_ready(s, g);
    }
}

typedef struct {
    sched_t *s;
    int32_t  worker;
} sched_worker_t;

static void *worker_main(void *ud) {
    sched_worker_t *wk = ud;
    sched_t        *s = wk->s;
    pthread_mutex_lock(&s->mu);
    for (;;) {
        while (s->r_head == s->r_tail && s->completed < s->n)
            pthread_cond_wait(&s->cv, &s->mu);
        if (s->completed >= s->n)
            break;
        int32_t e = s->ready[s->r_head++ % s->n];
        pthread_mutex_unlock(&s->mu);
        s->evs[e].exec(s->evs[e].ud, wk->worker);
        pthread_mutex_lock(&s->mu);
        complete(s, e);
        pthread_cond_broadcast(&s->cv);
    }
    pthread_mutex_unlock(&s->mu);
    return NULL;
}

int hc_sched_run(const hc_sched_ev_t *evs, int32_t n_evs,
                 int32_t n_cell_space, hc_schedule_policy_t policy,
                 int32_t nthreads, hc_arena_t *a) {
    if (policy == HC_SCHED_REPLAY) {
        for (int32_t i = 0; i < n_evs; i++)
            evs[i].exec(evs[i].ud, 0);
        return 0;
    }
    if (n_evs == 0)
        return 0;
    if (nthreads < 1)
        nthreads = 1;

    sched_t s;
    memset(&s, 0, sizeof s);
    s.evs = evs;
    s.n = n_evs;
    s.cell_off = hc_arena_alloc(a, sizeof(int32_t) * ((size_t)n_cell_space + 1),
                                _Alignof(int32_t));
    s.cell_cur =
        hc_arena_alloc(a, sizeof(int32_t) * (size_t)n_cell_space, 4);
    s.wait = hc_arena_alloc(a, sizeof(int32_t) * (size_t)n_evs, 4);
    s.parked = hc_arena_alloc(a, (size_t)n_evs, 1);
    s.globals = hc_arena_alloc(a, sizeof(int32_t) * (size_t)n_evs, 4);
    s.ready = hc_arena_alloc(a, sizeof(int32_t) * (size_t)n_evs, 4);
    if (!s.cell_off || !s.cell_cur || !s.wait || !s.parked || !s.globals ||
        !s.ready)
        return -1;
    memset(s.cell_off, 0, sizeof(int32_t) * ((size_t)n_cell_space + 1));
    memset(s.cell_cur, 0, sizeof(int32_t) * (size_t)n_cell_space);
    memset(s.parked, 0, (size_t)n_evs);

    size_t total = 0;
    for (int32_t i = 0; i < n_evs; i++) {
        const hc_sched_ev_t *e = &evs[i];
        if (e->n_cells == 0) {
            s.globals[s.n_globals++] = i;
            continue;
        }
        total += (size_t)e->n_cells;
        for (int32_t k = 0; k < e->n_cells; k++) {
            int32_t c = e->cells[k];
            if (c < 0 || c >= n_cell_space) {
                fprintf(stderr, "hc_sched: cell id %d out of space %d\n", c,
                        n_cell_space);
                abort();
            }
            s.cell_off[c + 1]++;
        }
    }
    for (int32_t c = 0; c < n_cell_space; c++)
        s.cell_off[c + 1] += s.cell_off[c];
    s.cell_evs = hc_arena_alloc(a, sizeof(int32_t) * (total ? total : 1), 4);
    if (!s.cell_evs)
        return -1;
    {
        int32_t *fill =
            hc_arena_alloc(a, sizeof(int32_t) * (size_t)n_cell_space, 4);
        if (!fill)
            return -1;
        memset(fill, 0, sizeof(int32_t) * (size_t)n_cell_space);
        for (int32_t i = 0; i < n_evs; i++)
            for (int32_t k = 0; k < evs[i].n_cells; k++) {
                int32_t c = evs[i].cells[k];
                s.cell_evs[s.cell_off[c] + fill[c]++] = i;
            }
    }
    /* 초기 wait/ready */
    for (int32_t i = 0; i < n_evs; i++)
        s.wait[i] = evs[i].n_cells;
    for (int32_t c = 0; c < n_cell_space; c++)
        if (s.cell_off[c + 1] > s.cell_off[c])
            s.wait[s.cell_evs[s.cell_off[c]]]--;
    for (int32_t i = 0; i < n_evs; i++)
        if (evs[i].n_cells > 0 && s.wait[i] == 0)
            admit(&s, i);
    if (s.n_globals > 0 && s.globals[0] == 0)
        push_ready(&s, 0); /* 첫 이벤트가 배리어 */

    pthread_mutex_init(&s.mu, NULL);
    pthread_cond_init(&s.cv, NULL);

    enum { MAX_W = 64 };
    if (nthreads > MAX_W)
        nthreads = MAX_W;
    pthread_t      tids[MAX_W];
    sched_worker_t wks[MAX_W];
    for (int32_t t = 0; t < nthreads; t++) {
        wks[t].s = &s;
        wks[t].worker = t;
        if (pthread_create(&tids[t], NULL, worker_main, &wks[t]) != 0) {
            fprintf(stderr, "hc_sched: pthread_create failed\n");
            abort();
        }
    }
    for (int32_t t = 0; t < nthreads; t++)
        pthread_join(tids[t], NULL);
    pthread_mutex_destroy(&s.mu);
    pthread_cond_destroy(&s.cv);
    if (s.completed != s.n) {
        fprintf(stderr, "hc_sched: deadlock — %d/%d completed (cell 신고나 "
                        "배리어 구성이 순환 대기를 만들었다)\n",
                s.completed, s.n);
        abort();
    }
    return 0;
}
