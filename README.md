# ferry — Range-capable HTTP file download server + client

A file-transfer pair built on [Sogou Workflow](https://github.com/sogou/workflow)
(fully asynchronous C++ framework) and built with [xmake](https://xmake.io):
`ferry-server` serves files as capped Range responses; `ferry-client` is a
multi-threaded, resumable downloader that pairs with it (and works against
any RFC-conformant Range server).

Features:

- **HTTP Range downloads** (`206 Partial Content`): resumable and
  parallel-friendly. Every response reveals the full file size — via
  `Content-Range: bytes s-e/TOTAL` on any 206, `HEAD` `Content-Length`, or
  200 `Content-Length` — so download managers can plan and resume.
- **Bounded memory**: each response is truncated to a configurable cap
  (default 8 MiB; RFC 9110 permits shorter ranges). Large files are served
  as a series of capped range responses; non-Range requests for files over
  the threshold get `413` with a self-describing body.
- **IP access control**: blacklist/whitelist with IPv4/IPv6 CIDR entries,
  blacklist priority, hot reload (file mtime polling, no restart).
- **Per-IP bandwidth limiting**: token-bucket soft shaping — an over-budget
  request is delayed inside the Workflow series (no thread blocked) up to a
  configured wait cap, beyond which it gets `429 + Retry-After`.
- **Proxy-aware client IP**: the real client is taken from the rightmost
  `X-Forwarded-For` entry (the one appended by the nearest trusted proxy),
  which cannot be forged by the client; falls back to the socket peer.

Server source lives in `server/`, client source in `client/` (no shared
library — the two are mirrors of each other's Range logic).

## Client (ferry-client)

Multi-threaded chunked downloader: splits the file into fixed chunks
(default 8 MiB), claims them dynamically across workers, verifies and
reassembles, and gates the final output file behind a streaming sha256.

```bash
xmake run ferry-client -- http://host:8080/path/file.bin
ferry-client -j 8 -o out.bin --checksum sha-256=<hex> <url>
```

| Option | Default | Meaning |
|---|---|---|
| `-o, --output PATH` | URL basename | Output file (data lands in `<out>.part` first) |
| `-j, --jobs N` | `4` | Worker count (effective = min(jobs, chunks)) |
| `--chunk-size BYTES` | `8388608` | Request chunk size — bounds per-response memory |
| `--checksum sha-256=<hex>` | off | Expected digest; mismatch keeps files + fails |
| `--no-verify` | off | Skip the final sha256 pass (digest is otherwise always computed and printed) |
| `--receive-timeout SEC` | `60` | Per-request timeout — must exceed the server's `max_wait_sec` |
| `--single-stream-limit BYTES` | `256 MiB` | Max size accepted from servers without Range support |
| `-q, --quiet` | off | Suppress the per-second progress line |

**Resume.** Progress persists in `<out>.part` (data) + `<out>.ferry.json`
(url, size, Last-Modified, chunk_size, completion bitmap), rewritten
atomically after every chunk. Re-run the same command to resume; a HEAD
check compares size and Last-Modified first — if the file changed, state is
discarded with a warning and the download restarts. SIGINT exits cleanly
with resumable state; kill -9 is also survivable (at most one finished
chunk re-downloads).

**Server compatibility.** Against Range servers (including ferry-server)
the client adapts to whatever cap the server applies — truncated responses
just mean more iterations. Against servers without Range support it falls
back to a single-stream download bounded by `--single-stream-limit`
(Workflow buffers whole responses, so an unbounded fallback would risk
OOM). 429 responses are honored via `max(Retry-After, backoff)`; transient
errors retry with exponential backoff (500 ms × 2^n, capped at 30 s, 8
attempts); 403/404 stop all workers.

**Two couplings to know.** (1) `--receive-timeout` must stay larger than
the server's `max_wait_sec`, or soft-shaped responses read as timeouts.
(2) ferry-server rate-limits per IP, so one client's workers share a single
bucket: concurrency hides latency but does not multiply bandwidth.

## Build & run

Requirements: Linux, a C++17 compiler, xmake. Sogou Workflow (v1.0.1) and
gtest are pulled and built automatically by xmake's package manager
(xrepo) on first build.

```bash
xmake                 # builds ferry-server, ferry-server-core, unit-test, integration-test
xmake run ferry-server config/server.conf
```

The server prints its effective configuration on startup and shuts down
gracefully on SIGINT/SIGTERM.

### Configuration reference (`config/server.conf`)

Flat `key = value` file; `#` comments. Invalid values fail startup loudly.

| Key | Default | Meaning |
|---|---|---|
| `port` | `8080` | Listen port |
| `root` | `.` | Directory whose files are served |
| `cap_bytes` | `8388608` | Max bytes per response (range truncation cap) |
| `size_threshold_bytes` | = `cap_bytes` | Non-Range requests above this → `413` |
| `rate_bytes_per_sec` | `0` (off) | Per-IP bandwidth limit |
| `max_wait_sec` | `30` | Max shaping delay before `429` |
| `trust_hops` | `1` | Which XFF entry from the right is the client |
| `acl_file` | empty (off) | ACL rules file (hot-reloaded) |
| `acl_poll_interval_sec` | `5` | ACL mtime poll period |
| `max_connections` | `2000` | Workflow server connection limit |

### ACL file format

One entry per line: `blacklist <ip-or-cidr>` or `whitelist <ip-or-cidr>`.
IPv4 and IPv6 supported; bare IPs are `/32`/`/128`. Semantics: a blacklist
hit always denies (even if also whitelisted); when the whitelist is
non-empty, any IP not in it is denied. The file is re-read when its mtime
changes; a broken file keeps the previous rules in effect. See
`config/acl.conf`.

## Deployment notes

### Reverse proxy requirement

The server trusts the `X-Forwarded-For` entry appended by the proxy in
front of it. Configure the proxy to **append** the connection's real
address, e.g. nginx:

```nginx
proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
```

With `trust_hops = 1` (default) the **rightmost** XFF entry is used — the
one written by the nearest trusted proxy. A client forging the left side of
the header gains nothing. With N chained trusted proxies set
`trust_hops = N`. No XFF header → the socket peer address is used
(direct-connection debugging works).

### Memory sizing

Responses are buffered before sending (Workflow's server model), so peak
memory ≈ concurrent in-flight responses × `cap_bytes`. Tune
`max_connections` and `cap_bytes` together: e.g. cap 8 MiB × 2000
connections ≈ 16 GiB worst case — lower `max_connections` for smaller
hosts.

### Rate limiting semantics

Limits are **per server instance** (single-instance design). For
multi-instance scale-out, use LB IP-affinity (consistent hashing) so each
client IP lands on one instance, or move the buckets to shared storage.
Limiter state entries for idle IPs are reclaimed automatically.

## Testing

Three layers (see the design docs' testing sections):

```bash
xmake run unit-test          # L1: pure logic, no sleeps (server: range/ACL/XFF/limiter/config/path;
                             #     client: planner/bitmap/backoff/cli/progress/probe/sha256)
xmake run integration-test   # L2: server suite + client closed loop (ferry handler serving,
                             #     client engine downloading: caps, shaping, 429, resume, fatals)
tests/system/run_l3.sh       # L3 server: curl-driven (content-verified ranges, hot reload, XFF)
tests/system/run_client_l3.sh # L3 client: real binaries (SIGKILL-resume, checksum gates,
                             #     python http.server interop, Range-less fallback)
```

AddressSanitizer run (catches nocopy-buffer lifetime bugs):

```bash
xmake f -m debug --asan=y    # configure with ASan + LeakSanitizer
xmake -r                     # rebuild everything
xmake run unit-test && xmake run integration-test
xmake f -m release --asan=n  # back to normal
```

## Design documents

Requirements, decisions and task history live under
[`openspec/`](openspec/changes/add-range-file-server/) (proposal, design,
specs, tasks).
