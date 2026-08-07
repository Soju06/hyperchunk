#!/usr/bin/env bash
# scalar-vs-avx2 canonical 동일 게이트 — 리전 판 (P2-4, ADR-004 D4).
#
#   scripts/check_isa_equiv.sh [빌드 디렉토리 기본 build-bench-o3]
#
# 풀 리전(1024청크) 을 --isa scalar 와 --isa avx2 로 각각 생성해 둘 다
# 골든 canonical 상수에 PASS 해야 한다 (bench 가 내부 판정) — 두 백엔드
# 의 바이트 동일이 상수를 경유해 전이 성립한다. AVX2 부재 호스트는 SKIP
# (게이트 공허 — 유닛 판 df_x4 와 동일 정책).
#
# 주의: 이 게이트는 REPLAY 모드다 (canonical 상수 판정면). FREE 모드의
# 백엔드 동일성은 free_region_own 게이트 + 벤치 런 자체 판정이 커버한다.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="${1:-$ROOT/build-bench-o3}"
PRESET="$(basename "$BUILD" | sed 's/^build-//')"
SEED="${SEED:-1234567890}"
THREADS="${THREADS:-20}"

[ -d "$BUILD" ] || cmake --preset "$PRESET" -S "$ROOT" >/dev/null
cmake --build "$BUILD" --target hyperchunk_bench -j"$(nproc)" >/dev/null

BENCH="$BUILD/bench/hyperchunk-bench"

if ! grep -q avx2 /proc/cpuinfo 2>/dev/null; then
    echo "SKIP: host lacks AVX2 — scalar-vs-avx2 gate vacuous"
    exit 0
fi

run_isa() {
    local isa="$1"
    local out
    out="$("$BENCH" --seed "$SEED" --region 0 0 --repo "$ROOT" \
        --threads "$THREADS" --policy replay --isa "$isa" 2>&1 >/dev/null)" || {
        echo "FAIL: --isa $isa run failed"
        echo "$out" | tail -5
        exit 1
    }
    if ! echo "$out" | grep -q "PASS: bit-exact parity"; then
        echo "FAIL: --isa $isa did not pass canonical judgment"
        echo "$out" | tail -5
        exit 1
    fi
    if ! echo "$out" | grep -q "isa=$isa"; then
        echo "FAIL: --isa $isa did not engage (reported line below)"
        echo "$out" | grep "hyperchunk-bench" || true
        exit 1
    fi
    echo "  isa=$isa: PASS (canonical)"
}

echo "== check_isa_equiv ($BUILD) =="
run_isa scalar
run_isa avx2
echo "PASS: scalar and avx2 both match golden canonical (byte-identical)"
