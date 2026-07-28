#!/usr/bin/env bash
# Extract the real (nested) server jar and bundled libraries out of the
# Mojang bundler jar downloaded by fetch_server.sh.
#
# The 26.x server download is a BUNDLER: the actual game classes live in
# META-INF/versions/<ver>/server-<ver>.jar and its Maven-style library
# tree in META-INF/libraries/. Both are needed as a compile/run classpath
# for the golden harnesses (RNG vectors, stage-dump mod build).
#
# Output (idempotent, gitignored):
#   tools/golden/libs/extracted/server-<ver>.jar
#   tools/golden/libs/extracted/libraries/**/*.jar
#   tools/golden/libs/extracted/classpath.txt   (colon-joined classpath)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
VER="$(cat "$ROOT/TARGET_VERSION")"
BUNDLER="$HERE/libs/server-$VER.jar"
OUT="$HERE/libs/extracted"

"$HERE/fetch_server.sh" >&2

if [ -f "$OUT/server-$VER.jar" ] && [ -f "$OUT/classpath.txt" ]; then
    echo "$OUT/classpath.txt"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
unzip -o -q "$BUNDLER" "META-INF/versions/$VER/*" 'META-INF/libraries/*' -d "$TMP"

mkdir -p "$OUT"
cp "$TMP/META-INF/versions/$VER/server-$VER.jar" "$OUT/server-$VER.jar"
rm -rf "$OUT/libraries"
cp -r "$TMP/META-INF/libraries" "$OUT/libraries"

{
    printf '%s' "$OUT/server-$VER.jar"
    find "$OUT/libraries" -name '*.jar' -print0 | while IFS= read -r -d '' j; do
        printf ':%s' "$j"
    done
    printf '\n'
} > "$OUT/classpath.txt"

echo "extracted nested server jar + $(find "$OUT/libraries" -name '*.jar' | wc -l) libraries" >&2
echo "$OUT/classpath.txt"
