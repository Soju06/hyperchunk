#ifndef HC_POSTPROCESS_H
#define HC_POSTPROCESS_H

#include "hc_features.h"

/* LevelChunk.postProcessGeneration(ServerLevel) 등가 (Task 13) — 승격된
 * 청크 하나의 마킹 드레인. 시맨틱 출처: 26.2 바이트코드 핀
 * (.hermes/notes/task13-close/ 완료 노트에 인용):
 *
 *   per mark (섹션 오름차순, ShortList append 순, 중복 유지):
 *     state = getBlockState(pos); fluid = state.getFluidState()
 *     if (!fluid.isEmpty()) fluid.tick(level, pos, state)   // fall-through
 *     if (block instanceof LiquidBlock) state.tick(...)     // bubble column
 *     else { new = Block.updateFromNeighbourShapes(state, level, pos)
 *            if (new != state) level.setBlock(pos, new, 276) }
 *   then ShortList.clear()
 *
 * 라이브 setBlock 의 부수효과 체인 (onPlace → updateNeighborsAt(W,E,D,U,N,S,
 * CollectingNeighborUpdater LIFO 규율) → updateNeighbourShapes(W,E,N,S,D,U))
 * 과 FlowingFluid 물 스프레드를 재현한다. 라이브 스케줄은 rg->ticks 에
 * t=지연값 (물 5 / 모래 2 / 잎 1 등) 으로 기록 — first-wins dedup 이
 * 월드젠 pendingTicks 와의 충돌을 바닐라처럼 제거한다.
 *
 * marks: 스테이지/피처 재생이 채운 지역 공유 레코더 (frozen 필수 — 아니면
 * die). rg->center 는 함수가 드레인 청크로 옮긴다 (쓰기 창 ±1). */
void hc_postprocess_chunk(hc_feat_region_t *rg, int32_t cx, int32_t cz,
                          const hc_ppg_recorder_t *marks);

/* 진단: 지지-상실 근사 클래스(초목)의 updateShape 평가 횟수 누계 —
 * 게이트가 미모델 측면 규칙에 얼마나 노출됐는지의 상한 지표 (완료 노트
 * 기록용; 값이 0 이 아니어도 게이트 바이트가 green 이면 커버된 것). */
int64_t hc_postprocess_unmodeled_veg_evals(void);

#endif /* HC_POSTPROCESS_H */
