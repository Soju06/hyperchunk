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

# 항상 재빌드 — 존재-검사 스킵은 스테일 바이너리로 낡은 코드를 판정하는
# false-PASS 채널이었다 (P2-4 에서 실제 발화: bench-o2 verify 가 P2-3
# 산출물인 채 PASS 를 찍었다). 증분 빌드라 무변경 시 비용은 ~0 이다.
cmake --build "$BUILD" --target hyperchunk_verify -j"$(nproc)"

"$BUILD/cli/hyperchunk-verify" --seed "$SEED" --region 0 0 \
    --repo "$ROOT" --out "$OUT"
