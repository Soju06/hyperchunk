# task7-surface — archived notes (2026-07-30)

Task 7 (05_surface 스테이지): MC 26.2 비난독화 서버 바이트코드를 javap로 1:1 재구성한 노트 A1~A6 (SurfaceSystem 본체, SurfaceRules$Context와 lazy condition 기계, JSON-facing 조건/룰 소스, RandomState 시딩·와이어링, BiomeManager 줌·바이옴 온도) — 바닐라 소스 추측 없이 C 포팅의 근거를 확보하기 위한 작업.
결과: 16/16 ctest, 9/9 chunks 0 diff (55,779 golden lines, blocks+heightmaps), check_no_fma PASS, ASan+UBSan full-suite PASS. 적대 리뷰로 exact JDK Math.round 비트 알고리즘, block_state_id 1-byte 스택 OOB, biome #tags fail-loud 등을 수정.
Mutation probe 매트릭스로 골든 게이트의 blind 분기(badlands, exposed water, steep 등)를 실측 문서화하고 Task 8 (carvers) 인계 사항을 정리했다.
관련 커밋: 7d22bda..530b52b (2026-07-30)
