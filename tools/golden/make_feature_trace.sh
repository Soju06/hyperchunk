#!/usr/bin/env bash
# Feature-trace golden harness (Plan Task 9a).
#
# Produces golden/features-trace/seed<seed>/ — the Task 9 bisect ladder:
#   traces/c.<x>.<z>.trace.txt   per decorated chunk: begin/s/p/f/end events
#                                (see FeatureTrace.java; p = positions that
#                                survived the placement-modifier pipeline)
#   c.<x>.<z>/...                ring-chunk (chessboard distance 2) stage
#                                dumps 01..07 — the 06 state + biomes the C
#                                replay needs for reads at the grid edge
#   order.manifest / order.snapshots  of THIS run
#
# VALIDITY ARGUMENT (ADR-007): traces are only usable as goldens for the
# committed primary bundle golden/stages/seed<seed> if this run decorated the
# grid in the same order and produced identical 07 state. NOTES.md records
# that 1-bg-thread runs have a sticky grid order; this script VERIFIES it:
#   - every grid-chunk 01..07 dump byte-matches the committed SHA256SUMS
#   - the manifest's first 9 data lines (seq chunkX chunkZ seed) match the
#     committed manifest
# and aborts without writing SHA256SUMS if not. Ring 03/06 dumps are
# order-free (Tier 1 stages) and valid regardless.
#
# The committed bundle is never touched: output goes to golden/features-trace/
# with its own SHA256SUMS. Grid-chunk dump dirs are pruned after verification
# (byte-identical duplicates of the committed bundle); ring 08..11 dumps are
# pruned (async-timing snapshots, not features-work input).
#
# Env overrides: HYPERCHUNK_RUN_DIR, HYPERCHUNK_SEED, HYPERCHUNK_PORT — as in
# make_stage_dumps.sh. Radius is fixed at 2, bg threads at 1 (the validity
# argument depends on both).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
VER="$(cat "$ROOT/TARGET_VERSION")"
SEED="${HYPERCHUNK_SEED:-1234567890}"
PORT="${HYPERCHUNK_PORT:-25600}"
RADIUS=2
GRID_RADIUS=1
RUN_DIR="${HYPERCHUNK_RUN_DIR:-$HERE/work/featuretrace-run}"
DUMP_DIR="$ROOT/golden/features-trace/seed$SEED"
PRIMARY_DIR="$ROOT/golden/stages/seed$SEED"
PRIMARY_SUMS="$ROOT/golden/SHA256SUMS"
SUMS="$ROOT/golden/features-trace/SHA256SUMS"
LOG="$RUN_DIR/server.log"
LOADER_VERSION="${FABRIC_LOADER_VERSION:-0.19.3}"
INSTALLER_VERSION="${FABRIC_INSTALLER_VERSION:-1.1.2}"
BOOT_TIMEOUT=300
DUMP_TIMEOUT=900

EXPECTED_CHUNKS=$(( (2 * RADIUS + 1) * (2 * RADIUS + 1) ))

[ -f "$PRIMARY_DIR/order.manifest" ] || {
    echo "FATAL: no committed primary manifest at $PRIMARY_DIR/order.manifest" >&2; exit 1; }
[ -f "$PRIMARY_SUMS" ] || { echo "FATAL: no $PRIMARY_SUMS" >&2; exit 1; }

"$HERE/fetch_server.sh"
"$HERE/extract_nested.sh" > /dev/null
"$HERE/fetch_fabric.sh"

echo "== building stage-dump mod (with feature-trace hooks)"
(cd "$HERE/stage-dump-mod" && ./gradlew --no-daemon -q build)
MOD_JAR="$HERE/stage-dump-mod/build/libs/hyperchunk-stagedump.jar"
[ -f "$MOD_JAR" ] || { echo "FATAL: mod jar not built" >&2; exit 1; }

