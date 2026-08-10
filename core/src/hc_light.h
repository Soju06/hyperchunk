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
    uint32_t    blk_written; /* bit (sec - SEC_MIN): block 레이어에 >0 쓰기가
                              * 있었음 = 바닐라 DataLayer 실체화 (감쇠로
                              * all-0 이 돼도 저장에 남는다 — Task 14 실측
                              * c.4.10: 버섯 배치-발광 후 엣지-사멸) */
    int32_t     top;        /* topSections: max 등록 섹션 y + 1; HC_LIGHT_NO_TOP */
    uint8_t     feat_done;  /* getChunkForLighting 가시성 (status >= FEATURES) */
    uint8_t     in_r;       /* 08 initializeLight 실행됨 (섹션 등록 주체) */
    uint8_t     enabled;    /* 09 propagateLightSources 실행됨 (S) */
    uint8_t     seeded;     /* 누적 모드: propagateLightSources 시딩 완료
                             * (바닐라는 청크당 1회 — 재시딩 없음) */
    uint8_t     geo_dirty;  /* 누적 모드: 라이브 쓰기 후 지오메트리 재유도
                             * 대기 (dirty 목록 중복 방지) */
    uint8_t     blk_dirty;  /* 누적 모드 (P2-8): 마지막 재유도 이후 이
                             * 청크 states[] 에 변경이 있었음 — prepare 가
                             * 이 플래그 없는 in_r 청크의 재유도를 스킵한다
                             * (무변경 재유도 = no-op 증명, P2-8 노트 §2).
                             * 설정: accum_write + 하네스 훅의
                             * mark_written (동결-창 쓰기 포함 — 라이트
                             * 미반영이어도 섹션 등록에는 라이브).
                             * 해제: 그 청크의 재유도 (08/flush/prepare). */
    int32_t     src_y[256]; /* ChunkSkyLightSources.getLowestSourceY; hc_col_idx */
} hc_light_chunk_t;

/* 이벤트-로컬 가변 상태 (P2-3): BFS 큐 + checkBlock 펜딩 + dirty 목록.
 * REPLAY 는 월드 내장 ctx0 하나로 기존과 동일하게 돈다. FREE 워커는
 * 각자 ctx 를 가진다 — 펜딩/큐는 "그 이벤트의 쓰기" 만 담으므로 이벤트당
 * flush 시맨틱이 REPLAY (데코마다 flush) 와 같고, 서로 무관한 이벤트의
 * flush 는 공간상 분리(footprint ±2 청크)라 슬롯 데이터 접근이 겹치지
 * 않는다 (스케줄러 충돌 규칙이 담보). */
typedef struct {
    uint64_t *inc;       /* 증가 BFS FIFO 링 (2^qlog 엔트리) */
    uint64_t *dec;       /* 감소 큐 (checkBlock 기계 — 증가와 동시 활성) */
    uint32_t  qlog;
    /* blockNodesToCheck 등가 (checkBlock 펜딩 셋; flush 가 소비).
     * check[i] = q_push 패킹과 같은 좌표 인코딩 (level/mask 0). */
    uint64_t *check;
    int32_t   n_check;
    uint32_t  check_log;
    /* 지오메트리 재유도 대기 청크 (라이브 쓰기 대상; flush 선두에서
     * updateSectionStatus 등가 처리) — 슬롯 인덱스 목록 */
    int32_t  *dirty;
    int32_t   n_dirty;
} hc_light_ctx_t;

typedef struct hc_light_world {
    hc_light_chunk_t *slots; /* [ (cz-cz0)*n + (cx-cx0) ] */
    int32_t   cx0, cz0, n;
    hc_light_ctx_t ctx0;     /* 기본 ctx (REPLAY/단일 스레드 경로) */
} hc_light_world_t;

/* 워커 ctx 할당 (arena). qlog/check_log 는 이벤트-로컬 규모로 줄여도
 * 된다 (한 이벤트 = 한 청크 시딩+드레인 또는 한 데코 flush). 실패 -1. */
int hc_light_ctx_init(hc_light_ctx_t *c, hc_arena_t *a, uint32_t qlog,
                      uint32_t check_log, int32_t dirty_cap);

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

