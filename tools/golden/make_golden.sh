#!/usr/bin/env bash
# Golden region baseline (Plan Task 2, strategy C).
#
# One command: boots the pinned vanilla 26.2 server with level-seed=1234567890,
# forces generation of every chunk in region (0,0) to status minecraft:full,
# captures world/region/r.0.0.mca into golden/seed1234567890_r.0.0.mca
# (gitignored) and records its sha256 (raw + canonical payload) in
# golden/SHA256SUMS (tracked).
#
# Ticking is suppressed as hard as vanilla allows (tick freeze + gamerules)
# so the region is pure worldgen output, not worldgen + N ticks of mutation.
# Determinism across runs is measured by compare_regions.py — see NOTES.md.
#
# Env overrides:
#   HYPERCHUNK_RUN_DIR   work dir (default tools/golden/work/golden-run)
#   HYPERCHUNK_SEED      level seed (default 1234567890)
#   HYPERCHUNK_PORT      server port (default 25599)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
VER="$(cat "$ROOT/TARGET_VERSION")"
JAR="$HERE/libs/server-$VER.jar"
SEED="${HYPERCHUNK_SEED:-1234567890}"
PORT="${HYPERCHUNK_PORT:-25599}"
RUN_DIR="${HYPERCHUNK_RUN_DIR:-$HERE/work/golden-run}"
GOLDEN_DIR="$ROOT/golden"
LOG="$RUN_DIR/server.log"
BOOT_TIMEOUT=300
GEN_TIMEOUT=1800

"$HERE/fetch_server.sh"

rm -rf "$RUN_DIR"
mkdir -p "$RUN_DIR" "$GOLDEN_DIR"
cd "$RUN_DIR"

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

mkfifo console.in
# -Dmax.bg.threads=1 pins Mojang's background worker pool (chunk generation,
# light engine) to a single thread. With the default multi-threaded pool the
# ORDER in which neighboring chunks run their `features` stage is scheduler-
# dependent, and vanilla decoration reads neighbor state (heightmaps, placed
# blocks) across chunk borders — so even the same seed produces different
# block content run to run. Single-threaded generation pins that order.
# Measured evidence in tools/golden/NOTES.md.
java -Xms2G -Xmx4G -Dmax.bg.threads=1 -jar "$JAR" nogui < console.in > "$LOG" 2>&1 &
SERVER_PID=$!
# Keep the fifo writer open for the whole session.
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

send() {
    echo "$1" >&3
}

wait_log() { # wait_log <regex> <timeout_s>
    local pattern="$1" timeout="$2" waited=0
    while ! grep -qE "$pattern" "$LOG" 2>/dev/null; do
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            echo "FATAL: server exited while waiting for: $pattern" >&2
            tail -20 "$LOG" >&2
            exit 1
        fi
        sleep 2
        waited=$((waited + 2))
        if [ "$waited" -ge "$timeout" ]; then
            echo "FATAL: timeout ($timeout s) waiting for: $pattern" >&2
            tail -20 "$LOG" >&2
            exit 1
        fi
    done
}

echo "== booting vanilla $VER (seed $SEED) ..."
wait_log 'Done \([0-9.]+s\)!' "$BOOT_TIMEOUT"

echo "== freezing ticks and disabling world mutation sources"
# 26.2 renamed the gamerules: randomTickSpeed -> random_tick_speed,
# doMobSpawning -> spawn_mobs, doWeatherCycle -> advance_weather,
# doDaylightCycle -> advance_time (verified against the live server).
send "tick freeze"
send "gamerule random_tick_speed 0"
send "gamerule spawn_mobs false"
send "gamerule advance_weather false"
send "gamerule advance_time false"
wait_log 'Gamerule advance_time is now set to: false' 60

echo "== forceloading all 1024 chunks of region (0,0)"
send "forceload add 0 0 255 255"
send "forceload add 256 0 511 255"
send "forceload add 0 256 255 511"
send "forceload add 256 256 511 511"
FL_COUNT=0
WAITED=0
while [ "$FL_COUNT" -lt 4 ]; do
    FL_COUNT=$(grep -cE 'to be force[- ]?loaded|force loaded' "$LOG" || true)
    sleep 2
    WAITED=$((WAITED + 2))
    if [ "$WAITED" -ge 120 ]; then
        echo "FATAL: forceload commands not acknowledged; log tail:" >&2
        tail -20 "$LOG" >&2
        exit 1
    fi
done

echo "== waiting for all 1024 chunks to reach minecraft:full"
# 26.2 world layout: regions live under dimensions/<ns>/<dim>/region/.
MCA="$RUN_DIR/world/dimensions/minecraft/overworld/region/r.0.0.mca"
WAITED=0
while true; do
    SAVES_BEFORE=$(grep -c 'Saved the game' "$LOG" || true)
    send "save-all flush"
    while [ "$(grep -c 'Saved the game' "$LOG" || true)" -le "$SAVES_BEFORE" ]; do
        sleep 2
        WAITED=$((WAITED + 2))
        [ "$WAITED" -ge "$GEN_TIMEOUT" ] && { echo "FATAL: save-all timeout" >&2; exit 1; }
    done
    STATS="$(python3 "$HERE/region_stats.py" "$MCA" 2>/dev/null || echo 'present 0')"
    FULL=$(echo "$STATS" | awk '$1=="status" && $2=="minecraft:full" {print $3}')
    PRESENT=$(echo "$STATS" | awk '$1=="present" {print $2}')
    echo "   present=$PRESENT full=${FULL:-0} (t+${WAITED}s)"
    if [ "${FULL:-0}" = "1024" ]; then
        break
    fi
    sleep 10
    WAITED=$((WAITED + 10))
    if [ "$WAITED" -ge "$GEN_TIMEOUT" ]; then
        echo "FATAL: generation did not finish in ${GEN_TIMEOUT}s; stats:" >&2
        echo "$STATS" >&2
        exit 1
    fi
done

echo "== final flush + stop"
send "save-all flush"
sleep 5
send "stop"
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

OUT="$GOLDEN_DIR/seed${SEED}_r.0.0.mca"
cp "$MCA" "$OUT"

RAW_SHA=$(sha256sum "$OUT" | cut -d' ' -f1)
CANON_SHA=$(python3 "$HERE/compare_regions.py" --canonical-hash "$OUT" | cut -d' ' -f1)

# Update golden/SHA256SUMS in place: one line per artifact, newest wins.
SUMS="$GOLDEN_DIR/SHA256SUMS"
touch "$SUMS"
grep -v "seed${SEED}_r.0.0.mca" "$SUMS" > "$SUMS.tmp" || true
{
    cat "$SUMS.tmp"
    echo "$RAW_SHA  seed${SEED}_r.0.0.mca"
    echo "$CANON_SHA  seed${SEED}_r.0.0.mca#canonical-payload"
} | sort -k2 > "$SUMS"
rm -f "$SUMS.tmp"

echo "== golden region captured"
echo "   file:      $OUT"
echo "   raw sha256:       $RAW_SHA"
echo "   canonical sha256: $CANON_SHA   (chunk payloads, LastUpdate masked)"
python3 "$HERE/region_stats.py" "$OUT" | grep -E 'present|compression'