echo "== installing fabric server (loader $LOADER_VERSION) into $RUN_DIR"
rm -rf "$RUN_DIR"
mkdir -p "$RUN_DIR"
cd "$RUN_DIR"
cp "$HERE/libs/server-$VER.jar" server.jar
java -jar "$HERE/libs/fabric/fabric-installer-$INSTALLER_VERSION.jar" server \
    -dir "$RUN_DIR" -mcversion "$VER" -loader "$LOADER_VERSION" > installer.log 2>&1 \
    || { echo "FATAL: fabric installer failed" >&2; cat installer.log >&2; exit 1; }
[ -f fabric-server-launch.jar ] || { echo "FATAL: no fabric-server-launch.jar" >&2; cat installer.log >&2; exit 1; }

mkdir -p mods
cp "$MOD_JAR" mods/

echo "eula=true" > eula.txt
cat > server.properties <<EOF
level-seed=$SEED
level-type=minecraft\\:normal
online-mode=false
max-players=1
view-distance=2
simulation-distance=2
difficulty=peaceful
spawn-monsters=false
server-port=$PORT
pause-when-empty-seconds=-1
enable-rcon=false
EOF

rm -rf "$DUMP_DIR"
mkdir -p "$DUMP_DIR"

mkfifo console.in
java -Xms2G -Xmx4G -Dmax.bg.threads=1 \
    -Dhyperchunk.dump.dir="$DUMP_DIR" \
    -Dhyperchunk.dump.centerX=0 \
    -Dhyperchunk.dump.centerZ=0 \
    -Dhyperchunk.dump.radius="$RADIUS" \
    -Dhyperchunk.dump.trace=true \
    -jar fabric-server-launch.jar nogui < console.in > "$LOG" 2>&1 &
SERVER_PID=$!
exec 3>console.in

cleanup() {
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "stop" >&3 2>/dev/null || true
        for _ in $(seq 1 30); do
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 1
        done
        kill -9 "$SERVER_PID" 2>/dev/null || true
    fi
    exec 3>&- 2>/dev/null || true
}
trap cleanup EXIT

wait_log() { # wait_log <regex> <timeout_s>
    local pattern="$1" timeout="$2" waited=0
    while ! grep -qE "$pattern" "$LOG" 2>/dev/null; do
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            echo "FATAL: server exited while waiting for: $pattern" >&2
            tail -30 "$LOG" >&2
            exit 1
        fi
        sleep 2
        waited=$((waited + 2))
        if [ "$waited" -ge "$timeout" ]; then
            echo "FATAL: timeout ($timeout s) waiting for: $pattern" >&2
            tail -30 "$LOG" >&2
            exit 1
        fi
    done
}

echo "== booting fabric server (seed $SEED, dump radius $RADIUS, traces on) ..."
wait_log 'Done \([0-9.]+s\)!' "$BOOT_TIMEOUT"
grep -m1 '\[hyperchunk-stagedump\] enabled' "$LOG" || {
    echo "FATAL: stage-dump harness did not report enabled; mixin not applied?" >&2
    tail -40 "$LOG" >&2
    exit 1
}

echo "== quieting the server and forcing generation of the 5x5"
echo "tick freeze" >&3
echo "gamerule random_tick_speed 0" >&3
echo "gamerule spawn_mobs false" >&3
echo "gamerule advance_weather false" >&3
echo "gamerule advance_time false" >&3
BLK_MIN=$(( -RADIUS * 16 ))
BLK_MAX=$(( RADIUS * 16 + 15 ))
echo "forceload add $BLK_MIN $BLK_MIN $BLK_MAX $BLK_MAX" >&3

echo "== waiting for $EXPECTED_CHUNKS chunks to dump their final (full) stage"
WAITED=0
while true; do
    GOT=$(find "$DUMP_DIR" -name '*_full.blocks.txt' 2>/dev/null | wc -l)
    FILES=$(find "$DUMP_DIR" -name '*.txt' 2>/dev/null | wc -l)
    echo "   full-stage dumps: $GOT/$EXPECTED_CHUNKS (total files: $FILES, t+${WAITED}s)"
    [ "$GOT" -ge "$EXPECTED_CHUNKS" ] && break
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "FATAL: server exited before dumps completed" >&2
        tail -30 "$LOG" >&2
        exit 1
    fi
    sleep 5
    WAITED=$((WAITED + 5))
    if [ "$WAITED" -ge "$DUMP_TIMEOUT" ]; then
        echo "FATAL: dumps incomplete after ${DUMP_TIMEOUT}s" >&2
        grep '\[hyperchunk-stagedump\]' "$LOG" | tail -20 >&2
        exit 1
    fi
