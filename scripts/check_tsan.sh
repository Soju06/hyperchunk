#!/usr/bin/env bash
# P2-3 TSan 게이트 (ADR-009 Pitfall 3): FREE 스케줄러/체인 병렬의 데이터
# 레이스 검출. 전체 ctest 를 TSan 빌드로 직렬 실행 (-j1 — 리전 테스트의
# 새도우 메모리 동시 상주 방지). halt_on_error 로 레이스 = 테스트 FAIL.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake --preset tsan >/dev/null
cmake --build build-tsan -j"$(nproc)" >/dev/null
# 커널 6.x 의 mmap_rnd_bits=32 ASLR 이 TSan 새도우 매핑과 충돌한다
# ("FATAL: unexpected memory mapping") — 프로세스 한정 ASLR 비활성으로 우회.
TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" \
  setarch "$(uname -m)" -R \
  ctest --test-dir build-tsan --no-tests=error --output-on-failure -j1
echo "PASS: full ctest suite clean under TSan"
