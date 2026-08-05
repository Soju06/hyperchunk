#!/usr/bin/env bash
# Regenerate golden/rng/jdk_log.txt (MarsagliaPolarGaussian parity). Pure
# JDK — no Minecraft classes, no server. The dump captures THIS machine's
# HotSpot Math.log intrinsic output (StubRoutines::dlog) over a fixed
# corpus; the C port (core/src/jdk_log.c) must bit-match it
# (tests/unit/test_jdk_log.c).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

BUILD="$HERE/work/log-classes"
mkdir -p "$BUILD"
javac -d "$BUILD" "$HERE/JdkLogGolden.java"
java -cp "$BUILD" JdkLogGolden "$ROOT/golden/rng"
