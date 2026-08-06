#ifndef HC_LIGHT_H
#define HC_LIGHT_H

#include <stdint.h>

#include "../include/hc_arena.h"
#include "../include/hc_chunk.h"

/* 26.2 라이트 엔진 배치 솔버 — 내부 전용 (core/src).
 *
 * 시맨틱 출처: .hermes/notes/task10-light/R1..R4 (javap, server-26.2.jar).
 * 바닐라의 증분 엔진(FIFO increase/decrease 큐)은 모노톤 격자 위 최소
 * 고정점(lfp)을 유지한다 (R2 §10 증명). 월드젠 윈도우에서 관측 가능한
 * 상태는 항상 (현재 블록 상태, 등록 집합 R, 광원 활성 집합 S) 의 lfp 다
 * — 파이프라인 피라미드가 "빛이 닿는 청크는 features 완료" 를 보장해서
 * stale 라이트가 존재할 수 없다 (R1 §5.2/§6). 따라서 스케줄 재현 없이
 * 스냅샷마다 0 에서 다시 푸는 배치 솔버로 덤프와 비트 일치가 가능하다.
 *
 * 순서 의존성은 두 입력으로만 들어온다:
 *  - 블록 상태: features order.manifest 프리픽스 (order.snapshots seqBegin)
 *  - S: 덤프 시점까지 propagateLightSources 가 실행된 청크 집합
 *
 * 이 지역 팔레트에는 useShapeForLightOcclusion 상태가 없다 (R4 §6 — 유일
 * 후보 snow[layers=1..7] 는 warm 바이옴이라 등장 불가). shapeOccludes 는
 * 구조적으로 항상 false — 코드에서 생략하고, 등장 불가 전제가 깨지는
 * 블록(die-list)은 조우 즉시 abort 한다. */

/* 라이트 섹션 범위: 월드 섹션 [-4..19] ±1 (26.2 storage 는 위아래 한
 * 섹션을 더 관리한다 — R3 §2.3) */
enum { HC_LIGHT_SEC_MIN = -5, HC_LIGHT_SEC_MAX = 20, HC_LIGHT_NSEC = 26 };
enum { HC_LIGHT_BLOCK = 0, HC_LIGHT_SKY = 1 };

/* src_y 열린 컬럼 센티널 (ChunkSkyLightSources.NEGATIVE_INFINITY 등가) */
#define HC_LIGHT_OPEN INT32_MIN
/* top 미설정 (topSections 미스 — 어떤 섹션도 등록 안 됨) */
#define HC_LIGHT_NO_TOP INT32_MIN

typedef struct {
    hc_chunk_t *chunk;      /* NULL = 슬롯 비어 있음 */
    uint8_t    *light[2];   /* [ (sec-SEC_MIN)*4096 + ((y&15)<<8|(z&15)<<4|(x&15)) ] */
    uint32_t    registered; /* bit (sec - SEC_MIN): DataLayer 존재 (LIGHT_ONLY 포함) */
    int32_t     top;        /* topSections: max 등록 섹션 y + 1; HC_LIGHT_NO_TOP */
    uint8_t     feat_done;  /* getChunkForLighting 가시성 (status >= FEATURES) */
    uint8_t     in_r;       /* 08 initializeLight 실행됨 (섹션 등록 주체) */
    uint8_t     enabled;    /* 09 propagateLightSources 실행됨 (S) */
    uint8_t     seeded;     /* 누적 모드: propagateLightSources 시딩 완료
                             * (바닐라는 청크당 1회 — 재시딩 없음) */
    int32_t     src_y[256]; /* ChunkSkyLightSources.getLowestSourceY; hc_col_idx */
} hc_light_chunk_t;

typedef struct {
    hc_light_chunk_t *slots; /* [ (cz-cz0)*n + (cx-cx0) ] */
    int32_t   cx0, cz0, n;
    uint64_t *queue;         /* BFS FIFO 링 (2^qlog 엔트리) */
    uint32_t  qlog;
} hc_light_world_t;

/* 슬롯/큐를 arena 에서 할당. 라이트 배열은 attach 시 할당. 실패 -1. */
int hc_light_world_init(hc_light_world_t *w, hc_arena_t *a, int32_t cx0,
                        int32_t cz0, int32_t n);

