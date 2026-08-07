#ifndef HC_DF_SIMD_H
#define HC_DF_SIMD_H

#include "../include/hc_df.h"

/* --- SIMD 백엔드 (P2-4, ADR-004 D1/D2/D3/D4) — 내부 전용 (core/src) ---
 *
 * AVX2 4-레인 콘 스트림 평가기. 계약은 단 하나다: **각 레인의 값은 같은
 * 좌표의 스칼라 평가와 IEEE-754 비트 동일** (ADR-004 D4 게이트 대상).
 * 이를 위해:
 *  - 레인별 연산 시퀀스는 스칼라 eval_node 와 동일 (재배열 금지). 수평
 *    리덕션 없음 — 레인은 서로 독립인 평가점이라 리덕션 자체가 없다.
 *  - FMA 계열 인트린식 금지 (mul/add 분리 — ADR-002 Pitfall 1, ADR-004
 *    D3). rcp/rsqrt 근사 금지 — 나눗셈은 vdivpd (IEEE 정확 반올림이라
 *    스칼라 나눗셈과 비트 동일).
 *  - 브랜치/선택 (jmin/jmax/clamp/range_choice/±0/NaN) 은 cmp+blend 로
 *    Java 시맨틱을 레인별 재현 — blend 는 비트 선택이라 반올림이 없다.
 *  - mth_floor/lfloor 의 포화·NaN 시맨틱이 필요한 좌표 범위 밖 레인이
 *    있으면 그 노드는 전 레인 스칼라 폴백 (26.2 오버월드 도달 불가,
 *    데이터팩 방어).
 *
 * 백엔드 선택은 cpuid 런타임 디스패치 (D2): 코어는 항상 두 경로를 담고,
 * AVX2 부재 호스트는 스칼라로 폴백한다. AVX-512 는 P2-6 슬롯 (미구현). */

typedef enum {
    HC_ISA_SCALAR = 0,
    HC_ISA_AVX2 = 1,
    /* HC_ISA_AVX512 — P2-6 슬롯 (cpuid 검출·커널 미구현) */
} hc_isa_t;

/* 검출 결과 (1회 cpuid, 원자 캐시 — TSan 클린). 오버라이드가 있으면
 * 그 값. x86 이 아니면 항상 SCALAR. */
hc_isa_t hc_isa_active(void);

/* 테스트/벤치 오버라이드: HC_ISA_SCALAR/HC_ISA_AVX2, 음수 = auto 재검출.
 * AVX2 강제는 cpuid 확인을 통과해야 반영된다 (SIGILL 방지 — 부재
 * 호스트에서는 SCALAR 로 남는다). 생성 스레드 기동 전 호출 전제. */
void hc_isa_force(int isa);

/* 4 평가점 레인 묶음. x/y/z 는 블록 좌표 (정수값 전제 — 스칼라 경로와
 * 동일). dx/dy/dz 는 CELL 모드 interp lerp3 델타 — 호출자가 스칼라와
 * 같은 식 ((double)in_cell / (double)cell_dim) 으로 채운다. */
typedef struct {
    double x[4], y[4], z[4];
    double dx[4], dy[4], dz[4];
} hc_df_lanes_t;

/* 스트림 = 콘 프로그램 (hc_df_cone_program) 또는 플레인 콘 리스트 —
 * 플레인 리스트 (전 워드 >= 0) 는 컨트롤 없는 프로그램과 동형이라 같은
 * 워커가 소비한다. 지원 op 화이트리스트 검사 (1회, 콘 산출 시). */
int hc_df_stream_x4_ok(const hc_df_graph_t *g, const int32_t *stream,
                       int32_t words);

/* 4-레인 동시 평가. vscratch 는 [g->n][4] SoA (32B 정렬), 결과는
 * vscratch[4*node + lane]. 사전 조건: hc_df_stream_x4_ok == 1,
 * hc_isa_active() == HC_ISA_AVX2. 값은 레인별 스칼라 평가와 비트 동일
 * — df_x4 게이트가 판정한다. */
void hc_df_eval_stream_x4_avx2(const hc_df_graph_t *g, const int32_t *stream,
                               int32_t words, const hc_df_lanes_t *lanes,
                               double *vscratch, const hc_df_cellctx_t *cc);

#endif /* HC_DF_SIMD_H */
