# task10-light — archived notes (2026-07-31)

Task 10(08_initialize_light + 09_light 라이트 스테이지) 구현을 위한 26.2 바이트코드 정찰(R1 오케스트레이션/스케줄링, R2 블록라이트 엔진, R3 스카이라이트/섹션 스토리지, R4 방출·차폐 테이블, R5a-d 링 피처 본체 geode/lake/root_system·azalea/monster_room, R6 골든 덤프 포렌식)과 구현/잔차 노트 묶음.
결과: 08은 풀 0-diff 게이트로 두 번들 18/18 GREEN, 09는 12/18 덤프 0-diff + 6 덤프 측정 잔차-캡 게이트(fail 3653→2917). 잔차의 단일 근원은 기록 서버의 비동기-저장 레이스로 리로드-복원된 mid-carve FINAL 하이트맵 기준선 위에서 링 청크 step-9 초목 위치가 어긋난 것 — 골든에서 재구성 불가 판정.
이 런의 신규 월드젠 시맨틱: melon(plain Block) canSurvive, noise_based_count 배치 모디파이어, *_WG 하이트맵 드롭/재프라임 모델. 라이트 재현에는 고정점만으로 불충분하고 스케줄 재현이 필수임을 확정.
Task 11 핸드오프 포함: 실측 diff로 09→10(spawn)은 변화 0, 10→11(full)은 heightmap kind 프루닝(6→4)만.
관련 커밋: e0abb60..24efb36 (2026-07-31)
