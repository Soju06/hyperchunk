#!/usr/bin/env bash
# ADR-009 D2: 전체 ctest 스위트를 ASan+UBSan 계측 하에 추가 실행하는 표준
# 게이트. 어떤 sanitizer 리포트든 곧 실패다.
#
# 주의 (ADR-009 Pitfall 2): 이 빌드(build-asan/)는 check_no_fma.sh 의 대상이
# 아니다 — 계측 코드가 섞여 디스어셈블 검사가 무의미하다. FMA 게이트는
# release 아카이브(build/)에만 건다.
#
# 한계 (ADR-009 Pitfall 1): sanitizer 는 실행된 경로만 검사한다. 이 게이트의
# 실효성 상한은 ctest 커버리지다.
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset asan-ubsan
cmake --build build-asan -j

# halt_on_error=1: 리포트 즉시 비정상 종료 → ctest FAIL 로 전파.
# (UBSan 은 기본이 '출력만 하고 계속' 이라 이 옵션 없이는 게이트가 새는
#  false-PASS 채널이 된다. print_stacktrace 는 리포트 진단용.)
ASAN_OPTIONS="halt_on_error=1" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
ctest --test-dir build-asan --output-on-failure

echo "PASS: full ctest suite clean under ASan+UBSan"
