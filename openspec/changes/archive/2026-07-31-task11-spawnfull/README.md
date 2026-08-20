# task11-spawnfull — archived notes (2026-07-31)

Task 11 (10_spawn/11_full 스테이지) 완료 노트 + Task 12 핸드오프. 바이트코드 확인(javap)으로 SPAWN은 mob 부기만(블록/하이트맵/바이옴/라이트 쓰기 제로), FULL은 ProtoChunk→LevelChunk 전환에서 FINAL_HEIGHTMAPS 4종만 setRawData 비트 복사하고 *_WG는 조용히 소멸함을 확정, C 쪽은 promoted 마커(10/11)로 구현했다.
게이트(test_spawn_full)는 36 덤프 대조에서 번들당 12/18 0-diff + 6 덤프가 09 캡과 정확히 같은 잔차(10과 11이 청크별 동일 카운트 = 순수 이월), biomes 전부 0, 신규 발산 0.
Task 12(리전 출력)용으로 golden r.0.0.mca 실측 인벤토리를 남겼다: canonical payload 해시 게이트 정의, 1024/1024 청크, 15키 NBT 방출 순서, DataVersion 4903, 그리드 4청크 block_ticks 681/817/419/558 엔트리(잎 스케줄-틱) 발견 등.
관련 커밋: 81457c2..4bebe8e (2026-07-31)
