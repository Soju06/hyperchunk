#!/usr/bin/env bash
# Determinism experiment: does STRICTLY SEQUENTIAL chunk generation make
# vanilla 26.2 worldgen output run-deterministic?
#
# Background (see tools/golden/NOTES.md): with the default chunk system —
# and even with -Dmax.bg.threads=1 — the order in which neighboring chunks
# run their `features` stage varies run to run, and vanilla decoration reads
# neighbor state across chunk borders, so world CONTENT differs between runs
# of the same seed. This probe forces chunks to full ONE AT A TIME in fixed
# row-major order, using the stage-dump mod's "dumped c.x.z/full" log line
# as the per-chunk completion signal, then compares two such runs.
#
# Usage: sequential_probe.sh <run-dir> <dump-dir> [gridN=7] [port=25601]
# The caller runs it twice with different dirs and diffs the results.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # tools/golden
ROOT="$(cd "$HERE/../.." && pwd)"
VER="$(cat "$ROOT/TARGET_VERSION")"
RUN_DIR="$1"
DUMP_DIR="$2"
GRID="${3:-7}"
PORT="${4:-25601}"
SEED=1234567890
LOADER_VERSION=0.19.3
INSTALLER_VERSION=1.1.2
LOG="$RUN_DIR/server.log"

CENTER=$(( GRID / 2 ))

"$HERE/fetch_server.sh" >/dev/null
"$HERE/fetch_fabric.sh" 2>/dev/null
(cd "$HERE/stage-dump-mod" && ./gradlew --no-daemon -q build)

rm -rf "$RUN_DIR" "$DUMP_DIR"
mkdir -p "$RUN_DIR" "$DUMP_DIR"
cd "$RUN_DIR"
cp "$HERE/libs/server-$VER.jar" server.jar
java -jar "$HERE/libs/fabric/fabric-installer-$INSTALLER_VERSION.jar" server \
    -dir "$RUN_DIR" -mcversion "$VER" -loader "$LOADER_VERSION" > installer.log 2>&1
mkdir -p mods
cp "$HERE/stage-dump-mod/build/libs/hyperchunk-stagedump.jar" mods/

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
EOF

mkfifo console.in
java -Xms2G -Xmx4G -Dmax.bg.threads=1 \
    -Dhyperchunk.dump.dir="$DUMP_DIR" \
    -Dhyperchunk.dump.centerX="$CENTER" \
    -Dhyperchunk.dump.centerZ="$CENTER" \
    -Dhyperchunk.dump.radius="$CENTER" \
    -jar fabric-server-launch.jar nogui < console.in > "$LOG" 2>&1 &
PID=$!
exec 3>console.in
cleanup() {
    if kill -0 "$PID" 2>/dev/null; then
        echo "stop" >&3 2>/dev/null || true
        sleep 10
        kill -9 "$PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

wait_log() {
    local pattern="$1" timeout="$2" waited=0
    while ! grep -qE "$pattern" "$LOG" 2>/dev/null; do
        kill -0 "$PID" 2>/dev/null || { echo "server died waiting for $pattern" >&2; exit 1; }
        sleep 1
        waited=$((waited + 1))
        # NB: must be if/fi — `[ cond ] && {}` returns 1 under set -e when false
        if [ "$waited" -ge "$timeout" ]; then
            echo "timeout waiting for $pattern" >&2
            tail -5 "$LOG" >&2
            exit 1
        fi
    done
}

wait_log 'Done \([0-9.]+s\)!' 300
echo "tick freeze" >&3
echo "gamerule random_tick_speed 0" >&3
echo "gamerule spawn_mobs false" >&3
echo "gamerule advance_weather false" >&3
echo "gamerule advance_time false" >&3
wait_log 'Gamerule advance_time is now set to: false' 60

echo "sequentially forcing ${GRID}x${GRID} chunks..."
for (( z=0; z<GRID; z++ )); do
    for (( x=0; x<GRID; x++ )); do
        echo "forceload add $((x * 16)) $((z * 16))" >&3
        wait_log "dumped c\\.$x\\.$z/full" 180
    done
done

echo "save-all flush" >&3
wait_log 'Saved the game' 120
echo "stop" >&3
for _ in $(seq 1 60); do kill -0 "$PID" 2>/dev/null || break; sleep 1; done
trap - EXIT
exec 3>&-
echo "done: dumps in $DUMP_DIR, region in $RUN_DIR/world/dimensions/minecraft/overworld/region/"
