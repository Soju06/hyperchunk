cmake_minimum_required(VERSION 3.28)
project(hyperchunk C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ADR-004 D3: FMA 전면 금지. 패리티 불변식.
# -ffp-contract=off 가 핵심. mul+add 를 FMA 로 접는 것을 막는다.
# HC_OPT_LEVEL 은 O-레벨만 바꾼다 (P2-0 -O2/-O3 비교용, bench-o2/o3 프리셋).
# FP 플래그는 협상 불가라 캐시 변수로 노출하지 않는다.
set(HC_OPT_LEVEL "-O2" CACHE STRING "Optimization level flag (-O2/-O3)")
add_compile_options(
  ${HC_OPT_LEVEL}
  -ffp-contract=off
  -fno-fast-math
  -Wall -Wextra -Werror
)

# NDEBUG(Release 계열)에서 assert 전용 파라미터가 unused 로 떨어져 -Werror
# 를 깬다 (structures.c:41 'cap' — release 프리셋은 지금껏 빌드된 적이
# 없어 잠복). -O3 는 범위 분석이 깊어져 의도된-절단 snprintf 에
# format-truncation 도 발화한다 (structures.c:659). 코어 수정은 P2-0 범위
# 밖이라 Release 한정 완화. 본 수정((void)cap 등)은 Phase 2 후속 태스크에서.
add_compile_options($<$<CONFIG:Release>:-Wno-unused-parameter>
                    $<$<CONFIG:Release>:-Wno-format-truncation>)

# ADR-009 D2: sanitizer 게이트 빌드 (CMakePresets.json 의 asan-ubsan 이 켠다).
# 이 빌드는 check_no_fma.sh 대상이 아니다 — 계측 코드가 섞여 디스어셈블
# 게이트가 무의미하다 (ADR-009 Pitfall 2). FMA 게이트는 release 아카이브 전용.
# float-cast-overflow 는 gcc 의 'undefined' 그룹에 포함되지 않아 명시해야
# 한다 — (int)double 범위 밖 캐스트가 이 프로젝트의 실제 UB 클래스다
# (floor 좌표 → int 변환 경로).
option(HC_SANITIZE "Build with ASan+UBSan (ADR-009 D2 gate build)" OFF)
if(HC_SANITIZE)
  add_compile_options(-fsanitize=address,undefined,float-cast-overflow
                      -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined,float-cast-overflow)
endif()

add_subdirectory(core)
add_subdirectory(cli)
add_subdirectory(bench)
enable_testing()
add_subdirectory(tests)
