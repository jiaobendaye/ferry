#!/usr/bin/env bash
# L3 system tests for ferry-client: real binaries end to end.
# Usage: ./tests/system/run_client_l3.sh [server-binary] [client-binary]
set -u

SERVER=$(readlink -f "${1:-build/linux/x86_64/release/ferry-server}")
CLIENT=$(readlink -f "${2:-build/linux/x86_64/release/ferry-client}")
WORK=$(mktemp -d /tmp/ferry_client_l3.XXXXXX)
FILES="$WORK/files"
OUT="$WORK/out"
PORT_FERRY=18191
PORT_SHAPED=18192
PORT_PY=18193
PORT_STUB=18194
TOTAL=$((64 * 1024 * 1024))
PASS=0; FAIL=0
PIDS=""

check() {
    if [ "$2" = "$3" ]; then PASS=$((PASS+1)); echo "ok   $1"
    else FAIL=$((FAIL+1)); echo "FAIL $1 (expected '$2', got '$3')"; fi
}

cleanup() {
    for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
    rm -rf "$WORK"
}
trap cleanup EXIT

sha() { sha256sum "$1" | cut -d' ' -f1; }

bitmap_done() {  # count set bits in the meta's bitmap hex
    python3 - "$1" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
print(bin(int(m["bitmap"], 16)).count("1") if m["bitmap"] else 0)
EOF
}

echo "== fixtures =="
mkdir -p "$FILES" "$OUT"
python3 - "$FILES/big.bin" "$TOTAL" <<'EOF'
import sys
path, total = sys.argv[1], int(sys.argv[2])
CH = 4 * 1024 * 1024
with open(path, "wb") as f:
    w = 0
    while w < total:
        n = min(CH, total - w)
        f.write(bytes((w + i) % 251 for i in range(n)))
        w += n
EOF
python3 -c "open('$FILES/small.bin','wb').write(bytes(i%251 for i in range(4096)))"
SRC_HASH=$(sha "$FILES/big.bin")
SRC_SMALL=$(sha "$FILES/small.bin")

start_ferry() {  # port, rate
    cat > "$WORK/srv_$1.conf" <<EOF
port = $1
root = $FILES
cap_bytes = 8388608
rate_bytes_per_sec = $2
acl_file =
EOF
    "$SERVER" "$WORK/srv_$1.conf" > "$WORK/srv_$1.log" 2>&1 &
    PIDS="$PIDS $!"
}

start_ferry $PORT_FERRY 0
start_ferry $PORT_SHAPED 8388608        # 8 MiB/s shaping for the kill test
sleep 1

