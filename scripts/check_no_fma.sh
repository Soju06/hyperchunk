#!/usr/bin/env bash
# ADR-004 D3/Verification: 산출물에 FMA 명령이 없어야 한다.
# FMA contraction 은 중간 반올림을 없애 지형 비트를 조용히 바꾼다 (ADR-002 Pitfall 1).
set -euo pipefail

LIB="${1:-build/core/libhyperchunk.a}"

if [ ! -f "$LIB" ]; then
  echo "FAIL: $LIB not found (build first: cmake --build build)"
  exit 1
fi

# grep -q 를 파이프에 직접 쓰면 첫 매치에서 조기 종료 → objdump SIGPIPE(141)
# → pipefail 아래에서 매치가 '있을 때' 파이프라인이 실패해 FAIL 경로를
# 건너뛰는 false-PASS 가 난다. 출력을 전부 소비해 캡처한 뒤 판정한다.
disasm="$(objdump -d "$LIB")"
matches="$(grep -E '\bvfmadd|\bvfmsub|\bvfnmadd|\bvfnmsub' <<<"$disasm" || true)"

if [ -n "$matches" ]; then
  echo "FAIL: FMA instruction found in $LIB"
  head -5 <<<"$matches"
  exit 1
fi
echo "PASS: no FMA instructions in $LIB"
