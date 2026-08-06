#ifndef HC_SCHED_H
#define HC_SCHED_H

/* P2-3 — 스케줄러 정책 주입점 (ADR-008 Pitfall 1: 라이브러리 계약).
 *
 * 이벤트 리스트는 base 선형화(= REPLAY 가 실행할 순서)로 주어진다.
 * 각 이벤트는 자신이 접촉하는 "셀"(청크 격자 + 가상 셀 — 구조물 피스
 * 그룹 등)의 집합을 신고한다. 두 이벤트는 셀이 겹치면 충돌쌍이다.
 *
 *  - HC_SCHED_REPLAY: base 순서 그대로 단일 스레드 실행 (검증 모드,
 *    ADR-008 D2). 기존 드라이버의 순차 루프와 정의상 동일.
 *  - HC_SCHED_FREE: 충돌쌍의 상대순서만 base 로 고정하고 (셀별 FIFO)
 *    나머지를 워커 풀에 푼다 (ADR-008 D1). 셀 신고가 이벤트의 실제
 *    읽기/쓰기 풋프린트를 덮으면, 비충돌 이벤트는 서로소 상태 위의
 *    순수 변환이라 교환 가능 — 결과는 REPLAY 와 비트 동일하다
 *    (Phase 1 A0 §1.3 "disjoint ⇒ commute" 원리의 스케줄러화).
 *
 * n_cells == 0 은 전역 배리어: 앞의 전 이벤트 완료 후 단독 실행되고,
 * 뒤의 어떤 이벤트도 그 완료 전에 시작하지 않는다 (light prepare 등
 * 전역 풋프린트 이벤트용).
 *
 * 스테이지 코드는 두 정책이 공유한다 (ADR-008 D3) — 이 모듈은 실행
 * "순서/병렬성"만 소유하고 이벤트 본문은 exec 콜백이다. 파일 I/O 없음
 * (ADR-003 D1) — manifest 파싱은 드라이버 소관, 여기는 순수 계산. */

#include <stdint.h>

#include "hc_arena.h"

typedef enum {
    HC_SCHED_REPLAY = 0, /* 검증 모드: base 순서 순차 재생 */
    HC_SCHED_FREE = 1,   /* 벤치 모드: 충돌쌍-보존 최대 병렬 */
} hc_schedule_policy_t;

typedef struct {
    const int32_t *cells;   /* 접촉 셀 id 배열 (0 <= id < n_cell_space),
                             * 중복 없음. NULL/0개 = 전역 배리어. */
    int32_t        n_cells;
    void (*exec)(void *ud, int32_t worker); /* worker: 0..nthreads-1 */
    void *ud;
} hc_sched_ev_t;

/* 실행. 내부 표는 arena 에서 할당 (호출자가 마크/롤백 가능). nthreads 는
 * FREE 전용 (REPLAY 는 호출 스레드 = worker 0). 성공 0, 오류 -1. */
int hc_sched_run(const hc_sched_ev_t *evs, int32_t n_evs,
                 int32_t n_cell_space, hc_schedule_policy_t policy,
                 int32_t nthreads, hc_arena_t *a);

#endif /* HC_SCHED_H */
