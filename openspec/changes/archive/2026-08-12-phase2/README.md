# phase2 — archived notes (2026-08-12)

Phase 2 성능 최적화 완료 노트 묶음 (P2-0 베이스라인 ~ P2-11 인터리브, 10건). P2-0에서 gen wall 198.0s(-O3, 20스레드)와 노이즈 지배(~97%, hc_df_eval_ex 전-프리픽스 평가 구조)를 실측한 뒤, 라이브-콘 평가(21.1x)·FTS y-분할(noise 1.254x)·FREE 셀-FIFO 스케줄러(2.37x)·lazy+AVX2(noise 2.97x)·SHA-NI(6.0x)·B-2/B-3 판정 GO 3건×2라운드·AVX-512 x8 백엔드(hc-e6 noise 1.176x)·2-웨이 콘-스트림 인터리브(커널 1.294x)를 순차 적용했다.
매 단계 canonical 패리티 게이트(a5963205…3c24) green을 유지하며 값-불변 최적화만 수행 — df_cones/df_x8/isa_equiv/sha_equiv/TSan 등 신규 게이트를 동반 신설했다.
결과: 로컬 FREE gen wall 198.0s → 1658.3 ms, hc-e6(EPYC 9J45 Zen5) 16T 919.4 ms.
관련 커밋: c823464..d01a8c2 (2026-08-06~2026-08-12)
