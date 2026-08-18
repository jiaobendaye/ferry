# Shared helpers for ferry stress scripts. Sourced, not executed.
#
# Opt-in, NOT part of CI: these scripts drive real load for seconds to
# minutes. Run them manually (see README.md in this directory).

STRESS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$STRESS_DIR/../.." && pwd)"
SERVER_BIN="$REPO_ROOT/build/linux/x86_64/release/ferry-server"
CLIENT_BIN="$REPO_ROOT/build/linux/x86_64/release/ferry-client"
DRIVER="$STRESS_DIR/ferry_stress.py"

STRESS_SECONDS="${STRESS_SECONDS:-10}"        # default wall time per script
PORT="${STRESS_PORT:-18990}"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: $*"; }

require_bins()
{
	[ -x "$SERVER_BIN" ] || fail "ferry-server not built ($SERVER_BIN); run: xmake"
	[ -x "$DRIVER" ] || fail "missing driver $DRIVER"
}

# make_root <dir> <file-size-bytes>: scratch root with one pattern file.
make_root()
{
	local dir="$1" size="$2"
	mkdir -p "$dir"
	python3 -c "
import sys
with open(sys.argv[1], 'wb') as f:
    f.write(bytes(i % 251 for i in range(int(sys.argv[2]))))
" "$dir/stress.bin" "$size"
}

# start_server <conf-file> <log-file> -> echoes PID
start_server()
{
	local conf="$1" log="$2"
	# stdout to /dev/null: the daemon must not hold any command-
	# substitution pipe open (start_server is called inside $(...))
	"$SERVER_BIN" "$conf" >/dev/null 2>"$log" &
	local pid=$!
	sleep 0.5
	kill -0 "$pid" 2>/dev/null || { cat "$log" >&2; fail "server did not start"; }
	echo "$pid"
}

stop_server()
{
	local pid="$1"
	kill -INT "$pid" 2>/dev/null || return 0

	# start_server is normally invoked via command substitution, so the
	# daemon is not a child of this shell and wait(1) cannot reap it.
	# Poll for graceful exit instead; force-kill only if shutdown wedges.
	local i
	for i in $(seq 1 50); do
		kill -0 "$pid" 2>/dev/null || return 0
		sleep 0.1
	done
	kill -KILL "$pid" 2>/dev/null || true
	for i in $(seq 1 20); do
		kill -0 "$pid" 2>/dev/null || return 0
		sleep 0.1
	done
	return 0
}

# stats_dump <pid>: SIGUSR1 and let the 1 s tick print it.
stats_dump()
{
	kill -USR1 "$1" 2>/dev/null
	sleep 1.5
}

# last_stats <log-file>: most recent [stats] line.
last_stats()
{
	grep '^\[stats\]' "$1" | tail -1
}

# field <stats-line> <key>: extract "key=VALUE" (VALUE up to next space).
field()
{
	echo "$1" | tr ' ' '\n' | grep "^$2=" | head -1 | cut -d= -f2
}

json_field()
{
	python3 -c "import json,sys; d=json.load(sys.stdin); print(d$1)"
}
