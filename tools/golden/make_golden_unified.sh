#!/usr/bin/env bash
# Unified golden capture (Task 13) — ONE server session produces a COHERENT
# bundle:
#   (a) r.0.0 full-coverage .mca            (seed 1234567890, 1024 chunks full)
#   (b) order.manifest + order.snapshots    (this session's features order)
#   (c) 11-stage dumps for the 3x3 grid     (this session's snapshots)
#   (d) autosave + unload suppression       (-Dhyperchunk.dump.nosave=true →
#       ServerLevelMixin forces noSave from t=0: no periodic autosave chunk
#       writes, no processUnloads, no eager saves — the mid-carve save race
#       class of tools/golden/NOTES.md "Recording-lifecycle artifacts" cannot
#       occur; the single save is the explicit `save-all flush` at the end,
#       force=true bypasses noSave)
#   (e) postprocess.manifest                (postProcessGeneration promotion
#       order + marked positions — fluid_ticks list order hangs off this;
#       R-D-bytecode-ticks.md §3)
#
# The 07-28 golden .mca and the 07-31 stage bundles were products of DIFFERENT
# server runs (stale-mca finding, .hermes/notes/task12-region/A-task12-*.md);
# this harness exists so that .mca <-> manifest <-> dumps are one run.
#
# GOLDEN REPLACEMENT IS NOT DONE HERE. This script only captures into the
# work dir + declared dump/mca destinations and verifies structural
# invariants. Cross-validation (mca<->manifest coherence) is a separate step:
# tools/golden/check_capture_coherence.py + the C replay gates. Only replace
# golden/ + SHA256SUMS after that passes (Task-13 절대 규칙).
#
# Env overrides:
#   HYPERCHUNK_RUN_DIR      scratch dir (default tools/golden/work/unified-run)
#   HYPERCHUNK_SEED         level seed (default 1234567890)
#   HYPERCHUNK_PORT         server port (default 25601)
#   HYPERCHUNK_RADIUS       dump-grid chebyshev radius (default 1 => 3x3)
#   HYPERCHUNK_BG_THREADS   -Dmax.bg.threads (default 1; alt bundles use >1)
#   HYPERCHUNK_DUMP_DIR     bundle destination
#                           (default <repo>/tools/golden/work/unified-bundle)
#   HYPERCHUNK_CAPTURE_MCA  1 (default) = forceload region (0,0) to full and
#                           capture r.0.0.mca; 0 = grid-only (alt bundle mode)
#   HYPERCHUNK_MCA_OUT      .mca destination (default $RUN_DIR/r.0.0.captured.mca)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
VER="$(cat "$ROOT/TARGET_VERSION")"
SEED="${HYPERCHUNK_SEED:-1234567890}"
PORT="${HYPERCHUNK_PORT:-25601}"
RADIUS="${HYPERCHUNK_RADIUS:-1}"
RUN_DIR="${HYPERCHUNK_RUN_DIR:-$HERE/work/unified-run}"
BG_THREADS="${HYPERCHUNK_BG_THREADS:-1}"
DUMP_DIR="${HYPERCHUNK_DUMP_DIR:-$HERE/work/unified-bundle}"
CAPTURE_MCA="${HYPERCHUNK_CAPTURE_MCA:-1}"
MCA_OUT="${HYPERCHUNK_MCA_OUT:-$RUN_DIR/r.0.0.captured.mca}"
LOG="$RUN_DIR/server.log"
LOADER_VERSION="${FABRIC_LOADER_VERSION:-0.19.3}"
INSTALLER_VERSION="${FABRIC_INSTALLER_VERSION:-1.1.2}"
BOOT_TIMEOUT=300
DUMP_TIMEOUT=900
GEN_TIMEOUT=3600

EXPECTED_CHUNKS=$(( (2 * RADIUS + 1) * (2 * RADIUS + 1) ))

"$HERE/fetch_server.sh"
"$HERE/extract_nested.sh" > /dev/null
"$HERE/fetch_fabric.sh"