/* 청크를 슬롯에 연결 (c->cx/cz 로 위치 결정; 범위 밖이면 abort). */
int hc_light_attach(hc_light_world_t *w, hc_arena_t *a, hc_chunk_t *c);

/* 라이트 값·R/S·feat 플래그 전부 초기화 (재솔브 준비). 청크 연결 유지. */
void hc_light_reset(hc_light_world_t *w);

/* 스냅샷 상태 표시. featured => getChunkForLighting 가 이 청크를 돌려준다.
 * register => 08 실행 (섹션 등록). enable => 09 실행 (S 멤버). */
void hc_light_set_featured(hc_light_world_t *w, int32_t cx, int32_t cz);
void hc_light_register(hc_light_world_t *w, int32_t cx, int32_t cz);
void hc_light_enable(hc_light_world_t *w, int32_t cx, int32_t cz);

/* 현재 블록 상태 + R/S 로 고정점 계산. 등록 지오메트리(섹션/top/src_y)는
 * 매번 현재 블록에서 재유도한다 (post-08 쓰기의 updateSectionStatus /
 * skyLightSources.update / checkBlock 재동기와 등가 — R3 §3.2/§4.4). */
void hc_light_solve(hc_light_world_t *w);

/* 덤프 읽기 (visible 스냅샷 규칙 — R3 §2.2): sky 는 top 이상 15, 미등록
 * 갭은 위쪽 첫 레이어 바닥 슬라이스; block 은 미등록 0. y 는 월드 좌표. */
int hc_light_get(const hc_light_world_t *w, int layer, int32_t x, int32_t y,
                 int32_t z);

/* --- 누적 (increase-only 이력) 모드 (Task 14) ---
 *
 * 바닐라 .mca 저장 라이트는 어느 시점의 lfp 도 아니고, 증가-전용 이력의
 * 최종 상태다: 각 청크의 propagateLightSources(09) 는 그 시점 블록으로
 * 1회 시딩·flood 하고, 이후 데코 쓰기는 재조명하지 않는다 (ProtoChunk
 * setBlockState 는 updateSectionStatus/skyLightSources.update 만 —
 * checkBlock 없음). 나중 배치 이웃의 flood 증가 유입은 반영된다.
 * 재현: 배치 시간순으로 (블록 전진, 등록/지오메트리 재유도, 새 청크만
 * 시딩) 을 리셋 없이 반복 — 값은 단조 증가, 최종 상태가 저장분.
 *
 * prepare: 등록 지오메트리 (in_r 전체, 현재 블록) 재유도. 마스크/top 은
 *   단조 성장 (updateSectionStatus 등가 — ProtoChunk 쓰기에 라이브).
 *   새 섹션의 라이트는 0 (createDataLayer 등가 — repeatFirstLayer 가
 *   필요한 below-top 생성은 fail-loud; 이 지역은 등록 범위가 전부
 *   바닥-고정이라 구조적으로 상단 확장뿐).
 * init_chunk: 08 (initializeLight) 이벤트 — 등록 + src_y 동결 (fillFrom).
 *   src_y 는 이후 블록 쓰기로 갱신되지 않는다 (c.2.26 실측: 나중-배치
 *   캐노피 아래 골든 sky 15 — 09 직접 채움이 08 시점 컬럼을 썼다).
 *   "등록은 제출 즉시 유효" (task13 §3) — 호출 시점 = 제출 카운터.
 * light_chunk: enable + 시딩 + flood 1회 (seeded 가드). 시딩은 현재
 *   블록/등록 + 08-동결 src_y (자신·이웃) 기준. */
void hc_light_accum_prepare(hc_light_world_t *w);
void hc_light_accum_init_chunk(hc_light_world_t *w, int32_t cx, int32_t cz);
void hc_light_accum_light_chunk(hc_light_world_t *w, int32_t cx, int32_t cz);

/* 스테이지 진입점 (gen_light_stages.c) — 08 은 register 의 별칭 + 선행
 * 검사, 09 는 반경-1 피라미드 요건 검사 후 enable. */
void hc_gen_initialize_light_stage(hc_light_world_t *w, int32_t cx,
                                   int32_t cz);
void hc_gen_light_stage(hc_light_world_t *w, int32_t cx, int32_t cz);

#endif /* HC_LIGHT_H */
