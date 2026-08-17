#!/usr/bin/env bash
# L3 system verification: drives the real ferry-server binary with curl and a
# multi-threaded downloader. Usage: ./tests/system/run_l3.sh [server-binary]
set -u

BIN="${1:-build/linux/x86_64/release/ferry-server}"
WORK=$(mktemp -d /tmp/ferry_l3.XXXXXX)
PORT=18091
CAP=$((8 * 1024 * 1024))
TOTAL=$((64 * 1024 * 1024))
BASE="http://127.0.0.1:$PORT"
PASS=0; FAIL=0

check() {  # check <description> <expected> <actual>
    if [ "$2" = "$3" ]; then PASS=$((PASS+1)); echo "ok   $1"
    else FAIL=$((FAIL+1)); echo "FAIL $1 (expected '$2', got '$3')"; fi
}

cleanup() {
    [ -n "${SERVER_PID:-}" ] && kill -9 "$SERVER_PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

echo "== fixtures =="
mkdir -p "$WORK/files/nested"
python3 - "$WORK/files" "$TOTAL" <<'EOF'
import sys, os
root, total = sys.argv[1], int(sys.argv[2])
CH = 4 * 1024 * 1024
with open(os.path.join(root, "big.bin"), "wb") as f:
    w = 0
    while w < total:
        n = min(CH, total - w)
        f.write(bytes((w + i) % 251 for i in range(n)))   # byte[i] = i % 251
        w += n
open(os.path.join(root, "small.txt"), "wb").write(bytes(i % 251 for i in range(4096)))
open(os.path.join(root, "nested/deep.bin"), "wb").write(bytes(i % 251 for i in range(70000)))
EOF
printf '# empty\n' > "$WORK/acl.conf"
cat > "$WORK/server.conf" <<EOF
port = $PORT
root = $WORK/files
cap_bytes = $CAP
rate_bytes_per_sec = 0
acl_file = $WORK/acl.conf
acl_poll_interval_sec = 2
EOF

echo "== start server =="
"$BIN" "$WORK/server.conf" > "$WORK/server.log" 2>&1 &
SERVER_PID=$!
sleep 1
kill -0 "$SERVER_PID" || { echo "server failed to start"; cat "$WORK/server.log"; exit 1; }

echo "== range semantics with content verification =="
code=$(curl -s -H "Range: bytes=1000000-2999999" -o "$WORK/s1" -w "%{http_code}" "$BASE/big.bin")
dd if="$WORK/files/big.bin" of="$WORK/s1.exp" bs=1 skip=1000000 count=2000000 2>/dev/null
check "206 middle range" "206" "$code"
cmp -s "$WORK/s1" "$WORK/s1.exp" && check "middle range content" ok ok || check "middle range content" ok differ

code=$(curl -s -H "Range: bytes=0-" -o "$WORK/s2" -D "$WORK/s2.hdr" -w "%{http_code}" "$BASE/big.bin")
check "206 open-ended (capped)" "206" "$code"
check "cap honored" "$CAP" "$(stat -c %s "$WORK/s2")"
grep -q "Content-Range: bytes 0-$((CAP-1))/$TOTAL" "$WORK/s2.hdr" && check "Content-Range total" ok ok || check "Content-Range total" ok missing

code=$(curl -s -H "Range: bytes=-1000" -o "$WORK/s3" -w "%{http_code}" "$BASE/big.bin")
tail -c 1000 "$WORK/files/big.bin" > "$WORK/s3.exp"
check "206 suffix" "206" "$code"
cmp -s "$WORK/s3" "$WORK/s3.exp" && check "suffix content" ok ok || check "suffix content" ok differ

check "416 past end" "416" "$(curl -s -o /dev/null -w %{http_code} -H 'Range: bytes=999999999-' "$BASE/big.bin")"
check "413 no-range large" "413" "$(curl -s -o /dev/null -w %{http_code} "$BASE/big.bin")"
check "200 small whole" "200" "$(curl -s -o /dev/null -w %{http_code} "$BASE/small.txt")"
check "HEAD full size" "Content-Length: $TOTAL" "$(curl -sI "$BASE/big.bin" | grep -i content-length | tr -d '\r')"
check "404 missing" "404" "$(curl -s -o /dev/null -w %{http_code} "$BASE/none")"
check "invalid unit -> 200" "200" "$(curl -s -o /dev/null -w %{http_code} -H 'Range: items=0-9' "$BASE/small.txt")"
trav=$(python3 -c "
import socket
s = socket.create_connection(('127.0.0.1', $PORT))
s.sendall(b'GET /..%2f..%2fetc/passwd HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')
print(s.recv(100).decode().split()[1]); s.close()")
check "encoded traversal -> 400" "400" "$trav"

echo "== multi-threaded cap-cycling download =="
python3 - "$BASE/big.bin" "$WORK/files/big.bin" "$WORK/dl.bin" "$TOTAL" <<'EOF' && check "mt-download sha256" ok ok || check "mt-download sha256" ok mismatch
import hashlib, sys, threading, urllib.request
url, srcp, dstp, total = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
W = 4; REGION = total // W; out = bytearray(total); errs = []
def worker(w):
    pos, end = w * REGION, (w + 1) * REGION
    while pos < end:
        req = urllib.request.Request(url, headers={"Range": f"bytes={pos}-{end-1}"})
        with urllib.request.urlopen(req) as r:
            assert r.status == 206
            s, rest = r.headers["Content-Range"].split(" ")[1].split("-")
            e, tot = rest.split("/")
            assert int(tot) == total and int(s) == pos
            d = r.read(); out[pos:pos+len(d)] = d; pos += len(d)
ts = [threading.Thread(target=worker, args=(w,)) for w in range(W)]
[t.start() for t in ts]; [t.join() for t in ts]
h1 = hashlib.sha256(bytes(out)).hexdigest()
h2 = hashlib.sha256(open(srcp, "rb").read()).hexdigest()
open(dstp, "wb").write(bytes(out))
sys.exit(0 if h1 == h2 else 1)
EOF

echo "== resume simulation =="
curl -s -H "Range: bytes=0-5242879" -o "$WORK/p1" "$BASE/big.bin"
curl -s -H "Range: bytes=5242880-" -o "$WORK/p2" "$BASE/big.bin"
python3 -c "
src = open('$WORK/files/big.bin','rb').read()
assert open('$WORK/p1','rb').read() == src[:5242880]
assert open('$WORK/p2','rb').read() == src[5242880:5242880+8388608]" \
    && check "resume alignment" ok ok || check "resume alignment" ok mismatch
check "resume at EOF -> 416" "416" "$(curl -s -o /dev/null -w %{http_code} -H "Range: bytes=$TOTAL-" "$BASE/big.bin")"

echo "== ACL hot reload + XFF (real timer) =="
printf 'blacklist 127.0.0.1\n' > "$WORK/acl.conf"
sleep 3.5
check "blacklist applied via poll" "403" "$(curl -s -o /dev/null -w %{http_code} "$BASE/small.txt")"
check "forged XFF left ignored" "403" "$(curl -s -o /dev/null -w %{http_code} -H 'X-Forwarded-For: 10.0.0.5, 127.0.0.1' "$BASE/small.txt")"
printf 'whitelist 10.0.0.5\n' > "$WORK/acl.conf"; touch -d "+30 seconds" "$WORK/acl.conf"
sleep 3.5
check "whitelist gates peer" "403" "$(curl -s -o /dev/null -w %{http_code} "$BASE/small.txt")"
check "XFF rightmost whitelisted" "200" "$(curl -s -o /dev/null -w %{http_code} -H 'X-Forwarded-For: 1.1.1.1, 10.0.0.5' "$BASE/small.txt")"
printf '# empty\n' > "$WORK/acl.conf"; touch -d "+60 seconds" "$WORK/acl.conf"
sleep 3.5
check "rules lifted" "200" "$(curl -s -o /dev/null -w %{http_code} "$BASE/small.txt")"

echo "== graceful shutdown =="
t0=$(date +%s%N); kill -TERM "$SERVER_PID"
while kill -0 "$SERVER_PID" 2>/dev/null; do sleep 0.1; done
ms=$(( ($(date +%s%N) - t0) / 1000000 )); SERVER_PID=""
[ "$ms" -lt 3000 ] && check "SIGTERM exit < 3s (${ms}ms)" ok ok || check "SIGTERM exit < 3s" ok "${ms}ms"
grep -q "ferry-server stopped" "$WORK/server.log" && check "clean-stop logged" ok ok || check "clean-stop logged" ok missing

echo
echo "L3 result: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
