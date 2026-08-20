# task9pre-order — archived notes (2026-07-31)

Task 9(features 스테이지 C 재현)의 사전 정찰: ADR-007 Tier-2 리플레이에 필요한 features 순서 기록(order.manifest)을 설계하기 위해 MC 26.2 서버 바이트코드를 javap로 1:1 해부한 노트 묶음(A1 generateFeatures 태스크, A2 applyBiomeDecoration, A3 WorldgenRandom 시딩, A4 WorldGenRegion 쓰기 윈도우, A5 스케줄러 순서, A6 훅 API 표면).
A0 종합 결론: 한 디멘션의 모든 스텝 바디가 단일 ConsecutiveExecutor("worldgen")로 직렬화되므로 청크당 전순서(per-chunk total order)가 최소·충분 기록이며, 훅은 `WorldgenRandom#setDecorationSeed@RETURN`을 `ChunkStatusTasks#generateFeatures` HEAD로 arm하는 방식으로 확정.
A7 충분성 프로브: 커밋된 primary/alt 골든 번들 대조에서 81 applications, 0 seed mismatches, 07 invariant 유지 — "PROBE PASSED: bundles coherent with per-chunk-order-only variation".
관련 커밋: 6c51f1e..4efc22a (2026-07-31)
