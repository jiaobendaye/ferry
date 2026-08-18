#!/usr/bin/env bash
# QPS gate stress: constant-rate load at 2x qps_total. max_wait_sec=0
# makes the gate a hard cap (a positive max_wait would absorb up to
# rate x max_wait extra requests as delay), so ~half the traffic must
# be admitted and ~half rejected with 429.
set -eu
source "$(dirname "$0")/stress_lib.sh"
require_bins

QPS="${QPS_TOTAL:-200}"
DUR="$STRESS_SECONDS"
WORK=$(mktemp -d /tmp/ferry_stress_qps.XXXXXX)
PID=""
trap '[ -z "$PID" ] || stop_server "$PID"; rm -rf "$WORK"' EXIT

make_root "$WORK/root" 4096

cat > "$WORK/server.conf" <<EOF
port = $PORT
root = $WORK/root
cap_bytes = 65536
qps_total = $QPS
max_wait_sec = 0
stats_interval_sec = 2
EOF

PID=$(start_server "$WORK/server.conf" "$WORK/server.log")
echo "driving $((QPS * 2)) req/s for ${DUR}s against qps_total=$QPS ..."

RESULT=$(python3 "$DRIVER" rate --port "$PORT" --path /stress.bin \
			--rate $((QPS * 2)) --duration "$DUR" --timeout 10)
stats_dump "$PID"
stop_server "$PID"
PID=""

echo "$RESULT"
TOTAL=$(echo "$RESULT" | json_field "['total']")
OK=$(echo "$RESULT" | json_field "['statuses'].get('200', 0)")
REJ=$(echo "$RESULT" | json_field "['statuses'].get('429', 0)")
PEAK=$(field "$(last_stats "$WORK/server.log")" peak)

[ "$TOTAL" -gt 0 ] || fail "no requests completed"
ADMITTED_PCT=$((OK * 100 / TOTAL))
echo "admitted: $OK / $TOTAL (${ADMITTED_PCT}%), 429s: $REJ, inflight peak: $PEAK"

# At 2x load, the steady-state admission ratio is 50%. A fresh bucket also
# grants one second of burst, which matters in deliberately short smoke runs.
# Bound against that theoretical allowance with 10 percentage points of
# scheduler/network slack, rather than falsely failing a 3-second smoke run.
EXPECTED_PCT=$(python3 -c \
	"print(round(min(100, (float('$DUR') + 1) / (2 * float('$DUR')) * 100)))")
LOWER_PCT=$((EXPECTED_PCT - 10))
UPPER_PCT=$((EXPECTED_PCT + 10))
[ "$LOWER_PCT" -lt 35 ] && LOWER_PCT=35
[ "$UPPER_PCT" -gt 100 ] && UPPER_PCT=100
[ "$ADMITTED_PCT" -ge "$LOWER_PCT" ] && [ "$ADMITTED_PCT" -le "$UPPER_PCT" ] \
	|| fail "admitted ${ADMITTED_PCT}% outside ${LOWER_PCT}..${UPPER_PCT}% (expected ${EXPECTED_PCT}% incl. initial burst)"
[ "$REJ" -gt 0 ] || fail "no 429s observed"
pass "qps_total admission matched 2x-load bound (steady-state ~50%)"
