#!/usr/bin/env bash
# P2-3: FREE-vs-REPLAY 동일성 게이트 (자체 이력 σ*/Λ*/π*).
# 같은 이벤트 리스트를 REPLAY(순차) 와 FREE(병렬) 정책으로 두 번 돌려
# canonical 해시 동일성을 판정한다 — "병렬로 돌려도 비트가 같다".
set -euo pipefail
BIN=$1; shift
REF=$1; STAGES=$2; BAND=$3; SEED=$4; RREF=$5
run() { # $1 = own-replay | own-free
  "$BIN" "$REF" "$STAGES" "$BAND" "$SEED" "$RREF" unused "$1" \
    | tee /dev/stderr | grep '^own-canonical' | awk '{print $3}'
}
H_REPLAY=$(run own-replay)
H_FREE=$(run own-free)
echo "own-replay canonical: $H_REPLAY"
echo "own-free    canonical: $H_FREE"
if [ -z "$H_REPLAY" ] || [ "$H_REPLAY" != "$H_FREE" ]; then
  echo "FAIL: FREE != REPLAY (자체 이력)"; exit 1
fi
echo "PASS: FREE == REPLAY (canonical $H_FREE)"
