#!/usr/bin/env bash
# Stage-dump harness (Plan Task 2, strategy A — the core deliverable).
#
# End to end, one command:
#   1. fetch + verify the pinned vanilla server jar (fetch_server.sh)
#   2. extract the nested unobfuscated server jar (extract_nested.sh)
#   3. fetch the pinned Fabric toolchain (fetch_fabric.sh)
#   4. build the mixin mod with the included gradle wrapper
#   5. install a Fabric server into a scratch dir (fabric-installer)
#   6. run it on a FRESH world (level-seed=1234567890) with the harness
#      enabled, forceload chunk (0,0) + its 3x3 neighborhood to FULL
#   7. collect per-(chunk,stage) dumps under golden/stages/seed<seed>/ plus
#      order.manifest — the features-stage execution order of THIS run
#      (ADR-007 Tier-2 replay input; see .hermes/notes/task9pre-order/)
#   8. record every dump file's sha256 in golden/SHA256SUMS
#      (or golden/stages-alt/SHA256SUMS when dumping an alt bundle)
#
# The dump format is documented in golden/stages/FORMAT.md (tracked).
#
# NOTE (ADR-007): stages 01-06 are order-free and must be byte-identical
# across runs; 07_features+ depend on the recorded order and are expected to
# differ run to run. A regenerated bundle REPLACES the old one as a coherent
# (dumps + order.manifest) pair.
#
# Env overrides:
#   HYPERCHUNK_RUN_DIR     scratch dir (default tools/golden/work/stagedump-run)
#   HYPERCHUNK_SEED        level seed (default 1234567890)
#   HYPERCHUNK_PORT        server port (default 25600)
#   HYPERCHUNK_RADIUS      chunk radius around (0,0) (default 1 => 3x3)
#   HYPERCHUNK_BG_THREADS  -Dmax.bg.threads value (default 1). >1 gives the
#                          scheduler more freedom => more order variation for
#                          alt bundles (ADR-007 D3 wants distinct orders); the
#                          manifest stays a total order either way because 26.2
#                          serializes step bodies per dimension (recon A5)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
VER="$(cat "$ROOT/TARGET_VERSION")"
SEED="${HYPERCHUNK_SEED:-1234567890}"
PORT="${HYPERCHUNK_PORT:-25600}"
RADIUS="${HYPERCHUNK_RADIUS:-1}"
RUN_DIR="${HYPERCHUNK_RUN_DIR:-$HERE/work/stagedump-run}"
BG_THREADS="${HYPERCHUNK_BG_THREADS:-1}"
DUMP_DIR="${HYPERCHUNK_DUMP_DIR:-$ROOT/golden/stages/seed$SEED}"
GOLDEN_DIR="$ROOT/golden"
LOG="$RUN_DIR/server.log"
LOADER_VERSION="${FABRIC_LOADER_VERSION:-0.19.3}"
INSTALLER_VERSION="${FABRIC_INSTALLER_VERSION:-1.1.2}"
BOOT_TIMEOUT=300
DUMP_TIMEOUT=900

EXPECTED_CHUNKS=$(( (2 * RADIUS + 1) * (2 * RADIUS + 1) ))

"$HERE/fetch_server.sh"
"$HERE/extract_nested.sh" > /dev/null
"$HERE/fetch_fabric.sh"

echo "== building stage-dump mod (gradle wrapper, no loom — 26.2 is unobfuscated)"
(cd "$HERE/stage-dump-mod" && ./gradlew --no-daemon -q build)
MOD_JAR="$HERE/stage-dump-mod/build/libs/hyperchunk-stagedump.jar"
[ -f "$MOD_JAR" ] || { echo "FATAL: mod jar not built" >&2; exit 1; }

echo "== installing fabric server (loader $LOADER_VERSION) into $RUN_DIR"
rm -rf "$RUN_DIR"
mkdir -p "$RUN_DIR"
cd "$RUN_DIR"
# Reuse the sha1-verified vanilla bundler jar instead of -downloadMinecraft.
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
# max.bg.threads=1 (default) reduces (but does not eliminate — NOTES.md)
# run-to-run order variance; the order.manifest records whatever order this
# run actually used, so higher values are equally valid golden inputs.
java -Xms2G -Xmx4G -Dmax.bg.threads="$BG_THREADS" \
    -Dhyperchunk.dump.dir="$DUMP_DIR" \
    -Dhyperchunk.dump.centerX=0 \
    -Dhyperchunk.dump.centerZ=0 \
    -Dhyperchunk.dump.radius="$RADIUS" \
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

echo "== booting fabric server (seed $SEED, dump radius $RADIUS) ..."
wait_log 'Done \([0-9.]+s\)!' "$BOOT_TIMEOUT"
grep -m1 '\[hyperchunk-stagedump\] enabled' "$LOG" || {
    echo "FATAL: stage-dump harness did not report enabled; mixin not applied?" >&2
    tail -40 "$LOG" >&2
    exit 1
}

echo "== quieting the server and forcing generation of the target chunks"
# 26.2 gamerule names (see make_golden.sh for the rename map)
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

Y_RANGE="$(grep -m1 'observed y-range' "$LOG" | sed 's/.*observed y-range: //')"
echo "== observed y-range: ${Y_RANGE:-<not reported>}"

echo "== verifying order.manifest"
MANIFEST="$DUMP_DIR/order.manifest"
[ -f "$MANIFEST" ] || { echo "FATAL: no order.manifest produced" >&2; exit 1; }
if grep -q '^# ERROR' "$MANIFEST"; then
    echo "FATAL: order.manifest contains ERROR lines (seed capture missed):" >&2
    grep '^# ERROR' "$MANIFEST" >&2
    exit 1
