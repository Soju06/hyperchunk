add_executable(hyperchunk_cli main.c)
set_target_properties(hyperchunk_cli PROPERTIES OUTPUT_NAME hyperchunk)
target_link_libraries(hyperchunk_cli PRIVATE hyperchunk)

# 패리티 게이트 CLI (Plan Task 13): r.0.0 생성 → canonical 페이로드 해시
# 판정 (scripts/parity_gate.sh 가 구동). 파이프라인 시맨틱의 소스 오브
# 트루스는 tests/parity/test_full_region.c.
add_executable(hyperchunk_verify hyperchunk_verify.c)
set_target_properties(hyperchunk_verify PROPERTIES OUTPUT_NAME
                                                   hyperchunk-verify)
target_link_libraries(hyperchunk_verify PRIVATE hyperchunk pthread)