done

echo "== stopping server"
echo "stop" >&3
for _ in $(seq 1 60); do
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 1
done
if kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "FATAL: server did not stop cleanly" >&2
    exit 1
fi
trap - EXIT
exec 3>&-

echo "== verifying run invariants"
MANIFEST="$DUMP_DIR/order.manifest"
[ -f "$MANIFEST" ] || { echo "FATAL: no order.manifest produced" >&2; exit 1; }
if grep -q '^# ERROR' "$MANIFEST"; then
    echo "FATAL: order.manifest contains ERROR lines:" >&2
    grep '^# ERROR' "$MANIFEST" >&2
    exit 1
fi
awk '!/^#/ { if ($1 != n++) { print "FATAL: seq gap at line " NR; exit 1 } }' "$MANIFEST" || exit 1
if grep -qE 'Detected unsafe terrain read during worldgen|Detected setBlock in a far chunk' "$LOG"; then
    echo "FATAL: out-of-window worldgen access in server log" >&2
    exit 1
fi
if grep -q 'ERROR writing trace\|ERROR trace armed' "$LOG"; then
    echo "FATAL: trace writer errors in server log" >&2
    grep 'ERROR writing trace\|ERROR trace armed' "$LOG" | head >&2
    exit 1
fi

echo "== verifying grid stickiness vs committed primary bundle"
# (a) manifest prefix: the 9 grid entries must be the run's first 9, in the
#     committed order with the committed seeds (fields 1-4).
if ! diff <(grep -v '^#' "$PRIMARY_DIR/order.manifest" | head -9 | awk '{print $1,$2,$3,$4}') \
          <(grep -v '^#' "$MANIFEST" | head -9 | awk '{print $1,$2,$3,$4}') >&2; then
    echo "FATAL: this run did not reproduce the committed grid decoration order;" >&2
    echo "       traces are NOT valid goldens for the primary bundle. Aborting." >&2
    exit 1
fi
# (b) every committed 01..07 dump hash of the 9 grid chunks must match this
#     run's file bytes.
MISMATCH=0
CHECKED=0
while read -r HASH REL; do
    case "$REL" in
        stages/seed$SEED/c.*/0[1-7]_*) ;;
        *) continue ;;
    esac
    NEW="$DUMP_DIR/${REL#stages/seed$SEED/}"
    if [ ! -f "$NEW" ]; then
        echo "FATAL: run is missing grid dump $NEW" >&2
        MISMATCH=$((MISMATCH + 1))
        continue
    fi
    GOT=$(sha256sum "$NEW" | cut -d' ' -f1)
    if [ "$GOT" != "$HASH" ]; then
        echo "FATAL: grid dump differs from committed bundle: $REL" >&2
        MISMATCH=$((MISMATCH + 1))
    fi
    CHECKED=$((CHECKED + 1))
done < "$PRIMARY_SUMS"
[ "$CHECKED" -gt 0 ] || { echo "FATAL: no grid 01..07 hashes found in $PRIMARY_SUMS" >&2; exit 1; }
if [ "$MISMATCH" -ne 0 ]; then
    echo "FATAL: $MISMATCH/$CHECKED grid dumps mismatched — sticky-order assumption broke." >&2
    exit 1
fi
echo "   grid stickiness OK: $CHECKED committed 01..07 dump hashes reproduced byte-exactly"
# (c) full-manifest comparison (informational — decides how far the traces
#     are coherent with the committed bundle beyond the grid).
if diff -q <(grep -v '^#' "$PRIMARY_DIR/order.manifest" | awk '{print $1,$2,$3,$4}') \
           <(grep -v '^#' "$MANIFEST" | awk '{print $1,$2,$3,$4}') > /dev/null; then
    echo "   full manifest identical to committed primary (all entries) — ring traces coherent too"
    FULL_MATCH=1
