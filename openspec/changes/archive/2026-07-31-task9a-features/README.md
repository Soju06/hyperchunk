# task9a-features — archived notes (2026-07-31)

Task 9a: features(장식) 스테이지 정찰 + 1차 구현 기록 (A1~A7). 바닐라 26.2 골든 06→07 덤프를 블록 단위 전수 diff해 실제 발화한 피처를 규명하고(그리드 총 변경 블록 primary 55659 / alt 56323, 블록을 만든 스텝은 6 ORES와 9 VEGETAL뿐), placement 파이프라인·ORE 패밀리·springs/disks/magma·FeatureSorter 레지스트리 순서를 문서화했다.
구현은 features_rng(WorldgenRandom)·jdk_trig(HotSpot sin/cos 스텁, glibc와 21/4099 벡터 1-ulp 차이 봉합)·features_compile·features(ORE/SPRING/UNDERWATER_MAGMA/MONSTER_ROOM)·gen_features_stage(스텝 0..8 워크) + 트레이스 골든 하네스.
게이트 결과: 장식 시드 81/81 primary + 81/81 alt 일치, 블록 게이트 primary 42,579 / alt 42,843 배치·하드 미스매치 0. 잔차는 전부 step 9/10 몫으로 분류해 9b 작업 목록으로 인계.
관련 커밋: ac8fcb2..bddbb93 (2026-07-31)
