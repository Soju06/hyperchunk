#!/usr/bin/env bash
# VIZ-4 per-chunk capture runner: one timed r.0.0 generation run (vanilla | c2me).
# COPY of B-6 b6_run.sh (~/benchmarks/b6-3way/2026-08-12) per
# tools/viz/capture/vanilla-c2me-probe.md. Changes vs B-6 (only):
#   - POLL_S default 3 -> 0.1 (HC_POLL_S still overrides; gate runs use 3)
#   - per-chunk t_done TSV (chunks-$MODE-$RUNID.tsv) via .done_prev diff
#   - TREL/GEN_S at ms precision (%.3f) so 0.1s polls are distinguishable
#   - ART -> ~/benchmarks/viz-capture/2026-08-19
#
# Protocol (identical for every server under test):
#   boot -> quiet (tick freeze + gamerules, same as golden capture) ->
#   PRE census (which region chunks already full from spawn prep) ->
#   t0: forceload add x4 (256 chunks each, region (0,0)) ->
#   poll: `execute if loaded <bx> 64 <bz> run say HCB_<cx>_<cz>` every POLL_S
#         for not-yet-confirmed chunks; a chunk is done when its HCB line
#         appears in the server log ->
#   t1: first poll where all 1024 confirmed (one-sided overestimate
#       <= POLL_S + command latency) ->
#   save-all flush (EXCLUDED from timing) -> region_stats (1024 x full) ->
#   canonical hash (tools/golden/compare_regions.py) -> stop -> delete world.
#
# Usage: viz_run.sh <vanilla|c2me> <runid>
set -euo pipefail

MODE="$1"; RUNID="$2"
BASE=/tmp/b6-3way
ART="$HOME/benchmarks/viz-capture/2026-08-19"
HC="$HOME/hyperchunk"
JAVA=/usr/lib/jvm/java-25-openjdk-amd64/bin/java
SEED=1234567890
PORT=25620
RUN_DIR="$BASE/run-$MODE-$RUNID"
LOG="$RUN_DIR/server.log"
RES="$ART/results.jsonl"
PROG="$ART/progress-$MODE-$RUNID.tsv"
POLL_S="${HC_POLL_S:-0.1}"
CHUNKS_TSV="$ART/chunks-$MODE-$RUNID.tsv"
BOOT_TIMEOUT=300
GEN_TIMEOUT=3600

mkdir -p "$ART"
rm -rf "$RUN_DIR"; mkdir -p "$RUN_DIR"; cd "$RUN_DIR"

case "$MODE" in
  vanilla)
    cp "$BASE/libs/server-26.2.jar" server.jar
    LAUNCH_JAR=server.jar
    ;;
  c2me)
    cp -r "$BASE/fabric-template/." .
    mkdir -p mods
    cp "$BASE"/libs/c2me-*.jar mods/
    if [ -n "${HC_C2ME_CFG:-}" ]; then
        mkdir -p config
        cp "$HC_C2ME_CFG" config/c2me.toml
    fi
    LAUNCH_JAR=fabric-server-launch.jar
    ;;
  *) echo "FATAL: unknown mode $MODE" >&2; exit 1 ;;
esac

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
sync-chunk-writes=false
EOF

mkfifo console.in
# No -Dmax.bg.threads override by default: each server uses its own default
# worker pool. HC_BG1=1 pins a single worker (golden-capture parity check
# runs only — NOT a competitor timing).
EXTRA_JVM=()
[ "${HC_BG1:-0}" = "1" ] && EXTRA_JVM+=("-Dmax.bg.threads=1")
"$JAVA" -Xms2G -Xmx8G "${EXTRA_JVM[@]+"${EXTRA_JVM[@]}"}" -jar "$LAUNCH_JAR" nogui < console.in > "$LOG" 2>&1 &
PID=$!
exec 3>console.in

