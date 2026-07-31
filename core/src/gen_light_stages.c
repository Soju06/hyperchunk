/* 08_initialize_light / 09_light 스테이지 진입점 (Task 10).
 *
 * 바닐라 대응 (R1 §1.2/§1.3):
 *  - 08 ChunkStatusTasks.initializeLight = initializeLightSources(src_y 는
 *    솔버가 현재 블록에서 재유도) + 비-공기 섹션 등록 + setLightEnabled(false).
 *    의존 반경 0.
 *  - 09 ChunkStatusTasks.light = propagateLightSources (광원 활성 + 시딩).
 *    의존: 반경 1 이 INITIALIZE_LIGHT 이상.
 *
 * 실제 수치 계산은 hc_light_solve 의 배치 고정점이 담당한다 — 증분
 * 엔진과의 등가 논증은 hc_light.h / R2 §10. 호출 규약: 스냅샷 상태를
 * (featured/register/enable) 로 표시한 뒤 solve 한 번. */

#include "hc_light.h"

#include <stdio.h>
#include <stdlib.h>

void hc_gen_initialize_light_stage(hc_light_world_t *w, int32_t cx,
                                   int32_t cz) {
    /* 피라미드: 08 은 자기 청크 FEATURES 만 요구 — hc_light_register 가
     * featured 선행을 검사한다. */
    hc_light_register(w, cx, cz);
}

void hc_gen_light_stage(hc_light_world_t *w, int32_t cx, int32_t cz) {
    /* 피라미드: 반경 1 이 08 완료여야 한다 (LIGHT addRequirement). */
    for (int32_t dz = -1; dz <= 1; dz++)
        for (int32_t dx = -1; dx <= 1; dx++) {
            hc_light_chunk_t *s =
                &w->slots[(cz + dz - w->cz0) * w->n + (cx + dx - w->cx0)];
            if (!s->chunk || !s->in_r) {
                fprintf(stderr,
                        "hc_gen_light_stage(%d,%d): neighbor (%d,%d) not at "
                        "INITIALIZE_LIGHT\n",
                        cx, cz, cx + dx, cz + dz);
                abort();
            }
        }
    hc_light_enable(w, cx, cz);
}
