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

# 노이즈 스택 (Task 6a). 한 바이너리, 모드별 golden 대조.
add_executable(test_noise unit/test_noise.c)
target_link_libraries(test_noise PRIVATE hyperchunk)
add_test(NAME noise_fork COMMAND test_noise fork
         ${CMAKE_SOURCE_DIR}/golden/rng/fork_seed1234567890.txt)
add_test(NAME noise_octaves COMMAND test_noise octaves
         ${CMAKE_SOURCE_DIR}/golden/rng/octaves_seed1234567890.txt)
