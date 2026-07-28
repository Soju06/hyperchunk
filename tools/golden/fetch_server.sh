#!/usr/bin/env bash
# Download the pinned vanilla server jar (TARGET_VERSION) into tools/golden/libs/.
# Idempotent: skips download if the jar already exists and its sha1 matches
# the piston-meta manifest.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
VER="$(cat "$ROOT/TARGET_VERSION")"
LIBS="$HERE/libs"
JAR="$LIBS/server-$VER.jar"
mkdir -p "$LIBS"

MANIFEST_URL="https://launchermeta.mojang.com/mc/game/version_manifest_v2.json"

read -r VJSON_URL <<<"$(curl -fsSL "$MANIFEST_URL" | python3 -c "
import json, sys
d = json.load(sys.stdin)
hits = [v['url'] for v in d['versions'] if v['id'] == '$VER']
if not hits:
    sys.exit('version $VER not found in manifest')
print(hits[0])
")"

read -r SERVER_URL SERVER_SHA1 <<<"$(curl -fsSL "$VJSON_URL" | python3 -c "
import json, sys
s = json.load(sys.stdin)['downloads']['server']
print(s['url'], s['sha1'])
")"

if [ -f "$JAR" ] && echo "$SERVER_SHA1  $JAR" | sha1sum -c --status 2>/dev/null; then
    echo "already fetched: $JAR (sha1 ok)"
    exit 0
fi

echo "downloading server $VER ..."
curl -fL -o "$JAR.tmp" "$SERVER_URL"
echo "$SERVER_SHA1  $JAR.tmp" | sha1sum -c --status
mv "$JAR.tmp" "$JAR"
echo "fetched: $JAR"
