#!/usr/bin/env bash
# Extract the features-stage datapack JSON the 26.2 overworld biome lists
# transitively reference, out of the vanilla server jar into reference/,
# mirroring the jar's data/minecraft/ layout (same pattern as
# extract_worldgen_json.sh / extract_carver_json.sh):
#
#   reference/placed_feature/<name>.json
#   reference/configured_feature/<name>.json
#   reference/tags/block/<name>.json         (rule-test / block-predicate tags,
#                                             nested-tag closure)
#   reference/biome_features-26.2.json       (per-biome per-step placed-feature
#                                             name lists, all datapack biomes —
#                                             the decoration walk's membership
#                                             input)
#
# The placed-feature seed set is reference/features_order-26.2.txt — the
# FeatureSorter output over the overworld MultiNoiseBiomeSource possibleBiomes
# (tools/golden/FeatureOrderGolden.java), i.e. exactly "every feature present
# in an overworld biome list". The closure then alternates namespaces:
#   placed_feature "feature" key            -> configured_feature ref
#   configured_feature selector-config keys -> placed_feature refs
#     (features / default / feature_true / feature_false / vegetation_feature
#      / feature — the 26.2 Holder<PlacedFeature> codec positions)
# Inline (non-string) features recurse naturally. Unresolved refs fail loudly.
#
# Idempotent; reruns overwrite. Extracted files are committed to git — they
# are the input contract for the features stage (core/src/features*.c).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

[ -f "$ROOT/reference/features_order-26.2.txt" ] || {
    echo "FATAL: run make_feature_order_golden.sh first (features_order seed set)" >&2
    exit 1
}

CP_FILE="$("$HERE/extract_nested.sh")"
SERVER_JAR="$(cut -d: -f1 "$CP_FILE")"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
unzip -o -q "$SERVER_JAR" \
    'data/minecraft/worldgen/biome/*' \
    'data/minecraft/worldgen/placed_feature/*' \
    'data/minecraft/worldgen/configured_feature/*' \
    'data/minecraft/tags/block/*' -d "$TMP"

python3 - "$ROOT" "$TMP" <<'EOF'
import json, os, shutil, sys

root, tmp = sys.argv[1], sys.argv[2]
biome_src = os.path.join(tmp, 'data/minecraft/worldgen/biome')
placed_src = os.path.join(tmp, 'data/minecraft/worldgen/placed_feature')
conf_src = os.path.join(tmp, 'data/minecraft/worldgen/configured_feature')
tag_src = os.path.join(tmp, 'data/minecraft/tags/block')

def load(src, ref):
    p = os.path.join(src, ref.split(':', 1)[1] + '.json')
    if not os.path.exists(p):
        sys.exit(f'unresolved reference: {ref} ({p})')
    return json.load(open(p))

# --- seed: every placed feature in the overworld FeatureSorter output ---
placed = set()
for line in open(os.path.join(root, 'reference/features_order-26.2.txt')):
    parts = line.split()
    if len(parts) == 2 and parts[0].isdigit():
        placed.add(parts[1])

# Holder<PlacedFeature> codec positions inside configured-feature configs
# (26.2: RandomFeatureConfiguration entries+default, SimpleRandomFeature
# "features", RandomBooleanFeature feature_true/false, RootSystem "feature",
# VegetationPatch "vegetation_feature"). A dict carrying a "placement" key is
# an INLINE PlacedFeature (e.g. root_system's config.feature) — its own
# "feature" key then references a CONFIGURED feature again.
PLACED_REF_KEYS = {'features', 'default', 'feature_true', 'feature_false',
                   'vegetation_feature', 'feature'}

tags = set()
configured = set()

def collect_tags(node):
    if isinstance(node, dict):
        for k, v in node.items():
            if k == 'tag' and isinstance(v, str):
                tags.add(v)
            else:
                collect_tags(v)
    elif isinstance(node, list):
        for v in node:
            collect_tags(v)

def walk_placed_value(v):
    """A value at a Holder<PlacedFeature> position: ref, inline, or list."""
    refs = set()
    if isinstance(v, str):
        refs.add(v)
    elif isinstance(v, dict):
        if 'placement' in v:            # inline PlacedFeature
            refs |= walk_placed_body(v)
        else:                           # weighted entry {feature, chance} etc.
            refs |= walk_config(v)
    elif isinstance(v, list):
        for e in v:
            refs |= walk_placed_value(e)
    return refs

def walk_config(node):
    """Configured-feature config: find placed refs, chase inline features."""
    refs = set()
    if isinstance(node, dict):
        for k, v in node.items():
            if k in PLACED_REF_KEYS:
                refs |= walk_placed_value(v)
            else:
                refs |= walk_config(v)
    elif isinstance(node, list):
        for v in node:
            refs |= walk_config(v)
    return refs

def walk_placed_body(data):
    """A PlacedFeature body (top-level file or inline): returns placed refs
    discovered transitively through any INLINE configured feature."""
    feat = data.get('feature')
    if isinstance(feat, str):
        configured.add(feat)
        return set()
    return walk_config((feat or {}).get('config', {}))

# --- alternate closure: placed -> configured -> placed ... ---
placed_done, conf_done = set(), set()
while placed - placed_done or configured - conf_done:
    for ref in sorted(placed - placed_done):
        placed_done.add(ref)
        data = load(placed_src, ref)
        collect_tags(data)
        placed |= walk_placed_body(data)
    for ref in sorted(configured - conf_done):
        conf_done.add(ref)
        data = load(conf_src, ref)
        collect_tags(data)
        placed |= walk_config(data.get('config', {}))

# --- block-tag closure (nested '#' refs) ---
tag_done = set()
while tags - tag_done:
    for ref in sorted(tags - tag_done):
        tag_done.add(ref)
        data = load(tag_src, ref)
        for v in data.get('values', []):
            name = v['id'] if isinstance(v, dict) else v
            if name.startswith('#'):
                tags.add(name[1:])

def install(names, src_dir, dst_dir):
    for ref in sorted(names):
        name = ref.split(':', 1)[1]
        src = os.path.join(src_dir, name + '.json')
        if not os.path.exists(src):
            sys.exit(f'unresolved reference: {ref} ({src})')
        dst = os.path.join(root, 'reference', dst_dir, name + '.json')
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)

install(placed_done, placed_src, 'placed_feature')
install(conf_done, conf_src, 'configured_feature')
install(tag_done, tag_src, 'tags/block')

# --- per-biome per-step placed-feature lists (all datapack biomes) ---
biomes = {}
for fname in sorted(os.listdir(biome_src)):
    if not fname.endswith('.json'):
        continue
    data = json.load(open(os.path.join(biome_src, fname)))
    steps = data.get('features', [])
    for step in steps:
        for f in step:
            if not isinstance(f, str):
                sys.exit(f'inline placed feature in biome {fname} — unsupported')
    biomes['minecraft:' + fname[:-5]] = steps
out = os.path.join(root, 'reference', 'biome_features-26.2.json')
with open(out, 'w') as fh:
    json.dump(biomes, fh, indent=1, sort_keys=True)
    fh.write('\n')

print(f'extracted {len(placed_done)} placed_feature + {len(conf_done)} configured_feature '
      f'+ {len(tag_done)} block tags + {len(biomes)} biome feature lists into reference/')
EOF
