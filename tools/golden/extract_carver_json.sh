#!/usr/bin/env bash
# Extract the configured-carver JSONs referenced by the 26.2 overworld/sulfur
# biomes, plus the full block-tag closure of their `replaceable` sets, out of
# the vanilla server jar into reference/, mirroring the jar's data/minecraft/
# layout (same pattern as extract_worldgen_json.sh):
#
#   reference/carver/<name>.json        (configured_carver configs)
#   reference/tags/block/<name>.json    (replaceable tag + nested tag closure)
#
# Self-verifying: asserts that every biome in the server datapack has one of
# the three known carver signatures (overworld triple / nether single / empty)
# so a future version bump that changes per-biome carvers fails loudly here
# instead of silently invalidating the "gating biome is value-neutral" fact
# recorded in .hermes/notes/task8-carvers/.
#
# Idempotent; reruns overwrite. Extracted files are committed to git — they
# are the input contract for the carver stage (core/src/carvers.c).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

CP_FILE="$("$HERE/extract_nested.sh")"
SERVER_JAR="$(cut -d: -f1 "$CP_FILE")"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
unzip -o -q "$SERVER_JAR" \
    'data/minecraft/worldgen/biome/*' \
    'data/minecraft/worldgen/configured_carver/*' \
    'data/minecraft/tags/block/*' -d "$TMP"

python3 - "$ROOT" "$TMP" <<'EOF'
import json, os, shutil, sys

root, tmp = sys.argv[1], sys.argv[2]
biome_src = os.path.join(tmp, 'data/minecraft/worldgen/biome')
carver_src = os.path.join(tmp, 'data/minecraft/worldgen/configured_carver')
tag_src = os.path.join(tmp, 'data/minecraft/tags/block')

OVERWORLD = ['minecraft:cave', 'minecraft:cave_extra_underground',
             'minecraft:canyon']
NETHER = 'minecraft:nether_cave'

# 1. Signature check over every biome in the datapack.
sigs = {}
for f in sorted(os.listdir(biome_src)):
    c = json.load(open(os.path.join(biome_src, f))).get('carvers')
    sigs.setdefault(json.dumps(c), []).append(f[:-5])
known = {json.dumps(OVERWORLD), json.dumps(NETHER), json.dumps([])}
unknown = {s: n for s, n in sigs.items() if s not in known}
if unknown:
    sys.exit(f'unexpected biome carver signature(s): {unknown}')
n_ow = len(sigs[json.dumps(OVERWORLD)])
assert 'sulfur_caves' in sigs[json.dumps(OVERWORLD)]

# 2. Extract the overworld carver configs.
tags = set()
for ref in OVERWORLD:
    name = ref.split(':', 1)[1]
    src = os.path.join(carver_src, name + '.json')
    cfg = json.load(open(src))['config']
    rep = cfg['replaceable']
    if not rep.startswith('#minecraft:'):
        sys.exit(f'replaceable is not a tag ref in {name}: {rep}')
    tags.add(rep[len('#minecraft:'):])
    dst = os.path.join(root, 'reference/carver', name + '.json')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)

# 3. Resolve the replaceable block-tag closure (# refs recurse).
seen = set()
while tags - seen:
    for t in sorted(tags - seen):
        seen.add(t)
        p = os.path.join(tag_src, t + '.json')
        if not os.path.exists(p):
            sys.exit(f'unresolved block tag reference: #{t}')
        for v in json.load(open(p))['values']:
            if isinstance(v, str) and v.startswith('#minecraft:'):
                tags.add(v[len('#minecraft:'):])

for t in sorted(seen):
    dst = os.path.join(root, 'reference/tags/block', t + '.json')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(os.path.join(tag_src, t + '.json'), dst)

print(f'verified {n_ow} overworld-signature biomes (incl. sulfur_caves); '
      f'extracted {len(OVERWORLD)} carver configs + {len(seen)} block tags '
      f'into reference/')
EOF