cleanup() {
    if kill -0 "$PID" 2>/dev/null; then
        echo "stop" >&3 2>/dev/null || true
        for _ in $(seq 1 30); do kill -0 "$PID" 2>/dev/null || break; sleep 1; done
        kill -9 "$PID" 2>/dev/null || true
    fi
    exec 3>&- 2>/dev/null || true
}
trap cleanup EXIT

wait_log() { # wait_log <regex> <timeout_s>
    local pattern="$1" timeout="$2" waited=0
    while ! grep -qE "$pattern" "$LOG" 2>/dev/null; do
        if ! kill -0 "$PID" 2>/dev/null; then
            echo "FATAL: server exited while waiting for: $pattern" >&2
            tail -30 "$LOG" >&2; exit 1
        fi
        sleep 1; waited=$((waited + 1))
        if [ "$waited" -ge "$timeout" ]; then
            echo "FATAL: timeout ($timeout s) waiting for: $pattern" >&2
            tail -30 "$LOG" >&2; exit 1
        fi
    done
}

now() { date +%s.%N; }

echo "== [$MODE/$RUNID] booting (seed $SEED)"
T_LAUNCH=$(now)
wait_log 'Done \([0-9.]+s\)!' "$BOOT_TIMEOUT"
T_BOOT=$(now)

echo "== quieting (26.2 gamerules, same as golden capture)"
echo "tick freeze" >&3
echo "gamerule random_tick_speed 0" >&3
echo "gamerule spawn_mobs false" >&3
echo "gamerule advance_weather false" >&3
echo "gamerule advance_time false" >&3
wait_log 'Gamerule advance_time is now set to: false' 60

# Probe mechanism self-check on a sacrificial far chunk (block 16000 ->
# chunk (1000,1000), region r.31.31 — no interaction with r.0.0). 26.2 keeps
# no spawn chunks loaded post-boot, so `if loaded ~ ~ ~` is always silent;
# a freshly forceloaded chunk is the only reliable true-condition target.
echo "== probe self-check (sacrificial chunk far outside r.0.0)"
echo 'forceload add 16000 16000' >&3
wait_log 'Marked chunk \[1000, 1000\]' 30
for _ in $(seq 1 60); do
    echo 'execute if loaded 16000 64 16000 run say HCB_SYNTAX_OK' >&3
    sleep 2
    grep -q 'HCB_SYNTAX_OK' "$LOG" && break
done
grep -q 'HCB_SYNTAX_OK' "$LOG" || { echo "FATAL: probe self-check never passed" >&2; tail -20 "$LOG" >&2; exit 1; }
echo 'forceload remove 16000 16000' >&3
if grep -qE 'Unknown or incomplete command|Incorrect argument' "$LOG"; then
    echo "FATAL: probe command rejected by server" >&2
    grep -E 'Unknown or incomplete command|Incorrect argument' "$LOG" >&2
    exit 1
fi

# Disk census: spawn selection generates+saves a few chunks at boot; if the
# world spawn landed inside r.0.0 those are pre-generated on disk (loaded
# probes cannot see them — everything is unloaded post-boot).
PRE_R00="none"
if [ -f world/dimensions/minecraft/overworld/region/r.0.0.mca ]; then
    PRE_R00=$(python3 "$HC/tools/golden/region_stats.py" world/dimensions/minecraft/overworld/region/r.0.0.mca 2>&1 | tr '\n' '; ')
fi
echo "   pre-t0 r.0.0.mca on disk: $PRE_R00"

echo "== PRE census: region chunks already loaded-full before forceload"
for CX in $(seq 0 31); do
    for CZ in $(seq 0 31); do
        printf 'execute if loaded %d 64 %d run say PRE_%d_%d\n' $((CX*16)) $((CZ*16)) "$CX" "$CZ" >&3
    done
done
sleep 5
PREDONE=$({ grep -oE 'PRE_[0-9]+_[0-9]+' "$LOG" || true; } | sort -u | wc -l)
echo "   pre-generated region chunks: $PREDONE/1024"

