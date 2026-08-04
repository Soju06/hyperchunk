#!/usr/bin/env bash
# Task 14: full-region quart biome band — chunks [-6..36]^2 (quarts
# [-24..147]^2, qy [-16..79]) from the pinned vanilla jar. Same
# pure-function-of-seed sampling as make_biome_band_golden.sh (see
# BiomeBandGolden.java header), window widened to cover the r.0.0
# generation margin (features -4..34, carvers/noise -5..35) plus the
# structure-placement biome probes just outside it.
#
# Gates:
#  (a) overlap with the surface golden's quart_biomes (qx,qz -8..11)
#  (b) second full JVM run byte-identical
#  (c) name-for-name match with the committed Task-10 band over its
#      whole window (chunks -6..6) — proves the widened window is a
#      superset capture, not a drifted one.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

CP_FILE="$("$HERE/extract_nested.sh")"
CP="$(cat "$CP_FILE")"

BUILD="$HERE/work/biome-band-classes"
mkdir -p "$BUILD" "$HERE/logs"
javac -cp "$CP" -d "$BUILD" "$HERE/BiomeBandGolden.java"

SEED=1234567890
OUT="$ROOT/golden/rng/biome_band_region_seed$SEED.txt"
SURFACE="$ROOT/golden/rng/surface_seed$SEED.txt"
NARROW="$ROOT/golden/rng/biome_band_seed$SEED.txt"
LOG="$HERE/logs/biome_band_region.log"
WIDE=(-Dhyperchunk.qmin=-48 -Dhyperchunk.qmax=175)

java -Xmx3G "${WIDE[@]}" -cp "$BUILD:$CP" BiomeBandGolden "$OUT" "$SURFACE" | tee "$LOG"

# determinism: second full JVM run must produce a byte-identical file
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
java -Xmx3G "${WIDE[@]}" -cp "$BUILD:$CP" BiomeBandGolden "$TMP" "$SURFACE" > /dev/null
cmp "$OUT" "$TMP"
echo "determinism gate: second run byte-identical"

# superset gate: wide band == committed narrow band over chunks -6..6
python3 - "$OUT" "$NARROW" <<'EOF'
import sys

def load(path):
    pal, grid, qy = [], {}, None
    qmin = qmax = None
    with open(path) as f:
        for line in f:
            if line.startswith('section quart_biomes'):
                t = line.split()
                qmin, qmax = int(t[3]), int(t[4])
            elif line.startswith('palette '):
                t = line.split(' ', 2)
                assert int(t[1]) == len(pal)
                pal.append(t[2].strip())
            elif line.startswith('qy '):
                qy = int(line[3:]); qz = qmin
            elif qy is not None and line.strip() and not line.startswith('#') \
                    and not line.startswith('seed'):
                row = line.split()
                if len(row) != qmax - qmin + 1:
                    continue
                for i, v in enumerate(row):
                    grid[(qmin + i, qy, qz)] = pal[int(v)]
                qz += 1
    return grid

wide, narrow = load(sys.argv[1]), load(sys.argv[2])
bad = 0
for k, name in narrow.items():
    if wide.get(k) != name:
        bad += 1
        if bad <= 10:
            print(f'SUPERSET MISMATCH {k}: wide={wide.get(k)} narrow={name}',
                  file=sys.stderr)
if bad:
    sys.exit(f'{bad} superset mismatches')
print(f'superset gate: {len(narrow)} quarts match committed Task-10 band')
EOF
