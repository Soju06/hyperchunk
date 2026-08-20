# phase1-vertical-slice — archived plan (2026-07-28)

Phase 1 수직 슬라이스 계획서. 순수 C로 5스테이지 월드젠을 최소 구현하고 단일 청크의 region 출력이 바닐라 Java와 sha256 단위로 일치함을 증명하는 것이 목표였다 (성능은 비목표, 패리티가 유일한 사망 원인이라는 전제).
스테이지별 golden 대조 전략(RNG → noise → surface → carvers → features → lighting → region)과 Task 0~13 단계 계획, 리스크(R1 golden 스테이지 덤프 확보, R2 features 병렬 패리티 등)를 정의했다.
ADR-001~005로 아키텍처를 잠근 커밋에 포함됐고, 이튿날 ADR-006으로 대상을 MC 26.2(비난독화, Java 25, FFM)로 리타깃해 계획에 반영했다.
관련 커밋: 3cc7771..0d1c267 (2026-07-28)
