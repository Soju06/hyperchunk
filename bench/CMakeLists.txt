# P2-0 벤치 하네스. hyperchunk_verify 와 동일 파이프라인 시맨틱 +
# clock_gettime 계측. 코어 라이브러리는 손대지 않는다.
add_executable(hyperchunk_bench hyperchunk_bench.c)
set_target_properties(hyperchunk_bench PROPERTIES OUTPUT_NAME
                                                  hyperchunk-bench)
target_link_libraries(hyperchunk_bench PRIVATE hyperchunk pthread)
