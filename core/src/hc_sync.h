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
    atomic_uchar f; /* 0 = 풀림, 1 = 잠김 */
} hc_spin_t;

#define HC_SPIN_INIT {0}

static inline void hc_spin_init(hc_spin_t *s) {
    atomic_store_explicit(&s->f, 0, memory_order_release);
}

/* TTAS (P2-9 수정): 획득 실패 시 relaxed 로드로만 관망하고, 풀린 것을
 * 본 뒤에만 acquire 교환을 재시도한다. 경합 RMW 를 반복하는 종전
 * 형태는 네이티브에선 캐시라인 핑퐁, TSan 게이트에선 전역 런타임 락
 * 컨보이가 된다 — P2-9 실발화: GO-1 로 deco 이벤트가 조밀해진 뒤
 * own-σ* (무충돌 산개 순서) 20-워커가 틱-레코더 락에 몰려
 * free_region_own 이 2분 → 75분+ 로 폭주했다 (gdb 백트레이스: 17/20
 * 스레드가 __tsan AtomicRMW 경로). 릴랙스드 로드는 HB 를 만들지
 * 않는다 — 획득 HB 는 acquire 교환만이 부여하므로 시맨틱(값 경로)
 * 불변. 짧은 임계구역(레코더 append/스캔) 전용 — 양보 없이 스핀. */
static inline void hc_spin_lock(hc_spin_t *s) {
    for (;;) {
        if (!atomic_exchange_explicit(&s->f, 1, memory_order_acquire))
            return;
        while (atomic_load_explicit(&s->f, memory_order_relaxed)) {
        }
    }
}

static inline void hc_spin_unlock(hc_spin_t *s) {
    atomic_store_explicit(&s->f, 0, memory_order_release);
}

#endif /* HC_SYNC_H */
