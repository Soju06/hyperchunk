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
  src/noise_chunk.c
  src/aquifer.c
  src/ore_veins.c
  src/gen_noise_stage.c
  src/blocks.c
  src/biome_zoom.c
  src/biome_temp.c
  src/sha256.c
  src/surface_compile.c
  src/surface.c
)
target_include_directories(hyperchunk PUBLIC include)
# ADR-003 D1: 서드파티 의존성 없음. libm 은 ISO C 수학 런타임 (glibc
# 2.34+ 에서는 libc 로 통합된 호환 스텁) — D1 위반 아님.
target_link_libraries(hyperchunk PUBLIC m)