else
    echo "   full manifest differs beyond the grid prefix — ring traces coherent only with THIS run"
    FULL_MATCH=0
fi

echo "== verifying traces"
MANIFEST_LINES=$(grep -vc '^#' "$MANIFEST")
TRACES=$(find "$DUMP_DIR/traces" -name 'c.*.trace.txt' | wc -l)
[ "$TRACES" -eq "$MANIFEST_LINES" ] || {
    echo "FATAL: $TRACES trace files for $MANIFEST_LINES manifest entries" >&2; exit 1; }
while read -r SEQ CX CZ SEEDHEX _; do
    T="$DUMP_DIR/traces/c.$CX.$CZ.trace.txt"
    [ -f "$T" ] || { echo "FATAL: missing trace $T" >&2; exit 1; }
    grep -q "^begin $CX $CZ $SEEDHEX\$" "$T" || {
        echo "FATAL: $T begin line missing or seed mismatch (want $SEEDHEX)" >&2; exit 1; }
    tail -1 "$T" | grep -q "^end $CX $CZ " || {
        echo "FATAL: $T not properly terminated" >&2; exit 1; }
done < <(grep -v '^#' "$MANIFEST")
# structure expectation: none in the grid (Task 9a scope assumption)
GRID_S=0
RING_S=0
for CX in $(seq "-$GRID_RADIUS" "$GRID_RADIUS"); do
    for CZ in $(seq "-$GRID_RADIUS" "$GRID_RADIUS"); do
        N=$(grep -c '^s ' "$DUMP_DIR/traces/c.$CX.$CZ.trace.txt" || true)
        GRID_S=$((GRID_S + N))
    done
done
RING_S=$(cat "$DUMP_DIR"/traces/c.*.trace.txt | grep -c '^s ' || true)
RING_S=$((RING_S - GRID_S))
if [ "$GRID_S" -ne 0 ]; then
    echo "FATAL: $GRID_S structure placements in GRID traces — 'no structures in grid' assumption broke" >&2
    grep -l '^s ' "$DUMP_DIR"/traces/c.*.trace.txt >&2
    exit 1
fi
echo "   traces OK: $TRACES files, begin/end + seed cross-check passed, 0 grid structure placements ($RING_S elsewhere)"

echo "== pruning verified-duplicate and out-of-scope dumps"
# grid chunk dirs: byte-identical to the committed bundle (verified above)
for CX in $(seq "-$GRID_RADIUS" "$GRID_RADIUS"); do
    for CZ in $(seq "-$GRID_RADIUS" "$GRID_RADIUS"); do
        rm -rf "$DUMP_DIR/c.$CX.$CZ"
    done
done
# ring chunks: keep 01..07 (features-work inputs + 9b ring gate); drop async
# 08..11 snapshots
find "$DUMP_DIR" -name '*.txt' -path '*/c.*' \
    | grep -E '/(08|09|10|11)_' | xargs -r rm -f

echo "== writing $SUMS"
mkdir -p "$(dirname "$SUMS")"
(cd "$ROOT/golden/features-trace" \
    && find "seed$SEED" \( -name '*.txt' -o -name 'order.manifest' -o -name 'order.snapshots' \) \
       | sort | xargs sha256sum) > "$SUMS"

echo "== feature-trace golden complete"
echo "   dir:        $DUMP_DIR"
echo "   traces:     $TRACES (grid structure placements: 0)"
echo "   ring dumps: $(find "$DUMP_DIR" -path '*/c.*' -name '*.txt' | wc -l) files (stages 01..07, 16 ring chunks)"
echo "   manifest:   $MANIFEST_LINES applications; full match vs committed: $FULL_MATCH"
echo "   hashes:     $SUMS ($(wc -l < "$SUMS") entries)"
