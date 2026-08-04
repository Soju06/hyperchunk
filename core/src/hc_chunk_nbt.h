#ifndef HC_CHUNK_NBT_H
#define HC_CHUNK_NBT_H

#include <stddef.h>
#include <stdint.h>

#include "../include/hc_arena.h"
#include "../include/hc_chunk.h"
#include "hc_biome.h"
#include "hc_features.h" /* hc_tick_rec_t */
#include "hc_light.h"    /* hc_light_chunk_t */

/* 26.2 청크 NBT 직렬화 — 내부 전용 (core/src), Task 12.
 *
 * 스펙 출처: .hermes/notes/task12-region/R-C (SerializableChunkData 방출
 * 시퀀스), R-D (틱), R-E (PalettedContainer 재팩/라이트 방출/하이트맵) +
 * golden r.0.0.mca 전수 실측. 요지:
 *  - 루트 15키 고정 스키마 (full 청크; blending_data/UpgradeData/entities
 *    등 조건 키는 신규 생성 월드에서 전부 부재).
 *  - 섹션: 라이트 섹션 범위 -5..20 순회, 블록 범위(-4..19)는 항상
 *    block_states+biomes, 라이트 레이어는 "저장소 등록 && (materialized
 *    || defv!=0)" — 증분 이력이 increase 뿐인 월드젠에선 "등록 && 최종값
 *    >0 존재" 와 동치 (R-E §7f).
 *  - 팔레트: 저장 시 재팩 — 인덱스 스캔 첫-등장 순, 블록 bits =
 *    max(4, ceillog2(n)) / 바이옴 = ceillog2(n), n==1 이면 data 생략,
 *    LSB-first 무스팬 패킹 (R-E §2/§3).
 *  - 틱: 레코더 배열에서 이 청크 소속만 기록순으로 방출 (R-D §4).
 *  - LastUpdate 는 인자 (canonical 게이트는 마스킹 — 0 을 넘기면 참조
 *    페이로드(마스킹본)와 직접 바이트 비교 가능).
 *
 * 반환: 페이로드 길이, 실패(-1: arena/버퍼 소진). scratch 는 노드/중간
 * 배열용 (호출자가 리셋 관리).
 *
 * sctx (Task 14): 구조물 컨텍스트 — starts/References/block_entities 를
 * 채운다. NULL = 기존 게이트 그대로 (빈 리스트/컴파운드). */
struct hc_sctx; /* hc_structures.h */
ptrdiff_t hc_chunk_to_nbt(const hc_chunk_t *c, const hc_biome_reg_t *biomes,
                          const hc_light_chunk_t *ls,
                          const hc_tick_rec_t *ticks, int32_t n_ticks,
                          int64_t last_update, const struct hc_sctx *sctx,
                          hc_arena_t *scratch, uint8_t *out, size_t cap);

#endif /* HC_CHUNK_NBT_H */
