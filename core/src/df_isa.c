#include "hc_df_simd.h"

#include <stdatomic.h>

/* ISA 검출/오버라이드 + x4 화이트리스트 (P2-4, ADR-004 D2).
 * 이 TU 는 -mavx2 없이 컴파일된다 — AVX2 코드는 df_simd_avx2.c 에만. */

#if defined(__x86_64__) || defined(__i386__)
static int detect_avx2(void) {
    /* __builtin_cpu_supports: GCC/Clang 내장 (외부 라이브러리 아님).
     * OS 의 YMM 상태 지원 (OSXSAVE/XGETBV) 까지 검사한다. */
    return __builtin_cpu_supports("avx2") ? HC_ISA_AVX2 : HC_ISA_SCALAR;
}
#else
static int detect_avx2(void) {
    return HC_ISA_SCALAR;
}
#endif

/* -1 = 미검출/auto. 검출은 멱등이라 중복 실행 무해 — 원자 접근만 보장
 * (TSan 클린, ADR-009 Pitfall 3 대비). */
static _Atomic int g_isa = -1;

hc_isa_t hc_isa_active(void) {
    int v = atomic_load_explicit(&g_isa, memory_order_relaxed);
    if (v < 0) {
        v = detect_avx2();
        atomic_store_explicit(&g_isa, v, memory_order_relaxed);
    }
    return (hc_isa_t)v;
}

void hc_isa_force(int isa) {
    if (isa < 0) {
        atomic_store_explicit(&g_isa, -1, memory_order_relaxed);
        return;
    }
    /* AVX2 강제는 하드웨어 확인 통과 시에만 — 부재 호스트 SIGILL 방지
     * (ADR-004 Verification: 폴백 확인 항목) */
    if (isa == HC_ISA_AVX2 && detect_avx2() != HC_ISA_AVX2)
        isa = HC_ISA_SCALAR;
    atomic_store_explicit(&g_isa, isa, memory_order_relaxed);
}

/* --- x4 화이트리스트 --- */

static int op_x4_ok(uint8_t op) {
    switch (op) {
    case HC_DF_CONST:
    case HC_DF_X:
    case HC_DF_Y:
    case HC_DF_Z:
    case HC_DF_ADD:
    case HC_DF_MUL:
    case HC_DF_MIN:
    case HC_DF_MAX:
    case HC_DF_ADD_CONST:
    case HC_DF_MUL_CONST:
    case HC_DF_ABS:
    case HC_DF_SQUARE:
    case HC_DF_CUBE:
    case HC_DF_HALF_NEGATIVE:
    case HC_DF_QUARTER_NEGATIVE:
    case HC_DF_SQUEEZE:
    case HC_DF_INVERT:
    case HC_DF_CLAMP:
    case HC_DF_Y_CLAMPED_GRADIENT:
    case HC_DF_RANGE_CHOICE:
    case HC_DF_NOISE:
    case HC_DF_BLENDED_NOISE:
    case HC_DF_INTERVAL_SELECT:
    case HC_DF_BLEND_OFFSET:
    case HC_DF_BLEND_ALPHA:
    case HC_DF_BLEND_DENSITY:
    case HC_DF_INTERPOLATED:
    case HC_DF_FLAT_CACHE:
    case HC_DF_CACHE_2D:
    case HC_DF_CACHE_ONCE:
    case HC_DF_CACHE_ALL_IN_CELL:
        return 1;
    default:
        /* SHIFTED_NOISE/SHIFT_A/SHIFT_B/SPLINE/FTS — 스칼라 전용 (핫 콘에
         * 없다; 필요해지면 커널 추가 후 화이트리스트 확장) */
        return 0;
    }
}

/* 스트림 구조 워크 (prog_run 과 동일한 디코딩) — 전 명령의 op 검사 */
static int walk_ok(const hc_df_graph_t *g, const int32_t *p, int32_t words) {
    const int32_t *end = p + words;
    while (p < end) {
        int32_t v = *p++;
        if (v >= 0) {
            if (!op_x4_ok(g->nodes[v].op))
                return 0;
            continue;
        }
        if (v == -1) { /* PROG_RC */
            int32_t ch = p[0], wt = p[1], we = p[2];
            if (!op_x4_ok(g->nodes[ch].op))
                return 0;
            if (!walk_ok(g, p + 3, wt) || !walk_ok(g, p + 3 + wt, we))
                return 0;
            p += 3 + wt + we;
        } else { /* PROG_IS */
            int32_t        ch = p[0], nf = p[1];
            const int32_t *w = p + 2;
            if (!op_x4_ok(g->nodes[ch].op))
                return 0;
            const int32_t *q = p + 2 + nf;
            for (int32_t k = 0; k < nf; k++) {
                if (!walk_ok(g, q, w[k]))
                    return 0;
                q += w[k];
            }
            p = q;
        }
    }
    return 1;
}

int hc_df_stream_x4_ok(const hc_df_graph_t *g, const int32_t *stream,
                       int32_t words) {
    return walk_ok(g, stream, words);
}
