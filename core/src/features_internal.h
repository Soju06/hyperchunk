#ifndef HC_FEATURES_INTERNAL_H
#define HC_FEATURES_INTERNAL_H

#include "hc_features.h"

/* features.c / features_tree.c 공유 내부 계약 — 코어 밖 비공개. */

typedef struct feat_env {
    hc_feat_region_t      *rg;
    hc_wgr_t              *rng;
    int64_t                level_seed; /* geode 내부 노이즈 (R5a §4.2) */
    const hc_feat_reg_t   *reg;
    const hc_biome_view_t *view;
    const hc_biome_reg_t  *biomes; /* freeze_top_layer 온도 게이트 */
    int32_t                sea_level;
    const hc_pfeat_t      *pf;
    int32_t                step, index;
    const hc_feat_trace_t *trace;
    int32_t                npos;
    int32_t                placed_any; /* 0/1 */
    int32_t                unknown;    /* 미구현 본문 도달 */
    int32_t                nested; /* PlacedFeature.place 경로 (biome 금지) */
} feat_env_t;

_Noreturn void hc_featx_die(const char *what, const char *detail);
int      hc_featx_mask_test(const uint64_t *mask, uint16_t id);
int32_t  hc_featx_iprov_sample(hc_wgr_t *r, const hc_iprov_t *p);
uint16_t hc_featx_sprov_sample(hc_wgr_t *r, const hc_sprov_t *p);
/* 위치 인자 버전 — noise_threshold_provider (Task 14) 만 위치를 쓴다 */
uint16_t hc_featx_sprov_sample_at(hc_wgr_t *r, const hc_sprov_t *p,
                                  int32_t x, int32_t y, int32_t z);
/* 블록 술어 평가 (features.c — 드로우 0) */
int hc_featx_bpred_eval(feat_env_t *e, const hc_bpred_t *p, int32_t x,
                        int32_t y, int32_t z);
/* 중첩 placed feature 실행 (PlacedFeature.place — 트레이스 없음) */
int hc_featx_run_nested(feat_env_t *e, const hc_pfeat_t *pf, int32_t x,
                        int32_t y, int32_t z);

/* features_tree.c — R2 본문 */
int hc_featx_tree_place(feat_env_t *e, int32_t x, int32_t y, int32_t z);
int hc_featx_ftree_place(feat_env_t *e, int32_t x, int32_t y, int32_t z);

/* Task 14 — BlockState.updateShape 디스패치 공유 (features_tree.c 구현;
 * structures_template.c 의 updateShapeAtEdge/updateFromNeighbourShapes 가
 * 사용). dir_mc: 0=DOWN,1=UP,2=N,3=S,4=W,5=E. edge_face 는 한 면 이벤트
 * (양측 updateShape + 조건부 flag-2 쓰기 + 틱). state/ticks 는 fold
 * (updateFromNeighbourShapes) 용 분해 진입점 — state 는 체인된 s 를
 * 받아 즉시 상태만 계산, ticks 는 틱 스케줄만. */
uint16_t hc_featx_edge_state(feat_env_t *e, uint16_t s, int32_t x, int32_t y,
                             int32_t z, int dir_mc, uint16_t ns);
void     hc_featx_edge_ticks(feat_env_t *e, uint16_t s, int32_t x, int32_t y,
                             int32_t z, int dir_mc, uint16_t ns);
void     hc_featx_edge_face(feat_env_t *e, int32_t x, int32_t y, int32_t z,
                            int dir_mc);
/* isFaceSturdy(FULL) — 풀큐브/azalea + stairs/slab/trapdoor 형상 면 */
int hc_featx_face_sturdy_t14(uint16_t s, int dir_mc);

/* features_ring.c — Task 10 링 프리픽스 본문 */
int hc_featx_disk_place(feat_env_t *e, const hc_disk_cfg_t *c, int32_t x,
                        int32_t y, int32_t z);
int hc_featx_seagrass_place(feat_env_t *e, const hc_seagrass_cfg_t *c,
                            int32_t x, int32_t y, int32_t z);
int hc_featx_lake_place(feat_env_t *e, const hc_lake_cfg_t *c, int32_t x,
                        int32_t y, int32_t z);
int hc_featx_rootsys_place(feat_env_t *e, const hc_rootsys_cfg_t *c,
                           int32_t x, int32_t y, int32_t z);
int hc_featx_geode_place(feat_env_t *e, const hc_geode_cfg_t *c, int32_t x,
                         int32_t y, int32_t z);
int hc_featx_kelp_place(feat_env_t *e, int32_t x, int32_t y, int32_t z);

/* features_lush.c — R3 본문 */
int hc_featx_vpatch_place(feat_env_t *e, const hc_vpatch_cfg_t *c, int32_t x,
                          int32_t y, int32_t z);
