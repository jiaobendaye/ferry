#!/usr/bin/env bash
# Opt-in page-cache policy benchmark. Uses a disk-backed workspace (not /tmp,
# which may be tmpfs), performs two hash-verified full downloads per policy,
# and reports first/second-read throughput, disk reads, target-file residency,
# process RSS/RssAnon, cgroup memory classes, and cache-advice counters.
set -euo pipefail
source "$(dirname "$0")/stress_lib.sh"
require_bins

FILE_SIZE="${FILE_SIZE:-67108864}"
CAP="${CAP_BYTES:-8388608}"
CONC="${CONCURRENCY:-10}"
POLICIES="${POLICIES:-normal noreuse drop_after_read}"
BASELINE_RESULT="${BASELINE_RESULT:-}"
SERVER_BIN="${SERVER_BIN_OVERRIDE:-$SERVER_BIN}"
CACHE_WORK_ROOT="${CACHE_WORK_ROOT:-$REPO_ROOT}"
COLD_START_MAX_BYTES="${COLD_START_MAX_BYTES:-4194304}"
IDLE_SECONDS="${IDLE_SECONDS:-5}"
WORK=$(mktemp -d "$CACHE_WORK_ROOT/.ferry_stress_cache.XXXXXX")
PID=""
SAMPLER=""
trap '[ -z "$SAMPLER" ] || kill "$SAMPLER" 2>/dev/null || true; \
	  [ -z "$PID" ] || stop_server "$PID"; rm -rf "$WORK"' EXIT

[ -x "$CLIENT_BIN" ] || fail "ferry-client not built ($CLIENT_BIN); run: xmake"
[ -x "$SERVER_BIN" ] || fail "ferry-server not executable: $SERVER_BIN"
command -v fincore >/dev/null 2>&1 || fail "fincore is required"
[ $((CAP % 1048576)) -eq 0 ] || fail "CAP_BYTES must be whole MiB"
CAP_MIB=$((CAP / 1048576))

