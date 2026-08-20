# task9b-features — archived notes (2026-07-31)

Task 9b: features 스테이지 잔여 피처(나무 jungle/oak straight·mega·fancy, bamboo/simple_block/vines, glow_lichen multiface_growth, cave_vines·dripleaf block_column, moss/clay vegetation_patch, freeze_top_layer)를 MC 26.2 바이트코드 recon(R1~R5, javap 검증)에 근거해 구현하고 게이트로 봉인한 기록.
결과: 07_features blocks + 6 heightmaps 양쪽 번들 0 diff(리플레이당 55,658 / 56,322 blocks placed), 데코 시드 81/81 primary + 81/81 alt 재계산 일치, 9개 그리드 청크 steps 0..10 trace line-exact, 20/20 ctest·sanitizer PASS.
"돌아간다"와 "0 diff"를 가른 버그 3건: java.util.HashMap treeify 오프바이원(jset에 TreeNode 기계 전체 포팅), updateShapeAtEdge는 no-op이 아님(그리드 팬텀 덩굴 13/17의 원인), azalea UP면 isFaceSturdy(FULL) 면-의존성.
A8은 Task 10(light) 핸드오프(팔레트 광원 상수, FINAL heightmap 4종 승계, HC_TREE_DEBUG/HC_VPATCH_DEBUG 디버그 도구)를 포함한다.
관련 커밋: dfc002e..446b8ce (2026-07-31)
