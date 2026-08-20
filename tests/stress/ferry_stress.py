#!/usr/bin/env python3
"""
Self-contained load driver for ferry-server stress tests (no external
tools required; scripts may substitute hey/vegeta/wrk if preferred).

Modes:
  rate    open-loop constant-rate fire (one request per 1/rate s slot,
          regardless of how slow responses are)
  closed  closed-loop: `concurrency` workers re-request back-to-back

Prints one JSON line on stdout:
  {"total": N, "statuses": {"200": n, ...}, "transport_errors": e,
   "elapsed_s": x, "bytes": b}
"""
import argparse
import http.client
import json
import sys
import threading
import time
from collections import Counter


def one_request(host, port, path, timeout, headers=None):
    t0 = time.monotonic()
    try:
        conn = http.client.HTTPConnection(host, port, timeout=timeout)
        conn.request("GET", path, headers=headers or {})
        resp = conn.getresponse()
        body = resp.read()
        status = str(resp.status)
        conn.close()
        return status, len(body), (time.monotonic() - t0) * 1000.0
    except Exception:
        return None, 0, (time.monotonic() - t0) * 1000.0


class XffGen:
    """Unique forged client identities (rightmost XFF entry, trust_hops=1)."""

    def __init__(self, prefix):
        self.prefix = prefix
        self.lock = threading.Lock()
        self.n = 0

    def headers(self):
        if not self.prefix:
            return {}
        with self.lock:
            i = self.n
            self.n += 1
        return {"X-Forwarded-For":
                f"{self.prefix}.{i // 250}.{i % 250}"}


class Tally:
    def __init__(self):
        self.lock = threading.Lock()
        self.statuses = Counter()
        self.errors = 0
        self.bytes = 0
        self.total = 0
        self.latencies_ms = []

    def record(self, status, size, latency_ms):
        with self.lock:
            self.total += 1
            if status is None:
                self.errors += 1
            else:
                self.statuses[status] += 1
                self.bytes += size
            self.latencies_ms.append(latency_ms)


def run_rate(args, tally):
    """Open-loop: schedule a request every 1/rate seconds."""
    interval = 1.0 / args.rate
    deadline = time.monotonic() + args.duration
    threads = []
    next_t = time.monotonic()
    xff = XffGen(args.xff_prefix)

    def fire():
        headers = dict(args.headers)
        headers.update(xff.headers())
        status, size, latency_ms = one_request(
            args.host, args.port, args.path, args.timeout, headers)
        tally.record(status, size, latency_ms)

    while time.monotonic() < deadline:
        now = time.monotonic()
        if now < next_t:
            time.sleep(next_t - now)
        t = threading.Thread(target=fire, daemon=True)
        t.start()
        threads.append(t)
        next_t += interval
        if len(threads) > 5000:        # bound memory on slow responses
            threads = [t for t in threads if t.is_alive()]

    for t in threads:
        t.join(timeout=args.timeout + 5)


def run_closed(args, tally):
    """Closed-loop: fixed worker count, back-to-back requests."""
    deadline = time.monotonic() + args.duration
    xff = XffGen(args.xff_prefix)

    def work():
        while time.monotonic() < deadline:
            headers = dict(args.headers)
            headers.update(xff.headers())
            status, size, latency_ms = one_request(
                args.host, args.port, args.path, args.timeout, headers)
            tally.record(status, size, latency_ms)

    threads = [threading.Thread(target=work) for _ in range(args.concurrency)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("mode", choices=["rate", "closed"])
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--path", default="/")
    p.add_argument("--duration", type=float, default=10.0)
    p.add_argument("--timeout", type=float, default=60.0)
    p.add_argument("--rate", type=float, default=100.0)        # rate mode
    p.add_argument("--concurrency", type=int, default=10)      # closed mode
    p.add_argument("--xff-prefix", default=None,
                   help="forge a unique X-Forwarded-For per request "
                        "(prefix.<i/250>.<i mod 250>), e.g. 10.51")
    p.add_argument("--header", action="append", default=[],
                   metavar="NAME:VALUE",
                   help="additional request header (repeatable)")
    args = p.parse_args()
    args.headers = {}
    for item in args.header:
        if ":" not in item:
            p.error(f"invalid --header {item!r}; expected NAME:VALUE")
        name, value = item.split(":", 1)
        if not name.strip():
            p.error("header name must not be empty")
        args.headers[name.strip()] = value.strip()

    tally = Tally()
    t0 = time.monotonic()
    if args.mode == "rate":
        run_rate(args, tally)
    else:
        run_closed(args, tally)
    elapsed = time.monotonic() - t0

    latencies = sorted(tally.latencies_ms)

    def percentile(q):
        if not latencies:
            return 0.0
        index = round((len(latencies) - 1) * q)
        return round(latencies[index], 3)

    json.dump({
        "total": tally.total,
        "statuses": dict(tally.statuses),
        "transport_errors": tally.errors,
        "elapsed_s": round(elapsed, 3),
        "bytes": tally.bytes,
        "latency_ms": {
            "p50": percentile(0.50),
            "p95": percentile(0.95),
            "p99": percentile(0.99),
            "max": round(latencies[-1], 3) if latencies else 0.0,
        },
    }, sys.stdout)
    print()


if __name__ == "__main__":
    main()
