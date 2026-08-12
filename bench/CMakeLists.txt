# P2-0 벤치 하네스. hyperchunk_verify 와 동일 파이프라인 시맨틱 +
# clock_gettime 계측. 코어 라이브러리는 손대지 않는다.
add_executable(hyperchunk_bench hyperchunk_bench.c)
set_target_properties(hyperchunk_bench PROPERTIES OUTPUT_NAME
                                                  hyperchunk-bench)
target_link_libraries(hyperchunk_bench PRIVATE hyperchunk pthread)

# B-4 단가 마이크로벤치 — 판정 입력 c_i/K_fp 전용 독립 도구. 게이트/벤치
# 프로토콜 비연결 (기본 off 계측 규약: 빌드만 되고 아무 게이트도 안 부른다).
add_executable(hyperchunk_microbench microbench.c)
set_target_properties(hyperchunk_microbench PROPERTIES OUTPUT_NAME
                                                       hyperchunk-microbench)
target_link_libraries(hyperchunk_microbench PRIVATE hyperchunk)

# B-5 체인 임계-경로 하한 도구 — 조항 3 재판정의 분모 산출 (모델, 실측
# 아님). 게이트/벤치 프로토콜 비연결, 스칼라 강제라 어느 호스트에서든
# 돈다. 재산출: hyperchunk-chaincp --repo <root> --lat low|nom [--fma]
add_executable(hyperchunk_chaincp chaincp.c)
set_target_properties(hyperchunk_chaincp PROPERTIES OUTPUT_NAME
                                                    hyperchunk-chaincp)
target_link_libraries(hyperchunk_chaincp PRIVATE hyperchunk m)
