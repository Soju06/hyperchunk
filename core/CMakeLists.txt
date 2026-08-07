add_library(hyperchunk STATIC
  src/version.c
  src/rng.c
  src/arena.c
  src/chunk.c
  src/md5.c
  src/json.c
  src/noise_perlin.c
  src/noise_octaves.c
  src/noise_blended.c
  src/noise_normal.c
  src/df_eval.c
  src/df_compile.c
  src/df_isa.c
  src/df_simd_avx2.c
  src/noise_chunk.c
  src/beard.c
  src/aquifer.c
  src/ore_veins.c
  src/gen_noise_stage.c
  src/blocks.c
  src/biome_zoom.c
  src/biome_temp.c
  src/sha256.c
  src/surface_compile.c
  src/surface.c
  src/carvers.c
  src/gen_carvers_stage.c
  src/jdk_trig.c
  src/jdk_log.c
  src/features_rng.c
  src/features_compile.c
  src/features.c
  src/gen_features_stage.c
  src/features_tree.c
  src/features_lush.c
  src/features_ring.c
  src/jset.c
  src/light_engine.c
  src/gen_light_stages.c
  src/gen_spawn_full_stages.c
  src/postprocess.c
  src/nbt.c
  src/chunk_nbt.c
  src/region.c
  src/structures.c
  src/structures_template.c
  src/structures_mineshaft.c
  src/sched.c
)
target_include_directories(hyperchunk PUBLIC include)

# AVX2 커널 TU 한정 -mavx2 (ADR-004 D1/D2): 나머지 TU 는 기본 ISA 로
# 남겨 AVX2 부재 호스트에서도 실행 가능해야 한다 (진입은 cpuid 디스패치
# 뒤에만). -mfma 는 켜지 않는다 — FMA 금지 (ADR-004 D3) 는 인트린식
# 선택 + check_no_fma.sh 가 이중으로 지킨다.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
  set_source_files_properties(src/df_simd_avx2.c PROPERTIES
    COMPILE_OPTIONS "-mavx2")
endif()
# ADR-003 D1: 서드파티 의존성 없음. libm 은 ISO C 수학 런타임 (glibc
# 2.34+ 에서는 libc 로 통합된 호환 스텁) — D1 위반 아님. pthread 는
# P2-3 스케줄러(sched.c)가 소유 — "스레딩은 pthread 허용" (기존 체인
# 병렬과 동일 기준), POSIX 스레드 런타임이라 D1 위반 아님.
find_package(Threads REQUIRED)
target_link_libraries(hyperchunk PUBLIC m Threads::Threads)