/* --- 누적 이력 모드 (Task 14) ---
 *
 * 바닐라 .mca 저장 라이트는 어느 시점의 lfp 도 아니고, 증분 이력의 최종
 * 상태다. 이벤트 시맨틱 (R1 §5.3/§6 + R3 §3.2/§4.4 + 잔차 실측 역산):
 *
 *  - fillFrom (src_y) 은 08 status 태스크 본체가 gen 워커에서 동기 실행
 *    (제출 카운터 시점 블록). 그런데 chunk status 의 INITIALIZE_LIGHT
 *    승격은 라이트 스레드가 08 큐 태스크를 처리한 뒤 (= 08 completion,
 *    stages.log comp 라인) 다. 그 사이 "동결 창" 의 블록 쓰기는
 *    ProtoChunk.setBlockState 라이트 훅 (status>=08 가드) 을 못 밟아
 *    영구 미반영 — src_y 도 checkBlock 도 없다. (r2 실측: 08-동결만으로
 *    최종-lfp 대비 sky 음수 잔차 18,237셀 전소.)
 *  - 승격 후 (라이브) 라이트-속성 변경 쓰기는 skyLightSources.update
 *    (§4.4 증분) + checkBlock (checkNode → 감소/PULL/재flood) 을 밟는다.
 *    accum_write + accum_flush 가 이 경로다.
 *  - 나중 배치 이웃 flood 의 증가 유입은 저장분에 반영된다 (배치-스냅샷
 *    모델은 음수 잔차로 기각 — r3 실측 442 mismatch).
 *
 * prepare: 등록 지오메트리 (in_r 전체, 현재 블록) 재유도. 마스크/top 은
 *   단조 성장 (updateSectionStatus 등가 — ProtoChunk 쓰기에 라이브).
 *   새 섹션의 라이트는 0 (createDataLayer 등가 — repeatFirstLayer 가
 *   필요한 below-top 생성은 fail-loud; 이 지역은 등록 범위가 전부
 *   바닥-고정이라 구조적으로 상단 확장뿐).
 * init_chunk: 08 (initializeLight) 이벤트 — 등록 + src_y fillFrom.
 *   "등록은 제출 즉시 유효" (task13 §3) — 호출 시점 = 제출 카운터.
 * light_chunk: enable + 시딩 + flood 1회 (seeded 가드). 시딩은 현재
 *   블록/등록 + 현재 src_y (자신·이웃) 기준.
 * write: 라이브 쓰기 1건 (호출자가 08-완료 카운터 게이트; 블록 배열은
 *   이미 갱신된 뒤). 라이트-속성 (dampening/emission/USO) 이 다르면
 *   src_y 증분 update + checkBlock 펜딩. 속성 동일이면 no-op.
 * flush: 펜딩 checkBlock 처리 = 레이어별 checkNode 전체 → 감소 큐 완전
 *   드레인 → 증가 큐 드레인 (runLightUpdates §3 순서). δ_wake 마이크로-
 *   배치 등가 — 그룹핑은 국소 lfp 재구축이라 결과 불변 (R2 §10). */
void hc_light_accum_prepare(hc_light_world_t *w);
void hc_light_accum_init_chunk(hc_light_world_t *w, int32_t cx, int32_t cz);
/* P2-8: states[] 변경 신고 (blk_dirty). 호출처 = 블록 쓰기 훅 — 라이트
 * 훅과 달리 동결-창 필터 **앞**에서 부른다: 동결-창 쓰기는 checkBlock/
 * src_y 에는 영구 미반영이지만 (헤더 위 주석), 섹션 등록은 현재 블록
 * 재유도라 라이브다 — 이 구분이 prepare dirty-skip 의 정확성 조건.
 * ore 벌크 쓰기 (features.c ore_do_place, BulkSectionAccess 등가) 는
 * 훅 미경유지만 비-공기→비-공기 뿐이라 섹션 비-공기 프로필을 못 바꾼다
 * (P2-8 노트 §2 채널 전수). 슬롯 밖 좌표는 no-op. */
void hc_light_accum_mark_written(hc_light_world_t *w, int32_t x, int32_t z);
void hc_light_accum_light_chunk(hc_light_world_t *w, int32_t cx, int32_t cz);
void hc_light_accum_write(hc_light_world_t *w, int32_t x, int32_t y, int32_t z,
                          uint16_t old_id, uint16_t new_id);
void hc_light_accum_flush(hc_light_world_t *w);

/* ctx 명시 변형 (P2-3 FREE 스케줄러) — 위 함수들은 &w->ctx0 래퍼.
 * 같은 스테이지 코드 공유 (ADR-008 D3): 정책이 바뀌어도 본문은 하나다. */
void hc_light_accum_light_chunk_ctx(hc_light_world_t *w, hc_light_ctx_t *c,
                                    int32_t cx, int32_t cz);
void hc_light_accum_write_ctx(hc_light_world_t *w, hc_light_ctx_t *c,
                              int32_t x, int32_t y, int32_t z,
                              uint16_t old_id, uint16_t new_id);
void hc_light_accum_flush_ctx(hc_light_world_t *w, hc_light_ctx_t *c);

/* 스테이지 진입점 (gen_light_stages.c) — 08 은 register 의 별칭 + 선행
 * 검사, 09 는 반경-1 피라미드 요건 검사 후 enable. */
void hc_gen_initialize_light_stage(hc_light_world_t *w, int32_t cx,
                                   int32_t cz);
void hc_gen_light_stage(hc_light_world_t *w, int32_t cx, int32_t cz);

#endif /* HC_LIGHT_H */
