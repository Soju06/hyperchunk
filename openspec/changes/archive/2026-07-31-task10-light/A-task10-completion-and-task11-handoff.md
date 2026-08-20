# Task 10 완료 상태 + Task 11 핸드오프 (2026-07-31, run 4)

## Task 10 결과 요약

- **08_initialize_light**: 풀 0-diff 게이트, 두 번들 18/18 GREEN.
- **09_light**: 12/18 덤프 0-diff (c.0.0 두 번들; full-window 로는
  c.-1.0·c.1.1 두 번들, alt c.0.1/c.-1.1/c.1.0 추가) + 6 덤프 측정
  잔차-캡 게이트. 잔차의 단일 근원 = 링 청크 step-9 초목이 기록 서버의
  리로드-복원(mid-carve 스냅샷) FINAL 하이트맵 기준선 위에서 굴러 위치가
  어긋난 것 (IMPL-notes.md §잔차 — 재현 불가능한 비동기-저장 레이스
  산물). 재활성: 골든 재기록 후 HC_LIGHT_STRICT=1.
- 라이트 엔진 자체 (light_engine.c): 12개 0-diff 덤프가 증거 — 등록/
  시딩/전파/댐프닝/방출 모두 관측 윈도 내 비트-정확. 스케줄 재현 필수
  (고정점만으로 불충분 — R6 §4, ALT 조기 덤프 = 비수렴 스냅샷).
- features 쪽 신규 시맨틱: melon(plain Block) canSurvive,
  noise_based_count, ***_WG 드롭/재프라임** (IMPL-notes.md — Task 11
  이후에도 유효한 월드젠 사실).

## Task 11 (10_spawn + 11_full) 핸드오프 — 실측 기반

`golden/{stages,stages-alt}` 전 18 청크덤프 실측 diff (헤더 `# stage`
라인 제외):

1. **09→10 (spawn): 변화 0.** blocks/heightmaps(값·kind 모두)/biomes
   전부 바이트 동일. 10/11 에는 light_* 덤프 자체가 없다.
   바닐라 spawn 스테이지 = NaturalSpawner.spawnForChunk (엔티티만,
   random_tick_speed=0/spawn_mobs=false 하네스 게임룰로 봉인) — 덤프
   관측면에는 순수 pass-through. **C 쪽 할 일: 없음** (스테이지 존재
   자체의 부기 외).

2. **10→11 (full): heightmaps kind 프루닝만.** 순수 삭제 diff:
   - c.0.0: WORLD_SURFACE_WG + OCEAN_FLOOR_WG 두 kind 삭제 (6→4).
   - 나머지 8청크: 이미 09 부터 WS_WG 부재 (리로드), 11 에서
     OCEAN_FLOOR_WG 삭제 (5→4).
   - 남는 4종 (WORLD_SURFACE/OCEAN_FLOOR/MOTION_BLOCKING/
     MOTION_BLOCKING_NO_LEAVES) 값은 10 과 바이트 동일.
   - blocks/biomes: 변화 0.
   근거 메커니즘: ProtoChunk→LevelChunk 전환이 클라이언트 4종만 유지
   (ChunkStatus.FINAL_HEIGHTMAPS — 이번 런에서 javap 로 확인한 그
   EnumSet). R6 §6: 11_full 덤프는 Server_thread 에서 실행; ALT 의
   `11_full -1 -1 17 18` 한 건만 torn (in-flight seq 17 = c.-3.-3,
   블록 diff 0 — 관측 무영향).
   **C 쪽 할 일: heightmap kind 집합 프루닝 + 스테이지 부기.**
   블록/바이옴/하이트맵 값 연산 없음.

3. **테스트 형태 제안**: test_light_stages 의 재생 인프라를 그대로 써서
   각 청크의 10/11 스냅샷 이벤트(order.snapshots 의 stage 10/11 행)에서
   blocks/heightmaps(존재 kind 만)/biomes 를 대조. 예상 잔차 = 09 와
   동일한 링-초목 클래스 (blocks 는 09 잔차가 그대로 이월 — 10/11 이
   블록 무변화이므로 캡도 동일하게 이월하면 된다. 신규 발산 없음이
   목표 불변량).

4. **경고**: 11_full 의 hm 4종은 "라이브" 가 아니라 리로드-복원 맵의
   최종 상태다. 09 게이트와 동일하게, 리로드 산물(OF_WG 재프라임과 달리
   FINAL 4종은 NBT 직렬화라 리로드로 안 끊김)이라 우리 라이브-갱신
   FINAL 맵과 그대로 대조 가능 — 실제로 10→11 값 diff 0 이 그 증거.

## 커버리지 경계 (light 게이트가 못 보는 것)

- 링 청크의 라이트 상태 자체 (그리드 9청크 광원·차폐만 관측).
- decrease/재전파 경로 (fresh solve 만 — 대치 이벤트 없음).
- shapeOccludes 실경로 (이 팔레트에 useShapeForLightOcclusion 상태
  부재 — die-list 가 지킴).
- 잔차-캡 6 덤프에서는 캡 이하의 회귀 (캡 초과만 fail).
