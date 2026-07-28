cmake_minimum_required(VERSION 3.28)
project(hyperchunk C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ADR-004 D3: FMA 전면 금지. 패리티 불변식.
# -ffp-contract=off 가 핵심. mul+add 를 FMA 로 접는 것을 막는다.
add_compile_options(
  -O2
  -ffp-contract=off
  -fno-fast-math
  -Wall -Wextra -Werror
)

# ADR-009 D2: sanitizer 게이트 빌드 (CMakePresets.json 의 asan-ubsan 이 켠다).
# 이 빌드는 check_no_fma.sh 대상이 아니다 — 계측 코드가 섞여 디스어셈블
# 게이트가 무의미하다 (ADR-009 Pitfall 2). FMA 게이트는 release 아카이브 전용.
option(HC_SANITIZE "Build with ASan+UBSan (ADR-009 D2 gate build)" OFF)
if(HC_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(core)
add_subdirectory(cli)
enable_testing()
add_subdirectory(tests)
