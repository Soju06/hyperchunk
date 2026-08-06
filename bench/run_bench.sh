#!/usr/bin/env bash
# P2-0 재현 커맨드 (1줄): bench/run_bench.sh
#
#   bench/run_bench.sh [-n <runs>=3] [-t <threads>=20] [-p <preset>=bench-o2] [-l <label>] [-m <policy>=replay]
#
# -m free 는 FREE 스케줄러 벤치 (ADR-008 Pitfall 2: 벤치 숫자는 FREE,
# 패리티 배지는 REPLAY — 파일명에 policy 가 박힌다. 판정 상수는
# #canonical-own-v1).
#
# 프리셋을 configure/build 하고 hyperchunk-bench 를 n회 반복 실행한다.
# 각 실행은 독립 프로세스라 상태 누적이 없고, 매 회 canonical 해시를
# 판정한다 (하나라도 FAIL 이면 exit 1). JSON 결과는
# bench/results/<UTC타임스탬프>-<label>.jsonl 로, 요약(중앙값/편차)은
# stdout 으로 낸다.
#
# 주의: 이 VM(claw)은 토폴로지 오보고 — 절대치는 참고치. 비중/배율만 유효.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
RUNS=3
THREADS=20
PRESET=bench-o2
LABEL=""
POLICY=replay
SEED="${SEED:-1234567890}"
while getopts "n:t:p:l:m:" opt; do
    case "$opt" in
    n) RUNS="$OPTARG" ;;
    t) THREADS="$OPTARG" ;;
    p) PRESET="$OPTARG" ;;
    l) LABEL="$OPTARG" ;;
    m) POLICY="$OPTARG" ;;
    *) exit 2 ;;
    esac
done

BUILD="$ROOT/build-$PRESET"
[ -d "$BUILD" ] || cmake --preset "$PRESET" -S "$ROOT" >/dev/null
cmake --build "$BUILD" --target hyperchunk_bench -j"$(nproc)" >/dev/null

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUTDIR="$HERE/results"
mkdir -p "$OUTDIR"
OUT="$OUTDIR/${STAMP}-${PRESET}-t${THREADS}-${POLICY}${LABEL:+-$LABEL}.jsonl"

for i in $(seq 1 "$RUNS"); do
    echo "-- run $i/$RUNS --" >&2
    "$BUILD/bench/hyperchunk-bench" --seed "$SEED" --region 0 0 \
        --repo "$ROOT" --threads "$THREADS" --policy "$POLICY" >>"$OUT"
done

echo "results: $OUT"
python3 "$HERE/summarize.py" "$OUT"
