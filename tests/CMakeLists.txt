# 단위/패리티 테스트. golden 텍스트는 런타임에 파싱한다 — 재생성 금지.
add_executable(test_rng unit/test_rng.c)
target_link_libraries(test_rng PRIVATE hyperchunk)
add_test(NAME rng_golden COMMAND test_rng ${CMAKE_SOURCE_DIR}/golden/rng)

add_executable(test_arena unit/test_arena.c)
target_link_libraries(test_arena PRIVATE hyperchunk)
add_test(NAME arena COMMAND test_arena)

add_executable(test_chunk unit/test_chunk.c)
target_link_libraries(test_chunk PRIVATE hyperchunk)
add_test(NAME chunk COMMAND test_chunk)

add_executable(test_perlin unit/test_perlin.c)
target_link_libraries(test_perlin PRIVATE hyperchunk)
add_test(NAME perlin_golden COMMAND test_perlin
         ${CMAKE_SOURCE_DIR}/golden/rng/perlin_seed1234567890.txt)

add_executable(test_df unit/test_df.c)
target_link_libraries(test_df PRIVATE hyperchunk)
add_test(NAME df_eval COMMAND test_df)

add_executable(test_md5 unit/test_md5.c)
target_link_libraries(test_md5 PRIVATE hyperchunk)
add_test(NAME md5 COMMAND test_md5)

add_executable(test_json unit/test_json.c)
target_link_libraries(test_json PRIVATE hyperchunk)
add_test(NAME json COMMAND test_json)

# NBT 라이터 (Task 12). HashMap 순서 에뮬레이터는 golden r.0.0.mca 전
# 청크 실측 키 순서로 검증한다 (실측표는 테스트에 내장).
add_executable(test_nbt unit/test_nbt.c)
target_link_libraries(test_nbt PRIVATE hyperchunk)
add_test(NAME nbt COMMAND test_nbt)

# NBT 리더 왕복 (Task 14) — 골든 starts 프래그먼트 파스→재방출 항등.
add_executable(test_nbt_read unit/test_nbt_read.c)
target_link_libraries(test_nbt_read PRIVATE hyperchunk)
add_test(NAME nbt_read COMMAND test_nbt_read
    ${CMAKE_SOURCE_DIR}/golden/structures/c.20.1.starts.nbt
    ${CMAKE_SOURCE_DIR}/golden/structures/c.9.5.starts.nbt
    ${CMAKE_SOURCE_DIR}/golden/structures/c.22.6.starts.nbt
    ${CMAKE_SOURCE_DIR}/golden/structures/c.16.14.starts.nbt)

# 노이즈 스택 (Task 6a). 한 바이너리, 모드별 golden 대조.
add_executable(test_noise unit/test_noise.c)
target_link_libraries(test_noise PRIVATE hyperchunk)
add_test(NAME noise_fork COMMAND test_noise fork
         ${CMAKE_SOURCE_DIR}/golden/rng/fork_seed1234567890.txt)
add_test(NAME noise_octaves COMMAND test_noise octaves
         ${CMAKE_SOURCE_DIR}/golden/rng/octaves_seed1234567890.txt)
add_test(NAME noise_blended COMMAND test_noise blended
         ${CMAKE_SOURCE_DIR}/golden/rng/router_seed1234567890.txt)
add_test(NAME noise_normal COMMAND test_noise normal
         ${CMAKE_SOURCE_DIR}/golden/rng/octaves_seed1234567890.txt)

# 바이옴 줌 (Task 7). obfuscateSeed + BiomeManager.getBiome 좌표 선택을
# surface_seed golden 의 12,288 zoomed_biomes 벡터와 대조.
add_executable(test_biome_zoom unit/test_biome_zoom.c)
target_link_libraries(test_biome_zoom PRIVATE hyperchunk)
add_test(NAME biome_zoom COMMAND test_biome_zoom
         ${CMAKE_SOURCE_DIR}/golden/rng/surface_seed1234567890.txt)

# SurfaceSystem 구성요소 (Task 7). 16 노이즈 + clay bands + surfaceDepth
# 를 surface_seed golden 과 비트 단위 대조.
add_executable(test_surface_unit unit/test_surface_unit.c)
target_link_libraries(test_surface_unit PRIVATE hyperchunk)
add_test(NAME surface_unit COMMAND test_surface_unit
         ${CMAKE_SOURCE_DIR}/reference
         ${CMAKE_SOURCE_DIR}/golden/rng/surface_seed1234567890.txt)

# Beardifier compute 패리티 (Task 14). BeardProbe.java 실서버 그리드
# 덤프와 비트 단위 대조 (커널/fastInvSqrt/adjustment 4경로/합산 순서).
add_executable(test_beard_math unit/test_beard_math.c)
target_link_libraries(test_beard_math PRIVATE hyperchunk)
add_test(NAME beard_math COMMAND test_beard_math
         ${CMAKE_SOURCE_DIR}/golden/rng/beard_compute.txt)

