#!/usr/bin/env bash
# Gate-overhead measurement (design decision D12): closed-loop throughput
# with all gates OFF vs all gates ON, same driver, same duration. Reports
# the ratio; no hard assertion — the number feeds the
# single-mutex-vs-sharding decision.
set -eu
source "$(dirname "$0")/stress_lib.sh"
require_bins

DUR="${STRESS_SECONDS:-5}"
CONC="${CONCURRENCY:-32}"
WORK=$(mktemp -d /tmp/ferry_stress_overhead.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

make_root "$WORK/root" 4096

run_case()
{
	local name="$1" gates="$2"
	cat > "$WORK/server.conf" <<EOF
port = $PORT
root = $WORK/root
cap_bytes = 65536
stats_interval_sec = 0
$gates
EOF
	local pid
	pid=$(start_server "$WORK/server.conf" "$WORK/$name.log")
	trap "stop_server '$pid'" EXIT
	local result
	result=$(python3 "$DRIVER" closed --port "$PORT" --path /stress.bin \
				--concurrency "$CONC" --duration "$DUR" --timeout 10)
	stop_server "$pid"
	trap - EXIT
	local total elapsed
	total=$(echo "$result" | json_field "['total']")
	elapsed=$(echo "$result" | json_field "['elapsed_s']")
	python3 -c "print(round($total / $elapsed, 1))"
}

echo "measuring closed-loop throughput ($CONC workers, ${DUR}s each) ..."
RPS_OFF=$(run_case off "")
RPS_ON=$(run_case on "qps_total = 1000000
qps_per_ip = 1000000
max_inflight = 100000
max_inflight_per_ip = 100000
rate_total_bps = 1099511627776
rate_bytes_per_sec = 1099511627776")

echo "gates off: ${RPS_OFF} req/s"
echo "gates on : ${RPS_ON} req/s"
python3 -c "
off, on = $RPS_OFF, $RPS_ON
overhead = (off - on) / off * 100 if off else 0
print(f'overhead : {overhead:.1f}%')
print('verdict  : ' + ('acceptable (<5%)' if overhead < 5 else
                  'measure again / consider sharded maps (D12)'))"
pass "overhead measured"