mkdir -p "$WORK/root"
EXPECTED=$(python3 - "$WORK/root/cache.bin" "$FILE_SIZE" <<'PY'
import hashlib, os, sys
path, remaining = sys.argv[1], int(sys.argv[2])
seed = bytes(range(251))
block = (seed * (1024 * 1024 // len(seed) + 1))[:1024 * 1024]
digest = hashlib.sha256()
with open(path, "wb", buffering=0) as output:
    while remaining:
        chunk = block[:min(remaining, len(block))]
        output.write(chunk)
        digest.update(chunk)
        remaining -= len(chunk)
    os.fsync(output.fileno())
print(digest.hexdigest())
PY
)

evict_source()
{
	python3 - "$WORK/root/cache.bin" <<'PY'
import os, sys
fd = os.open(sys.argv[1], os.O_RDONLY)
try:
    os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
finally:
    os.close(fd)
PY
}

resident_bytes()
{
	fincore --bytes --noheadings --output RES "$WORK/root/cache.bin" | tr -d ' '
}

proc_ticks() { awk '{print $14 + $15}' "/proc/$1/stat"; }
proc_read_bytes() { awk '/^read_bytes:/ {print $2}' "/proc/$1/io"; }
proc_memory_kb()
{
	awk '/^VmRSS:/ {rss=$2} /^RssAnon:/ {anon=$2}
		 END {print rss + 0, anon + 0}' "/proc/$1/status"
}

stats_field_or()
{
	local stats="$1" key="$2" fallback="$3" value
	value=$(field "$stats" "$key" || true)
	[ -n "$value" ] && echo "$value" || echo "$fallback"
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

download_once()
{
	local output="$1" start end
	rm -f "$output" "$output.part" "$output.ferry.json"
	start=$(date +%s%N)
	"$CLIENT_BIN" -q -j "$CONC" --chunk-size "$CAP_MIB" \
		--receive-timeout 120 --checksum "sha-256=$EXPECTED" \
		-o "$output" "http://127.0.0.1:$PORT/cache.bin"
	end=$(date +%s%N)
	echo $((end - start))
}

run_case()
{
	local policy="$1" conf="$WORK/$1.conf" log="$WORK/$1.log"
	evict_source
	local cold_before
	cold_before=$(resident_bytes)
	[ "$cold_before" -le "$COLD_START_MAX_BYTES" ] ||
		fail "source is not cold: resident=$cold_before limit=$COLD_START_MAX_BYTES"
	cat > "$conf" <<EOF
port = $PORT
root = $WORK/root
file_body_mode = pread
page_cache_policy = $policy
cap_bytes = $CAP
size_threshold_bytes = $CAP
max_inflight = $((CONC + 4))
max_connections = $((CONC * 4 + 32))
stats_interval_sec = 0
EOF

	PID=$(start_server "$conf" "$log")
	local memory_before
	memory_before=$(proc_memory_kb "$PID")
	sample_memory "$PID" "$WORK/$policy.memory" &
	SAMPLER=$!
	local ticks0 ticks1 ticks2 reads0 reads1 reads2 first_ns second_ns
	ticks0=$(proc_ticks "$PID")
	reads0=$(proc_read_bytes "$PID")
	first_ns=$(download_once "$WORK/$policy.first")
	ticks1=$(proc_ticks "$PID")
	reads1=$(proc_read_bytes "$PID")
	local resident_first
	resident_first=$(resident_bytes)
	second_ns=$(download_once "$WORK/$policy.second")
	ticks2=$(proc_ticks "$PID")
	reads2=$(proc_read_bytes "$PID")
	sleep "$IDLE_SECONDS"
	local resident_idle memory_idle
	resident_idle=$(resident_bytes)
	memory_idle=$(proc_memory_kb "$PID")
	stats_dump "$PID"
	stop_server "$PID"
	PID=""
	wait "$SAMPLER" 2>/dev/null || true
	SAMPLER=""

	local memory stats
	memory=$(cat "$WORK/$policy.memory")
	stats=$(last_stats "$log")
	python3 - "$policy" "$FILE_SIZE" "$CAP" "$cold_before" "$first_ns" \
		"$second_ns" "$ticks0" "$ticks1" "$ticks2" "$(getconf CLK_TCK)" \
		"$reads0" "$reads1" "$reads2" "$resident_first" "$resident_idle" \
		$memory_before $memory $memory_idle \
		"$(stats_field_or "$stats" cache_advice_calls 0)" \
		"$(stats_field_or "$stats" cache_advice_bytes 0)" \
		"$(stats_field_or "$stats" cache_advice_errors 0)" \
		"$(stats_field_or "$stats" inflight -1)" \
		"$(stats_field_or "$stats" mem_anon -1)" \
		"$(stats_field_or "$stats" mem_file -1)" \
		"$(stats_field_or "$stats" mem_sock -1)" <<'PY'
import json, sys
(policy, size, cap, cold_before, first_ns, second_ns, t0, t1, t2, hz,
 r0, r1, r2, resident_first, resident_idle, before_rss, before_anon,
 peak_rss, peak_anon, idle_rss, idle_anon,
 advice_calls, advice_bytes, advice_errors, inflight, mem_anon, mem_file,
 mem_sock) = sys.argv[1:]
size, cap, cold_before = int(size), int(cap), int(cold_before)
first_s, second_s = int(first_ns) / 1e9, int(second_ns) / 1e9
t0, t1, t2, hz = map(int, (t0, t1, t2, hz))
gib = size / (1024 ** 3)
print(json.dumps({
    "policy": policy, "file_size": size,
    "cold_resident_before": cold_before,
    "first_mib_s": round(size / first_s / (1024 ** 2), 2),
    "second_mib_s": round(size / second_s / (1024 ** 2), 2),
    "first_cpu_s_per_gib": round(((t1 - t0) / hz) / gib, 4),
    "second_cpu_s_per_gib": round(((t2 - t1) / hz) / gib, 4),
    "first_disk_read_bytes": int(r1) - int(r0),
    "second_disk_read_bytes": int(r2) - int(r1),
    "resident_after_first": int(resident_first),
    "resident_after_idle": int(resident_idle),
    "idle_threshold": max(64 * 1024 * 1024, 2 * cap),
    "idle_threshold_met": int(resident_idle) <= max(64 * 1024 * 1024, 2 * cap),
    "before_rss_kb": int(before_rss), "before_rss_anon_kb": int(before_anon),
    "peak_rss_kb": int(peak_rss), "peak_rss_anon_kb": int(peak_anon),
    "idle_rss_kb": int(idle_rss), "idle_rss_anon_kb": int(idle_anon),
    "cache_advice_calls": int(advice_calls),
    "cache_advice_bytes": int(advice_bytes),
    "cache_advice_errors": int(advice_errors),
    "inflight": int(inflight),
    "mem_anon": int(mem_anon), "mem_file": int(mem_file),
    "mem_sock": int(mem_sock),
}, sort_keys=True))
PY
	rm -f "$WORK/$policy.first" "$WORK/$policy.second"
}

echo "page-cache policy benchmark: file=$FILE_SIZE cap=$CAP concurrency=$CONC"
RESULT_FILES=()
for policy in $POLICIES; do
	case "$policy" in
		normal|noreuse|drop_after_read) ;;
		*) fail "unknown policy in POLICIES: $policy" ;;
	esac
	run_case "$policy" | tee "$WORK/$policy.result"
	RESULT_FILES+=("$WORK/$policy.result")
done

python3 - "$CAP" "$BASELINE_RESULT" "${RESULT_FILES[@]}" <<'PY'
import json, sys
cap, baseline_path = int(sys.argv[1]), sys.argv[2]
rows = [json.load(open(path)) for path in sys.argv[3:]]
by_policy = {row["policy"]: row for row in rows}
for row in rows:
    if row["inflight"] != 0:
        raise SystemExit(f"{row['policy']} did not drain inflight requests")
aggregate = {}
normal = by_policy.get("normal")
drop = by_policy.get("drop_after_read")
if drop:
    threshold = max(64 * 1024 * 1024, 2 * cap)
    aggregate.update(idle_threshold=threshold,
                     drop_idle_resident=drop["resident_after_idle"])
    if drop["resident_after_idle"] > threshold:
        raise SystemExit(
            f"drop_after_read residency {drop['resident_after_idle']} > {threshold}")
    if drop["cache_advice_errors"] != 0:
        raise SystemExit("drop_after_read reported cache advice errors")
if normal:
    aggregate["normal_second_vs_first_pct"] = round(
        (normal["second_mib_s"] / normal["first_mib_s"] - 1) * 100, 2)
if normal and drop:
    drop_cold_regression = (1 - drop["first_mib_s"] /
                            normal["first_mib_s"]) * 100
    aggregate["drop_cold_throughput_regression_pct"] = round(
        drop_cold_regression, 2)
    aggregate["drop_second_vs_first_pct"] = round(
        (drop["second_mib_s"] / drop["first_mib_s"] - 1) * 100, 2)
    if drop_cold_regression > 10:
        raise SystemExit(
            f"drop cold throughput regression {drop_cold_regression:.2f}% > 10%")
if baseline_path:
    if not normal:
        raise SystemExit("BASELINE_RESULT requires normal in POLICIES")
    baseline = json.load(open(baseline_path))
    throughput_regression = (1 - normal["first_mib_s"] /
                             baseline["first_mib_s"]) * 100
    cpu_regression = (normal["first_cpu_s_per_gib"] /
                      baseline["first_cpu_s_per_gib"] - 1) * 100
    aggregate["normal_throughput_regression_pct"] = round(
        throughput_regression, 2)
    aggregate["normal_cpu_per_gib_regression_pct"] = round(cpu_regression, 2)
    if throughput_regression > 3:
        raise SystemExit(
            f"normal throughput regression {throughput_regression:.2f}% > 3%")
    if cpu_regression > 5:
        raise SystemExit(f"normal CPU/GiB regression {cpu_regression:.2f}% > 5%")
print("aggregate=" + json.dumps(aggregate, sort_keys=True))
PY
pass "cache policies verified: integrity, residency, memory, and repeat-read cost"
