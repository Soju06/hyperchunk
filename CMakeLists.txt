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

add_subdirectory(core)
add_subdirectory(cli)
enable_testing()
add_subdirectory(tests)
