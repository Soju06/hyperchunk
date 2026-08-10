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
  # 함수 배치 룰렛 고정 (P2-2 실측): df 오브젝트가 커지자 무손대 직렬
  # 페이즈(light09/features)의 -O3 wall 이 +60% 튀었고, 오브젝트 디스어셈
  # 블리는 바이트 동일 — 링크 레이아웃 효과. 64B 정렬로 재현·회복 확인.
  # FP 시맨틱 무관 (parity_gate/check_no_fma 로 재판정).
  -falign-functions=64
  -Wall -Wextra -Werror
)

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

# ADR-009 Pitfall 3: 멀티스레드 확장(P2-3 FREE 스케줄러) 진입 시 TSan 게이트
# 의무. ASan 과 결합 불가라 별도 빌드 (CMakePresets.json 의 tsan).
# check_no_fma 대상 아님 (계측 혼입 — ADR-009 Pitfall 2 와 동일 논리).
option(HC_TSAN "Build with ThreadSanitizer (P2-3 scheduler gate build)" OFF)
if(HC_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
endif()

# B-3 연산 카운터의 최핫 사이트 (hc_counters.h HC_CTR_INC_HOT — x4_run
# 노드 루프, cond_test/rule_apply 진입). 기본 OFF = 코드 부재 (off 상태의
# 로드+분기조차 noise CPU-sum +5.6% 로 실측돼 컴파일 타임 게이트).
# 카운트 수집 시에만 로컬 빌드에서 켠다 — 벤치 수치·게이트 빌드에 금지.
option(HC_CTR_HOT "Compile hot-path op counters (B-3; local count runs only)" OFF)
if(HC_CTR_HOT)
  add_compile_definitions(HC_CTR_HOT)
endif()

add_subdirectory(core)
add_subdirectory(cli)
add_subdirectory(bench)
enable_testing()
add_subdirectory(tests)
