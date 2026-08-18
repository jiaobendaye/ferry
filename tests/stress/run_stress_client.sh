#!/usr/bin/env bash
# Closed-loop ecosystem stress: ferry-client -j 32 against a server with
# tight per-IP gates. The client's 32 workers share ONE IP's budget, so
# most of them will eat 429/503 and back off — the download must still
# complete, hash-verified, without hanging.
set -eu
source "$(dirname "$0")/stress_lib.sh"
require_bins
[ -x "$CLIENT_BIN" ] || fail "ferry-client not built ($CLIENT_BIN); run: xmake"

SIZE="${FILE_SIZE:-$((32 * 1024 * 1024))}"
CHUNK_SIZE="${CHUNK_SIZE:-1048576}"
WORK=$(mktemp -d /tmp/ferry_stress_client.XXXXXX)
PID=""
trap '[ -z "$PID" ] || stop_server "$PID"; rm -rf "$WORK"' EXIT

make_root "$WORK/root" "$SIZE"
EXPECTED=$(sha256sum "$WORK/root/stress.bin" | cut -d' ' -f1)

cat > "$WORK/server.conf" <<EOF
port = $PORT
root = $WORK/root
cap_bytes = 1048576
qps_per_ip = 30
max_inflight_per_ip = 8
rate_bytes_per_sec = 2097152
max_wait_sec = 10
stats_interval_sec = 2
EOF

PID=$(start_server "$WORK/server.conf" "$WORK/server.log")
echo "ferry-client -j 32 vs per-IP gates (qps=30, inflight=8, rate=2MiB/s) ..."
# Keep client chunks aligned with server cap. The current client accepts
# legally-shorter 206 responses but marks its whole requested chunk done;
# a larger client chunk would test that separate client bug rather than
# admission gates.
set +e
timeout 300 "$CLIENT_BIN" -j 32 --chunk-size "$CHUNK_SIZE" \
	-o "$WORK/out.bin" \
	--checksum "sha-256=$EXPECTED" \
	"http://127.0.0.1:$PORT/stress.bin" > "$WORK/client.log" 2>&1
RC=$?
set -e

stats_dump "$PID"
stop_server "$PID"
PID=""

[ "$RC" -eq 0 ] || { tail -5 "$WORK/client.log" >&2; fail "client exited rc=$RC (124 = hung & killed)"; }
[ -f "$WORK/out.bin" ] || fail "output file missing (checksum gate blocked?)"
ACTUAL=$(sha256sum "$WORK/out.bin" | cut -d' ' -f1)
[ "$ACTUAL" = "$EXPECTED" ] || fail "content mismatch"

REJ_QPS=$(field "$(last_stats "$WORK/server.log")" 'rej(qps_per_ip)')
REJ_INF=$(field "$(last_stats "$WORK/server.log")" 'rej(inflight_per_ip)')
REJ_QPS=${REJ_QPS:-0}
REJ_INF=${REJ_INF:-0}
REJ_TOTAL=$((REJ_QPS + REJ_INF))
[ "$REJ_TOTAL" -gt 0 ] || fail "download completed but no per-IP gate pressure was observed (increase FILE_SIZE or lower limits)"
echo "client completed; gate pressure observed: qps_per_ip 429s=$REJ_QPS, inflight_per_ip 503s=$REJ_INF"
pass "closed-loop download completed under per-IP gates, sha256 verified"
