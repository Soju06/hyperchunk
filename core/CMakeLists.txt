add_library(hyperchunk STATIC
  src/version.c
  src/rng.c
  src/arena.c
  src/chunk.c
)
target_include_directories(hyperchunk PUBLIC include)
# ADR-003 D1: libc 외 의존성 없음
