/* B-3 연산 카운터 (HC_BENCH_COUNTERS) — 기본 off, 값 경로 무접촉.
 *
 * 하한 모델(B-3)의 work-floor 실측용: 스테이지별 필수 연산의 실행 횟수만
 * 센다. 값을 만들거나 바꾸는 코드가 아니며, off(기본) 상태의 비용은
 * 사이트당 전역 int 로드 + not-taken 분기 하나다 (B-2 HC_BENCH_TIMELINE
 * 과 같은 검증 프로토콜: on/off 3런 중앙값 비교 + 매 런 canonical PASS).
 *
 * 스레딩 모델: 증가는 _Thread_local (원자 없음), hc_ctr_flush() 가 TLS 를
 * 전역 relaxed 원자 합으로 옮긴다. hc_ctr_enable() 은 워커 스폰 전 메인
 * 에서만 호출한다 (pthread_create HB 로 hc_ctr_on 전파 — TSan 클린).
 * TLS 예산: HC_CTR_N × 8B ≈ 0.3KB (PT_TLS 8.2MB 마진 무관 수준). */
#ifndef HC_COUNTERS_H
#define HC_COUNTERS_H

#include <stdint.h>

enum {
    /* 바이옴 줌 (biome_zoom.c) — 미스당 lcg 정확히 64회 (8콘너×8),
     * 쿼리당 잔여 산술은 코드 구조 고정 (8콘너 × 5add+3mul+1cmp) */
    HC_CTR_ZOOM_Q = 0,   /* hc_biome_zoom 쿼리 */
    HC_CTR_ZOOM_MISS,    /* 셀-캐시 미스 (풀-소진 폴백 포함) */

    /* surface 룰 트리 (surface.c) */
    HC_CTR_SURF_COND,       /* cond_test 진입 (재귀 포함) */
    HC_CTR_SURF_APPLY,      /* rule_apply 진입 (재귀 포함) */
    HC_CTR_SURF_ROOT,       /* 07 루트 tryApply (defaultBlock 셀) */
    HC_CTR_SURF_TOPMAT,     /* 08 카버 topMaterial 루트 (교차-스테이지 분리) */
    HC_CTR_SURF_YITER,      /* 컬럼 y-루프 트립 합 (h2..MIN_Y) */
    HC_CTR_SURF_SAMP_MISS,  /* 샘플러 memo 미스 = normal-noise 실평가 */
    HC_CTR_SURF_SEC_MISS,   /* surface_secondary memo 미스 */
    HC_CTR_SURF_MSL_MISS,   /* min_surface_level 재계산 */
    HC_CTR_SURF_STEEP_MISS, /* steep 재계산 */

    /* 04 noise — x4 커널 (df_simd_avx2.c) + 스칼라 프로그램 (df_eval.c) */
    HC_CTR_X4_SLICE,     /* fill_slice x4 스트림 호출 (레인=y 4점) */
    HC_CTR_X4_CELL,      /* select_cell x4 스트림 호출 (레인=z 4점) */
    HC_CTR_X4_NODE,      /* x4_run 플레인 노드 실행 (1회 = 4레인) */
    HC_CTR_X4_PERLIN,    /* perlin_x4 호출 (4레인 옥타브 샘플) */
    HC_CTR_X4_SCALAR_FB, /* perlin_x4 포화 폴백 (26.2 오버월드 0 예상) */
    HC_CTR_X4_RC_MIX,    /* RC 세그먼트 혼합-레인 (0<m<0xF): 죽은-레인 작업 */
    HC_CTR_X4_IS_MIX,    /* IS 세그먼트 레인별 sel 상이 */
    HC_CTR_X4_BLEND_MIX, /* blended over/under 혼합-레인 뱅크 계산 */
    HC_CTR_SP_NODE,      /* 스칼라 prog_run 노드 실행 */
    HC_CTR_FTS_ITER,     /* FindTopSurface 사다리 반복 (y-가변 콘 워크) */

