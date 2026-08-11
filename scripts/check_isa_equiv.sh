#!/usr/bin/env bash
# scalar/avx2/avx512 canonical 동일 게이트 — 리전 판 (P2-4/P2-10,
# ADR-004 D4).
#
#   scripts/check_isa_equiv.sh [빌드 디렉토리 기본 build-bench-o3]
#
# 풀 리전(1024청크) 을 --isa scalar/avx2/avx512 로 각각 생성해 전부
# 골든 canonical 상수에 PASS 해야 한다 (bench 가 내부 판정) — 백엔드
# 간 바이트 동일이 상수를 경유해 전이 성립한다. AVX2 부재 호스트는 SKIP
# (게이트 공허 — 유닛 판 df_x4 와 동일 정책). AVX-512(F/DQ/BW/VL) 부재
# 호스트 (로컬 claw=Zen3) 는 avx512 레그만 명시적 SKIP — 2-way 는 그대로
# 판정하고 (게이트 약화 없음), 3-way 실판정은 AVX-512 호스트 (hc-e6) 에서
# 이 스크립트를 그대로 돌린다 (P2-10 노트에 기록).
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

has_avx512() {
    local flags
    flags="$(grep -m1 '^flags' /proc/cpuinfo 2>/dev/null || true)"
    for f in avx512f avx512dq avx512bw avx512vl; do
        grep -qw "$f" <<<"$flags" || return 1
    done
    return 0
}

echo "== check_isa_equiv ($BUILD) =="
run_isa scalar
run_isa avx2
if has_avx512; then
    run_isa avx512
    echo "PASS: scalar/avx2/avx512 all match golden canonical (byte-identical, 3-way)"
else
    echo "SKIP: avx512 leg — host lacks AVX-512 (F/DQ/BW/VL)."
    echo "      3-way must be judged on an AVX-512 host (hc-e6): run this script there."
    echo "PASS: scalar and avx2 both match golden canonical (byte-identical, 2-way)"
fi
