#!/usr/bin/env bash
# Regenerate golden/rng/{fork,octaves,router}_seed*.txt from the pinned
# vanilla server jar (Plan Task 6). One command, idempotent: fetches +
# extracts the server jar if needed, compiles NoiseGolden.java against the
# unobfuscated classes, runs it (bootstraps game registries, ~10s).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

CP_FILE="$("$HERE/extract_nested.sh")"
CP="$(cat "$CP_FILE")"

BUILD="$HERE/work/noise-classes"
mkdir -p "$BUILD"
javac -cp "$CP" -d "$BUILD" "$HERE/NoiseGolden.java"
java -Xmx2G -cp "$BUILD:$CP" NoiseGolden "$ROOT/golden/rng"
