#!/usr/bin/env bash
# Fetch the pinned Fabric toolchain jars into tools/golden/libs/fabric/
# (gitignored). Idempotent.
#
# Versions are pinned; 0.19.3 is the loader version meta.fabricmc.net
# lists as stable for MC 26.2, where intermediary is a stub (0.0.0):
# the runtime namespace is the official Mojang namespace (ADR-006 D3).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/libs/fabric"
mkdir -p "$OUT"

LOADER_VERSION="${FABRIC_LOADER_VERSION:-0.19.3}"
INSTALLER_VERSION="${FABRIC_INSTALLER_VERSION:-1.1.2}"
MIXIN_VERSION="${SPONGE_MIXIN_VERSION:-0.17.3+mixin.0.8.7}"
MAVEN=https://maven.fabricmc.net

fetch() { # fetch <url> <dest>
    if [ ! -f "$2" ]; then
        echo "downloading $(basename "$2") ..." >&2
        curl -fsSL -o "$2.tmp" "$1"
        mv "$2.tmp" "$2"
    fi
}

fetch "$MAVEN/net/fabricmc/fabric-loader/$LOADER_VERSION/fabric-loader-$LOADER_VERSION.jar" \
      "$OUT/fabric-loader-$LOADER_VERSION.jar"
fetch "$MAVEN/net/fabricmc/fabric-installer/$INSTALLER_VERSION/fabric-installer-$INSTALLER_VERSION.jar" \
      "$OUT/fabric-installer-$INSTALLER_VERSION.jar"
fetch "$MAVEN/net/fabricmc/sponge-mixin/$MIXIN_VERSION/sponge-mixin-$MIXIN_VERSION.jar" \
      "$OUT/sponge-mixin-$MIXIN_VERSION.jar"

echo "fabric toolchain ready in $OUT" >&2
