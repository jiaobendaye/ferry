# ferry — Range-capable HTTP file download server

An HTTP file server built on [Sogou Workflow](https://github.com/sogou/workflow)
(fully asynchronous C++ framework) and built with [xmake](https://xmake.io).

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

Source lives in `server/`; a download **client** is planned under `client/`.

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

Three layers (see `openspec/changes/add-range-file-server/design.md`, D7):

```bash
xmake run unit-test          # L1: pure logic (range/ACL/XFF/limiter/config/path), no sleeps
xmake run integration-test   # L2: real WFHttpServer on ephemeral ports + workflow HTTP client
tests/system/                # L3: shell scripts driving the real binary with curl (fixtures, rate, XFF)
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
