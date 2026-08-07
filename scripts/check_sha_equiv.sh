#!/usr/bin/env bash
# sw-vs-SHA-NI sha256 canonical 동일 게이트 — 리전 판 (P2-5).
#
#   scripts/check_sha_equiv.sh [빌드 디렉토리 기본 build-bench-o3]
#
# 게이트 판정 해시(canonical)가 hc_sha256 자신이므로, 백엔드가 갈리면
# 판정면 자체가 갈린다 — 풀 리전을 --sha sw 와 --sha ni 로 각각 생성해
# 둘 다 골든 canonical 상수에 PASS 해야 한다 (다이제스트 동일이 상수를
# 경유해 전이 성립; 유닛 판은 test_sha256 KAT+이중 배터리). SHA-NI 부재
# 호스트는 SKIP (게이트 공허 — check_isa_equiv 와 동일 정책; 그 호스트
# 의 sw 경로는 모든 상시 게이트가 이미 판정한다).
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

if ! grep -q sha_ni /proc/cpuinfo 2>/dev/null; then
    echo "SKIP: host lacks SHA-NI — sw-vs-ni gate vacuous"
    exit 0
fi

run_sha() {
    local sha="$1"
    local out
    out="$("$BENCH" --seed "$SEED" --region 0 0 --repo "$ROOT" \
        --threads "$THREADS" --policy replay --sha "$sha" 2>&1 >/dev/null)" || {
        echo "FAIL: --sha $sha run failed"
        echo "$out" | tail -5
        exit 1
    }
    if ! echo "$out" | grep -q "PASS: bit-exact parity"; then
        echo "FAIL: --sha $sha did not pass canonical judgment"
        echo "$out" | tail -5
        exit 1
    fi
    if ! echo "$out" | grep -q "sha=$sha"; then
        echo "FAIL: --sha $sha did not engage (reported line below)"
        echo "$out" | grep "hyperchunk-bench" || true
        exit 1
    fi
    echo "  sha=$sha: PASS (canonical)"
}

echo "== check_sha_equiv ($BUILD) =="
run_sha sw
run_sha ni
echo "PASS: sw and ni sha256 both match golden canonical (digest-identical)"