# Golden-capture sequence replication: generate the 3x3 dump grid to full
# BEFORE the region quadrants, exactly like make_golden_unified.sh does
# (verification runs only — changes decoration arrival order).
if [ "${HC_GRID_FIRST:-0}" = "1" ]; then
    echo "== grid-first: forceload 3x3 grid (blocks -16..31) and wait for full"
    echo "forceload add -16 -16 31 31" >&3
    for _ in $(seq 1 300); do
        for CX in -1 0 1; do for CZ in -1 0 1; do
            printf 'execute if loaded %d 64 %d run say GRD_%d_%d\n' $((CX*16)) $((CZ*16)) "$CX" "$CZ" >&3
        done; done
        sleep 2
        NGRID=$({ grep -oE 'GRD_-?[0-9]+_-?[0-9]+' "$LOG" || true; } | sort -u | wc -l)
        [ "$NGRID" -ge 9 ] && break
    done
    NGRID=$({ grep -oE 'GRD_-?[0-9]+_-?[0-9]+' "$LOG" || true; } | sort -u | wc -l)
    [ "$NGRID" -ge 9 ] || { echo "FATAL: grid never reached full" >&2; exit 1; }
    echo "   grid full: $NGRID/9"
fi

LOAD_T0=$(cut -d' ' -f1-3 /proc/loadavg)
echo "== t0: forceload region (0,0) (4 x 256-chunk commands)"
T0=$(now)
echo "forceload add 0 0 255 255" >&3
echo "forceload add 256 0 511 255" >&3
echo "forceload add 0 256 255 511" >&3
echo "forceload add 256 256 511 511" >&3

: > "$PROG"
: > "$CHUNKS_TSV"
: > "$RUN_DIR/.done"
cp /dev/null "$RUN_DIR/.done_prev"
for CX in $(seq 0 31); do for CZ in $(seq 0 31); do echo "HCB_${CX}_${CZ}"; done; done | sort > "$RUN_DIR/.all"
DONE=0
FIRST_POLL=1
while true; do
    # probe every chunk not yet confirmed (one comm pass, builtin printf only)
    comm -23 "$RUN_DIR/.all" "$RUN_DIR/.done" | while IFS=_ read -r _ CX CZ; do
        printf 'execute if loaded %d 64 %d run say HCB_%d_%d\n' $((CX*16)) $((CZ*16)) "$CX" "$CZ"
    done >&3
    sleep "$POLL_S"
    { grep -oE 'HCB_[0-9]+_[0-9]+' "$LOG" || true; } | sort -u > "$RUN_DIR/.done"
    DONE=$(wc -l < "$RUN_DIR/.done")
    TREL=$(awk -v a="$(now)" -v b="$T0" 'BEGIN{printf "%.3f", a-b}')
    printf '%s\t%s\n' "$TREL" "$DONE" >> "$PROG"
    # per-chunk t_done: chunks newly confirmed in this poll cycle
    comm -13 "$RUN_DIR/.done_prev" "$RUN_DIR/.done" | while IFS=_ read -r _ CX CZ; do
        printf '%s\t%s\t%s\n' "$TREL" "$CX" "$CZ"
    done >> "$CHUNKS_TSV"
    cp "$RUN_DIR/.done" "$RUN_DIR/.done_prev"
    echo "   t+${TREL}s: $DONE/1024 chunks full"
    if [ "$FIRST_POLL" = 1 ]; then
        ps -T -p "$PID" -o comm= 2>/dev/null | sort | uniq -c | sort -rn > "$RUN_DIR/.threads" || true
        FIRST_POLL=0
    fi
    [ "$DONE" -ge 1024 ] && break
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "FATAL: server exited during generation" >&2; tail -30 "$LOG" >&2; exit 1
    fi
    if awk -v t="$TREL" -v m="$GEN_TIMEOUT" 'BEGIN{exit !(t>m)}'; then
        echo "FATAL: generation did not finish in ${GEN_TIMEOUT}s" >&2; exit 1
    fi
