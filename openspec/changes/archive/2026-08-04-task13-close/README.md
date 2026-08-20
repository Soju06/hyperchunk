# task13-close — archived notes (2026-08-04)

Task 13-close 완료 노트 — Phase 1 종결: unified 재캡처 골든(recapture-3, gameTime 28) 확정 + strict 대조 기본 승격 + postProcessGeneration 구현.
결과: 풀 스위트 26/26 green(strict가 코드 기본값), region 게이트 4/4 byte-exact(c.0.0/c.1.0/c.0.1/c.1.1 페이로드 완전 일치), 라이트 09 미드 스냅샷 36/36 중 35 덤프 0-diff + 1건은 레코딩 엔진의 크로스보더 스카이 유입 과도상태로 골든 아티팩트 판정(캡 고정 문서화 잔차), check_no_fma.sh PASS.
light 09 모델 v3(stages.log v2 제출 라인 기반 배치 재생)로 구모델 34/36 → 35/36 full 0-diff로 개선했고, postProcessGeneration(물/모래 패스)이 수용 기준이던 c.1.0 물 스프레드 1셀 잔차를 strict 하에서 byte-exact로 닫았다.
관련 커밋: a5e5c4f (2026-08-04)