echo "== building stage-dump mod"
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
[ -f fabric-server-launch.jar ] || { echo "FATAL: no fabric-server-launch.jar" >&2; exit 1; }

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
java -Xms2G -Xmx8G -Dmax.bg.threads="$BG_THREADS" \
    -Dhyperchunk.dump.dir="$DUMP_DIR" \
    -Dhyperchunk.dump.centerX=0 \
    -Dhyperchunk.dump.centerZ=0 \
    -Dhyperchunk.dump.radius="$RADIUS" \
    -Dhyperchunk.dump.nosave=true \
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

echo "== booting fabric server (seed $SEED, bg.threads $BG_THREADS, capture_mca $CAPTURE_MCA)"
wait_log 'Done \([0-9.]+s\)!' "$BOOT_TIMEOUT"
grep -m1 '\[hyperchunk-stagedump\] enabled' "$LOG" || {
    echo "FATAL: stage-dump harness did not report enabled" >&2
    tail -40 "$LOG" >&2
    exit 1
}
grep -m1 'noSave forced for minecraft:overworld' "$LOG" || {
    echo "FATAL: noSave mixin did not fire — autosave/unload suppression NOT active" >&2
    exit 1
}

echo "== quieting the server (26.2 gamerule names)"
echo "tick freeze" >&3
echo "gamerule random_tick_speed 0" >&3
echo "gamerule spawn_mobs false" >&3
echo "gamerule advance_weather false" >&3
echo "gamerule advance_time false" >&3
wait_log 'Gamerule advance_time is now set to: false' 60

BLK_MIN=$(( -RADIUS * 16 ))
BLK_MAX=$(( RADIUS * 16 + 15 ))
echo "forceload add $BLK_MIN $BLK_MIN $BLK_MAX $BLK_MAX" >&3

echo "== waiting for $EXPECTED_CHUNKS grid chunks to dump their final (full) stage"
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

if [ "$CAPTURE_MCA" = "1" ]; then
    echo "== forceloading all 1024 chunks of region (0,0) (4 x 256-chunk commands)"
    echo "forceload add 0 0 255 255" >&3
    echo "forceload add 256 0 511 255" >&3
    echo "forceload add 0 256 255 511" >&3
    echo "forceload add 256 256 511 511" >&3

    echo "== waiting for all 1024 region chunks to be promoted to ticking (postProcess)"
    # With noSave nothing hits disk mid-run, so progress is polled from the
    # postprocess manifest: every forceloaded chunk gets postProcessGeneration
    # at its ticking promotion (this is also what stamps the live t=5/t=2
    # tick rows into the eventual save).
    PPM="$DUMP_DIR/postprocess.manifest"
    WAITED=0
    while true; do
        DONE=$(awk '$1 ~ /^[0-9]+$/ && $2>=0 && $2<=31 && $3>=0 && $3<=31 {print $2","$3}' "$PPM" 2>/dev/null | sort -u | wc -l)
        TOTAL=$(awk '$1 ~ /^[0-9]+$/' "$PPM" 2>/dev/null | wc -l)
        echo "   region chunks postProcessed: $DONE/1024 (all-dim promotions: $TOTAL, t+${WAITED}s)"
        [ "$DONE" -ge 1024 ] && break
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            echo "FATAL: server exited during region generation" >&2
            tail -30 "$LOG" >&2
            exit 1
        fi
        sleep 10
        WAITED=$((WAITED + 10))
        if [ "$WAITED" -ge "$GEN_TIMEOUT" ]; then
            echo "FATAL: region generation/promotion did not finish in ${GEN_TIMEOUT}s" >&2
            exit 1
        fi
    done

    # Quiesce, then the ONE and ONLY save of the session.
    sleep 10
    echo "== single save-all flush (force=true bypasses noSave)"
    SAVES_BEFORE=$(grep -c 'Saved the game' "$LOG" || true)
    echo "save-all flush" >&3
    WAITED=0
    while [ "$(grep -c 'Saved the game' "$LOG" || true)" -le "$SAVES_BEFORE" ]; do
        sleep 2
        WAITED=$((WAITED + 2))
        [ "$WAITED" -ge 600 ] && { echo "FATAL: save-all flush timeout" >&2; exit 1; }
    done

    MCA="$RUN_DIR/world/dimensions/minecraft/overworld/region/r.0.0.mca"
    [ -f "$MCA" ] || { echo "FATAL: no r.0.0.mca at $MCA" >&2; find "$RUN_DIR/world" -name '*.mca' >&2 || true; exit 1; }
    STATS="$(python3 "$HERE/region_stats.py" "$MCA")"
    echo "$STATS" | sed 's/^/   /'
    FULL=$(echo "$STATS" | awk '$1=="status" && $2=="minecraft:full" {print $3}')
    if [ "${FULL:-0}" != "1024" ]; then
        echo "FATAL: saved region is not 1024 x minecraft:full" >&2
        exit 1
    fi
