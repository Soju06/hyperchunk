#ifndef HC_SYNC_H
#define HC_SYNC_H

/* 최소 동기화 프리미티브 (P2-3 FREE 스케줄러) — 순수 C11 stdatomic.
 * 코어 무의존 원칙(ADR-003 D1) 유지: pthread 는 스케줄러 TU(sched.c)에만.
 *
 * 용도: 리전-공유 레코더(틱/BE)의 물리적 append 안전. 논리적 순서는
 * 스케줄러의 충돌쌍 순서 보존이 담보한다 — 같은 키(pos)의 접근자는 항상
 * 같은 청크의 ±1 이웃 = 충돌 이벤트라 동시 실행이 없고, 스핀락은 서로
 * 무관한 이벤트의 동시 append 에서 배열 카운터/슬롯 초기화의 찢김만
 * 막는다. REPLAY(단일 스레드) 경로엔 무경합 lock/unlock 비용뿐. */

#include <stdatomic.h>

typedef struct {
    atomic_flag f;
} hc_spin_t;

#define HC_SPIN_INIT {ATOMIC_FLAG_INIT}

static inline void hc_spin_init(hc_spin_t *s) {
    atomic_flag_clear_explicit(&s->f, memory_order_release);
}

static inline void hc_spin_lock(hc_spin_t *s) {
    while (atomic_flag_test_and_set_explicit(&s->f, memory_order_acquire)) {
        /* 짧은 임계구역(레코더 append/스캔) 전용 — 양보 없이 스핀 */
    }
}

static inline void hc_spin_unlock(hc_spin_t *s) {
    atomic_flag_clear_explicit(&s->f, memory_order_release);
}

#endif /* HC_SYNC_H */
