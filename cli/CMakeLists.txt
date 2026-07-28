# CLI placeholder. hyperchunk-verify 는 Plan Task 13 에서 추가된다.
add_executable(hyperchunk_cli main.c)
set_target_properties(hyperchunk_cli PROPERTIES OUTPUT_NAME hyperchunk)
target_link_libraries(hyperchunk_cli PRIVATE hyperchunk)
