#!/usr/bin/env bash
# ADR-002 D3 수용 기준 — r.0.0 풀 리전 canonical 페이로드 해시 판정.
# hyperchunk-verify 가 시드에서 1024청크를 생성해 golden/SHA256SUMS 의
# `#canonical-payload` 라인과 대조한다 (LastUpdate=0 마스킹; raw .mca 는
# 컨테이너 타임스탬프/압축 배치가 비결정이라 페이로드 해시가 판정 기준).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SEED="${SEED:-1234567890}"
BUILD="${BUILD:-$ROOT/build}"
OUT="${OUT:-/tmp/hyperchunk_verify_r.0.0.mca}"

if [ ! -x "$BUILD/cli/hyperchunk-verify" ]; then
    cmake --build "$BUILD" --target hyperchunk_verify -j"$(nproc)"
fi

"$BUILD/cli/hyperchunk-verify" --seed "$SEED" --region 0 0 \
    --repo "$ROOT" --out "$OUT"
