#!/usr/bin/env bash
# Regenerate golden/rng/biome_band_seed*.txt from the pinned vanilla server
# jar (Plan Task 10). One command, idempotent: compiles BiomeBandGolden.java
# against the unobfuscated classes, runs it (bootstraps game registries,
# ~10s; NOT a server), gates on (a) name-for-name match with the surface
# golden's quart_biomes section over the overlap region and (b) a second
# full JVM run producing a byte-identical file.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

CP_FILE="$("$HERE/extract_nested.sh")"
CP="$(cat "$CP_FILE")"

BUILD="$HERE/work/biome-band-classes"
mkdir -p "$BUILD" "$HERE/logs"
javac -cp "$CP" -d "$BUILD" "$HERE/BiomeBandGolden.java"

SEED=1234567890
OUT="$ROOT/golden/rng/biome_band_seed$SEED.txt"
SURFACE="$ROOT/golden/rng/surface_seed$SEED.txt"
LOG="$HERE/logs/biome_band_golden.log"
java -Xmx2G -cp "$BUILD:$CP" BiomeBandGolden "$OUT" "$SURFACE" | tee "$LOG"

# determinism: second full JVM run must produce a byte-identical file
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
java -Xmx2G -cp "$BUILD:$CP" BiomeBandGolden "$TMP" "$SURFACE" > /dev/null
cmp "$OUT" "$TMP"
echo "determinism gate: second run byte-identical"
