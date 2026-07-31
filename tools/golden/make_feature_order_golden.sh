#!/usr/bin/env bash
# Regenerate reference/features_order-26.2.txt from the pinned vanilla
# server jar (Plan Task 9a). One command, idempotent: fetches + extracts
# the server jar if needed, compiles FeatureOrderGolden.java against the
# unobfuscated classes, runs it (bootstraps game registries, ~10s), runs
# it a SECOND time to a temp file and asserts byte-identical output
# (determinism gate), then keeps the first run's file.
#
# Sanity checks / possibleBiomes order / biome-union index sets go to
# stdout (tee'd to tools/golden/logs/feature_order_golden.log).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

CP_FILE="$("$HERE/extract_nested.sh")"
CP="$(cat "$CP_FILE")"

BUILD="$HERE/work/feature-order-classes"
mkdir -p "$BUILD" "$HERE/logs"
javac -cp "$CP" -d "$BUILD" "$HERE/FeatureOrderGolden.java"

OUT="$ROOT/reference/features_order-26.2.txt"
LOG="$HERE/logs/feature_order_golden.log"
java -Xmx2G -cp "$BUILD:$CP" FeatureOrderGolden "$OUT" | tee "$LOG"

# determinism: second full JVM run must produce a byte-identical file
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
java -Xmx2G -cp "$BUILD:$CP" FeatureOrderGolden "$TMP" > /dev/null
cmp "$OUT" "$TMP"
echo "PASS determinism: two JVM runs byte-identical ($OUT)"
