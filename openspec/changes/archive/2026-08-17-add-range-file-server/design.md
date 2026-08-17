# Design: add-range-file-server

## Context

Greenfield project at `~/lab/wf` (empty git repo with openspec scaffolding). The server is built on Sogou Workflow v1.0.1 (consumed as the xrepo `workflow` package; a local `~/lab/workflow` checkout was used during early development and later dropped) and compiled with xmake v3.0.9.

Facts established by source-level exploration of Workflow v1.0.1 (these shape every decision below):

- **Responses are buffered**: `WFServerTask` sends the reply only after the whole series completes; `append_output_body_nocopy()` accumulates the body in memory. Official docs state the file-server pattern is "not suitable for very large files". There is no server-side streaming; the new `WFHttpChunkedTask` (commit c56986be) is a *client*-side feature only.
- **Peer address is available**: `WFNetworkTask::get_peer_addr()` works for server tasks — `Communicator::accept()` stores the accepted peer sockaddr into a `CommServiceTarget` which becomes the task's `target` (verified chain: Communicator.cc:1528 → WFTask.inl:20 → WFTask.h:168).
- **Async file IO maps onto Range**: `WFTaskFactory::create_pread_task(fd, buf, count, offset, cb)` uses Linux AIO; `offset`/`count` correspond 1:1 to an HTTP byte range.
- **Content-Length is not auto-generated**: `HttpMessage::encode()` (HttpMessage.cc:159) serializes only the start line, headers the user added, and body blocks. All headers (including `Content-Length`) must be set explicitly.
- **Series model enables delay injection**: a `WFTimerTask` can be pushed into the request series before the pread task, implementing throttling without blocking any thread.
- Official starting point: `tutorial/tutorial-09-http_file_server.cc` (pread + nocopy append pattern), whose buffer lifetime is managed by freeing in the server task callback.

Deployment assumptions: single instance behind a reverse proxy that appends the real client IP to `X-Forwarded-For` (nginx `$proxy_add_x_forwarded_for`). No TLS on the server. A download client may be added later; sources are organized accordingly (`server/` now, `client/` reserved).

## Goals / Non-Goals

**Goals:**
- RFC 9110/7233-conformant Range serving where total file size is always discoverable by clients (via `Content-Range` totals, `HEAD` `Content-Length`, or 200 `Content-Length`).
- Bounded memory: peak memory ≈ concurrent responses × `cap`; no response ever buffers more than `cap` bytes.
- Per-IP bandwidth limiting that degrades to slowness rather than errors whenever possible.
- IP ACL with blacklist priority and zero-downtime rule updates.
- Correct client identification behind a proxy (spoof-resistant).

**Non-Goals:**
- HTTPS/TLS (terminates at the proxy).
- Multi-instance coordination of rate limits (per-instance token buckets only).
- Multi-range (`multipart/byteranges`) responses.
- ETag / `If-None-Match` conditional requests (v1 sends `Last-Modified` only).
- Upload, directory listing, WebDAV.
- The download client itself (future change; repo layout only reserves `client/`).

## Decisions

### D1: Capped ranges instead of streaming (Plan A)

Every response is truncated to a configurable `cap` (default 8 MiB): a satisfiable range request returns at most `cap` bytes (RFC 9110 explicitly permits returning a shorter range); a non-Range request returns the whole file only if `size ≤ threshold` (default: `threshold = cap`), otherwise `413`.

**Why:** Workflow cannot stream a server response (see Context). Download managers (aria2/IDM-style, and the planned `ferry-client`) learn the full size from `Content-Range: bytes s-e/TOTAL` on the first capped 206, then issue follow-up ranges — the truncation is invisible to them. Memory becomes bounded by `cap`.

**Alternatives considered:**
- *Plan B — custom server on the comm layer* (own `ProtocolMessage`, chunked socket writes): true streaming and exact shaping, but means reimplementing HTTP/1.1 connection management, parsing, keep-alive, timeouts — several times the effort, and fragile. Rejected for v1.
- *Plan C — hybrid* (small files whole, large files capped): essentially what we ship, with `threshold` decoupled from `cap` via config so behavior can diverge later without code changes.
- *Buffering whole files* (tutorial-09 as-is): unbounded memory — the exact failure mode this design exists to avoid.

### D2: Rate limiting = per-IP token bucket + series-timer soft shaping

Bandwidth-only limiting (no QPS dimension). Each resolved client IP owns a token bucket (rate = configured bytes/sec). Before serving, the handler computes the wait `t = deficit / rate` for the bytes it intends to send (always ≤ cap):

