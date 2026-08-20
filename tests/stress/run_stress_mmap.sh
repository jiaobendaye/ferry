#!/usr/bin/env bash
# Hot-page-cache A/B benchmark for the pread and mmap response body paths.
# Each mode gets a fresh server process. The fixture is created once and is
# already resident after creation; this intentionally measures the case in
# which mmap has the best chance to remove a copy. Cold-file/event-loop
# latency needs a separate production-like storage test.
set -euo pipefail
source "$(dirname "$0")/stress_lib.sh"
require_bins

DUR="${STRESS_SECONDS:-10}"
CONC="${CONCURRENCY:-32}"
CAP="${CAP_BYTES:-8388608}"
FILE_SIZE="${FILE_SIZE:-67108864}"
REPS="${REPETITIONS:-1}"
WORK=$(mktemp -d /tmp/ferry_stress_mmap.XXXXXX)
PID=""
SAMPLER=""
trap '[ -z "$SAMPLER" ] || kill "$SAMPLER" 2>/dev/null || true; \
	  [ -z "$PID" ] || stop_server "$PID"; rm -rf "$WORK"' EXIT

make_root "$WORK/root" "$FILE_SIZE"
CLK_TCK=$(getconf CLK_TCK)

proc_ticks()
{
	awk '{print $14 + $15}' "/proc/$1/stat"
}

proc_faults()
{
	awk '{print $10, $12}' "/proc/$1/stat"
}

sample_memory()
{
	local pid="$1" output="$2" max_rss=0 max_anon=0
	while kill -0 "$pid" 2>/dev/null; do
		local rss anon
		rss=$(awk '/^VmRSS:/ {print $2}' "/proc/$pid/status" 2>/dev/null || true)
		anon=$(awk '/^RssAnon:/ {print $2}' "/proc/$pid/status" 2>/dev/null || true)
		[ -z "$rss" ] || [ "$rss" -le "$max_rss" ] || max_rss="$rss"
		[ -z "$anon" ] || [ "$anon" -le "$max_anon" ] || max_anon="$anon"
		sleep 0.05
	done
	echo "$max_rss $max_anon" > "$output"
}

run_case()
{
	local mode="$1"
	local case_port="$2"
	local conf="$WORK/$mode.conf" log="$WORK/$mode.log"
	cat > "$conf" <<EOF
port = $case_port
root = $WORK/root
file_body_mode = $mode
cap_bytes = $CAP
size_threshold_bytes = $CAP
rate_bytes_per_sec = 0
stats_interval_sec = 0
max_connections = $((CONC * 4 + 32))
EOF

	PID=$(start_server "$conf" "$log")
	# Warm-up also verifies the selected mode can serve one complete range.
	curl -fsS -H 'Range: bytes=0-' -o /dev/null \
		"http://127.0.0.1:$case_port/stress.bin"

	sample_memory "$PID" "$WORK/$mode.memory" &
	SAMPLER=$!
	local ticks_before faults_before result ticks_after faults_after
	ticks_before=$(proc_ticks "$PID")
	faults_before=$(proc_faults "$PID")
	result=$(python3 "$DRIVER" closed --port "$case_port" --path /stress.bin \
				--header 'Range:bytes=0-' --concurrency "$CONC" \
				--duration "$DUR" --timeout 60)
	ticks_after=$(proc_ticks "$PID")
	faults_after=$(proc_faults "$PID")

	stats_dump "$PID"
	stop_server "$PID"
	PID=""
	wait "$SAMPLER" 2>/dev/null || true
	SAMPLER=""

	local memory stats
	memory=$(cat "$WORK/$mode.memory")
	stats=$(last_stats "$log")
	python3 - "$mode" "$result" "$ticks_before" "$ticks_after" \
		"$CLK_TCK" "$faults_before" "$faults_after" $memory \
		"$(field "$stats" mmap_resps)" "$(field "$stats" mmap_fallbacks)" \
		"$(field "$stats" peak)" "$(field "$stats" mmap_peak)" <<'PY'
import json, sys
mode, raw = sys.argv[1], sys.argv[2]
d = json.loads(raw)
tb, ta, hz = int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
minor_b, major_b = map(int, sys.argv[6].split())
minor_a, major_a = map(int, sys.argv[7].split())
rss_kb, anon_kb = int(sys.argv[8]), int(sys.argv[9])
mmap_resps, fallbacks = int(sys.argv[10]), int(sys.argv[11])
inflight_peak, mmap_peak = int(sys.argv[12]), int(sys.argv[13])
cpu_s = (ta - tb) / hz
gib = d["bytes"] / (1024 ** 3)
print(json.dumps({
    "mode": mode,
    "requests": d["total"],
    "statuses": d["statuses"],
    "transport_errors": d["transport_errors"],
    "mib_s": round(d["bytes"] / d["elapsed_s"] / (1024 ** 2), 2),
    "cpu_s": round(cpu_s, 3),
    "cpu_s_per_gib": round(cpu_s / gib, 4) if gib else 0,
    "latency_ms": d["latency_ms"],
    "peak_rss_kb": rss_kb,
    "peak_rss_anon_kb": anon_kb,
    "minor_faults": minor_a - minor_b,
    "major_faults": major_a - major_b,
    "mmap_responses": mmap_resps,
    "mmap_fallbacks": fallbacks,
    "inflight_peak": inflight_peak,
    "mmap_active_peak_bytes": mmap_peak,
}, sort_keys=True))
PY
}

echo "hot-cache mmap A/B: cap=$CAP file=$FILE_SIZE concurrency=$CONC duration=${DUR}s repetitions=$REPS"
for rep in $(seq 1 "$REPS"); do
	echo "repetition $rep/$REPS"
	pread_port=$((PORT + rep * 2))
	mmap_port=$((pread_port + 1))
	if [ $((rep % 2)) -eq 1 ]; then
		run_case pread "$pread_port" | tee -a "$WORK/pread.result"
		run_case mmap "$mmap_port" | tee -a "$WORK/mmap.result"
	else
		run_case mmap "$mmap_port" | tee -a "$WORK/mmap.result"
		run_case pread "$pread_port" | tee -a "$WORK/pread.result"
	fi
done

python3 - "$WORK/pread.result" "$WORK/mmap.result" <<'PY'
import json, statistics, sys

def load(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]

pread, mmap = load(sys.argv[1]), load(sys.argv[2])

def mean(rows, field):
    values = []
    for row in rows:
        value = row
        for part in field.split("."):
            value = value[part]
        values.append(value)
    return statistics.mean(values)

fields = ["mib_s", "cpu_s_per_gib", "latency_ms.p99", "peak_rss_anon_kb"]
summary = {"pread": {}, "mmap": {}, "mmap_delta_pct": {}}
for field in fields:
    before, after = mean(pread, field), mean(mmap, field)
    summary["pread"][field] = round(before, 3)
    summary["mmap"][field] = round(after, 3)
    summary["mmap_delta_pct"][field] = round((after / before - 1) * 100, 2)
summary["runs"] = len(pread)
summary["transport_errors"] = sum(r["transport_errors"] for r in pread + mmap)
summary["mmap_fallbacks"] = sum(r["mmap_fallbacks"] for r in mmap)
print("aggregate=" + json.dumps(summary, sort_keys=True))
PY
pass "mmap A/B completed (compare MiB/s, CPU s/GiB, p99, and peak RssAnon)"
