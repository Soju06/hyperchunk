#!/usr/bin/env bash
# Regenerate golden/rng/jdk_sincos.txt (Plan Task 9a). Pure JDK — no
# Minecraft classes, no server. The dump captures THIS machine's HotSpot
# Math.sin/cos intrinsic output over the ore-angle domain; the C port
# (core/src/jdk_trig.c) must bit-match it (tests/unit/test_features_rng.c).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

BUILD="$HERE/work/sincos-classes"
mkdir -p "$BUILD"
javac -d "$BUILD" "$HERE/JdkSinCosGolden.java"
java -cp "$BUILD" JdkSinCosGolden "$ROOT/golden/rng"
