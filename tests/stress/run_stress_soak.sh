#!/usr/bin/env bash
# Shaping-backlog soak: sustained load while the bandwidth shaper holds a
# backlog of delayed requests. The server must stay stable for the whole
# window: periodic stats lines keep coming, inflight never exceeds the
# cap, no 5xx, and the process is still alive at the end.
#
# Default 10 minutes; shorten with SOAK_SECONDS=60 for a smoke run.
set -eu
source "$(dirname "$0")/stress_lib.sh"
require_bins

SOAK="${SOAK_SECONDS:-600}"
INFLIGHT="${MAX_INFLIGHT:-30}"
WORK=$(mktemp -d /tmp/ferry_stress_soak.XXXXXX)
PID=""
trap '[ -z "$PID" ] || stop_server "$PID"; rm -rf "$WORK"' EXIT

make_root "$WORK/root" 262144

cat > "$WORK/server.conf" <<EOF
port = $PORT
root = $WORK/root
cap_bytes = 262144
max_inflight = $INFLIGHT
qps_total = 500
rate_total_bps = 524288
max_wait_sec = 30
stats_interval_sec = 5
EOF

PID=$(start_server "$WORK/server.conf" "$WORK/server.log")
echo "soaking for ${SOAK}s (shaping backlog, max_inflight=$INFLIGHT) ..."

RESULT=$(python3 "$DRIVER" closed --port "$PORT" --path /stress.bin \
			--concurrency 40 --duration "$SOAK" --timeout 90)
kill -0 "$PID" 2>/dev/null || fail "server died during soak"
stats_dump "$PID"
stop_server "$PID"
PID=""

echo "$RESULT"
ERR5XX=$(echo "$RESULT" | json_field "['statuses'].get('503', 0)")
TERR=$(echo "$RESULT" | json_field "['transport_errors']")
LINES=$(grep -c '^\[stats\]' "$WORK/server.log")
LAST=$(last_stats "$WORK/server.log")
PEAK=$(field "$LAST" peak)
NOW=$(field "$LAST" inflight)

# one line per 5 s interval, minus slack for startup/shutdown edges
MIN_LINES=$(( (SOAK / 5) - 3 ))
[ "$MIN_LINES" -lt 1 ] && MIN_LINES=1
[ "$LINES" -ge "$MIN_LINES" ] || fail "only $LINES stats lines (< $MIN_LINES)"
[ "$PEAK" -le "$INFLIGHT" ] || fail "inflight peak $PEAK exceeded cap $INFLIGHT"
[ "$NOW" = "0" ] || fail "inflight did not drain (now=$NOW)"
[ "$TERR" -eq 0 ] || fail "$TERR transport errors during soak"
echo "stats lines: $LINES, inflight peak: $PEAK/$INFLIGHT, 503s: $ERR5XX"
pass "soak stable: bounded inflight, no transport errors, stats continuous"
