# task14-fullregion — archived notes (2026-08-06)

Task 14: r.0.0 풀 리전 1024청크 페이로드를 바닐라 26.2와 byte-exact로 맞추는 canonical 게이트 작업. 구조물 배치(mineshaft/dungeon/템플릿)·직렬화 순서(CompoundTag HashMap·fastutil 순회)·블록 프로퍼티 실측 등록을 디컴파일 핀 근거로 조사한 R-노트 5편과 골든 덤프가 기반.
블록 잔차를 8,934셀·455청크 → 131셀·211청크 → 0셀·0청크로 12개 잔차 클래스(버섯 canSurvive 라이트 스냅샷, 마인샤프트 행잉 CENTER 지지, 펜스 연결 폐포, BE 저장 순서 충돌쌍 반전, SkyLight all-15 섹션 등)를 전부 실측 소진.
결과: 풀 리전 canonical 게이트 PASS — canonical sha256 a59632059bab42d808a24c27091d4b8786b5615405263a84ec1ba9a193223c24, ctest 30/30 green (strict full_region 포함), check_no_fma PASS, parity_gate/hyperchunk-verify "PASS: bit-exact parity".
완료 노트에 커버리지 경계(repeatFirstLayer, cave_vines head↔body, kelp 등 fail-loud 경로)와 기각 가설도 기록.
관련 커밋: 5665182..82d0161 (2026-08-04~2026-08-06)
