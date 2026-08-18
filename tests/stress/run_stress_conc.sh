#!/usr/bin/env bash
# Concurrency gate stress: closed-loop concurrency far above max_inflight,
# with every admitted response held ~4 s by a tight bandwidth shaper, so
# the in-flight pool stays saturated. Each request forges its own IP
# (fresh per-IP bucket: deficit 212 KB at 50 KB/s) so the shaper holds
# every admitted request instead of rejecting repeat offenders with 429.
# Expects 503s for the overflow and a stats peak of exactly max_inflight.
set -eu
source "$(dirname "$0")/stress_lib.sh"
require_bins

INFLIGHT="${MAX_INFLIGHT:-20}"
CONC="${CONCURRENCY:-100}"
DUR="$STRESS_SECONDS"
WORK=$(mktemp -d /tmp/ferry_stress_conc.XXXXXX)
PID=""
trap '[ -z "$PID" ] || stop_server "$PID"; rm -rf "$WORK"' EXIT

make_root "$WORK/root" 262144

cat > "$WORK/server.conf" <<EOF
port = $PORT
root = $WORK/root
cap_bytes = 262144
max_inflight = $INFLIGHT
rate_bytes_per_sec = 50000
max_wait_sec = 30
stats_interval_sec = 2
EOF

PID=$(start_server "$WORK/server.conf" "$WORK/server.log")
echo "driving $CONC closed-loop workers for ${DUR}s against max_inflight=$INFLIGHT ..."

RESULT=$(python3 "$DRIVER" closed --port "$PORT" --path /stress.bin \
			--concurrency "$CONC" --duration "$DUR" --timeout 30 \
			--xff-prefix 10.51)
stats_dump "$PID"
stop_server "$PID"
PID=""

echo "$RESULT"
OK=$(echo "$RESULT" | json_field "['statuses'].get('200', 0)")
REJ=$(echo "$RESULT" | json_field "['statuses'].get('503', 0)")
LAST=$(last_stats "$WORK/server.log")
PEAK=$(field "$LAST" peak)
NOW=$(field "$LAST" inflight)

[ "$REJ" -gt 0 ] || fail "no 503s under ${CONC}x saturation"
[ "$PEAK" = "$INFLIGHT" ] || fail "inflight peak $PEAK != cap $INFLIGHT"
[ "$NOW" = "0" ] || fail "inflight did not drain (now=$NOW)"
pass "max_inflight=$INFLIGHT enforced exactly (peak=$PEAK, drained, $REJ x 503, $OK served)"