- `t ≤ max_wait` (30 s): push `WFTimerTask(t)` into the series before the pread — the client experiences slowness, not an error. No thread is blocked.
- `t > max_wait`: reply `429` with `Retry-After`.

Charging is by body bytes actually served (200/206 only; HEAD and error responses are free). Idle buckets are reclaimed by a periodic sweep (timer task, e.g. every 60 s, entries idle > 5 min). A single mutex guards the map — contention is trivial at expected scale; sharding is a documented future optimization, not v1 work. The bucket's time source is injectable (see D7) so tests never sleep.

**Why not hard-reject (429-first):** a download server whose clients constantly receive 429s pushes retry/backoff complexity onto every client. Soft shaping keeps the protocol loop simple; the 30 s cap prevents a starving client from pinning a connection indefinitely.

### D3: Client IP = rightmost X-Forwarded-For entry (trust-hops based)

`X-Forwarded-For: a, b, c` — each proxy *appends* the address it saw. The rightmost entry was written by the proxy nearest to us (trusted), so it cannot be forged by the client; the leftmost can be. Resolution rule: take the `trust_hops`-th entry from the right (`trust_hops = 1` default ⇒ rightmost). No XFF header → `get_peer_addr()` fallback (also covers direct-connection debugging). An unparseable selected entry → fall back to peer address.

**Constraint documented for operators:** the front proxy must append/overwrite XFF correctly (nginx `$proxy_add_x_forwarded_for`); a proxy that merely forwards a client-supplied XFF breaks this guarantee.

### D4: ACL semantics — blacklist priority, whitelist gated

Order: (1) if IP ∈ blacklist → `403` (even if also whitelisted); (2) if whitelist is non-empty and IP ∉ whitelist → `403`; (3) else allow. Entries are CIDRs (IPv4/IPv6); bare IPs are /32 or /128. Matching: linear scan over two vectors — rule counts are expected in the dozens; no fancier structure in v1.

**Hot reload:** a timer task polls the ACL file's mtime every `acl_poll_interval` seconds (default 5). On change: parse the new file fully, then swap the rule set atomically (new struct behind a lock / shared_ptr swap). A parse failure keeps the old rules and logs — never a half-loaded ACL.

### D5: File serving pipeline (series composition)

```
process(server_task):
  ip = resolve_client_ip()          # D3
  acl.allow(ip)?                    # D4        → 403
  path = safe_join(root, uri)       # decode + containment → 400/404
  stat(path)                        #           → 404
  parse Range / apply cap+threshold # D1        → 200/206/413/416 decision
  limiter.wait_bytes(ip, len)       # D2        → 429 or timer(t)
  series: [timer(t)?] → pread(fd, buf, len, offset) → reply
```

- `fd` is opened in `process` and closed in the pread callback (tutorial-09 pattern); `buf` is freed in the server-task callback (`set_callback`), matching the nocopy body lifetime.
- HEAD: identical header computation, no pread, no body.
- `If-Range`: supported only with `Last-Modified`. If present and stale/mismatched, the request is treated as having no Range header (RFC-permissible behavior of returning the full representation — which then follows the 413/threshold rule).

### D6: Project layout & xmake integration

```
ferry/
├── xmake.lua              # includes("server"); later includes("client")
├── config/                # server config + ACL file (sample/default)
├── server/
│   ├── xmake.lua          # targets: "ferry-server-core" (static) + "ferry-server" (binary)
│   ├── main.cc            # config load, server start, signals, graceful stop
│   ├── config.{h,cc}      # simple key=value parser
│   ├── acl.{h,cc}         # CIDR parse/match, mtime-poll reload
│   ├── rate_limiter.{h,cc}# token buckets + wait computation + sweep
│   ├── range.{h,cc}       # Range header parse, cap/threshold application
│   ├── client_ip.{h,cc}   # XFF rightmost + fallback
│   └── handler.{h,cc}     # D5 pipeline
├── tests/
│   ├── unit/              # gtest, pure-logic tests (no server, no sleep)
│   └── integration/       # gtest, in-process WFHttpServer + workflow client
└── client/                # reserved, not in this change
```

Server sources (everything except `main.cc`) form a static library target `ferry-server-core` that both the `ferry-server` binary and the test binaries link against — this split is what makes L1/L2 tests possible without code duplication.