fi
# seq must be dense ascending from 0 in file order (assigned under the writer lock)
awk '!/^#/ { if ($1 != n++) { print "FATAL: seq gap at line " NR ": got " $1 " want " n-1; exit 1 } }' "$MANIFEST" || exit 1
# every grid chunk decorates exactly once
for CX in $(seq "-$RADIUS" "$RADIUS"); do
    for CZ in $(seq "-$RADIUS" "$RADIUS"); do
        N=$(awk -v x="$CX" -v z="$CZ" '!/^#/ && $2==x && $3==z {n++} END{print n+0}' "$MANIFEST")
        [ "$N" -eq 1 ] || { echo "FATAL: chunk ($CX,$CZ) has $N manifest lines (want 1)" >&2; exit 1; }
    done
done
# decoration seed of chunk (0,0) degenerates to the level seed (block origin
# 0,0 — recon A3), so it is checkable without reimplementing the RNG here
if [[ "$SEED" =~ ^[0-9]+$ ]]; then
    WANT=$(printf '%016x' "$SEED")
    GOT=$(awk '!/^#/ && $2==0 && $3==0 {print $4}' "$MANIFEST")
    [ "$GOT" = "$WANT" ] || { echo "FATAL: chunk (0,0) decoration seed $GOT != level seed $WANT" >&2; exit 1; }
fi
# out-of-window worldgen access would falsify the 3x3-window sufficiency
# argument (recon A4) — vanilla logs these; require none
if grep -qE 'Detected unsafe terrain read during worldgen|Detected setBlock in a far chunk' "$LOG"; then
    echo "FATAL: out-of-window worldgen access in server log:" >&2
    grep -E 'Detected unsafe terrain read during worldgen|Detected setBlock in a far chunk' "$LOG" | head >&2
    exit 1
fi
MANIFEST_LINES=$(grep -vc '^#' "$MANIFEST")
echo "   order.manifest OK: $MANIFEST_LINES features applications, grid covered, seq dense, (0,0) seed check passed"

echo "== verifying order.snapshots"
SNAPSHOTS="$DUMP_DIR/order.snapshots"
[ -f "$SNAPSHOTS" ] || { echo "FATAL: no order.snapshots produced" >&2; exit 1; }
SNAP_LINES=$(grep -vc '^#' "$SNAPSHOTS")
WANT_SNAPS=$((EXPECTED_CHUNKS * 11)) # 11 generation-pyramid stages per dumped chunk
[ "$SNAP_LINES" -eq "$WANT_SNAPS" ] || { echo "FATAL: $SNAP_LINES snapshot lines, want $WANT_SNAPS" >&2; exit 1; }
# 07 dumps run synchronously inside the serialized features body: they must
# be clean (seqBegin == seqEnd) and sit right after their own manifest event
awk 'NR==FNR { if (!/^#/) m[$2","$3]=$1; next }
     !/^#/ && $1 ~ /^07_/ {
       if ($4 != $5) { print "FATAL: torn 07 snapshot: " $0; bad=1 }
       if (m[$2","$3]+1 != $4) { print "FATAL: 07 snapshot not at own manifest event: " $0 " (manifest seq " m[$2","$3] ")"; bad=1 }
     } END { exit bad }' "$MANIFEST" "$SNAPSHOTS" || exit 1
TORN=$(awk '!/^#/ && $4 != $5' "$SNAPSHOTS" | wc -l)
echo "   order.snapshots OK: $SNAP_LINES snapshots, 07 invariant holds, $TORN torn (async stages only)"

SUMS=""
if [ "$DUMP_DIR" = "$ROOT/golden/stages/seed$SEED" ]; then
    SUMS="$GOLDEN_DIR/SHA256SUMS"; SUMS_BASE="$GOLDEN_DIR"; SUMS_PREFIX="stages/seed$SEED"
elif [ "$DUMP_DIR" = "$ROOT/golden/stages-alt/seed$SEED" ]; then
    SUMS="$ROOT/golden/stages-alt/SHA256SUMS"; SUMS_BASE="$ROOT/golden/stages-alt"; SUMS_PREFIX="seed$SEED"
fi
if [ -n "$SUMS" ]; then
    echo "== updating $SUMS with dump + manifest hashes"
    touch "$SUMS"
    grep -v "^[0-9a-f]*  $SUMS_PREFIX/" "$SUMS" > "$SUMS.tmp" || true
    (cd "$SUMS_BASE" && find "$SUMS_PREFIX" \( -name '*.txt' -o -name 'order.manifest' -o -name 'order.snapshots' \) -not -name 'FORMAT*' | sort | xargs sha256sum) >> "$SUMS.tmp"
    sort -k2 "$SUMS.tmp" > "$SUMS"
    rm -f "$SUMS.tmp"
else
    echo "== custom HYPERCHUNK_DUMP_DIR; skipping SHA256SUMS update"
fi

DUMPED_STAGES=$(find "$DUMP_DIR" -name '*.blocks.txt' -printf '%f\n' | sed 's/\.blocks\.txt//' | sort -u | tr '\n' ' ')
echo "== stage dumps complete"
echo "   dir:      $DUMP_DIR"
echo "   chunks:   $(find "$DUMP_DIR" -maxdepth 1 -type d -name 'c.*' | wc -l)"
echo "   stages:   $DUMPED_STAGES"
echo "   files:    $(find "$DUMP_DIR" -name '*.txt' | wc -l) (hashes: ${SUMS:-not updated})"
echo "   manifest: $MANIFEST_LINES applications, sha256 $(sha256sum "$MANIFEST" | cut -d' ' -f1)"
echo "   snapshots: $SNAP_LINES dump events, $TORN torn"
