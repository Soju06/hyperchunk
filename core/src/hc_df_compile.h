#ifndef HC_DF_COMPILE_H
#define HC_DF_COMPILE_H

#include "../include/hc_df.h"
#include "hc_json.h"

/* worldgen JSON → 평탄 DF IR 컴파일러 — 내부 전용 (core/src), 의존성 0.
 *
 * 호출자(테스트/CLI)가 reference JSON 을 파싱해 이름표로 넘기면, 여기서
 * noise_router 표현식을 hc_df_graph_t 로 내린다. 파일 I/O 는 하지 않는다
 * (ADR-003 D1). 시맨틱 원칙:
 *
 *  - 이름 참조 (minecraft:overworld/... 등) 는 사이클을 검출하고, 두 번
 *    참조돼도 한 번만 컴파일해 노드를 공유한다 — 바닐라 레지스트리가
 *    인스턴스를 공유하고 6c 캐시 시맨틱이 그 정체성에 의존한다.
 *  - "noise" 파라미터는 레지스트리 키 문자열로 dedup 하고 (RandomState
 *    getOrCreateNoise 의 ConcurrentHashMap 과 동형), fromHashOf(키) 로
 *    시딩한다. old_blended_noise 는 fromHashOf("minecraft:terrain").
 *  - add/mul 은 상수 인자 감지 시 바닐라 create() 그대로 MulOrAdd
 *    (ADD_CONST/MUL_CONST) 로 내린다 — Ap2 MUL 단락과 비트가 다르다.
 *  - 마커(interpolated/cache_*)는 별도 op 로 유지된 pass-through 다. */

typedef struct {
    const char      *name; /* 전체 id, 예: "minecraft:overworld/factor" */
    const hc_json_t *json;
} hc_df_source_t;

/* 고정 상한 — 초과는 컴파일 에러로 fail-loud (조용한 절단 금지).
 * 26.2 오버월드 라우터 실측: 노드 ~600, 스플라인 ~400, 노이즈 ~35. */
enum {
    HC_DFC_MAX_NODES = 4096,
    HC_DFC_MAX_NOISES = 128,
    HC_DFC_MAX_BLENDED = 8,
    HC_DFC_MAX_SPLINES = 2048,
    HC_DFC_MAX_IPOOL = 16384,
    HC_DFC_MAX_DPOOL = 1024,
    HC_DFC_MAX_DEPTH = 128, /* JSON 중첩/이름 참조 재귀 한도 */
};

typedef struct {
    hc_arena_t           *arena;
    int64_t               seed;
    const hc_df_source_t *dfs;
    int32_t               n_dfs;
    const hc_df_source_t *noise_params;
    int32_t               n_noise_params;

    hc_df_graph_t *g;

    /* dedup/사이클 상태 */
    int32_t *df_state;   /* dfs[i] → 노드 idx, -1 미컴파일, -2 진행 중 */
    char   **noise_keys; /* g->noises[i] 의 정규화된 키 */
    int32_t  depth;

    const char *err; /* 실패 시 정적/버퍼 메시지 */
    char        errbuf[256];
} hc_df_compiler_t;

/* 그래프 배열을 arena 에서 상한 크기로 확보하고 상태를 초기화한다.
 * 실패(-1) 는 arena 부족뿐이다. */
int hc_df_compiler_init(hc_df_compiler_t *c, hc_df_graph_t *g,
                        hc_arena_t *arena, int64_t seed,
                        const hc_df_source_t *dfs, int32_t n_dfs,
                        const hc_df_source_t *noise_params,
                        int32_t n_noise_params);

/* 표현식(숫자/이름 문자열/객체) 하나를 컴파일해 루트 노드 인덱스를
 * 돌려준다. 실패 시 -1, c->err 에 원인. 여러 번 불러 슬롯별 루트를
 * 얻는다 — 이름 참조/노이즈는 호출 간 공유된다. */
int32_t hc_df_compile_expr(hc_df_compiler_t *c, const hc_json_t *expr);

#endif /* HC_DF_COMPILE_H */
