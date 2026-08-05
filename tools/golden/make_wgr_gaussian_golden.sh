#!/usr/bin/env bash
# Regenerate golden/rng/wgr_gaussian.txt (WorldgenRandom.nextGaussian parity).
# 서버 jar 의 deobf WorldgenRandom/XoroshiroRandomSource 를 직접 구동 —
# Marsaglia 캐시의 재시드 지속 시맨틱까지 실서버 기준으로 캡처한다.
# C 대응: hc_wgr_next_gaussian (tests/unit/test_features_rng.c 소비).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

CP="$ROOT/tools/golden/libs/extracted/server-26.2.jar"
if [ -f "$ROOT/tools/golden/libs/extracted/classpath.txt" ]; then
  CP="$CP:$(tr '\n' ':' < "$ROOT/tools/golden/libs/extracted/classpath.txt")"
fi

BUILD="$HERE/work/gauss-classes"
mkdir -p "$BUILD"
javac -cp "$CP" -d "$BUILD" "$HERE/WgrGaussianGolden.java"
java -cp "$BUILD:$CP" WgrGaussianGolden "$ROOT/golden/rng"