fi

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

if [ "$CAPTURE_MCA" = "1" ]; then
    cp "$MCA" "$MCA_OUT"
    echo "== captured mca: $MCA_OUT"
    echo "   raw sha256:       $(sha256sum "$MCA_OUT" | cut -d' ' -f1)"
    echo "   canonical sha256: $(python3 "$HERE/compare_regions.py" --canonical-hash "$MCA_OUT" | cut -d' ' -f1)"
fi

echo "== verifying order.manifest"
MANIFEST="$DUMP_DIR/order.manifest"
[ -f "$MANIFEST" ] || { echo "FATAL: no order.manifest produced" >&2; exit 1; }
if grep -q '^# ERROR' "$MANIFEST"; then
    echo "FATAL: order.manifest contains ERROR lines:" >&2
    grep '^# ERROR' "$MANIFEST" >&2
    exit 1
fi
awk '!/^#/ { if ($1 != n++) { print "FATAL: seq gap at line " NR ": got " $1 " want " n-1; exit 1 } }' "$MANIFEST" || exit 1
for CX in $(seq "-$RADIUS" "$RADIUS"); do
    for CZ in $(seq "-$RADIUS" "$RADIUS"); do
        N=$(awk -v x="$CX" -v z="$CZ" '!/^#/ && $2==x && $3==z {n++} END{print n+0}' "$MANIFEST")
        [ "$N" -eq 1 ] || { echo "FATAL: chunk ($CX,$CZ) has $N manifest lines (want 1)" >&2; exit 1; }
    done
done
if [[ "$SEED" =~ ^[0-9]+$ ]]; then
    WANT=$(printf '%016x' "$SEED")
    GOT=$(awk '!/^#/ && $2==0 && $3==0 {print $4}' "$MANIFEST")
    [ "$GOT" = "$WANT" ] || { echo "FATAL: chunk (0,0) decoration seed $GOT != level seed $WANT" >&2; exit 1; }
fi
if grep -qE 'Detected unsafe terrain read during worldgen|Detected setBlock in a far chunk' "$LOG"; then
    echo "FATAL: out-of-window worldgen access in server log" >&2
    exit 1
fi
MANIFEST_LINES=$(grep -vc '^#' "$MANIFEST")
echo "   order.manifest OK: $MANIFEST_LINES features applications"

echo "== verifying order.snapshots"
SNAPSHOTS="$DUMP_DIR/order.snapshots"
[ -f "$SNAPSHOTS" ] || { echo "FATAL: no order.snapshots produced" >&2; exit 1; }
SNAP_LINES=$(grep -vc '^#' "$SNAPSHOTS")
WANT_SNAPS=$((EXPECTED_CHUNKS * 11))
[ "$SNAP_LINES" -eq "$WANT_SNAPS" ] || { echo "FATAL: $SNAP_LINES snapshot lines, want $WANT_SNAPS" >&2; exit 1; }
awk 'NR==FNR { if (!/^#/) m[$2","$3]=$1; next }
     !/^#/ && $1 ~ /^07_/ {
       if ($4 != $5) { print "FATAL: torn 07 snapshot: " $0; bad=1 }
       if (m[$2","$3]+1 != $4) { print "FATAL: 07 snapshot not at own manifest event: " $0; bad=1 }
     } END { exit bad }' "$MANIFEST" "$SNAPSHOTS" || exit 1
