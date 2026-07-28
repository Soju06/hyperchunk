#!/usr/bin/env bash
# Regenerate golden/rng/*.txt from the pinned vanilla server jar.
# One command, idempotent: fetches + extracts the server jar if needed,
# compiles RngGolden.java against the unobfuscated classes, runs it.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

CP_FILE="$("$HERE/extract_nested.sh")"
CP="$(cat "$CP_FILE")"

BUILD="$HERE/work/rng-classes"
mkdir -p "$BUILD"
javac -cp "$CP" -d "$BUILD" "$HERE/RngGolden.java"
java -cp "$BUILD:$CP" RngGolden "$ROOT/golden/rng"