# 라우터 슬롯 패리티 (Task 6b). reference JSON → IR 컴파일 → golden 대조.
add_executable(test_router_slots parity/test_router_slots.c)
target_link_libraries(test_router_slots PRIVATE hyperchunk)
add_test(NAME router_slots COMMAND test_router_slots
         ${CMAKE_SOURCE_DIR}/golden/rng/router_seed1234567890.txt
         ${CMAKE_SOURCE_DIR}/reference)

# 라이브-콘 기계 등가 (P2-2). 콘/컷/분할 경로를 프리픽스 워크와 비트
# 대조 — 창-밖 컬럼·음수 좌표·재방문 시퀀스 포함 (테스트 헤더 주석 참조).
add_executable(test_df_cones parity/test_df_cones.c)
target_link_libraries(test_df_cones PRIVATE hyperchunk)
add_test(NAME df_cones COMMAND test_df_cones ${CMAKE_SOURCE_DIR}/reference)

# 노이즈 스테이지 청크 패리티 (Task 6c). 셀 시맨틱 + aquifer + veins +
# doFill → golden 스테이지 덤프와 라인 단위 0 diff.
add_executable(test_noise_stage parity/test_noise_stage.c)
target_link_libraries(test_noise_stage PRIVATE hyperchunk)
add_test(NAME noise_stage COMMAND test_noise_stage
         ${CMAKE_SOURCE_DIR}/reference
         ${CMAKE_SOURCE_DIR}/golden/stages/seed1234567890
         1234567890)

# surface 스테이지 청크 패리티 (Task 7). 우리 04_noise 상태에서 출발
# (스테이지 체이닝) + 골든 바이옴 입력 → 05_surface 와 0 diff.
add_executable(test_surface_stage parity/test_surface_stage.c)
target_link_libraries(test_surface_stage PRIVATE hyperchunk)
add_test(NAME surface_stage COMMAND test_surface_stage
         ${CMAKE_SOURCE_DIR}/reference
         ${CMAKE_SOURCE_DIR}/golden/stages/seed1234567890
         ${CMAKE_SOURCE_DIR}/golden/rng/surface_seed1234567890.txt
         1234567890)

add_executable(test_carvers_unit unit/test_carvers_unit.c)
target_link_libraries(test_carvers_unit hyperchunk m)
add_test(NAME carvers_unit COMMAND test_carvers_unit
         ${CMAKE_SOURCE_DIR}/golden/rng/mth_sin_table.txt)

add_executable(test_carvers_stage parity/test_carvers_stage.c)
target_link_libraries(test_carvers_stage hyperchunk m)
add_test(NAME carvers_stage COMMAND test_carvers_stage
         ${CMAKE_SOURCE_DIR}/reference
         ${CMAKE_SOURCE_DIR}/golden/stages/seed1234567890
         ${CMAKE_SOURCE_DIR}/golden/rng/surface_seed1234567890.txt
         1234567890)

add_executable(test_features_rng unit/test_features_rng.c)
target_link_libraries(test_features_rng PRIVATE hyperchunk)
add_test(NAME features_rng COMMAND test_features_rng
         ${CMAKE_SOURCE_DIR}/golden/rng)

add_executable(test_jdk_log unit/test_jdk_log.c)
target_link_libraries(test_jdk_log PRIVATE hyperchunk)
add_test(NAME jdk_log COMMAND test_jdk_log
         ${CMAKE_SOURCE_DIR}/golden/rng/jdk_log.txt)

add_executable(test_features_walk parity/test_features_walk.c)
target_link_libraries(test_features_walk PRIVATE hyperchunk)
add_test(NAME features_walk COMMAND test_features_walk
         ${CMAKE_SOURCE_DIR}/reference
         ${CMAKE_SOURCE_DIR}/golden/stages/seed1234567890
         ${CMAKE_SOURCE_DIR}/golden/stages-alt/seed1234567890
         ${CMAKE_SOURCE_DIR}/golden/features-trace/seed1234567890
         ${CMAKE_SOURCE_DIR}/golden/rng/surface_seed1234567890.txt
         1234567890)

# 라이트 스테이지 패리티 (Task 10). 08/09 덤프 18개 게이트 — 11x11 월드
# 체인(노이즈 지배적)이라 무겁다.
add_executable(test_light_stages parity/test_light_stages.c)
target_link_libraries(test_light_stages PRIVATE hyperchunk)
add_test(NAME light_stages COMMAND test_light_stages
         ${CMAKE_SOURCE_DIR}/reference
         ${CMAKE_SOURCE_DIR}/golden/stages/seed1234567890
         ${CMAKE_SOURCE_DIR}/golden/stages-alt/seed1234567890
         ${CMAKE_SOURCE_DIR}/golden/features-trace/seed1234567890
         ${CMAKE_SOURCE_DIR}/golden/rng/surface_seed1234567890.txt
         ${CMAKE_SOURCE_DIR}/golden/rng/biome_band_seed1234567890.txt
         1234567890)
