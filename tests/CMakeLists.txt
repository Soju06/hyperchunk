# 단위/패리티 테스트. golden 텍스트는 런타임에 파싱한다 — 재생성 금지.
add_executable(test_rng unit/test_rng.c)
target_link_libraries(test_rng PRIVATE hyperchunk)
add_test(NAME rng_golden COMMAND test_rng ${CMAKE_SOURCE_DIR}/golden/rng)