echo "== 7.1 end-to-end + checksum gates =="
(cd "$OUT" && "$CLIENT" -q http://127.0.0.1:$PORT_FERRY/big.bin) >/dev/null 2>&1
check "e2e exit code" "0" "$?"
check "e2e hash" "$SRC_HASH" "$(sha "$OUT/big.bin" 2>/dev/null)"
[ -e "$OUT/big.bin.ferry.json" ] && check "meta removed" gone present || check "meta removed" gone gone
rm -f "$OUT/big.bin"

(cd "$OUT" && "$CLIENT" -q --checksum sha-256=$SRC_HASH \
    http://127.0.0.1:$PORT_FERRY/big.bin) >/dev/null 2>&1
check "checksum pass exit" "0" "$?"
rm -f "$OUT/big.bin"

BADHASH=$(printf '%064d' 0)
(cd "$OUT" && "$CLIENT" -q --checksum sha-256=$BADHASH \
    http://127.0.0.1:$PORT_FERRY/big.bin) >/dev/null 2>&1
rc=$?
[ "$rc" != "0" ] && check "checksum mismatch exit nonzero" ok ok || check "checksum mismatch exit nonzero" ok "rc=$rc"
[ -e "$OUT/big.bin" ] && check "no output on mismatch" absent present || check "no output on mismatch" absent absent
[ -e "$OUT/big.bin.part" ] && check "part kept on mismatch" kept kept || check "part kept on mismatch" kept missing
rm -f "$OUT/big.bin.part" "$OUT/big.bin.ferry.json"

echo "== 7.2 SIGKILL mid-download then resume =="
(cd "$OUT" && exec "$CLIENT" -q -j 4 http://127.0.0.1:$PORT_SHAPED/big.bin \
    > "$WORK/kill_run1.log" 2>&1) &
echo $! > "$WORK/cli.pid"
sleep 3
kill -9 "$(cat "$WORK/cli.pid")" 2>/dev/null
sleep 0.3
[ -e "$OUT/big.bin.part" ] && check "part after SIGKILL" kept kept || check "part after SIGKILL" kept missing
[ -e "$OUT/big.bin.ferry.json" ] && check "meta after SIGKILL" kept kept || check "meta after SIGKILL" kept missing
DONE_BEFORE=$(bitmap_done "$OUT/big.bin.ferry.json")
[ "$DONE_BEFORE" -gt 0 ] && check "progress made before kill" ok ok || check "progress made before kill" ok "done=$DONE_BEFORE"

(cd "$OUT" && "$CLIENT" -q -j 4 http://127.0.0.1:$PORT_SHAPED/big.bin \
    > "$WORK/kill_run2.log" 2>&1)
check "resume exit code" "0" "$?"
check "resume hash" "$SRC_HASH" "$(sha "$OUT/big.bin" 2>/dev/null)"
grep -q "resuming" "$WORK/kill_run2.log" && check "resume detected" ok ok || check "resume detected" ok "no resume note"
rm -f "$OUT/big.bin"

echo "== 7.3 foreign servers =="
python3 -m http.server $PORT_PY --bind 127.0.0.1 --directory "$FILES" \
    > "$WORK/py.log" 2>&1 &
PIDS="$PIDS $!"
sleep 1
(cd "$OUT" && "$CLIENT" -q -o py.bin http://127.0.0.1:$PORT_PY/big.bin) >/dev/null 2>&1
check "py http.server exit" "0" "$?"
check "py http.server hash" "$SRC_HASH" "$(sha "$OUT/py.bin" 2>/dev/null)"
rm -f "$OUT/py.bin"

python3 - $PORT_STUB "$FILES" <<'EOF' > "$WORK/stub.log" 2>&1 &
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
port, root = int(sys.argv[1]), sys.argv[2]
class H(BaseHTTPRequestHandler):
    def _reply(self, send_body):
        try:
            data = open(root + self.path, "rb").read()
        except OSError:
            self.send_response(404); self.end_headers(); return
        self.send_response(200)          # deliberately ignores Range
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if send_body:
            self.wfile.write(data)
    def do_GET(self): self._reply(True)
    def do_HEAD(self): self._reply(False)
    def log_message(self, *a): pass
HTTPServer(("127.0.0.1", port), H).serve_forever()
EOF
PIDS="$PIDS $!"
sleep 1

(cd "$OUT" && "$CLIENT" -q -o stub_small.bin \
    http://127.0.0.1:$PORT_STUB/small.bin) >/dev/null 2>&1
check "rangeless single-stream exit" "0" "$?"
check "rangeless hash" "$SRC_SMALL" "$(sha "$OUT/stub_small.bin" 2>/dev/null)"
rm -f "$OUT/stub_small.bin"

(cd "$OUT" && "$CLIENT" -q --single-stream-limit 1 -o stub_big.bin \
    http://127.0.0.1:$PORT_STUB/big.bin) > "$WORK/stub_refuse.log" 2>&1
rc=$?
[ "$rc" != "0" ] && check "rangeless oversize refused" ok ok || check "rangeless oversize refused" ok "rc=$rc"
grep -q "single-stream-limit" "$WORK/stub_refuse.log" && check "refusal reason shown" ok ok || check "refusal reason shown" ok missing

echo "== 7.4 UX / misuse =="
# shaped server -> download takes ~8 s so the 1 s ticker fires
(cd "$OUT" && "$CLIENT" -j 4 -o prog.bin http://127.0.0.1:$PORT_SHAPED/big.bin \
    > "$WORK/progress.log" 2>&1)
grep -q "%" "$WORK/progress.log" && check "progress lines shown" ok ok || check "progress lines shown" ok missing
rm -f "$OUT/prog.bin"

(cd "$OUT" && "$CLIENT" -q -j 4 http://127.0.0.1:$PORT_FERRY/small.bin \
    > "$WORK/quiet.log" 2>&1)
grep -q "%" "$WORK/quiet.log" && check "quiet silences progress" noisy quiet || check "quiet silences progress" quiet quiet
rm -f "$OUT/small.bin"

"$CLIENT" -j 0 http://x/y > "$WORK/badj.log" 2>&1
rc=$?
[ "$rc" != "0" ] && check "-j 0 rejected" ok ok || check "-j 0 rejected" ok "rc=$rc"
grep -qi "jobs" "$WORK/badj.log" && check "-j 0 message clear" ok ok || check "-j 0 message clear" ok missing

"$CLIENT" --checksum sha-256=xyz http://x/y > "$WORK/badc.log" 2>&1
rc=$?
[ "$rc" != "0" ] && check "bad checksum rejected" ok ok || check "bad checksum rejected" ok "rc=$rc"

echo
echo "L3 client result: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
