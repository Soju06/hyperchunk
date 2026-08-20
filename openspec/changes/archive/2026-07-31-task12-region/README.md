# task12-region — archived notes (2026-07-31)

Task 12 — 26.2 리전(.mca) 직렬화기 구축 노트 묶음. R-A~R-E 서베이(tools/golden·코어 C·SerializableChunkData write 경로·block/fluid_ticks·팔레트/라이트 방출)로 바닐라 저장 경로를 바이트코드 수준에서 핀 다운했다.
무의존성 NBT 라이터(HashMap cap16/LF .75 순회 에뮬레이션, golden 1024청크 키 순서 전수 실측)·청크 직렬화기(팔레트 재팩 24,576 섹션 항등)·틱 레코더·stored-block zlib .mca 조립기를 구현했다.
게이트 결과: c.0.0 100081 바이트 완전 일치(block_ticks 681 시간순 포함), 나머지 3청크는 문서화된 stale-mca 잔차 봉투 이내. golden .mca가 order.manifest 이전 별개 런의 산물임을 규명하고, Task 13 풀-리전 게이트의 선행 조건으로 골든 재캡처를 핸드오프했다.
관련 커밋: ec7e23d (2026-07-31)