set_tests_properties(light_stages PROPERTIES TIMEOUT 1200)

# 10_spawn/11_full 패리티 (Task 11). 36 덤프 게이트 (9청크 x 2스테이지 x
# 2번들) — 라이트 없는 order-replay, 09 잔차 캡 이월.
add_executable(test_spawn_full parity/test_spawn_full.c)
target_link_libraries(test_spawn_full PRIVATE hyperchunk)
add_test(NAME spawn_full COMMAND test_spawn_full
         ${CMAKE_SOURCE_DIR}/reference
         ${CMAKE_SOURCE_DIR}/golden/stages/seed1234567890
         ${CMAKE_SOURCE_DIR}/golden/stages-alt/seed1234567890
         ${CMAKE_SOURCE_DIR}/golden/features-trace/seed1234567890
         ${CMAKE_SOURCE_DIR}/golden/rng/surface_seed1234567890.txt
         ${CMAKE_SOURCE_DIR}/golden/rng/biome_band_seed1234567890.txt
         1234567890)
set_tests_properties(spawn_full PROPERTIES TIMEOUT 1200)

# 리전 출력 패리티 (Task 12). primary 번들 풀 재생 → r.0.0 ∩ 그리드
# 4청크 직렬화. (0,0) 은 golden canonical 페이로드와 무조건 바이트 일치
# (참조는 tools/golden/extract_region_ref.py 산출물 — .mca 처럼 로컬
# 전용, 해시는 golden/SHA256SUMS). 나머지 3청크는 stale-mca 잔차
# (07-28 .mca vs 07-31 번들, test_region.c 헤더 주석) — roundtrip 이
# 컨테이너 왕복을, residuals 가 문서화된 잔차 봉투를 강제한다.
add_executable(test_region parity/test_region.c)
target_link_libraries(test_region PRIVATE hyperchunk)
add_test(NAME region_out COMMAND test_region
         ${CMAKE_SOURCE_DIR}/reference
         ${CMAKE_SOURCE_DIR}/golden/stages/seed1234567890
         ${CMAKE_SOURCE_DIR}/golden/features-trace/seed1234567890
         ${CMAKE_SOURCE_DIR}/golden/rng/biome_band_seed1234567890.txt
         1234567890
         ${CMAKE_SOURCE_DIR}/golden/region-ref
         ${CMAKE_BINARY_DIR}/region_out_r.0.0.mca)
set_tests_properties(region_out PROPERTIES TIMEOUT 1200
                     FIXTURES_SETUP region_mca)
add_test(NAME region_out_roundtrip COMMAND python3
         ${CMAKE_SOURCE_DIR}/tools/golden/check_region_roundtrip.py
         ${CMAKE_BINARY_DIR}/region_out_r.0.0.mca
         ${CMAKE_SOURCE_DIR}/golden/seed1234567890_r.0.0.mca)
set_tests_properties(region_out_roundtrip PROPERTIES
                     FIXTURES_REQUIRED region_mca)
add_test(NAME region_out_residuals COMMAND python3
         ${CMAKE_SOURCE_DIR}/tools/golden/check_region_residuals.py
         ${CMAKE_BINARY_DIR}/region_out_r.0.0.mca
         ${CMAKE_SOURCE_DIR}/golden/seed1234567890_r.0.0.mca)
set_tests_properties(region_out_residuals PROPERTIES
                     FIXTURES_REQUIRED region_mca)

# 풀 리전 canonical 게이트 (Task 14). 41x41 월드 → 리전 전체 manifest
# 재생 → postprocess 승격 드레인 → stages.log R/S 라이트 고정점 →
# 1024청크 직렬화가 golden canonical 해시 (golden/SHA256SUMS
# #canonical-payload) 와 byte-exact. 참조 페이로드/와이드 밴드는 로컬
# 추출물 (extract_region_ref.py / make_biome_band_region.sh).
add_executable(test_full_region parity/test_full_region.c)
target_link_libraries(test_full_region PRIVATE hyperchunk pthread)
add_test(NAME full_region COMMAND test_full_region
         ${CMAKE_SOURCE_DIR}/reference
         ${CMAKE_SOURCE_DIR}/golden/stages/seed1234567890
         ${CMAKE_SOURCE_DIR}/golden/rng/biome_band_region_seed1234567890.txt
         1234567890
         ${CMAKE_SOURCE_DIR}/golden/region-ref
         a59632059bab42d808a24c27091d4b8786b5615405263a84ec1ba9a193223c24
         ${CMAKE_BINARY_DIR}/full_region_r.0.0.mca)
set_tests_properties(full_region PROPERTIES TIMEOUT 5400)
