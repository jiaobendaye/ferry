# ferry — Range-capable HTTP file download server + client

English | [简体中文](README.zh-CN.md)

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
- **Admission gates**: global QPS, in-flight, and bandwidth caps protect the
  server as a whole; per-IP QPS, in-flight, and bandwidth quotas keep clients
  from crowding one another. Rate gates soft-shape then return `429`, while
  concurrency gates reject immediately with `503 + Retry-After`.
- **Runtime statistics**: request/status/served-byte totals, per-gate
  rejections, current/peak in-flight requests, and active per-IP buckets are
  available as periodic single-line logs and on demand with `SIGUSR1`.
- **Proxy-aware client IP**: the real client is taken from the rightmost
  `X-Forwarded-For` entry (the one appended by the nearest trusted proxy),
  which cannot be forged by the client; falls back to the socket peer.

Server source lives in `server/`, client source in `client/` (no shared
library — the two are mirrors of each other's Range logic).

Source is grouped by domain. The server uses `access/`, `admission/`,
`config/`, `file/`, `http/`, and `observability/`; the client uses
`download/`, `http/`, `integrity/`, `resume/`, and `ui/`. Executable entry
points live in each side's `app/` directory. `tests/unit/` and
`tests/integration/` mirror server/client and the corresponding source
domains.

## Client (ferry-client)

Multi-threaded chunked downloader: splits the file into fixed chunks
(default 8 MiB), claims them dynamically across workers, verifies and
reassembles, and gates the final output file behind a streaming sha256.

```bash
xmake run ferry-client -- http://host:8080/path/file.bin
ferry-client -j 8 -o out.bin --checksum sha-256=<hex> <url>
ferry-client --chunk-size 16 --single-stream-limit 512 <url>
```

| Option | Default | Meaning |
|---|---|---|
| `-o, --output PATH` | URL basename | Output file (data lands in `<out>.part` first) |
| `-j, --jobs N` | `4` | Worker count (effective = min(jobs, chunks)) |
| `--chunk-size MiB` | `8 MiB` | Request chunk size, entered as an integer MiB count — bounds per-response memory |
| `--checksum sha-256=<hex>` | off | Expected digest; mismatch keeps files + fails |
| `--no-verify` | off | Skip the final sha256 pass (digest is otherwise always computed and printed) |
| `--receive-timeout SEC` | `60` | Per-request timeout — must exceed the server's `max_wait_sec` |
| `--single-stream-limit MiB` | `256 MiB` | Max size accepted from servers without Range support, entered as an integer MiB count |
| `-q, --quiet` | off | Suppress the per-second progress line |

The two `MiB` arguments take an integer count without a suffix: use
`--chunk-size 8`, not `--chunk-size 8MiB`. They are converted to bytes
internally with overflow checking.

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
(2) One client's workers share that IP's QPS, concurrency, and bandwidth
budgets. A large `-j` can therefore trigger `429`/`503` and backoff;
concurrency hides latency but does not multiply the per-IP quotas.

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

`cap_bytes`, `size_threshold_bytes`, and `rate_bytes_per_sec` accept either a
decimal byte count or an integer followed by the case-sensitive binary suffix
`B`, `KiB`, `MiB`, `GiB`, or `TiB`; whitespace before the suffix is optional.
For example, `cap_bytes = 8MiB` and `size_threshold_bytes = 16MiB` set response
and non-Range thresholds, while `rate_bytes_per_sec = 10 MiB` means 10 MiB/s.
Decimal SI suffixes such as `MB` and fractional quantities are not accepted.

| Key | Default | Meaning |
|---|---|---|
| `port` | `8080` | Listen port |
| `root` | `.` | Directory whose files are served |
| `file_body_mode` | `pread` | File body path: stable `pread`, or experimental `mmap` for A/B measurement |
| `page_cache_policy` | `normal` | Buffered-I/O cache policy: `normal`, `noreuse`, or `drop_after_read` (`pread` only for non-normal values) |
| `cap_bytes` | `8388608` | Max bytes per response (range truncation cap) |
| `size_threshold_bytes` | = `cap_bytes` | Non-Range requests above this → `413` |
| `rate_bytes_per_sec` | `0` (off) | Per-IP bandwidth limit |
| `rate_total_bps` | `0` (off) | Whole-server bandwidth limit |
| `qps_total` | `0` (off) | Whole-server request-rate limit |
| `qps_per_ip` | `0` (off) | Per-IP request-rate quota |
| `max_inflight` | `0` (off) | Whole-server in-flight request cap |
| `max_inflight_per_ip` | `0` (off) | Per-IP in-flight request quota |
| `max_wait_sec` | `30` | Max shaping delay before `429` |
| `stats_interval_sec` | `0` (off) | Periodic stats-log interval |
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

Responses are buffered before sending (Workflow's server model). In the
default `pread` mode, peak anonymous response-buffer memory is bounded by
`max_inflight × cap_bytes` when `max_inflight` is configured. For example,
128 × 8 MiB is approximately 1 GiB. With that gate off, use
`max_connections × cap_bytes` as the conservative sizing bound.

Buffered `pread` also populates Linux page cache. This is reclaimable cgroup
`file` memory and is not included in the process's ordinary `RssAnon`; it can
remain after requests drain. `page_cache_policy = normal` keeps the current
cache-friendly behavior. `noreuse` submits `POSIX_FADV_NOREUSE` before each
read, while `drop_after_read` submits `POSIX_FADV_DONTNEED` for fully covered
pages after a successful read has copied them into the response buffer. Both
are best-effort kernel advice, not hard cache quotas. Use `drop_after_read` for
one-shot large artifacts; repeated downloads normally benefit from `normal`.
Non-normal policies are rejected with `file_body_mode = mmap`. Roll back by
restoring `page_cache_policy = normal` and restarting.

Use cgroup controls for a hard total-memory boundary. For an 8 MiB cap and a
16-request global limit (128 MiB anonymous-body budget), an initial systemd
canary might use:

```ini
[Service]
MemoryHigh=512M
MemoryMax=768M
```

Size these values for baseline RSS, sockets, stacks, and any other processes in
the cgroup. `MemoryHigh` drives reclaim/throttling; `MemoryMax` is the last line
and can OOM-kill the unit if set below non-reclaimable demand. Never schedule
host-wide `/proc/sys/vm/drop_caches` as a per-service cache control.

`file_body_mode = mmap` is an experimental measurement path: it maps only
the selected response range and removes the anonymous `malloc + pread`
buffer, but Workflow still writes from memory to the socket, so this is not
`sendfile` zero-copy. Cold mapped pages can fault in the communication path,
and truncating a mapped file while it is being sent can raise `SIGBUS`; use it
only for immutable files until the A/B workload proves a benefit. Stats expose
`mmap_resps`, `mmap_bytes`, `mmap_active`, `mmap_peak`, and
`mmap_fallbacks` so a benchmark cannot silently measure the fallback path.

### Protection versus fairness

Global gates (`qps_total`, `max_inflight`, `rate_total_bps`) protect one
server instance regardless of how many identities generate the traffic.
Per-IP gates (`qps_per_ip`, `max_inflight_per_ip`, `rate_bytes_per_sec`)
divide capacity fairly but do not cap aggregate demand when the number of
IPs grows. Set global protection first, then add per-IP quotas for fairness.
Global gates run before per-IP gates so rejected floods do not grow the
per-IP limiter maps. Waiting requests occupy in-flight slots, so leave room
for shaped backlog when choosing `max_inflight`.

### Rate limiting semantics

Limits are **per server instance** (single-instance design). For
multi-instance scale-out, use LB IP-affinity (consistent hashing) so each
client IP lands on one instance, or move the buckets to shared storage.
Limiter state entries for idle IPs are reclaimed automatically.

### Statistics and on-demand dump

Set `stats_interval_sec` to emit one parseable line at each interval:

```text
[stats] reqs=120(+20) 2xx=100 404=2 4xx=15 5xx=3 rej(qps_total)=10 ... inflight=4 peak=18 buckets(qps)=7 buckets(bw)=6 served=10485760
```

The line includes cumulative and interval request counts, status classes,
all six gate-rejection counters, current/peak in-flight requests, active
per-IP bucket counts, served bytes, cache-advice calls/accepted bytes/errors,
and cgroup-v2 `mem_anon`, `mem_file`, and `mem_sock`. Memory values are bytes;
`-1` means unavailable. `mem_file` is cgroup-wide reclaimable file memory, not
per-file attribution or process RSS, and advice bytes do not prove physical
eviction. Send `SIGUSR1` to request a line
within one second even when periodic output is disabled:

```bash
kill -USR1 <ferry-server-pid>
```

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
tests/stress/run_stress_mmap.sh # opt-in hot-cache pread/mmap A/B benchmark
tests/stress/run_stress_cache.sh # opt-in cold-cache policy and repeat-read benchmark
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