done
T1=$(now)
LOAD_T1=$(cut -d' ' -f1-3 /proc/loadavg)
GEN_S=$(awk -v a="$T1" -v b="$T0" 'BEGIN{printf "%.3f", a-b}')
echo "== t1: all 1024 confirmed full — gen interval ${GEN_S}s (overestimate <= ${POLL_S}s + cmd latency)"

# mid-run flush census (autosave fired?) BEFORE our explicit save
PRESAVE_MCA=$(find world -name '*.mca' -printf '%p %s\n' 2>/dev/null | sort || true)
echo "   pre-save .mca state: ${PRESAVE_MCA:-none}"

echo "== save-all flush (excluded from timing)"
SAVES_BEFORE=$(grep -c 'Saved the game' "$LOG" || true)
echo "save-all flush" >&3
WAITED=0
while [ "$(grep -c 'Saved the game' "$LOG" || true)" -le "$SAVES_BEFORE" ]; do
    sleep 2; WAITED=$((WAITED + 2))
    [ "$WAITED" -ge 600 ] && { echo "FATAL: save-all flush timeout" >&2; exit 1; }
done

MCA="$RUN_DIR/world/dimensions/minecraft/overworld/region/r.0.0.mca"
[ -f "$MCA" ] || { echo "FATAL: no r.0.0.mca at $MCA" >&2; find world -name '*.mca' >&2 || true; exit 1; }
STATS="$(python3 "$HC/tools/golden/region_stats.py" "$MCA")"
echo "$STATS" | sed 's/^/   /'
FULL=$(echo "$STATS" | awk '$1=="status" && $2=="minecraft:full" {print $3}')
CANON=$(python3 "$HC/tools/golden/compare_regions.py" --canonical-hash "$MCA" | cut -d' ' -f1)
RAW=$(sha256sum "$MCA" | cut -d' ' -f1)
echo "   full: ${FULL:-0}/1024  canonical: $CANON"

echo "== stopping server"
echo "stop" >&3
for _ in $(seq 1 60); do kill -0 "$PID" 2>/dev/null || break; sleep 1; done
kill -0 "$PID" 2>/dev/null && { echo "FATAL: server did not stop" >&2; exit 1; }
trap - EXIT
exec 3>&-

WORKERS=$(awk '/Worker-Main/{n+=$1} END{print n+0}' "$RUN_DIR/.threads" 2>/dev/null || echo 0)
python3 - "$RES" <<PYEOF
import json, sys
rec = {
  "mode": "$MODE", "run": "$RUNID", "seed": $SEED,
  "gen_s": float("$GEN_S"), "poll_s": $POLL_S,
  "predone_chunks": $PREDONE, "pre_t0_r00_disk": """${PRE_R00:-none}""",
  "full_chunks": int("${FULL:-0}"),
  "canonical": "$CANON", "raw_sha256": "$RAW",
  "boot_s": round(float("$T_BOOT") - float("$T_LAUNCH"), 2),
  "worker_main_threads": int("${WORKERS:-0}"),
  "load_t0": "$LOAD_T0", "load_t1": "$LOAD_T1",
  "presave_mca": """${PRESAVE_MCA:-none}""",
  "jvm": "-Xms2G -Xmx8G (JDK 25.0.3)",
}
with open(sys.argv[1], "a") as f:
    f.write(json.dumps(rec) + "\n")
print("   result appended:", json.dumps(rec))
PYEOF

cp "$LOG" "$ART/server-$MODE-$RUNID.log"
cp "$MCA" "$ART/r.0.0-$MODE-$RUNID.mca"
cp "$RUN_DIR/.threads" "$ART/threads-$MODE-$RUNID.txt" 2>/dev/null || true
[ -d config ] && cp -r config "$ART/config-$MODE-$RUNID" || true
cd /
rm -rf "$RUN_DIR"
echo "== [$MODE/$RUNID] done, run dir removed"
