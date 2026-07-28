#!/usr/bin/env bash
# Extract every worldgen JSON the 26.2 overworld noise_router transitively
# references (density functions + noise parameters) out of the vanilla server
# jar into reference/, mirroring the jar's data/minecraft/worldgen/ layout:
#
#   reference/density_function/<name>.json   (e.g. overworld/offset.json)
#   reference/noise/<name>.json              (amplitudes / firstOctave)
#
# The closure is seeded from reference/overworld-26.2.json (noise_router).
# Reference resolution rules mirror vanilla codecs:
#   - a bare string where a density function is expected -> DF registry ref
#   - the "noise" key, and the "argument" of shift_a/shift_b/shift
#     -> noise-parameters registry ref
#
# Idempotent; reruns overwrite. Extracted files are committed to git — they
# are the input contract for the JSON->IR compiler (core/src/df_json.c).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
VER="$(cat "$ROOT/TARGET_VERSION")"

CP_FILE="$("$HERE/extract_nested.sh")"
SERVER_JAR="$(cut -d: -f1 "$CP_FILE")"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
unzip -o -q "$SERVER_JAR" \
    'data/minecraft/worldgen/density_function/*' \
    'data/minecraft/worldgen/noise/*' -d "$TMP"

python3 - "$ROOT" "$TMP" <<'EOF'
import json, os, shutil, sys

root, tmp = sys.argv[1], sys.argv[2]
df_src = os.path.join(tmp, 'data/minecraft/worldgen/density_function')
noise_src = os.path.join(tmp, 'data/minecraft/worldgen/noise')

# shift_a/shift_b/shift take a NOISE reference in "argument" (vanilla codec),
# every other bare string in a DF position is a density_function reference.
NOISE_ARG_TYPES = {'minecraft:shift_a', 'minecraft:shift_b', 'minecraft:shift'}
ENUM_KEYS = {'rarity_value_mapper'}  # enum strings, not registry refs

def collect(node, dfs, noises, key=None, parent_type=None):
    if isinstance(node, dict):
        t = node.get('type')
        for k, v in node.items():
            if k != 'type':
                collect(v, dfs, noises, k, t)
    elif isinstance(node, list):
        for v in node:
            collect(v, dfs, noises, key, parent_type)
    elif isinstance(node, str):
        if key == 'noise' or (parent_type in NOISE_ARG_TYPES and key == 'argument'):
            noises.add(node)
        elif key not in ENUM_KEYS:
            dfs.add(node)

router = json.load(open(os.path.join(root, 'reference/overworld-26.2.json')))['noise_router']
dfs, noises = set(), set()
collect(router, dfs, noises)

seen = set()
while dfs - seen:
    for ref in sorted(dfs - seen):
        seen.add(ref)
        name = ref.split(':', 1)[1]
        p = os.path.join(df_src, name + '.json')
        if not os.path.exists(p):
            sys.exit(f'unresolved density_function reference: {ref}')
        collect(json.load(open(p)), dfs, noises)

def install(names, src_dir, dst_dir):
    for ref in sorted(names):
        name = ref.split(':', 1)[1]
        src = os.path.join(src_dir, name + '.json')
        if not os.path.exists(src):
            sys.exit(f'unresolved reference: {ref} ({src})')
        dst = os.path.join(root, 'reference', dst_dir, name + '.json')
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)

install(seen, df_src, 'density_function')
install(noises, noise_src, 'noise')
print(f'extracted {len(seen)} density_function + {len(noises)} noise JSON files into reference/')
EOF
