#!/usr/bin/env bash
# Regenerate golden/rng/mth_sin_table.txt from the pinned vanilla server jar
# (Plan Task 8). One command, idempotent: fetches + extracts the server jar
# if needed, compiles MthGolden.java against the unobfuscated classes, runs
# it (no server bootstrap; Mth is self-contained). Seed-independent.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

CP_FILE="$("$HERE/extract_nested.sh")"
CP="$(cat "$CP_FILE")"

BUILD="$HERE/work/mth-classes"
mkdir -p "$BUILD"
javac -cp "$CP" -d "$BUILD" "$HERE/MthGolden.java"
java -cp "$BUILD:$CP" MthGolden "$ROOT/golden/rng"