Workflow integration: the library comes from xrepo — root `xmake.lua` declares `add_requires("workflow v1.0.1")` and `ferry-server-core` consumes it via `add_packages("workflow", {public = true})` (which propagates headers, static lib, and OpenSSL/pthread/dl deps to dependents). No local checkout is needed; xmake builds/caches the package on first build. (During development a local `~/lab/workflow` prefix + `workflow_prefix` option was used, then replaced by the xrepo package.) Shared server/client code (e.g. range utilities) is deliberately NOT extracted into `common/` yet — wait for the client to materialize first.

Config is a flat `key = value` file: `port`, `root`, `cap_bytes`, `size_threshold_bytes`, `rate_bytes_per_sec`, `max_wait_sec`, `trust_hops`, `acl_file`, `acl_poll_interval_sec`, `max_connections`. Missing keys fall back to defaults; invalid values fail startup loudly.

### D7: Testing architecture — three layers, fake clock, shared static core

The module split above deliberately isolates pure logic from Workflow glue, which determines the test pyramid:

- **L1 unit tests** (gtest, milliseconds, no server, no sleep): `range` parsing, ACL CIDR parse/match, XFF selection, token-bucket arithmetic, config parsing, path safety. Framework choice follows the ecosystem: Workflow's own test suite uses `gtest` via xrepo (`add_requires("gtest", {configs = {main = true}})`), so we do the same.
- **L2 in-process integration tests** (gtest): a real `WFHttpServer` bound to an **ephemeral port** (`start(0)` + `get_listen_addr()` — verified available in WFServer.h:153), driven by Workflow HTTP client tasks. Covers the full status machine (200/206/413/416/403/404/400/429 + HEAD), XFF header injection, and soft-shaping delay. Workflow's `test/http_unittest.cc` is the reference skeleton.
- **L3 system tests** (real binary, real clients): curl-driven scenario checks (tasks §10), `cmp`-verified content of range slices, aria2 multi-threaded download of a large file, hot-reload timing, ASan runs, concurrency/memory sanity.

Two enabling design rules:

1. **Injectable clock for the token bucket** — the limiter takes a time source (e.g. `std::function<std::chrono::steady_clock::time_point()>`); production passes the real clock, unit tests pass a fake one and advance it exactly. No `sleep`-based unit test exists anywhere.
2. **Timing-dependent integration assertions use generous tolerance windows** (e.g. elapsed ∈ [expected×0.5, expected×4]) and shrunk knobs (`rate_bytes_per_sec` tiny, `max_wait_sec = 1`, `acl_poll_interval_sec = 1`) so waits stay sub-second; every knob being configurable is what makes this possible.

Known weakness accepted: L2 uses the same framework as client and server, so a shared protocol misunderstanding could mask itself — L3 (curl/aria2 as independent referees) is the mitigation.

## Risks / Trade-offs

- [Memory = concurrent responses × cap can still exhaust a small host (e.g. 1000 × 8 MiB = 8 GB)] → `max_connections` (Workflow `WFServerParams`) is configured in tandem with `cap`; sizing guidance in README.
- [XFF spoofing if the front proxy misbehaves (forwards client-supplied XFF untouched)] → operator constraint documented (D3); `trust_hops` adjustable for multi-tier proxies.
- [Rate limits are per-instance; future multi-instance deployment breaks global limits] → explicit non-goal; future scale-out should use LB IP-affinity (consistent hashing) or move buckets to shared storage (new change).
- [Linux AIO assumptions on non-local filesystems (NFS et al.)] → documented assumption: files on local disk.
- [nocopy buffer lifetime bugs (use-after-free / leak)] → single-ownership pattern copied from tutorial-09: `buf` freed exactly once in the server-task callback, on every path including pread failure.
- [Truncation is invisible to smart clients but not to naive ones] → a plain `wget`/`curl -O` (no Range) against a large file gets `413` with a self-describing body telling it to use Range; considered acceptable for a download server.
- [`If-Range` with entity-tags unsupported] → v1 responds as if the precondition failed (full representation semantics); documented limitation.
- [Timing-dependent tests can be flaky on loaded CI machines] → unit layer uses the injected fake clock (zero sleeping); integration layer shrinks all time/size knobs and asserts only generous tolerance windows, never exact durations.

## Migration Plan

Greenfield — no migration. Delivery = `xmake && xmake run ferry-server` (or install). Rollback: remove the binary; there is no prior state.

## Open Questions

- Exact 413 response body wording (self-describing, must mention Range usage) — settle during implementation.
- Whether `trust_hops > 1` needs live multi-tier testing before v1 ships (single-proxy deployment is the stated target).