int hc_featx_bcol_place(feat_env_t *e, const hc_bcol_cfg_t *c, int32_t x,
                        int32_t y, int32_t z);
int hc_featx_mface_place(feat_env_t *e, const hc_mface_cfg_t *c, int32_t x,
                         int32_t y, int32_t z);

/* ================= Java HashSet<BlockPos> 순회 에뮬레이션 =================
 *
 * 왜: TreeDecorator$Context 는 HashSet 을 리스트로 복사 후 getY 안정
 * 정렬(같은 y 는 HashSet 순회 순서), updateLeaves 의 레벨 셋 poll,
 * vegetation_patch 의 ground/water 셋 순회가 전부 HashSet 순서를 따르고
 * 데코레이터/초목 드로우가 그 순서를 소비한다 (R2 §12, R3 §3).
 *
 * JDK 25 HashMap 시맨틱 (jset.c — 본 머신의 java.util.HashMap$TreeNode
 * javap 로 검증): cap 16 시작(0.75), index = (cap-1) & spread,
 * spread = h ^ h>>>16 (노드가 spread 를 저장), 체인 꼬리 삽입, 삽입 후
 * size > threshold 면 리사이즈(버킷을 lo/hi 로 상대순서 보존 분할), 한
 * 버킷이 9개가 되는 삽입에서 cap<64 → 리사이즈 1회 / cap>=64 → TREEIFY
 * (레드블랙 트리 빈 — moveRootToFront 가 순회 순서를 바꾼다; putTreeVal
 * 삽입은 트리 부모 뒤에 링크). 순회 = 버킷 오름차순, 체인 head→tail.
 * BlockPos.hashCode = (y + z*31)*31 + x (Vec3i). 동일 hashCode 키의
 * 트리 순서는 JVM identityHashCode(재현 불가) — 도달 시 즉사. */

enum { HC_JSET_MAX_ENTRIES = 8192, HC_JSET_MAX_CAP = 16384 };

typedef struct {
    int32_t  x, y, z;
    uint32_t hash; /* JDK spread(hashCode) — 노드 저장값과 동일 */
    int32_t  next, prev;          /* 체인 링크 (-1 끝) */
    int32_t  parent, left, right; /* 트리 빈 전용 (-1 없음) */
    uint8_t  red;
    uint8_t  dead; /* remove 후 1 (엔트리 배열은 재사용 안 함) */
} hc_jent_t;

typedef struct {
    hc_jent_t ent[HC_JSET_MAX_ENTRIES];
    int32_t   n_ent;
    int32_t   bucket[HC_JSET_MAX_CAP]; /* head 엔트리 인덱스, -1 빔 */
    uint8_t   tree[HC_JSET_MAX_CAP];   /* 트리화된 빈 */
    int32_t   cap;                     /* 2^k */
    int32_t   size, threshold;
} hc_jset_t;

typedef struct {
    const hc_jset_t *s;
    int32_t          b, e;
} hc_jit_t;

/* isFaceSturdy(SupportType.FULL) — 팔레트 축약: 완전 큐브는 전 면; azalea/
 * flowering_azalea 는 UP 면만 (SHAPE = or(column(16, 8..16), column(4,
 * 0..8)) — 상부가 완전 16x16 슬랩, 본 세션 javap; R1 §7 열린 항목 확정.
 * vegetation_patch 의 floor 판정이 이 면을 실제로 조회한다). 잎은 지지
 * 형상 EMPTY 라 제외 (R1 §2.5). dir_mc: 0=DOWN,1=UP,2=N,3=S,4=W,5=E. */
static inline int hc_featx_face_sturdy_full(uint16_t s, int dir_mc) {
    if (hc_block_is_full_cube(s))
        return 1;
    return (s == HC_B_AZALEA || s == HC_B_FLOWERING_AZALEA) && dir_mc == 1;
}

void hc_jset_init(hc_jset_t *s);
/* HashSet.add — 이미 있으면 false (순서 불변) */
int  hc_jset_add(hc_jset_t *s, int32_t x, int32_t y, int32_t z);
/* 순회 위치 좌표 (Task 14 — BE 직렬화 순서 재구성) */
int  hc_jit_pos(const hc_jit_t *it, int32_t *x, int32_t *y, int32_t *z);
/* 첫 원소 꺼내 제거 (HashIterator.next+remove — movable=false) */
int  hc_jset_poll_first(hc_jset_t *s, int32_t *x, int32_t *y, int32_t *z);
/* 순회: 버킷 오름차순 → 체인 순 */
void hc_jit_begin(hc_jit_t *it, const hc_jset_t *s);
int  hc_jit_valid(const hc_jit_t *it);
void hc_jit_next(hc_jit_t *it);

#endif /* HC_FEATURES_INTERNAL_H */