    /* 04 noise — 스칼라 단일점 DF 평가, 콜사이트 클래스별 (aquifer.c /
     * ore_veins.c / noise_chunk.c). 노드 수는 클래스별 콘 len 으로 유도. */
    HC_CTR_DF_AQ_EROSION,
    HC_CTR_DF_AQ_DEPTH,
    HC_CTR_DF_AQ_FLOOD,
    HC_CTR_DF_AQ_SPREAD,
    HC_CTR_DF_AQ_LAVA,
    HC_CTR_DF_AQ_BARRIER,
    HC_CTR_AQ_FS_MISS,   /* 유체상태 그리드-셀 캐시 미스 (compute_fluid) */
    HC_CTR_AQ_SOLID,     /* substance density>0 조기 탈출 (고체 경로) */
    HC_CTR_DF_VEIN_TOGGLE,
    HC_CTR_DF_VEIN_RIDGED,
    HC_CTR_VEIN_PRE_GAP, /* gap 평가 도달 상한 (richness && 단락 — 상한만) */
    HC_CTR_DF_PSL_MISS,  /* preliminarySurfaceLevel memo 미스 */

    /* light 누적 엔진 (light_engine.c) — 큐 인덱스 읽기라 핫패스 비용 0 */
    HC_CTR_L_SKY_SEED, /* l09 sky 시드 push */
    HC_CTR_L_SKY_POP,  /* l09 sky BFS 총 엔트리 (시드 포함) */
    HC_CTR_L_BLK_SEED,
    HC_CTR_L_BLK_POP,
    HC_CTR_L_FLUSH_DEC,  /* flush 감소 큐 드레인 엔트리 */
    HC_CTR_L_FLUSH_INC,  /* flush 증가 큐 드레인 엔트리 */
    HC_CTR_L_CHECK,      /* flush 시점 checkBlock 펜딩 수 (기존 n_check) */
    HC_CTR_L_DIRTY_CH,   /* flush 시점 재유도 대상 청크 수 (기존 n_dirty) */
    HC_CTR_L_DERIVE_08,  /* l08 init 재유도 */
    HC_CTR_L_DERIVE_FL,  /* flush 재유도 */
    HC_CTR_L_PREP_SKIP,  /* prepare dirty-skip 된 청크 */
    HC_CTR_L_PREP_SCAN,  /* prepare 실재유도 청크 */

    /* 09 deco/구조물 (features.c / gen_features_stage.c / structures.c) */
    HC_CTR_FEAT_SETBLK,  /* set_block 펀넬 (일반+ks, 창 가드 통과분) */
    HC_CTR_FEAT_ATTEMPT, /* (청크, step, feature) 배치 시도 (idxset 통과) */
    HC_CTR_STRUCT_STEP,  /* structures_step 호출 (청크×step) */
    HC_CTR_STRUCT_SCAN,  /* references/beard/step 스캔 스텝 (start 검사 수)
                          * — P2-9 GO-1 전 26×289×n_starts 삼중 루프 트립,
                          * 후 ord[] 순회 트립 (HOT: 전 형상이 ~2.5G/런) */

    HC_CTR_N
};

extern int                    hc_ctr_on;
extern _Thread_local uint64_t hc_ctr_tls[HC_CTR_N];

#define HC_CTR_INC(ev)                                                       \
    do {                                                                     \
        if (hc_ctr_on)                                                       \
            hc_ctr_tls[ev]++;                                                \
    } while (0)
/* 최핫 사이트 전용 (x4_run 노드 루프 ~430M, cond_test/rule_apply 진입
 * ~625M/런): off 상태의 로드+분기조차 실측된 비용이라 (noise CPU-sum
 * +5.6%, surface +7% — B-3 실측) 컴파일 타임 게이트를 겹친다.
 * 기본 빌드는 코드 부재 (비용 0); 카운트 수집은 로컬
 * `cmake -DHC_CTR_HOT=ON` 빌드로만 한다 (커밋 빌드 캐시에 남기지 말 것). */
#ifdef HC_CTR_HOT
#define HC_CTR_INC_HOT(ev) HC_CTR_INC(ev)
#else
#define HC_CTR_INC_HOT(ev) ((void)0)
#endif
#define HC_CTR_ADD(ev, n)                                                    \
    do {                                                                     \
        if (hc_ctr_on)                                                       \
            hc_ctr_tls[ev] += (uint64_t)(n);                                 \
    } while (0)

void        hc_ctr_enable(void); /* 워커 스폰 전 메인에서만 */
void        hc_ctr_flush(void);  /* TLS → 전역 relaxed 합, TLS 클리어 */
uint64_t    hc_ctr_total(int ev);
const char *hc_ctr_name(int ev);

#endif