TORN=$(awk '!/^#/ && $4 != $5' "$SNAPSHOTS" | wc -l)
echo "   order.snapshots OK: $SNAP_LINES snapshots, $TORN torn (async stages only)"

echo "== verifying stages.log"
SLOG="$DUMP_DIR/stages.log"
[ -f "$SLOG" ] || { echo "FATAL: no stages.log produced" >&2; exit 1; }
# every dump-grid chunk must have a recorded 09 light completion (the 09
# dump replay consumes ring+grid light completion order from this file)
for CX in $(seq "-$RADIUS" "$RADIUS"); do
    for CZ in $(seq "-$RADIUS" "$RADIUS"); do
        # v2: 완료 라인만 (제출 라인은 $1=="s" — 명시 제외)
        N=$(awk -v x="$CX" -v z="$CZ" '!/^#/ && $1 != "s" && $2=="light" && $3==x && $4==z {n++} END{print n+0}' "$SLOG")
        [ "$N" -eq 1 ] || { echo "FATAL: grid chunk ($CX,$CZ) has $N 'light' completion lines (want 1)" >&2; exit 1; }
        NS=$(awk -v x="$CX" -v z="$CZ" '$1 == "s" && $3=="light" && $4==x && $5==z {n++} END{print n+0}' "$SLOG")
        [ "$NS" -eq 1 ] || { echo "FATAL: grid chunk ($CX,$CZ) has $NS 'light' submission lines (want 1, v2)" >&2; exit 1; }
    done
done
SLOG_COMP=$(awk '!/^#/ && $1 != "s"' "$SLOG" | wc -l)
SLOG_SUB=$(awk '$1 == "s"' "$SLOG" | wc -l)
echo "   stages.log OK: $SLOG_COMP completions + $SLOG_SUB submissions (v2)"

echo "== verifying postprocess.manifest"
PPM="$DUMP_DIR/postprocess.manifest"
[ -f "$PPM" ] || { echo "FATAL: no postprocess.manifest produced" >&2; exit 1; }
awk '$1 ~ /^[0-9]+$/ { if ($1 != n++) { print "FATAL: postprocess seq gap at line " NR; exit 1 } }' "$PPM" || exit 1
PP_CHUNKS=$(awk '$1 ~ /^[0-9]+$/' "$PPM" | wc -l)
PP_POS=$(awk '$1 == "p"' "$PPM" | wc -l)
PP_DUP=$(awk '$1 ~ /^[0-9]+$/ {print $2","$3}' "$PPM" | sort | uniq -d | wc -l)
[ "$PP_DUP" -eq 0 ] || { echo "FATAL: $PP_DUP chunks promoted more than once (unload/reload happened?)" >&2; exit 1; }
echo "   postprocess.manifest OK: $PP_CHUNKS promotions (each chunk once), $PP_POS marked positions"

# Preserve the session log next to the bundle — coherence evidence.
# (named without .txt so the SHA256SUMS dump glob does not pick it up)
cp "$LOG" "$DUMP_DIR/server.log"

echo "== unified capture complete (NOT installed into golden/ — run cross-validation first)"
echo "   bundle:   $DUMP_DIR"
[ "$CAPTURE_MCA" = "1" ] && echo "   mca:      $MCA_OUT"
echo "   manifest: $MANIFEST_LINES applications; snapshots: $SNAP_LINES; postprocess: $PP_CHUNKS"
echo "   cross-validation (install gate — durable log, exit code must gate the install):"
echo "     python3 tools/golden/check_capture_coherence.py <mca> <bundle> 2>&1 \\"
echo "       | tee tools/golden/logs/coherence-<name>.log; test \${PIPESTATUS[0]} -eq 0"
